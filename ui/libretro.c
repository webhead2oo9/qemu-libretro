#include <libretro.h>

#include <stdio.h>
#include <string.h>
#include <libgen.h>
#include <pthread.h>
#include <glib/gstdio.h>

#include "retro-gen.h"
#include "qemu/datadir.h"
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qemu-main.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "ui/console.h"
#include "ui/kbd-state.h"
#include "audio/audio.h"
#include "audio/audio_int.h"

#define QEMU_CMD_PREFIX "qemu-system-"
#define DEFAULT_ARCH "x86_64"
#define QEMU_CMD                                                               \
	(QEMU_CMD_PREFIX DEFAULT_ARCH), "-libretro", "-audiodev",              \
		"libretro,id=snd0", "-machine", "pcspk-audiodev=snd0",         \
		"-device", "AC97,audiodev=snd0"

void rcu_init(void);

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

// Input. Written by the frontend thread (retro_run / keyboard callback),
// consumed by the emu thread (refresh); the two run concurrently, so all
// access goes through input_mutex. Mouse deltas accumulate because the two
// sides tick at independent rates.
#define KEY_EVENT_QUEUE_LEN 64
struct key_event {
	bool down;
	QKeyCode key;
};
static struct key_event key_event_queue[KEY_EVENT_QUEUE_LEN];
static size_t num_pending_keys = 0;
static int mouse_dx, mouse_dy;
static bool buttons_down[INPUT_BUTTON__MAX];
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;

// Video. QEMU runs free; the emu thread copies each finished frame here and
// retro_run presents the latest one. This replaces an older design that ran
// the two threads in lockstep, which throttled the guest to the frontend's
// frame rate and stalled I/O handling (the emu thread used to park holding
// the BQL between frames).
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *frame_buf;
static int frame_w, frame_h;
static bool frame_valid = false;   // a frame has been copied at least once
static bool frame_updated = false; // new content since the last present
// While qemu-3dfx pass-through presents (Win98 Glide/GL games), frames
// come from the readback glue instead of the VGA surface; refresh()
// stands down so the stale surface cannot overwrite them. Written and
// read under the BQL only.
static bool fx_active;

// Notifications. QEMU reports errors from arbitrary threads, but libretro
// environment calls are only safe from the frontend thread, so messages
// queue here and retro_run delivers them.
#define PENDING_MSGS_LEN 8
static pthread_mutex_t msg_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct pending_msg {
	char *msg;
	unsigned duration;
	enum retro_log_level level;
} pending_msgs[PENDING_MSGS_LEN];
static size_t num_pending_msgs = 0;

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
	info->valid_extensions = "qemu_cmd_line|iso|img|qcow|qcow2";
	info->library_version = "0.1.0";
	info->library_name = "qemu";
}

static pthread_mutex_t av_info_lock = PTHREAD_MUTEX_INITIALIZER;
static bool changed_av_info = false;
static struct retro_system_av_info av_info = {
	.geometry = {
		.base_width = 100,
		.base_height = 100,
		.max_width = 100,
		.max_height = 100,
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
	if (keycode >= RETROK_LAST) {
		return;
	}
	QKeyCode qc = key_map[keycode];
	if (!qc) {
		return;
	}

	pthread_mutex_lock(&input_mutex);
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
	pthread_mutex_unlock(&input_mutex);
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
	// The frontend consumes S16 stereo only; fail the voice rather than
	// abort the process on other configurations.
	if (as->fmt != AUDIO_FORMAT_S16 || as->nchannels != 2) {
		return -1;
	}

	pthread_mutex_lock(&av_info_lock);
	av_info.timing.sample_rate = as->freq;
	changed_av_info = true;
	pthread_mutex_unlock(&av_info_lock);

	CALL_QEMU_FUNC(audio_pcm_init_info, &hw->info, as);

	hw->samples = 1024;

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
	pthread_mutex_unlock(&hw_voice_out_mutex);
	return ret;
}

static void gfx_update(DisplayChangeListener *dcl, int x, int y, int w, int h)
{
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
		av_info.geometry.base_width = av_info.geometry.max_width = w;
		av_info.geometry.base_height = av_info.geometry.max_height = h;
		changed_av_info = true;
		pthread_mutex_unlock(&av_info_lock);
	}
	surface = new_surface;
}

static bool gfx_check_format(DisplayChangeListener *dcl,
			     pixman_format_code_t format)
{
	return format == PIXMAN_x8r8g8b8;
}

static void refresh(DisplayChangeListener *dcl)
{
	CALL_QEMU_FUNC(graphic_hw_update, dcl->con);

	// Publish the frame: copy it out so the frontend can present it
	// while QEMU keeps running. This runs on the emu thread under the
	// BQL, so the copy must be the only thing the frontend can wait on.
	if (surface && !fx_active) {
		int w = surface_width(surface);
		int h = surface_height(surface);
		int stride = surface_stride(surface);
		uint8_t *src = surface_data(surface);

		pthread_mutex_lock(&frame_mutex);
		if (!frame_buf || frame_w != w || frame_h != h) {
			frame_buf = g_realloc(frame_buf, (size_t)w * h * 4);
			frame_w = w;
			frame_h = h;
		}
		if (stride == w * 4) {
			memcpy(frame_buf, src, (size_t)w * h * 4);
		} else {
			for (int y = 0; y < h; y++) {
				memcpy(frame_buf + (size_t)y * w * 4,
				       src + (size_t)y * stride, w * 4);
			}
		}
		frame_valid = true;
		frame_updated = true;
		pthread_mutex_unlock(&frame_mutex);
	}

	// Inject input collected by the frontend since the last refresh.
	pthread_mutex_lock(&input_mutex);

	for (size_t i = 0; i < num_pending_keys; i++) {
		CALL_QEMU_FUNC(qkbd_state_key_event, kbd,
			       key_event_queue[i].key, key_event_queue[i].down);
	}
	num_pending_keys = 0;

	CALL_QEMU_FUNC(qemu_input_queue_rel, dcl->con, INPUT_AXIS_X, mouse_dx);
	CALL_QEMU_FUNC(qemu_input_queue_rel, dcl->con, INPUT_AXIS_Y, mouse_dy);
	mouse_dx = 0;
	mouse_dy = 0;

	for (size_t i = 0; i < INPUT_BUTTON__MAX; i++) {
		CALL_QEMU_FUNC(qemu_input_queue_btn, dcl->con, i,
			       buttons_down[i]);
	}

	pthread_mutex_unlock(&input_mutex);

	CALL_QEMU_FUNC(qemu_input_event_sync);
}

#ifdef CONFIG_QEMU_3DFX
// qemu-3dfx frame sink (see ui/libretro-3dfx.c). publish arrives from a
// vCPU thread under the BQL with the finished 3D frame; rows come
// bottom-up from glReadPixels and BGRA bytes already match XRGB8888.
static void fx_sink_publish(const void *pixels, int w, int h)
{
	const uint8_t *src = pixels;

	pthread_mutex_lock(&frame_mutex);
	if (!frame_buf || frame_w != w || frame_h != h) {
		frame_buf = g_realloc(frame_buf, (size_t)w * h * 4);
		frame_w = w;
		frame_h = h;
	}
	for (int y = 0; y < h; y++) {
		memcpy(frame_buf + (size_t)y * w * 4,
		       src + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
	}
	frame_valid = true;
	frame_updated = true;
	pthread_mutex_unlock(&frame_mutex);

	pthread_mutex_lock(&av_info_lock);
	if (av_info.geometry.base_width != w ||
	    av_info.geometry.base_height != h) {
		av_info.geometry.base_width = av_info.geometry.max_width = w;
		av_info.geometry.base_height = av_info.geometry.max_height = h;
		changed_av_info = true;
	}
	pthread_mutex_unlock(&av_info_lock);
}

static void fx_sink_set_active(bool on)
{
	fx_active = on;

	// Back to 2D: snap the geometry to the VGA surface so the next
	// published frame is not presented at the 3D resolution.
	if (!on && surface) {
		int w = surface_width(surface);
		int h = surface_height(surface);

		pthread_mutex_lock(&av_info_lock);
		if (av_info.geometry.base_width != w ||
		    av_info.geometry.base_height != h) {
			av_info.geometry.base_width =
				av_info.geometry.max_width = w;
			av_info.geometry.base_height =
				av_info.geometry.max_height = h;
			changed_av_info = true;
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

static const QemuFxSink fx_sink = {
	.publish = fx_sink_publish,
	.set_active = fx_sink_set_active,
	.get_guest_dims = fx_sink_guest_dims,
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

static void display_init(DisplayState *ds, DisplayOptions *o)
{
	dcl.con = CALL_QEMU_FUNC(qemu_console_lookup_by_index, 0);
	kbd = CALL_QEMU_FUNC(qkbd_state_init, dcl.con);
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
	if (num_pending_msgs < PENDING_MSGS_LEN) {
		pending_msgs[num_pending_msgs++] = (struct pending_msg){
			.msg = msg,
			.duration = duration,
			.level = level,
		};
		msg = NULL;
	}
	pthread_mutex_unlock(&msg_mutex);

	g_free(msg);

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

	pthread_mutex_lock(&msg_mutex);
	n = num_pending_msgs;
	memcpy(local, pending_msgs, n * sizeof(local[0]));
	num_pending_msgs = 0;
	pthread_mutex_unlock(&msg_mutex);

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

static void *emu_thread_fn(void *arg)
{
	char *game_dir = g_path_get_dirname(game_path);
	g_chdir(game_dir);
	g_free(game_dir);

	if (g_str_has_suffix(game_path, ".qemu_cmd_line")) {
		char *cmd_line = NULL;
		// Must start NULL: GLib only stores into a NULL GError*, and
		// the error paths below dereference it.
		GError *error = NULL;
		bool success =
			g_file_get_contents(game_path, &cmd_line, NULL, &error);
		if (!success) {
			early_error_report("failed reading file '%s':\n%s",
					   game_path, error->message);
			return NULL;
		}
		char **argv;
		success = g_shell_parse_argv(cmd_line, NULL, &argv, &error);
		g_free(cmd_line);
		if (!success) {
			early_error_report("failed parsing file '%s':\n%s",
					   game_path, error->message);
			return NULL;
		}
		if (!argv[0]) {
			early_error_report("empty command in file '%s'",
					   game_path);
			return NULL;
		}
		if (!g_str_has_prefix(argv[0], QEMU_CMD_PREFIX)) {
			early_error_report(
				"command must be of the form " QEMU_CMD_PREFIX
				"ARCH, not '%s': '%s'",
				argv[0], game_path);
			return NULL;
		}
		target_arch = &argv[0][strlen(QEMU_CMD_PREFIX)];
		start_qemu_with_args((const char **)argv);
	} else if (g_str_has_suffix(game_path, ".iso")) {
		target_arch = DEFAULT_ARCH;
		start_qemu_with_args((const char *[]){ QEMU_CMD, "-cdrom",
						       game_path, NULL });
	} else {
		target_arch = DEFAULT_ARCH;
		start_qemu_with_args(
			(const char *[]){ QEMU_CMD, game_path, NULL });
	}
	return NULL;
}

bool retro_load_game(const struct retro_game_info *game)
{
	if (!game) {
		return false;
	}

	// The frontend only guarantees game->path stays valid during this
	// call, and old frontends (RetroArch 1.7.5-era) reuse the
	// string quickly, so copy it before the emu thread reads it.
	game_path = g_strdup(game->path);
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

void retro_run(void)
{
	cb_input_poll();

	pthread_mutex_lock(&input_mutex);
	// Deltas accumulate: the emu thread consumes them at its own rate.
	mouse_dx += cb_input_state(0, RETRO_DEVICE_MOUSE, 0,
				   RETRO_DEVICE_ID_MOUSE_X);
	mouse_dy += cb_input_state(0, RETRO_DEVICE_MOUSE, 0,
				   RETRO_DEVICE_ID_MOUSE_Y);

#define BTN(q, r)                                                              \
	buttons_down[INPUT_BUTTON_##q] = cb_input_state(                       \
		0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_##r)
	BTN(LEFT, LEFT);
	BTN(RIGHT, RIGHT);
	BTN(MIDDLE, MIDDLE);
	BTN(WHEEL_UP, WHEELUP);
	BTN(WHEEL_DOWN, WHEELDOWN);
#undef BTN
	pthread_mutex_unlock(&input_mutex);

	flush_pending_messages();

	if (exited) {
		join_emu_thread();
		cb_env(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
		return;
	}

	pthread_mutex_lock(&av_info_lock);
	if (changed_av_info) {
		changed_av_info = false;
		cb_env(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
	}
	pthread_mutex_unlock(&av_info_lock);

	// Present the latest frame QEMU published; NULL means "same frame as
	// last time" and lets the frontend skip the upload.
	pthread_mutex_lock(&frame_mutex);
	if (frame_valid) {
		cb_video_refresh(frame_updated ? frame_buf : NULL,
				 frame_w, frame_h, (size_t)frame_w * 4);
		frame_updated = false;
	} else {
		// Nothing rendered yet (or the config has no libretro
		// display); present a duped blank frame at the advertised
		// geometry instead of blocking.
		cb_video_refresh(NULL, av_info.geometry.base_width,
				 av_info.geometry.base_height,
				 (size_t)av_info.geometry.base_width * 4);
	}
	pthread_mutex_unlock(&frame_mutex);
}
