/*
 * retro_host.c — minimal libretro host for driving qemu_libretro.dll
 * without RetroArch. Used to reproduce/verify:
 *   1. QMP connect+disconnect hot-poll (CPU burn)
 *   2. teardown AV after QMP quit → retro_unload_game → FreeLibrary
 *
 * Usage: retro_host.exe <core.dll> <content> <qmp_port> [--no-qmp] [--no-quit]
 *          [--opt key=value]... [--flip key=value]...
 *          [--disk-swap N | --disk-append PATH | --disk-remove N]...
 *          [--disk-swap-unload N]
 *          [--frame-profile START_EVENT STOP_EVENT]
 *   --opt  seeds a core-option value served via GET_VARIABLE
 *   --flip changes one mid-run (after baseline) and raises
 *          GET_VARIABLE_UPDATE; flips apply in order, ~1.5s apart
 *   --disk-* drive the disk-control interface with RetroArch 1.7.5's
 *          exact call sequences, in order, after the baseline; each is
 *          followed by a QMP query-block dump. --disk-swap-unload
 *          commits a swap and unloads immediately (pending-BH test).
 *
 * Phases (markers on stdout):
 *   BASELINE_CPU <percent-of-one-core>
 *   AFTER_QMP_CPU <percent-of-one-core>
 *   TEARDOWN_OK            (process survived unload+FreeLibrary)
 * Exit codes: 0 ok, 2 usage/setup error, 3 crash (exception filter).
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include <libretro.h>

static char system_dir[MAX_PATH] = ".";

static volatile bool g_shutdown;
static bool g_gamepad;
static bool g_joycpl;
static bool g_rst_close;
static bool g_instant_close;
static bool g_events_close;
static int g_qmp_cycles = 1;
static volatile long g_frames;

#define MAX_FRAME_PROFILE_INTERVALS 65536
static bool g_frame_profile_enabled;
static bool g_frame_profile_started;
static bool g_frame_profile_stopped;
static HANDLE g_frame_profile_start_event;
static HANDLE g_frame_profile_stop_event;
static LARGE_INTEGER g_frame_profile_frequency;
static LARGE_INTEGER g_frame_profile_start;
static LARGE_INTEGER g_frame_profile_stop;
static LARGE_INTEGER g_frame_profile_last_fresh;
static unsigned long g_frame_profile_callbacks;
static unsigned long g_frame_profile_fresh;
static unsigned long g_frame_profile_duped;
static unsigned long g_frame_profile_interval_overflow;
static LONGLONG g_frame_profile_intervals[MAX_FRAME_PROFILE_INTERVALS];
static size_t g_frame_profile_interval_count;

/* core options served via GET_VARIABLE */
#define MAX_OPTS 32
static struct opt { char key[64]; char val[128]; } g_opts[MAX_OPTS];
static int g_nopts;
static struct opt g_flips[MAX_OPTS];
static int g_nflips;
static volatile bool g_vars_dirty;
static int g_boot_wait; /* --wait: extra ms of pumping after startup */

static bool set_opt(const char *kv)
{
    const char *eq = strchr(kv, '=');
    if (!eq || eq == kv || strlen(eq + 1) >= sizeof(g_opts[0].val) ||
        (size_t)(eq - kv) >= sizeof(g_opts[0].key)) {
        return false;
    }
    for (int i = 0; i < g_nopts; i++) {
        if (!strncmp(g_opts[i].key, kv, eq - kv) &&
            g_opts[i].key[eq - kv] == 0) {
            strcpy(g_opts[i].val, eq + 1);
            return true;
        }
    }
    if (g_nopts == MAX_OPTS) {
        return false;
    }
    memcpy(g_opts[g_nopts].key, kv, eq - kv);
    g_opts[g_nopts].key[eq - kv] = 0;
    strcpy(g_opts[g_nopts].val, eq + 1);
    g_nopts++;
    return true;
}
static struct retro_audio_callback g_audio_cb;
static HANDLE g_audio_thread;
static volatile bool g_audio_stop;

/* disk-control interface captured from the env call */
static struct retro_disk_control_callback g_disk;
static bool g_have_disk;

#define MAX_ACTS 16
enum act_kind { ACT_SWAP, ACT_APPEND, ACT_REMOVE, ACT_SWAP_UNLOAD };
static struct act {
    enum act_kind kind;
    int idx;
    char path[MAX_PATH];
} g_acts[MAX_ACTS];
static int g_nacts;
static bool g_unload_now; /* set by ACT_SWAP_UNLOAD: skip graceful quit */

/* ---- libretro callbacks ---- */

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = system_dir;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        const struct retro_game_geometry *g = data;
        fprintf(stderr, "[env] SET_GEOMETRY %ux%u (max %ux%u)\n",
                g->base_width, g->base_height, g->max_width, g->max_height);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        const struct retro_system_av_info *a = data;
        fprintf(stderr, "[env] SET_SYSTEM_AV_INFO %ux%u (max %ux%u) "
                "%.1f fps %.0f Hz  ** DRIVER REINIT **\n",
                a->geometry.base_width, a->geometry.base_height,
                a->geometry.max_width, a->geometry.max_height,
                a->timing.fps, a->timing.sample_rate);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK:
        g_audio_cb = *(struct retro_audio_callback *)data;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        const struct retro_variable *v = data;
        for (; v->key; v++) {
            fprintf(stderr, "[vars] %s = \"%s\"\n", v->key, v->value);
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        v->value = NULL;
        for (int i = 0; i < g_nopts; i++) {
            if (!strcmp(g_opts[i].key, v->key)) {
                v->value = g_opts[i].val;
                return true;
            }
        }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = g_vars_dirty;
        g_vars_dirty = false;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
        const struct retro_message_ext *m = data;
        fprintf(stderr, "[core msg] %s\n", m->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const struct retro_message *m = data;
        fprintf(stderr, "[core msg] %s\n", m->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        if (!data) {
            /* RetroArch 1.7.5 dereferences data without a NULL check,
             * so a NULL here is a core bug that would crash EmuVR */
            fprintf(stderr, "[disk] BUG: core passed NULL interface\n");
            g_have_disk = false;
            return true;
        }
        g_disk = *(const struct retro_disk_control_callback *)data;
        g_have_disk = true;
        fprintf(stderr, "[disk] interface registered\n");
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        g_shutdown = true;
        return true;
    default:
        return false;
    }
}

/* --dump: write one BMP per distinct resolution, ~90 fresh frames after
 * the mode settles (lets the guest actually paint the desktop). XRGB8888
 * little-endian bytes are exactly BMP 32bpp BGRX, so rows copy verbatim;
 * negative biHeight = top-down. */
static bool g_dump;
static bool g_dump_all;
static unsigned long g_dump_every = 1;
static unsigned long g_dump_callback;
static unsigned long g_dump_sequence;
static struct { unsigned w, h; int count; } g_dump_st;

static void frame_profile_callback(const void *data)
{
    LARGE_INTEGER now;

    if (!g_frame_profile_enabled) {
        return;
    }
    QueryPerformanceCounter(&now);
    if (WaitForSingleObject(g_frame_profile_start_event, 0) ==
        WAIT_OBJECT_0) {
        g_frame_profile_started = true;
        g_frame_profile_stopped = false;
        g_frame_profile_start = now;
        g_frame_profile_stop.QuadPart = 0;
        g_frame_profile_last_fresh.QuadPart = 0;
        g_frame_profile_callbacks = 0;
        g_frame_profile_fresh = 0;
        g_frame_profile_duped = 0;
        g_frame_profile_interval_count = 0;
        g_frame_profile_interval_overflow = 0;
    }
    if (!g_frame_profile_started || g_frame_profile_stopped) {
        return;
    }
    if (WaitForSingleObject(g_frame_profile_stop_event, 0) ==
        WAIT_OBJECT_0) {
        g_frame_profile_stopped = true;
        g_frame_profile_stop = now;
        return;
    }

    g_frame_profile_callbacks++;
    if (!data) {
        g_frame_profile_duped++;
        return;
    }

    g_frame_profile_fresh++;
    if (g_frame_profile_last_fresh.QuadPart) {
        LONGLONG interval =
            now.QuadPart - g_frame_profile_last_fresh.QuadPart;
        if (g_frame_profile_interval_count <
            MAX_FRAME_PROFILE_INTERVALS) {
            g_frame_profile_intervals[g_frame_profile_interval_count++] =
                interval;
        } else {
            g_frame_profile_interval_overflow++;
        }
    }
    g_frame_profile_last_fresh = now;
}

static void dump_bmp(const void *data, unsigned w, unsigned h, size_t pitch,
                     unsigned long sequence)
{
    char name[64];
    if (sequence) {
        snprintf(name, sizeof(name), "frame_%ux%u_%06lu.bmp",
                 w, h, sequence);
    } else {
        snprintf(name, sizeof(name), "frame_%ux%u.bmp", w, h);
    }
    FILE *f = fopen(name, "wb");
    if (!f) {
        return;
    }
    unsigned img = w * h * 4;
    unsigned char fh[14] = { 'B', 'M' };
    unsigned char ih[40] = { 40 };
    *(unsigned *)(fh + 2) = 14 + 40 + img;
    *(unsigned *)(fh + 10) = 14 + 40;
    *(int *)(ih + 4) = (int)w;
    *(int *)(ih + 8) = -(int)h;
    *(short *)(ih + 12) = 1;
    *(short *)(ih + 14) = 32;
    *(unsigned *)(ih + 20) = img;
    fwrite(fh, 1, 14, f);
    fwrite(ih, 1, 40, f);
    for (unsigned y = 0; y < h; y++) {
        fwrite((const char *)data + y * pitch, 1, w * 4, f);
    }
    fclose(f);
    fprintf(stderr, "[dump] %s\n", name);
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    InterlockedIncrement(&g_frames);
    frame_profile_callback(data);
    if (!g_dump || !data) {
        return;
    }
    if (w != g_dump_st.w || h != g_dump_st.h) {
        g_dump_st.w = w;
        g_dump_st.h = h;
        g_dump_st.count = 0;
    }
    if (g_dump_all) {
        if (g_dump_callback++ % g_dump_every) {
            return;
        }
        dump_bmp(data, w, h, pitch, ++g_dump_sequence);
        return;
    }
    /* re-dump every 300 fresh frames so an interactive session's latest
     * state is visible; same-size dumps overwrite */
    if (++g_dump_st.count % 300 == 30) {
        dump_bmp(data, w, h, pitch, 0);
    }
}

static int compare_frame_intervals(const void *left, const void *right)
{
    LONGLONG a = *(const LONGLONG *)left;
    LONGLONG b = *(const LONGLONG *)right;
    return (a > b) - (a < b);
}

static unsigned long long frame_profile_microseconds(LONGLONG ticks)
{
    return (unsigned long long)(ticks * 1000000ULL /
                                g_frame_profile_frequency.QuadPart);
}

static unsigned long long frame_profile_percentile(unsigned percentile)
{
    if (!g_frame_profile_interval_count) {
        return 0;
    }
    size_t index =
        (g_frame_profile_interval_count * percentile + 99) / 100 - 1;
    return frame_profile_microseconds(g_frame_profile_intervals[index]);
}

static void frame_profile_report(void)
{
    unsigned long stalls_50ms = 0;
    unsigned long stalls_100ms = 0;
    unsigned long stalls_250ms = 0;
    unsigned long stalls_1000ms = 0;
    unsigned long long duration_us;
    unsigned long long fresh_fps_milli;
    unsigned long long callback_fps_milli;

    if (!g_frame_profile_enabled) {
        return;
    }
    if (g_frame_profile_started && !g_frame_profile_stopped) {
        QueryPerformanceCounter(&g_frame_profile_stop);
        g_frame_profile_stopped = true;
    }
    if (!g_frame_profile_started ||
        g_frame_profile_stop.QuadPart <= g_frame_profile_start.QuadPart) {
        fprintf(stderr, "FrameProfile unavailable started=%d stopped=%d\n",
                g_frame_profile_started, g_frame_profile_stopped);
        return;
    }

    qsort(g_frame_profile_intervals, g_frame_profile_interval_count,
          sizeof(g_frame_profile_intervals[0]), compare_frame_intervals);
    for (size_t i = 0; i < g_frame_profile_interval_count; i++) {
        unsigned long long interval_us =
            frame_profile_microseconds(g_frame_profile_intervals[i]);
        if (interval_us >= 50000) stalls_50ms++;
        if (interval_us >= 100000) stalls_100ms++;
        if (interval_us >= 250000) stalls_250ms++;
        if (interval_us >= 1000000) stalls_1000ms++;
    }

    duration_us = frame_profile_microseconds(
        g_frame_profile_stop.QuadPart - g_frame_profile_start.QuadPart);
    fresh_fps_milli = duration_us ?
        (unsigned long long)g_frame_profile_fresh * 1000000000ULL /
            duration_us : 0;
    callback_fps_milli = duration_us ?
        (unsigned long long)g_frame_profile_callbacks * 1000000000ULL /
            duration_us : 0;
    fprintf(stderr,
            "FrameProfile durationUs=%llu callbacks=%lu fresh=%lu "
            "duped=%lu freshFpsMilli=%llu callbackFpsMilli=%llu "
            "intervals=%llu overflow=%lu p50Us=%llu p95Us=%llu "
            "p99Us=%llu maxUs=%llu stalls50ms=%lu stalls100ms=%lu "
            "stalls250ms=%lu stalls1000ms=%lu\n",
            duration_us, g_frame_profile_callbacks, g_frame_profile_fresh,
            g_frame_profile_duped, fresh_fps_milli, callback_fps_milli,
            (unsigned long long)g_frame_profile_interval_count,
            g_frame_profile_interval_overflow,
            frame_profile_percentile(50), frame_profile_percentile(95),
            frame_profile_percentile(99), frame_profile_percentile(100),
            stalls_50ms, stalls_100ms, stalls_250ms, stalls_1000ms);
}

static void audio_sample_cb(int16_t l, int16_t r) { (void)l; (void)r; }

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    (void)data;
    return frames; /* swallow everything */
}

static void input_poll_cb(void) {}

static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx,
                              unsigned id)
{
    if (!g_gamepad || port != 0) {
        return 0;
    }
    unsigned phase = (GetTickCount() / 1000) % 8;
    if (dev == RETRO_DEVICE_JOYPAD) {
        if (id == RETRO_DEVICE_ID_JOYPAD_B) {
            return phase & 1;
        }
        if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT) {
            return phase >= 2 && phase <= 4;
        }
    }
    if (dev == RETRO_DEVICE_ANALOG && idx == RETRO_DEVICE_INDEX_ANALOG_LEFT &&
        id == RETRO_DEVICE_ID_ANALOG_X) {
        return phase < 4 ? 24000 : -24000;
    }
    return 0;
}

static DWORD WINAPI audio_thread_fn(LPVOID arg)
{
    (void)arg;
    while (!g_audio_stop) {
        if (g_audio_cb.callback) {
            g_audio_cb.callback(); /* core blocks ~20ms when silent */
        } else {
            Sleep(10);
        }
    }
    return 0;
}

/* ---- helpers ---- */

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    fprintf(stderr, "FATAL: exception 0x%08lx at %p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    fflush(stderr);
    ExitProcess(3);
}

static double cpu_seconds(void)
{
    FILETIME c, e, k, u;
    ULARGE_INTEGER ku, uu;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ku.LowPart = k.dwLowDateTime; ku.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
    return (ku.QuadPart + uu.QuadPart) / 1e7;
}

/* count live threads in this process; if mod != NULL, also count how
 * many have their Win32 start address inside that module's image */
static void count_threads(HMODULE mod, int *total, int *in_module)
{
    typedef LONG(WINAPI *NtQIT)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static NtQIT qit;
    MODULEINFO mi = { 0 };
    THREADENTRY32 te = { .dwSize = sizeof(te) };
    HANDLE snap;

    *total = 0;
    *in_module = 0;
    if (!qit) {
        qit = (NtQIT)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                            "NtQueryInformationThread");
    }
    if (mod) {
        GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi));
    }
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    for (BOOL ok = Thread32First(snap, &te); ok;
         ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) {
            continue;
        }
        (*total)++;
        if (mi.lpBaseOfDll && qit) {
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE,
                                  te.th32ThreadID);
            PVOID start = NULL;
            if (h) {
                /* 9 = ThreadQuerySetWin32StartAddress */
                if (qit(h, 9, &start, sizeof(start), NULL) == 0 &&
                    (char *)start >= (char *)mi.lpBaseOfDll &&
                    (char *)start < (char *)mi.lpBaseOfDll + mi.SizeOfImage) {
                    (*in_module)++;
                }
                CloseHandle(h);
            }
        }
    }
    CloseHandle(snap);
}

typedef void (*retro_run_t)(void);
static retro_run_t g_retro_run;

/* pump retro_run for ms milliseconds */
static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end && !g_shutdown) {
        g_retro_run();
        Sleep(15);
    }
}

/* pump and measure process CPU over the window, as % of one core */
static double pump_measure(DWORD ms)
{
    double c0 = cpu_seconds();
    DWORD t0 = GetTickCount();
    pump(ms);
    double dc = cpu_seconds() - c0;
    DWORD dt = GetTickCount() - t0;
    return dt ? (100.0 * dc / (dt / 1000.0)) : 0.0;
}

/* ---- disk-control driving (RetroArch 1.7.5's exact sequences) ---- */

static void disk_report(void)
{
    if (!g_have_disk) {
        return;
    }
    fprintf(stderr, "[disk] num=%u index=%u ejected=%d\n",
            g_disk.get_num_images(), g_disk.get_image_index(),
            g_disk.get_eject_state());
}

/* menu flow: Cycle Tray Status -> Disk Index -> Cycle Tray Status */
static void disk_swap(int idx)
{
    fprintf(stderr, "[disk] swap -> %d\n", idx);
    g_disk.set_eject_state(true);
    pump(200);
    if (!g_disk.set_image_index((unsigned)idx)) {
        fprintf(stderr, "[disk] set_image_index refused\n");
    }
    g_disk.set_eject_state(false);
    pump(800);
    disk_report();
}

/* command_event_disk_control_append_image order, verbatim */
static void disk_append(const char *path)
{
    fprintf(stderr, "[disk] append %s\n", path);
    g_disk.set_eject_state(true);
    g_disk.add_image_index();
    unsigned n = g_disk.get_num_images();
    struct retro_game_info gi = { .path = path };
    if (!n || !g_disk.replace_image_index(n - 1, &gi)) {
        fprintf(stderr, "[disk] append failed\n");
    }
    g_disk.set_image_index(n - 1);
    g_disk.set_eject_state(false);
    pump(800);
    disk_report();
}

/* replace_image_index(idx, NULL) while the tray is open */
static void disk_remove(int idx)
{
    fprintf(stderr, "[disk] remove %d\n", idx);
    g_disk.set_eject_state(true);
    if (!g_disk.replace_image_index((unsigned)idx, NULL)) {
        fprintf(stderr, "[disk] remove refused\n");
    }
    g_disk.set_eject_state(false);
    pump(800);
    disk_report();
}

/* dump query-block so the inserted medium/tray state is assertable */
static void qmp_query_block(int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    static char buf[16384];
    DWORD tmo = 2000;
    int n, off = 0;

    if (s == INVALID_SOCKET) {
        return;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "[query-block] connect failed\n");
        closesocket(s);
        return;
    }
    recv(s, buf, sizeof(buf) - 1, 0); /* greeting */
    const char *caps = "{\"execute\":\"qmp_capabilities\"}\n";
    const char *qb = "{\"execute\":\"query-block\"}\n";
    send(s, caps, (int)strlen(caps), 0);
    recv(s, buf, sizeof(buf) - 1, 0);
    send(s, qb, (int)strlen(qb), 0);
    while (off < (int)sizeof(buf) - 1 &&
           (n = recv(s, buf + off, sizeof(buf) - 1 - off, 0)) > 0) {
        off += n;
        buf[off] = 0;
        if (strchr(buf, '\n')) {
            break; /* one complete response line */
        }
    }
    closesocket(s);
    if (off <= 0) {
        fprintf(stderr, "[query-block] no response\n");
        return;
    }
    buf[off] = 0;
    /* raw dump; the test run greps for "file", "tray_open", "device" */
    fprintf(stderr, "[query-block] %.6000s\n", buf);
}

static void qmp_open_joycpl(int port)
{
    static const char *keys[] = {
        "ctrl-esc", "r", "j", "o", "y", "dot", "c", "p", "l", "ret",
    };
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    char buf[4096], command[256];
    DWORD tmo = 2000;

    if (s == INVALID_SOCKET) {
        return;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "[joycpl] qmp connect failed\n");
        closesocket(s);
        return;
    }
    recv(s, buf, sizeof(buf) - 1, 0);
    const char *caps = "{\"execute\":\"qmp_capabilities\"}\n";
    send(s, caps, (int)strlen(caps), 0);
    recv(s, buf, sizeof(buf) - 1, 0);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        snprintf(command, sizeof(command),
                 "{\"execute\":\"human-monitor-command\",\"arguments\":"
                 "{\"command-line\":\"sendkey %s\"}}\n", keys[i]);
        send(s, command, (int)strlen(command), 0);
        recv(s, buf, sizeof(buf) - 1, 0);
        Sleep(i <= 1 ? 750 : 120);
    }
    closesocket(s);
    fprintf(stderr, "[joycpl] requested Game Controllers panel\n");
}

/* connect to QMP, read greeting, optionally negotiate+quit, close */
static bool qmp_cycle(int port, bool do_quit)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    char buf[4096];
    DWORD tmo = 2000;
    int n;

    if (s == INVALID_SOCKET) {
        return false;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "qmp connect failed: %d\n", WSAGetLastError());
        closesocket(s);
        return false;
    }
    if (g_instant_close) {
        /* vanish before even reading the greeting */
        struct linger lg = { .l_onoff = 1, .l_linger = 0 };
        setsockopt(s, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof(lg));
        closesocket(s);
        return true;
    }
    n = recv(s, buf, sizeof(buf) - 1, 0); /* greeting */
    if (n > 0) {
        buf[n] = 0;
        fprintf(stderr, "[qmp greeting] %.120s...\n", buf);
    }
    if (g_events_close) {
        /* queue responses + STOP/RESUME events, never read any of it,
         * then RST — output pending on a dead connection */
        const char *caps = "{\"execute\":\"qmp_capabilities\"}\n";
        const char *stop = "{\"execute\":\"stop\"}\n";
        const char *cont = "{\"execute\":\"cont\"}\n";
        struct linger lg = { .l_onoff = 1, .l_linger = 0 };
        send(s, caps, (int)strlen(caps), 0);
        send(s, stop, (int)strlen(stop), 0);
        send(s, cont, (int)strlen(cont), 0);
        Sleep(300); /* let the commands execute, keep rx buffer full */
        setsockopt(s, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof(lg));
        closesocket(s);
        return true;
    }
    {
        const char *caps = "{\"execute\":\"qmp_capabilities\"}\n";
        const char *stat = "{\"execute\":\"query-status\"}\n";
        const char *mem  = "{\"execute\":\"query-memory-size-summary\"}\n";
        const char *hmp  = "{\"execute\":\"human-monitor-command\","
                           "\"arguments\":{\"command-line\":"
                           "\"info registers\"}}\n";
        const char *aud  = "{\"execute\":\"human-monitor-command\","
                           "\"arguments\":{\"command-line\":"
                           "\"info audiodev\"}}\n";
        const char *usb  = "{\"execute\":\"human-monitor-command\","
                           "\"arguments\":{\"command-line\":"
                           "\"info usb\"}}\n";
        send(s, caps, (int)strlen(caps), 0);
        recv(s, buf, sizeof(buf) - 1, 0);
        send(s, stat, (int)strlen(stat), 0);
        recv(s, buf, sizeof(buf) - 1, 0);
        send(s, mem, (int)strlen(mem), 0);
        n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            fprintf(stderr, "[qmp mem] %.200s\n", buf);
        }
        send(s, hmp, (int)strlen(hmp), 0);
        recv(s, buf, sizeof(buf) - 1, 0);
        send(s, aud, (int)strlen(aud), 0);
        n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            fprintf(stderr, "[qmp audiodev] %.400s\n", buf);
        }
        send(s, usb, (int)strlen(usb), 0);
        n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            fprintf(stderr, "[qmp usb] %.1000s\n", buf);
        }
    }
    if (do_quit) {
        /* generate disk I/O right before quitting so block thread-pool
         * workers (10s idle timeout) are alive across the teardown */
        const char *io = "{\"execute\":\"human-monitor-command\","
                         "\"arguments\":{\"command-line\":"
                         "\"qemu-io ide0-hd0 \\\"write 0 1M\\\"\"}}\n";
        const char *quit = "{\"execute\":\"quit\"}\n";
        send(s, io, (int)strlen(io), 0);
        n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            fprintf(stderr, "[qemu-io] %.100s\n", buf);
        }
        send(s, quit, (int)strlen(quit), 0);
        recv(s, buf, sizeof(buf) - 1, 0);
    } else if (g_rst_close) {
        /* fire one more command and slam the door mid-response: RST,
         * response undeliverable — mimics a killed qmp.ps1 */
        const char *hmp2 = "{\"execute\":\"human-monitor-command\","
                           "\"arguments\":{\"command-line\":"
                           "\"info qtree\"}}\n";
        struct linger lg = { .l_onoff = 1, .l_linger = 0 };
        send(s, hmp2, (int)strlen(hmp2), 0);
        setsockopt(s, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof(lg));
    }
    closesocket(s);
    return true;
}

int main(int argc, char **argv)
{
    bool do_qmp = true, do_quit = true;
    const char *configured_system_dir;
    const char *frame_profile_start_name = NULL;
    const char *frame_profile_stop_name = NULL;
    WSADATA wsd;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crash_filter);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    configured_system_dir = getenv("RETRO_HOST_SYSTEM_DIR");
    if (configured_system_dir && configured_system_dir[0]) {
        strncpy(system_dir, configured_system_dir, sizeof(system_dir) - 1);
        system_dir[sizeof(system_dir) - 1] = '\0';
    }
    g_dump_all = getenv("RETRO_HOST_DUMP_ALL") != NULL;
    if (getenv("RETRO_HOST_DUMP_EVERY")) {
        unsigned long every = strtoul(getenv("RETRO_HOST_DUMP_EVERY"),
                                      NULL, 10);
        if (every) g_dump_every = every;
    }

    if (argc < 4) {
        fprintf(stderr, "usage: %s <core.dll> <content> <qmp_port> "
                        "[--no-qmp] [--no-quit] [--opt k=v]... "
                        "[--flip k=v]... "
                        "[--frame-profile START_EVENT STOP_EVENT]\n",
                        argv[0]);
        return 2;
    }
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--no-qmp"))  do_qmp = false;
        if (!strcmp(argv[i], "--no-quit")) do_quit = false;
        if (!strcmp(argv[i], "--rst"))     g_rst_close = true;
        if (!strcmp(argv[i], "--instant")) g_instant_close = true;
        if (!strcmp(argv[i], "--events"))  g_events_close = true;
        if (!strcmp(argv[i], "--multi"))   g_qmp_cycles = 5;
        if (!strcmp(argv[i], "--dump"))    g_dump = true;
        if (!strcmp(argv[i], "--gamepad")) g_gamepad = true;
        if (!strcmp(argv[i], "--joycpl"))  g_joycpl = true;
        if (!strcmp(argv[i], "--wait") && i + 1 < argc) {
            g_boot_wait = atoi(argv[++i]);
        }
        if (!strcmp(argv[i], "--frame-profile") && i + 2 < argc) {
            frame_profile_start_name = argv[++i];
            frame_profile_stop_name = argv[++i];
        }
        if (!strcmp(argv[i], "--opt") && i + 1 < argc) {
            if (!set_opt(argv[++i])) {
                fprintf(stderr, "bad --opt '%s'\n", argv[i]);
                return 2;
            }
        }
        if ((!strcmp(argv[i], "--disk-swap") ||
             !strcmp(argv[i], "--disk-remove") ||
             !strcmp(argv[i], "--disk-swap-unload")) && i + 1 < argc) {
            if (g_nacts == MAX_ACTS) {
                fprintf(stderr, "too many --disk-* actions\n");
                return 2;
            }
            g_acts[g_nacts].kind =
                !strcmp(argv[i], "--disk-swap") ? ACT_SWAP :
                !strcmp(argv[i], "--disk-remove") ? ACT_REMOVE :
                                                    ACT_SWAP_UNLOAD;
            g_acts[g_nacts].idx = atoi(argv[i + 1]);
            g_nacts++;
            i++;
        } else if (!strcmp(argv[i], "--disk-append") && i + 1 < argc) {
            if (g_nacts == MAX_ACTS) {
                fprintf(stderr, "too many --disk-* actions\n");
                return 2;
            }
            g_acts[g_nacts].kind = ACT_APPEND;
            snprintf(g_acts[g_nacts].path, sizeof(g_acts[g_nacts].path),
                     "%s", argv[i + 1]);
            g_nacts++;
            i++;
        }
        if (!strcmp(argv[i], "--flip") && i + 1 < argc) {
            const char *kv = argv[++i];
            if (g_nflips == MAX_OPTS || !strchr(kv, '=')) {
                fprintf(stderr, "bad --flip '%s'\n", kv);
                return 2;
            }
            const char *eq = strchr(kv, '=');
            memcpy(g_flips[g_nflips].key, kv, eq - kv);
            g_flips[g_nflips].key[eq - kv] = 0;
            strcpy(g_flips[g_nflips].val, eq + 1);
            g_nflips++;
        }
    }

    if (frame_profile_start_name) {
        g_frame_profile_start_event = OpenEventA(
            SYNCHRONIZE, FALSE, frame_profile_start_name);
        g_frame_profile_stop_event = OpenEventA(
            SYNCHRONIZE, FALSE, frame_profile_stop_name);
        if (!g_frame_profile_start_event || !g_frame_profile_stop_event ||
            !QueryPerformanceFrequency(&g_frame_profile_frequency)) {
            fprintf(stderr, "frame-profile event setup failed: %lu\n",
                    GetLastError());
            return 2;
        }
        g_frame_profile_enabled = true;
    }
    int port = atoi(argv[3]);
    WSAStartup(MAKEWORD(2, 2), &wsd);

    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) {
        fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 2;
    }
    typedef void (*set_env_t)(retro_environment_t);
    typedef void (*set_video_t)(retro_video_refresh_t);
    typedef void (*set_audio_t)(retro_audio_sample_t);
    typedef void (*set_audio_batch_t)(retro_audio_sample_batch_t);
    typedef void (*set_input_poll_t)(retro_input_poll_t);
    typedef void (*set_input_state_t)(retro_input_state_t);
    typedef void (*void_fn_t)(void);
    typedef bool (*load_game_t)(const struct retro_game_info *);
    typedef void (*get_av_info_t)(struct retro_system_av_info *);

#define RESOLVE(name, type) \
    type p_##name = (type)(void *)GetProcAddress(dll, #name); \
    if (!p_##name) { fprintf(stderr, "missing export " #name "\n"); return 2; }

    RESOLVE(retro_set_environment, set_env_t);
    RESOLVE(retro_set_video_refresh, set_video_t);
    RESOLVE(retro_set_audio_sample, set_audio_t);
    RESOLVE(retro_set_audio_sample_batch, set_audio_batch_t);
    RESOLVE(retro_set_input_poll, set_input_poll_t);
    RESOLVE(retro_set_input_state, set_input_state_t);
    RESOLVE(retro_init, void_fn_t);
    RESOLVE(retro_deinit, void_fn_t);
    RESOLVE(retro_load_game, load_game_t);
    RESOLVE(retro_unload_game, void_fn_t);
    RESOLVE(retro_run, void_fn_t);
    RESOLVE(retro_get_system_av_info, get_av_info_t);

    g_retro_run = p_retro_run;

    p_retro_set_environment(env_cb);
    p_retro_set_video_refresh(video_cb);
    p_retro_set_audio_sample(audio_sample_cb);
    p_retro_set_audio_sample_batch(audio_batch_cb);
    p_retro_set_input_poll(input_poll_cb);
    p_retro_set_input_state(input_state_cb);
    p_retro_init();

    struct retro_game_info info = { .path = argv[2] };
    if (!p_retro_load_game(&info)) {
        fprintf(stderr, "retro_load_game failed\n");
        return 2;
    }
    fprintf(stderr, "[disk] registered=%d\n", g_have_disk);
    disk_report();

    struct retro_system_av_info av;
    p_retro_get_system_av_info(&av);
    fprintf(stderr, "av: %ux%u @ %.1f fps\n", av.geometry.base_width,
            av.geometry.base_height, av.timing.fps);

    g_audio_thread = CreateThread(NULL, 0, audio_thread_fn, NULL, 0, NULL);

    pump(3000); /* let QEMU come up */
    if (g_boot_wait > 0) {
        pump((DWORD)g_boot_wait); /* e.g. let a guest OS reach the desktop */
    }
    if (g_joycpl) {
        qmp_open_joycpl(port);
        pump(30000);
    }
    printf("BASELINE_CPU %.1f (frames %ld)\n", pump_measure(5000), g_frames);

    for (int i = 0; i < g_nflips && !g_shutdown; i++) {
        char kv[200];
        snprintf(kv, sizeof(kv), "%s=%s", g_flips[i].key, g_flips[i].val);
        fprintf(stderr, "[flip] %s\n", kv);
        set_opt(kv);
        g_vars_dirty = true;
        pump(1500); /* let the core re-read and emit any notice */
    }

    for (int i = 0; i < g_nacts && !g_shutdown; i++) {
        struct act *a = &g_acts[i];
        if (!g_have_disk) {
            fprintf(stderr, "[disk] no interface; skipping actions\n");
            break;
        }
        if (a->kind == ACT_SWAP_UNLOAD) {
            /* commit a swap and unload with the BH possibly pending */
            fprintf(stderr, "[disk] swap %d then immediate unload\n",
                    a->idx);
            g_disk.set_eject_state(true);
            g_disk.set_image_index((unsigned)a->idx);
            g_disk.set_eject_state(false);
            g_unload_now = true;
            break;
        }
        switch (a->kind) {
        case ACT_SWAP:   disk_swap(a->idx);      break;
        case ACT_APPEND: disk_append(a->path);   break;
        case ACT_REMOVE: disk_remove(a->idx);    break;
        default:                                 break;
        }
        if (do_qmp) {
            qmp_query_block(port);
        }
    }

    if (do_qmp && !g_shutdown && !g_unload_now) {
        for (int i = 0; i < g_qmp_cycles; i++) {
            if (!qmp_cycle(port, false)) {
                fprintf(stderr, "qmp cycle failed\n");
                return 2;
            }
            pump(300);
        }
        pump(1000); /* settle */
        printf("AFTER_QMP_CPU %.1f (frames %ld)\n", pump_measure(5000),
               g_frames);
    }

    if (do_quit && !g_shutdown && !g_unload_now) {
        qmp_cycle(port, true);
        /* pump until the core reports shutdown (guest quit) */
        DWORD end = GetTickCount() + 30000;
        while (!g_shutdown && GetTickCount() < end) {
            p_retro_run();
            Sleep(15);
        }
        printf("SHUTDOWN_SEEN %d\n", g_shutdown ? 1 : 0);
    }

    g_audio_stop = true;
    WaitForSingleObject(g_audio_thread, 1000);

    p_retro_unload_game();
    p_retro_deinit();
    frame_profile_report();
    if (g_frame_profile_start_event) {
        CloseHandle(g_frame_profile_start_event);
    }
    if (g_frame_profile_stop_event) {
        CloseHandle(g_frame_profile_stop_event);
    }

    int total, in_mod;
    count_threads(dll, &total, &in_mod);
    printf("THREADS_BEFORE_FREE total=%d in_module=%d\n", total, in_mod);

    FreeLibrary(dll);
    /* cover the block thread-pool's 10s idle timeout: a leaked worker
     * wakes inside unmapped code within this window */
    Sleep(12000);
    count_threads(NULL, &total, &in_mod);
    printf("THREADS_AFTER total=%d\n", total);
    printf("TEARDOWN_OK\n");
    return 0;
}
