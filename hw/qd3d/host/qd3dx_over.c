/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Hand-written replacements (qd3dxo_*) for the native call of generic
 * methods, declared in qd3dx_gen.h.
 *
 * Capability policy: adapter identifier and device caps are the host's
 * native values; nothing is clamped there. GetAvailableTextureMem is
 * clamped to 512 MiB because XP-era titles store it in signed 32-bit
 * variables or size caches from it, and Direct3D 9Ex on a 24 GiB GPU
 * reports values that overflow those; the DEFAULT resources created since
 * are subtracted from the clamp, as a native value would move.
 *
 * ValidateDevice: current drivers answer E_FAIL for every state, including
 * a fresh device's defaults (measured: RTX 4090, Windows 11, Direct3D 9 and
 * 9Ex), while games of the period treat any failure as fatal. A definite
 * native verdict is kept; otherwise the fixed-function stage setup is checked
 * against the device caps, as the drivers of the period answered.
 */
#include <stdlib.h>

#include "qd3dx_priv.h"
#include "qd3dx_gen.h" /* prototypes of the qd3dxo_* functions defined here */

#define QD3DX_TEXTURE_MEM_CLAMP (512u * 1024u * 1024u)

/* Private-data tag holding the guest pool of a resource whose host pool
 * differs (guest D3DPOOL_MANAGED created in D3DPOOL_DEFAULT). */
/* {b51e6a2c-93d4-4c4f-8e0a-6f2d7c1b4a58} */
static const GUID qd3dx_guid_pool =
    { 0xb51e6a2c, 0x93d4, 0x4c4f, { 0x8e, 0x0a, 0x6f, 0x2d, 0x7c, 0x1b, 0x4a, 0x58 } };

D3DPOOL qd3dx_guest_pool(IDirect3DResource9 *res, D3DPOOL native_pool)
{
    DWORD pool = 0, size = sizeof pool;

    if (SUCCEEDED(res->lpVtbl->GetPrivateData(res, &qd3dx_guid_pool, &pool, &size)) && size == sizeof pool)
        return (D3DPOOL)pool;
    return native_pool;
}

/* Maps a guest pool to the host pool; Direct3D 9Ex has no managed pool. */
static D3DPOOL qd3dx_host_pool(D3DPOOL pool)
{
    return pool == D3DPOOL_MANAGED ? D3DPOOL_DEFAULT : pool;
}

/* Tags a created resource with its guest pool when it was remapped. A
 * failure releases the resource: an untagged managed resource would take
 * uploads through the wrong path. */
static HRESULT qd3dx_tag_pool(HRESULT hr, IDirect3DResource9 **pres, D3DPOOL guest)
{
    DWORD pool = (DWORD)guest;
    HRESULT th;

    if (FAILED(hr) || !pres || !*pres || guest != D3DPOOL_MANAGED)
        return hr;
    th = (*pres)->lpVtbl->SetPrivateData(*pres, &qd3dx_guid_pool, &pool, sizeof pool, 0);
    if (FAILED(th)) {
        (*pres)->lpVtbl->Release(*pres);
        *pres = NULL;
        return th;
    }
    return hr;
}

HRESULT qd3dxo_IDirect3D9_GetAdapterIdentifier(struct qd3dx *ex, IDirect3D9 *This, UINT Adapter, DWORD Flags,
                                               D3DADAPTER_IDENTIFIER9 *pIdentifier)
{
    return This->lpVtbl->GetAdapterIdentifier(This, Adapter, Flags, pIdentifier);
}

HRESULT qd3dxo_IDirect3D9_GetDeviceCaps(struct qd3dx *ex, IDirect3D9 *This, UINT Adapter, D3DDEVTYPE DeviceType,
                                        D3DCAPS9 *pCaps)
{
    return This->lpVtbl->GetDeviceCaps(This, Adapter, DeviceType, pCaps);
}

/* Private data of a DEFAULT resource created here: the bytes it counts in
 * qd3dx_dev.vidmem. */
/* {9e2c4b71-6a3d-4f05-8c1e-5b7d2a9f0c36} */
static const GUID qd3dx_guid_vidmem =
    { 0x9e2c4b71, 0x6a3d, 0x4f05, { 0x8c, 0x1e, 0x5b, 0x7d, 0x2a, 0x9f, 0x0c, 0x36 } };

static uint64_t qd3dx_resource_bytes(IDirect3DResource9 *res)
{
    D3DRESOURCETYPE type = res->lpVtbl->GetType(res);
    uint64_t n = 0;
    UINT l, levels;

    switch (type) {
    case D3DRTYPE_TEXTURE:
    case D3DRTYPE_CUBETEXTURE:
        levels = ((IDirect3DBaseTexture9 *)res)->lpVtbl->GetLevelCount((IDirect3DBaseTexture9 *)res);
        for (l = 0; l < levels; l++) {
            D3DSURFACE_DESC d;
            HRESULT hr = type == D3DRTYPE_TEXTURE
                             ? ((IDirect3DTexture9 *)res)->lpVtbl->GetLevelDesc((IDirect3DTexture9 *)res, l, &d)
                             : ((IDirect3DCubeTexture9 *)res)->lpVtbl->GetLevelDesc((IDirect3DCubeTexture9 *)res, l,
                                                                                    &d);

            if (SUCCEEDED(hr))
                n += qd3dx_level_bytes(d.Format, d.Width, d.Height);
        }
        return type == D3DRTYPE_CUBETEXTURE ? n * 6u : n;
    case D3DRTYPE_VOLUMETEXTURE:
        levels = ((IDirect3DBaseTexture9 *)res)->lpVtbl->GetLevelCount((IDirect3DBaseTexture9 *)res);
        for (l = 0; l < levels; l++) {
            D3DVOLUME_DESC d;

            if (SUCCEEDED(((IDirect3DVolumeTexture9 *)res)->lpVtbl->GetLevelDesc((IDirect3DVolumeTexture9 *)res, l,
                                                                                  &d)))
                n += qd3dx_level_bytes(d.Format, d.Width, d.Height) * d.Depth;
        }
        return n;
    case D3DRTYPE_VERTEXBUFFER: {
        D3DVERTEXBUFFER_DESC d;

        return SUCCEEDED(((IDirect3DVertexBuffer9 *)res)->lpVtbl->GetDesc((IDirect3DVertexBuffer9 *)res, &d)) ? d.Size
                                                                                                             : 0u;
    }
    case D3DRTYPE_INDEXBUFFER: {
        D3DINDEXBUFFER_DESC d;

        return SUCCEEDED(((IDirect3DIndexBuffer9 *)res)->lpVtbl->GetDesc((IDirect3DIndexBuffer9 *)res, &d)) ? d.Size
                                                                                                           : 0u;
    }
    default:
        return 0u;
    }
}

/* Counts a resource just created in the DEFAULT pool against its device. */
static HRESULT qd3dx_count_vidmem(struct qd3dx *ex, IDirect3DDevice9 *dev, HRESULT hr, IDirect3DResource9 *res,
                                  D3DPOOL pool)
{
    struct qd3dx_dev *d;
    uint64_t size;

    if (FAILED(hr) || !res || qd3dx_host_pool(pool) != D3DPOOL_DEFAULT || !(d = qd3dx_dev_find(ex, dev)))
        return hr;
    size = qd3dx_resource_bytes(res);
    if (size && SUCCEEDED(res->lpVtbl->SetPrivateData(res, &qd3dx_guid_vidmem, &size, sizeof size, 0)))
        d->vidmem += size;
    return hr;
}

void qd3dx_vidmem_release(struct qd3dx *ex, IUnknown *obj, uint32_t type)
{
    IDirect3DResource9 *res = (IDirect3DResource9 *)obj;
    IDirect3DDevice9 *dev = NULL;
    struct qd3dx_dev *d;
    uint64_t size = 0;
    DWORD n = sizeof size;

    if (!qd3d_type_isa(type, QD3D_T_IDirect3DResource9) ||
        FAILED(res->lpVtbl->GetPrivateData(res, &qd3dx_guid_vidmem, &size, &n)) || n != sizeof size)
        return;
    res->lpVtbl->FreePrivateData(res, &qd3dx_guid_vidmem);
    if (SUCCEEDED(res->lpVtbl->GetDevice(res, &dev)) && dev) {
        d = qd3dx_dev_find(ex, dev);
        if (d)
            d->vidmem = d->vidmem > size ? d->vidmem - size : 0u;
        dev->lpVtbl->Release(dev);
    }
}

/* The clamp minus the DEFAULT resources created since: the native value of
 * a large adapter saturates (4095 MiB on an RTX 4090) and never moves, while
 * applications measure their allocations with it. A smaller native value
 * moves by itself. */
UINT qd3dxo_IDirect3DDevice9_GetAvailableTextureMem(struct qd3dx *ex, IDirect3DDevice9 *This)
{
    struct qd3dx_dev *d = qd3dx_dev_find(ex, This);
    UINT mem = This->lpVtbl->GetAvailableTextureMem(This);
    uint64_t used = d ? d->vidmem : 0u;

    if (mem <= QD3DX_TEXTURE_MEM_CLAMP)
        return mem;
    return used >= QD3DX_TEXTURE_MEM_CLAMP ? 0u : (UINT)(QD3DX_TEXTURE_MEM_CLAMP - used);
}

HRESULT qd3dxo_IDirect3DDevice9_GetDeviceCaps(struct qd3dx *ex, IDirect3DDevice9 *This, D3DCAPS9 *pCaps)
{
    return This->lpVtbl->GetDeviceCaps(This, pCaps);
}

HRESULT qd3dxo_IDirect3DDevice9_CreateTexture(struct qd3dx *ex, IDirect3DDevice9 *This, UINT Width, UINT Height,
                                              UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                              IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle)
{
    HRESULT hr = This->lpVtbl->CreateTexture(This, Width, Height, Levels, Usage, Format, qd3dx_host_pool(Pool),
                                             ppTexture, pSharedHandle);

    return qd3dx_count_vidmem(ex, This, qd3dx_tag_pool(hr, (IDirect3DResource9 **)ppTexture, Pool),
                              ppTexture ? (IDirect3DResource9 *)*ppTexture : NULL, Pool);
}

HRESULT qd3dxo_IDirect3DDevice9_CreateVolumeTexture(struct qd3dx *ex, IDirect3DDevice9 *This, UINT Width,
                                                    UINT Height, UINT Depth, UINT Levels, DWORD Usage,
                                                    D3DFORMAT Format, D3DPOOL Pool,
                                                    IDirect3DVolumeTexture9 **ppVolumeTexture,
                                                    HANDLE *pSharedHandle)
{
    HRESULT hr = This->lpVtbl->CreateVolumeTexture(This, Width, Height, Depth, Levels, Usage, Format,
                                                   qd3dx_host_pool(Pool), ppVolumeTexture, pSharedHandle);

    return qd3dx_count_vidmem(ex, This, qd3dx_tag_pool(hr, (IDirect3DResource9 **)ppVolumeTexture, Pool),
                              ppVolumeTexture ? (IDirect3DResource9 *)*ppVolumeTexture : NULL, Pool);
}

HRESULT qd3dxo_IDirect3DDevice9_CreateCubeTexture(struct qd3dx *ex, IDirect3DDevice9 *This, UINT EdgeLength,
                                                  UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                                  IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle)
{
    HRESULT hr = This->lpVtbl->CreateCubeTexture(This, EdgeLength, Levels, Usage, Format, qd3dx_host_pool(Pool),
                                                 ppCubeTexture, pSharedHandle);

    return qd3dx_count_vidmem(ex, This, qd3dx_tag_pool(hr, (IDirect3DResource9 **)ppCubeTexture, Pool),
                              ppCubeTexture ? (IDirect3DResource9 *)*ppCubeTexture : NULL, Pool);
}

HRESULT qd3dxo_IDirect3DDevice9_CreateVertexBuffer(struct qd3dx *ex, IDirect3DDevice9 *This, UINT Length,
                                                   DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                                   IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle)
{
    HRESULT hr = This->lpVtbl->CreateVertexBuffer(This, Length, Usage, FVF, qd3dx_host_pool(Pool),
                                                  ppVertexBuffer, pSharedHandle);

    return qd3dx_count_vidmem(ex, This, qd3dx_tag_pool(hr, (IDirect3DResource9 **)ppVertexBuffer, Pool),
                              ppVertexBuffer ? (IDirect3DResource9 *)*ppVertexBuffer : NULL, Pool);
}

HRESULT qd3dxo_IDirect3DDevice9_CreateIndexBuffer(struct qd3dx *ex, IDirect3DDevice9 *This, UINT Length,
                                                  DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                                  IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle)
{
    HRESULT hr = This->lpVtbl->CreateIndexBuffer(This, Length, Usage, Format, qd3dx_host_pool(Pool),
                                                 ppIndexBuffer, pSharedHandle);

    return qd3dx_count_vidmem(ex, This, qd3dx_tag_pool(hr, (IDirect3DResource9 **)ppIndexBuffer, Pool),
                              ppIndexBuffer ? (IDirect3DResource9 *)*ppIndexBuffer : NULL, Pool);
}

/*
 * SYSTEMMEM textures. Direct3D 9Ex cannot sample them here (the driver does
 * not report D3DDEVCAPS_TEXTURESYSTEMMEMORY): a draw with one bound is
 * dropped together with the commands batched before it, even a Clear, and
 * its error then fails the next allocation the driver makes, so the next
 * CreateTexture or CreateOffscreenPlainSurface answers E_FAIL (measured:
 * RTX 4090, Windows 11, Direct3D 9Ex without QD3D). The stock runtime
 * samples them on NVIDIA. A stage given a SYSTEMMEM texture therefore
 * samples a DEFAULT twin holding a copy of every 2D or cube level made at
 * SetTexture (nothing for volume textures), and GetTexture still answers
 * the SYSTEMMEM texture.
 */

/* Private data of a SYSTEMMEM texture: its DEFAULT twin. */
/* {4a1f7c3e-2b9d-4e58-b6a0-9c3d5e7f1a24} */
static const GUID qd3dx_guid_twin =
    { 0x4a1f7c3e, 0x2b9d, 0x4e58, { 0xb6, 0xa0, 0x9c, 0x3d, 0x5e, 0x7f, 0x1a, 0x24 } };

static int qd3dx_stage_index(DWORD stage)
{
    if (stage < 16u)
        return (int)stage;
    if (stage >= D3DDMAPSAMPLER && stage <= D3DVERTEXTEXTURESAMPLER3)
        return 16 + (int)(stage - D3DDMAPSAMPLER);
    return -1;
}

static D3DPOOL qd3dx_texture_pool(IDirect3DBaseTexture9 *t)
{
    D3DSURFACE_DESC sd;
    D3DVOLUME_DESC vd;

    switch (t->lpVtbl->GetType(t)) {
    case D3DRTYPE_TEXTURE:
        if (SUCCEEDED(((IDirect3DTexture9 *)t)->lpVtbl->GetLevelDesc((IDirect3DTexture9 *)t, 0, &sd)))
            return sd.Pool;
        break;
    case D3DRTYPE_CUBETEXTURE:
        if (SUCCEEDED(((IDirect3DCubeTexture9 *)t)->lpVtbl->GetLevelDesc((IDirect3DCubeTexture9 *)t, 0, &sd)))
            return sd.Pool;
        break;
    case D3DRTYPE_VOLUMETEXTURE:
        if (SUCCEEDED(((IDirect3DVolumeTexture9 *)t)->lpVtbl->GetLevelDesc((IDirect3DVolumeTexture9 *)t, 0, &vd)))
            return vd.Pool;
        break;
    default:
        break;
    }
    return D3DPOOL_DEFAULT;
}

static IDirect3DSurface9 *qd3dx_tex_surface(IDirect3DBaseTexture9 *t, int cube, UINT face, UINT level)
{
    IDirect3DSurface9 *s = NULL;

    if (cube)
        ((IDirect3DCubeTexture9 *)t)->lpVtbl->GetCubeMapSurface((IDirect3DCubeTexture9 *)t, (D3DCUBEMAP_FACES)face,
                                                                 level, &s);
    else
        ((IDirect3DTexture9 *)t)->lpVtbl->GetSurfaceLevel((IDirect3DTexture9 *)t, level, &s);
    return s;
}

/* The DEFAULT twin of SYSTEMMEM texture src (referenced), created on first
 * use and refreshed from every level; NULL for volume textures and formats
 * DEFAULT textures cannot have. */
static IDirect3DBaseTexture9 *qd3dx_twin(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *src)
{
    D3DRESOURCETYPE type = src->lpVtbl->GetType(src);
    int cube = type == D3DRTYPE_CUBETEXTURE;
    UINT levels = src->lpVtbl->GetLevelCount(src), f, l;
    IDirect3DBaseTexture9 *tw = NULL;
    IUnknown *unk = NULL;
    DWORD size = sizeof unk;
    D3DSURFACE_DESC d;
    HRESULT hr;

    if (type != D3DRTYPE_TEXTURE && !cube)
        return NULL;
    if (SUCCEEDED(src->lpVtbl->GetPrivateData(src, &qd3dx_guid_twin, &unk, &size)) && unk) {
        tw = (IDirect3DBaseTexture9 *)unk; /* GetPrivateData added a reference */
    } else {
        if (cube) {
            IDirect3DCubeTexture9 *cx = (IDirect3DCubeTexture9 *)src;

            hr = cx->lpVtbl->GetLevelDesc(cx, 0, &d);
            if (SUCCEEDED(hr))
                hr = dev->lpVtbl->CreateCubeTexture(dev, d.Width, levels, 0, d.Format, D3DPOOL_DEFAULT,
                                                    (IDirect3DCubeTexture9 **)&tw, NULL);
        } else {
            IDirect3DTexture9 *tx = (IDirect3DTexture9 *)src;

            hr = tx->lpVtbl->GetLevelDesc(tx, 0, &d);
            if (SUCCEEDED(hr))
                hr = dev->lpVtbl->CreateTexture(dev, d.Width, d.Height, levels, 0, d.Format, D3DPOOL_DEFAULT,
                                                (IDirect3DTexture9 **)&tw, NULL);
        }
        if (FAILED(hr) || !tw)
            return NULL;
        /* With D3DSPD_IUNKNOWN, pData is the interface pointer itself. */
        unk = (IUnknown *)tw;
        if (FAILED(src->lpVtbl->SetPrivateData(src, &qd3dx_guid_twin, unk, sizeof unk, D3DSPD_IUNKNOWN))) {
            tw->lpVtbl->Release(tw);
            return NULL;
        }
    }
    for (f = 0; f < (cube ? 6u : 1u); f++) {
        for (l = 0; l < levels; l++) {
            IDirect3DSurface9 *s = qd3dx_tex_surface(src, cube, f, l), *t = qd3dx_tex_surface(tw, cube, f, l);

            if (s && t)
                dev->lpVtbl->UpdateSurface(dev, s, NULL, t, NULL);
            if (s)
                s->lpVtbl->Release(s);
            if (t)
                t->lpVtbl->Release(t);
        }
    }
    return tw;
}

static void qd3dx_drop_stage_refs(struct qd3dx_dev *d)
{
    int i;

    for (i = 0; i < QD3DX_STAGES; i++) {
        if (d->sysmem_bound[i])
            d->sysmem_bound[i]->lpVtbl->Release(d->sysmem_bound[i]);
        d->sysmem_bound[i] = NULL;
        d->stage_twin[i] = NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* Managed texture LOD                                                      */
/* ------------------------------------------------------------------------ */
/*
 * The native runtime gives a managed texture with LOD n a video-memory copy
 * that starts at level n: every sampler reads level n as its most detailed
 * level, whatever its MIPFILTER, and D3DSAMP_MAXMIPLEVEL m then limits it to
 * max(m, n) (Wine's maxmip_test passes on the stock runtime). Direct3D 9Ex
 * has no managed pool, so the texture here is the whole DEFAULT texture,
 * whose SetLOD does nothing. Around each draw, every stage whose texture
 * has a LOD gets MAXMIPLEVEL max(m, n), and with MIPFILTER NONE (where the
 * driver ignores MAXMIPLEVEL) POINT mip filtering with MAXMIPLEVEL n and a
 * -16 LOD bias, so level n is sampled; the guest's values come back after
 * the draw, so getters and state blocks see them. Draws inside a state
 * block recording are left alone (the changes would be recorded).
 */

/* Private data of a texture the guest created MANAGED: its LOD (absent: 0). */
/* {6d2b8e41-0c7a-4f93-a5d8-3e19b4c7f260} */
static const GUID qd3dx_guid_lod =
    { 0x6d2b8e41, 0x0c7a, 0x4f93, { 0xa5, 0xd8, 0x3e, 0x19, 0xb4, 0xc7, 0xf2, 0x60 } };

static DWORD qd3dx_tex_lod(IDirect3DBaseTexture9 *t)
{
    DWORD lod = 0, size = sizeof lod;

    if (!t || FAILED(t->lpVtbl->GetPrivateData(t, &qd3dx_guid_lod, &lod, &size)) || size != sizeof lod)
        return 0;
    return lod;
}

static DWORD qd3dx_stage_of(int i)
{
    return i < 16 ? (DWORD)i : D3DDMAPSAMPLER + (DWORD)(i - 16);
}

/* Re-reads the LOD of the texture bound at stage index i. */
static void qd3dx_lod_stage(struct qd3dx_dev *d, int i)
{
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)d->dev;
    IDirect3DBaseTexture9 *t = NULL;
    DWORD lod = 0;

    if (SUCCEEDED(dev->lpVtbl->GetTexture(dev, qd3dx_stage_of(i), &t)) && t) {
        lod = qd3dx_tex_lod(t);
        t->lpVtbl->Release(t);
    }
    d->lod_val[i] = lod;
    if (lod)
        d->lod_mask |= 1u << i;
    else
        d->lod_mask &= ~(1u << i);
}

static void qd3dx_lod_rescan(struct qd3dx_dev *d)
{
    int i;

    for (i = 0; i < QD3DX_STAGES; i++)
        qd3dx_lod_stage(d, i);
}

struct qd3dx_lod_saved {
    uint32_t mask;                 /* stages changed for the draw */
    DWORD filter[QD3DX_STAGES], maxmip[QD3DX_STAGES], bias[QD3DX_STAGES];
};

static void qd3dx_lod_begin(struct qd3dx *ex, IDirect3DDevice9 *dev, struct qd3dx_lod_saved *s)
{
    struct qd3dx_dev *d;
    uint32_t m;

    s->mask = 0;
    if (!ex->lod_used) /* the common case: one load per draw */
        return;
    d = qd3dx_dev_find(ex, dev);
    if (!d || !d->lod_mask || d->recording)
        return;
    for (m = d->lod_mask; m; m &= m - 1u) {
        int i = __builtin_ctz(m);
        DWORD stage = qd3dx_stage_of(i), lod = d->lod_val[i], filter = D3DTEXF_NONE, maxmip = 0, bias = 0;

        dev->lpVtbl->GetSamplerState(dev, stage, D3DSAMP_MIPFILTER, &filter);
        dev->lpVtbl->GetSamplerState(dev, stage, D3DSAMP_MAXMIPLEVEL, &maxmip);
        if (filter == D3DTEXF_NONE) {
            dev->lpVtbl->GetSamplerState(dev, stage, D3DSAMP_MIPMAPLODBIAS, &bias);
            dev->lpVtbl->SetSamplerState(dev, stage, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
            dev->lpVtbl->SetSamplerState(dev, stage, D3DSAMP_MAXMIPLEVEL, lod);
            dev->lpVtbl->SetSamplerState(dev, stage, D3DSAMP_MIPMAPLODBIAS, qd3d_f2u(-16.0f));
        } else if (maxmip < lod) {
            dev->lpVtbl->SetSamplerState(dev, stage, D3DSAMP_MAXMIPLEVEL, lod);
        } else {
            continue;
        }
        s->filter[i] = filter;
        s->maxmip[i] = maxmip;
        s->bias[i] = bias;
        s->mask |= 1u << i;
    }
}

static void qd3dx_lod_end(IDirect3DDevice9 *dev, const struct qd3dx_lod_saved *s)
{
    uint32_t m;

    for (m = s->mask; m; m &= m - 1u) {
        int i = __builtin_ctz(m);
        DWORD stage = qd3dx_stage_of(i);

        dev->lpVtbl->SetSamplerState(dev, stage, D3DSAMP_MAXMIPLEVEL, s->maxmip[i]);
        if (s->filter[i] == D3DTEXF_NONE) {
            dev->lpVtbl->SetSamplerState(dev, stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->lpVtbl->SetSamplerState(dev, stage, D3DSAMP_MIPMAPLODBIAS, s->bias[i]);
        }
    }
}

DWORD qd3dxo_IDirect3DBaseTexture9_SetLOD(struct qd3dx *ex, IDirect3DBaseTexture9 *This, DWORD LODNew)
{
    DWORD levels = This->lpVtbl->GetLevelCount(This), old = qd3dx_tex_lod(This), lod;
    IDirect3DDevice9 *dev = NULL;
    struct qd3dx_dev *d;

    if (qd3dx_guest_pool((IDirect3DResource9 *)This, qd3dx_texture_pool(This)) != D3DPOOL_MANAGED)
        return This->lpVtbl->SetLOD(This, LODNew);
    lod = levels && LODNew >= levels ? levels - 1u : LODNew;
    if (lod == old)
        return old;
    if (lod)
        This->lpVtbl->SetPrivateData(This, &qd3dx_guid_lod, &lod, sizeof lod, 0);
    else
        This->lpVtbl->FreePrivateData(This, &qd3dx_guid_lod);
    if (SUCCEEDED(This->lpVtbl->GetDevice(This, &dev)) && dev) {
        d = qd3dx_dev_find(ex, dev);
        if (d && lod)
            d->lod_used = ex->lod_used = 1;
        if (d && d->lod_used)
            qd3dx_lod_rescan(d);
        dev->lpVtbl->Release(dev);
    }
    return old;
}

HRESULT qd3dxo_IDirect3DStateBlock9_Apply(struct qd3dx *ex, IDirect3DStateBlock9 *This)
{
    HRESULT hr = This->lpVtbl->Apply(This);
    IDirect3DDevice9 *dev = NULL;
    struct qd3dx_dev *d;

    if (SUCCEEDED(hr) && SUCCEEDED(This->lpVtbl->GetDevice(This, &dev)) && dev) {
        d = qd3dx_dev_find(ex, dev);
        if (d && d->lod_used)
            qd3dx_lod_rescan(d); /* the block may bind textures */
        dev->lpVtbl->Release(dev);
    }
    return hr;
}

HRESULT qd3dxo_IDirect3DDevice9_BeginStateBlock(struct qd3dx *ex, IDirect3DDevice9 *This)
{
    struct qd3dx_dev *d = qd3dx_dev_find(ex, This);
    HRESULT hr = This->lpVtbl->BeginStateBlock(This);

    if (SUCCEEDED(hr) && d)
        d->recording = 1;
    return hr;
}

HRESULT qd3dxo_IDirect3DDevice9_EndStateBlock(struct qd3dx *ex, IDirect3DDevice9 *This, IDirect3DStateBlock9 **ppSB)
{
    struct qd3dx_dev *d = qd3dx_dev_find(ex, This);
    HRESULT hr = This->lpVtbl->EndStateBlock(This, ppSB);

    if (SUCCEEDED(hr) && d)
        d->recording = 0;
    return hr;
}

HRESULT qd3dxo_IDirect3DDevice9_DrawPrimitive(struct qd3dx *ex, IDirect3DDevice9 *This, D3DPRIMITIVETYPE PrimitiveType,
                                              UINT StartVertex, UINT PrimitiveCount)
{
    struct qd3dx_lod_saved s;
    HRESULT hr;

    qd3dx_lod_begin(ex, This, &s);
    hr = This->lpVtbl->DrawPrimitive(This, PrimitiveType, StartVertex, PrimitiveCount);
    if (s.mask)
        qd3dx_lod_end(This, &s);
    return hr;
}

HRESULT qd3dxo_IDirect3DDevice9_DrawIndexedPrimitive(struct qd3dx *ex, IDirect3DDevice9 *This,
                                                     D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex,
                                                     UINT MinVertexIndex, UINT NumVertices, UINT startIndex,
                                                     UINT primCount)
{
    struct qd3dx_lod_saved s;
    HRESULT hr;

    qd3dx_lod_begin(ex, This, &s);
    hr = This->lpVtbl->DrawIndexedPrimitive(This, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices,
                                            startIndex, primCount);
    if (s.mask)
        qd3dx_lod_end(This, &s);
    return hr;
}

HRESULT qd3dxo_IDirect3DDevice9_DrawPrimitiveUP(struct qd3dx *ex, IDirect3DDevice9 *This,
                                                D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                                const void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    struct qd3dx_lod_saved s;
    HRESULT hr;

    qd3dx_lod_begin(ex, This, &s);
    hr = This->lpVtbl->DrawPrimitiveUP(This, PrimitiveType, PrimitiveCount, pVertexStreamZeroData,
                                       VertexStreamZeroStride);
    if (s.mask)
        qd3dx_lod_end(This, &s);
    return hr;
}

/* ------------------------------------------------------------------------ */
/* Device state                                                             */
/* ------------------------------------------------------------------------ */

void qd3dx_dev_init_state(struct qd3dx_dev *d)
{
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)d->dev;
    D3DCAPS9 caps;

    memset(&caps, 0, sizeof caps);
    dev->lpVtbl->GetDeviceCaps(dev, &caps);
    d->max_streams = caps.MaxStreams ? caps.MaxStreams : 1u;
    d->num_rts = caps.NumSimultaneousRTs ? caps.NumSimultaneousRTs : 1u;
    if (FAILED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &d->defaults)))
        d->defaults = NULL;
}

/* Native D3D9 Reset leaves the device as created: every state at its
 * default, nothing bound, lights off, the viewport and scissor rectangle
 * covering the new back buffer, only render target 0. */
void qd3dx_dev_reset_state(struct qd3dx_dev *d, UINT width, UINT height)
{
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)d->dev;
    D3DVIEWPORT9 vp = { 0, 0, width, height, 0.0f, 1.0f };
    RECT sc = { 0, 0, (LONG)width, (LONG)height };
    uint32_t i;

    if (d->defaults)
        d->defaults->lpVtbl->Apply(d->defaults);
    for (i = 1; i < d->num_rts; i++)
        dev->lpVtbl->SetRenderTarget(dev, i, NULL);
    dev->lpVtbl->SetViewport(dev, &vp);
    dev->lpVtbl->SetScissorRect(dev, &sc);
    for (i = 0; i < d->nlights; i++)
        dev->lpVtbl->LightEnable(dev, d->lights[i], FALSE);
    d->nlights = 0;
    qd3dx_drop_stage_refs(d);
    /* Nothing is bound any more, and Reset ends a recording. */
    memset(d->lod_val, 0, sizeof d->lod_val);
    d->lod_mask = 0;
    d->recording = 0;
}

void qd3dx_dev_drop_state(struct qd3dx_dev *d)
{
    qd3dx_drop_stage_refs(d);
    if (d->defaults)
        d->defaults->lpVtbl->Release(d->defaults);
    d->defaults = NULL;
    free(d->palettes);
    d->palettes = NULL;
    free(d->lights);
    d->lights = NULL;
    d->nlights = d->cap_lights = 0;
}

HRESULT qd3dxo_IDirect3DDevice9_SetTexture(struct qd3dx *ex, IDirect3DDevice9 *This, DWORD Stage,
                                           IDirect3DBaseTexture9 *pTexture)
{
    struct qd3dx_dev *d = qd3dx_dev_find(ex, This);
    int i = qd3dx_stage_index(Stage);
    int sysmem = pTexture && qd3dx_texture_pool(pTexture) == D3DPOOL_SYSTEMMEM;
    IDirect3DBaseTexture9 *twin;
    HRESULT hr;

    if (i < 0)
        return D3D_OK; /* the native runtime answers so up to stage 0xfff, then crashes */
    twin = sysmem ? qd3dx_twin(This, pTexture) : NULL;
    hr = This->lpVtbl->SetTexture(This, Stage, sysmem ? twin : pTexture);
    if (SUCCEEDED(hr) && d) {
        if (d->sysmem_bound[i])
            d->sysmem_bound[i]->lpVtbl->Release(d->sysmem_bound[i]);
        d->sysmem_bound[i] = sysmem ? pTexture : NULL;
        d->stage_twin[i] = twin;
        if (sysmem)
            pTexture->lpVtbl->AddRef(pTexture);
        /* What the stage samples now (unchanged while a block records). */
        if (d->lod_used)
            qd3dx_lod_stage(d, i);
    }
    if (twin)
        twin->lpVtbl->Release(twin); /* the SYSTEMMEM texture keeps it */
    return hr;
}

HRESULT qd3dxo_IDirect3DDevice9_GetTexture(struct qd3dx *ex, IDirect3DDevice9 *This, DWORD Stage,
                                           IDirect3DBaseTexture9 **ppTexture)
{
    struct qd3dx_dev *d = qd3dx_dev_find(ex, This);
    int i = qd3dx_stage_index(Stage);
    HRESULT hr;

    if (i < 0)
        return D3DERR_INVALIDCALL;
    hr = This->lpVtbl->GetTexture(This, Stage, ppTexture);
    if (SUCCEEDED(hr) && ppTexture && d && d->sysmem_bound[i] && (const void *)*ppTexture == d->stage_twin[i]) {
        if (*ppTexture)
            (*ppTexture)->lpVtbl->Release(*ppTexture);
        *ppTexture = d->sysmem_bound[i];
        (*ppTexture)->lpVtbl->AddRef(*ppTexture);
    }
    return hr;
}

/* ------------------------------------------------------------------------ */
/* Index arguments                                                          */
/* ------------------------------------------------------------------------ */
/*
 * The retail Direct3D 9Ex runtime trusts these arguments and crashes, hangs
 * or corrupts its heap on out-of-range values (measured, Windows 11 64-bit,
 * without QD3D): SetLight/LightEnable from about 0x10000000, SetPaletteEntries
 * at 0x7fffffff (0xffffffff hangs), GetPaletteEntries and
 * SetCurrentTexturePalette on any palette never set, SetTexture/GetTexture
 * from stage 0x1000, SetStreamSource at stream 16 (heap corruption) and from
 * 33 (crash), GetRenderTarget from 0x100, Set/GetSamplerState with type
 * 0x10000, DrawIndexedPrimitiveUP with an index past its vertex data. Values
 * outside the documented ranges therefore never reach it: setters the
 * runtime answers D3D_OK for at such values answer that without effect,
 * the rest D3DERR_INVALIDCALL.
 */

#define QD3DX_LIGHTS   0x10000u /* light indices accepted */
#define QD3DX_PALETTES 0x10000u /* palette numbers (D3D9 allows 65536) */

static int qd3dx_tss_ok(DWORD stage, D3DTEXTURESTAGESTATETYPE type)
{
    if (stage >= 8u)
        return 0;
    switch (type) {
    case D3DTSS_COLOROP: case D3DTSS_COLORARG1: case D3DTSS_COLORARG2: case D3DTSS_ALPHAOP:
    case D3DTSS_ALPHAARG1: case D3DTSS_ALPHAARG2: case D3DTSS_BUMPENVMAT00: case D3DTSS_BUMPENVMAT01:
    case D3DTSS_BUMPENVMAT10: case D3DTSS_BUMPENVMAT11: case D3DTSS_TEXCOORDINDEX:
    case D3DTSS_BUMPENVLSCALE: case D3DTSS_BUMPENVLOFFSET: case D3DTSS_TEXTURETRANSFORMFLAGS:
    case D3DTSS_COLORARG0: case D3DTSS_ALPHAARG0: case D3DTSS_RESULTARG: case D3DTSS_CONSTANT:
        return 1;
    default:
        return 0;
    }
}

static int qd3dx_sampler_ok(DWORD sampler, D3DSAMPLERSTATETYPE type)
{
    return qd3dx_stage_index(sampler) >= 0 && type >= D3DSAMP_ADDRESSU && type <= D3DSAMP_DMAPOFFSET;
}

static int qd3dx_stream_ok(struct qd3dx *ex, IDirect3DDevice9 *dev, UINT stream)
{
    struct qd3dx_dev *d = qd3dx_dev_find(ex, dev);

    return d && stream < d->max_streams;
}

static int qd3dx_palette_set(const struct qd3dx_dev *d, UINT n)
{
    return d && n < QD3DX_PALETTES && d->palettes && (d->palettes[n / 8u] & (1u << (n % 8u)));
}

HRESULT qd3dxo_IDirect3DDevice9_SetTextureStageState(struct qd3dx *ex, IDirect3DDevice9 *This, DWORD Stage,
                                                     D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    if (!qd3dx_tss_ok(Stage, Type))
        return D3D_OK;
    return This->lpVtbl->SetTextureStageState(This, Stage, Type, Value);
}

HRESULT qd3dxo_IDirect3DDevice9_GetTextureStageState(struct qd3dx *ex, IDirect3DDevice9 *This, DWORD Stage,
                                                     D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
    if (!qd3dx_tss_ok(Stage, Type))
        return D3DERR_INVALIDCALL;
    return This->lpVtbl->GetTextureStageState(This, Stage, Type, pValue);
}

HRESULT qd3dxo_IDirect3DDevice9_SetSamplerState(struct qd3dx *ex, IDirect3DDevice9 *This, DWORD Sampler,
                                                D3DSAMPLERSTATETYPE Type, DWORD Value)
{
    if (!qd3dx_sampler_ok(Sampler, Type))
        return D3D_OK;
    return This->lpVtbl->SetSamplerState(This, Sampler, Type, Value);
}

HRESULT qd3dxo_IDirect3DDevice9_GetSamplerState(struct qd3dx *ex, IDirect3DDevice9 *This, DWORD Sampler,
                                                D3DSAMPLERSTATETYPE Type, DWORD *pValue)
{
    if (!qd3dx_sampler_ok(Sampler, Type))
        return D3DERR_INVALIDCALL;
    return This->lpVtbl->GetSamplerState(This, Sampler, Type, pValue);
}

HRESULT qd3dxo_IDirect3DDevice9_SetLight(struct qd3dx *ex, IDirect3DDevice9 *This, DWORD Index,
                                         const D3DLIGHT9 *pLight)
{
    if (Index >= QD3DX_LIGHTS)
        return D3DERR_INVALIDCALL;
    return This->lpVtbl->SetLight(This, Index, pLight);
}

HRESULT qd3dxo_IDirect3DDevice9_GetLight(struct qd3dx *ex, IDirect3DDevice9 *This, DWORD Index, D3DLIGHT9 *pLight)
{
    if (Index >= QD3DX_LIGHTS)
        return D3DERR_INVALIDCALL;
    return This->lpVtbl->GetLight(This, Index, pLight);
}

HRESULT qd3dxo_IDirect3DDevice9_LightEnable(struct qd3dx *ex, IDirect3DDevice9 *This, DWORD Index, BOOL Enable)
{
    struct qd3dx_dev *d = qd3dx_dev_find(ex, This);
    HRESULT hr;
    uint32_t i;

    if (Index >= QD3DX_LIGHTS || !d)
        return D3DERR_INVALIDCALL;
    hr = This->lpVtbl->LightEnable(This, Index, Enable);
    if (FAILED(hr) || !Enable)
        return hr;
    /* Remembered for Reset, which turns every light off. */
    for (i = 0; i < d->nlights && d->lights[i] != Index; i++)
        ;
    if (i == d->nlights) {
        if (d->nlights == d->cap_lights) {
            uint32_t cap = d->cap_lights ? d->cap_lights * 2u : 8u;
            uint32_t *l = realloc(d->lights, cap * sizeof *l);

            if (!l)
                return hr; /* the light stays on across Reset */
            d->lights = l;
            d->cap_lights = cap;
        }
        d->lights[d->nlights++] = Index;
    }
    return hr;
}

HRESULT qd3dxo_IDirect3DDevice9_GetLightEnable(struct qd3dx *ex, IDirect3DDevice9 *This, DWORD Index, BOOL *pEnable)
{
    if (Index >= QD3DX_LIGHTS)
        return D3DERR_INVALIDCALL;
    return This->lpVtbl->GetLightEnable(This, Index, pEnable);
}

HRESULT qd3dxo_IDirect3DDevice9_SetPaletteEntries(struct qd3dx *ex, IDirect3DDevice9 *This, UINT PaletteNumber,
                                                  const PALETTEENTRY *pEntries)
{
    struct qd3dx_dev *d = qd3dx_dev_find(ex, This);
    HRESULT hr;

    if (PaletteNumber >= QD3DX_PALETTES || !d)
        return D3DERR_INVALIDCALL;
    if (!d->palettes && !(d->palettes = calloc(QD3DX_PALETTES / 8u, 1)))
        return E_OUTOFMEMORY;
    hr = This->lpVtbl->SetPaletteEntries(This, PaletteNumber, pEntries);
    if (SUCCEEDED(hr))
        d->palettes[PaletteNumber / 8u] |= (uint8_t)(1u << (PaletteNumber % 8u));
    return hr;
}

HRESULT qd3dxo_IDirect3DDevice9_GetPaletteEntries(struct qd3dx *ex, IDirect3DDevice9 *This, UINT PaletteNumber,
                                                  PALETTEENTRY *pEntries)
{
    if (!qd3dx_palette_set(qd3dx_dev_find(ex, This), PaletteNumber))
        return D3DERR_INVALIDCALL;
    return This->lpVtbl->GetPaletteEntries(This, PaletteNumber, pEntries);
}

HRESULT qd3dxo_IDirect3DDevice9_SetCurrentTexturePalette(struct qd3dx *ex, IDirect3DDevice9 *This, UINT PaletteNumber)
{
    if (!qd3dx_palette_set(qd3dx_dev_find(ex, This), PaletteNumber))
        return D3DERR_INVALIDCALL;
    return This->lpVtbl->SetCurrentTexturePalette(This, PaletteNumber);
}

HRESULT qd3dxo_IDirect3DDevice9_SetStreamSource(struct qd3dx *ex, IDirect3DDevice9 *This, UINT StreamNumber,
                                                IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride)
{
    if (!qd3dx_stream_ok(ex, This, StreamNumber))
        return D3DERR_INVALIDCALL;
    return This->lpVtbl->SetStreamSource(This, StreamNumber, pStreamData, OffsetInBytes, Stride);
}

HRESULT qd3dxo_IDirect3DDevice9_GetStreamSource(struct qd3dx *ex, IDirect3DDevice9 *This, UINT StreamNumber,
                                                IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes,
                                                UINT *pStride)
{
    if (!qd3dx_stream_ok(ex, This, StreamNumber))
        return D3DERR_INVALIDCALL;
    return This->lpVtbl->GetStreamSource(This, StreamNumber, ppStreamData, pOffsetInBytes, pStride);
}

HRESULT qd3dxo_IDirect3DDevice9_SetStreamSourceFreq(struct qd3dx *ex, IDirect3DDevice9 *This, UINT StreamNumber,
                                                    UINT Setting)
{
    if (!qd3dx_stream_ok(ex, This, StreamNumber))
        return D3DERR_INVALIDCALL;
    return This->lpVtbl->SetStreamSourceFreq(This, StreamNumber, Setting);
}

HRESULT qd3dxo_IDirect3DDevice9_GetStreamSourceFreq(struct qd3dx *ex, IDirect3DDevice9 *This, UINT StreamNumber,
                                                    UINT *pSetting)
{
    if (!qd3dx_stream_ok(ex, This, StreamNumber))
        return D3DERR_INVALIDCALL;
    return This->lpVtbl->GetStreamSourceFreq(This, StreamNumber, pSetting);
}

HRESULT qd3dxo_IDirect3DDevice9_GetRenderTarget(struct qd3dx *ex, IDirect3DDevice9 *This, DWORD RenderTargetIndex,
                                                IDirect3DSurface9 **ppRenderTarget)
{
    struct qd3dx_dev *d = qd3dx_dev_find(ex, This);

    if (!d || RenderTargetIndex >= d->num_rts)
        return D3DERR_INVALIDCALL;
    return This->lpVtbl->GetRenderTarget(This, RenderTargetIndex, ppRenderTarget);
}

/* The payload was copied out of the ring before decoding (qd3dx_core.c), so
 * the indices checked here are the ones the runtime reads. */
HRESULT qd3dxo_IDirect3DDevice9_DrawIndexedPrimitiveUP(struct qd3dx *ex, IDirect3DDevice9 *This,
                                                       D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
                                                       UINT NumVertices, UINT PrimitiveCount, const void *pIndexData,
                                                       D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData,
                                                       UINT VertexStreamZeroStride)
{
    uint64_t count = qd3d_prim_vertex_count(PrimitiveType, PrimitiveCount), end = (uint64_t)MinVertexIndex + NumVertices;
    struct qd3dx_lod_saved s;
    uint64_t i;
    HRESULT hr;

    if (IndexDataFormat != D3DFMT_INDEX16 && IndexDataFormat != D3DFMT_INDEX32)
        return D3DERR_INVALIDCALL;
    if (pIndexData) {
        for (i = 0; i < count; i++) {
            uint32_t v;

            if (IndexDataFormat == D3DFMT_INDEX16) {
                uint16_t s16;

                memcpy(&s16, (const uint8_t *)pIndexData + i * 2u, 2);
                v = s16;
            } else {
                memcpy(&v, (const uint8_t *)pIndexData + i * 4u, 4);
            }
            if (v >= end)
                return D3DERR_INVALIDCALL;
        }
    }
    qd3dx_lod_begin(ex, This, &s);
    hr = This->lpVtbl->DrawIndexedPrimitiveUP(This, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount,
                                              pIndexData, IndexDataFormat, pVertexStreamZeroData,
                                              VertexStreamZeroStride);
    if (s.mask)
        qd3dx_lod_end(This, &s);
    return hr;
}

/* What the guest's screen shows of the device, into dest (A8R8G8B8 or
 * X8R8G8B8, lockable; its top-left is the screen's): the last published
 * frame at the guest screen rectangle it was presented to (scaled to it,
 * nearest texel), opaque black elsewhere, before the first present and
 * when the window showed nothing. Natively the front buffer of a windowed
 * device is the desktop, with the window's image at its client area. A
 * frame still in flight is published first; the cursor is never in it. */
static HRESULT qd3dx_front_buffer(struct qd3dx *ex, IDirect3DDevice9 *dev, IDirect3DSurface9 *dest)
{
    struct qd3dx_dev *d = qd3dx_dev_find(ex, dev);
    IDirect3DSurface9 *front = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr, fr;
    uint32_t *col = NULL;
    int64_t sl = 0, st = 0, sw = 0, sh = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0, x, y;
    uint32_t fw = 0, fh = 0;
    HRESULT hr;

    if (!dest || !d)
        return D3DERR_INVALIDCALL;
    hr = dest->lpVtbl->GetDesc(dest, &desc);
    if (FAILED(hr))
        return hr;
    if (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)
        return D3DERR_INVALIDCALL;
    qd3dx_present_flush(ex);
    hr = dest->lpVtbl->LockRect(dest, &lr, NULL, 0);
    if (FAILED(hr))
        return hr;
    if (lr.Pitch <= 0 || (uint64_t)lr.Pitch < (uint64_t)desc.Width * 4u) {
        dest->lpVtbl->UnlockRect(dest);
        return D3DERR_INVALIDCALL;
    }
    if (d->front >= 0) {
        sl = d->front_screen[0];
        st = d->front_screen[1];
        sw = (int64_t)d->front_screen[2] - sl;
        sh = (int64_t)d->front_screen[3] - st;
        x0 = sl > 0 ? sl : 0;
        y0 = st > 0 ? st : 0;
        x1 = sl + sw < (int64_t)desc.Width ? sl + sw : (int64_t)desc.Width;
        y1 = st + sh < (int64_t)desc.Height ? st + sh : (int64_t)desc.Height;
        if (sw > 0 && sh > 0 && x0 < x1 && y0 < y1)
            col = malloc((size_t)(x1 - x0) * sizeof *col);
        if (col && FAILED(d->copy[d->front]->lpVtbl->LockRect(d->copy[d->front], &fr, NULL, D3DLOCK_READONLY))) {
            free(col);
            col = NULL;
        }
        if (col) {
            front = d->copy[d->front];
            fw = d->copy_w[d->front];
            fh = d->copy_h[d->front];
            for (x = x0; x < x1; x++)
                col[x - x0] = (uint32_t)((x - sl) * fw / sw);
        }
    }
    for (y = 0; y < (int64_t)desc.Height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch);

        for (x = 0; x < (int64_t)desc.Width; x++)
            row[x] = 0xff000000u;
        if (front && y >= y0 && y < y1) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)fr.pBits +
                                                     (size_t)((y - st) * fh / sh) * (size_t)fr.Pitch);

            for (x = x0; x < x1; x++)
                row[x] = src[col[x - x0]] | 0xff000000u;
        }
    }
    if (front)
        front->lpVtbl->UnlockRect(front);
    free(col);
    return dest->lpVtbl->UnlockRect(dest);
}

HRESULT qd3dxo_IDirect3DDevice9_GetFrontBufferData(struct qd3dx *ex, IDirect3DDevice9 *This, UINT iSwapChain,
                                                   IDirect3DSurface9 *pDestSurface)
{
    if (iSwapChain >= This->lpVtbl->GetNumberOfSwapChains(This))
        return D3DERR_INVALIDCALL;
    return qd3dx_front_buffer(ex, This, pDestSurface);
}

HRESULT qd3dxo_IDirect3DSwapChain9_GetFrontBufferData(struct qd3dx *ex, IDirect3DSwapChain9 *This,
                                                      IDirect3DSurface9 *pDestSurface)
{
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = This->lpVtbl->GetDevice(This, &dev);

    if (FAILED(hr))
        return hr;
    hr = qd3dx_front_buffer(ex, dev, pDestSurface);
    dev->lpVtbl->Release(dev);
    return hr;
}

/* Native ValidateDevice results that are answers rather than "cannot say". */
static int qd3dx_validate_verdict(HRESULT hr)
{
    switch (hr) {
    case D3DERR_WRONGTEXTUREFORMAT:
    case D3DERR_UNSUPPORTEDCOLOROPERATION:
    case D3DERR_UNSUPPORTEDCOLORARG:
    case D3DERR_UNSUPPORTEDALPHAOPERATION:
    case D3DERR_UNSUPPORTEDALPHAARG:
    case D3DERR_TOOMANYOPERATIONS:
    case D3DERR_CONFLICTINGTEXTUREFILTER:
    case D3DERR_UNSUPPORTEDFACTORVALUE:
    case D3DERR_CONFLICTINGRENDERSTATE:
    case D3DERR_UNSUPPORTEDTEXTUREFILTER:
    case D3DERR_CONFLICTINGTEXTUREPALETTE:
    case D3DERR_DEVICELOST:
    case D3DERR_INVALIDCALL:
        return 1;
    default:
        return 0;
    }
}

static int qd3dx_top_supported(const D3DCAPS9 *caps, DWORD op)
{
    return op >= D3DTOP_DISABLE && op <= D3DTOP_LERP && (caps->TextureOpCaps & (1ul << (op - 1u)));
}

/* Arguments an operation reads: bit 0 ARG0, bit 1 ARG1, bit 2 ARG2. */
static unsigned qd3dx_top_args(DWORD op)
{
    switch (op) {
    case D3DTOP_SELECTARG1:
        return 2u;
    case D3DTOP_SELECTARG2:
        return 4u;
    case D3DTOP_MULTIPLYADD:
    case D3DTOP_LERP:
        return 7u;
    default:
        return 6u;
    }
}

static int qd3dx_targ_supported(const D3DCAPS9 *caps, DWORD arg)
{
    DWORD sel = arg & D3DTA_SELECTMASK;

    if (arg & ~(DWORD)(D3DTA_SELECTMASK | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE))
        return 0;
    if (sel > D3DTA_CONSTANT)
        return 0;
    if (sel == D3DTA_TEMP && !(caps->PrimitiveMiscCaps & D3DPMISCCAPS_TSSARGTEMP))
        return 0;
    if (sel == D3DTA_CONSTANT && !(caps->PrimitiveMiscCaps & D3DPMISCCAPS_PERSTAGECONSTANT))
        return 0;
    return 1;
}

/* Checks the operation and arguments of one blend unit (color or alpha);
 * sets *samples when the unit reads the stage texture. */
static int qd3dx_unit_supported(IDirect3DDevice9 *dev, const D3DCAPS9 *caps, DWORD stage, DWORD op,
                                const D3DTEXTURESTAGESTATETYPE args[3], int *samples, int *arg_ok)
{
    unsigned used = qd3dx_top_args(op), i;

    *arg_ok = 1;
    if (!qd3dx_top_supported(caps, op))
        return 0;
    if (op == D3DTOP_BUMPENVMAP || op == D3DTOP_BUMPENVMAPLUMINANCE)
        *samples = 1;
    for (i = 0; i < 3u; i++) {
        DWORD arg = 0;

        if (!(used & (1u << i)))
            continue;
        dev->lpVtbl->GetTextureStageState(dev, stage, args[i], &arg);
        if (!qd3dx_targ_supported(caps, arg))
            *arg_ok = 0;
        else if ((arg & D3DTA_SELECTMASK) == D3DTA_TEXTURE)
            *samples = 1;
    }
    return 1;
}

/* The fixed-function texture stage cascade against the device caps. */
static HRESULT qd3dx_validate_stages(IDirect3DDevice9 *dev, DWORD *stage_out)
{
    static const D3DTEXTURESTAGESTATETYPE color_args[3] = { D3DTSS_COLORARG0, D3DTSS_COLORARG1,
                                                            D3DTSS_COLORARG2 };
    static const D3DTEXTURESTAGESTATETYPE alpha_args[3] = { D3DTSS_ALPHAARG0, D3DTSS_ALPHAARG1,
                                                            D3DTSS_ALPHAARG2 };
    IDirect3DPixelShader9 *ps = NULL;
    D3DCAPS9 caps;
    DWORD stage, textures = 0;
    HRESULT hr;

    *stage_out = 0;
    hr = dev->lpVtbl->GetDeviceCaps(dev, &caps);
    if (FAILED(hr))
        return hr;
    /* A pixel shader replaces the stage cascade. */
    if (SUCCEEDED(dev->lpVtbl->GetPixelShader(dev, &ps)) && ps) {
        ps->lpVtbl->Release(ps);
        return D3D_OK;
    }
    for (stage = 0; stage < 8u; stage++) {
        DWORD cop = D3DTOP_DISABLE, aop = D3DTOP_DISABLE, result = D3DTA_CURRENT;
        int samples = 0, arg_ok;

        *stage_out = stage;
        dev->lpVtbl->GetTextureStageState(dev, stage, D3DTSS_COLOROP, &cop);
        if (cop == D3DTOP_DISABLE)
            break;
        if (stage >= caps.MaxTextureBlendStages)
            return D3DERR_TOOMANYOPERATIONS;
        if (!qd3dx_unit_supported(dev, &caps, stage, cop, color_args, &samples, &arg_ok))
            return D3DERR_UNSUPPORTEDCOLOROPERATION;
        if (!arg_ok)
            return D3DERR_UNSUPPORTEDCOLORARG;
        dev->lpVtbl->GetTextureStageState(dev, stage, D3DTSS_ALPHAOP, &aop);
        if (aop != D3DTOP_DISABLE) {
            if (!qd3dx_unit_supported(dev, &caps, stage, aop, alpha_args, &samples, &arg_ok))
                return D3DERR_UNSUPPORTEDALPHAOPERATION;
            if (!arg_ok)
                return D3DERR_UNSUPPORTEDALPHAARG;
        }
        dev->lpVtbl->GetTextureStageState(dev, stage, D3DTSS_RESULTARG, &result);
        if (result != D3DTA_CURRENT &&
            !(result == D3DTA_TEMP && (caps.PrimitiveMiscCaps & D3DPMISCCAPS_TSSARGTEMP)))
            return D3DERR_UNSUPPORTEDCOLORARG;
        if (samples && ++textures > caps.MaxSimultaneousTextures)
            return D3DERR_TOOMANYOPERATIONS;
    }
    return D3D_OK;
}

HRESULT qd3dxo_IDirect3DDevice9_ValidateDevice(struct qd3dx *ex, IDirect3DDevice9 *This, DWORD *pNumPasses)
{
    static int noted;
    HRESULT hr = This->lpVtbl->ValidateDevice(This, pNumPasses);
    DWORD stage;

    if (SUCCEEDED(hr) || qd3dx_validate_verdict(hr) || !pNumPasses)
        return hr;
    if (!noted) {
        noted = 1;
        qd3dx_log(ex, "qd3dx: native ValidateDevice answered 0x%08lx; validating against the device caps",
                  (unsigned long)hr);
    }
    hr = qd3dx_validate_stages(This, &stage);
    if (FAILED(hr)) {
        qd3dx_warn(ex, "qd3dx: ValidateDevice: stage %lu unsupported (0x%08lx)", (unsigned long)stage,
                   (unsigned long)hr);
        return hr;
    }
    *pNumPasses = 1;
    return D3D_OK;
}
