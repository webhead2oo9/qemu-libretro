/*
 * Compat stub: MSHV (Microsoft Hypervisor / Linux) does not exist on the
 * 9.0.50 base. Upstream's shared emulator helpers include this header;
 * nothing from it is used in the WHPX build.
 */
#ifndef QEMU_SYSTEM_MSHV_STUB_H
#define QEMU_SYSTEM_MSHV_STUB_H
#define mshv_enabled() 0
#endif
