/*
 * Core-option overrides shared between the libretro glue (ui/libretro.c)
 * and the qemu-3dfx config parsers (hw/mesa, hw/3dfx). Written on the
 * libretro frontend thread, read on emu/vCPU threads each time a 3D
 * context/window is created; -1 means "auto" (leave the cfg-file value
 * or built-in default alone). Word-sized + volatile is sufficient for
 * this single-writer, sample-at-creation pattern.
 */
#ifndef UI_LIBRETRO_VARS_H
#define UI_LIBRETRO_VARS_H

extern volatile int libretro_cfg_fps_limit;
extern volatile int libretro_cfg_ext_year;

#endif
