/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * QD3D host executor core: channel initialisation, the command loop with
 * framing validation, reply publication, the fault path, idle/wake
 * handshake, and the id <-> object table.
 */
#include <initguid.h> /* defines the IID_IDirect3D* GUIDs once (selectany) */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define QD3D_DEFINE_OP_INFO
#include "qd3dx_priv.h"

/* ------------------------------------------------------------------------ */
/* Diagnostics                                                              */
/* ------------------------------------------------------------------------ */

static void qd3dx_vlog(struct qd3dx *ex, const char *fmt, va_list ap) __attribute__((format(gnu_printf, 2, 0)));
static void qd3dx_vlog(struct qd3dx *ex, const char *fmt, va_list ap)
{
    char line[512];

    if (!ex->hooks.log)
        return;
    vsnprintf(line, sizeof line, fmt, ap);
    ex->hooks.log(ex->hooks.user, line);
}

void qd3dx_log(struct qd3dx *ex, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    qd3dx_vlog(ex, fmt, ap);
    va_end(ap);
}

void qd3dx_warn(struct qd3dx *ex, const char *fmt, ...)
{
    va_list ap;

    if (ex->log_budget == 0u)
        return;
    if (--ex->log_budget == 0u) {
        qd3dx_log(ex, "qd3dx: further warnings suppressed for this session");
        return;
    }
    va_start(ap, fmt);
    qd3dx_vlog(ex, fmt, ap);
    va_end(ap);
}

static const char *qd3dx_fault_name(uint32_t code)
{
    switch (code) {
    case QD3D_FAULT_FRAMING:    return "FRAMING";
    case QD3D_FAULT_DECODE:     return "DECODE";
    case QD3D_FAULT_OBJECT:     return "OBJECT";
    case QD3D_FAULT_SIZE:       return "SIZE";
    case QD3D_FAULT_REPLY:      return "REPLY";
    case QD3D_FAULT_UNEXPECTED: return "UNEXPECTED";
    case QD3D_FAULT_ID:         return "ID";
    default:                    return "?";
    }
}

static const char *qd3dx_op_name(uint32_t op)
{
    static const char *const ext[] = {
        "PAD", "HELLO", "GOODBYE", "RELEASE", "CREATE_DEVICE", "RESET",
        "PRESENT", "UPLOAD", "READBACK", "QUERY_DATA", "CURSOR", "CREATE_SWAPCHAIN", "CURSOR_POS", "SOURCE_SHOWN",
    };

    if (op < sizeof ext / sizeof ext[0])
        return ext[op];
    if (op >= QD3D_OP_METHOD_BASE && op - QD3D_OP_METHOD_BASE < QD3D_OP_METHOD_COUNT)
        return qd3d_op_info[op - QD3D_OP_METHOD_BASE].name;
    return "unknown";
}

/* QD3DX_TRACE: once a second, every command of that second by count (busiest
 * first, eight per line), so a stalled or misrendering program shows what it
 * does, and the published frames with their delay after PRESENT and the time
 * spent waiting for their readback. */
static void qd3dx_trace_flush(struct qd3dx *ex)
{
    enum { TOP = 64, PER_LINE = 8 };
    const uint32_t n = QD3D_OP_METHOD_BASE + QD3D_OP_METHOD_COUNT;
    uint64_t now = GetTickCount64();
    uint32_t top[TOP], ntop = 0, total = 0, i, j;
    char line[512];
    int len = 0;

    if (now - ex->trace_t0 < 1000u)
        return;
    for (i = 0; i < n; i++) {
        uint32_t c = ex->trace[i];

        if (!c)
            continue;
        total += c;
        if (ntop < TOP)
            ntop++;
        else if (ex->trace[top[TOP - 1]] >= c)
            continue;
        for (j = ntop - 1; j > 0 && ex->trace[top[j - 1]] < c; j--)
            top[j] = top[j - 1];
        top[j] = i;
    }
    if (total) {
        qd3dx_log(ex, "qd3dx: %u commands in %.1f s", total, (double)(now - ex->trace_t0) / 1000.0);
        for (j = 0; j < ntop; j++) {
            const char *name = qd3dx_op_name(top[j]);

            if (!strncmp(name, "IDirect3DDevice9::", 18))
                name += 18;
            if (j % PER_LINE == 0)
                len = snprintf(line, sizeof line, "qd3dx:  ");
            if (len > 0 && (size_t)len < sizeof line)
                len += snprintf(line + len, sizeof line - (size_t)len, " %s %u", name, ex->trace[top[j]]);
            if (j % PER_LINE == PER_LINE - 1 || j + 1 == ntop)
                qd3dx_log(ex, "%s", line);
        }
        ex->trace_quiet = 0;
    } else if (!ex->trace_quiet) {
        qd3dx_log(ex, "qd3dx: no commands");
        ex->trace_quiet = 1;
    }
    if (ex->trace_frames) {
        double us = 1e6 / (double)ex->trace_qpf;

        qd3dx_log(ex, "qd3dx: %u frames, present to publish avg %.0f max %.0f us, readback wait avg %.0f max %.0f us",
                  ex->trace_frames, (double)ex->trace_lat_sum * us / ex->trace_frames, (double)ex->trace_lat_max * us,
                  (double)ex->trace_wait_sum * us / ex->trace_frames, (double)ex->trace_wait_max * us);
        ex->trace_frames = 0;
        ex->trace_lat_sum = ex->trace_lat_max = ex->trace_wait_sum = ex->trace_wait_max = 0;
    }
    memset(ex->trace, 0, n * sizeof *ex->trace);
    ex->trace_t0 = now;
}

/* ------------------------------------------------------------------------ */
/* Map                                                                      */
/* ------------------------------------------------------------------------ */

static uint32_t qd3dx_map_slot(const struct qd3dx_map *m, uint64_t key)
{
    uint64_t h = key;

    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (uint32_t)h & (m->cap - 1u);
}

static void *qd3dx_map_get(const struct qd3dx_map *m, uint64_t key)
{
    uint32_t i;

    if (!m->cap || !key)
        return NULL;
    for (i = qd3dx_map_slot(m, key); m->keys[i]; i = (i + 1u) & (m->cap - 1u))
        if (m->keys[i] == key)
            return m->vals[i];
    return NULL;
}

static int qd3dx_map_grow(struct qd3dx_map *m)
{
    struct qd3dx_map n;
    uint32_t i;

    n.cap = m->cap ? m->cap * 2u : 256u;
    if (n.cap < m->cap)
        return 0;
    n.count = 0;
    n.keys = calloc(n.cap, sizeof *n.keys);
    n.vals = calloc(n.cap, sizeof *n.vals);
    if (!n.keys || !n.vals) {
        free(n.keys);
        free(n.vals);
        return 0;
    }
    for (i = 0; i < m->cap; i++) {
        uint32_t j;

        if (!m->keys[i])
            continue;
        for (j = qd3dx_map_slot(&n, m->keys[i]); n.keys[j]; j = (j + 1u) & (n.cap - 1u))
            ;
        n.keys[j] = m->keys[i];
        n.vals[j] = m->vals[i];
        n.count++;
    }
    free(m->keys);
    free(m->vals);
    *m = n;
    return 1;
}

static int qd3dx_map_put(struct qd3dx_map *m, uint64_t key, void *val)
{
    uint32_t i;

    if (!key)
        return 0;
    if ((uint64_t)(m->count + 1u) * 4u > (uint64_t)m->cap * 3u && !qd3dx_map_grow(m))
        return 0;
    for (i = qd3dx_map_slot(m, key); m->keys[i]; i = (i + 1u) & (m->cap - 1u)) {
        if (m->keys[i] == key) {
            m->vals[i] = val;
            return 1;
        }
    }
    m->keys[i] = key;
    m->vals[i] = val;
    m->count++;
    return 1;
}

/* Linear probing with backward-shift deletion (no tombstones). */
static void qd3dx_map_del(struct qd3dx_map *m, uint64_t key)
{
    uint32_t mask, i, j;

    if (!m->cap || !key)
        return;
    mask = m->cap - 1u;
    for (i = qd3dx_map_slot(m, key); m->keys[i] != key; i = (i + 1u) & mask)
        if (!m->keys[i])
            return;
    m->keys[i] = 0;
    m->vals[i] = NULL;
    m->count--;
    for (j = (i + 1u) & mask; m->keys[j]; j = (j + 1u) & mask) {
        uint32_t home = qd3dx_map_slot(m, m->keys[j]);

        /* Move j into the hole at i unless its home lies cyclically in (i, j]. */
        if (((j - home) & mask) >= ((j - i) & mask)) {
            m->keys[i] = m->keys[j];
            m->vals[i] = m->vals[j];
            m->keys[j] = 0;
            m->vals[j] = NULL;
            i = j;
        }
    }
}

static void qd3dx_map_free(struct qd3dx_map *m)
{
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

/* ------------------------------------------------------------------------ */
/* Object table                                                             */
/* ------------------------------------------------------------------------ */

struct qd3dx_ent *qd3dx_ent_get(struct qd3dx *ex, uint32_t id)
{
    return id ? (struct qd3dx_ent *)qd3dx_map_get(&ex->by_id, id) : NULL;
}

struct qd3dx_ent *qd3dx_ent_by_ptr(struct qd3dx *ex, const void *obj)
{
    return obj ? (struct qd3dx_ent *)qd3dx_map_get(&ex->by_ptr, (uint64_t)(uintptr_t)obj) : NULL;
}

IUnknown *qd3dx_obj(struct qd3dx *ex, uint32_t id, uint32_t type)
{
    struct qd3dx_ent *e = qd3dx_ent_get(ex, id);

    if (!e || !qd3d_type_isa(e->type, type))
        return NULL;
    return e->obj;
}

int qd3dx_id_is_fresh(struct qd3dx *ex, uint32_t id)
{
    return id != 0u && !QD3D_ID_IS_HOST(id) && id > ex->max_guest_id;
}

static struct qd3dx_ent *qd3dx_ent_add(struct qd3dx *ex, uint32_t id, IUnknown *obj, uint32_t type)
{
    struct qd3dx_ent *e = calloc(1, sizeof *e);

    if (!e)
        return NULL;
    e->id = id;
    e->type = type;
    e->obj = obj;
    if (!qd3dx_map_put(&ex->by_id, id, e)) {
        free(e);
        return NULL;
    }
    /* A native object bound under several ids (a runtime that returns a
     * cached object from a Create call) maps to the most recent id. */
    if (!qd3dx_map_put(&ex->by_ptr, (uint64_t)(uintptr_t)obj, e)) {
        qd3dx_map_del(&ex->by_id, id);
        free(e);
        return NULL;
    }
    return e;
}

int qd3dx_bind(struct qd3dx *ex, uint32_t id, IUnknown *obj, uint32_t type)
{
    if (!obj || !qd3dx_id_is_fresh(ex, id) || type >= QD3D_T_COUNT || !qd3d_type_concrete[type])
        return 0;
    if (!qd3dx_ent_add(ex, id, obj, type))
        return 0;
    ex->max_guest_id = id;
    return 1;
}

static uint32_t qd3dx_rtype_to_type(D3DRESOURCETYPE t)
{
    switch (t) {
    case D3DRTYPE_SURFACE:       return QD3D_T_IDirect3DSurface9;
    case D3DRTYPE_VOLUME:        return QD3D_T_IDirect3DVolume9;
    case D3DRTYPE_TEXTURE:       return QD3D_T_IDirect3DTexture9;
    case D3DRTYPE_VOLUMETEXTURE: return QD3D_T_IDirect3DVolumeTexture9;
    case D3DRTYPE_CUBETEXTURE:   return QD3D_T_IDirect3DCubeTexture9;
    case D3DRTYPE_VERTEXBUFFER:  return QD3D_T_IDirect3DVertexBuffer9;
    case D3DRTYPE_INDEXBUFFER:   return QD3D_T_IDirect3DIndexBuffer9;
    default:                     return QD3D_T_NONE;
    }
}

/* Concrete type of obj, which the runtime returned for declared type. */
static uint32_t qd3dx_concrete_type(IUnknown *obj, uint32_t declared)
{
    static const struct { const IID *iid; uint32_t type; } probe[] = {
        { &IID_IDirect3DSurface9, QD3D_T_IDirect3DSurface9 },
        { &IID_IDirect3DTexture9, QD3D_T_IDirect3DTexture9 },
        { &IID_IDirect3DCubeTexture9, QD3D_T_IDirect3DCubeTexture9 },
        { &IID_IDirect3DVolumeTexture9, QD3D_T_IDirect3DVolumeTexture9 },
        { &IID_IDirect3DVertexBuffer9, QD3D_T_IDirect3DVertexBuffer9 },
        { &IID_IDirect3DIndexBuffer9, QD3D_T_IDirect3DIndexBuffer9 },
        { &IID_IDirect3DVolume9, QD3D_T_IDirect3DVolume9 },
        { &IID_IDirect3DSwapChain9, QD3D_T_IDirect3DSwapChain9 },
        { &IID_IDirect3DDevice9, QD3D_T_IDirect3DDevice9 },
        { &IID_IDirect3D9, QD3D_T_IDirect3D9 },
        { &IID_IDirect3DStateBlock9, QD3D_T_IDirect3DStateBlock9 },
        { &IID_IDirect3DVertexDeclaration9, QD3D_T_IDirect3DVertexDeclaration9 },
        { &IID_IDirect3DVertexShader9, QD3D_T_IDirect3DVertexShader9 },
        { &IID_IDirect3DPixelShader9, QD3D_T_IDirect3DPixelShader9 },
        { &IID_IDirect3DQuery9, QD3D_T_IDirect3DQuery9 },
    };
    size_t i;

    if (declared < QD3D_T_COUNT && qd3d_type_concrete[declared])
        return declared;
    if (qd3d_type_isa(declared, QD3D_T_IDirect3DResource9)) {
        IDirect3DResource9 *r = (IDirect3DResource9 *)obj;
        uint32_t t = qd3dx_rtype_to_type(r->lpVtbl->GetType(r));

        return qd3d_type_isa(t, declared) ? t : QD3D_T_NONE;
    }
    for (i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        void *p = NULL;

        if (SUCCEEDED(obj->lpVtbl->QueryInterface(obj, probe[i].iid, &p)) && p) {
            ((IUnknown *)p)->lpVtbl->Release((IUnknown *)p);
            if (p == (void *)obj && qd3d_type_isa(probe[i].type, declared))
                return probe[i].type;
        }
    }
    return QD3D_T_NONE;
}

/* The next free host id. The counter runs over the executor's lifetime, so
 * ids of earlier sessions come back only after 2^31 - 1 more host ids, and
 * then never while still live. Returns 0 when every id is live. */
static uint32_t qd3dx_host_id_next(struct qd3dx *ex)
{
    uint32_t tries, id;

    for (tries = 0; tries <= ex->by_id.count; tries++) {
        id = QD3D_ID_HOST_BIT | ex->next_host_id;
        ex->next_host_id = ex->next_host_id >= 0x7fffffffu ? 1u : ex->next_host_id + 1u;
        if (!qd3dx_map_get(&ex->by_id, id))
            return id;
    }
    return 0u;
}

uint32_t qd3dx_out_id(struct qd3dx *ex, IUnknown *obj, uint32_t declared_type, uint32_t *type)
{
    struct qd3dx_ent *e;
    uint32_t t, id = 0u;

    *type = QD3D_T_NONE;
    if (!obj)
        return 0u;
    e = qd3dx_ent_by_ptr(ex, obj);
    if (e) {
        obj->lpVtbl->Release(obj); /* the id already holds its reference */
        *type = e->type;
        return e->id;
    }
    t = qd3dx_concrete_type(obj, declared_type);
    if (t != QD3D_T_NONE)
        id = qd3dx_host_id_next(ex);
    if (!id) {
        qd3dx_warn(ex, "qd3dx: cannot name object returned as %u", declared_type);
        obj->lpVtbl->Release(obj);
        return 0u;
    }
    if (!qd3dx_ent_add(ex, id, obj, t)) {
        obj->lpVtbl->Release(obj);
        return 0u;
    }
    *type = t;
    return id;
}

void *qd3dx_scratch(struct qd3dx *ex, uint64_t bytes)
{
    uint64_t start = (ex->scratch_used + 15u) & ~(uint64_t)15u;
    void *p;

    if (bytes > ex->scratch_size || start + bytes > ex->scratch_size)
        return NULL;
    p = ex->scratch + start;
    ex->scratch_used = (uint32_t)(start + bytes);
    return p;
}

struct qd3dx_dev *qd3dx_dev_find(struct qd3dx *ex, const void *dev)
{
    struct qd3dx_dev *d;

    for (d = ex->devices; d; d = d->next)
        if ((const void *)d->dev == dev)
            return d;
    return NULL;
}

void qd3dx_dev_free(struct qd3dx *ex, struct qd3dx_dev *d)
{
    struct qd3dx_dev **pp;

    for (pp = &ex->devices; *pp; pp = &(*pp)->next) {
        if (*pp == d) {
            *pp = d->next;
            break;
        }
    }
    if (d->hwnd)
        DestroyWindow(d->hwnd);
    free(d);
}

/* A frame source (a device, or an additional swap chain: a guest id) going
 * away: the frames presented before reach the sink first, the display
 * policy forgets the swap chain, and when the source of the last published
 * frame is the one released, the embedder learns that no more frames can
 * come from it (hooks.source_released, after the release). */
static int qd3dx_source_release(struct qd3dx *ex, struct qd3dx_ent *e)
{
    int sc = e->type == QD3D_T_IDirect3DSwapChain9 && !QD3D_ID_IS_HOST(e->id);
    int shown;

    if (!e->dev && !sc)
        return 0;
    qd3dx_present_flush(ex);
    if (sc) {
        IDirect3DSwapChain9 *s = (IDirect3DSwapChain9 *)e->obj;
        IDirect3DDevice9 *dev = NULL;
        struct qd3dx_dev *d;

        if (SUCCEEDED(s->lpVtbl->GetDevice(s, &dev)) && dev) {
            d = qd3dx_dev_find(ex, dev);
            if (d && d->shown_sc == (const void *)s)
                d->shown_sc = NULL;
            dev->lpVtbl->Release(dev);
        }
    }
    shown = ex->shown_dev && (sc ? ex->shown_sc == (const void *)e->obj : ex->shown_dev == e->dev);
    if (shown) {
        ex->shown_dev = NULL;
        ex->shown_sc = NULL;
        ex->shown_hidden = 0;
    }
    return shown;
}

void qd3dx_ent_release(struct qd3dx *ex, struct qd3dx_ent *e)
{
    struct qd3dx_dev *d = e->dev;
    int shown = qd3dx_source_release(ex, e);

    qd3dx_map_del(&ex->by_id, e->id);
    if (qd3dx_map_get(&ex->by_ptr, (uint64_t)(uintptr_t)e->obj) == e)
        qd3dx_map_del(&ex->by_ptr, (uint64_t)(uintptr_t)e->obj);
    if (d) {
        qd3dx_dev_drop_present(ex, d);
        qd3dx_dev_drop_state(d);
    }
    qd3dx_vidmem_release(ex, e->obj, e->type);
    e->obj->lpVtbl->Release(e->obj);
    if (d)
        qd3dx_dev_free(ex, d); /* window outlives the device reference */
    if (ex->upload_prev.valid && ex->upload_prev.obj == e->id)
        ex->upload_prev.valid = 0;
    free(e);
    if (shown && ex->hooks.source_released)
        ex->hooks.source_released(ex->hooks.user);
}

/* Release rank: children before parents. */
static int qd3dx_rank(uint32_t type)
{
    switch (type) {
    case QD3D_T_IDirect3DSurface9:
    case QD3D_T_IDirect3DVolume9:   return 0;
    case QD3D_T_IDirect3DSwapChain9: return 2;
    case QD3D_T_IDirect3DDevice9:   return 3;
    case QD3D_T_IDirect3D9:         return 4;
    default:                        return 1;
    }
}

void qd3dx_session_release(struct qd3dx *ex)
{
    uint32_t n = 0, i, k;
    struct qd3dx_ent **all;
    int rank;

    if (ex->by_id.count) {
        all = malloc((size_t)ex->by_id.count * sizeof *all);
        if (all) {
            /* Snapshot first: deletions reshuffle the map's slots. */
            for (i = 0; i < ex->by_id.cap; i++)
                if (ex->by_id.keys[i])
                    all[n++] = ex->by_id.vals[i];
            /* A released entry is freed: later rank passes skip its slot. */
            for (rank = 0; rank <= 4; rank++)
                for (k = 0; k < n; k++)
                    if (all[k] && qd3dx_rank(all[k]->type) == rank) {
                        qd3dx_ent_release(ex, all[k]);
                        all[k] = NULL;
                    }
            free(all);
        } else {
            /* Out of memory: release in slot order, still complete. */
            while (ex->by_id.count)
                for (i = 0; i < ex->by_id.cap; i++)
                    if (ex->by_id.keys[i]) {
                        qd3dx_ent_release(ex, ex->by_id.vals[i]);
                        break;
                    }
        }
    }
    ex->max_guest_id = 0;
    ex->upload_prev.valid = 0;
    qd3dx_cursor_forget(ex);
    ex->lod_used = 0;
}

uint32_t qd3dx_live_objects(const struct qd3dx *ex)
{
    return ex->by_id.count;
}

/* ------------------------------------------------------------------------ */
/* Channel                                                                  */
/* ------------------------------------------------------------------------ */

static int qd3dx_apply_release(struct qd3dx *ex);

static void qd3dx_fault(struct qd3dx *ex, const struct qd3d_cmd *c, uint32_t code)
{
    struct qd3d_ctrl *ctrl = ex->ctrl;
    uint32_t op = c ? c->op : 0xffffffffu;

    ctrl->fault_op = op;
    ctrl->fault_code = code;
    MemoryBarrier();
    ctrl->host_state = QD3D_HOST_FAULT;
    ex->faulted = 1;
    if (c)
        qd3dx_log(ex, "qd3dx: FAULT %s (%u) on op %u %s obj 0x%08x seq %u size %u at tail 0x%08x",
                  qd3dx_fault_name(code), code, op, qd3dx_op_name(op), c->obj, c->seq, c->size,
                  ctrl->tail);
    else
        qd3dx_log(ex, "qd3dx: FAULT %s (%u): ring cursors head 0x%08x tail 0x%08x",
                  qd3dx_fault_name(code), code, ctrl->head, ctrl->tail);
}

typedef uint32_t (*qd3dx_ext_fn)(struct qd3dx *, const struct qd3d_cmd *, struct qd3d_rd *,
                                 struct qd3d_wr *, uint32_t *);

static const struct {
    qd3dx_ext_fn fn;
    int sync;
} qd3dx_ext_ops[] = {
    [QD3D_OP_HELLO] = { qd3dx_op_hello, 1 },
    [QD3D_OP_GOODBYE] = { qd3dx_op_goodbye, 0 },
    [QD3D_OP_RELEASE] = { qd3dx_op_release, 0 },
    [QD3D_OP_CREATE_DEVICE] = { qd3dx_op_create_device, 1 },
    [QD3D_OP_RESET] = { qd3dx_op_reset, 1 },
    [QD3D_OP_PRESENT] = { qd3dx_op_present, 0 },
    [QD3D_OP_UPLOAD] = { qd3dx_op_upload, 0 },
    [QD3D_OP_READBACK] = { qd3dx_op_readback, 1 },
    [QD3D_OP_QUERY_DATA] = { qd3dx_op_query_data, 1 },
    [QD3D_OP_CURSOR] = { qd3dx_op_cursor, 0 },
    [QD3D_OP_CREATE_SWAPCHAIN] = { qd3dx_op_create_swapchain, 1 },
    [QD3D_OP_CURSOR_POS] = { qd3dx_op_cursor_pos, 0 },
    [QD3D_OP_SOURCE_SHOWN] = { qd3dx_op_source_shown, 0 },
};

/* Commands whose bulk payload is consumed in place instead of copied. Every
 * length they use is read once from the ring; concurrent guest writes can
 * only change the bytes copied, never a bound. DrawIndexedPrimitiveUP is not
 * among them: its indices are validated against the vertex data, so they
 * must not change after the check. */
static int qd3dx_in_place(uint32_t op)
{
    return op == QD3D_OP_UPLOAD || op == QD3D_OP_IDirect3DDevice9_DrawPrimitiveUP;
}

/* QD3DX_TRACE bookkeeping for one executed command: per-op counts, the first
 * failures of each op with their leading argument words, and for the very
 * first failure the commands that led up to it. */
static void qd3dx_trace_cmd(struct qd3dx *ex, const struct qd3d_cmd *c, uint32_t ret, const uint32_t *argw,
                            uint32_t nargw)
{
    const uint32_t n = sizeof ex->trace_recent / sizeof ex->trace_recent[0];
    uint32_t i;

    ex->trace[c->op]++;
    if (FAILED((HRESULT)ret) && ex->trace_fails[c->op] < 3u) {
        char args[96], line[512];
        int len = 0;

        args[0] = 0;
        for (i = 0; i < nargw && len >= 0 && (size_t)len < sizeof args; i++)
            len += snprintf(args + len, sizeof args - (size_t)len, " 0x%x", argw[i]);
        qd3dx_log(ex, "qd3dx: %s on 0x%08x returned 0x%08x; args%s", qd3dx_op_name(c->op), c->obj, ret, args);
        if (!ex->trace_fails[c->op]++) {
            len = snprintf(line, sizeof line, "qd3dx:   after");
            for (i = 0; i < n && len > 0 && (size_t)len < sizeof line; i++) {
                uint32_t op = ex->trace_recent[(ex->trace_recent_pos + i) % n];
                const char *name;

                if (!op)
                    continue;
                name = qd3dx_op_name(op);
                if (!strncmp(name, "IDirect3DDevice9::", 18))
                    name += 18;
                len += snprintf(line + len, sizeof line - (size_t)len, " %s", name);
            }
            qd3dx_log(ex, "%s", line);
        }
    }
    ex->trace_recent[ex->trace_recent_pos++ % n] = c->op;
}

/* Executes the command at tail. Returns 1 when executed, 0 when the ring is
 * empty, -1 on a fault. */
static int qd3dx_step(struct qd3dx *ex)
{
    struct qd3d_ctrl *ctrl = ex->ctrl;
    uint32_t head, tail, avail, off, space, stride, fault, ret = 0;
    uint32_t argw[6], nargw = 0;
    struct qd3d_cmd c;
    struct qd3d_rd rd;
    struct qd3d_wr wr;
    const uint8_t *src;

    head = ctrl->head;
    MemoryBarrier();
    /* Read head BEFORE checking release: if the next owner's head is
     * visible, the request published before its claim is visible too.
     * A request arriving later can only race this old owner's command. */
    if (qd3dx_apply_release(ex))
        return 0;
    tail = ctrl->tail;
    avail = head - tail;
    if (avail == 0u)
        return 0;
    if (avail > ex->ring_size || (avail & (QD3D_CMD_ALIGN - 1u)) || (tail & (QD3D_CMD_ALIGN - 1u))) {
        qd3dx_fault(ex, NULL, QD3D_FAULT_FRAMING);
        return -1;
    }
    off = tail & (ex->ring_size - 1u);
    space = ex->ring_size - off;
    src = ex->ring + off;
    memcpy(&c, src, sizeof c);

    if (c.size < sizeof c) {
        qd3dx_fault(ex, &c, QD3D_FAULT_FRAMING);
        return -1;
    }
    if (c.op == QD3D_OP_PAD) {
        if (c.size != space || space > avail) {
            qd3dx_fault(ex, &c, QD3D_FAULT_FRAMING);
            return -1;
        }
        MemoryBarrier();
        ctrl->tail = tail + space;
        return 1;
    }
    if (c.size > ex->ring_size / 2u) {
        qd3dx_fault(ex, &c, QD3D_FAULT_FRAMING);
        return -1;
    }
    stride = QD3D_CMD_STRIDE(c.size);
    if (stride > space || stride > avail) {
        qd3dx_fault(ex, &c, QD3D_FAULT_FRAMING);
        return -1;
    }

    rd.left = c.size - (uint32_t)sizeof c;
    rd.err = 0;
    if (qd3dx_in_place(c.op)) {
        rd.p = src + sizeof c;
    } else {
        memcpy(ex->cmdbuf, src + sizeof c, rd.left);
        rd.p = ex->cmdbuf;
    }
    if (ex->trace) {
        nargw = rd.left / 4u < 6u ? rd.left / 4u : 6u;
        memcpy(argw, rd.p, nargw * 4u);
    }
    wr.p = ex->reply + sizeof(struct qd3d_reply);
    wr.cap = ex->reply_size - (uint32_t)sizeof(struct qd3d_reply);
    wr.used = 0;
    wr.err = 0;
    ex->scratch_used = 0;

    if (c.op < QD3D_OP_METHOD_BASE) {
        if (c.op >= sizeof qd3dx_ext_ops / sizeof qd3dx_ext_ops[0] || !qd3dx_ext_ops[c.op].fn)
            fault = QD3D_FAULT_UNEXPECTED;
        else if (qd3dx_ext_ops[c.op].sync && c.seq == 0u)
            fault = QD3D_FAULT_FRAMING;
        else
            fault = qd3dx_ext_ops[c.op].fn(ex, &c, &rd, &wr, &ret);
    } else if (c.op - QD3D_OP_METHOD_BASE >= QD3D_OP_METHOD_COUNT) {
        fault = QD3D_FAULT_FRAMING;
    } else if (!qd3dx_methods[c.op - QD3D_OP_METHOD_BASE]) {
        fault = QD3D_FAULT_UNEXPECTED;
    } else {
        fault = qd3dx_methods[c.op - QD3D_OP_METHOD_BASE](ex, &c, &rd, &wr, &ret);
    }
    if (fault != QD3D_FAULT_NONE) {
        qd3dx_fault(ex, &c, fault);
        return -1;
    }
    if (c.op != QD3D_OP_UPLOAD)
        ex->upload_prev.valid = 0;

    if (c.seq != 0u) {
        struct qd3d_reply r;

        r.seq = c.seq;
        r.ret = ret;
        r.size = wr.used;
        r.reserved = 0;
        memcpy(ex->reply, &r, sizeof r);
        MemoryBarrier();
        ctrl->sync_done = c.seq;
    }
    MemoryBarrier();
    ctrl->tail = tail + stride;
    if (ex->trace)
        qd3dx_trace_cmd(ex, &c, ret, argw, nargw);
    return 1;
}

static void qd3dx_pump(struct qd3dx *ex)
{
    struct qd3dx_dev *d;

    for (d = ex->devices; d; d = d->next) {
        MSG msg;
        int n = 0;

        while (n++ < 64 && PeekMessageW(&msg, d->hwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

struct qd3dx *qd3dx_create(void *base, uint32_t size, const struct qd3dx_hooks *hooks)
{
    struct qd3dx *ex;
    struct qd3d_ctrl *ctrl;

    if (!base || size < QD3D_DEFAULT_SIZE)
        return NULL;
    ex = calloc(1, sizeof *ex);
    if (!ex)
        return NULL;
    ex->base = base;
    ex->ctrl = ctrl = (struct qd3d_ctrl *)base;
    ex->ring_size = QD3D_DEFAULT_RING;
    ex->ring = ex->base + QD3D_CTRL_SIZE;
    ex->reply_size = QD3D_DEFAULT_REPLY;
    ex->reply = ex->base + QD3D_CTRL_SIZE + QD3D_DEFAULT_RING;
    if (hooks)
        ex->hooks = *hooks;
    ex->cmdbuf_size = ex->ring_size / 2u;
    ex->cmdbuf = malloc(ex->cmdbuf_size);
    ex->scratch_size = ex->reply_size - (uint32_t)sizeof(struct qd3d_reply);
    ex->scratch = malloc(ex->scratch_size);
    if (!ex->cmdbuf || !ex->scratch) {
        free(ex->cmdbuf);
        free(ex->scratch);
        free(ex);
        return NULL;
    }
    ex->next_host_id = 1;
    ex->log_budget = QD3DX_WARN_BUDGET;
    if (getenv("QD3DX_TRACE")) {
        ex->trace = calloc(QD3D_OP_METHOD_BASE + QD3D_OP_METHOD_COUNT, sizeof *ex->trace);
        ex->trace_fails = calloc(QD3D_OP_METHOD_BASE + QD3D_OP_METHOD_COUNT, 1);
        if (!ex->trace || !ex->trace_fails) {
            free(ex->trace);
            free(ex->trace_fails);
            ex->trace = NULL;
            ex->trace_fails = NULL;
        }
        ex->trace_t0 = GetTickCount64();
        {
            LARGE_INTEGER f;

            QueryPerformanceFrequency(&f);
            ex->trace_qpf = (uint64_t)f.QuadPart;
        }
    }

    ctrl->magic = 0;
    MemoryBarrier();
    memset((uint8_t *)ctrl + sizeof ctrl->magic, 0, QD3D_CTRL_SIZE - sizeof ctrl->magic);
    ctrl->abi = QD3D_ABI_VERSION;
    ctrl->total_size = QD3D_DEFAULT_SIZE;
    ctrl->ring_offset = QD3D_CTRL_SIZE;
    ctrl->ring_size = QD3D_DEFAULT_RING;
    ctrl->reply_offset = QD3D_CTRL_SIZE + QD3D_DEFAULT_RING;
    ctrl->reply_size = QD3D_DEFAULT_REPLY;
    ctrl->host_state = QD3D_HOST_READY;
    MemoryBarrier();
    ctrl->magic = QD3D_MAGIC;
    MemoryBarrier();
    return ex;
}

int qd3dx_poll(struct qd3dx *ex, uint32_t budget)
{
    uint32_t n = 0;

    if (ex->trace)
        qd3dx_trace_flush(ex);
    qd3dx_apply_release(ex);
    if (ex->faulted)
        return -1;
    qd3dx_pump(ex);
    while (n < budget) {
        int r = qd3dx_step(ex);

        if (r < 0) {
            if (qd3dx_apply_release(ex))
                break;
            return -1;
        }
        if (r == 0)
            break;
        n++;
        ex->alive = 1;
    }
    qd3dx_apply_release(ex);
    return (int)n;
}

int qd3dx_alive(struct qd3dx *ex)
{
    uint32_t hb = ex->ctrl->heartbeat;
    int r = ex->alive || hb != ex->heartbeat_seen;

    ex->alive = 0;
    ex->heartbeat_seen = hb;
    return r;
}

int qd3dx_prepare_wait(struct qd3dx *ex)
{
    struct qd3d_ctrl *ctrl = ex->ctrl;

    qd3dx_apply_release(ex);
    /* The guest went quiet: show its last frame before sleeping. */
    qd3dx_present_flush(ex);
    qd3dx_apply_release(ex);
    ctrl->host_idle = 1;
    MemoryBarrier(); /* idle store before the head load (pairs with the guest) */
    if (ex->release_request || (!ex->faulted && ctrl->head != ctrl->tail)) {
        ctrl->host_idle = 0;
        return 1;
    }
    return 0;
}

void qd3dx_wake(struct qd3dx *ex)
{
    ex->ctrl->host_idle = 0;
    MemoryBarrier();
}

static void qd3dx_end_session(struct qd3dx *ex, uint32_t head)
{
    struct qd3d_ctrl *ctrl = ex->ctrl;

    qd3dx_present_drop(ex); /* abandoned session: its last frame is not shown */
    qd3dx_session_release(ex);
    ex->pid = 0;
    ex->faulted = 0;
    ex->alive = 0;
    ex->heartbeat_seen = ctrl->heartbeat;
    ex->log_budget = QD3DX_WARN_BUDGET;
    ctrl->tail = head;
    ctrl->frames_done = ctrl->frames_submitted;
    ctrl->fault_op = 0;
    ctrl->fault_code = QD3D_FAULT_NONE;
    ctrl->host_idle = 0;
    MemoryBarrier();
    ctrl->host_state = QD3D_HOST_READY;
    MemoryBarrier();
}

void qd3dx_request_release(struct qd3dx *ex)
{
    uint32_t head = ex->ctrl->head;

    MemoryBarrier();
    InterlockedExchange64(&ex->release_request, (LONG64)((1ull << 32) | head));
}

static int qd3dx_apply_release(struct qd3dx *ex)
{
    LONG64 request;

    if (!ex->release_request)
        return 0;
    request = InterlockedExchange64(&ex->release_request, 0);
    if (!request)
        return 0;
    qd3dx_log(ex, "qd3dx: slot release pid %u, releasing %u objects", ex->pid, qd3dx_live_objects(ex));
    qd3dx_end_session(ex, (uint32_t)request);
    return 1;
}

void qd3dx_reset(struct qd3dx *ex)
{
    uint32_t head = ex->ctrl->head;

    /* Fault recovery can race the kernel freeing a slot. Read head before
     * the release check, just as step does, and prefer the captured cut so
     * recovery cannot swallow a new claimant's synchronous HELLO. */
    MemoryBarrier();
    if (!qd3dx_apply_release(ex))
        qd3dx_end_session(ex, head);
}

uint32_t qd3dx_session_pid(const struct qd3dx *ex)
{
    return ex->pid;
}

void qd3dx_destroy(struct qd3dx *ex)
{
    if (!ex)
        return;
    /* No hook from here (qd3dx.h): the embedder may be shutting down. */
    ex->hooks.frame = NULL;
    ex->hooks.source_released = NULL;
    ex->hooks.source_hidden = NULL;
    qd3dx_present_drop(ex);
    qd3dx_session_release(ex);
    if (ex->d3d)
        ex->d3d->lpVtbl->Release(ex->d3d);
    if (ex->d3d9_dll)
        FreeLibrary(ex->d3d9_dll);
    ex->ctrl->host_state = QD3D_HOST_DOWN;
    MemoryBarrier();
    qd3dx_map_free(&ex->by_id);
    qd3dx_map_free(&ex->by_ptr);
    free(ex->cmdbuf);
    free(ex->scratch);
    free(ex->cursor_save);
    free(ex->trace);
    free(ex->trace_fails);
    free(ex);
}
