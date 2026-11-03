/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Executor internals shared by the core, the extension operations, the
 * transfer (UPLOAD/READBACK) code, the qd3dxo_* overrides and the self-test.
 * Not part of the embedding API (qd3dx.h).
 */
#ifndef QD3DX_PRIV_H
#define QD3DX_PRIV_H

#include "qd3dx_rt.h"
#include "qd3dx.h"

/* One live id. */
struct qd3dx_ent {
    uint32_t id;
    uint32_t type;      /* concrete enum qd3d_type */
    IUnknown *obj;      /* holds exactly one reference */
    struct qd3dx_dev *dev; /* device state for QD3D_T_IDirect3DDevice9 */
};

/* Open-addressing map from a nonzero 64-bit key to a pointer. */
struct qd3dx_map {
    uint64_t *keys;     /* 0 = empty slot */
    void **vals;
    uint32_t cap;       /* power of two */
    uint32_t count;
};

/* System-memory copies of presented frames per device: the published front
 * buffer, the frame whose readback is in flight, and the next frame. */
#define QD3DX_FRAME_COPIES 3

/* Texture stages SetTexture addresses: 16 pixel samplers, the displacement
 * map sampler and four vertex samplers. */
#define QD3DX_STAGES 21

/* A session's cursor image (QD3D_OP_CURSOR; pixel format in qd3d_proto.h),
 * shared with the frame in flight that draws it. */
struct qd3dx_cursor {
    uint32_t refs;
    uint32_t width, height, hot_x, hot_y;
    uint32_t px[];
};

/* Host state of one device. */
struct qd3dx_dev {
    struct qd3dx_dev *next;
    IDirect3DDevice9Ex *dev;   /* borrowed; the device entry owns the ref */
    HWND hwnd;
    /* Present readback: StretchRect resolves the back buffer into
     * present_rt (X8R8G8B8, no multisampling), GetRenderTargetData copies
     * that into one of the system-memory copies. */
    IDirect3DSurface9 *present_rt;
    uint32_t present_w, present_h;
    IDirect3DSurface9 *copy[QD3DX_FRAME_COPIES];
    uint32_t copy_w[QD3DX_FRAME_COPIES], copy_h[QD3DX_FRAME_COPIES];
    int front;                 /* copy holding the last published frame, -1: none */
    /* SYSTEMMEM textures the guest bound per stage (referenced): the device
     * samples their DEFAULT twin (the native binding, compared only) instead. */
    IDirect3DBaseTexture9 *sysmem_bound[QD3DX_STAGES];
    const void *stage_twin[QD3DX_STAGES];
    /* Caps that bound guest stream and render-target indices. */
    uint32_t max_streams, num_rts;
    /* Palette numbers the guest set (one bit each), allocated on first use:
     * the native runtime crashes reading or selecting one never set. */
    uint8_t *palettes;
    /* Light indices the guest enabled, disabled again by Reset. */
    uint32_t *lights;
    uint32_t nlights, cap_lights;
    /* D3DSBT_ALL state block of the pristine device, applied after Reset. */
    IDirect3DStateBlock9 *defaults;
    /* Bytes of DEFAULT-pool resources created through the executor, which
     * GetAvailableTextureMem subtracts (the native value of a large adapter
     * is saturated and never moves). */
    uint64_t vidmem;
    /* Guest screen rectangle of the published front frame (PRESENT). */
    int32_t front_screen[4];
    /* Swap chain whose frames are shown (display policy, qd3dx_ext.c):
     * compared only, never dereferenced; when it last presented (ms) and
     * the area of its frames. */
    const void *shown_sc;
    uint64_t shown_ms;
    uint64_t shown_area;
    /* Managed texture LOD (SetLOD, qd3dx_over.c): set once a texture of the
     * device got a nonzero LOD; then the LOD of the texture bound at each
     * stage (0: none) and the stages where it is nonzero. */
    int lod_used;
    uint32_t lod_mask;
    DWORD lod_val[QD3DX_STAGES];
    int recording;             /* BeginStateBlock..EndStateBlock */
};

/* Previous UPLOAD, for chunk continuity. */
struct qd3dx_upload_prev {
    int valid;
    uint32_t obj, kind, level, face;
    uint32_t x, y, z, w, h, d;
};

struct qd3dx {
    uint8_t *base;
    struct qd3d_ctrl *ctrl;
    uint8_t *ring;
    uint32_t ring_size;
    uint8_t *reply;
    uint32_t reply_size;
    struct qd3dx_hooks hooks;
    /* Cross-thread release mailbox: (1ull << 32) | captured head; 0 means
     * none. Naturally 8-byte aligned on the Windows x64 host: ordinary
     * volatile reads are atomic, exchange publishes/takes the whole cut. */
    volatile LONG64 release_request;

    /* Private copy of the current command (TOCTOU protection). */
    uint8_t *cmdbuf;
    uint32_t cmdbuf_size;
    /* Bump allocator for qd3dx_scratch, reset per command. */
    uint8_t *scratch;
    uint32_t scratch_size;
    uint32_t scratch_used;

    /* Direct3D, created by the first HELLO and kept across sessions. */
    HMODULE d3d9_dll;
    IDirect3D9Ex *d3d;
    ATOM wndclass;

    /* Number of the next host id (QD3D_ID_HOST_BIT | n), counted over the
     * executor's lifetime: proxies of a superseded session may still name
     * that session's ids, so a later session never gets them. */
    uint32_t next_host_id;

    /* Session. */
    uint32_t pid;
    uint32_t max_guest_id;
    struct qd3dx_map by_id;   /* id -> struct qd3dx_ent * */
    struct qd3dx_map by_ptr;  /* IUnknown * -> struct qd3dx_ent * */
    struct qd3dx_dev *devices;
    struct qd3dx_upload_prev upload_prev;
    /* The presented frame whose readback is in flight (at most one, of any
     * device): published by the next PRESENT or qd3dx_prepare_wait, with
     * the cursor image it draws (referenced, NULL: none) at its top-left
     * frame pixel, and its guest screen rectangle. */
    struct qd3dx_dev *inflight_dev;  /* NULL: none */
    int inflight_copy;
    struct qd3dx_cursor *inflight_cursor;
    int32_t inflight_cx, inflight_cy;
    int32_t inflight_screen[4];
    /* Source of the frame in flight beyond its device: the additional swap
     * chain it was presented through (compared only; NULL: the implicit
     * swap chain). Then the source of the last frame that reached the frame
     * hook (compared only; NULL device: none, or it was released), for
     * hooks.source_released, and whether its window is minimized or hidden
     * (PRESENT hidden, SOURCE_SHOWN): nothing is published again until it
     * shows (hooks.source_hidden). */
    const void *inflight_sc;
    struct qd3dx_dev *shown_dev;
    const void *shown_sc;
    int shown_hidden;
    /* The guest's cursor as last reported (PRESENT, CURSOR_POS): shown and
     * where (guest screen). The last published frame is due again with it
     * (cursor_pending) once QD3DX_CURSOR_REPUBLISH_US have passed since the
     * last publication (cursor_published, microseconds); cursor_drawn: that
     * publication drew the cursor. */
    int cursor_on;
    int32_t cursor_pos[2];
    int cursor_pending;
    int cursor_drawn;
    uint64_t cursor_published;
    /* Signs of life since qd3dx_alive last answered: a command executed,
     * or ctrl->heartbeat moved from heartbeat_seen. */
    int alive;
    uint32_t heartbeat_seen;
    /* The session's cursor image (referenced, NULL: none) and room for the
     * frame pixels under the cursor while the frame hook runs. */
    struct qd3dx_cursor *cursor;
    uint32_t *cursor_save;
    /* A managed texture of the session got a nonzero LOD: draws check the
     * stages of their device (qd3dx_over.c); 0 keeps the draw path as is. */
    int lod_used;

    /* Fault state. */
    int faulted;
    uint32_t log_budget;      /* rate limit for non-fault diagnostics */

    /* QD3DX_TRACE: commands per op in the current second, failures reported
     * per op, and the most recent ops (NULL / unused when off). */
    uint32_t *trace;
    uint8_t *trace_fails;
    uint64_t trace_t0;
    int trace_quiet;          /* "no commands" already reported */
    uint32_t trace_recent[16];
    uint32_t trace_recent_pos;
    /* QD3DX_TRACE frame statistics for the current second, in QPC ticks. */
    uint64_t trace_qpf;
    uint64_t trace_inflight_t;       /* when the frame in flight was presented */
    uint32_t trace_frames;
    uint64_t trace_lat_sum, trace_lat_max, trace_wait_sum, trace_wait_max;
};

/* Diagnostics through hooks.log (printf style, no trailing newline). */
void qd3dx_log(struct qd3dx *ex, const char *fmt, ...)
    __attribute__((format(gnu_printf, 2, 3)));
/* Rate-limited form for per-command native failures. */
void qd3dx_warn(struct qd3dx *ex, const char *fmt, ...)
    __attribute__((format(gnu_printf, 2, 3)));

/* Rate limit for per-command native failure warnings, per session. */
#define QD3DX_WARN_BUDGET 200u

/* Object table internals. */
struct qd3dx_ent *qd3dx_ent_get(struct qd3dx *ex, uint32_t id);
struct qd3dx_ent *qd3dx_ent_by_ptr(struct qd3dx *ex, const void *obj);
/* Releases the reference of one id and forgets it (device state too). */
void qd3dx_ent_release(struct qd3dx *ex, struct qd3dx_ent *e);
/* Releases every object of the session (children before parents). */
void qd3dx_session_release(struct qd3dx *ex);
/* Number of live ids (diagnostics and self-test). */
uint32_t qd3dx_live_objects(const struct qd3dx *ex);
/* Device state for a native device pointer, or NULL. */
struct qd3dx_dev *qd3dx_dev_find(struct qd3dx *ex, const void *dev);
void qd3dx_dev_free(struct qd3dx *ex, struct qd3dx_dev *d);

/* Present pipeline (qd3dx_ext.c). */
/* Publishes the frame in flight, if any: waits for its readback, calls the
 * frame hook and advances frames_done. */
void qd3dx_present_flush(struct qd3dx *ex);
/* Forgets the frame in flight without publishing it (the session is being
 * abandoned); frames_done still advances. */
void qd3dx_present_drop(struct qd3dx *ex);
/* Releases the present resources of a device that goes away, publishing its
 * frame in flight first. */
void qd3dx_dev_drop_present(struct qd3dx *ex, struct qd3dx_dev *d);
/* Drops the session's cursor image (HELLO, GOODBYE, reset, destroy). */
void qd3dx_cursor_forget(struct qd3dx *ex);
/* Device state the executor keeps (qd3dx_over.c). init reads the caps that
 * bound guest indices and captures the pristine D3DSBT_ALL state after
 * creation; reset restores the native D3D9 defaults after Reset (Direct3D
 * 9Ex keeps every state across ResetEx); drop releases it all before the
 * device goes away. */
void qd3dx_dev_init_state(struct qd3dx_dev *d);
void qd3dx_dev_reset_state(struct qd3dx_dev *d, UINT width, UINT height);
void qd3dx_dev_drop_state(struct qd3dx_dev *d);
/* Bytes of one level of fmt in DEFAULT-pool memory (qd3dx_xfer.c). */
uint64_t qd3dx_level_bytes(D3DFORMAT fmt, uint32_t width, uint32_t height);
/* Gives back the bytes a DEFAULT resource counted when it was created
 * (qd3dx_over.c); called before an id releases its object. */
void qd3dx_vidmem_release(struct qd3dx *ex, IUnknown *obj, uint32_t type);

/* Extension operations. Return QD3D_FAULT_* and set *ret on success. */
uint32_t qd3dx_op_hello(struct qd3dx *ex, const struct qd3d_cmd *c,
                        struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);
uint32_t qd3dx_op_goodbye(struct qd3dx *ex, const struct qd3d_cmd *c,
                          struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);
uint32_t qd3dx_op_release(struct qd3dx *ex, const struct qd3d_cmd *c,
                          struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);
uint32_t qd3dx_op_create_device(struct qd3dx *ex, const struct qd3d_cmd *c,
                                struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);
uint32_t qd3dx_op_reset(struct qd3dx *ex, const struct qd3d_cmd *c,
                        struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);
uint32_t qd3dx_op_present(struct qd3dx *ex, const struct qd3d_cmd *c,
                          struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);
uint32_t qd3dx_op_upload(struct qd3dx *ex, const struct qd3d_cmd *c,
                         struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);
uint32_t qd3dx_op_readback(struct qd3dx *ex, const struct qd3d_cmd *c,
                           struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);
uint32_t qd3dx_op_query_data(struct qd3dx *ex, const struct qd3d_cmd *c,
                             struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);
uint32_t qd3dx_op_cursor(struct qd3dx *ex, const struct qd3d_cmd *c,
                         struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);
uint32_t qd3dx_op_cursor_pos(struct qd3dx *ex, const struct qd3d_cmd *c,
                             struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);
uint32_t qd3dx_op_source_shown(struct qd3dx *ex, const struct qd3d_cmd *c,
                               struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);
uint32_t qd3dx_op_create_swapchain(struct qd3dx *ex, const struct qd3d_cmd *c,
                                   struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret);

/* Reads exactly n raw bytes; NULL (and err) when fewer remain. */
static inline const void *qd3d_rd_raw(struct qd3d_rd *r, uint32_t n)
{
    const void *p;

    if (r->err || r->left < n) {
        r->err = 1;
        return NULL;
    }
    p = r->p;
    r->p += n;
    r->left -= n;
    return p;
}

/* Reserves n raw reply bytes; NULL (and err) when they do not fit. */
static inline void *qd3d_wr_raw(struct qd3d_wr *w, uint32_t n)
{
    void *p;

    if (w->err || w->cap - w->used < n) {
        w->err = 1;
        return NULL;
    }
    p = w->p + w->used;
    w->used += n;
    return p;
}

/* Guest pool of a resource (private data tag set at creation); the native
 * pool when untagged. */
D3DPOOL qd3dx_guest_pool(IDirect3DResource9 *res, D3DPOOL native_pool);

#endif /* QD3DX_PRIV_H */
