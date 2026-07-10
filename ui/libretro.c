#include <libretro.h>

#include <stdio.h>
#include <string.h>
#include <libgen.h>
#include <pthread.h>
#include <glib/gstdio.h>

#include "retro-gen.h"
#include "qemu/datadir.h"
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qemu-main.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "ui/console.h"
#include "ui/kbd-state.h"
#include "ui/libretro-vars.h"
#include "audio/audio.h"
#include "audio/audio_int.h"
#include "sysemu/block-backend.h"
#include "qapi/qapi-commands-block.h"
#include "qapi/error.h"

#define QEMU_CMD_PREFIX "qemu-system-"
#define DEFAULT_ARCH "x86_64"
#define QEMU_CMD                                                               \
	(QEMU_CMD_PREFIX DEFAULT_ARCH), "-libretro", "-audiodev",              \
		"libretro,id=snd0", "-machine", "pcspk-audiodev=snd0",         \
		"-device", "AC97,audiodev=snd0"

void rcu_init(void);

G_GNUC_PRINTF(1, 2)
static void notice_report(const char *fmt, ...);

static const char *game_path;
static const char *system_dir;
static const char *target_arch;
static pthread_t emu_thread;
static DisplaySurface *surface;
static QKbdState *kbd;
static bool exited = false;
static bool emu_thread_started = false;
static bool emu_thread_joined = false;

static HWVoiceOut *hw_voice_out = NULL;
static pthread_mutex_t hw_voice_out_mutex = PTHREAD_MUTEX_INITIALIZER;
// Signaled when the emu thread buffers samples; audio_callback waits on
// it (bounded) instead of returning empty-handed, because the frontend's
// audio-callback thread re-invokes the callback in a tight loop and an
// instant return busy-spins a host core whenever the guest is silent —
// which includes the entire boot, before the voice even opens.
static pthread_cond_t hw_voice_out_cond = PTHREAD_COND_INITIALIZER;

// Input. Written by the frontend thread (retro_run), consumed on the main
// loop thread by input_inject_bh; the two run concurrently, so all access
// goes through input_mutex. Mouse deltas and wheel detents accumulate
// because the two sides tick at independent rates.
//
// The keyboard is POLLED via RETRO_DEVICE_KEYBOARD instead of taken from
// the keyboard-event callback: RetroArch 1.7.5's callback path drops the
// extended-key bit before translating scancodes (arrows arrive as keypad
// digits, right Ctrl as left Ctrl), swallows every event while the
// frontend's on-screen-keyboard toggle (F12) is latched, and emits a
// bogus RETROK_UNKNOWN pseudo-keydown per typed character — while its
// polled path translates correctly and is gated by none of that. The
// callback stays registered as a capability hint (newer frontends
// auto-enable Game Focus for cores that register one) but its events are
// ignored.
#define KEY_EVENT_QUEUE_LEN 64
struct key_event {
	bool down;
	QKeyCode key;
};
static struct key_event key_event_queue[KEY_EVENT_QUEUE_LEN];
static size_t num_pending_keys = 0;
static int mouse_dx, mouse_dy;
static int wheel_accum; // scroll detents, up positive
static bool buttons_down[INPUT_BUTTON__MAX];
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
// Injection bottom half; created on the emu thread once the console
// exists, scheduled by retro_run after each poll (legal from any thread).
static QEMUBH *input_bh;

// Core options (v0 API — RetroArch 1.7.5 has SET_VARIABLES/GET_VARIABLE
// only). Boot-time snapshots (opt_*) are written on the frontend thread
// in retro_load_game BEFORE the emu thread is created, so emu_thread_fn
// reads them without locking (happens-before via pthread_create); 0/NULL
// means auto = leave the command line alone. mouse_speed_pct is frontend
// thread only. The two 3dfx overrides are declared in ui/libretro-vars.h
// and sampled by hw/mesa and hw/3dfx on emu/vCPU threads at 3D context
// creation; -1 = auto (defined here so non-3dfx builds still link).
volatile int libretro_cfg_fps_limit = -1;
volatile int libretro_cfg_ext_year = -1;

struct choice {
	const char *str;
	int val;
};
static const struct choice ram_choices[] = {
	{ "64 MB", 64 },   { "128 MB", 128 }, { "256 MB", 256 },
	{ "512 MB", 512 }, { "1 GB", 1024 },  { "2 GB", 2048 },
	{ NULL, 0 },
};
static const struct choice boot_choices[] = {
	{ "hard disk", 'c' },
	{ "CD-ROM", 'd' },
	{ "floppy", 'a' },
	{ NULL, 0 },
};
static const struct choice audio_buffer_choices[] = {
	// -audiodev out.buffer-length takes microseconds.
	{ "32 ms", 32000 },
	{ "48 ms", 48000 },
	{ "64 ms", 64000 },
	{ "128 ms", 128000 },
	{ NULL, 0 },
};
static const struct choice mouse_speed_choices[] = {
	{ "50%", 50 },	 { "75%", 75 },	  { "100%", 100 },
	{ "150%", 150 }, { "200%", 200 }, { NULL, 0 },
};
static const struct choice whpx_isolation_choices[] = {
	{ "on", 1 },
	{ "off (trusted guests only)", -1 },
	{ NULL, 0 },
};
static int opt_ram_mb;
static char opt_boot_order_char;
static const char *opt_accel; // "whpx"/"tcg" string literal, or NULL
// 0 = auto/leave argv alone, 1 = secure isolation on, -1 = performance mode.
static int opt_whpx_isolation;
static int opt_audio_buffer_us;
static int mouse_speed_pct = 100;
static int mouse_rem_x, mouse_rem_y; // sub-unit motion carry; retro_run only
static unsigned restart_notified_mask; // frontend thread only

// Disk control (RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, v0 — the
// only version RetroArch 1.7.5 drives). The disc list and tray/index
// bookkeeping are frontend-thread-only: all seven callbacks and
// retro_load_game run there. The only cross-thread state is disk_req,
// handed to disk_swap_bh on the main loop under disk_mutex — the same
// split as the input state above. Reset at the top of every
// retro_load_game because statics persist across load cycles.
static GPtrArray *disc_list; // char *, absolute paths; owns entries
static unsigned disc_index; // selected disc (== inserted when tray shut)
static bool tray_ejected; // tray state requested by the frontend
static bool disc_change_pending; // selection changed while tray open
// Boot descriptor. Written by retro_load_game before the emu thread is
// created (happens-before, no locks) and selects the argv branch in
// emu_thread_fn: exactly one non-NULL — a .qemu_cmd_line to parse or a
// medium for -cdrom; both NULL boots game_path as a bare disk image.
static char *boot_cmd_line_path;
static char *boot_cdrom_path;
// Tray/medium operation awaiting disk_swap_bh; a newer request overwrites
// an unconsumed one, so rapid operations coalesce to the final state.
enum disk_request_type {
	DISK_REQ_NONE,
	DISK_REQ_OPEN,
	DISK_REQ_CLOSE,
	DISK_REQ_CHANGE,
	DISK_REQ_EJECT_CLOSE,
};
static struct {
	enum disk_request_type type;
	char *filename; // owned; medium to insert for DISK_REQ_CHANGE
} disk_req;
static pthread_mutex_t disk_mutex = PTHREAD_MUTEX_INITIALIZER;
// Swap bottom half; created on the emu thread once the console exists
// (display_init), scheduled by disk_request (legal from any thread).
static QEMUBH *disk_bh;
static void disk_swap_bh(void *opaque);
// These predicates live in internal block-layer headers (block_int-*.h)
// that don't belong in a common translation unit; declared here so
// CALL_QEMU_FUNC has their type.
bool blk_dev_has_removable_media(BlockBackend *blk);
bool blk_dev_has_tray(BlockBackend *blk);

// Video. QEMU runs free; publishers — refresh() on the emu thread and the
// qemu-3dfx readback glue on a vCPU thread, both under the BQL — copy each
// finished frame into the pending slot, and retro_run swaps the slots and
// presents outside frame_mutex. Two slots so the handoff is a pointer
// exchange: a publisher must never wait on the frontend's upload/vsync,
// which stalls the BQL and with it the emu thread's 10 ms audio timer
// (audible crackle). This replaces an older design that ran the two
// threads in lockstep, which throttled the guest to the frontend's frame
// rate and stalled I/O handling (the emu thread used to park holding the
// BQL between frames).
struct frame_slot {
	uint8_t *buf;
	int w, h;
	// glReadPixels row order; the flip happens on the frontend thread
	// at acquire time, not on the BQL-holding publisher.
	bool bottom_up;
};
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct frame_slot pending_frame; // under frame_mutex
static struct frame_slot present_frame; // frontend thread only
static bool pending_updated = false;	// under frame_mutex
static bool present_valid = false;	// frontend thread only
// While qemu-3dfx pass-through presents (Win98 Glide/GL games), frames
// come from the readback glue instead of the VGA surface; refresh()
// stands down so the stale surface cannot overwrite them. Written and
// read under the BQL only.
static bool fx_active;
#ifdef CONFIG_QEMU_3DFX
/* retro_run heartbeat: incremented by the frontend thread and sampled by
 * the 3D readback thread so capture can stop while the frontend is idle. */
static uint32_t frontend_frame_count;
#endif

// Size the pending slot for a w-by-h XRGB8888 frame; frame_mutex held.
// Buffers are only ever reallocated, never freed: the present slot may
// still be feeding the frontend when the machine shuts down.
static void pending_ensure(int w, int h)
{
	if (!pending_frame.buf || pending_frame.w != w ||
	    pending_frame.h != h) {
		pending_frame.buf =
			g_realloc(pending_frame.buf, (size_t)w * h * 4);
		pending_frame.w = w;
		pending_frame.h = h;
	}
}

// Swap in the newest published frame, if any; frontend thread only and
// frame_mutex held. Returns true when present_frame now holds fresh content.
static bool frame_acquire_locked(void)
{
	bool fresh = false;

	if (pending_updated) {
		struct frame_slot tmp = pending_frame;
		pending_frame = present_frame;
		present_frame = tmp;
		pending_updated = false;
		present_valid = true;
		fresh = true;
	}
	return fresh;
}

// Finish acquiring a 3D frame outside every handoff lock.
static void frame_finish_acquire(bool fresh)
{
	// 3D frames arrive in glReadPixels order; flip them here, outside
	// the lock, so the vCPU that published never pays for it.
	if (fresh && present_frame.bottom_up) {
		static uint8_t *row;
		static size_t row_size;
		size_t stride = (size_t)present_frame.w * 4;

		if (row_size < stride) {
			row = g_realloc(row, stride);
			row_size = stride;
		}
		for (int y = 0; y < present_frame.h / 2; y++) {
			uint8_t *top = present_frame.buf + (size_t)y * stride;
			uint8_t *bot = present_frame.buf +
				       (size_t)(present_frame.h - 1 - y) *
					       stride;
			memcpy(row, top, stride);
			memcpy(top, bot, stride);
			memcpy(bot, row, stride);
		}
		present_frame.bottom_up = false;
	}
}

// Notifications. QEMU reports errors from arbitrary threads, but libretro
// environment calls are only safe from the frontend thread, so messages
// queue here and retro_run delivers them.
#define PENDING_MSGS_LEN 32
static pthread_mutex_t msg_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct pending_msg {
	char *msg;
	unsigned duration;
	enum retro_log_level level;
} pending_msgs[PENDING_MSGS_LEN];
static size_t num_pending_msgs = 0;
static unsigned num_dropped_msgs = 0; // under msg_mutex

static const QKeyCode key_map[RETROK_LAST] = {
#define KEY(q, r) [RETROK_##r] = Q_KEY_CODE_##q
	KEY(SHIFT, LSHIFT),
	KEY(SHIFT_R, RSHIFT),
	KEY(ALT, LALT),
	KEY(ALT_R, RALT),
	KEY(CTRL, LCTRL),
	KEY(CTRL_R, RCTRL),
	KEY(MENU, MENU),
	KEY(ESC, ESCAPE),
	KEY(1, 1),
	KEY(2, 2),
	KEY(3, 3),
	KEY(4, 4),
	KEY(5, 5),
	KEY(6, 6),
	KEY(7, 7),
	KEY(8, 8),
	KEY(9, 9),
	KEY(0, 0),
	KEY(MINUS, MINUS),
	KEY(EQUAL, EQUALS),
	KEY(BACKSPACE, BACKSPACE),
	KEY(TAB, TAB),
	KEY(Q, q),
	KEY(W, w),
	KEY(E, e),
	KEY(R, r),
	KEY(T, t),
	KEY(Y, y),
	KEY(U, u),
	KEY(I, i),
	KEY(O, o),
	KEY(P, p),
	KEY(BRACKET_LEFT, LEFTBRACKET),
	KEY(BRACKET_RIGHT, RIGHTBRACKET),
	KEY(RET, RETURN),
	KEY(A, a),
	KEY(S, s),
	KEY(D, d),
	KEY(F, f),
	KEY(G, g),
	KEY(H, h),
	KEY(J, j),
	KEY(K, k),
	KEY(L, l),
	KEY(SEMICOLON, SEMICOLON),
	KEY(APOSTROPHE, QUOTE),
	KEY(GRAVE_ACCENT, BACKQUOTE),
	KEY(BACKSLASH, BACKSLASH),
	KEY(Z, z),
	KEY(X, x),
	KEY(C, c),
	KEY(V, v),
	KEY(B, b),
	KEY(N, n),
	KEY(M, m),
	KEY(COMMA, COMMA),
	KEY(DOT, PERIOD),
	KEY(SLASH, SLASH),
	KEY(ASTERISK, ASTERISK),
	KEY(SPC, SPACE),
	KEY(CAPS_LOCK, CAPSLOCK),
	KEY(F1, F1),
	KEY(F2, F2),
	KEY(F3, F3),
	KEY(F4, F4),
	KEY(F5, F5),
	KEY(F6, F6),
	KEY(F7, F7),
	KEY(F8, F8),
	KEY(F9, F9),
	KEY(F10, F10),
	KEY(NUM_LOCK, NUMLOCK),
	KEY(SCROLL_LOCK, SCROLLOCK),
	KEY(KP_DIVIDE, KP_DIVIDE),
	KEY(KP_MULTIPLY, KP_MULTIPLY),
	KEY(KP_SUBTRACT, KP_MINUS),
	KEY(KP_ADD, KP_PLUS),
	KEY(KP_ENTER, KP_ENTER),
	KEY(KP_DECIMAL, KP_PERIOD),
	KEY(SYSRQ, SYSREQ),
	KEY(KP_0, KP0),
	KEY(KP_1, KP1),
	KEY(KP_2, KP2),
	KEY(KP_3, KP3),
	KEY(KP_4, KP4),
	KEY(KP_5, KP5),
	KEY(KP_6, KP6),
	KEY(KP_7, KP7),
	KEY(KP_8, KP8),
	KEY(KP_9, KP9),
	KEY(LESS, LESS),
	KEY(F11, F11),
	KEY(F12, F12),
	KEY(PRINT, PRINT),
	KEY(HOME, HOME),
	KEY(PGUP, PAGEUP),
	KEY(PGDN, PAGEDOWN),
	KEY(END, END),
	KEY(LEFT, LEFT),
	KEY(UP, UP),
	KEY(DOWN, DOWN),
	KEY(RIGHT, RIGHT),
	KEY(INSERT, INSERT),
	KEY(DELETE, DELETE),
	KEY(UNDO, UNDO),
	KEY(HELP, HELP),
	KEY(META_L, LMETA),
	KEY(META_R, RMETA),
	KEY(COMPOSE, COMPOSE),
	KEY(PAUSE, PAUSE),
	KEY(KP_EQUALS, KP_EQUALS),
	KEY(POWER, POWER),
	KEY(AUDIONEXT, MEDIA_NEXT),
	KEY(AUDIOPREV, MEDIA_PREV),
	KEY(AUDIOSTOP, MEDIA_STOP),
	KEY(AUDIOPLAY, MEDIA_PLAY_PAUSE),
	KEY(AUDIOMUTE, VOLUME_MUTE),
	KEY(VOLUMEUP, VOLUME_UP),
	KEY(VOLUMEDOWN, VOLUME_DOWN),
	KEY(MAIL, LAUNCH_MAIL),
	KEY(AC_HOME, BROWSER_HOME),
	KEY(AC_BACK, BROWSER_BACK),
	KEY(AC_FORWARD, BROWSER_FORWARD),
	KEY(AC_REFRESH, BROWSER_REFRESH),
	KEY(AC_BOOKMARKS, BROWSER_FAVORITES),
	KEY(F13, F13),
	KEY(F14, F14),
	KEY(F15, F15)
#undef KEY
};

// Wait for emu thread to exit
static void join_emu_thread(void)
{
	// The frontend joins both when the guest powers off (retro_run sees
	// exited) and again from retro_unload_game; a second join would be
	// use-after-free of the thread handle.
	if (!emu_thread_started || emu_thread_joined) {
		return;
	}
	emu_thread_joined = true;

	pthread_join(emu_thread, NULL);
}

static void emu_thread_exit(void)
{
	// Post-cleanup only: this runs on the emu thread itself, either after
	// qemu_default_main returned (block layer already flushed) or before
	// QEMU started. It must never run mid-emulation — killing worker
	// threads mid-write corrupts guest disks.
	if (target_arch && arch_is_valid(target_arch)) {
		CALL_QEMU_FUNC(qemu_thread_kill_all);
	}

	exited = true;

	pthread_exit(NULL);
}

void retro_init(void)
{
}

void retro_deinit(void)
{
}

unsigned retro_api_version(void)
{
	return 1;
}

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->need_fullpath = true;
	info->valid_extensions = "qemu_cmd_line|iso|img|qcow|qcow2|m3u";
	info->library_version = "0.1.0";
	info->library_name = "qemu";
}

static pthread_mutex_t av_info_lock = PTHREAD_MUTEX_INITIALIZER;
// Geometry and timing changes are delivered differently: geometry rides
// RETRO_ENVIRONMENT_SET_GEOMETRY, which is cheap and safe every frame,
// while timing needs SET_SYSTEM_AV_INFO — which RetroArch 1.7.5 answers
// with a full teardown/reinit of the video, audio AND input drivers. That
// reinit destroys the window mid-session, drops the Game Focus keyboard
// grab (the recreated dinput device loses its hotkey-block flags, so
// guest typing suddenly triggers frontend hotkeys) and restarts the audio
// thread. Guests switch video modes several times per boot, so geometry
// avoids the SET_SYSTEM_AV_INFO path — with one exception: 1.7.5-era
// frontends size their video texture from the geometry in effect at the
// last (re)init and SET_GEOMETRY never grows it, so a mode LARGER than
// anything delivered before would have its right/bottom edge clipped
// (observed in EmuVR: 1280-wide modes lost the right 256 px). The first
// time a dimension exceeds the session's delivered envelope the change is
// promoted to SET_SYSTEM_AV_INFO so the frontend reallocates; every
// switch inside the envelope still rides SET_GEOMETRY.
static bool changed_geometry = false;
static bool changed_timing = false;
// Largest geometry delivered to the frontend so far (the envelope its
// texture is known to accommodate). Frontend-thread-only: retro_run and
// retro_load_game.
static unsigned delivered_env_w;
static unsigned delivered_env_h;
static struct retro_system_av_info av_info = {
	.geometry = {
		// base_* track the guest mode. max_* start above every Cirrus/std
		// VGA mode (those top out at 1600x1200); qemu-3dfx may grow them
		// through QEMU_FX_MAX_* and trigger exactly the one-time
		// SET_SYSTEM_AV_INFO reinit described above.
		//
		// The initial base is deliberately 1024x768 rather than the
		// 640x480 the guest actually boots in: frontends size their
		// texture from this, smaller modes render fine as a
		// sub-rectangle, and it makes the whole boot-to-desktop mode
		// dance of a 1024x768 guest fit the envelope with zero
		// promoted reinits.
		.base_width = 1024,
		.base_height = 768,
		.max_width = 1920,
		.max_height = 1200,
		.aspect_ratio = 0.0,
	},
	.timing = {
		// Frontends divide by these for pacing before the emu thread
		// has reported real values; zeroes break RetroArch 1.7.x.
		.fps = 60.0,
		.sample_rate = 44100.0,
	},
};

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	*info = av_info;
}

static void keyboard_event(bool down, unsigned keycode, uint32_t character,
			   uint16_t key_modifiers)
{
	// Intentionally ignored — the keyboard is polled in retro_run (see
	// the input comment block above). Registration alone is what makes
	// newer frontends offer automatic Game Focus.
}

// input_mutex held.
static void enqueue_key_locked(bool down, QKeyCode qc)
{
	if (num_pending_keys == KEY_EVENT_QUEUE_LEN) {
		// Drop the oldest event rather than the newest: dropping a
		// key-up leaves the guest with a stuck key.
		memmove(&key_event_queue[0], &key_event_queue[1],
			(KEY_EVENT_QUEUE_LEN - 1) * sizeof(key_event_queue[0]));
		num_pending_keys--;
	}
	key_event_queue[num_pending_keys++] = (struct key_event){
		.down = down,
		.key = qc,
	};
}

static bool key_is_modifier(unsigned k)
{
	switch (k) {
	case RETROK_LSHIFT:
	case RETROK_RSHIFT:
	case RETROK_LCTRL:
	case RETROK_RCTRL:
	case RETROK_LALT:
	case RETROK_RALT:
	case RETROK_LMETA:
	case RETROK_RMETA:
		return true;
	default:
		return false;
	}
}

static retro_audio_sample_t cb_audio_sample;

void retro_set_audio_sample(retro_audio_sample_t cb)
{
	cb_audio_sample = cb;
}

static retro_audio_sample_batch_t cb_audio_sample_batch;

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	cb_audio_sample_batch = cb;
}

static void audio_callback(void)
{
	// If nothing is buffered, wait for samples — but bounded, because
	// the frontend's pause and driver-reinit paths block until an
	// in-flight callback returns.
	pthread_mutex_lock(&hw_voice_out_mutex);
	if (!hw_voice_out || !hw_voice_out->pending_emul) {
		gint64 deadline = g_get_real_time() + 20000; // 20 ms
		struct timespec ts = {
			.tv_sec = deadline / G_USEC_PER_SEC,
			.tv_nsec = (long)(deadline % G_USEC_PER_SEC) * 1000,
		};
		pthread_cond_timedwait(&hw_voice_out_cond, &hw_voice_out_mutex,
				       &ts);
	}
	pthread_mutex_unlock(&hw_voice_out_mutex);

	// Copy samples out under the lock but call the frontend without it:
	// holding the voice mutex across cb_audio_sample_batch stalls the emu
	// thread's audio timer (and with it the whole machine) whenever the
	// frontend blocks, e.g. while paused with a full audio buffer. Short
	// writes are allowed by the API; the remainder stays pending.
	for (;;) {
		int16_t chunk[1024];
		size_t chunk_len = 0;

		pthread_mutex_lock(&hw_voice_out_mutex);
		if (hw_voice_out && hw_voice_out->pending_emul) {
			size_t start = audio_ring_posb(
				hw_voice_out->pos_emul,
				hw_voice_out->pending_emul,
				hw_voice_out->size_emul);
			g_assert(start < hw_voice_out->size_emul);
			chunk_len = MIN(hw_voice_out->pending_emul,
					hw_voice_out->size_emul - start);
			chunk_len = MIN(chunk_len, sizeof(chunk)) & ~3;
			memcpy(chunk, hw_voice_out->buf_emul + start,
			       chunk_len);
		}
		pthread_mutex_unlock(&hw_voice_out_mutex);

		if (chunk_len == 0) {
			break;
		}

		size_t written = cb_audio_sample_batch(chunk, chunk_len / 4);

		pthread_mutex_lock(&hw_voice_out_mutex);
		if (hw_voice_out) {
			hw_voice_out->pending_emul -=
				MIN(written * 4, hw_voice_out->pending_emul);
		}
		pthread_mutex_unlock(&hw_voice_out_mutex);

		if (written * 4 < chunk_len) {
			// Frontend buffer is full; try again next callback.
			break;
		}
	}
}

static retro_environment_t cb_env;

// v0 format: "Label; value|value|..." — first value is the default, and
// the frontend UI shows the raw value strings, so they must read well on
// their own. The frontend copies the array during the call.
static const struct retro_variable qemu_core_variables[] = {
	{ "qemu_ram",
	  "Guest RAM (restart); auto|64 MB|128 MB|256 MB|512 MB|1 GB|2 GB" },
	{ "qemu_boot_order", "Boot device (restart); auto|hard disk|CD-ROM|floppy" },
	{ "qemu_accel", "CPU accelerator (restart); auto|WHPX|TCG" },
	{ "qemu_whpx_isolation",
	  "WHPX security isolation (restart); auto|on|off (trusted guests only)" },
	{ "qemu_audio_buffer", "Audio buffer (restart); auto|32 ms|48 ms|64 ms|128 ms" },
	{ "qemu_mouse_speed", "Mouse speed; 100%|50%|75%|150%|200%" },
#ifdef CONFIG_QEMU_3DFX
	{ "qemu_3dfx_fps_limit",
	  "3D FPS limit (next 3D app); auto|30|60|72|75|100|120|144" },
	{ "qemu_3dfx_ext_year",
	  "3D GL extensions year (next 3D app); auto|1998|2000|2002|2004|2006|2008|2010" },
#endif
	{ NULL, NULL },
};

void retro_set_environment(retro_environment_t cb)
{
	cb_env = cb;

	cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT,
	   &(enum retro_pixel_format){ RETRO_PIXEL_FORMAT_XRGB8888 });
	cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK,
	   &(struct retro_keyboard_callback){ keyboard_event });
	cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO,
	   (struct retro_controller_info[]){
		   {
			   .types = (struct retro_controller_description[]){ {
				   .desc = "Keyboard",
				   .id = RETRO_DEVICE_KEYBOARD,
			   } },
			   .num_types = 1,
		   },
		   { 0 } });
	cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
	cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)qemu_core_variables);

	cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK,
	   &(struct retro_audio_callback){
		   .callback = audio_callback,
		   .set_state = NULL,
	   });
}

static retro_video_refresh_t cb_video_refresh;

void retro_set_video_refresh(retro_video_refresh_t cb)
{
	cb_video_refresh = cb;
}

static retro_input_poll_t cb_input_poll;

void retro_set_input_poll(retro_input_poll_t cb)
{
	cb_input_poll = cb;
}

static retro_input_state_t cb_input_state;

void retro_set_input_state(retro_input_state_t cb)
{
	cb_input_state = cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

void retro_reset(void)
{
	// target_arch is NULL until the emu thread starts QEMU; the
	// per-target dispatch can't handle that.
	if (!exited && target_arch) {
		CALL_QEMU_FUNC(qemu_system_reset_request,
			       SHUTDOWN_CAUSE_HOST_UI);
	}
}

static void *audio_init(Audiodev *dev, Error **errp)
{
	return dev;
}

static void audio_fini(void *opaque)
{
}

static int audio_init_out(HWVoiceOut *hw, struct audsettings *as,
			  void *drv_opaque)
{
	Audiodev *dev = drv_opaque;

	// The frontend consumes S16 stereo only; fail the voice rather than
	// abort the process on other configurations.
	if (as->fmt != AUDIO_FORMAT_S16 || as->nchannels != 2) {
		return -1;
	}

	// Only flag a change on a genuine rate difference: the default
	// (44100) matches the advertised timing, and SET_SYSTEM_AV_INFO is
	// a full driver reinit on RetroArch 1.7.5 (see av_info comment).
	pthread_mutex_lock(&av_info_lock);
	if (av_info.timing.sample_rate != as->freq) {
		av_info.timing.sample_rate = as->freq;
		changed_timing = true;
	}
	pthread_mutex_unlock(&av_info_lock);

	CALL_QEMU_FUNC(audio_pcm_init_info, &hw->info, as);

	// Ring size: -audiodev libretro,out.buffer-length=USECS, default
	// ~23 ms (the old hardcoded 1024 frames at 44.1 kHz). This is the
	// only underrun headroom there is — samples are produced by the emu
	// thread's 10 ms audio timer under the BQL and drained at the
	// frontend's audio callback cadence.
	hw->samples = CALL_QEMU_FUNC(audio_buffer_frames,
				     dev->u.libretro.out, as, 23220);

	pthread_mutex_lock(&hw_voice_out_mutex);
	g_assert(!hw_voice_out);
	hw_voice_out = hw;
	pthread_mutex_unlock(&hw_voice_out_mutex);

	return 0;
}

static void audio_fini_out(HWVoiceOut *hw)
{
	pthread_mutex_lock(&hw_voice_out_mutex);
	g_assert(hw_voice_out == hw);
	hw_voice_out = NULL;
	// Wake a waiting audio_callback so it notices the voice is gone.
	pthread_cond_broadcast(&hw_voice_out_cond);
	pthread_mutex_unlock(&hw_voice_out_mutex);
}

static void audio_enable_out(HWVoiceOut *hw, bool enable)
{
}

static size_t audio_write(HWVoiceOut *hw, void *buf, size_t size)
{
	// No lock here: audio_generic_write calls back into get_buffer_out /
	// put_buffer_out below, which take the (non-recursive) mutex.
	return CALL_QEMU_FUNC(audio_generic_write, hw, buf, size);
}

static size_t audio_buffer_get_free(HWVoiceOut *hw)
{
	pthread_mutex_lock(&hw_voice_out_mutex);
	size_t ret = CALL_QEMU_FUNC(audio_generic_buffer_get_free, hw);
	pthread_mutex_unlock(&hw_voice_out_mutex);
	return ret;
}

static void *audio_get_buffer_out(HWVoiceOut *hw, size_t *size)
{
	pthread_mutex_lock(&hw_voice_out_mutex);
	void *ret = CALL_QEMU_FUNC(audio_generic_get_buffer_out, hw, size);
	pthread_mutex_unlock(&hw_voice_out_mutex);
	return ret;
}

static size_t audio_put_buffer_out(HWVoiceOut *hw, void *buf, size_t size)
{
	pthread_mutex_lock(&hw_voice_out_mutex);
	size_t ret =
		CALL_QEMU_FUNC(audio_generic_put_buffer_out, hw, buf, size);
	pthread_cond_signal(&hw_voice_out_cond);
	pthread_mutex_unlock(&hw_voice_out_mutex);
	return ret;
}

// The console reports damaged regions here after graphic_hw_update
// re-renders them; refresh() skips publishing when nothing changed.
// Meaningful only with working dirty-VRAM tracking (WHPX with
// dirty-page tracking, or TCG) — without it every update reports full
// damage and this degrades to publish-every-tick.
static bool surface_damaged;

static void gfx_update(DisplayChangeListener *dcl, int x, int y, int w, int h)
{
	surface_damaged = true;
}

static void gfx_switch(DisplayChangeListener *dcl, DisplaySurface *new_surface)
{
	// The console can drop its surface at teardown; forget the old
	// pointer instead of copying from freed memory in the next refresh.
	if (!new_surface) {
		surface = NULL;
		return;
	}

	int w = surface_width(new_surface);
	int h = surface_height(new_surface);
	bool changed_resolution = !surface || !(surface_width(surface) == w &&
						surface_height(surface) == h);
	if (changed_resolution) {
		pthread_mutex_lock(&av_info_lock);
		av_info.geometry.base_width = w;
		av_info.geometry.base_height = h;
		changed_geometry = true;
		pthread_mutex_unlock(&av_info_lock);
	}
	surface = new_surface;
	surface_damaged = true;
}

static bool gfx_check_format(DisplayChangeListener *dcl,
			     pixman_format_code_t format)
{
	return format == PIXMAN_x8r8g8b8;
}

static void refresh(DisplayChangeListener *dcl)
{
	// Re-render at the full tick rate: with WHPX dirty-page tracking
	// (or TCG) the VGA code only re-converts lines whose VRAM actually
	// changed, so a quiet display costs almost nothing here. Hosts
	// without tracking (pre-1903 Windows) pay a full re-convert per
	// tick, as they did before the tracking existed.
	CALL_QEMU_FUNC(graphic_hw_update, dcl->con);

	// Publish the frame: copy it out so the frontend can present it
	// while QEMU keeps running — but only when the console reported
	// damage; retro_run dupes the previous frame on quiet ticks.
	// gfx_switch and the fx handoff force the flag, so geometry
	// changes always publish a real frame (the frontend re-inits its
	// video driver to black on those and needs one to recover).
	if (surface && !fx_active && surface_damaged) {
		surface_damaged = false;
		int w = surface_width(surface);
		int h = surface_height(surface);
		int stride = surface_stride(surface);
		uint8_t *src = surface_data(surface);

		pthread_mutex_lock(&frame_mutex);
		pending_ensure(w, h);
		if (stride == w * 4) {
			memcpy(pending_frame.buf, src, (size_t)w * h * 4);
		} else {
			for (int y = 0; y < h; y++) {
				memcpy(pending_frame.buf + (size_t)y * w * 4,
				       src + (size_t)y * stride, w * 4);
			}
		}
		pending_frame.bottom_up = false;
		pending_updated = true;
		pthread_mutex_unlock(&frame_mutex);
	}
}

#ifdef CONFIG_QEMU_3DFX
// qemu-3dfx frame sink (see ui/libretro-3dfx.c). publish arrives from a
// vCPU thread under the BQL with the finished 3D frame; rows come
// bottom-up from glReadPixels (flipped at acquire time on the frontend
// thread) and BGRA bytes already match XRGB8888.
static void fx_sink_publish(const void *pixels, int w, int h)
{
	bool geometry_changed = false;

	/* ui/libretro-3dfx.c validates before creating its window/PBOs. Keep
	 * this boundary defensive: never size a frontend buffer from unchecked
	 * dimensions if another producer is added later. */
	if (w <= 0 || h <= 0 || w > QEMU_FX_MAX_WIDTH ||
	    h > QEMU_FX_MAX_HEIGHT) {
		notice_report("qemu-3dfx rejected unsafe frame size %dx%d", w, h);
		return;
	}

	/* Build the replacement while it is hidden. Publishers are serialized
	 * under the BQL, so clearing pending_updated only discards an older frame
	 * that this newer one supersedes. */
	pthread_mutex_lock(&frame_mutex);
	pending_updated = false;
	pending_ensure(w, h);
	memcpy(pending_frame.buf, pixels, (size_t)w * h * 4);
	pending_frame.bottom_up = true;
	pthread_mutex_unlock(&frame_mutex);

	/* retro_run observes AV state and the pending-frame flag together under
	 * this same AV -> frame lock order. Publish both as one handoff, so an
	 * oversized frame can never be acquired with the old frontend envelope. */
	pthread_mutex_lock(&av_info_lock);
	if (av_info.geometry.max_width < (unsigned)w) {
		av_info.geometry.max_width = w;
		geometry_changed = true;
	}
	if (av_info.geometry.max_height < (unsigned)h) {
		av_info.geometry.max_height = h;
		geometry_changed = true;
	}
	if (av_info.geometry.base_width != w ||
	    av_info.geometry.base_height != h) {
		av_info.geometry.base_width = w;
		av_info.geometry.base_height = h;
		geometry_changed = true;
	}
	if (geometry_changed) {
		changed_geometry = true;
	}
	pthread_mutex_lock(&frame_mutex);
	pending_updated = true;
	pthread_mutex_unlock(&frame_mutex);
	pthread_mutex_unlock(&av_info_lock);
}

static void fx_sink_set_active(bool on)
{
	fx_active = on;
	// returning to 2D: force one publish even if the console reports
	// no fresh damage
	if (!on) {
		surface_damaged = true;
	}

	// Back to 2D: snap the geometry to the VGA surface so the next
	// published frame is not presented at the 3D resolution.
	if (!on && surface) {
		int w = surface_width(surface);
		int h = surface_height(surface);

		pthread_mutex_lock(&av_info_lock);
		if (av_info.geometry.base_width != w ||
		    av_info.geometry.base_height != h) {
			av_info.geometry.base_width = w;
			av_info.geometry.base_height = h;
			changed_geometry = true;
		}
		pthread_mutex_unlock(&av_info_lock);
	}
}

static void fx_sink_guest_dims(int *w, int *h)
{
	if (surface) {
		*w = surface_width(surface);
		*h = surface_height(surface);
	}
}

static uint32_t fx_sink_frontend_frame_count(void)
{
	return qatomic_read(&frontend_frame_count);
}

static const QemuFxSink fx_sink = {
	.publish = fx_sink_publish,
	.set_active = fx_sink_set_active,
	.get_guest_dims = fx_sink_guest_dims,
	.get_frontend_frame_count = fx_sink_frontend_frame_count,
};
#endif

static DisplayChangeListener dcl = {
	// Refresh at ~60 Hz instead of QEMU's 30 ms default; the frontend
	// presents whatever frame is latest, so this only sets guest-side
	// display smoothness.
	.update_interval = 16,
	.ops = (DisplayChangeListenerOps[]){ {
		.dpy_name = "libretro",
		.dpy_gfx_update = gfx_update,
		.dpy_gfx_switch = gfx_switch,
		.dpy_gfx_check_format = gfx_check_format,
		.dpy_refresh = refresh,
	} },
};

// Inject the input collected by the frontend into the guest. Runs as a
// bottom half on the main loop thread under the BQL; retro_run schedules
// it after each poll, so injection waits one BH dispatch instead of up to
// a display refresh period. Events are sent on real changes only: the
// PS/2 mouse emits a packet (and an IRQ12, waking an idle guest) on every
// sync while enabled, and counts wheel motion again on each repeated
// wheel-button down event.
static void input_inject_bh(void *opaque)
{
	// Level state already sent to the guest; this thread only. Wheel
	// buttons have no map entry — the PS/2 model treats a wheel down
	// event as one detent, so they are pulsed from wheel_accum below
	// instead of tracked as level state.
	static uint32_t buttons_old;
	static uint32_t btn_map[INPUT_BUTTON__MAX] = {
		[INPUT_BUTTON_LEFT] = 1u << INPUT_BUTTON_LEFT,
		[INPUT_BUTTON_RIGHT] = 1u << INPUT_BUTTON_RIGHT,
		[INPUT_BUTTON_MIDDLE] = 1u << INPUT_BUTTON_MIDDLE,
	};
	struct key_event keys[KEY_EVENT_QUEUE_LEN];
	size_t nkeys;
	uint32_t buttons_new = 0;
	int dx, dy, wheel;

	pthread_mutex_lock(&input_mutex);
	nkeys = num_pending_keys;
	memcpy(keys, key_event_queue, nkeys * sizeof(keys[0]));
	num_pending_keys = 0;
	dx = mouse_dx;
	dy = mouse_dy;
	mouse_dx = 0;
	mouse_dy = 0;
	wheel = wheel_accum;
	wheel_accum = 0;
	for (size_t i = 0; i < INPUT_BUTTON__MAX; i++) {
		if (buttons_down[i]) {
			buttons_new |= 1u << i;
		}
	}
	pthread_mutex_unlock(&input_mutex);

	bool changed = nkeys || dx || dy || wheel || buttons_new != buttons_old;

	for (size_t i = 0; i < nkeys; i++) {
		CALL_QEMU_FUNC(qkbd_state_key_event, kbd, keys[i].key,
			       keys[i].down);
	}

	if (dx) {
		CALL_QEMU_FUNC(qemu_input_queue_rel, dcl.con, INPUT_AXIS_X, dx);
	}
	if (dy) {
		CALL_QEMU_FUNC(qemu_input_queue_rel, dcl.con, INPUT_AXIS_Y, dy);
	}

	CALL_QEMU_FUNC(qemu_input_update_buttons, dcl.con, btn_map,
		       buttons_old, buttons_new);
	buttons_old = buttons_new;

	// One down-then-up pulse per detent; the PS/2 model counts wheel
	// motion on down events and ignores ups.
	for (; wheel > 0; wheel--) {
		CALL_QEMU_FUNC(qemu_input_queue_btn, dcl.con,
			       INPUT_BUTTON_WHEEL_UP, true);
		CALL_QEMU_FUNC(qemu_input_queue_btn, dcl.con,
			       INPUT_BUTTON_WHEEL_UP, false);
	}
	for (; wheel < 0; wheel++) {
		CALL_QEMU_FUNC(qemu_input_queue_btn, dcl.con,
			       INPUT_BUTTON_WHEEL_DOWN, true);
		CALL_QEMU_FUNC(qemu_input_queue_btn, dcl.con,
			       INPUT_BUTTON_WHEEL_DOWN, false);
	}

	if (changed) {
		CALL_QEMU_FUNC(qemu_input_event_sync);
	}
}

static void display_init(DisplayState *ds, DisplayOptions *o)
{
	dcl.con = CALL_QEMU_FUNC(qemu_console_lookup_by_index, 0);
	kbd = CALL_QEMU_FUNC(qkbd_state_init, dcl.con);
	input_bh = CALL_QEMU_FUNC(qemu_bh_new_full, input_inject_bh, NULL,
				  "libretro-input", NULL);
	disk_bh = CALL_QEMU_FUNC(qemu_bh_new_full, disk_swap_bh, NULL,
				 "libretro-disk", NULL);
	CALL_QEMU_FUNC(register_displaychangelistener, &dcl);
#ifdef CONFIG_QEMU_3DFX
	CALL_QEMU_FUNC(qemu_fx_register_sink, &fx_sink);
#endif
}

static QemuDisplay display = {
	.type = DISPLAY_TYPE_LIBRETRO,
	.init = display_init,
};

static struct audio_pcm_ops pcm_ops = {
	.init_out = audio_init_out,
	.fini_out = audio_fini_out,
	.enable_out = audio_enable_out,
	.write = audio_write,
	.buffer_get_free = audio_buffer_get_free,
	.get_buffer_out = audio_get_buffer_out,
	.put_buffer_out = audio_put_buffer_out,
};

static struct audio_driver libretro_audio_driver = {
	.name = "libretro",
	.descr = "libretro https://www.libretro.com/",
	.init = audio_init,
	.fini = audio_fini,
	.pcm_ops = &pcm_ops,
	.max_voices_out = 1,
	.max_voices_in = 0,
	.voice_size_out = sizeof(HWVoiceOut),
	.voice_size_in = 0,
};

static void register_libretro(void)
{
	CALL_QEMU_FUNC(qemu_display_register, &display);
	CALL_QEMU_FUNC(audio_driver_register, &libretro_audio_driver);
}

void vreport(report_type type, const char *fmt, va_list ap)
{
	unsigned duration = 5000; // 5 seconds
	enum retro_log_level level;
	switch (type) {
	case REPORT_TYPE_WARNING:
		level = RETRO_LOG_WARN;
		break;
	case REPORT_TYPE_INFO:
		level = RETRO_LOG_INFO;
		break;
	default:
		level = RETRO_LOG_ERROR;
		// I can't find a way to make it permanent, so instead display for a full day
		duration = 1000 * 60 * 60 * 24;
		break;
	}

	char *msg = g_strdup_vprintf(fmt, ap);

	fprintf(stderr, "%s\n", msg);

	// This can be called from any QEMU thread, but libretro environment
	// calls are only safe on the frontend thread — queue the message for
	// retro_run to deliver.
	pthread_mutex_lock(&msg_mutex);
	if (num_pending_msgs == PENDING_MSGS_LEN) {
		// Drop the oldest, not the newest: a failing boot produces a
		// burst, and whatever comes after the burst is what the user
		// still needs to see. The drops themselves are reported by
		// flush_pending_messages.
		g_free(pending_msgs[0].msg);
		memmove(&pending_msgs[0], &pending_msgs[1],
			(PENDING_MSGS_LEN - 1) * sizeof(pending_msgs[0]));
		num_pending_msgs--;
		num_dropped_msgs++;
	}
	pending_msgs[num_pending_msgs++] = (struct pending_msg){
		.msg = msg,
		.duration = duration,
		.level = level,
	};
	pthread_mutex_unlock(&msg_mutex);

	// Deliberately non-fatal: QEMU calls error_report() for plenty of
	// recoverable conditions (failed device emulation, bad options, I/O
	// hiccups). Tearing down the emulator from here — especially from a
	// vcpu or worker thread — kills threads mid-write and corrupts guest
	// disks. Fatal paths must opt in explicitly (early_error_report).
}

// Deliver queued vreport messages; frontend thread only.
static void flush_pending_messages(void)
{
	struct pending_msg local[PENDING_MSGS_LEN];
	size_t n;
	unsigned dropped;

	pthread_mutex_lock(&msg_mutex);
	n = num_pending_msgs;
	memcpy(local, pending_msgs, n * sizeof(local[0]));
	num_pending_msgs = 0;
	dropped = num_dropped_msgs;
	num_dropped_msgs = 0;
	pthread_mutex_unlock(&msg_mutex);

	// Delivered before the retained tail so the on-screen order
	// matches what actually happened.
	if (dropped && n < PENDING_MSGS_LEN) {
		memmove(&local[1], &local[0], n * sizeof(local[0]));
		local[0] = (struct pending_msg){
			.msg = g_strdup_printf(
				"(%u earlier QEMU messages dropped)", dropped),
			.duration = 5000,
			.level = RETRO_LOG_WARN,
		};
		n++;
	}

	for (size_t i = 0; i < n; i++) {
		bool shown = cb_env(RETRO_ENVIRONMENT_SET_MESSAGE_EXT,
				    &(struct retro_message_ext){
					    .msg = local[i].msg,
					    .duration = local[i].duration,
					    .priority = 0,
					    .level = local[i].level,
					    .target = RETRO_MESSAGE_TARGET_ALL,
					    .type = RETRO_MESSAGE_TYPE_NOTIFICATION,
				    });
		if (!shown) {
			// SET_MESSAGE_EXT needs RetroArch 1.9+; older ones ship
			// 1.7.5, so fall back to the original message API
			// (duration is in frames, assume 60 fps).
			cb_env(RETRO_ENVIRONMENT_SET_MESSAGE,
			       &(struct retro_message){
				       .msg = local[i].msg,
				       .frames = local[i].duration * 60 / 1000,
			       });
		}
		g_free(local[i].msg);
	}
}

G_GNUC_PRINTF(1, 2)
static void early_error_report(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vreport(REPORT_TYPE_ERROR, fmt, ap);
	va_end(ap);

	// Only used on the emu thread before/instead of running QEMU; exit
	// so the frontend's handshake is released instead of hanging.
	emu_thread_exit();
}

// Non-fatal user-facing notice, deliverable from any thread (rides the
// vreport queue; retro_run flushes it).
G_GNUC_PRINTF(1, 2)
static void notice_report(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vreport(REPORT_TYPE_INFO, fmt, ap);
	va_end(ap);
}

// Fetch a core option's current value; frontend thread only. NULL when
// unset or unavailable; the string is owned by the frontend.
static const char *get_var(const char *key)
{
	struct retro_variable var = { .key = key };

	if (!cb_env || !cb_env(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) {
		return NULL;
	}
	return var.value;
}

// Unknown/unset/"auto" parse to the auto sentinel (0 here, -1 for the
// numeric 3dfx options) rather than erroring: a stale options file from
// an older core version must not break booting.
static int parse_choice(const struct choice *table, const char *s)
{
	if (!s) {
		return 0;
	}
	for (; table->str; table++) {
		if (!strcmp(table->str, s)) {
			return table->val;
		}
	}
	return 0;
}

static const char *parse_accel(const char *s)
{
	if (s && !strcmp(s, "WHPX")) {
		return "whpx";
	}
	if (s && !strcmp(s, "TCG")) {
		return "tcg";
	}
	return NULL;
}

static int parse_int_auto(const char *s)
{
	if (!s || !g_ascii_isdigit(s[0])) {
		return -1;
	}
	return atoi(s);
}

// Read all core options; frontend thread only. On first load (called in
// retro_load_game before the emu thread exists — 1.7.5 has only loaded
// its options file by then, retro_set_environment is too early) the
// boot-time options are snapshotted for emu_thread_fn's argv surgery.
// Afterwards a boot-time change only earns a one-shot "takes effect on
// restart" notice; reverting the option re-arms it.
static void update_variables(bool first_load)
{
	int ram = parse_choice(ram_choices, get_var("qemu_ram"));
	int boot = parse_choice(boot_choices, get_var("qemu_boot_order"));
	const char *accel = parse_accel(get_var("qemu_accel"));
	int whpx_isolation = parse_choice(whpx_isolation_choices,
					  get_var("qemu_whpx_isolation"));
	int abuf = parse_choice(audio_buffer_choices,
				get_var("qemu_audio_buffer"));

	if (first_load) {
		opt_ram_mb = ram;
		opt_boot_order_char = boot;
		opt_accel = accel;
		opt_whpx_isolation = whpx_isolation;
		opt_audio_buffer_us = abuf;
	} else {
		const struct {
			bool changed;
			const char *label;
		} boot_opts[] = {
			{ ram != opt_ram_mb, "Guest RAM" },
			{ boot != opt_boot_order_char, "Boot device" },
			// Pointer compare is exact: parse_accel returns
			// interned literals or NULL.
			{ accel != opt_accel, "CPU accelerator" },
			{ whpx_isolation != opt_whpx_isolation,
			  "WHPX security isolation" },
			{ abuf != opt_audio_buffer_us, "Audio buffer" },
		};
		for (size_t i = 0; i < G_N_ELEMENTS(boot_opts); i++) {
			if (!boot_opts[i].changed) {
				restart_notified_mask &= ~(1u << i);
			} else if (!(restart_notified_mask & (1u << i))) {
				restart_notified_mask |= 1u << i;
				notice_report(
					"%s change takes effect on restart",
					boot_opts[i].label);
			}
		}
	}

	int speed = parse_choice(mouse_speed_choices,
				 get_var("qemu_mouse_speed"));
	mouse_speed_pct = speed ? speed : 100;

#ifdef CONFIG_QEMU_3DFX
	libretro_cfg_fps_limit = parse_int_auto(get_var("qemu_3dfx_fps_limit"));
	libretro_cfg_ext_year = parse_int_auto(get_var("qemu_3dfx_ext_year"));
#endif
}

// argv surgery for the boot-time option overrides. args holds g_strdup'd
// tokens; only "-"-prefixed flag tokens are matched, so argv[0] (the
// qemu-system-ARCH word) is never touched.
static int args_find(GPtrArray *args, const char *flag)
{
	for (guint i = 0; i < args->len; i++) {
		if (!strcmp(args->pdata[i], flag)) {
			return i;
		}
	}
	return -1;
}

// Replace the value after an existing flag, or append the pair.
static void args_set(GPtrArray *args, const char *flag, const char *value)
{
	int i = args_find(args, flag);

	if (i >= 0 && (guint)(i + 1) < args->len) {
		g_free(args->pdata[i + 1]);
		args->pdata[i + 1] = g_strdup(value);
	} else {
		g_ptr_array_add(args, g_strdup(flag));
		g_ptr_array_add(args, g_strdup(value));
	}
}

static void args_remove_all(GPtrArray *args, const char *flag)
{
	int i;

	while ((i = args_find(args, flag)) >= 0) {
		// Take the value with it unless the next token is a flag.
		guint n = ((guint)(i + 1) < args->len &&
			   ((char *)args->pdata[i + 1])[0] != '-') ?
				  2 :
				  1;
		g_ptr_array_remove_range(args, i, n);
	}
}

// Rewrite out.buffer-length on the libretro audiodev's option string.
// Only the token following "-audiodev" whose driver is libretro is
// touched; a command line without one gets a notice instead of a second
// audiodev (a duplicate id fails qemu_init).
static void apply_audio_buffer(GPtrArray *args, int usecs)
{
	for (guint i = 0; i + 1 < args->len; i++) {
		if (strcmp(args->pdata[i], "-audiodev") != 0 ||
		    !g_str_has_prefix(args->pdata[i + 1], "libretro")) {
			continue;
		}
		char **parts = g_strsplit(args->pdata[i + 1], ",", -1);
		GString *out = g_string_new(NULL);
		for (char **p = parts; *p; p++) {
			if (g_str_has_prefix(*p, "out.buffer-length=")) {
				continue;
			}
			if (out->len) {
				g_string_append_c(out, ',');
			}
			g_string_append(out, *p);
		}
		g_string_append_printf(out, ",out.buffer-length=%d", usecs);
		g_strfreev(parts);
		g_free(args->pdata[i + 1]);
		args->pdata[i + 1] = g_string_free(out, FALSE);
		return;
	}
	notice_report(
		"Audio buffer option ignored: no libretro audiodev in the command line");
}

// Set the WHPX SeparateSecurityDomain accelerator property without
// disturbing fallback accelerators or unrelated WHPX properties. QEMU's
// default is isolation on; this only runs for an explicit non-auto option.
static void apply_whpx_isolation(GPtrArray *args, bool enabled)
{
	bool found = false;

	for (guint i = 0; i + 1 < args->len; i++) {
		const char *spec = args->pdata[i + 1];

		if (strcmp(args->pdata[i], "-accel") != 0 ||
		    (strcmp(spec, "whpx") != 0 &&
		     !g_str_has_prefix(spec, "whpx,"))) {
			continue;
		}

		char **parts = g_strsplit(spec, ",", -1);
		GString *out = g_string_new("whpx");

		for (char **p = parts + 1; *p; p++) {
			if (g_str_has_prefix(*p, "ssd=")) {
				continue;
			}
			g_string_append_c(out, ',');
			g_string_append(out, *p);
		}
		g_string_append_printf(out, ",ssd=%s", enabled ? "on" : "off");
		g_strfreev(parts);
		g_free(args->pdata[i + 1]);
		args->pdata[i + 1] = g_string_free(out, FALSE);
		found = true;
	}

	if (!found) {
		notice_report(
			"WHPX security isolation option ignored: no -accel whpx entry in the command line");
	}
}

static void apply_option_overrides(GPtrArray *args)
{
	char buf[16];

	if (opt_ram_mb) {
		g_snprintf(buf, sizeof(buf), "%d", opt_ram_mb);
		args_set(args, "-m", buf);
	}
	if (opt_boot_order_char) {
		// Wholesale: a pre-existing "-boot menu=on" etc. is replaced;
		// forcing the boot device is the point of the option.
		g_snprintf(buf, sizeof(buf), "order=%c", opt_boot_order_char);
		args_set(args, "-boot", buf);
	}
	if (opt_accel) {
		// Remove the whole fallback chain ("-accel whpx -accel tcg");
		// appending alone would leave the first entry winning.
		args_remove_all(args, "-accel");
		g_ptr_array_add(args, g_strdup("-accel"));
		g_ptr_array_add(args, g_strdup(opt_accel));
	}
	if (opt_whpx_isolation) {
		apply_whpx_isolation(args, opt_whpx_isolation > 0);
	}
	if (opt_audio_buffer_us) {
		apply_audio_buffer(args, opt_audio_buffer_us);
	}
}

static void start_qemu_with_args(const char *argv[])
{
	int argc = 0;
	while (argv[argc]) {
		argc++;
	}

	if (!system_dir) {
		early_error_report("failed finding libretro system directory");
		return;
	}
	CALL_QEMU_FUNC(qemu_add_data_dir,
		       g_build_filename(system_dir, "qemu", NULL));

	CALL_QEMU_FUNC(register_module_init, register_libretro,
		       MODULE_INIT_QOM);
	CALL_QEMU_FUNC(rcu_init);
	CALL_QEMU_FUNC(qemu_init, argc, (char **)argv);
	CALL_QEMU_FUNC(qemu_default_main);

	emu_thread_exit();
}

size_t retro_serialize_size(void)
{
	return 0;
}

bool retro_serialize(void *data, size_t size)
{
	return false;
}

bool retro_unserialize(const void *data, size_t size)
{
	return false;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

// Parse an .m3u playlist into `out` as absolute paths: one disc image
// per line, blank lines and '#' comments skipped, UTF-8 BOM and CRLF
// tolerated, relative entries resolved against the playlist's own
// directory (not the cwd — the emu thread chdirs later). No nesting.
// Read failure is a notice, not an error: a missing boot medium is
// caught by qemu_init, and a missing sidecar disc only matters if the
// user tries to insert it.
static void m3u_parse(const char *m3u_path, GPtrArray *out)
{
	char *text = NULL;
	if (!g_file_get_contents(m3u_path, &text, NULL, NULL)) {
		notice_report("could not read playlist '%s'", m3u_path);
		return;
	}
	char *dir = g_path_get_dirname(m3u_path);
	char **lines = g_strsplit(text, "\n", -1);
	g_free(text);
	for (size_t i = 0; lines[i]; i++) {
		char *line = lines[i];
		if (i == 0 && g_str_has_prefix(line, "\xef\xbb\xbf")) {
			line += 3;
		}
		line = g_strstrip(line); // also eats the CR of CRLF files
		if (!line[0] || line[0] == '#') {
			continue;
		}
		g_ptr_array_add(out, g_path_is_absolute(line) ?
					     g_canonicalize_filename(line, NULL) :
					     g_canonicalize_filename(line, dir));
	}
	g_strfreev(lines);
	g_free(dir);
}

static int disc_find(const char *abs_path)
{
	for (unsigned i = 0; i < disc_list->len; i++) {
		if (!strcmp(disc_list->pdata[i], abs_path)) {
			return (int)i;
		}
	}
	return -1;
}

// The medium the machine boots with must be swappable back to, so make
// sure it's in the disc list (playlist entries keep their listed order
// after it) and start the disk-control index on it.
static void disc_ensure_boot(const char *abs_path)
{
	if (disc_find(abs_path) < 0) {
		g_ptr_array_insert(disc_list, 0, g_strdup(abs_path));
	}
	disc_index = disc_find(abs_path);
}

// A playlist sitting next to the content ("foo.iso" + "foo.m3u") seeds
// the disc list without changing what boots — invisible to frontends
// and scanners that only know the content file.
static void sidecar_m3u_parse(const char *content_path)
{
	const char *dot = strrchr(content_path, '.');
	char *m3u = g_strdup_printf("%.*s.m3u", (int)(dot - content_path),
				    content_path);
	if (g_file_test(m3u, G_FILE_TEST_IS_REGULAR)) {
		m3u_parse(m3u, disc_list);
		// The same playlist may double as the content form, whose
		// first line names the boot config. A machine description
		// is never an insertable disc, so drop such entries — this
		// makes opening the .qemu_cmd_line and opening the .m3u
		// converge on the same disc set.
		for (unsigned i = disc_list->len; i-- > 0;) {
			if (g_str_has_suffix(disc_list->pdata[i],
					     ".qemu_cmd_line")) {
				g_ptr_array_remove_index(disc_list, i);
			}
		}
	}
	g_free(m3u);
}

// Best-effort scan for the boot config's -cdrom medium so it lands in
// the disc list and the disk-control index starts on it; returns
// whether one was found. Failures are silently ignored here —
// emu_thread_fn re-parses the file and owns the fatal error path.
static bool scan_cmd_line_cdrom(const char *cmd_line_path)
{
	char *cmd_line = NULL;
	char **argv = NULL;
	bool found = false;
	if (!g_file_get_contents(cmd_line_path, &cmd_line, NULL, NULL)) {
		return false;
	}
	bool ok = g_shell_parse_argv(cmd_line, NULL, &argv, NULL);
	g_free(cmd_line);
	if (!ok) {
		return false;
	}
	for (char **p = argv; *p; p++) {
		if (!strcmp(*p, "-cdrom") && p[1]) {
			// Relative media resolve against the command-file dir:
			// that's QEMU's cwd once emu_thread_fn chdirs.
			char *dir = g_path_get_dirname(cmd_line_path);
			char *abs = g_path_is_absolute(p[1]) ?
					    g_canonicalize_filename(p[1], NULL) :
					    g_canonicalize_filename(p[1], dir);
			disc_ensure_boot(abs);
			g_free(abs);
			g_free(dir);
			found = true;
			break;
		}
	}
	g_strfreev(argv);
	return found;
}

// Hand a tray/medium operation to disk_swap_bh. Frontend thread.
static void disk_request(enum disk_request_type type, const char *filename)
{
	pthread_mutex_lock(&disk_mutex);
	g_free(disk_req.filename);
	disk_req.type = type;
	disk_req.filename = g_strdup(filename);
	pthread_mutex_unlock(&disk_mutex);
	// disk_bh is only created once QEMU is up (display_init), so a
	// non-NULL pointer means the main loop exists to run it.
	if (disk_bh && !exited) {
		CALL_QEMU_FUNC(qemu_bh_schedule, disk_bh);
	}
}

// Close the tray with the current selection. Frontend thread.
static void disk_commit(void)
{
	if (disc_index < disc_list->len &&
	    ((const char *)disc_list->pdata[disc_index])[0]) {
		disk_request(DISK_REQ_CHANGE, disc_list->pdata[disc_index]);
	} else {
		// Past the end (frontend removed the disc without
		// inserting another — RetroArch 1.7.5 clamps and never
		// sends it) or an append placeholder that never received its
		// image: remove the medium and close the empty tray.
		disk_request(DISK_REQ_EJECT_CLOSE, NULL);
	}
}

// Apply a pending tray/medium operation. Runs as a bottom half on the main loop
// thread under the BQL — the context the qmp_* handlers require.
static void disk_swap_bh(void *opaque)
{
	pthread_mutex_lock(&disk_mutex);
	enum disk_request_type type = disk_req.type;
	char *filename = disk_req.filename;
	disk_req.type = DISK_REQ_NONE;
	disk_req.filename = NULL;
	pthread_mutex_unlock(&disk_mutex);
	if (type == DISK_REQ_NONE) {
		return; // an earlier dispatch already applied the request
	}

	// Target the machine's first CD-ROM drive: removable media and a
	// tray (floppies have removable media but no tray).
	BlockBackend *blk = NULL;
	for (BlockBackend *b = CALL_QEMU_FUNC(blk_next, NULL); b;
	     b = CALL_QEMU_FUNC(blk_next, b)) {
		if (CALL_QEMU_FUNC(blk_dev_has_removable_media, b) &&
		    CALL_QEMU_FUNC(blk_dev_has_tray, b)) {
			blk = b;
			break;
		}
	}
	if (!blk) {
		notice_report("disc swap: no CD-ROM drive in this machine");
		g_free(filename);
		return;
	}

	// force=true: Windows guests lock the tray (PREVENT MEDIUM REMOVAL)
	// while a disc is mounted, which would fail exactly when an installer
	// asks for the next disc. The change path raises the ATAPI "medium may
	// have changed" sequence, so the guest re-reads the new disc.
	const char *dev = CALL_QEMU_FUNC(blk_name, blk);
	Error *err = NULL;
	switch (type) {
	case DISK_REQ_OPEN:
		CALL_QEMU_FUNC(qmp_blockdev_open_tray, dev, NULL,
			       true, true, &err);
		break;
	case DISK_REQ_CLOSE:
		CALL_QEMU_FUNC(qmp_blockdev_close_tray, dev, NULL, &err);
		break;
	case DISK_REQ_CHANGE:
		CALL_QEMU_FUNC(qmp_blockdev_change_medium, dev, NULL,
			       filename, NULL, true, true, false,
			       BLOCKDEV_CHANGE_READ_ONLY_MODE_RETAIN, &err);
		break;
	case DISK_REQ_EJECT_CLOSE:
		CALL_QEMU_FUNC(qmp_eject, dev, NULL, true, true, &err);
		if (!err) {
			CALL_QEMU_FUNC(qmp_blockdev_close_tray, dev, NULL, &err);
		}
		break;
	case DISK_REQ_NONE:
		g_assert_not_reached();
	}
	if (err) {
		notice_report("disc swap failed: %s",
			      CALL_QEMU_FUNC(error_get_pretty, err));
		CALL_QEMU_FUNC(error_free, err);
	}
	g_free(filename);
}

// The libretro v0 disk-control callbacks (frontend thread). Every state
// transition is forwarded to QEMU so the state reported to the frontend
// agrees with the guest-visible tray.
static bool disk_set_eject_state(bool ejected)
{
	if (ejected == tray_ejected) {
		return true;
	}
	tray_ejected = ejected;
	if (ejected) {
		disc_change_pending = false;
		disk_request(DISK_REQ_OPEN, NULL);
	} else if (disc_change_pending) {
		disc_change_pending = false;
		disk_commit();
	} else {
		disk_request(DISK_REQ_CLOSE, NULL);
	}
	return true;
}

static bool disk_get_eject_state(void)
{
	return tray_ejected;
}

static unsigned disk_get_image_index(void)
{
	return disc_index;
}

static bool disk_set_image_index(unsigned index)
{
	if (!tray_ejected) {
		return false; // only legal while the tray is open
	}
	// An unchanged index is a no-op by spec — it must not queue a
	// re-insert, or open/close without moving the index would fire a
	// phantom media-change at the guest. A pending change set by
	// replace_image_index survives.
	if (index != disc_index) {
		disc_index = index;
		disc_change_pending = true;
	}
	return true;
}

static unsigned disk_get_num_images(void)
{
	return disc_list ? disc_list->len : 0;
}

static bool disk_replace_image_index(unsigned index,
				     const struct retro_game_info *info)
{
	if (!disc_list || index >= disc_list->len) {
		return false;
	}
	if (info) {
		if (!info->path) {
			return false;
		}
		g_free(disc_list->pdata[index]);
		disc_list->pdata[index] =
			g_canonicalize_filename(info->path, NULL);
		if (index == disc_index) {
			disc_change_pending = true;
		}
	} else {
		// NULL = the frontend dropped this image; the list
		// shifts down and the selection follows its disc — or
		// collapses onto the neighbor if its own disc vanished.
		g_ptr_array_remove_index(disc_list, index);
		if (index < disc_index) {
			disc_index--;
		} else if (index == disc_index) {
			if (disc_index >= disc_list->len) {
				disc_index = disc_list->len ?
						     disc_list->len - 1 :
						     0;
			}
			disc_change_pending = true;
		}
	}
	return true;
}

static bool disk_add_image_index(void)
{
	if (!disc_list) {
		return false;
	}
	// Placeholder slot; the frontend fills it right away with
	// replace_image_index (the documented append sequence).
	g_ptr_array_add(disc_list, g_strdup(""));
	return true;
}

// Never registered with NULL/empty state: RetroArch 1.7.5 dereferences
// the env-call data without a NULL check (the documented "deregister"
// crashes it), and it shows the Disk Options menu whenever the
// interface exists, even with zero images.
static const struct retro_disk_control_callback disk_control_cb = {
	.set_eject_state = disk_set_eject_state,
	.get_eject_state = disk_get_eject_state,
	.get_image_index = disk_get_image_index,
	.set_image_index = disk_set_image_index,
	.get_num_images = disk_get_num_images,
	.replace_image_index = disk_replace_image_index,
	.add_image_index = disk_add_image_index,
};

static void *emu_thread_fn(void *arg)
{
	const char *working_path = boot_cmd_line_path ? boot_cmd_line_path :
						 game_path;
	char *working_dir = g_path_get_dirname(working_path);
	g_chdir(working_dir);
	g_free(working_dir);

	// All content branches build argv here so core-option overrides
	// apply uniformly. Intentionally leaked: start_qemu_with_args blocks
	// until session end and never copies, and target_arch aliases
	// pdata[0] and is read again at session end (emu_thread_exit,
	// retro_unload_game).
	GPtrArray *args = g_ptr_array_new_with_free_func(g_free);

	if (boot_cmd_line_path) {
		char *cmd_line = NULL;
		// Must start NULL: GLib only stores into a NULL GError*, and
		// the error paths below dereference it.
		GError *error = NULL;
		bool success = g_file_get_contents(boot_cmd_line_path,
						   &cmd_line, NULL, &error);
		if (!success) {
			early_error_report("failed reading file '%s':\n%s",
					   boot_cmd_line_path, error->message);
			return NULL;
		}
		char **argv;
		success = g_shell_parse_argv(cmd_line, NULL, &argv, &error);
		g_free(cmd_line);
		if (!success) {
			early_error_report("failed parsing file '%s':\n%s",
					   boot_cmd_line_path, error->message);
			return NULL;
		}
		if (!argv[0]) {
			early_error_report("empty command in file '%s'",
					   boot_cmd_line_path);
			return NULL;
		}
		if (!g_str_has_prefix(argv[0], QEMU_CMD_PREFIX)) {
			early_error_report(
				"command must be of the form " QEMU_CMD_PREFIX
				"ARCH, not '%s': '%s'",
				argv[0], boot_cmd_line_path);
			return NULL;
		}
		for (char **p = argv; *p; p++) {
			g_ptr_array_add(args, *p);
		}
		g_free(argv); // tokens now owned by args
	} else if (boot_cdrom_path) {
		const char *base[] = { QEMU_CMD, "-cdrom", boot_cdrom_path,
				       NULL };
		for (const char **p = base; *p; p++) {
			g_ptr_array_add(args, g_strdup(*p));
		}
	} else {
		const char *base[] = { QEMU_CMD, game_path, NULL };
		for (const char **p = base; *p; p++) {
			g_ptr_array_add(args, g_strdup(*p));
		}
	}

	apply_option_overrides(args);
	target_arch = (char *)args->pdata[0] + strlen(QEMU_CMD_PREFIX);
	g_ptr_array_add(args, NULL);

	// The effective command line, post-overrides — the first thing to
	// ask for in a bug report.
	char *joined = g_strjoinv(" ", (char **)args->pdata);
	fprintf(stderr, "qemu-libretro argv: %s\n", joined);
	g_free(joined);

	start_qemu_with_args((const char **)args->pdata);
	return NULL;
}

bool retro_load_game(const struct retro_game_info *game)
{
	if (!game) {
		return false;
	}

	// Fresh disk-control state; statics persist across load cycles
	// and the previous emu thread (if any) is already joined.
	pthread_mutex_lock(&disk_mutex);
	g_free(disk_req.filename);
	disk_req.filename = NULL;
	disk_req.type = DISK_REQ_NONE;
	pthread_mutex_unlock(&disk_mutex);
	if (disc_list) {
		g_ptr_array_set_size(disc_list, 0);
	} else {
		disc_list = g_ptr_array_new_with_free_func(g_free);
	}
	disc_index = 0;
	tray_ejected = false;
	disc_change_pending = false;
	g_free(boot_cmd_line_path);
	g_free(boot_cdrom_path);
	boot_cmd_line_path = NULL;
	boot_cdrom_path = NULL;
	// Session flags too: the API guarantees retro_unload_game ran (and
	// joined the previous emu thread), but a stale exited=true would
	// silently stop the new session's BH scheduling (input and disk)
	// and make retro_run request shutdown immediately.
	exited = false;
	emu_thread_started = false;
	emu_thread_joined = false;

	// The frontend only guarantees game->path stays valid during this
	// call, and old frontends (RetroArch 1.7.5-era) reuse the
	// string quickly, so copy it before the emu thread reads it.
	// Copy = canonicalize: frontends also pass the path relative to
	// their own cwd (EmuVR launches with "..\Games\..."), which stops
	// resolving the moment emu_thread_fn chdirs to the game directory —
	// so make it absolute while the cwd is still the frontend's.
	game_path = g_canonicalize_filename(game->path, NULL);
	// Snapshot the core options before the emu thread exists: the
	// thread-creation happens-before lets emu_thread_fn read the opt_*
	// statics without locking.
	update_variables(true);
	// Content dispatch: the boot descriptor picks the argv branch in
	// emu_thread_fn, the disc list feeds the disk-control interface.
	// Everything is canonicalized while the cwd is still the
	// frontend's, and published by pthread_create below.
	if (g_str_has_suffix(game_path, ".m3u")) {
		GPtrArray *entries = g_ptr_array_new_with_free_func(g_free);
		m3u_parse(game_path, entries);
		unsigned first_disc = 0;
		if (entries->len &&
		    g_str_has_suffix(entries->pdata[0], ".qemu_cmd_line")) {
			// First entry names the boot config; the rest
			// are the disc set.
			boot_cmd_line_path = g_strdup(entries->pdata[0]);
			first_disc = 1;
		}
		for (unsigned i = first_disc; i < entries->len; i++) {
			// Configs are never discs; only the first line may
			// name one (handled above).
			if (g_str_has_suffix(entries->pdata[i],
					     ".qemu_cmd_line")) {
				continue;
			}
			g_ptr_array_add(disc_list,
					g_strdup(entries->pdata[i]));
		}
		g_ptr_array_free(entries, TRUE);
		if (!boot_cmd_line_path) {
			if (!disc_list->len) {
				notice_report("playlist '%s' lists no discs",
					      game_path);
				return false;
			}
			boot_cdrom_path = g_strdup(disc_list->pdata[0]);
		}
	} else if (g_str_has_suffix(game_path, ".qemu_cmd_line")) {
		boot_cmd_line_path = g_strdup(game_path);
		sidecar_m3u_parse(game_path);
	} else if (g_str_has_suffix(game_path, ".iso")) {
		boot_cdrom_path = g_strdup(game_path);
		sidecar_m3u_parse(game_path);
	}
	if (boot_cmd_line_path) {
		if (!scan_cmd_line_cdrom(boot_cmd_line_path)) {
			// No boot medium: the drive starts empty, which
			// the v0 API spells index >= get_num_images() —
			// starting at 0 would make disc 0 uninsertable
			// (an unchanged index is a no-op by spec).
			disc_index = disc_list->len;
		}
	} else if (boot_cdrom_path) {
		disc_ensure_boot(boot_cdrom_path);
	}
	if (disc_list->len > 0) {
		cb_env(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE,
		       (void *)&disk_control_cb);
	}
	// The frontend sized its texture from retro_get_system_av_info, so
	// the delivered envelope starts at whatever base that reported
	// (av_info persists across load cycles).
	pthread_mutex_lock(&av_info_lock);
	delivered_env_w = av_info.geometry.base_width;
	delivered_env_h = av_info.geometry.base_height;
	pthread_mutex_unlock(&av_info_lock);
	if (pthread_create(&emu_thread, NULL, emu_thread_fn, NULL) != 0) {
		return false;
	}
	emu_thread_started = true;
	return true;
}

bool retro_load_game_special(unsigned game_type,
			     const struct retro_game_info *info,
			     size_t num_info)
{
	return false;
}

void retro_unload_game(void)
{
	// target_arch is set by the emu thread just before QEMU starts; if
	// it's still NULL there is no QEMU instance to ask for shutdown (and
	// the per-target dispatch can't handle NULL).
	if (!exited && target_arch) {
		// Request shutdown
		CALL_QEMU_FUNC(qemu_system_shutdown_request,
			       SHUTDOWN_CAUSE_HOST_UI);
	}
	join_emu_thread();
}

unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
	return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
	return 0;
}

// Scale a mouse delta by mouse_speed_pct with remainder carry so
// sub-unit motion at slow speeds accumulates instead of vanishing;
// exact identity at 100%. Frontend thread only.
static int scale_mouse(int d, int *rem)
{
	long num = (long)d * mouse_speed_pct + *rem;
	int out = (int)(num / 100);

	*rem = (int)(num - (long)out * 100);
	return out;
}

// Throwaway probe (pre-RetroPad scoping — remove with the real mapper):
// reports what the frontend actually delivers on the joypad/analog
// interface. Edge-triggered, so silent on frontends that deliver
// nothing. Frontend thread only.
static void pad_probe(void)
{
	static const char *const btn_names[16] = {
		"B",	"Y",	 "Select", "Start", "Up", "Down",
		"Left", "Right", "A",	   "X",	    "L",  "R",
		"L2",	"R2",	 "L3",	   "R3",
	};
	static uint16_t down_prev;
	static unsigned analog_hold;
	uint16_t down_now = 0;

	for (unsigned i = 0; i < 16; i++) {
		if (cb_input_state(0, RETRO_DEVICE_JOYPAD, 0, i)) {
			down_now |= 1u << i;
		}
	}
	for (unsigned i = 0; i < 16; i++) {
		if ((down_now ^ down_prev) & (1u << i)) {
			notice_report("[pad] %s %s", btn_names[i],
				      (down_now & (1u << i)) ? "down" : "up");
		}
	}
	down_prev = down_now;

	int lx = cb_input_state(0, RETRO_DEVICE_ANALOG,
				RETRO_DEVICE_INDEX_ANALOG_LEFT,
				RETRO_DEVICE_ID_ANALOG_X);
	int ly = cb_input_state(0, RETRO_DEVICE_ANALOG,
				RETRO_DEVICE_INDEX_ANALOG_LEFT,
				RETRO_DEVICE_ID_ANALOG_Y);
	int rx = cb_input_state(0, RETRO_DEVICE_ANALOG,
				RETRO_DEVICE_INDEX_ANALOG_RIGHT,
				RETRO_DEVICE_ID_ANALOG_X);
	int ry = cb_input_state(0, RETRO_DEVICE_ANALOG,
				RETRO_DEVICE_INDEX_ANALOG_RIGHT,
				RETRO_DEVICE_ID_ANALOG_Y);
	if (analog_hold) {
		analog_hold--;
	} else if (ABS(lx) > 8192 || ABS(ly) > 8192 || ABS(rx) > 8192 ||
		   ABS(ry) > 8192) {
		notice_report("[pad] L(%d,%d) R(%d,%d)", lx, ly, rx, ry);
		analog_hold = 120; // ~2 s between analog reports
	}
}

void retro_run(void)
{
#ifdef CONFIG_QEMU_3DFX
	qatomic_inc(&frontend_frame_count);
#endif
	// Last keyboard state this thread saw; used for edge detection
	// against the polled state below. Frontend thread only.
	static bool key_down_prev[RETROK_LAST];

	cb_input_poll();

	// The frontend flags option changes instead of signalling them;
	// poll the flag and re-read on demand.
	bool vars_updated = false;
	if (cb_env(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &vars_updated) &&
	    vars_updated) {
		update_variables(false);
	}

	pad_probe();

	// Sample all input into locals first — a frontend is free to do
	// arbitrary work inside input_state, so no lock is held across it.
	int dx = cb_input_state(0, RETRO_DEVICE_MOUSE, 0,
				RETRO_DEVICE_ID_MOUSE_X);
	int dy = cb_input_state(0, RETRO_DEVICE_MOUSE, 0,
				RETRO_DEVICE_ID_MOUSE_Y);
	bool btn[INPUT_BUTTON__MAX] = {
		[INPUT_BUTTON_LEFT] = cb_input_state(
			0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT),
		[INPUT_BUTTON_RIGHT] = cb_input_state(
			0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT),
		[INPUT_BUTTON_MIDDLE] = cb_input_state(
			0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE),
	};
	// The frontend reports wheel motion as a momentary button; count
	// detents so none are lost or repeated across injection ticks.
	int wheel = 0;
	if (cb_input_state(0, RETRO_DEVICE_MOUSE, 0,
			   RETRO_DEVICE_ID_MOUSE_WHEELUP)) {
		wheel++;
	}
	if (cb_input_state(0, RETRO_DEVICE_MOUSE, 0,
			   RETRO_DEVICE_ID_MOUSE_WHEELDOWN)) {
		wheel--;
	}

	bool key_down_now[RETROK_LAST];
	for (unsigned k = 1; k < RETROK_LAST; k++) {
		if (key_map[k]) {
			key_down_now[k] = cb_input_state(
				0, RETRO_DEVICE_KEYBOARD, 0, k);
		}
	}

	pthread_mutex_lock(&input_mutex);
	// Deltas accumulate: the emu thread consumes them at its own rate.
	mouse_dx += scale_mouse(dx, &mouse_rem_x);
	mouse_dy += scale_mouse(dy, &mouse_rem_y);
	memcpy(buttons_down, btn, sizeof(buttons_down));
	wheel_accum += wheel;

	// Emit key transitions in three passes — modifier presses first,
	// then everything else, then modifier releases — so a shift+letter
	// combination first seen in the same poll reaches the guest in an
	// order that produces the shifted character.
	for (int pass = 0; pass < 3; pass++) {
		for (unsigned k = 1; k < RETROK_LAST; k++) {
			if (!key_map[k] || key_down_now[k] == key_down_prev[k]) {
				continue;
			}
			bool mod = key_is_modifier(k);
			if ((pass == 0 && !(mod && key_down_now[k])) ||
			    (pass == 1 && mod) ||
			    (pass == 2 && !(mod && !key_down_now[k]))) {
				continue;
			}
			enqueue_key_locked(key_down_now[k], key_map[k]);
			key_down_prev[k] = key_down_now[k];
		}
	}
	pthread_mutex_unlock(&input_mutex);

	// input_bh is only created once QEMU is up (display_init), so a
	// non-NULL pointer means the main loop exists to run it.
	if (input_bh && !exited) {
		CALL_QEMU_FUNC(qemu_bh_schedule, input_bh);
	}

	flush_pending_messages();

	if (exited) {
		join_emu_thread();
		cb_env(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
		return;
	}

	// Snapshot av_info and acquire its corresponding frame before releasing
	// av_info_lock. The 3D producer publishes AV -> frame in the same lock
	// order, so an oversized frame cannot outrun the frontend envelope.
	// Deliver callbacks outside both locks:
	// SET_SYSTEM_AV_INFO can re-init the frontend's drivers, and
	// publishers must stay free to update av_info meanwhile.
	struct retro_system_av_info av_local;
	bool geom_changed, timing_changed, fresh;
	pthread_mutex_lock(&av_info_lock);
	geom_changed = changed_geometry;
	timing_changed = changed_timing;
	changed_geometry = false;
	changed_timing = false;
	av_local = av_info;
	pthread_mutex_lock(&frame_mutex);
	fresh = frame_acquire_locked();
	pthread_mutex_unlock(&frame_mutex);
	pthread_mutex_unlock(&av_info_lock);
	bool env_grows = av_local.geometry.base_width > delivered_env_w ||
			 av_local.geometry.base_height > delivered_env_h;
	if (timing_changed || (geom_changed && env_grows)) {
		// Full driver reinit on RetroArch 1.7.5 — acceptable only
		// on the rare paths that need it: a sample-rate change
		// (at most once, when the guest's audio driver first opens
		// the voice at a non-default rate) or a mode that outgrew
		// everything delivered this session, where the frontend's
		// texture may be too small and SET_GEOMETRY wouldn't grow
		// it (see av_info comment).
		cb_env(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_local);
		// The reinit re-sizes the frontend's texture from the
		// geometry delivered with it, so the envelope resets to
		// that geometry rather than accumulating a sticky maximum
		// (a frontend sizing exactly from base would otherwise be
		// credited with room it no longer has).
		delivered_env_w = av_local.geometry.base_width;
		delivered_env_h = av_local.geometry.base_height;
	} else if (geom_changed) {
		cb_env(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_local.geometry);
	}

	// Present the latest frame QEMU published; NULL means "same frame as
	// last time" and lets the frontend skip the upload. present_frame is
	// owned by this thread, so no lock is held across cb_video_refresh —
	// publishers run under the BQL and must never wait on the frontend's
	// upload/vsync.
	frame_finish_acquire(fresh);
	if (present_valid) {
		cb_video_refresh(fresh ? present_frame.buf : NULL,
				 present_frame.w, present_frame.h,
				 (size_t)present_frame.w * 4);
	} else {
		// Nothing rendered yet (or the config has no libretro
		// display); present a duped blank frame at the advertised
		// geometry instead of blocking.
		cb_video_refresh(NULL, av_local.geometry.base_width,
				 av_local.geometry.base_height,
				 (size_t)av_local.geometry.base_width * 4);
	}
}
