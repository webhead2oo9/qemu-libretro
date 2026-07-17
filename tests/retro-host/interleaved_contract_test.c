#include <stdio.h>

#include "../../hw/mesa/mesapt_interleaved.h"

static int require(int condition, const char *message)
{
    if (condition) return 1;
    fprintf(stderr, "interleaved_contract_test: %s\n", message);
    return 0;
}

int main(void)
{
    MesaptInterleavedArray arrays[3] = { 0 };
    MesaptInterleavedLayout layout;

    arrays[0].enabled = true;
    arrays[0].handle = MESAPT_D3D_INTERLEAVED_BASE;
    arrays[0].stride = 32;
    arrays[0].element_bytes = 16;
    arrays[1].enabled = true;
    arrays[1].handle = MESAPT_D3D_INTERLEAVED_BASE + 16;
    arrays[1].stride = 32;
    arrays[1].element_bytes = 4;
    arrays[2].enabled = true;
    arrays[2].handle = MESAPT_D3D_INTERLEAVED_BASE + 24;
    arrays[2].stride = 32;
    arrays[2].element_bytes = 8;
    if (!require(mesapt_interleaved_layout(
                     arrays, 3, 0, 2, 65536, &layout) == 1,
                 "rejected valid shared layout") ||
        !require(layout.stride == 32 && layout.destination_offset == 0 &&
                 layout.copy_bytes == 96 && layout.consumed_bytes == 96,
                 "wrong shared layout result")) return 1;

    arrays[2].stride = 28;
    if (!require(mesapt_interleaved_layout(
                     arrays, 3, 0, 2, 65536, &layout) == -1,
                 "accepted mixed strides")) return 1;
    arrays[2].stride = 32;
    arrays[2].handle = MESAPT_D3D_INTERLEAVED_BASE + 30;
    if (!require(mesapt_interleaved_layout(
                     arrays, 3, 0, 2, 65536, &layout) == -1,
                 "accepted element beyond stride")) return 1;
    arrays[2].handle = 0x72000000U;
    if (!require(mesapt_interleaved_layout(
                     arrays, 3, 0, 2, 65536, &layout) == -1,
                 "accepted mixed private ranges")) return 1;
    arrays[0].handle = 0x72000000U;
    arrays[1].handle = 0x72000010U;
    if (!require(mesapt_interleaved_layout(
                     arrays, 3, 0, 2, 65536, &layout) == 0,
                 "claimed a legacy layout")) return 1;
    arrays[0].handle = MESAPT_D3D_INTERLEAVED_BASE;
    arrays[1].handle = MESAPT_D3D_INTERLEAVED_BASE + 16;
    arrays[2].handle = MESAPT_D3D_INTERLEAVED_BASE + 24;
    if (!require(mesapt_interleaved_layout(
                     arrays, 3, 0, 2048, 65536, &layout) == -1,
                 "accepted cache overflow")) return 1;
    if (!require(mesapt_interleaved_layout(
                     arrays, 3, -1, 2, 65536, &layout) == -1,
                 "accepted negative start")) return 1;

    puts("interleaved_contract_test: ok");
    return 0;
}
