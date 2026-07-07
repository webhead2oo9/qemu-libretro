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
#include "ui/console.h"

#include <windows.h>

/* GL 1.1 subset, resolved from the opengl32.dll the devices already load */
#define FX_GL_NO_ERROR       0
#define FX_GL_UNSIGNED_BYTE  0x1401
#define FX_GL_PACK_ALIGNMENT 0x0D05
#define FX_GL_BGRA           0x80E1

static struct {
    const QemuFxSink *sink;
    HWND hwnd;
    int width, height;
    bool class_registered;
    uint32_t glide_res;      /* packed ((h & 0x7FFF) << 16) | w */
    bool glide_ready;
    void *readback;
    void (WINAPI *glReadPixels)(int, int, int, int, unsigned, unsigned,
                                void *);
    void (WINAPI *glPixelStorei)(unsigned, int);
    unsigned (WINAPI *glGetError)(void);
} fx;

void qemu_fx_register_sink(const QemuFxSink *sink)
{
    fx.sink = sink;
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
    fx.glGetError = (void *)GetProcAddress(gl, "glGetError");
    return fx.glReadPixels && fx.glPixelStorei && fx.glGetError;
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

static bool fx_window_create(int w, int h)
{
    static const char class_name[] = "qemu-3dfx-libretro";

    if (!fx.class_registered) {
        WNDCLASSA wc = {
            .style = CS_OWNDC,  /* GL needs a stable, private DC */
            .lpfnWndProc = DefWindowProcA,
            .hInstance = GetModuleHandleA(NULL),
            .lpszClassName = class_name,
        };
        if (!RegisterClassA(&wc)) {
            warn_report("qemu-3dfx: RegisterClass failed (0x%lx)",
                        GetLastError());
            return false;
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

static void fx_swap_readback(void)
{
    if (!fx.hwnd || !fx.width || !fx.height ||
        !fx.sink || !fx.sink->publish) {
        return;
    }
    if (!fx.glReadPixels && !fx_gl_resolve()) {
        return;
    }
    if (!fx.readback) {
        fx.readback = g_malloc((size_t)fx.width * fx.height * 4);
    }
    /* pre-swap: the frame is complete and still in the back buffer of
     * the context current on this thread */
    fx.glPixelStorei(FX_GL_PACK_ALIGNMENT, 4);
    fx.glReadPixels(0, 0, fx.width, fx.height,
                    FX_GL_BGRA, FX_GL_UNSIGNED_BYTE, fx.readback);
    if (fx.glGetError() != FX_GL_NO_ERROR) {
        warn_report_once("qemu-3dfx: BGRA readback failed; "
                         "3D output will not reach the frontend");
        return;
    }
    fx.sink->publish(fx.readback, fx.width, fx.height);
}

void mesa_swap_notify(void)
{
    fx_swap_readback();
}

void glide_swap_notify(void)
{
    fx_swap_readback();
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
