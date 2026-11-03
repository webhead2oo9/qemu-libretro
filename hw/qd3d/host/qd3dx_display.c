/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Display policy of an embedder whose display is one executor's frames or
 * the guest's desktop (qd3dx.h): the VM device follows it, the loopback
 * harness logs it, the self-test checks it with synthetic times.
 */
#include <string.h>

#include "qd3dx.h"

void qd3dx_display_init(struct qd3dx_display *d)
{
    memset(d, 0, sizeof *d);
}

int qd3dx_display_frame(struct qd3dx_display *d, unsigned slot, uint32_t repeat, uint64_t now)
{
    if (repeat == QD3DX_REPEAT_CURSOR && !(d->on && d->slot == slot))
        return QD3DX_DISPLAY_DROP;
    d->slot = slot;
    d->alive_us = now;
    d->idle_off = 0;
    if (d->on)
        return QD3DX_DISPLAY_SHOW;
    d->on = 1;
    return QD3DX_DISPLAY_ON;
}

int qd3dx_display_gone(struct qd3dx_display *d, unsigned slot)
{
    if (slot != d->slot)
        return 0;
    d->idle_off = 0;
    if (!d->on)
        return 0;
    d->on = 0;
    return 1;
}

int qd3dx_display_alive(struct qd3dx_display *d, unsigned slot, uint64_t now)
{
    if (slot != d->slot)
        return 0;
    d->alive_us = now;
    if (!d->idle_off)
        return 0;
    d->idle_off = 0;
    return 1;
}

int qd3dx_display_idle(struct qd3dx_display *d, uint64_t now)
{
    if (!d->on || (int64_t)(now - d->alive_us) <= (int64_t)QD3DX_DISPLAY_IDLE_US)
        return 0;
    d->on = 0;
    d->idle_off = 1;
    return 1;
}

int qd3dx_display_off(struct qd3dx_display *d)
{
    int was = d->on;

    d->on = 0;
    d->idle_off = 0;
    return was;
}
