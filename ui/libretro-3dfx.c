/*
 * qemu-3dfx pass-through display glue for the libretro core
 *
 * The hw/3dfx and hw/mesa devices call these hooks when a guest enters
 * or leaves 3D rendering. Upstream qemu-3dfx implements them in
 * ui/sdl2.c against the SDL window; this fork has no window, so the
 * final implementation will create an offscreen WGL context and read
 * frames back into the libretro framebuffer (ui/libretro.c).
 *
 * Stub stage: keeps the devices linkable and inert. A guest that
 * activates 3D pass-through gets a warning instead of output.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "ui/console.h"

static void fx_glue_todo(void)
{
    warn_report_once("qemu-3dfx: libretro display glue not implemented yet; "
                     "3D pass-through output is disabled");
}

void glide_prepare_window(uint32_t res, int msaa, void *opaque, void *cwnd_fn)
{
    fx_glue_todo();
}

void glide_release_window(void *opaque, void *cwnd_fn)
{
    fx_glue_todo();
}

int glide_window_stat(const int activate)
{
    return 0;
}

int glide_gui_fullscreen(int *width, int *height)
{
    if (width) {
        *width = 640;
    }
    if (height) {
        *height = 480;
    }
    return 0;
}

void glide_renderer_stat(const int activate)
{
}

void mesa_renderer_stat(const int activate)
{
}

void mesa_prepare_window(int msaa, int alpha, int scale_x, void *cwnd_fn)
{
    fx_glue_todo();
}

void mesa_release_window(void)
{
}

void mesa_cursor_define(int hot_x, int hot_y, int width, int height,
                        const void *data)
{
}

void mesa_mouse_warp(int x, int y, const int on)
{
}

int mesa_gui_fullscreen(int *sizev)
{
    if (sizev) {
        sizev[0] = 640;
        sizev[1] = 480;
        sizev[2] = 640;
        sizev[3] = 480;
    }
    return 0;
}
