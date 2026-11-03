/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * UPLOAD and READBACK: guest lock contents to host resources and back, for
 * vertex/index buffers, surfaces, texture and cube levels, volumes and
 * volume levels, in every pool. Guest D3DPOOL_MANAGED resources are
 * DEFAULT-pool resources on the host with a lazily created SYSTEMMEM
 * staging copy (attached to the resource as private data) that receives the
 * uploads and is copied with UpdateSurface/UpdateTexture.
 *
 * Every copy is bounded by the native lock it writes or reads: the region
 * must lie inside the level in the layout that lock exposes (else the
 * session faults), and the lock's pitches and extent must hold it (else the
 * transfer answers D3DERR_INVALIDCALL); nothing is copied otherwise.
 */
#include "qd3dx_priv.h"

#define QD3DX_FOURCC(a, b, c, d) \
    ((D3DFORMAT)((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24)))

#define QD3DX_FMT_ATI1 QD3DX_FOURCC('A', 'T', 'I', '1')
#define QD3DX_FMT_ATI2 QD3DX_FOURCC('A', 'T', 'I', '2')
#define QD3DX_FMT_YV12 QD3DX_FOURCC('Y', 'V', '1', '2')
#define QD3DX_FMT_NV12 QD3DX_FOURCC('N', 'V', '1', '2')

/* ------------------------------------------------------------------------ */
/* Level layouts                                                            */
/* ------------------------------------------------------------------------ */

#define QD3DX_PLANAR_NONE 0u
#define QD3DX_PLANAR_YV12 1u /* V plane, then U plane, at half the pitch */
#define QD3DX_PLANAR_NV12 2u /* interleaved U/V rows at the full pitch */

/*
 * Memory layout of one level as its native lock exposes it: blocks of
 * bw x bh texels and bpb bytes, block rows one pitch apart, ceil(height / bh)
 * rows per level (per slice for volumes). Formats the runtime knows have one
 * layout in every pool. ATI1N, ATI2N, YV12 and NV12 are opaque to the
 * runtime, so their layout depends on who allocates the level (measured with
 * the 32-bit d3d9 and 64-bit d3d9Ex runtimes of Windows 11, RTX 4090):
 *   - runtime memory (SYSTEMMEM, SCRATCH, and the staging copy behind a guest
 *     MANAGED resource): one byte per texel, pitch = width rounded up to 4,
 *     pitch * height bytes per level; YV12/NV12 get no chroma space there.
 *   - driver memory (DEFAULT): ATI1N/ATI2N in 4x4 blocks of 8/16 bytes
 *     (pitch 520/1040 at 257 texels); YV12/NV12 luma rows followed by chroma
 *     rows, pitch * height * 3/2 bytes in all (pitch 512/256 at 64 texels),
 *     defined for even sizes only. Such levels are locked whole and must be
 *     mapped memory.
 * The guest presents the same layouts, chosen by the guest pool, and the
 * wire carries them unchanged. Other FOURCCs are not transferred: the
 * runtime takes their texel size from the driver (INTZ locks 4 bytes per
 * texel, MET1 8).
 */
struct qd3dx_layout {
    uint32_t bw, bh, bpb;
    uint32_t planar;  /* QD3DX_PLANAR_*: chroma rows follow the luma rows */
    int driver;       /* defined by the driver, not the runtime */
};

/* Layout of fmt in memory of the native pool; 0 when it is unknown. */
static int qd3dx_layout(D3DFORMAT fmt, D3DPOOL pool, struct qd3dx_layout *l)
{
    l->bw = l->bh = 1u;
    l->bpb = 0u;
    l->planar = QD3DX_PLANAR_NONE;
    l->driver = 0;
    switch ((uint32_t)fmt) {
    case D3DFMT_R3G3B2: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4:
    case D3DFMT_S8_LOCKABLE:
        l->bpb = 1u;
        return 1;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_A8R3G3B2: case D3DFMT_X4R4G4B4: case D3DFMT_A8P8: case D3DFMT_A8L8:
    case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1:
    case D3DFMT_D16: case D3DFMT_L16: case D3DFMT_INDEX16: case D3DFMT_R16F: case D3DFMT_CxV8U8:
        l->bpb = 2u;
        return 1;
    case D3DFMT_R8G8B8:
        l->bpb = 3u;
        return 1;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8: case D3DFMT_G16R16: case D3DFMT_A2R10G10B10: case D3DFMT_X8L8V8U8:
    case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_A2W10V10U10: case D3DFMT_D32:
    case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4: case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_D24FS8: case D3DFMT_INDEX32: case D3DFMT_G16R16F: case D3DFMT_R32F:
    case D3DFMT_D32_LOCKABLE:
        l->bpb = 4u;
        return 1;
    case D3DFMT_A16B16G16R16: case D3DFMT_Q16W16V16U16: case D3DFMT_A16B16G16R16F:
    case D3DFMT_G32R32F:
        l->bpb = 8u;
        return 1;
    case D3DFMT_A32B32G32R32F:
        l->bpb = 16u;
        return 1;
    case D3DFMT_UYVY: case D3DFMT_YUY2: case D3DFMT_R8G8_B8G8: case D3DFMT_G8R8_G8B8:
        l->bw = 2u;
        l->bpb = 4u;
        return 1;
    case D3DFMT_DXT1:
        l->bw = l->bh = 4u;
        l->bpb = 8u;
        return 1;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        l->bw = l->bh = 4u;
        l->bpb = 16u;
        return 1;
    default:
        break;
    }
    if (fmt == QD3DX_FMT_ATI1 || fmt == QD3DX_FMT_ATI2) {
        if (pool == D3DPOOL_DEFAULT) {
            l->bw = l->bh = 4u;
            l->bpb = fmt == QD3DX_FMT_ATI1 ? 8u : 16u;
            l->driver = 1;
        } else {
            l->bpb = 1u;
        }
        return 1;
    }
    if (fmt == QD3DX_FMT_YV12 || fmt == QD3DX_FMT_NV12) {
        l->bpb = 1u;
        if (pool == D3DPOOL_DEFAULT) {
            l->planar = fmt == QD3DX_FMT_YV12 ? QD3DX_PLANAR_YV12 : QD3DX_PLANAR_NV12;
            l->driver = 1;
        }
        return 1;
    }
    return 0;
}

static int qd3dx_layout_eq(const struct qd3dx_layout *a, const struct qd3dx_layout *b)
{
    return a->bw == b->bw && a->bh == b->bh && a->bpb == b->bpb && a->planar == b->planar && a->driver == b->driver;
}

uint64_t qd3dx_level_bytes(D3DFORMAT fmt, uint32_t width, uint32_t height)
{
    struct qd3dx_layout l;

    if (!qd3dx_layout(fmt, D3DPOOL_DEFAULT, &l))
        return (uint64_t)width * height * 4u;
    return ((uint64_t)width + l.bw - 1u) / l.bw * l.bpb * (((uint64_t)height + l.bh - 1u) / l.bh) *
           (l.planar ? 3u : 2u) / 2u;
}

/* ------------------------------------------------------------------------ */
/* Target resolution                                                        */
/* ------------------------------------------------------------------------ */

struct qd3dx_xt {
    uint32_t kind;                 /* QD3D_UPLOAD_* */
    IDirect3DVertexBuffer9 *vb;    /* borrowed */
    IDirect3DIndexBuffer9 *ib;     /* borrowed */
    IDirect3DSurface9 *surf;       /* referenced */
    IDirect3DVolume9 *vol;         /* referenced */
    IDirect3DBaseTexture9 *tex;    /* referenced container or NULL */
    uint32_t tex_type;             /* QD3D_T_* of tex */
    uint32_t level, face;
    D3DPOOL guest_pool;
    D3DPOOL pool;                  /* native pool */
    DWORD usage;
    D3DFORMAT format;
    D3DMULTISAMPLE_TYPE ms;
    uint32_t width, height, depth; /* texels; bytes for buffers */
    /* Validated geometry, in the layout of the memory that holds the bytes
     * for the guest. */
    int known;                     /* layout known */
    struct qd3dx_layout lay;
    uint32_t rows, rowbytes;       /* (luma) block rows of the region, bytes per row */
};

static void qd3dx_xt_free(struct qd3dx_xt *t)
{
    if (t->surf)
        t->surf->lpVtbl->Release(t->surf);
    if (t->vol)
        t->vol->lpVtbl->Release(t->vol);
    if (t->tex)
        t->tex->lpVtbl->Release(t->tex);
}

static uint32_t qd3dx_tex_type(IDirect3DBaseTexture9 *tex)
{
    switch (tex->lpVtbl->GetType(tex)) {
    case D3DRTYPE_TEXTURE:       return QD3D_T_IDirect3DTexture9;
    case D3DRTYPE_CUBETEXTURE:   return QD3D_T_IDirect3DCubeTexture9;
    case D3DRTYPE_VOLUMETEXTURE: return QD3D_T_IDirect3DVolumeTexture9;
    default:                     return QD3D_T_NONE;
    }
}

/* Finds the level/face of a standalone surface/volume inside its texture
 * container. Returns 0 when it is not a level of that container. */
static int qd3dx_find_level(struct qd3dx_xt *t)
{
    uint32_t levels = t->tex->lpVtbl->GetLevelCount(t->tex), l, f;

    for (l = 0; l < levels; l++) {
        if (t->tex_type == QD3D_T_IDirect3DTexture9) {
            IDirect3DTexture9 *tx = (IDirect3DTexture9 *)t->tex;
            IDirect3DSurface9 *s = NULL;

            if (SUCCEEDED(tx->lpVtbl->GetSurfaceLevel(tx, l, &s)) && s) {
                s->lpVtbl->Release(s);
                if (s == t->surf) {
                    t->level = l;
                    t->face = 0;
                    return 1;
                }
            }
        } else if (t->tex_type == QD3D_T_IDirect3DCubeTexture9) {
            IDirect3DCubeTexture9 *cx = (IDirect3DCubeTexture9 *)t->tex;

            for (f = 0; f < 6u; f++) {
                IDirect3DSurface9 *s = NULL;

                if (SUCCEEDED(cx->lpVtbl->GetCubeMapSurface(cx, (D3DCUBEMAP_FACES)f, l, &s)) && s) {
                    s->lpVtbl->Release(s);
                    if (s == t->surf) {
                        t->level = l;
                        t->face = f;
                        return 1;
                    }
                }
            }
        } else if (t->tex_type == QD3D_T_IDirect3DVolumeTexture9) {
            IDirect3DVolumeTexture9 *vx = (IDirect3DVolumeTexture9 *)t->tex;
            IDirect3DVolume9 *v = NULL;

            if (SUCCEEDED(vx->lpVtbl->GetVolumeLevel(vx, l, &v)) && v) {
                v->lpVtbl->Release(v);
                if (v == t->vol) {
                    t->level = l;
                    t->face = 0;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void qd3dx_surface_desc(struct qd3dx_xt *t)
{
    D3DSURFACE_DESC d;

    memset(&d, 0, sizeof d);
    t->surf->lpVtbl->GetDesc(t->surf, &d);
    t->pool = d.Pool;
    t->usage = d.Usage;
    t->format = d.Format;
    t->ms = d.MultiSampleType;
    t->width = d.Width;
    t->height = d.Height;
    t->depth = 1;
}

static void qd3dx_volume_desc(struct qd3dx_xt *t)
{
    D3DVOLUME_DESC d;

    memset(&d, 0, sizeof d);
    t->vol->lpVtbl->GetDesc(t->vol, &d);
    t->pool = d.Pool;
    t->usage = d.Usage;
    t->format = d.Format;
    t->ms = D3DMULTISAMPLE_NONE;
    t->width = d.Width;
    t->height = d.Height;
    t->depth = d.Depth;
}

/* Resolves cmd->obj plus region addressing to a sub-resource. */
static uint32_t qd3dx_resolve(struct qd3dx *ex, uint32_t id, const struct qd3d_region *r,
                              struct qd3dx_xt *t)
{
    struct qd3dx_ent *e = qd3dx_ent_get(ex, id);
    HRESULT hr = D3D_OK;

    memset(t, 0, sizeof *t);
    if (!e)
        return QD3D_FAULT_OBJECT;
    t->kind = r->kind;
    switch (e->type) {
    case QD3D_T_IDirect3DVertexBuffer9: {
        D3DVERTEXBUFFER_DESC d;

        if (r->kind != QD3D_UPLOAD_BUFFER || r->level || r->face)
            return QD3D_FAULT_DECODE;
        t->vb = (IDirect3DVertexBuffer9 *)e->obj;
        memset(&d, 0, sizeof d);
        t->vb->lpVtbl->GetDesc(t->vb, &d);
        t->pool = d.Pool;
        t->usage = d.Usage;
        t->width = d.Size;
        t->guest_pool = qd3dx_guest_pool((IDirect3DResource9 *)t->vb, d.Pool);
        return QD3D_FAULT_NONE;
    }
    case QD3D_T_IDirect3DIndexBuffer9: {
        D3DINDEXBUFFER_DESC d;

        if (r->kind != QD3D_UPLOAD_BUFFER || r->level || r->face)
            return QD3D_FAULT_DECODE;
        t->ib = (IDirect3DIndexBuffer9 *)e->obj;
        memset(&d, 0, sizeof d);
        t->ib->lpVtbl->GetDesc(t->ib, &d);
        t->pool = d.Pool;
        t->usage = d.Usage;
        t->width = d.Size;
        t->guest_pool = qd3dx_guest_pool((IDirect3DResource9 *)t->ib, d.Pool);
        return QD3D_FAULT_NONE;
    }
    case QD3D_T_IDirect3DSurface9:
        if (r->kind != QD3D_UPLOAD_SURFACE || r->level || r->face)
            return QD3D_FAULT_DECODE;
        t->surf = (IDirect3DSurface9 *)e->obj;
        t->surf->lpVtbl->AddRef(t->surf);
        qd3dx_surface_desc(t);
        if (SUCCEEDED(t->surf->lpVtbl->GetContainer(t->surf, &IID_IDirect3DBaseTexture9, (void **)&t->tex)) &&
            t->tex) {
            t->tex_type = qd3dx_tex_type(t->tex);
            if (!qd3dx_find_level(t)) {
                t->tex->lpVtbl->Release(t->tex);
                t->tex = NULL;
            }
        } else {
            t->tex = NULL;
        }
        break;
    case QD3D_T_IDirect3DVolume9:
        if (r->kind != QD3D_UPLOAD_VOLUME || r->level || r->face)
            return QD3D_FAULT_DECODE;
        t->vol = (IDirect3DVolume9 *)e->obj;
        t->vol->lpVtbl->AddRef(t->vol);
        qd3dx_volume_desc(t);
        if (SUCCEEDED(t->vol->lpVtbl->GetContainer(t->vol, &IID_IDirect3DBaseTexture9, (void **)&t->tex)) &&
            t->tex) {
            t->tex_type = qd3dx_tex_type(t->tex);
            if (!qd3dx_find_level(t)) {
                t->tex->lpVtbl->Release(t->tex);
                t->tex = NULL;
            }
        } else {
            t->tex = NULL;
        }
        break;
    case QD3D_T_IDirect3DTexture9: {
        IDirect3DTexture9 *tx = (IDirect3DTexture9 *)e->obj;

        if (r->kind != QD3D_UPLOAD_SURFACE || r->face ||
            r->level >= tx->lpVtbl->GetLevelCount(tx))
            return QD3D_FAULT_DECODE;
        hr = tx->lpVtbl->GetSurfaceLevel(tx, r->level, &t->surf);
        if (SUCCEEDED(hr) && t->surf)
            qd3dx_surface_desc(t);
        break;
    }
    case QD3D_T_IDirect3DCubeTexture9: {
        IDirect3DCubeTexture9 *cx = (IDirect3DCubeTexture9 *)e->obj;

        if (r->kind != QD3D_UPLOAD_SURFACE || r->face > (uint32_t)D3DCUBEMAP_FACE_NEGATIVE_Z ||
            r->level >= cx->lpVtbl->GetLevelCount(cx))
            return QD3D_FAULT_DECODE;
        hr = cx->lpVtbl->GetCubeMapSurface(cx, (D3DCUBEMAP_FACES)r->face, r->level, &t->surf);
        if (SUCCEEDED(hr) && t->surf)
            qd3dx_surface_desc(t);
        break;
    }
    case QD3D_T_IDirect3DVolumeTexture9: {
        IDirect3DVolumeTexture9 *vx = (IDirect3DVolumeTexture9 *)e->obj;

        if (r->kind != QD3D_UPLOAD_VOLUME || r->face ||
            r->level >= vx->lpVtbl->GetLevelCount(vx))
            return QD3D_FAULT_DECODE;
        hr = vx->lpVtbl->GetVolumeLevel(vx, r->level, &t->vol);
        if (SUCCEEDED(hr) && t->vol)
            qd3dx_volume_desc(t);
        break;
    }
    default:
        return QD3D_FAULT_OBJECT;
    }
    if (e->type == QD3D_T_IDirect3DTexture9 || e->type == QD3D_T_IDirect3DCubeTexture9 ||
        e->type == QD3D_T_IDirect3DVolumeTexture9) {
        if (FAILED(hr) || (!t->surf && !t->vol)) {
            /* Level counts were validated, so this is a native failure. */
            qd3dx_xt_free(t);
            memset(t, 0, sizeof *t);
            return QD3D_FAULT_OBJECT;
        }
        t->tex = (IDirect3DBaseTexture9 *)e->obj;
        t->tex->lpVtbl->AddRef(t->tex);
        t->tex_type = e->type;
        t->level = r->level;
        t->face = r->face;
    }
    t->guest_pool = t->tex ? qd3dx_guest_pool((IDirect3DResource9 *)t->tex, t->pool) : t->pool;
    return QD3D_FAULT_NONE;
}

static int qd3dx_is_emulated_managed(const struct qd3dx_xt *t)
{
    return t->tex && t->guest_pool == D3DPOOL_MANAGED && t->pool == D3DPOOL_DEFAULT;
}

/* Pool of the native memory that holds the level's bytes for the guest: the
 * SYSTEMMEM staging copy for an emulated managed level. */
static D3DPOOL qd3dx_data_pool(const struct qd3dx_xt *t)
{
    return qd3dx_is_emulated_managed(t) ? D3DPOOL_SYSTEMMEM : t->pool;
}

/* Chroma of a planar level (even width and height): number of planes
 * (returned), rows and bytes per row of each. */
static uint32_t qd3dx_chroma(const struct qd3dx_xt *t, uint32_t *rows, uint32_t *bytes)
{
    *rows = t->height / 2u;
    if (t->lay.planar == QD3DX_PLANAR_YV12) {
        *bytes = t->width / 2u;
        return 2u;
    }
    *bytes = t->width;
    return 1u;
}

/* Validates the region against the sub-resource in the layout of the memory
 * that holds its bytes; fills lay, rows and rowbytes. */
static uint32_t qd3dx_check_region(struct qd3dx_xt *t, const struct qd3d_region *r)
{
    uint64_t rows, rowbytes, need;

    if (t->kind == QD3D_UPLOAD_BUFFER) {
        if ((uint64_t)r->x + r->w > t->width || r->data_size != r->w)
            return QD3D_FAULT_SIZE;
        t->known = 1;
        return QD3D_FAULT_NONE;
    }
    t->known = qd3dx_layout(t->format, qd3dx_data_pool(t), &t->lay);
    /* Odd planar sizes have no defined layout: the native driver loses data
     * of 5x3 or 7x7 YV12/NV12 surfaces through its own lock round trip. */
    if (t->known && t->lay.planar && ((t->width | t->height) & 1u))
        t->known = 0;
    if (!t->known)
        return QD3D_FAULT_NONE; /* layout unknown: caller declines the transfer */
    if (r->w == 0u || r->h == 0u || r->x % t->lay.bw || r->y % t->lay.bh || r->w % t->lay.bw ||
        r->h % t->lay.bh)
        return QD3D_FAULT_SIZE;
    if ((uint64_t)r->x + r->w > ((uint64_t)t->width + t->lay.bw - 1u) / t->lay.bw * t->lay.bw ||
        (uint64_t)r->y + r->h > ((uint64_t)t->height + t->lay.bh - 1u) / t->lay.bh * t->lay.bh)
        return QD3D_FAULT_SIZE;
    rows = r->h / t->lay.bh;
    rowbytes = (uint64_t)(r->w / t->lay.bw) * t->lay.bpb;
    if (r->row_pitch < rowbytes)
        return QD3D_FAULT_SIZE;
    if (t->lay.planar) {
        uint32_t crows, cbytes, planes = qd3dx_chroma(t, &crows, &cbytes);

        /* The whole surface in one region: the luma rows, then each chroma
         * plane at row_pitch / planes (an even row_pitch). */
        if (t->kind != QD3D_UPLOAD_SURFACE || r->x || r->y || r->w != t->width || r->h != t->height ||
            (r->row_pitch & 1u) || (uint64_t)cbytes * planes > r->row_pitch)
            return QD3D_FAULT_SIZE;
        need = (uint64_t)r->row_pitch * (rows + crows);
    } else if (t->kind == QD3D_UPLOAD_SURFACE) {
        need = (uint64_t)r->row_pitch * rows;
    } else {
        if (r->d == 0u || (uint64_t)r->z + r->d > t->depth ||
            (uint64_t)r->slice_pitch < (uint64_t)r->row_pitch * rows)
            return QD3D_FAULT_SIZE;
        need = (uint64_t)r->slice_pitch * r->d;
    }
    if (need != r->data_size)
        return QD3D_FAULT_SIZE;
    t->rows = (uint32_t)rows;
    t->rowbytes = (uint32_t)rowbytes;
    return QD3D_FAULT_NONE;
}

static RECT qd3dx_rect(const struct qd3dx_xt *t, const struct qd3d_region *r)
{
    RECT rc;

    rc.left = (LONG)r->x;
    rc.top = (LONG)r->y;
    rc.right = (LONG)((uint64_t)r->x + r->w < t->width ? r->x + r->w : t->width);
    rc.bottom = (LONG)((uint64_t)r->y + r->h < t->height ? r->y + r->h : t->height);
    return rc;
}

static D3DBOX qd3dx_box(const struct qd3dx_xt *t, const struct qd3d_region *r)
{
    D3DBOX b;
    RECT rc = qd3dx_rect(t, r);

    b.Left = (UINT)rc.left;
    b.Top = (UINT)rc.top;
    b.Right = (UINT)rc.right;
    b.Bottom = (UINT)rc.bottom;
    b.Front = r->z;
    b.Back = r->z + r->d;
    return b;
}

/* ------------------------------------------------------------------------ */
/* Copies bounded by the native lock                                        */
/* ------------------------------------------------------------------------ */

/* Rows of one plane: offsets of the first row from the lock pointer and in
 * the wire bytes, both pitches, bytes per row. */
struct qd3dx_run {
    size_t noff, woff, npitch, wpitch;
    uint32_t rowbytes, rows;
};

/* A checked copy: up to three planes per slice, slices nslice (native) and
 * wslice (wire) bytes apart. */
struct qd3dx_plan {
    struct qd3dx_run run[3];
    uint32_t nrun, slices;
    size_t nslice, wslice;
};

/* Whether [p, p + n) is committed memory the copy may read (or write). */
static int qd3dx_mapped(const void *p, size_t n, int write)
{
    const DWORD ok = write ? (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
                           : (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                              PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    uintptr_t q = (uintptr_t)p, end = q + n;
    MEMORY_BASIC_INFORMATION mbi;

    if (end < q)
        return 0;
    while (q < end) {
        if (VirtualQuery((const void *)q, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT ||
            (mbi.Protect & PAGE_GUARD) || !(mbi.Protect & ok))
            return 0;
        q = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

/*
 * Plans the copy of region r between the wire and a native lock of the
 * level in memory of the given pool: bits and pitch (slice for volumes) as
 * the lock returned them; whole when the lock covers the level rather than
 * the region's rectangle or box, so the region's offset is added here.
 * Returns 0, copying nothing, when that memory has another layout than the
 * region was validated in, or when the lock cannot hold the region: a pitch
 * shorter than a row, a slice shorter than the level's rows, rows past the
 * end of the level, or for driver layouts memory that is not mapped.
 */
static int qd3dx_plan(const struct qd3dx_xt *t, const struct qd3d_region *r, D3DPOOL pool, const void *bits,
                      INT pitch, INT slice, int whole, int write, struct qd3dx_plan *pl)
{
    struct qd3dx_layout nl;
    uint64_t np, ns = 0, lrows, xoff, yrow, avail, base, end, wend, last = 0;
    uint32_t i;

    if (!bits || pitch <= 0 || !qd3dx_layout(t->format, pool, &nl) || !qd3dx_layout_eq(&nl, &t->lay) ||
        (t->lay.driver && !whole))
        return 0;
    np = (uint64_t)pitch;
    lrows = ((uint64_t)t->height + t->lay.bh - 1u) / t->lay.bh;
    xoff = (uint64_t)(r->x / t->lay.bw) * t->lay.bpb;
    yrow = r->y / t->lay.bh;
    if (xoff + t->rowbytes > np)
        return 0;
    memset(pl, 0, sizeof *pl);
    pl->slices = 1u;
    if (t->kind == QD3D_UPLOAD_VOLUME) {
        if (slice <= 0 || (uint64_t)slice < lrows * np)
            return 0;
        ns = (uint64_t)slice;
        pl->slices = r->d;
        pl->nslice = (size_t)ns;
        pl->wslice = r->slice_pitch;
        /* Bytes from bits to the end of the level. */
        avail = whole ? t->depth * ns : (t->depth - r->z) * ns - yrow * np - xoff;
        base = whole ? r->z * ns + yrow * np + xoff : 0u;
    } else {
        avail = whole ? lrows * np : (lrows - yrow) * np - xoff;
        base = whole ? yrow * np + xoff : 0u;
    }
    pl->run[0].noff = (size_t)base;
    pl->run[0].npitch = (size_t)np;
    pl->run[0].wpitch = r->row_pitch;
    pl->run[0].rowbytes = t->rowbytes;
    pl->run[0].rows = t->rows;
    pl->nrun = 1u;
    if (t->lay.planar) {
        uint32_t crows, cbytes, planes = qd3dx_chroma(t, &crows, &cbytes);
        uint64_t cnp = np / planes, cwp = r->row_pitch / planes;

        /* Whole level (validated): chroma follows the luma rows. */
        if (np % planes || cbytes > cnp)
            return 0;
        avail = np * (lrows + crows);
        for (i = 0; i < planes; i++) {
            struct qd3dx_run *c = &pl->run[1u + i];

            c->noff = (size_t)(np * lrows + i * cnp * crows);
            c->woff = (size_t)((uint64_t)r->row_pitch * t->rows + i * cwp * crows);
            c->npitch = (size_t)cnp;
            c->wpitch = (size_t)cwp;
            c->rowbytes = cbytes;
            c->rows = crows;
        }
        pl->nrun = 1u + planes;
    }
    for (i = 0; i < pl->nrun; i++) {
        const struct qd3dx_run *q = &pl->run[i];

        end = q->noff + (uint64_t)(pl->slices - 1u) * ns + (uint64_t)(q->rows - 1u) * q->npitch + q->rowbytes;
        wend = q->woff + (uint64_t)(pl->slices - 1u) * pl->wslice + (uint64_t)(q->rows - 1u) * q->wpitch +
               q->rowbytes;
        if (end > avail || wend > r->data_size)
            return 0;
        if (end > last)
            last = end;
    }
    return !t->lay.driver || qd3dx_mapped(bits, (size_t)last, write);
}

static void qd3dx_put_plan(uint8_t *bits, const struct qd3dx_plan *pl, const uint8_t *data)
{
    uint32_t s, i, y;

    for (s = 0; s < pl->slices; s++) {
        for (i = 0; i < pl->nrun; i++) {
            const struct qd3dx_run *q = &pl->run[i];
            uint8_t *dst = bits + (size_t)s * pl->nslice + q->noff;
            const uint8_t *src = data + (size_t)s * pl->wslice + q->woff;

            for (y = 0; y < q->rows; y++)
                memcpy(dst + (size_t)y * q->npitch, src + (size_t)y * q->wpitch, q->rowbytes);
        }
    }
}

/* The wire bytes between and after the rows read as zero. */
static void qd3dx_get_plan(const uint8_t *bits, const struct qd3dx_plan *pl, uint8_t *out, uint32_t size)
{
    uint32_t s, i, y;

    memset(out, 0, size);
    for (s = 0; s < pl->slices; s++) {
        for (i = 0; i < pl->nrun; i++) {
            const struct qd3dx_run *q = &pl->run[i];
            const uint8_t *src = bits + (size_t)s * pl->nslice + q->noff;
            uint8_t *dst = out + (size_t)s * pl->wslice + q->woff;

            for (y = 0; y < q->rows; y++)
                memcpy(dst + (size_t)y * q->wpitch, src + (size_t)y * q->npitch, q->rowbytes);
        }
    }
}

/* The region's rectangle widened to whole blocks of the level's layout in
 * driver memory (ATI1N/ATI2N: 4x4 there, one byte per texel in the staging
 * copy) and clipped to the level, for UpdateSurface into the DEFAULT level. */
static RECT qd3dx_update_rect(const struct qd3dx_xt *t, const struct qd3d_region *r)
{
    struct qd3dx_layout dl;
    RECT rc = qd3dx_rect(t, r);

    if (qd3dx_layout(t->format, D3DPOOL_DEFAULT, &dl) && (dl.bw > 1u || dl.bh > 1u)) {
        rc.left -= rc.left % (LONG)dl.bw;
        rc.top -= rc.top % (LONG)dl.bh;
        rc.right = (LONG)(((uint64_t)rc.right + dl.bw - 1u) / dl.bw * dl.bw);
        rc.bottom = (LONG)(((uint64_t)rc.bottom + dl.bh - 1u) / dl.bh * dl.bh);
        if ((uint64_t)rc.right > t->width)
            rc.right = (LONG)t->width;
        if ((uint64_t)rc.bottom > t->height)
            rc.bottom = (LONG)t->height;
    }
    return rc;
}

/* ------------------------------------------------------------------------ */
/* Managed staging                                                          */
/* ------------------------------------------------------------------------ */

/* {7d3c5b84-4f2e-4b8e-9a51-3c1f6d2e9b07} */
static const GUID qd3dx_guid_staging =
    { 0x7d3c5b84, 0x4f2e, 0x4b8e, { 0x9a, 0x51, 0x3c, 0x1f, 0x6d, 0x2e, 0x9b, 0x07 } };

/* SYSTEMMEM twin of the container (referenced), created on first use and
 * owned by the container through private data. NULL if create is 0 and none
 * exists yet. */
static IDirect3DBaseTexture9 *qd3dx_staging(struct qd3dx *ex, struct qd3dx_xt *t, int create, HRESULT *phr)
{
    IDirect3DBaseTexture9 *tex = t->tex;
    IUnknown *unk = NULL;
    DWORD size = sizeof unk;
    IDirect3DDevice9 *dev = NULL;
    IDirect3DBaseTexture9 *st = NULL;
    UINT levels;
    HRESULT hr;

    *phr = D3D_OK;
    if (SUCCEEDED(tex->lpVtbl->GetPrivateData(tex, &qd3dx_guid_staging, &unk, &size)) && unk)
        return (IDirect3DBaseTexture9 *)unk; /* GetPrivateData added a reference */
    if (!create)
        return NULL;
    levels = (t->usage & D3DUSAGE_AUTOGENMIPMAP) ? 1u : tex->lpVtbl->GetLevelCount(tex);
    hr = tex->lpVtbl->GetDevice(tex, &dev);
    if (FAILED(hr)) {
        *phr = hr;
        return NULL;
    }
    if (t->tex_type == QD3D_T_IDirect3DTexture9) {
        IDirect3DTexture9 *tx = (IDirect3DTexture9 *)tex;
        D3DSURFACE_DESC d;

        hr = tx->lpVtbl->GetLevelDesc(tx, 0, &d);
        if (SUCCEEDED(hr))
            hr = dev->lpVtbl->CreateTexture(dev, d.Width, d.Height, levels, 0, d.Format,
                                            D3DPOOL_SYSTEMMEM, (IDirect3DTexture9 **)&st, NULL);
    } else if (t->tex_type == QD3D_T_IDirect3DCubeTexture9) {
        IDirect3DCubeTexture9 *cx = (IDirect3DCubeTexture9 *)tex;
        D3DSURFACE_DESC d;

        hr = cx->lpVtbl->GetLevelDesc(cx, 0, &d);
        if (SUCCEEDED(hr))
            hr = dev->lpVtbl->CreateCubeTexture(dev, d.Width, levels, 0, d.Format,
                                                D3DPOOL_SYSTEMMEM, (IDirect3DCubeTexture9 **)&st, NULL);
    } else {
        IDirect3DVolumeTexture9 *vx = (IDirect3DVolumeTexture9 *)tex;
        D3DVOLUME_DESC d;

        hr = vx->lpVtbl->GetLevelDesc(vx, 0, &d);
        if (SUCCEEDED(hr))
            hr = dev->lpVtbl->CreateVolumeTexture(dev, d.Width, d.Height, d.Depth, levels, 0, d.Format,
                                                  D3DPOOL_SYSTEMMEM, (IDirect3DVolumeTexture9 **)&st, NULL);
    }
    dev->lpVtbl->Release(dev);
    if (FAILED(hr) || !st) {
        *phr = FAILED(hr) ? hr : E_FAIL;
        return NULL;
    }
    /* With D3DSPD_IUNKNOWN, pData is the interface pointer itself. */
    unk = (IUnknown *)st;
    hr = tex->lpVtbl->SetPrivateData(tex, &qd3dx_guid_staging, unk, sizeof unk, D3DSPD_IUNKNOWN);
    if (FAILED(hr)) {
        st->lpVtbl->Release(st);
        *phr = hr;
        return NULL;
    }
    return st;
}

/* Sub-resource of the staging twin matching t (referenced). */
static HRESULT qd3dx_staging_level(const struct qd3dx_xt *t, IDirect3DBaseTexture9 *st,
                                   IDirect3DSurface9 **ps, IDirect3DVolume9 **pv)
{
    UINT levels = st->lpVtbl->GetLevelCount(st);

    *ps = NULL;
    *pv = NULL;
    if (t->level >= levels)
        return D3DERR_INVALIDCALL; /* autogen staging holds only level 0 */
    if (t->tex_type == QD3D_T_IDirect3DTexture9)
        return ((IDirect3DTexture9 *)st)->lpVtbl->GetSurfaceLevel((IDirect3DTexture9 *)st, t->level, ps);
    if (t->tex_type == QD3D_T_IDirect3DCubeTexture9)
        return ((IDirect3DCubeTexture9 *)st)->lpVtbl->GetCubeMapSurface((IDirect3DCubeTexture9 *)st,
                                                                         (D3DCUBEMAP_FACES)t->face, t->level, ps);
    return ((IDirect3DVolumeTexture9 *)st)->lpVtbl->GetVolumeLevel((IDirect3DVolumeTexture9 *)st, t->level, pv);
}

/* ------------------------------------------------------------------------ */
/* Upload                                                                   */
/* ------------------------------------------------------------------------ */

/* Writes the region into surface s, whose memory belongs to pool. DISCARD
 * and driver layouts lock the whole level, otherwise the region's rect. */
static HRESULT qd3dx_put_surface(const struct qd3dx_xt *t, IDirect3DSurface9 *s, D3DPOOL pool,
                                 const struct qd3d_region *r, const uint8_t *data, DWORD flags)
{
    D3DLOCKED_RECT lr;
    RECT rc = qd3dx_rect(t, r);
    struct qd3dx_plan pl;
    int whole = (flags & D3DLOCK_DISCARD) || t->lay.driver;
    HRESULT hr = s->lpVtbl->LockRect(s, &lr, whole ? NULL : &rc, flags);

    if (FAILED(hr))
        return hr;
    if (!qd3dx_plan(t, r, pool, lr.pBits, lr.Pitch, 0, whole, 1, &pl)) {
        s->lpVtbl->UnlockRect(s);
        return D3DERR_INVALIDCALL;
    }
    qd3dx_put_plan((uint8_t *)lr.pBits, &pl, data);
    return s->lpVtbl->UnlockRect(s);
}

static HRESULT qd3dx_put_volume(const struct qd3dx_xt *t, IDirect3DVolume9 *v, D3DPOOL pool,
                                const struct qd3d_region *r, const uint8_t *data, DWORD flags)
{
    D3DLOCKED_BOX lb;
    D3DBOX box = qd3dx_box(t, r);
    struct qd3dx_plan pl;
    int whole = (flags & D3DLOCK_DISCARD) || t->lay.driver;
    HRESULT hr = v->lpVtbl->LockBox(v, &lb, whole ? NULL : &box, flags);

    if (FAILED(hr))
        return hr;
    if (!qd3dx_plan(t, r, pool, lb.pBits, lb.RowPitch, lb.SlicePitch, whole, 1, &pl)) {
        v->lpVtbl->UnlockBox(v);
        return D3DERR_INVALIDCALL;
    }
    qd3dx_put_plan((uint8_t *)lb.pBits, &pl, data);
    return v->lpVtbl->UnlockBox(v);
}

/* DEFAULT surface that cannot be locked: system-memory copy + UpdateSurface
 * (refused by the plan for layouts that differ between the two pools). */
static HRESULT qd3dx_put_surface_update(const struct qd3dx_xt *t, const struct qd3d_region *r,
                                        const uint8_t *data)
{
    IDirect3DDevice9 *dev = NULL;
    IDirect3DSurface9 *tmp = NULL;
    RECT rc = qd3dx_rect(t, r);
    POINT pt;
    struct qd3dx_xt tt = *t;
    struct qd3d_region tr = *r;
    HRESULT hr;

    hr = t->surf->lpVtbl->GetDevice(t->surf, &dev);
    if (FAILED(hr))
        return hr;
    hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, (UINT)(rc.right - rc.left), (UINT)(rc.bottom - rc.top),
                                                  t->format, D3DPOOL_SYSTEMMEM, &tmp, NULL);
    if (SUCCEEDED(hr)) {
        tt.width = (uint32_t)(rc.right - rc.left);
        tt.height = (uint32_t)(rc.bottom - rc.top);
        tr.x = tr.y = 0;
        hr = qd3dx_put_surface(&tt, tmp, D3DPOOL_SYSTEMMEM, &tr, data, 0);
    }
    if (SUCCEEDED(hr)) {
        pt.x = rc.left;
        pt.y = rc.top;
        hr = dev->lpVtbl->UpdateSurface(dev, tmp, NULL, t->surf, &pt);
    }
    if (tmp)
        tmp->lpVtbl->Release(tmp);
    dev->lpVtbl->Release(dev);
    return hr;
}

static HRESULT qd3dx_put_managed(struct qd3dx *ex, const struct qd3dx_xt *t,
                                 const struct qd3d_region *r, const uint8_t *data)
{
    IDirect3DBaseTexture9 *st;
    IDirect3DSurface9 *ss = NULL;
    IDirect3DVolume9 *sv = NULL;
    IDirect3DDevice9 *dev = NULL;
    int autogen = (t->usage & D3DUSAGE_AUTOGENMIPMAP) != 0;
    HRESULT hr;

    if (autogen && t->level != 0u)
        return D3D_OK; /* sublevels of autogen textures are generated */
    st = qd3dx_staging(ex, (struct qd3dx_xt *)t, 1, &hr);
    if (!st)
        return hr;
    hr = qd3dx_staging_level(t, st, &ss, &sv);
    if (SUCCEEDED(hr))
        hr = ss ? qd3dx_put_surface(t, ss, D3DPOOL_SYSTEMMEM, r, data, 0)
                : qd3dx_put_volume(t, sv, D3DPOOL_SYSTEMMEM, r, data, 0);
    if (SUCCEEDED(hr))
        hr = t->tex->lpVtbl->GetDevice(t->tex, &dev);
    if (SUCCEEDED(hr)) {
        if (ss && !autogen) {
            RECT rc = qd3dx_update_rect(t, r);
            POINT pt;

            pt.x = rc.left;
            pt.y = rc.top;
            hr = dev->lpVtbl->UpdateSurface(dev, ss, &rc, t->surf, &pt);
        } else {
            hr = dev->lpVtbl->UpdateTexture(dev, st, t->tex);
        }
        dev->lpVtbl->Release(dev);
    }
    if (ss)
        ss->lpVtbl->Release(ss);
    if (sv)
        sv->lpVtbl->Release(sv);
    st->lpVtbl->Release(st);
    return hr;
}

static int qd3dx_continues(const struct qd3dx_upload_prev *p, uint32_t obj, const struct qd3d_region *r)
{
    if (!p->valid || p->obj != obj || p->kind != r->kind || p->level != r->level || p->face != r->face)
        return 0;
    switch (r->kind) {
    case QD3D_UPLOAD_BUFFER:
        return (uint64_t)r->x >= (uint64_t)p->x + p->w;
    case QD3D_UPLOAD_SURFACE:
        return (uint64_t)r->y >= (uint64_t)p->y + p->h;
    default:
        return (uint64_t)r->z >= (uint64_t)p->z + p->d ||
               (r->d == 1u && p->d == 1u && r->z == p->z && (uint64_t)r->y >= (uint64_t)p->y + p->h);
    }
}

uint32_t qd3dx_op_upload(struct qd3dx *ex, const struct qd3d_cmd *c,
                         struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    const void *p = qd3d_rd_raw(rd, sizeof(struct qd3d_region));
    struct qd3d_region r;
    struct qd3dx_xt t;
    const uint8_t *data;
    uint32_t fault;
    DWORD gflags, flags;
    int first, dynamic;
    HRESULT hr = D3D_OK;

    if (!p)
        return QD3D_FAULT_DECODE;
    memcpy(&r, p, sizeof r);
    if (r.kind < QD3D_UPLOAD_BUFFER || r.kind > QD3D_UPLOAD_VOLUME ||
        (r.lock_flags &
         ~(uint32_t)(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE | D3DLOCK_NO_DIRTY_UPDATE | QD3D_UPLOAD_CONTINUE)))
        return QD3D_FAULT_DECODE;
    if (rd->left != r.data_size)
        return QD3D_FAULT_SIZE;
    data = (const uint8_t *)qd3d_rd_raw(rd, r.data_size);
    if (!qd3d_rd_end(rd))
        return QD3D_FAULT_DECODE;
    first = !(r.lock_flags & QD3D_UPLOAD_CONTINUE);
    if (!first && !qd3dx_continues(&ex->upload_prev, c->obj, &r))
        return QD3D_FAULT_DECODE;
    fault = qd3dx_resolve(ex, c->obj, &r, &t);
    if (fault == QD3D_FAULT_NONE)
        fault = qd3dx_check_region(&t, &r);
    if (fault != QD3D_FAULT_NONE) {
        qd3dx_xt_free(&t);
        return fault;
    }

    ex->upload_prev.valid = 1;
    ex->upload_prev.obj = c->obj;
    ex->upload_prev.kind = r.kind;
    ex->upload_prev.level = r.level;
    ex->upload_prev.face = r.face;
    ex->upload_prev.x = r.x;
    ex->upload_prev.y = r.y;
    ex->upload_prev.z = r.z;
    ex->upload_prev.w = r.w;
    ex->upload_prev.h = r.h;
    ex->upload_prev.d = r.d;

    gflags = r.lock_flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
    dynamic = t.pool == D3DPOOL_DEFAULT && (t.usage & D3DUSAGE_DYNAMIC);

    if (!t.known) {
        qd3dx_warn(ex, "qd3dx: UPLOAD to 0x%08x: format %u has no known layout, skipped",
                   c->obj, (unsigned)t.format);
    } else if (r.data_size == 0u) {
        /* nothing to apply */
    } else if (t.kind == QD3D_UPLOAD_BUFFER) {
        void *dst = NULL;

        flags = dynamic ? (first ? gflags : D3DLOCK_NOOVERWRITE) : 0;
        if (t.vb)
            hr = t.vb->lpVtbl->Lock(t.vb, r.x, r.w, &dst, flags);
        else
            hr = t.ib->lpVtbl->Lock(t.ib, r.x, r.w, &dst, flags);
        if (SUCCEEDED(hr)) {
            memcpy(dst, data, r.w);
            hr = t.vb ? t.vb->lpVtbl->Unlock(t.vb) : t.ib->lpVtbl->Unlock(t.ib);
        }
    } else if (qd3dx_is_emulated_managed(&t)) {
        hr = qd3dx_put_managed(ex, &t, &r, data);
    } else {
        /* Textures take no NOOVERWRITE; DISCARD only on the first chunk.
         * NO_DIRTY_UPDATE leaves a SYSTEMMEM level's dirty region (the
         * UpdateTexture source) as the guest's lock left it. */
        flags = (dynamic && first) ? (gflags & D3DLOCK_DISCARD) : 0;
        if (t.pool == D3DPOOL_SYSTEMMEM)
            flags |= r.lock_flags & D3DLOCK_NO_DIRTY_UPDATE;
        if (t.surf) {
            hr = qd3dx_put_surface(&t, t.surf, t.pool, &r, data, flags);
            if (FAILED(hr) && t.pool == D3DPOOL_DEFAULT)
                hr = qd3dx_put_surface_update(&t, &r, data);
        } else {
            hr = qd3dx_put_volume(&t, t.vol, t.pool, &r, data, flags);
        }
    }
    if (FAILED(hr))
        qd3dx_warn(ex, "qd3dx: UPLOAD to 0x%08x (kind %u level %u face %u pool %u fmt %u) failed 0x%08lx",
                   c->obj, r.kind, r.level, r.face, (unsigned)t.pool, (unsigned)t.format, (unsigned long)hr);
    qd3dx_xt_free(&t);
    *ret = (uint32_t)hr;
    return QD3D_FAULT_NONE;
}

/* ------------------------------------------------------------------------ */
/* Readback                                                                 */
/* ------------------------------------------------------------------------ */

/* Reads the region out of surface s, whose memory belongs to pool. */
static HRESULT qd3dx_get_surface(const struct qd3dx_xt *t, IDirect3DSurface9 *s, D3DPOOL pool,
                                 const struct qd3d_region *r, uint8_t *out)
{
    D3DLOCKED_RECT lr;
    RECT rc = qd3dx_rect(t, r);
    struct qd3dx_plan pl;
    int whole = t->lay.driver;
    HRESULT hr = s->lpVtbl->LockRect(s, &lr, whole ? NULL : &rc, D3DLOCK_READONLY);

    if (FAILED(hr))
        return hr;
    if (!qd3dx_plan(t, r, pool, lr.pBits, lr.Pitch, 0, whole, 0, &pl)) {
        s->lpVtbl->UnlockRect(s);
        return D3DERR_INVALIDCALL;
    }
    qd3dx_get_plan((const uint8_t *)lr.pBits, &pl, out, r->data_size);
    return s->lpVtbl->UnlockRect(s);
}

static HRESULT qd3dx_get_volume(const struct qd3dx_xt *t, IDirect3DVolume9 *v, D3DPOOL pool,
                                const struct qd3d_region *r, uint8_t *out)
{
    D3DLOCKED_BOX lb;
    D3DBOX box = qd3dx_box(t, r);
    struct qd3dx_plan pl;
    int whole = t->lay.driver;
    HRESULT hr = v->lpVtbl->LockBox(v, &lb, whole ? NULL : &box, D3DLOCK_READONLY);

    if (FAILED(hr))
        return hr;
    if (!qd3dx_plan(t, r, pool, lb.pBits, lb.RowPitch, lb.SlicePitch, whole, 0, &pl)) {
        v->lpVtbl->UnlockBox(v);
        return D3DERR_INVALIDCALL;
    }
    qd3dx_get_plan((const uint8_t *)lb.pBits, &pl, out, r->data_size);
    return v->lpVtbl->UnlockBox(v);
}

/* GPU-only surface: copy into a lockable system-memory surface. */
static HRESULT qd3dx_get_surface_gpu(const struct qd3dx_xt *t, const struct qd3d_region *r, uint8_t *out)
{
    IDirect3DDevice9 *dev = NULL;
    IDirect3DSurface9 *rt = NULL, *sys = NULL, *src = t->surf;
    HRESULT hr;

    hr = t->surf->lpVtbl->GetDevice(t->surf, &dev);
    if (FAILED(hr))
        return hr;
    if (!(t->usage & D3DUSAGE_RENDERTARGET) || t->ms != D3DMULTISAMPLE_NONE) {
        hr = dev->lpVtbl->CreateRenderTarget(dev, t->width, t->height, t->format, D3DMULTISAMPLE_NONE,
                                             0, FALSE, &rt, NULL);
        if (SUCCEEDED(hr))
            hr = dev->lpVtbl->StretchRect(dev, t->surf, NULL, rt, NULL, D3DTEXF_NONE);
        src = rt;
    }
    if (SUCCEEDED(hr))
        hr = dev->lpVtbl->CreateOffscreenPlainSurface(dev, t->width, t->height, t->format,
                                                      D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (SUCCEEDED(hr))
        hr = dev->lpVtbl->GetRenderTargetData(dev, src, sys);
    if (SUCCEEDED(hr))
        hr = qd3dx_get_surface(t, sys, D3DPOOL_SYSTEMMEM, r, out);
    if (sys)
        sys->lpVtbl->Release(sys);
    if (rt)
        rt->lpVtbl->Release(rt);
    dev->lpVtbl->Release(dev);
    return hr;
}

uint32_t qd3dx_op_readback(struct qd3dx *ex, const struct qd3d_cmd *c,
                           struct qd3d_rd *rd, struct qd3d_wr *rp, uint32_t *ret)
{
    const void *p = qd3d_rd_raw(rd, sizeof(struct qd3d_region));
    struct qd3d_region r;
    struct qd3dx_xt t;
    uint8_t *out;
    uint32_t fault;
    HRESULT hr = D3D_OK;

    if (!p || !qd3d_rd_end(rd))
        return QD3D_FAULT_DECODE;
    memcpy(&r, p, sizeof r);
    if (r.kind < QD3D_UPLOAD_BUFFER || r.kind > QD3D_UPLOAD_VOLUME)
        return QD3D_FAULT_DECODE;
    if (r.data_size > rp->cap - rp->used)
        return QD3D_FAULT_SIZE;
    fault = qd3dx_resolve(ex, c->obj, &r, &t);
    if (fault == QD3D_FAULT_NONE)
        fault = qd3dx_check_region(&t, &r);
    if (fault != QD3D_FAULT_NONE) {
        qd3dx_xt_free(&t);
        return fault;
    }
    out = rp->p + rp->used;

    if (!t.known) {
        hr = D3DERR_INVALIDCALL;
    } else if (r.data_size == 0u) {
        hr = D3D_OK;
    } else if (t.kind == QD3D_UPLOAD_BUFFER) {
        void *src = NULL;

        if (t.vb)
            hr = t.vb->lpVtbl->Lock(t.vb, r.x, r.w, &src, D3DLOCK_READONLY);
        else
            hr = t.ib->lpVtbl->Lock(t.ib, r.x, r.w, &src, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) {
            memcpy(out, src, r.w);
            hr = t.vb ? t.vb->lpVtbl->Unlock(t.vb) : t.ib->lpVtbl->Unlock(t.ib);
        }
    } else {
        IDirect3DBaseTexture9 *st = NULL;

        if (qd3dx_is_emulated_managed(&t)) {
            HRESULT shr;

            st = qd3dx_staging(ex, &t, 0, &shr);
        }
        if (st) {
            IDirect3DSurface9 *ss = NULL;
            IDirect3DVolume9 *sv = NULL;

            hr = qd3dx_staging_level(&t, st, &ss, &sv);
            if (SUCCEEDED(hr))
                hr = ss ? qd3dx_get_surface(&t, ss, D3DPOOL_SYSTEMMEM, &r, out)
                        : qd3dx_get_volume(&t, sv, D3DPOOL_SYSTEMMEM, &r, out);
            if (ss)
                ss->lpVtbl->Release(ss);
            if (sv)
                sv->lpVtbl->Release(sv);
            st->lpVtbl->Release(st);
        } else if (t.surf) {
            hr = qd3dx_get_surface(&t, t.surf, t.pool, &r, out);
            if (FAILED(hr) && t.pool == D3DPOOL_DEFAULT)
                hr = qd3dx_get_surface_gpu(&t, &r, out);
        } else {
            hr = qd3dx_get_volume(&t, t.vol, t.pool, &r, out);
        }
    }
    if (FAILED(hr))
        qd3dx_warn(ex, "qd3dx: READBACK of 0x%08x (kind %u level %u face %u pool %u fmt %u) failed 0x%08lx",
                   c->obj, r.kind, r.level, r.face, (unsigned)t.pool, (unsigned)t.format, (unsigned long)hr);
    else
        rp->used += r.data_size;
    qd3dx_xt_free(&t);
    *ret = (uint32_t)hr;
    return QD3D_FAULT_NONE;
}
