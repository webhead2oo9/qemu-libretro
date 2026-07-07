/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX) support
 *
 * Compat forwarder: the real header lives at system/whpx.h (upstream
 * moved include/sysemu -> include/system in QEMU 10). Kept so 9.0-era
 * callers that include "sysemu/whpx.h" keep building.
 */

#ifndef QEMU_SYSEMU_WHPX_COMPAT_H
#define QEMU_SYSEMU_WHPX_COMPAT_H

#include "system/whpx.h"

/* 9.0-era API name; upstream renamed the concept to irqchip-in-kernel */
#define whpx_apic_in_platform() (whpx_irqchip_in_kernel())

#endif /* QEMU_SYSEMU_WHPX_COMPAT_H */
