/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Extension operations other than UPLOAD/READBACK: HELLO, GOODBYE, RELEASE,
 * CREATE_DEVICE, RESET, PRESENT, QUERY_DATA, and the per-device hidden
 * window and present readback.
 */
#include <stdlib.h>

#include "qd3dx_priv.h"

static const wchar_t qd3dx_wndclass_name[] = L"QD3DXHiddenDeviceWindow";

typedef HRESULT (WINAPI *qd3dx_create9ex_fn)(UINT, IDirect3D9Ex **);

/* Loads the system d3d9.dll by full path (never a proxy beside the exe). */
static HRESULT qd3dx_d3d_init(struct qd3dx *ex)
{
    char path[MAX_PATH + 16];
    UINT n;
    qd3dx_create9ex_fn create;
    HRESULT hr;

    if (ex->d3d)
        return S_OK;
    if (!ex->d3d9_dll) {
        n = GetSystemDirectoryA(path, MAX_PATH);
        if (n == 0u || n >= MAX_PATH)
            return D3DERR_NOTAVAILABLE;
        memcpy(path + n, "\\d3d9.dll", sizeof "\\d3d9.dll");
        ex->d3d9_dll = LoadLibraryA(path);
        if (!ex->d3d9_dll) {
            qd3dx_log(ex, "qd3dx: cannot load %s (error %lu)", path, GetLastError());
            return D3DERR_NOTAVAILABLE;
        }
    }
    create = (qd3dx_create9ex_fn)(void (*)(void))GetProcAddress(ex->d3d9_dll, "Direct3DCreate9Ex");
    if (!create) {
        qd3dx_log(ex, "qd3dx: d3d9.dll has no Direct3DCreate9Ex");
        return D3DERR_NOTAVAILABLE;
    }
    hr = create(D3D_SDK_VERSION, &ex->d3d);
    if (FAILED(hr) || !ex->d3d) {
        qd3dx_log(ex, "qd3dx: Direct3DCreate9Ex failed 0x%08lx", (unsigned long)hr);
        ex->d3d = NULL;
        return FAILED(hr) ? hr : D3DERR_NOTAVAILABLE;
    }
    return S_OK;
}

uint32_t qd3dx_op_hello(struct qd3dx *ex, const struct qd3d_cmd *c,
                        struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    const void *p = qd3d_rd_raw(rd, sizeof(struct qd3d_hello));
    struct qd3d_hello h;
    struct qd3d_hello_reply r;
    void *out;
    HRESULT hr;

    if (!p || !qd3d_rd_end(rd) || c->obj != 0u)
        return QD3D_FAULT_DECODE;
    memcpy(&h, p, sizeof h);
    if (h.d3d_id == 0u || QD3D_ID_IS_HOST(h.d3d_id))
        return QD3D_FAULT_ID;

    /* A new session: everything of the previous one goes, including a
     * frame still in flight (an owner of the slot that ended without
     * GOODBYE, so its process died or lost the channel). */
    if (ex->pid || qd3dx_live_objects(ex))
        qd3dx_log(ex, "qd3dx: HELLO pid %u ends the session of pid %u, releasing %u objects", h.pid, ex->pid,
                  qd3dx_live_objects(ex));
    qd3dx_present_drop(ex);
    qd3dx_session_release(ex);
    /* HELLO is its owner's first command: frames a dead predecessor counted
     * but never sent (it died inside Present) must not hold back the new
     * session's frame pacing. */
    ex->ctrl->frames_done = ex->ctrl->frames_submitted;
    ex->pid = h.pid;
    ex->log_budget = QD3DX_WARN_BUDGET;

    memset(&r, 0, sizeof r);
    r.abi = QD3D_ABI_VERSION;
    r.max_cmd = ex->ring_size / 2u;
    r.max_reply = ex->reply_size - (uint32_t)sizeof(struct qd3d_reply);
    if (h.abi != QD3D_ABI_VERSION) {
        qd3dx_log(ex, "qd3dx: HELLO abi %u, host speaks %u", h.abi, QD3D_ABI_VERSION);
        hr = D3DERR_NOTAVAILABLE;
    } else {
        hr = qd3dx_d3d_init(ex);
        if (SUCCEEDED(hr)) {
            ex->d3d->lpVtbl->AddRef(ex->d3d);
            if (!qd3dx_bind(ex, h.d3d_id, (IUnknown *)ex->d3d, QD3D_T_IDirect3D9)) {
                ex->d3d->lpVtbl->Release(ex->d3d);
                hr = E_OUTOFMEMORY;
            }
        }
    }
    qd3dx_log(ex, "qd3dx: HELLO pid %u sdk 0x%x d3d id %u -> 0x%08lx", h.pid, h.sdk_version,
              h.d3d_id, (unsigned long)hr);
    out = qd3d_wr_raw(rp, sizeof r);
    if (!out)
        return QD3D_FAULT_REPLY;
    memcpy(out, &r, sizeof r);
    *ret = (uint32_t)hr;
    return QD3D_FAULT_NONE;
}

uint32_t qd3dx_op_goodbye(struct qd3dx *ex, const struct qd3d_cmd *c,
                          struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    if (!qd3d_rd_end(rd) || c->obj != 0u)
        return QD3D_FAULT_DECODE;
    qd3dx_log(ex, "qd3dx: GOODBYE pid %u, releasing %u objects", ex->pid, qd3dx_live_objects(ex));
    qd3dx_session_release(ex);
    ex->pid = 0;
    *ret = (uint32_t)D3D_OK;
    return QD3D_FAULT_NONE;
}

uint32_t qd3dx_op_release(struct qd3dx *ex, const struct qd3d_cmd *c,
                          struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    struct qd3dx_ent *e;

    if (!qd3d_rd_end(rd))
        return QD3D_FAULT_DECODE;
    e = qd3dx_ent_get(ex, c->obj);
    if (!e)
        return QD3D_FAULT_OBJECT;
    qd3dx_ent_release(ex, e);
    *ret = (uint32_t)D3D_OK;
    return QD3D_FAULT_NONE;
}

/* ------------------------------------------------------------------------ */
/* Devices                                                                  */
/* ------------------------------------------------------------------------ */

static HWND qd3dx_hidden_window(struct qd3dx *ex, uint32_t w, uint32_t h)
{
    static int registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    HWND hwnd;

    if (!registered) {
        WNDCLASSEXW wc;

        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = inst;
        wc.lpszClassName = qd3dx_wndclass_name;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            qd3dx_log(ex, "qd3dx: RegisterClassEx failed (%lu)", GetLastError());
            return NULL;
        }
        registered = 1;
    }
    hwnd = CreateWindowExW(0, qd3dx_wndclass_name, L"qd3dx device", WS_OVERLAPPEDWINDOW,
                           0, 0, (int)(w ? w : 64u), (int)(h ? h : 64u), NULL, NULL, inst, NULL);
    if (!hwnd)
        qd3dx_log(ex, "qd3dx: CreateWindowEx failed (%lu)", GetLastError());
    return hwnd;
}

/* Guest present parameters -> host present parameters (off screen). */
static void qd3dx_pp_from_wire(D3DPRESENT_PARAMETERS *pp, const struct qd3d_wire_pp *w, HWND hwnd)
{
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = w->BackBufferWidth;
    pp->BackBufferHeight = w->BackBufferHeight;
    pp->BackBufferFormat = (D3DFORMAT)w->BackBufferFormat;
    pp->BackBufferCount = w->BackBufferCount;
    pp->MultiSampleType = (D3DMULTISAMPLE_TYPE)w->MultiSampleType;
    pp->MultiSampleQuality = w->MultiSampleQuality;
    pp->SwapEffect = (D3DSWAPEFFECT)w->SwapEffect;
    pp->hDeviceWindow = hwnd;
    pp->Windowed = TRUE;
    pp->EnableAutoDepthStencil = w->EnableAutoDepthStencil ? TRUE : FALSE;
    pp->AutoDepthStencilFormat = (D3DFORMAT)w->AutoDepthStencilFormat;
    pp->Flags = w->Flags;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

/* Accepted parameters: native values, except the guest-owned window,
 * windowed, refresh and interval fields, which are echoed. */
static void qd3dx_pp_to_wire(struct qd3d_wire_pp *w, const D3DPRESENT_PARAMETERS *pp,
                             const struct qd3d_wire_pp *guest)
{
    *w = *guest;
    w->BackBufferWidth = pp->BackBufferWidth;
    w->BackBufferHeight = pp->BackBufferHeight;
    w->BackBufferFormat = (uint32_t)pp->BackBufferFormat;
    w->BackBufferCount = pp->BackBufferCount;
    w->MultiSampleType = (uint32_t)pp->MultiSampleType;
    w->MultiSampleQuality = pp->MultiSampleQuality;
    w->SwapEffect = (uint32_t)pp->SwapEffect;
    w->EnableAutoDepthStencil = (uint32_t)pp->EnableAutoDepthStencil;
    w->AutoDepthStencilFormat = (uint32_t)pp->AutoDepthStencilFormat;
    w->Flags = pp->Flags;
}

/* Guest parameters the host cannot honour off screen; answered with
 * D3DERR_INVALIDCALL like the native runtime would for nonsense sizes. */
static int qd3dx_pp_valid(const struct qd3d_wire_pp *w)
{
    return w->BackBufferWidth >= 1u && w->BackBufferWidth <= 16384u &&
           w->BackBufferHeight >= 1u && w->BackBufferHeight <= 16384u &&
           w->BackBufferCount <= 3u;
}

#define QD3DX_BEHAVIOR_KEEP (D3DCREATE_SOFTWARE_VERTEXPROCESSING | \
                             D3DCREATE_HARDWARE_VERTEXPROCESSING | \
                             D3DCREATE_MIXED_VERTEXPROCESSING | \
                             D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED)

uint32_t qd3dx_op_create_device(struct qd3dx *ex, const struct qd3d_cmd *c,
                                struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    const void *p = qd3d_rd_raw(rd, sizeof(struct qd3d_create_device));
    struct qd3d_create_device cd;
    struct qd3d_wire_pp accepted;
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9Ex *dev = NULL;
    struct qd3dx_dev *d;
    struct qd3dx_ent *e;
    void *out;
    HRESULT hr;

    if (!qd3dx_obj(ex, c->obj, QD3D_T_IDirect3D9))
        return QD3D_FAULT_OBJECT;
    if (!p || !qd3d_rd_end(rd))
        return QD3D_FAULT_DECODE;
    memcpy(&cd, p, sizeof cd);
    if (!qd3dx_id_is_fresh(ex, cd.device_id))
        return QD3D_FAULT_ID;

    accepted = cd.pp;
    if (!qd3dx_pp_valid(&cd.pp) || cd.adapter >= ex->d3d->lpVtbl->GetAdapterCount(ex->d3d)) {
        hr = D3DERR_INVALIDCALL;
        goto reply;
    }
    d = calloc(1, sizeof *d);
    if (!d) {
        hr = E_OUTOFMEMORY;
        goto reply;
    }
    d->front = -1;
    d->hwnd = qd3dx_hidden_window(ex, cd.pp.BackBufferWidth, cd.pp.BackBufferHeight);
    if (!d->hwnd) {
        free(d);
        hr = D3DERR_NOTAVAILABLE;
        goto reply;
    }
    qd3dx_pp_from_wire(&pp, &cd.pp, d->hwnd);
    hr = ex->d3d->lpVtbl->CreateDeviceEx(ex->d3d, cd.adapter, D3DDEVTYPE_HAL, d->hwnd,
                                         cd.behavior_flags & QD3DX_BEHAVIOR_KEEP, &pp, NULL, &dev);
    if (FAILED(hr) || !dev) {
        qd3dx_log(ex, "qd3dx: CreateDeviceEx %ux%u fmt %u ms %u swap %u flags 0x%x failed 0x%08lx",
                  cd.pp.BackBufferWidth, cd.pp.BackBufferHeight, cd.pp.BackBufferFormat,
                  cd.pp.MultiSampleType, cd.pp.SwapEffect, cd.behavior_flags, (unsigned long)hr);
        DestroyWindow(d->hwnd);
        free(d);
        if (SUCCEEDED(hr))
            hr = D3DERR_NOTAVAILABLE;
        goto reply;
    }
    if (!qd3dx_bind(ex, cd.device_id, (IUnknown *)dev, QD3D_T_IDirect3DDevice9)) {
        dev->lpVtbl->Release(dev);
        DestroyWindow(d->hwnd);
        free(d);
        hr = E_OUTOFMEMORY;
        goto reply;
    }
    e = qd3dx_ent_get(ex, cd.device_id);
    d->dev = dev;
    d->next = ex->devices;
    ex->devices = d;
    e->dev = d;
    qd3dx_dev_init_state(d);
    qd3dx_pp_to_wire(&accepted, &pp, &cd.pp);
    qd3dx_log(ex, "qd3dx: device %u: %ux%u fmt %u count %u ms %u swap %u depth %u/%u",
              cd.device_id, pp.BackBufferWidth, pp.BackBufferHeight, (unsigned)pp.BackBufferFormat,
              pp.BackBufferCount, (unsigned)pp.MultiSampleType, (unsigned)pp.SwapEffect,
              (unsigned)pp.EnableAutoDepthStencil, (unsigned)pp.AutoDepthStencilFormat);

reply:
    out = qd3d_wr_raw(rp, sizeof accepted);
    if (!out)
        return QD3D_FAULT_REPLY;
    memcpy(out, &accepted, sizeof accepted);
    *ret = (uint32_t)hr;
    return QD3D_FAULT_NONE;
}

uint32_t qd3dx_op_reset(struct qd3dx *ex, const struct qd3d_cmd *c,
                        struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    const void *p = qd3d_rd_raw(rd, sizeof(struct qd3d_wire_pp));
    struct qd3d_wire_pp w, accepted;
    D3DPRESENT_PARAMETERS pp;
    struct qd3dx_ent *e = qd3dx_ent_get(ex, c->obj);
    struct qd3dx_dev *d;
    void *out;
    HRESULT hr;

    if (!e || e->type != QD3D_T_IDirect3DDevice9 || !e->dev)
        return QD3D_FAULT_OBJECT;
    if (!p || !qd3d_rd_end(rd))
        return QD3D_FAULT_DECODE;
    memcpy(&w, p, sizeof w);
    d = e->dev;
    accepted = w;
    if (!qd3dx_pp_valid(&w)) {
        hr = D3DERR_INVALIDCALL;
    } else {
        qd3dx_present_flush(ex); /* frames presented before Reset reach the sink first */
        qd3dx_pp_from_wire(&pp, &w, d->hwnd);
        hr = d->dev->lpVtbl->ResetEx(d->dev, &pp, NULL);
        if (SUCCEEDED(hr)) {
            qd3dx_dev_reset_state(d, pp.BackBufferWidth, pp.BackBufferHeight);
            qd3dx_pp_to_wire(&accepted, &pp, &w);
        } else {
            qd3dx_log(ex, "qd3dx: ResetEx %ux%u fmt %u failed 0x%08lx", w.BackBufferWidth,
                      w.BackBufferHeight, w.BackBufferFormat, (unsigned long)hr);
        }
    }
    out = qd3d_wr_raw(rp, sizeof accepted);
    if (!out)
        return QD3D_FAULT_REPLY;
    memcpy(out, &accepted, sizeof accepted);
    *ret = (uint32_t)hr;
    return QD3D_FAULT_NONE;
}

/* ------------------------------------------------------------------------ */
/* Additional swap chains                                                   */
/* ------------------------------------------------------------------------ */

/* The guest resolved the defaults and refused fullscreen swap chains (as
 * the native runtime does); here the swap chain presents to the device's
 * hidden window like the implicit one. */
uint32_t qd3dx_op_create_swapchain(struct qd3dx *ex, const struct qd3d_cmd *c,
                                   struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    const void *p = qd3d_rd_raw(rd, sizeof(struct qd3d_create_swapchain));
    struct qd3dx_ent *e = qd3dx_ent_get(ex, c->obj);
    struct qd3d_create_swapchain cs;
    struct qd3d_wire_pp accepted;
    D3DPRESENT_PARAMETERS pp;
    IDirect3DSwapChain9 *sc = NULL;
    IDirect3DDevice9Ex *dev;
    void *out;
    HRESULT hr;

    if (!e || e->type != QD3D_T_IDirect3DDevice9 || !e->dev)
        return QD3D_FAULT_OBJECT;
    if (!p || !qd3d_rd_end(rd))
        return QD3D_FAULT_DECODE;
    memcpy(&cs, p, sizeof cs);
    if (!qd3dx_id_is_fresh(ex, cs.swapchain_id))
        return QD3D_FAULT_ID;
    dev = e->dev->dev;
    accepted = cs.pp;
    if (!qd3dx_pp_valid(&cs.pp)) {
        hr = D3DERR_INVALIDCALL;
    } else {
        qd3dx_pp_from_wire(&pp, &cs.pp, e->dev->hwnd);
        hr = dev->lpVtbl->CreateAdditionalSwapChain(dev, &pp, &sc);
        if (SUCCEEDED(hr) && !sc)
            hr = D3DERR_NOTAVAILABLE;
        if (SUCCEEDED(hr) && !qd3dx_bind(ex, cs.swapchain_id, (IUnknown *)sc, QD3D_T_IDirect3DSwapChain9)) {
            sc->lpVtbl->Release(sc);
            hr = E_OUTOFMEMORY;
        }
        if (SUCCEEDED(hr))
            qd3dx_pp_to_wire(&accepted, &pp, &cs.pp);
        else
            qd3dx_log(ex, "qd3dx: CreateAdditionalSwapChain %ux%u fmt %u failed 0x%08lx", cs.pp.BackBufferWidth,
                      cs.pp.BackBufferHeight, cs.pp.BackBufferFormat, (unsigned long)hr);
    }
    out = qd3d_wr_raw(rp, sizeof accepted);
    if (!out)
        return QD3D_FAULT_REPLY;
    memcpy(out, &accepted, sizeof accepted);
    *ret = (uint32_t)hr;
    return QD3D_FAULT_NONE;
}

/* ------------------------------------------------------------------------ */
/* Cursor                                                                   */
/* ------------------------------------------------------------------------ */

static void qd3dx_cursor_unref(struct qd3dx_cursor *cur)
{
    if (cur && --cur->refs == 0u)
        free(cur);
}

void qd3dx_cursor_forget(struct qd3dx *ex)
{
    qd3dx_cursor_unref(ex->cursor);
    ex->cursor = NULL;
}

/* CURSOR: the session's cursor image, which PRESENT and CURSOR_POS place. */
uint32_t qd3dx_op_cursor(struct qd3dx *ex, const struct qd3d_cmd *c,
                         struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    const void *p, *px;
    struct qd3dx_cursor *cur = NULL;
    struct qd3d_cursor h;
    uint32_t n;

    p = qd3d_rd_raw(rd, sizeof(struct qd3d_cursor));
    if (!p || c->obj != 0u)
        return QD3D_FAULT_DECODE;
    memcpy(&h, p, sizeof h);
    if (h.width > QD3D_CURSOR_MAX || h.height > QD3D_CURSOR_MAX || !h.width != !h.height ||
        (h.width ? h.hot_x >= h.width || h.hot_y >= h.height : h.hot_x || h.hot_y))
        return QD3D_FAULT_DECODE;
    n = h.width * h.height * 4u;
    px = qd3d_rd_raw(rd, n);
    if (!px || !qd3d_rd_end(rd))
        return QD3D_FAULT_SIZE;
    if (n) {
        if (!ex->cursor_save)
            ex->cursor_save = malloc((size_t)QD3D_CURSOR_MAX * QD3D_CURSOR_MAX * 4u);
        cur = ex->cursor_save ? malloc(sizeof *cur + n) : NULL;
        if (cur) {
            cur->refs = 1;
            cur->width = h.width;
            cur->height = h.height;
            cur->hot_x = h.hot_x;
            cur->hot_y = h.hot_y;
            memcpy(cur->px, px, n);
        } else {
            qd3dx_warn(ex, "qd3dx: no memory for a %ux%u cursor", h.width, h.height);
        }
    }
    qd3dx_cursor_forget(ex);
    ex->cursor = cur;
    *ret = (uint32_t)D3D_OK;
    return QD3D_FAULT_NONE;
}

/* Floor of a / b for b > 0. */
static int64_t qd3dx_floordiv(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/* The session's cursor image over a fw x fh frame presented to the guest
 * screen rectangle screen, its hot spot at pos (guest screen) when on: the
 * image, referenced, and the frame pixel of its top-left corner; NULL when
 * there is none or nothing of it lands in the frame. */
static struct qd3dx_cursor *qd3dx_cursor_place(struct qd3dx *ex, const int32_t *screen, int on, const int32_t *pos,
                                               uint32_t fw, uint32_t fh, int32_t *x, int32_t *y)
{
    int64_t sw = (int64_t)screen[2] - screen[0], sh = (int64_t)screen[3] - screen[1];
    struct qd3dx_cursor *cur = ex->cursor;
    int64_t cx, cy;

    if (!on || !cur || sw <= 0 || sh <= 0)
        return NULL;
    cx = qd3dx_floordiv(((int64_t)pos[0] - screen[0]) * fw, sw) - cur->hot_x;
    cy = qd3dx_floordiv(((int64_t)pos[1] - screen[1]) * fh, sh) - cur->hot_y;
    if (cx >= (int64_t)fw || cy >= (int64_t)fh || cx + cur->width <= 0 || cy + cur->height <= 0)
        return NULL;
    *x = (int32_t)cx;
    *y = (int32_t)cy;
    cur->refs++;
    return cur;
}

/* Blends cur with its top-left at frame pixel (x, y), clipped to the
 * frame, after saving the pixels it covers (restore = 0), or puts those
 * pixels back (restore = 1): the frame hook sees the cursor, the front
 * buffer copy keeps none, as with a hardware cursor. The X byte of the
 * frame is left alone. */
static void qd3dx_cursor_apply(const struct qd3dx_frame *f, const struct qd3dx_cursor *cur, int32_t x, int32_t y,
                               uint32_t *save, int restore)
{
    uint32_t x0 = x < 0 ? (uint32_t)-x : 0u, y0 = y < 0 ? (uint32_t)-y : 0u;
    uint32_t x1 = cur->width, y1 = cur->height, i, j, n = 0;

    if ((int64_t)x + x1 > (int64_t)f->width)
        x1 = (uint32_t)((int64_t)f->width - x);
    if ((int64_t)y + y1 > (int64_t)f->height)
        y1 = (uint32_t)((int64_t)f->height - y);
    for (j = y0; j < y1; j++) {
        uint8_t *row = (uint8_t *)f->pixels + (size_t)(uint32_t)(y + (int32_t)j) * f->pitch;

        for (i = x0; i < x1; i++, n++) {
            uint32_t *dp = (uint32_t *)(row + (size_t)(uint32_t)(x + (int32_t)i) * 4u);
            uint32_t s = cur->px[j * cur->width + i], d = *dp, a = s >> 24, out;

            if (restore) {
                *dp = save[n];
                continue;
            }
            save[n] = d;
            if (a == 255u) {
                out = s & 0x00ffffffu;
            } else if (a == 0u) {
                out = (d ^ s) & 0x00ffffffu;
            } else {
                uint32_t k;

                out = 0;
                for (k = 0; k < 24u; k += 8u)
                    out |= ((((s >> k) & 0xffu) * a + ((d >> k) & 0xffu) * (255u - a) + 127u) / 255u) << k;
            }
            *dp = out | (d & 0xff000000u);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Repeats of the last published frame                                      */
/* ------------------------------------------------------------------------ */
/*
 * Between presents the last published frame goes to the frame hook again
 * from its system-memory copy (never read back again), with the cursor
 * where the guest reported it last drawn over it and taken out afterwards:
 *
 * - CURSOR_POS moves the cursor as a hardware cursor plane moves natively:
 *   a frame still in flight takes the new state along; otherwise the frame
 *   is due again (QD3DX_REPEAT_CURSOR) and goes out once
 *   QD3DX_CURSOR_REPUBLISH_US have passed since the last publication: a
 *   faster update stays pending, and the next CURSOR_POS or
 *   qd3dx_present_flush (the embedder's idle round, qd3dx_prepare_wait)
 *   shows the latest state. Nothing goes out when neither the last
 *   publication nor this one draws the cursor (it moves outside the frame
 *   or stays hidden). A PRESENT supersedes a pending update; an abandoned
 *   session drops it.
 * - The window of the frame's source minimized or hidden (PRESENT hidden,
 *   SOURCE_SHOWN) stops all of it (hooks.source_hidden) until the window
 *   shows again, which publishes the frame again (QD3DX_REPEAT_SHOWN), as
 *   qd3dx_show_again does on the embedder's request.
 */

/* 125 a second: a fresh cursor position for every refresh of a 60 to 120 Hz
 * display, and a bound on what a flood of updates costs. */
#define QD3DX_CURSOR_REPUBLISH_US 8000u

static uint64_t qd3dx_now_us(void)
{
    LARGE_INTEGER f, c;

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart / (uint64_t)f.QuadPart * 1000000u +
           (uint64_t)c.QuadPart % (uint64_t)f.QuadPart * 1000000u / (uint64_t)f.QuadPart;
}

/* The source a PRESENT or SOURCE_SHOWN names (e: a device, or a swap chain
 * of device state d) is that of the last published frame. */
static int qd3dx_is_shown(const struct qd3dx *ex, const struct qd3dx_ent *e, const struct qd3dx_dev *d)
{
    if (!ex->shown_dev)
        return 0;
    if (e->type == QD3D_T_IDirect3DSwapChain9 && !QD3D_ID_IS_HOST(e->id))
        return ex->shown_sc == (const void *)e->obj;
    return ex->shown_dev == d && !ex->shown_sc;
}

/* The window of the source of the last published frame was minimized or
 * hidden: the embedder shows the desktop. */
static void qd3dx_hide(struct qd3dx *ex)
{
    if (ex->shown_hidden)
        return;
    ex->shown_hidden = 1;
    ex->cursor_pending = 0;
    if (ex->hooks.source_hidden)
        ex->hooks.source_hidden(ex->hooks.user);
}

/* The last published frame again (nothing in flight), as repeat. */
static void qd3dx_republish(struct qd3dx *ex, uint32_t repeat)
{
    struct qd3dx_dev *d = ex->shown_dev;
    struct qd3dx_cursor *cur;
    IDirect3DSurface9 *s;
    D3DLOCKED_RECT lr;
    struct qd3dx_frame f;
    int32_t x = 0, y = 0;

    if (!d || d->front < 0 || ex->shown_hidden || !ex->hooks.frame || ex->release_request)
        return;
    f.width = d->copy_w[d->front];
    f.height = d->copy_h[d->front];
    f.repeat = repeat;
    cur = qd3dx_cursor_place(ex, d->front_screen, ex->cursor_on, ex->cursor_pos, f.width, f.height, &x, &y);
    if (repeat == QD3DX_REPEAT_CURSOR && !cur && !ex->cursor_drawn)
        return; /* no cursor over the frame before or now */
    s = d->copy[d->front];
    if (FAILED(s->lpVtbl->LockRect(s, &lr, NULL, 0))) {
        qd3dx_cursor_unref(cur);
        return;
    }
    f.pixels = lr.pBits;
    f.pitch = (uint32_t)lr.Pitch;
    if (cur)
        qd3dx_cursor_apply(&f, cur, x, y, ex->cursor_save, 0);
    if (!ex->release_request)
        ex->hooks.frame(ex->hooks.user, &f);
    if (cur)
        qd3dx_cursor_apply(&f, cur, x, y, ex->cursor_save, 1);
    s->lpVtbl->UnlockRect(s);
    ex->cursor_drawn = cur != NULL;
    ex->cursor_published = qd3dx_now_us();
    qd3dx_cursor_unref(cur);
}

/* A cursor update with nothing in flight: the frame again once due. */
static void qd3dx_cursor_due(struct qd3dx *ex)
{
    if (qd3dx_now_us() - ex->cursor_published < QD3DX_CURSOR_REPUBLISH_US)
        return;
    ex->cursor_pending = 0;
    qd3dx_republish(ex, QD3DX_REPEAT_CURSOR);
}

void qd3dx_show_again(struct qd3dx *ex)
{
    if (!ex->inflight_dev)
        qd3dx_republish(ex, QD3DX_REPEAT_SHOWN);
}

uint32_t qd3dx_op_cursor_pos(struct qd3dx *ex, const struct qd3d_cmd *c,
                             struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    const void *p = qd3d_rd_raw(rd, sizeof(struct qd3d_cursor_pos));
    struct qd3dx_dev *d = ex->inflight_dev;
    struct qd3d_cursor_pos cp;

    if (!p || !qd3d_rd_end(rd) || c->obj != 0u)
        return QD3D_FAULT_DECODE;
    memcpy(&cp, p, sizeof cp);
    if (cp.visible > 1u)
        return QD3D_FAULT_DECODE;
    ex->cursor_on = (int)cp.visible;
    memcpy(ex->cursor_pos, cp.pos, sizeof ex->cursor_pos);
    if (d) {
        qd3dx_cursor_unref(ex->inflight_cursor);
        ex->inflight_cursor = qd3dx_cursor_place(ex, ex->inflight_screen, ex->cursor_on, ex->cursor_pos,
                                                 d->copy_w[ex->inflight_copy], d->copy_h[ex->inflight_copy],
                                                 &ex->inflight_cx, &ex->inflight_cy);
    } else {
        ex->cursor_pending = 1;
        qd3dx_cursor_due(ex);
    }
    *ret = (uint32_t)D3D_OK;
    return QD3D_FAULT_NONE;
}

uint32_t qd3dx_op_source_shown(struct qd3dx *ex, const struct qd3d_cmd *c,
                               struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    const void *p = qd3d_rd_raw(rd, sizeof(struct qd3d_source_shown));
    struct qd3dx_ent *e = qd3dx_ent_get(ex, c->obj);
    struct qd3d_source_shown ss;

    if (!e || (e->type != QD3D_T_IDirect3DDevice9 &&
               (e->type != QD3D_T_IDirect3DSwapChain9 || QD3D_ID_IS_HOST(e->id))))
        return QD3D_FAULT_OBJECT;
    if (!p || !qd3d_rd_end(rd))
        return QD3D_FAULT_DECODE;
    memcpy(&ss, p, sizeof ss);
    if (ss.shown > 1u)
        return QD3D_FAULT_DECODE;
    /* What was presented before reaches the sink first. */
    qd3dx_present_flush(ex);
    if (qd3dx_is_shown(ex, e, e->dev)) {
        if (!ss.shown) {
            qd3dx_hide(ex);
        } else if (ex->shown_hidden) {
            ex->shown_hidden = 0;
            qd3dx_republish(ex, QD3DX_REPEAT_SHOWN);
        }
    }
    *ret = (uint32_t)D3D_OK;
    return QD3D_FAULT_NONE;
}

/* ------------------------------------------------------------------------ */
/* Present                                                                  */
/* ------------------------------------------------------------------------ */
/*
 * PRESENT resolves the presented part of the back buffer (its source
 * rectangle) into the device's X8R8G8B8 target and queues
 * GetRenderTargetData into a free system-memory copy; the driver performs
 * the copy while the executor goes on with the next frame. The frame is
 * published (frame hook, then frames_done) by the next PRESENT of any
 * device, by qd3dx_prepare_wait when the guest goes quiet, or before Reset,
 * GetFrontBufferData, the release of its device and GOODBYE: in present
 * order and at most one frame behind. A new session (HELLO, qd3dx_reset)
 * drops it. Publishing locks the copy, which waits only when the frame took
 * less time than its readback.
 *
 * Measured (RTX 4090, Windows 11, Direct3D 9Ex): GetRenderTargetData returns
 * at once and the copy completes in the background. No readiness test is
 * cheaper than that lock: an event query issued after the copy completes
 * only after D3DGETDATA_FLUSH, which itself blocks until the copy is done,
 * and LockRect ignores D3DLOCK_DONOTWAIT on a system-memory surface.
 *
 * Display policy: the VM shows one frame at a time, so of several swap
 * chains of a device the largest that presents is shown: a swap chain's
 * frame is published unless another swap chain of the device with frames
 * at least as large is shown and presented within QD3DX_SHOWN_HOLD_MS (the
 * shown one keeps a tie, one that stopped presenting yields after the
 * hold). A frame not shown is not read back; frames_done still counts it.
 */

_Static_assert(QD3DX_FRAME_COPIES >= 3, "front buffer, frame in flight and next frame");

#define QD3DX_SHOWN_HOLD_MS 250u

static int qd3dx_present_shown(struct qd3dx_dev *d, const void *sc, uint64_t area)
{
    uint64_t now = GetTickCount64();

    if (d->shown_sc && d->shown_sc != sc && now - d->shown_ms <= QD3DX_SHOWN_HOLD_MS && area <= d->shown_area)
        return 0;
    d->shown_sc = sc;
    d->shown_ms = now;
    d->shown_area = area;
    return 1;
}

/* Resolves the src part of bb and queues its readback into a copy of d that
 * is neither the front buffer nor in flight; *copy receives its index. */
static HRESULT qd3dx_present_issue(struct qd3dx *ex, struct qd3dx_dev *d, IDirect3DSurface9 *bb, const RECT *src,
                                   int *copy)
{
    IDirect3DDevice9Ex *dev = d->dev;
    uint32_t w = (uint32_t)(src->right - src->left), h = (uint32_t)(src->bottom - src->top);
    HRESULT hr;
    int i = 0;

    while (i == d->front || (ex->inflight_dev == d && i == ex->inflight_copy))
        i++;
    if (!d->present_rt || d->present_w != w || d->present_h != h) {
        if (d->present_rt)
            d->present_rt->lpVtbl->Release(d->present_rt);
        d->present_rt = NULL;
        d->present_w = d->present_h = 0;
        hr = dev->lpVtbl->CreateRenderTarget(dev, w, h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                             &d->present_rt, NULL);
        if (FAILED(hr)) {
            d->present_rt = NULL;
            return hr;
        }
        d->present_w = w;
        d->present_h = h;
    }
    if (!d->copy[i] || d->copy_w[i] != w || d->copy_h[i] != h) {
        if (d->copy[i])
            d->copy[i]->lpVtbl->Release(d->copy[i]);
        d->copy[i] = NULL;
        d->copy_w[i] = d->copy_h[i] = 0;
        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &d->copy[i],
                                                      NULL);
        if (FAILED(hr)) {
            d->copy[i] = NULL;
            return hr;
        }
        d->copy_w[i] = w;
        d->copy_h[i] = h;
    }
    hr = dev->lpVtbl->StretchRect(dev, bb, src, d->present_rt, NULL, D3DTEXF_NONE);
    if (SUCCEEDED(hr))
        hr = dev->lpVtbl->GetRenderTargetData(dev, d->present_rt, d->copy[i]);
    if (SUCCEEDED(hr))
        *copy = i;
    return hr;
}

void qd3dx_present_flush(struct qd3dx *ex)
{
    struct qd3dx_dev *d = ex->inflight_dev;
    struct qd3dx_cursor *cur = ex->inflight_cursor;
    IDirect3DSurface9 *s;
    D3DLOCKED_RECT lr;
    struct qd3dx_frame f;
    LARGE_INTEGER t0 = { .QuadPart = 0 }, t1;
    HRESULT hr;
    int i;

    if (ex->release_request) {
        qd3dx_present_drop(ex);
        return;
    }
    if (!d) {
        /* Nothing in flight: a pending cursor update shows the last frame
         * again once due. */
        if (ex->cursor_pending)
            qd3dx_cursor_due(ex);
        return;
    }
    i = ex->inflight_copy;
    s = d->copy[i];
    ex->inflight_dev = NULL;
    ex->inflight_cursor = NULL;
    if (ex->trace)
        QueryPerformanceCounter(&t0);
    /* Waits for the copy; the cursor is drawn into it and taken out again. */
    hr = s->lpVtbl->LockRect(s, &lr, NULL, cur && ex->hooks.frame ? 0u : (DWORD)D3DLOCK_READONLY);
    if (ex->trace) {
        uint64_t wait, lat;

        QueryPerformanceCounter(&t1);
        wait = (uint64_t)(t1.QuadPart - t0.QuadPart);
        lat = (uint64_t)t1.QuadPart - ex->trace_inflight_t;
        ex->trace_frames++;
        ex->trace_wait_sum += wait;
        ex->trace_lat_sum += lat;
        if (wait > ex->trace_wait_max)
            ex->trace_wait_max = wait;
        if (lat > ex->trace_lat_max)
            ex->trace_lat_max = lat;
    }
    if (SUCCEEDED(hr)) {
        /* A release can arrive while LockRect waits for the GPU. Do not
         * publish that abandoned frame; teardown follows this command. */
        if (!ex->release_request) {
            d->front = i;
            memcpy(d->front_screen, ex->inflight_screen, sizeof d->front_screen);
            ex->shown_dev = d;
            ex->shown_sc = ex->inflight_sc;
            ex->shown_hidden = 0;
            if (ex->hooks.frame) {
                f.pixels = lr.pBits;
                f.width = d->copy_w[i];
                f.height = d->copy_h[i];
                f.pitch = (uint32_t)lr.Pitch;
                f.repeat = QD3DX_REPEAT_NONE;
                if (cur)
                    qd3dx_cursor_apply(&f, cur, ex->inflight_cx, ex->inflight_cy, ex->cursor_save, 0);
                ex->hooks.frame(ex->hooks.user, &f);
                if (cur)
                    qd3dx_cursor_apply(&f, cur, ex->inflight_cx, ex->inflight_cy, ex->cursor_save, 1);
            }
        }
        s->lpVtbl->UnlockRect(s);
        /* It showed the latest cursor (CURSOR_POS moves the frame in flight's). */
        ex->cursor_pending = 0;
        ex->cursor_drawn = cur != NULL;
        ex->cursor_published = qd3dx_now_us();
    } else {
        qd3dx_warn(ex, "qd3dx: frame readback failed 0x%08lx", (unsigned long)hr);
    }
    qd3dx_cursor_unref(cur);
    MemoryBarrier();
    ex->ctrl->frames_done = ex->ctrl->frames_done + 1u;
}

void qd3dx_present_drop(struct qd3dx *ex)
{
    ex->cursor_pending = 0; /* an abandoned session shows nothing again */
    if (!ex->inflight_dev)
        return;
    ex->inflight_dev = NULL;
    qd3dx_cursor_unref(ex->inflight_cursor);
    ex->inflight_cursor = NULL;
    MemoryBarrier();
    ex->ctrl->frames_done = ex->ctrl->frames_done + 1u;
}

void qd3dx_dev_drop_present(struct qd3dx *ex, struct qd3dx_dev *d)
{
    int i;

    if (ex->inflight_dev == d)
        qd3dx_present_flush(ex);
    if (d->present_rt)
        d->present_rt->lpVtbl->Release(d->present_rt);
    d->present_rt = NULL;
    d->present_w = d->present_h = 0;
    for (i = 0; i < QD3DX_FRAME_COPIES; i++) {
        if (d->copy[i])
            d->copy[i]->lpVtbl->Release(d->copy[i]);
        d->copy[i] = NULL;
        d->copy_w[i] = d->copy_h[i] = 0;
    }
    d->front = -1;
}

/* The presented part of a w x h back buffer: the guest's source rectangle
 * when it lies inside, else all of it. */
static RECT qd3dx_present_src(const struct qd3d_present *pr, uint32_t w, uint32_t h)
{
    RECT r = { 0, 0, (LONG)w, (LONG)h };

    if (pr->has_src && pr->src[0] >= 0 && pr->src[1] >= 0 && pr->src[0] < pr->src[2] && pr->src[1] < pr->src[3] &&
        (uint32_t)pr->src[2] <= w && (uint32_t)pr->src[3] <= h) {
        r.left = pr->src[0];
        r.top = pr->src[1];
        r.right = pr->src[2];
        r.bottom = pr->src[3];
    }
    return r;
}

uint32_t qd3dx_op_present(struct qd3dx *ex, const struct qd3d_cmd *c,
                          struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    const void *p = qd3d_rd_raw(rd, sizeof(struct qd3d_present));
    struct qd3dx_ent *e = qd3dx_ent_get(ex, c->obj);
    struct qd3dx_cursor *cur = NULL;
    struct qd3d_present pr;
    IDirect3DSwapChain9 *sc = NULL;
    IDirect3DDevice9 *dev = NULL;
    IDirect3DSurface9 *bb = NULL;
    struct qd3dx_dev *d;
    LARGE_INTEGER issued = { .QuadPart = 0 };
    D3DSURFACE_DESC desc;
    int32_t cx = 0, cy = 0;
    int copy = -1, shown = 0;
    RECT src = { 0, 0, 0, 0 };
    HRESULT hr;

    if (!e || (e->type != QD3D_T_IDirect3DDevice9 && e->type != QD3D_T_IDirect3DSwapChain9))
        return QD3D_FAULT_OBJECT;
    if (!p || !qd3d_rd_end(rd))
        return QD3D_FAULT_DECODE;
    memcpy(&pr, p, sizeof pr);
    if (pr.has_src > 1u || pr.has_dst > 1u || pr.cursor > 1u || pr.hidden > 1u)
        return QD3D_FAULT_DECODE;

    if (e->type == QD3D_T_IDirect3DDevice9) {
        dev = (IDirect3DDevice9 *)e->obj;
        dev->lpVtbl->AddRef(dev);
        hr = dev->lpVtbl->GetSwapChain(dev, 0, &sc);
    } else {
        sc = (IDirect3DSwapChain9 *)e->obj;
        sc->lpVtbl->AddRef(sc);
        hr = sc->lpVtbl->GetDevice(sc, &dev);
    }
    if (SUCCEEDED(hr))
        hr = sc->lpVtbl->GetBackBuffer(sc, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (SUCCEEDED(hr))
        hr = bb->lpVtbl->GetDesc(bb, &desc);
    d = dev ? qd3dx_dev_find(ex, dev) : NULL;
    if (SUCCEEDED(hr) && !d)
        hr = D3DERR_INVALIDCALL;
    if (SUCCEEDED(hr)) {
        src = qd3dx_present_src(&pr, desc.Width, desc.Height);
        /* A frame whose window is minimized or hidden does not show. */
        shown = !pr.hidden &&
                qd3dx_present_shown(d, sc, (uint64_t)(src.right - src.left) * (uint64_t)(src.bottom - src.top));
    }
    if (shown)
        hr = qd3dx_present_issue(ex, d, bb, &src, &copy);
    if (shown && SUCCEEDED(hr) && ex->trace)
        QueryPerformanceCounter(&issued);
    if (FAILED(hr))
        qd3dx_warn(ex, "qd3dx: present on 0x%08x failed 0x%08lx", c->obj, (unsigned long)hr);
    if (bb)
        bb->lpVtbl->Release(bb);
    /* Native present keeps swap-chain semantics (buffer rotation, discard)
     * and gives the driver a frame boundary; the window is hidden. */
    if (sc) {
        sc->lpVtbl->Present(sc, NULL, NULL, NULL, NULL, 0);
        sc->lpVtbl->Release(sc);
    }
    if (dev)
        dev->lpVtbl->Release(dev);

    /* The PRESENT reports the guest's cursor: no older position is shown
     * after it. The previous frame's readback has normally finished by now. */
    ex->cursor_on = pr.cursor == 1u;
    memcpy(ex->cursor_pos, pr.cursor_pos, sizeof ex->cursor_pos);
    ex->cursor_pending = 0;
    qd3dx_present_flush(ex);
    if (shown && SUCCEEDED(hr)) {
        cur = qd3dx_cursor_place(ex, pr.screen, ex->cursor_on, pr.cursor_pos, (uint32_t)(src.right - src.left),
                                 (uint32_t)(src.bottom - src.top), &cx, &cy);
        ex->inflight_dev = d;
        ex->inflight_copy = copy;
        ex->inflight_cursor = cur;
        ex->inflight_cx = cx;
        ex->inflight_cy = cy;
        memcpy(ex->inflight_screen, pr.screen, sizeof ex->inflight_screen);
        /* Guest ids name additional swap chains; the implicit one (a host
         * id, or the device itself) ends only with its device. */
        ex->inflight_sc = e->type == QD3D_T_IDirect3DSwapChain9 && !QD3D_ID_IS_HOST(e->id) ? (const void *)e->obj
                                                                                         : NULL;
        if (ex->trace)
            ex->trace_inflight_t = (uint64_t)issued.QuadPart;
    } else {
        MemoryBarrier();
        ex->ctrl->frames_done = ex->ctrl->frames_done + 1u;
        if (pr.hidden && qd3dx_is_shown(ex, e, d))
            qd3dx_hide(ex);
    }
    *ret = (uint32_t)hr;
    return QD3D_FAULT_NONE;
}

/* ------------------------------------------------------------------------ */
/* Queries                                                                  */
/* ------------------------------------------------------------------------ */

uint32_t qd3dx_op_query_data(struct qd3dx *ex, const struct qd3d_cmd *c,
                             struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    IDirect3DQuery9 *q = (IDirect3DQuery9 *)qd3dx_obj(ex, c->obj, QD3D_T_IDirect3DQuery9);
    const void *p = qd3d_rd_raw(rd, sizeof(struct qd3d_query_data));
    struct qd3d_query_data qd;
    uint8_t buf[256];
    HRESULT hr;

    if (!q)
        return QD3D_FAULT_OBJECT;
    if (!p || !qd3d_rd_end(rd))
        return QD3D_FAULT_DECODE;
    memcpy(&qd, p, sizeof qd);
    if (qd.size > sizeof buf)
        return QD3D_FAULT_SIZE;
    if (qd.flags & ~(uint32_t)D3DGETDATA_FLUSH)
        return QD3D_FAULT_DECODE;
    /* The buffer holds the query's whole data (that of every Direct3D 9Ex
     * query type), so no dwSize the guest passes lets GetData write past it. */
    if (q->lpVtbl->GetDataSize(q) > sizeof buf) {
        *ret = (uint32_t)D3DERR_INVALIDCALL;
        return QD3D_FAULT_NONE;
    }
    memset(buf, 0, sizeof buf);
    hr = q->lpVtbl->GetData(q, qd.size ? buf : NULL, qd.size, qd.flags);
    if (hr == S_OK && qd.size) {
        void *out = qd3d_wr_raw(rp, qd.size);

        if (!out)
            return QD3D_FAULT_REPLY;
        memcpy(out, buf, qd.size);
    }
    *ret = (uint32_t)hr;
    return QD3D_FAULT_NONE;
}
