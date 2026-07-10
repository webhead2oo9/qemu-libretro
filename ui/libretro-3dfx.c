/*
 * qemu-3dfx pass-through display glue for the libretro core
 *
 * Upstream qemu-3dfx renders into the QEMU SDL2 window: ui/sdl2.c hands
 * the native window handle to hw/3dfx and hw/mesa, which create their
 * GL context on it and present with SwapBuffers. This fork has no
 * window at all, so instead:
 *
 *  - prepare_window creates a hidden native window sized to the guest
 *    render resolution and hands its HWND to the device callback; the
 *    pass-through GL context lives on that window. The window is never
 *    shown, but it must be full-size: the GL default framebuffer is the
 *    window, and an undersized one leaves nothing to read back.
 *
 *  - right before each buffer swap the devices call *_swap_notify();
 *    the finished frame is glReadPixels'd out of the back buffer and
 *    pushed to the QemuFxSink the libretro frontend registered, which
 *    publishes it in place of the VGA surface.
 *
 * Everything here runs under the BQL, on the vCPU thread that hit the
 * device MMIO handler (or a QEMU timer for deferred deactivation); the
 * pass-through GL context is only ever current on the vCPU thread.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/boards.h"
#include "sysemu/runstate.h"
#include "ui/console.h"
#include "trace.h"

#include <windows.h>

/* GL 1.1 subset, resolved from the opengl32.dll the devices already load */
#define FX_GL_UNSIGNED_BYTE  0x1401
#define FX_GL_PACK_ALIGNMENT 0x0D05
#define FX_GL_BGRA           0x80E1

/* pixel-buffer-object subset, resolved through wglGetProcAddress */
#define FX_GL_PIXEL_PACK_BUFFER 0x88EB
#define FX_GL_STREAM_READ       0x88E1
#define FX_GL_READ_ONLY         0x88B8

/* GL_ARB_sync subset. GLsync is an opaque pointer-sized handle. */
#define FX_GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define FX_GL_ALREADY_SIGNALED           0x911A
#define FX_GL_TIMEOUT_EXPIRED            0x911B
#define FX_GL_CONDITION_SATISFIED        0x911C

#define FX_PBO_LEGACY_COUNT 2
#define FX_PBO_FENCED_COUNT 3
#define FX_FRONTEND_IDLE_US 100000

/* Capture at most every 8 ms: uncapped guests (the swap interval is
 * forced to 0) can swap far faster than the frontend's ~60 Hz present,
 * and every readback stalls a vCPU that holds the BQL. */
#define FX_CAPTURE_MIN_US 8000

static struct {
    const QemuFxSink *sink;
    HWND hwnd;
    int width, height;
    bool class_registered;
    uint32_t glide_res;      /* packed ((h & 0x7FFF) << 16) | w */
    bool glide_ready;
    void *readback;
    int64_t last_capture_us;
    int pub_width, pub_height;  /* geometry of the last published frame */
    unsigned pbo[FX_PBO_FENCED_COUNT]; /* current-context object names */
    void *pbo_fence[FX_PBO_FENCED_COUNT];
    uint64_t pbo_sequence[FX_PBO_FENCED_COUNT];
    bool pbo_top_down[FX_PBO_FENCED_COUNT]; /* row order at capture time */
    uint64_t next_sequence;
    uint64_t min_publish_sequence;
    size_t pbo_size;
    int pbo_count;
    int pbo_index;              /* legacy path's next readback target */
    bool pbo_filled[FX_PBO_LEGACY_COUNT];
    bool sync_available;
    uint32_t frontend_frame_count;
    int64_t frontend_last_seen_us;
    bool frontend_idle;
    bool smp_error_reported;
    bool geometry_error_reported;
    bool pbo_broken;            /* no PBO support, or it errored: stay
                                 * on the synchronous path for good */
    void (WINAPI *glReadPixels)(int, int, int, int, unsigned, unsigned,
                                void *);
    void (WINAPI *glPixelStorei)(unsigned, int);
    void (WINAPI *glGenBuffers)(int, unsigned *);
    void (WINAPI *glBindBuffer)(unsigned, unsigned);
    void (WINAPI *glBufferData)(unsigned, ptrdiff_t, const void *,
                                unsigned);
    void *(WINAPI *glMapBuffer)(unsigned, unsigned);
    unsigned char (WINAPI *glUnmapBuffer)(unsigned);
    void *(WINAPI *glFenceSync)(unsigned, unsigned);
    unsigned (WINAPI *glClientWaitSync)(void *, unsigned, uint64_t);
    void (WINAPI *glDeleteSync)(void *);
} fx;

void qemu_fx_register_sink(const QemuFxSink *sink)
{
    fx.sink = sink;
    fx.smp_error_reported = false;
    fx.geometry_error_reported = false;
}

static bool fx_gl_resolve(void)
{
    /* already loaded by mesagl_impl.c or the host glide wrapper; if it
     * is not, no pass-through GL context can exist yet either */
    HMODULE gl = GetModuleHandleA("opengl32.dll");

    if (!gl) {
        return false;
    }
    fx.glReadPixels = (void *)GetProcAddress(gl, "glReadPixels");
    fx.glPixelStorei = (void *)GetProcAddress(gl, "glPixelStorei");
    return fx.glReadPixels && fx.glPixelStorei;
}

/* GL extension entry points are context/driver state. Never carry them, PBO
 * names, or a latched capability failure into the next hidden-window context. */
static void fx_gl_forget_context(void)
{
    fx.glReadPixels = NULL;
    fx.glPixelStorei = NULL;
    fx.glGenBuffers = NULL;
    fx.glBindBuffer = NULL;
    fx.glBufferData = NULL;
    fx.glMapBuffer = NULL;
    fx.glUnmapBuffer = NULL;
    fx.glFenceSync = NULL;
    fx.glClientWaitSync = NULL;
    fx.glDeleteSync = NULL;
    fx.sync_available = false;
    fx.pbo_broken = false;
}

static void fx_guest_dims(int *w, int *h)
{
    *w = 640;
    *h = 480;
    if (fx.sink && fx.sink->get_guest_dims) {
        fx.sink->get_guest_dims(w, h);
    }
    if (*w <= 0 || *h <= 0) {
        *w = 640;
        *h = 480;
    }
}

static bool fx_geometry_allowed(int w, int h)
{
    if (w <= 0 || h <= 0 || w > QEMU_FX_MAX_WIDTH ||
        h > QEMU_FX_MAX_HEIGHT) {
        if (!fx.geometry_error_reported) {
            error_report("qemu-3dfx resolution %dx%d is outside the safe "
                         "range 1x1 through %dx%d. The VM was paused before "
                         "allocating host graphics resources",
                         w, h, QEMU_FX_MAX_WIDTH, QEMU_FX_MAX_HEIGHT);
            fx.geometry_error_reported = true;
        }
        vm_stop(RUN_STATE_INTERNAL_ERROR);
        return false;
    }
    return true;
}

static bool fx_window_create(int w, int h)
{
    static const char class_name[] = "qemu-3dfx-libretro";

    if (!fx_geometry_allowed(w, h)) {
        return false;
    }

    if (!fx.class_registered) {
        WNDCLASSA wc = {
            .style = CS_OWNDC,  /* GL needs a stable, private DC */
            .lpfnWndProc = DefWindowProcA,
            .hInstance = GetModuleHandleA(NULL),
            .lpszClassName = class_name,
        };
        if (!RegisterClassA(&wc)) {
            DWORD error = GetLastError();
            WNDCLASSA existing = { 0 };

            /* The class is owned by the frontend EXE, so it survives
             * FreeLibrary/reload of this core DLL. Its window procedure is
             * user32's DefWindowProc, not unloaded core code: reuse it after
             * validating the GL-critical properties instead of making the
             * second in-process 3D session fail. */
            if (error != ERROR_CLASS_ALREADY_EXISTS ||
                !GetClassInfoA(wc.hInstance, class_name, &existing) ||
                existing.lpfnWndProc != DefWindowProcA ||
                !(existing.style & CS_OWNDC)) {
                warn_report("qemu-3dfx: incompatible window class "
                            "'%s' (RegisterClass error 0x%lx)",
                            class_name, error);
                return false;
            }
        }
        fx.class_registered = true;
    }
    /* WS_POPUP: the client area is exactly w x h; never shown */
    fx.hwnd = CreateWindowExA(0, class_name, "qemu-3dfx", WS_POPUP,
                              0, 0, w, h, NULL, NULL,
                              GetModuleHandleA(NULL), NULL);
    if (!fx.hwnd) {
        warn_report("qemu-3dfx: CreateWindow failed (0x%lx)",
                    GetLastError());
        return false;
    }
    fx.width = w;
    fx.height = h;
    return true;
}

static void fx_window_destroy(void)
{
    if (fx.hwnd) {
        DestroyWindow(fx.hwnd);
        fx.hwnd = NULL;
    }
    g_free(fx.readback);
    fx.readback = NULL;
    fx.width = 0;
    fx.height = 0;
    /* The GL context that owned the PBOs dies with the window; forget
     * its object names and retry capability resolution in the next one. */
    memset(fx.pbo, 0, sizeof(fx.pbo));
    memset(fx.pbo_fence, 0, sizeof(fx.pbo_fence));
    memset(fx.pbo_sequence, 0, sizeof(fx.pbo_sequence));
    memset(fx.pbo_top_down, 0, sizeof(fx.pbo_top_down));
    fx.pbo_size = 0;
    fx.pbo_count = 0;
    fx.pbo_index = 0;
    fx.pbo_filled[0] = false;
    fx.pbo_filled[1] = false;
    fx.next_sequence = 0;
    fx.min_publish_sequence = 0;
    fx.last_capture_us = 0;
    fx.frontend_frame_count = 0;
    fx.frontend_last_seen_us = 0;
    fx.frontend_idle = false;
    fx_gl_forget_context();
    /* zero pub geometry: the first frame in a new window must publish
     * immediately, however recently the old window published */
    fx.pub_width = 0;
    fx.pub_height = 0;
}

static void fx_set_active(bool on)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);

    /* the devices set the passthrough flag themselves but never clear
     * it; keep it in lockstep with the renderer state in both directions */
    if (con) {
        graphic_hw_passthrough(con, on);
    }
    if (fx.sink && fx.sink->set_active) {
        fx.sink->set_active(on);
    }
}

/* The qemu-3dfx devices issue host GL calls directly from the vCPU thread that
 * handled their MMIO. A WGL context cannot migrate safely between multiple
 * vCPU threads without dedicated render-thread marshalling, so stop before a
 * context/window exists rather than allowing intermittent host-driver access.
 * Check max_cpus as well as online CPUs so later CPU hotplug cannot invalidate
 * a context that was safe when it was created. */
static bool fx_3d_activation_allowed(void)
{
    unsigned int cpus;

    if (!current_machine || current_machine->smp.max_cpus <= 1) {
        return true;
    }

    cpus = current_machine->smp.max_cpus;
    if (!fx.smp_error_reported) {
        error_report("qemu-3dfx requires one virtual CPU; this VM permits "
                     "%u. The VM was paused before 3D activation. Remove "
                     "-smp or use -smp 1, then restart the content", cpus);
        fx.smp_error_reported = true;
    }
    vm_stop(RUN_STATE_INTERNAL_ERROR);
    return false;
}

/* Windows may return 1, 2, 3, or -1 for an unsupported extension symbol,
 * not just NULL. Treat every documented sentinel as absent before deciding
 * whether the driver supports the fenced path. */
static void *fx_wgl_get_proc(PROC (WINAPI *getproc)(LPCSTR),
                             const char *name)
{
    void *p = (void *)getproc(name);

    if (!p || p == (void *)(uintptr_t)1 || p == (void *)(uintptr_t)2 ||
        p == (void *)(uintptr_t)3 || p == (void *)(intptr_t)-1) {
        return NULL;
    }
    return p;
}

static bool fx_pbo_resolve(void)
{
    /* PBO entry points postdate opengl32.dll's export table; they only
     * come from wglGetProcAddress, which itself needs a current GL
     * context — true here, we are called mid-swap. Resolved directly
     * from opengl32.dll rather than via hw/mesa's MesaGLGetProc, which
     * is unusable in Glide-only sessions (SetMesaFuncPtr never ran). */
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    PROC (WINAPI *getproc)(LPCSTR);

    if (!gl) {
        return false;
    }
    getproc = (PROC (WINAPI *)(LPCSTR))GetProcAddress(gl,
                                                      "wglGetProcAddress");
    if (!getproc) {
        return false;
    }
#define FX_RESOLVE(fn) \
    fx.fn = fx_wgl_get_proc(getproc, #fn); \
    if (!fx.fn) { \
        fx.fn = fx_wgl_get_proc(getproc, #fn "ARB"); \
    }
    FX_RESOLVE(glGenBuffers);
    FX_RESOLVE(glBindBuffer);
    FX_RESOLVE(glBufferData);
    FX_RESOLVE(glMapBuffer);
    FX_RESOLVE(glUnmapBuffer);
    FX_RESOLVE(glFenceSync);
    FX_RESOLVE(glClientWaitSync);
    FX_RESOLVE(glDeleteSync);
#undef FX_RESOLVE
    fx.sync_available = fx.glFenceSync && fx.glClientWaitSync &&
                        fx.glDeleteSync;
    return fx.glGenBuffers && fx.glBindBuffer && fx.glBufferData &&
           fx.glMapBuffer && fx.glUnmapBuffer;
}

/* QEMU continues running when a libretro frontend pauses its retro_run calls.
 * Stop GPU readback after a short grace period, but leave guest execution and
 * audio alone. A wrapping counter avoids a cross-thread timestamp dependency. */
static bool fx_frontend_wants_capture(int64_t now, bool *resumed)
{
    uint32_t count;

    if (!fx.sink || !fx.sink->get_frontend_frame_count) {
        return true;
    }

    count = fx.sink->get_frontend_frame_count();
    if (count != fx.frontend_frame_count) {
        fx.frontend_frame_count = count;
        fx.frontend_last_seen_us = now;
        if (fx.frontend_idle) {
            fx.frontend_idle = false;
            fx.min_publish_sequence = fx.next_sequence + 1;
            fx.pbo_index = 0;
            fx.pbo_filled[0] = false;
            fx.pbo_filled[1] = false;
            *resumed = true;
            trace_libretro_3dfx_readback("frontend-resume",
                                         fx.width, fx.height);
        }
        return true;
    }

    if (!fx.frontend_last_seen_us) {
        fx.frontend_last_seen_us = now;
        return true;
    }
    if (now - fx.frontend_last_seen_us <= FX_FRONTEND_IDLE_US) {
        return true;
    }

    if (!fx.frontend_idle) {
        fx.frontend_idle = true;
        fx.min_publish_sequence = fx.next_sequence + 1;
        fx.pbo_index = 0;
        fx.pbo_filled[0] = false;
        fx.pbo_filled[1] = false;
        trace_libretro_3dfx_readback("frontend-idle",
                                     fx.width, fx.height);
    }
    return false;
}

static void fx_pbo_clear_fences(void)
{
    if (fx.glDeleteSync) {
        for (int i = 0; i < fx.pbo_count; i++) {
            if (fx.pbo_fence[i]) {
                fx.glDeleteSync(fx.pbo_fence[i]);
            }
        }
    }
    memset(fx.pbo_fence, 0, sizeof(fx.pbo_fence));
    memset(fx.pbo_sequence, 0, sizeof(fx.pbo_sequence));
}

static void fx_pbo_fail(void)
{
    fx.glBindBuffer(FX_GL_PIXEL_PACK_BUFFER, 0);
    fx_pbo_clear_fences();
    fx.pbo_broken = true;
    warn_report_once("qemu-3dfx: PBO readback failed; using "
                     "synchronous readback");
}

/* Have the current context's PBO ring exist at the right size. */
static bool fx_pbo_ready(size_t size)
{
    if (!fx.glGenBuffers && !fx_pbo_resolve()) {
        fx.pbo_broken = true;
        return false;
    }
    if (!fx.pbo[0]) {
        fx.pbo_count = fx.sync_available ? FX_PBO_FENCED_COUNT :
                                          FX_PBO_LEGACY_COUNT;
        fx.glGenBuffers(fx.pbo_count, fx.pbo);
        for (int i = 0; i < fx.pbo_count; i++) {
            if (!fx.pbo[i]) {
                fx.pbo_broken = true;
                return false;
            }
        }
        fx.pbo_size = 0;
    }
    if (fx.pbo_size != size) {
        fx_pbo_clear_fences();
        for (int i = 0; i < fx.pbo_count; i++) {
            fx.glBindBuffer(FX_GL_PIXEL_PACK_BUFFER, fx.pbo[i]);
            fx.glBufferData(FX_GL_PIXEL_PACK_BUFFER, size, NULL,
                            FX_GL_STREAM_READ);
        }
        fx.pbo_filled[0] = false;
        fx.pbo_filled[1] = false;
        fx.pbo_size = size;
        fx.pbo_index = 0;
        fx.next_sequence = 0;
        fx.min_publish_sequence = 0;
    }
    return true;
}

/* The fence has already reported completion, so mapping must not wait for
 * the GPU. publish() copies the mapped bytes before this function unmaps. */
static bool fx_pbo_publish(int index, const char *event, bool *published)
{
    fx.glBindBuffer(FX_GL_PIXEL_PACK_BUFFER, fx.pbo[index]);
    int64_t map_start = g_get_monotonic_time();
    void *p = fx.glMapBuffer(FX_GL_PIXEL_PACK_BUFFER, FX_GL_READ_ONLY);

    trace_libretro_3dfx_map(fx.width, fx.height,
                            g_get_monotonic_time() - map_start);
    if (!p) {
        return false;
    }

    /* Row order was latched when this PBO's readback was kicked; the
     * frontend flips bottom-up frames on present and passes top-down
     * ones through. */
    fx.sink->publish(p, fx.width, fx.height, fx.pbo_top_down[index]);
    trace_libretro_3dfx_readback(event, fx.width, fx.height);
    *published = true;
    return fx.glUnmapBuffer(FX_GL_PIXEL_PACK_BUFFER) != 0;
}

/* Compatibility path for drivers that expose PBOs but not GL_ARB_sync.
 * This preserves the previous two-buffer behavior, including its possible
 * map wait, rather than dropping PBO acceleration on older wrappers. */
static bool fx_pbo_legacy_readback(bool top_down, bool *published)
{
    int cur = fx.pbo_index;
    int prev = cur ^ 1;

    fx.glBindBuffer(FX_GL_PIXEL_PACK_BUFFER, fx.pbo[cur]);
    fx.glReadPixels(0, 0, fx.width, fx.height,
                    FX_GL_BGRA, FX_GL_UNSIGNED_BYTE, NULL);
    fx.pbo_top_down[cur] = top_down;
    trace_libretro_3dfx_readback("pbo-kick-legacy", fx.width, fx.height);

    if (fx.pbo_filled[prev] &&
        !fx_pbo_publish(prev, "pbo-publish-legacy", published)) {
        fx_pbo_fail();
        return false;
    }

    fx.glBindBuffer(FX_GL_PIXEL_PACK_BUFFER, 0);

    fx.pbo_filled[cur] = true;
    fx.pbo_index = prev;
    return true;
}

/* Poll every fence with a zero timeout and publish only the newest completed
 * transfer. Older completed frames are discarded, and a full ring skips the
 * next capture; neither condition is allowed to turn into a vCPU wait. */
static bool fx_pbo_fenced_readback(bool top_down, bool *published)
{
    bool ready[FX_PBO_FENCED_COUNT] = { false };
    int newest = -1;
    uint64_t newest_sequence = 0;

    for (int i = 0; i < fx.pbo_count; i++) {
        unsigned status;

        if (!fx.pbo_fence[i]) {
            continue;
        }
        status = fx.glClientWaitSync(fx.pbo_fence[i], 0, 0);
        if (status == FX_GL_ALREADY_SIGNALED ||
            status == FX_GL_CONDITION_SATISFIED) {
            ready[i] = true;
            if (fx.pbo_sequence[i] >= fx.min_publish_sequence &&
                (newest < 0 || fx.pbo_sequence[i] > newest_sequence)) {
                newest = i;
                newest_sequence = fx.pbo_sequence[i];
            }
        } else if (status != FX_GL_TIMEOUT_EXPIRED) {
            trace_libretro_3dfx_readback("pbo-wait-failed",
                                         fx.width, fx.height);
            fx_pbo_fail();
            return false;
        }
    }

    for (int i = 0; i < fx.pbo_count; i++) {
        if (!ready[i] || i == newest) {
            continue;
        }
        fx.glDeleteSync(fx.pbo_fence[i]);
        fx.pbo_fence[i] = NULL;
        fx.pbo_sequence[i] = 0;
        trace_libretro_3dfx_readback("pbo-discard", fx.width, fx.height);
    }

    if (newest >= 0) {
        bool ok = fx_pbo_publish(newest, "pbo-publish", published);

        fx.glDeleteSync(fx.pbo_fence[newest]);
        fx.pbo_fence[newest] = NULL;
        fx.pbo_sequence[newest] = 0;
        if (!ok) {
            fx_pbo_fail();
            return false;
        }
    }

    for (int i = 0; i < fx.pbo_count; i++) {
        if (fx.pbo_fence[i]) {
            continue;
        }
        fx.glBindBuffer(FX_GL_PIXEL_PACK_BUFFER, fx.pbo[i]);
        fx.glReadPixels(0, 0, fx.width, fx.height,
                        FX_GL_BGRA, FX_GL_UNSIGNED_BYTE, NULL);
        fx.pbo_top_down[i] = top_down;
        fx.pbo_fence[i] = fx.glFenceSync(FX_GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!fx.pbo_fence[i]) {
            trace_libretro_3dfx_readback("pbo-fence-failed",
                                         fx.width, fx.height);
            fx_pbo_fail();
            return false;
        }
        fx.pbo_sequence[i] = ++fx.next_sequence;
        trace_libretro_3dfx_readback("pbo-kick", fx.width, fx.height);
        fx.glBindBuffer(FX_GL_PIXEL_PACK_BUFFER, 0);
        return true;
    }

    fx.glBindBuffer(FX_GL_PIXEL_PACK_BUFFER, 0);
    trace_libretro_3dfx_readback("pbo-full-skip", fx.width, fx.height);
    return true;
}

static void fx_swap_readback(bool top_down)
{
    if (!fx.hwnd || !fx.width || !fx.height ||
        !fx.sink || !fx.sink->publish) {
        return;
    }
    int64_t now = g_get_monotonic_time();
    bool resumed = false;

    if (!fx_frontend_wants_capture(now, &resumed)) {
        return;
    }
    if (!fx.glReadPixels && !fx_gl_resolve()) {
        return;
    }

    /* Bound the capture/poll rate; publishing faster than the frontend
     * presents only burns vCPU time under the BQL. Never skip after a
     * geometry change — the frontend re-inits its video driver to
     * black on those and needs a real frame at the new size. */
    bool resized = fx.width != fx.pub_width || fx.height != fx.pub_height;
    bool force_sync = resized || resumed;
    if (!force_sync && now - fx.last_capture_us < FX_CAPTURE_MIN_US) {
        return;
    }
    fx.last_capture_us = now;

    size_t size = (size_t)fx.width * fx.height * 4;
    bool published = false;
    bool async_done = false;

    /* pre-swap: the frame is complete and still in the back buffer of
     * the context current on this thread */
    fx.glPixelStorei(FX_GL_PACK_ALIGNMENT, 4);

    /* The fenced three-buffer path maps only transfers the GPU reports
     * complete. Drivers without sync objects retain the old two-PBO
     * fallback. After a size change or resume, the synchronous path below
     * supplies a current frame immediately instead of one swap later. */
    if (!fx.pbo_broken && !force_sync && fx_pbo_ready(size)) {
        if (fx.sync_available) {
            async_done = fx_pbo_fenced_readback(top_down, &published);
        } else {
            async_done = fx_pbo_legacy_readback(top_down, &published);
        }
    }

    if (!async_done && !published) {
        /* Synchronous path: PBO-less or erroring drivers, and the first
         * frame after a geometry change or frontend resume. */
        if (!fx.readback) {
            /* glReadPixels has no operation-local status result. Keep a
             * deterministic black fallback without consuming the guest's
             * context-global GL error queue to infer success. */
            fx.readback = g_malloc0(size);
        }
        fx.glReadPixels(0, 0, fx.width, fx.height,
                        FX_GL_BGRA, FX_GL_UNSIGNED_BYTE, fx.readback);
        trace_libretro_3dfx_readback("sync-publish", fx.width, fx.height);
        fx.sink->publish(fx.readback, fx.width, fx.height, top_down);
        published = true;
    }

    if (published) {
        fx.pub_width = fx.width;
        fx.pub_height = fx.height;
    }
}

/* The mesa hidden window is sized from the VGA surface when the guest
 * creates its first GL context — for wine9x that happens at adapter
 * enumeration, before the game switches the display mode, so the game
 * then renders a smaller frame into a stale, larger back buffer (seen
 * as the picture sitting high in a black band). Follow guest mode
 * changes by resizing the window in place; the GL default framebuffer
 * tracks the window. Glide is excluded: its window is sized from
 * grSstWinOpen's packed resolution, which the VGA surface need not
 * match (DOS-era guests run Glide at 640x480 over a text-mode VGA).
 * Return true when the current back buffer must not be captured: either a
 * resize discarded it, or an unsafe size was rejected and the VM stopped. */
static bool fx_mesa_prepare_guest_size(void)
{
    int w, h;

    if (!fx.hwnd) {
        return false;
    }
    fx_guest_dims(&w, &h);
    if (w == fx.width && h == fx.height) {
        return false;
    }
    if (!fx_geometry_allowed(w, h)) {
        return true;
    }
    if (!SetWindowPos(fx.hwnd, NULL, 0, 0, w, h,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        warn_report_once("qemu-3dfx: window resize to %dx%d failed (0x%lx)",
                         w, h, GetLastError());
        return false;
    }
    fx.width = w;
    fx.height = h;
    /* The sync-path buffer is sized once per window; the PBO ring
     * re-sizes itself (and drops in-flight fences) in fx_pbo_ready. */
    g_clear_pointer(&fx.readback, g_free);
    trace_libretro_3dfx_readback("window-resize", w, h);
    return true;
}

void mesa_swap_notify(int top_down)
{
    /* A resize discards the back buffer, so there is nothing valid to
     * read this swap; the size change forces a synchronous publish on
     * the next one (pub_width/pub_height no longer match). */
    if (fx_mesa_prepare_guest_size()) {
        return;
    }
    fx_swap_readback(top_down != 0);
}

void glide_swap_notify(void)
{
    /* Native GL rows, always bottom-up. */
    fx_swap_readback(false);
}

/*
 * MESA GL pass-through hooks (hw/mesa).
 *
 * mesa_prepare_window must create the window and invoke cwnd_fn before
 * returning: cwnd_mesagl stores the HWND and flags wnd_ready, and the
 * device does GetDC(hwnd) immediately after this returns.
 */

void mesa_prepare_window(int msaa, int alpha, int scale_x, void *cwnd_fn)
{
    void (*cwnd)(void *, void *, void *) = cwnd_fn;
    int w, h;

    fx_window_destroy();
    if (!fx_3d_activation_allowed()) {
        return;
    }
    fx_guest_dims(&w, &h);
    if (!fx_window_create(w, h)) {
        return;         /* wnd_ready stays 0; the guest keeps polling */
    }
    cwnd(NULL, fx.hwnd, NULL);
}

void mesa_release_window(void)
{
    fx_set_active(false);
    fx_window_destroy();
}

void mesa_renderer_stat(const int activate)
{
    fx_set_active(activate != 0);
}

int mesa_gui_fullscreen(int *sizev)
{
    if (sizev) {
        int w = fx.width ? fx.width : 640;
        int h = fx.height ? fx.height : 480;

        /* present size == guest size: the device's blit scaler no-ops
         * and rendering stays at guest-native resolution */
        sizev[0] = w;
        sizev[1] = h;   /* bit 15 clear: preserve aspect */
        sizev[2] = w;
        sizev[3] = h;
    }
    return 0;
}

void mesa_cursor_define(int hot_x, int hot_y, int width, int height,
                        const void *data)
{
    /* host cursor overlay: not applicable, the libretro core drives the
     * guest cursor with relative input */
}

void mesa_mouse_warp(int x, int y, const int on)
{
}

/*
 * Glide pass-through hooks (hw/3dfx). res packs the guest resolution as
 * ((h & 0x7FFF) << 16) | w. Invoking cwnd_fn runs grSstWinOpen /
 * grSstWinClose in the host glide wrapper DLL against our window
 * (disp_cb->FEnum was set by the device beforehand).
 */

void glide_prepare_window(uint32_t res, int msaa, void *opaque,
                          void *cwnd_fn)
{
    void (*cwnd)(void *, void *, void *) = cwnd_fn;
    int w = res & 0xFFFFU;
    int h = (res >> 0x10) & 0x7FFFU;

    fx_window_destroy();
    fx.glide_ready = false;
    if (!fx_3d_activation_allowed()) {
        return;
    }
    if (!w || !h) {
        w = 640;
        h = 480;
    }
    if (!fx_window_create(w, h)) {
        return;         /* glide_window_stat keeps reporting not-ready */
    }
    fx.glide_res = ((h & 0x7FFFU) << 0x10) | (w & 0xFFFFU);
    cwnd(NULL, fx.hwnd, opaque);        /* grSstWinOpen(hwnd, ...) */
    fx.glide_ready = true;
}

void glide_release_window(void *opaque, void *cwnd_fn)
{
    void (*cwnd)(void *, void *, void *) = cwnd_fn;

    if (fx.hwnd) {
        cwnd(NULL, fx.hwnd, opaque);    /* grSstWinClose / teardown */
    }
    fx.glide_ready = false;
    fx_set_active(false);
    fx_window_destroy();
}

int glide_window_stat(const int activate)
{
    if (activate) {
        /* > 1 = window is up; the device replaces the value with its
         * own packed resolution and declares the guest ready */
        return fx.glide_ready ? (int)fx.glide_res : 0;
    }
    return 0;                           /* deactivation complete */
}

int glide_gui_fullscreen(int *width, int *height)
{
    /* windowed + zero size: keeps the wrapper from touching the host
     * display mode and keeps the device's auto-upscale (cfg_scaleX)
     * off, so rendering stays at guest-native resolution */
    if (width) {
        *width = 0;
    }
    if (height) {
        *height = 0;
    }
    return 0;
}

void glide_renderer_stat(const int activate)
{
    fx_set_active(activate != 0);
}
