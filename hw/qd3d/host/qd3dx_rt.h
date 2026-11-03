/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host runtime contract for the generated executor (qd3dx_gen.c) and the
 * hand-written executor core and overrides. Windows x64, MinGW, C11.
 *
 * The guest is untrusted: every id, length, and count is validated before
 * native Direct3D sees it. A validation failure is a fault: the executor
 * stops the session (QD3D_HOST_FAULT) instead of guessing.
 */
#ifndef QD3DX_RT_H
#define QD3DX_RT_H

#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <string.h>

#include "../proto/qd3d_proto.h"
#include "../proto/qd3d_ops.h"
#include "../proto/qd3d_sizes.h"

struct qd3dx; /* executor, defined by the core */

/* Command payload reader. Never reads past left; sets err instead. */
struct qd3d_rd {
    const uint8_t *p;
    uint32_t left;
    int err;
};

static inline uint32_t qd3d_rd_u32(struct qd3d_rd *r)
{
    uint32_t v;

    if (r->err || r->left < 4u) {
        r->err = 1;
        return 0u;
    }
    memcpy(&v, r->p, 4u);
    r->p += 4u;
    r->left -= 4u;
    return v;
}

/* Returns NULL for QD3D_DATA_NULL (with *nbytes = 0) and on error. */
static inline const void *qd3d_rd_data(struct qd3d_rd *r, uint32_t *nbytes)
{
    uint32_t n = qd3d_rd_u32(r);
    uint64_t padded;
    const void *p;

    *nbytes = 0u;
    if (r->err || n == QD3D_DATA_NULL)
        return NULL;
    padded = QD3D_PAD4((uint64_t)n);
    if (padded > r->left) {
        r->err = 1;
        return NULL;
    }
    p = r->p;
    r->p += padded;
    r->left -= (uint32_t)padded;
    *nbytes = n;
    return p;
}

/* True when the payload decoded without error and was consumed exactly. */
static inline int qd3d_rd_end(const struct qd3d_rd *r)
{
    return !r->err && r->left == 0u;
}

/* Reply payload writer. Sets err instead of overflowing cap. */
struct qd3d_wr {
    uint8_t *p;
    uint32_t cap;
    uint32_t used;
    int err;
};

static inline void qd3d_wr_u32(struct qd3d_wr *w, uint32_t v)
{
    if (w->err || w->cap - w->used < 4u) {
        w->err = 1;
        return;
    }
    memcpy(w->p + w->used, &v, 4u);
    w->used += 4u;
}

static inline void qd3d_wr_data(struct qd3d_wr *w, const void *p, uint64_t nbytes)
{
    uint64_t padded = QD3D_PAD4(nbytes);

    if (!p) {
        qd3d_wr_u32(w, QD3D_DATA_NULL);
        return;
    }
    if (w->err || nbytes >= QD3D_DATA_NULL || 4u + padded > (uint64_t)(w->cap - w->used)) {
        w->err = 1;
        return;
    }
    qd3d_wr_u32(w, (uint32_t)nbytes);
    memcpy(w->p + w->used, p, (size_t)nbytes);
    memset(w->p + w->used + nbytes, 0, (size_t)(padded - nbytes));
    w->used += (uint32_t)padded;
}

/* Object table (implemented by the core). */
/* The object for id if it is live and its type isa `type`, else NULL. */
IUnknown *qd3dx_obj(struct qd3dx *ex, uint32_t id, uint32_t type);
/* True if id is a guest id greater than every guest id bound this session. */
int qd3dx_id_is_fresh(struct qd3dx *ex, uint32_t id);
/* Binds a fresh guest id, taking ownership of one reference to obj. Returns
 * 0 on allocation failure; the caller then still owns the reference. */
int qd3dx_bind(struct qd3dx *ex, uint32_t id, IUnknown *obj, uint32_t type);
/* OUTOBJ: maps obj (holding the reference a getter just added) to its id,
 * assigning a host id if needed, and consumes that reference. Stores the
 * concrete interface type in *type. Returns 0 (and type 0) for NULL. */
uint32_t qd3dx_out_id(struct qd3dx *ex, IUnknown *obj, uint32_t declared_type,
                      uint32_t *type);
/* Scratch memory valid until the next command; NULL if bytes exceeds the
 * reply limit. */
void *qd3dx_scratch(struct qd3dx *ex, uint64_t bytes);

/*
 * Generated per-method executor. Decodes and validates the payload, calls
 * native Direct3D (or the qd3dxo_* override), binds created objects, writes
 * reply items to rp and the return value to *ret. Returns QD3D_FAULT_NONE or
 * the fault code; on a fault nothing has been called.
 */
typedef uint32_t (*qd3dx_method_fn)(struct qd3dx *ex, const struct qd3d_cmd *c,
                                    struct qd3d_rd *rd, struct qd3d_wr *rp,
                                    uint32_t *ret);

/* Indexed by op - QD3D_OP_METHOD_BASE; NULL for guest-local and extension
 * methods, which the core treats as QD3D_FAULT_UNEXPECTED. */
extern const qd3dx_method_fn qd3dx_methods[QD3D_OP_METHOD_COUNT];

#endif /* QD3DX_RT_H */
