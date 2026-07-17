#ifndef MESAPT_INTERLEAVED_H
#define MESAPT_INTERLEAVED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESAPT_D3D_INTERLEAVED_BASE 0x73000000U
#define MESAPT_D3D_INTERLEAVED_SPAN 0x00010000U

typedef struct MesaptInterleavedArray {
    bool enabled;
    uint32_t handle;
    uint32_t stride;
    uint32_t element_bytes;
} MesaptInterleavedArray;

typedef struct MesaptInterleavedLayout {
    uint32_t stride;
    size_t destination_offset;
    size_t copy_bytes;
    size_t consumed_bytes;
} MesaptInterleavedLayout;

/* Returns 1 for a valid shared-interleaved submission, 0 when no enabled
 * array uses the reserved range, and -1 for a malformed reserved-range use. */
static inline int mesapt_interleaved_layout(
    const MesaptInterleavedArray *arrays, size_t array_count,
    int start, int end, size_t cache_bytes,
    MesaptInterleavedLayout *layout)
{
    uint32_t stride = 0;
    size_t vertex_count;
    size_t destination_offset;
    size_t copy_bytes;
    bool candidate = false;
    size_t index;

    if (!arrays || !layout || start < 0 || end < start) return -1;
    for (index = 0; index < array_count; ++index) {
        if (arrays[index].enabled &&
            arrays[index].handle >= MESAPT_D3D_INTERLEAVED_BASE &&
            arrays[index].handle < MESAPT_D3D_INTERLEAVED_BASE +
                                      MESAPT_D3D_INTERLEAVED_SPAN) {
            candidate = true;
            break;
        }
    }
    if (!candidate) return 0;
    for (index = 0; index < array_count; ++index) {
        uint32_t offset;

        if (!arrays[index].enabled) continue;
        if (arrays[index].handle < MESAPT_D3D_INTERLEAVED_BASE ||
            arrays[index].handle >= MESAPT_D3D_INTERLEAVED_BASE +
                                      MESAPT_D3D_INTERLEAVED_SPAN ||
            !arrays[index].stride || !arrays[index].element_bytes)
            return -1;
        if (!stride) stride = arrays[index].stride;
        if (arrays[index].stride != stride) return -1;
        offset = arrays[index].handle - MESAPT_D3D_INTERLEAVED_BASE;
        if (offset >= stride || arrays[index].element_bytes > stride - offset)
            return -1;
    }
    if (!stride) return -1;
    vertex_count = (size_t)(end - start) + 1U;
    if (vertex_count > SIZE_MAX / stride ||
        (size_t)start > SIZE_MAX / stride) return -1;
    destination_offset = (size_t)start * stride;
    copy_bytes = vertex_count * stride;
    if (destination_offset > cache_bytes / 2U ||
        copy_bytes > cache_bytes / 2U - destination_offset)
        return -1;
    layout->stride = stride;
    layout->destination_offset = destination_offset;
    layout->copy_bytes = copy_bytes;
    layout->consumed_bytes = (copy_bytes + 7U) & ~(size_t)7U;
    return 1;
}

#endif
