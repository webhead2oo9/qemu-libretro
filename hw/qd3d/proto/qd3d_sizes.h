/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Size helpers shared by the guest proxy (to size DATA items it reads from
 * application memory) and the host executor (to validate DATA items before
 * native Direct3D reads them). The *_bounded variants never read past the
 * supplied byte count and return 0 for malformed input.
 */
#ifndef QD3D_SIZES_H
#define QD3D_SIZES_H

#include <stdint.h>
#include <d3d9.h>

static inline uint64_t qd3d_prim_vertex_count(D3DPRIMITIVETYPE type, uint32_t prims)
{
    switch (type) {
    case D3DPT_POINTLIST:     return prims;
    case D3DPT_LINELIST:      return 2ull * prims;
    case D3DPT_LINESTRIP:     return prims ? (uint64_t)prims + 1u : 0u;
    case D3DPT_TRIANGLELIST:  return 3ull * prims;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   return prims ? (uint64_t)prims + 2u : 0u;
    default:                  return 0u;
    }
}

static inline uint64_t qd3d_vertex_data_size(D3DPRIMITIVETYPE type, uint32_t prims, uint32_t stride)
{
    return qd3d_prim_vertex_count(type, prims) * (uint64_t)stride;
}

static inline uint64_t qd3d_index_data_size(D3DPRIMITIVETYPE type, uint32_t prims, D3DFORMAT format)
{
    return qd3d_prim_vertex_count(type, prims) * (format == D3DFMT_INDEX32 ? 4u : 2u);
}

/*
 * Length in bytes of a Direct3D 9 shader token stream, end token included.
 * Shader model 2.0+ instruction tokens carry their length in bits 24-27.
 * Shader model 1.x does not; there every parameter token has bit 31 set,
 * except the four raw constants of def. Comments (low word 0xfffe) carry
 * their DWORD count in bits 16-30. Returns 0 when no end token is reached
 * within max_bytes or a length runs past it.
 */
#define QD3D_SIO_DEF     0x51u
#define QD3D_SIO_COMMENT 0xfffeu
#define QD3D_SIO_END     0x0000ffffu

static inline uint32_t qd3d_shader_size_bounded(const void *code, uint32_t max_bytes)
{
    const uint32_t *t = (const uint32_t *)code;
    uint32_t n = max_bytes / 4u;
    uint32_t i, major;

    if (!code || n < 2u)
        return 0u;
    if ((t[0] & 0xfffe0000u) != 0xfffe0000u)  /* vs 0xfffe...., ps 0xffff.... */
        return 0u;
    major = (t[0] >> 8) & 0xffu;
    i = 1u;
    while (i < n) {
        uint32_t tok = t[i];
        if (tok == QD3D_SIO_END)
            return (i + 1u) * 4u;
        if ((tok & 0xffffu) == QD3D_SIO_COMMENT) {
            uint32_t len = (tok >> 16) & 0x7fffu;
            if (len > n - i - 1u)
                return 0u;
            i += 1u + len;
        } else if (major >= 2u) {
            uint32_t len = (tok >> 24) & 0x0fu;
            if (len > n - i - 1u)
                return 0u;
            i += 1u + len;
        } else if ((tok & 0xffffu) == QD3D_SIO_DEF) {
            if (5u > n - i - 1u)
                return 0u;
            i += 6u;
        } else {
            i++;
            while (i < n && (t[i] & 0x80000000u))
                i++;
        }
    }
    return 0u;
}

/* Unbounded form for the guest, which reads application memory that the
 * runtime contract says is a complete shader. */
static inline uint32_t qd3d_shader_size(const DWORD *code)
{
    return qd3d_shader_size_bounded(code, 0x01000000u);
}

/* Number of D3DVERTEXELEMENT9 entries including D3DDECL_END, or 0 when no
 * terminator occurs within max_elems. */
static inline uint32_t qd3d_decl_count_bounded(const D3DVERTEXELEMENT9 *e, uint32_t max_elems)
{
    uint32_t i;

    if (!e)
        return 0u;
    for (i = 0; i < max_elems; i++)
        if (e[i].Stream == 0xffu)
            return i + 1u;
    return 0u;
}

static inline uint32_t qd3d_decl_count(const D3DVERTEXELEMENT9 *e)
{
    return qd3d_decl_count_bounded(e, 65u); /* MAXD3DDECLLENGTH + end */
}

#endif /* QD3D_SIZES_H */
