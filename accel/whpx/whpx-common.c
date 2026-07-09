/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX)
 *
 * Copyright Microsoft Corp. 2017
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "system/address-spaces.h"
#include "system/ioport.h"
#include "gdbstub/helpers.h"
#include "qemu/accel.h"
#include "accel/accel-ops.h"
#include "system/memory.h"
#include "system/whpx.h"
#include "system/cpus.h"
#include "system/runstate.h"
#include "qemu/main-loop.h"
#include "hw/core/boards.h"
#include "hw/intc/ioapic.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/qapi-types-common.h"
#include "qapi/qapi-visit-common.h"
#include "migration/blocker.h"
#include "accel/accel-cpu-target.h"
#include <winerror.h>

#include "system/whpx-internal.h"
#include "system/whpx-accel-ops.h"
#include "system/whpx-common.h"
#include "system/whpx-all.h"

#include <winhvplatform.h>
#include <winhvplatformdefs.h>

bool whpx_allowed;
bool whpx_irqchip_in_kernel;
static bool whp_dispatch_initialized;
static HMODULE hWinHvPlatform;

struct whpx_state whpx_global;
struct WHPDispatch whp_dispatch;

void whpx_flush_cpu_state(CPUState *cpu)
{
    if (cpu->vcpu_dirty) {
        whpx_set_registers(cpu, WHPX_LEVEL_RUNTIME_STATE);
        cpu->vcpu_dirty = false;
    }
}

void whpx_get_reg(CPUState *cpu, WHV_REGISTER_NAME reg, WHV_REGISTER_VALUE* val)
{
    struct whpx_state *whpx = &whpx_global;
    HRESULT hr;

    whpx_flush_cpu_state(cpu);

    hr = whp_dispatch.WHvGetVirtualProcessorRegisters(whpx->partition, cpu->cpu_index,
         &reg, 1, val);

    if (FAILED(hr)) {
        error_report("WHPX: Failed to get register %08x, hr=%08lx", reg, hr);
    }
}

void whpx_set_reg(CPUState *cpu, WHV_REGISTER_NAME reg, WHV_REGISTER_VALUE val)
{
    struct whpx_state *whpx = &whpx_global;
    HRESULT hr;
    hr = whp_dispatch.WHvSetVirtualProcessorRegisters(whpx->partition, cpu->cpu_index,
         &reg, 1, &val);

    if (FAILED(hr)) {
        error_report("WHPX: Failed to set register %08x, hr=%08lx", reg, hr);
    }
}

/* Tries to find a breakpoint at the specified address. */
struct whpx_breakpoint *whpx_lookup_breakpoint_by_addr(uint64_t address)
{
    struct whpx_state *whpx = &whpx_global;
    int i;

    if (whpx->breakpoints.breakpoints) {
        for (i = 0; i < whpx->breakpoints.breakpoints->used; i++) {
            if (address == whpx->breakpoints.breakpoints->data[i].address) {
                return &whpx->breakpoints.breakpoints->data[i];
            }
        }
    }

    return NULL;
}

/*
 * This function is called when the a VCPU is about to start and no other
 * VCPUs have been started so far. Since the VCPU start order could be
 * arbitrary, it doesn't have to be VCPU#0.
 *
 * It is used to commit the breakpoints into memory, and configure WHPX
 * to intercept debug exceptions.
 *
 * Note that whpx_set_exception_exit_bitmap() cannot be called if one or
 * more VCPUs are already running, so this is the best place to do it.
 */
int whpx_first_vcpu_starting(CPUState *cpu)
{
    struct whpx_state *whpx = &whpx_global;

    g_assert(bql_locked());

    if (!QTAILQ_EMPTY(&cpu->breakpoints) ||
            (whpx->breakpoints.breakpoints &&
             whpx->breakpoints.breakpoints->used)) {
        CPUBreakpoint *bp;
        int i = 0;
        bool update_pending = false;

        QTAILQ_FOREACH(bp, &cpu->breakpoints, entry) {
            if (i >= whpx->breakpoints.original_address_count ||
                bp->pc != whpx->breakpoints.original_addresses[i]) {
                update_pending = true;
            }

            i++;
        }

        if (i != whpx->breakpoints.original_address_count) {
            update_pending = true;
        }

        if (update_pending) {
            /*
             * The CPU breakpoints have changed since the last call to
             * whpx_translate_cpu_breakpoints(). WHPX breakpoints must
             * now be recomputed.
             */
            whpx_translate_cpu_breakpoints(&whpx->breakpoints, cpu, i);
        }
        /* Actually insert the breakpoints into the memory. */
        whpx_apply_breakpoints(whpx->breakpoints.breakpoints, cpu, true);
    }
    HRESULT hr;
    uint64_t exception_mask;
    if (whpx->step_pending ||
        (whpx->breakpoints.breakpoints &&
         whpx->breakpoints.breakpoints->used)) {
        /*
         * We are either attempting to single-step one or more CPUs, or
         * have one or more breakpoints enabled. Both require intercepting
         * the WHvX64ExceptionTypeBreakpointTrap exception.
         */
        exception_mask = 1UL << WHPX_INTERCEPT_DEBUG_TRAPS;
    } else {
        /* Let the guest handle all exceptions. */
        exception_mask = 0;
    }
    hr = whpx_set_exception_exit_bitmap(exception_mask);
    if (!SUCCEEDED(hr)) {
        error_report("WHPX: Failed to update exception exit mask,"
                     "hr=%08lx.", hr);
        return 1;
    }
    return 0;
}

/*
 * This function is called when the last VCPU has finished running.
 * It is used to remove any previously set breakpoints from memory.
 */
int whpx_last_vcpu_stopping(CPUState *cpu)
{
    whpx_apply_breakpoints(whpx_global.breakpoints.breakpoints, cpu, false);
    return 0;
}

static void do_whpx_cpu_synchronize_state(CPUState *cpu, run_on_cpu_data arg)
{
    if (!cpu->vcpu_dirty) {
        whpx_get_registers(cpu, WHPX_LEVEL_FULL_STATE);
        cpu->vcpu_dirty = true;
    }
}

static void do_whpx_cpu_synchronize_post_reset(CPUState *cpu,
                                               run_on_cpu_data arg)
{
    whpx_set_registers(cpu, WHPX_LEVEL_RESET_STATE);
    cpu->vcpu_dirty = false;
}

static void do_whpx_cpu_synchronize_post_init(CPUState *cpu,
                                              run_on_cpu_data arg)
{
    whpx_set_registers(cpu, WHPX_LEVEL_FULL_STATE);
    cpu->vcpu_dirty = false;
}

static void do_whpx_cpu_synchronize_pre_loadvm(CPUState *cpu,
                                               run_on_cpu_data arg)
{
    cpu->vcpu_dirty = true;
}

/*
 * CPU support.
 */

void whpx_cpu_synchronize_state(CPUState *cpu)
{
    if (!cpu->vcpu_dirty) {
        run_on_cpu(cpu, do_whpx_cpu_synchronize_state, RUN_ON_CPU_NULL);
    }
}

void whpx_cpu_synchronize_post_reset(CPUState *cpu)
{
    run_on_cpu(cpu, do_whpx_cpu_synchronize_post_reset, RUN_ON_CPU_NULL);
}

void whpx_cpu_synchronize_post_init(CPUState *cpu)
{
    run_on_cpu(cpu, do_whpx_cpu_synchronize_post_init, RUN_ON_CPU_NULL);
}

void whpx_cpu_synchronize_pre_loadvm(CPUState *cpu)
{
    run_on_cpu(cpu, do_whpx_cpu_synchronize_pre_loadvm, RUN_ON_CPU_NULL);
}

/* 9.0 base: registered as AccelOpsClass::synchronize_pre_resume (no AccelState) */
void whpx_pre_resume_vm(bool step_pending)
{
    whpx_global.step_pending = step_pending;
}

/*
 * Vcpu support.
 */

int whpx_vcpu_exec(CPUState *cpu)
{
    int ret;
    int fatal;

    for (;;) {
        if (cpu->exception_index >= EXCP_INTERRUPT) {
            ret = cpu->exception_index;
            cpu->exception_index = -1;
            break;
        }

        fatal = whpx_vcpu_run(cpu);

        if (fatal) {
            error_report("WHPX: Failed to exec a virtual processor");
            abort();
        }
    }

    return ret;
}

void whpx_destroy_vcpu(CPUState *cpu)
{
    struct whpx_state *whpx = &whpx_global;

    whp_dispatch.WHvDeleteVirtualProcessor(whpx->partition, cpu->cpu_index);
    whpx_arch_destroy_vcpu(cpu);
    g_free(cpu->accel);
}


void whpx_vcpu_kick(CPUState *cpu)
{
    struct whpx_state *whpx = &whpx_global;
    whp_dispatch.WHvCancelRunVirtualProcessor(
        whpx->partition, cpu->cpu_index, 0);
}

/*
 * Memory support.
 */

/*
 * Sections mapped with WHvMapGpaRangeFlagTrackDirtyPages: page-aligned
 * writable RAM, while tracking is enabled. Must mirror the mapping
 * conditions in whpx_set_phys_mem(). If tracking got disabled after a
 * section was already mapped with the flag, this under-reports — safe,
 * because a false result only means falling back to all-dirty.
 */
static bool whpx_section_tracked(MemoryRegionSection *section)
{
    MemoryRegion *area = section->mr;
    uint64_t page_size = qemu_real_host_page_size();

    return whpx_global.dirty_page_tracking &&
           memory_region_is_ram(area) &&
           !area->readonly && !area->rom_device &&
           QEMU_IS_ALIGNED(int128_get64(section->size), page_size) &&
           QEMU_IS_ALIGNED(section->offset_within_address_space, page_size);
}

/*
 * Read-and-reset the hypervisor's dirty bitmap for the section and
 * forward it, run-coalesced, into QEMU's dirty bitmap. A failing query
 * degrades to marking the whole section dirty.
 *
 * cpu_physical_memory_set_dirty_lebitmap() would apply the bitmap in
 * one call, but it is compiled out on Windows hosts (ram_addr.h), so
 * runs of set pages go through memory_region_set_dirty() instead.
 */
static void whpx_query_and_apply_dirty(MemoryRegionSection *section)
{
    struct whpx_state *whpx = &whpx_global;
    MemoryRegion *mr = section->mr;
    uint64_t page_size = qemu_real_host_page_size();
    uint64_t gpa = section->offset_within_address_space;
    uint64_t size = int128_get64(section->size);
    uint64_t npages = size / page_size;
    uint32_t bitmap_size = ROUND_UP(npages, 64) / 8;
    uint64_t page, run_start = 0;
    bool in_run = false;
    HRESULT hr;

    if (bitmap_size > whpx->dirty_bitmap_size) {
        whpx->dirty_bitmap = g_realloc(whpx->dirty_bitmap, bitmap_size);
        whpx->dirty_bitmap_size = bitmap_size;
    }

    hr = whp_dispatch.WHvQueryGpaRangeDirtyBitmap(whpx->partition, gpa, size,
                                                  whpx->dirty_bitmap,
                                                  bitmap_size);
    if (FAILED(hr)) {
        warn_report_once("WHPX: dirty bitmap query failed, hr=%08lx; "
                         "marking range fully dirty", hr);
        memory_region_set_dirty(mr, section->offset_within_region, size);
        return;
    }

    page = 0;
    while (page < npages) {
        if (!in_run && (page % 64) == 0 && whpx->dirty_bitmap[page / 64] == 0) {
            page += 64;
            continue;
        }
        if (whpx->dirty_bitmap[page / 64] & (1ULL << (page % 64))) {
            if (!in_run) {
                run_start = page;
                in_run = true;
            }
        } else if (in_run) {
            memory_region_set_dirty(mr,
                section->offset_within_region + run_start * page_size,
                (page - run_start) * page_size);
            in_run = false;
        }
        page++;
    }
    if (in_run) {
        memory_region_set_dirty(mr,
            section->offset_within_region + run_start * page_size,
            (npages - run_start) * page_size);
    }
}

static void whpx_set_phys_mem(MemoryRegionSection *section, bool add)
{
    struct whpx_state *whpx = &whpx_global;
    MemoryRegion *area = section->mr;
    bool writable = !area->readonly && !area->rom_device;
    WHV_MAP_GPA_RANGE_FLAGS flags;
    uint64_t page_size = qemu_real_host_page_size();
    uint64_t gva = section->offset_within_address_space;
    uint64_t size = int128_get64(section->size);
    HRESULT hr;
    void *mem;

    if (!memory_region_is_ram(area)) {
        if (writable) {
            return;
        } else if (!memory_region_is_romd(area)) {
            add = false;
        }
    }

    if (!QEMU_IS_ALIGNED(size, page_size) ||
        !QEMU_IS_ALIGNED(gva, page_size)) {
        /* Not page aligned, so we can not map as RAM */
        add = false;
    }

    if (!add) {
        /*
         * Harvest pending dirty info before the mapping (and the
         * hypervisor-side bitmap with it) disappears: VGA bank
         * switching unmaps windows wholesale, and writes since the
         * last sync would otherwise be lost.
         */
        if (whpx_section_tracked(section)) {
            whpx_query_and_apply_dirty(section);
        }
        hr = whp_dispatch.WHvUnmapGpaRange(whpx->partition,
                gva, size);
        if (FAILED(hr)) {
            error_report("WHPX: failed to unmap GPA range");
            abort();
        }
        return;
    }

    flags = WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute
     | (writable ? WHvMapGpaRangeFlagWrite : 0);
    if (writable && whpx->dirty_page_tracking &&
        memory_region_is_ram(area)) {
        flags |= WHvMapGpaRangeFlagTrackDirtyPages;
    }
    mem = memory_region_get_ram_ptr(area) + section->offset_within_region;

    hr = whp_dispatch.WHvMapGpaRange(whpx->partition,
         mem, gva, size, flags);
    if (FAILED(hr) && (flags & WHvMapGpaRangeFlagTrackDirtyPages)) {
        /*
         * Sticky fallback: some hypervisor versions accept the
         * capability query but reject the flag. Without it every
         * section reads as untracked and log_sync degrades to
         * all-dirty, which is always correct.
         */
        warn_report("WHPX: TrackDirtyPages mapping rejected, hr=%08lx; "
                    "disabling dirty-page tracking", hr);
        whpx->dirty_page_tracking = false;
        flags &= ~WHvMapGpaRangeFlagTrackDirtyPages;
        hr = whp_dispatch.WHvMapGpaRange(whpx->partition,
             mem, gva, size, flags);
    }
    if (FAILED(hr)) {
        error_report("WHPX: failed to map GPA range");
        abort();
    }

    /*
     * A fresh mapping starts with a clean hypervisor bitmap whatever
     * the memory contains; make the next sync treat it as fully dirty
     * so consumers (VGA) re-read it.
     */
    if (writable && memory_region_is_ram(area)) {
        memory_region_set_dirty(area, section->offset_within_region, size);
    }
}

#ifdef CONFIG_QEMU_3DFX
/*
 * qemu-3dfx (hw/3dfx, hw/mesa): map/unmap a raw host VA range into guest
 * PA space for the LFB write-merge buffer. Unlike whpx_set_phys_mem()
 * there is no MemoryRegion behind the range, so map it directly. The
 * upstream qemu-3dfx patch fabricates a stack RAMBlock to funnel this
 * through the listener path; calling WHvMapGpaRange directly is
 * equivalent and avoids poking RAMBlock internals.
 */
void whpx_update_guest_pa_range(uint64_t start_pa, uint64_t size,
                                void *host_va, int readonly, int add)
{
    struct whpx_state *whpx = &whpx_global;
    uint64_t aligned_size = ROUND_UP(size, qemu_real_host_page_size());
    HRESULT hr;

    if (!add) {
        hr = whp_dispatch.WHvUnmapGpaRange(whpx->partition, start_pa,
                                           aligned_size);
        if (FAILED(hr)) {
            error_report("WHPX: failed to unmap 3dfx GPA range");
        }
        return;
    }

    hr = whp_dispatch.WHvMapGpaRange(whpx->partition, host_va, start_pa,
                                     aligned_size,
                                     WHvMapGpaRangeFlagRead |
                                     WHvMapGpaRangeFlagExecute |
                                     (readonly ? 0 : WHvMapGpaRangeFlagWrite));
    if (FAILED(hr)) {
        error_report("WHPX: failed to map 3dfx GPA range");
    }
}
#endif /* CONFIG_QEMU_3DFX */

static void whpx_region_add(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    whpx_set_phys_mem(section, true);
}

static void whpx_region_del(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    whpx_set_phys_mem(section, false);
}

static void whpx_transaction_begin(MemoryListener *listener)
{
}

static void whpx_transaction_commit(MemoryListener *listener)
{
}

static void whpx_log_sync(MemoryListener *listener,
                         MemoryRegionSection *section)
{
    MemoryRegion *mr = section->mr;

    if (!memory_region_is_ram(mr)) {
        return;
    }

    if (whpx_section_tracked(section)) {
        whpx_query_and_apply_dirty(section);
    } else {
        /*
         * No tracking: pessimistically mark the whole section dirty.
         * Note [offset_within_region, +size): marking [0, size) as
         * before under-marked sections that alias into the middle of
         * their region (banked VRAM windows).
         */
        memory_region_set_dirty(mr, section->offset_within_region,
                                int128_get64(section->size));
    }
}

static MemoryListener whpx_memory_listener = {
    .name = "whpx",
    .begin = whpx_transaction_begin,
    .commit = whpx_transaction_commit,
    .region_add = whpx_region_add,
    .region_del = whpx_region_del,
    .log_sync = whpx_log_sync,
    .priority = MEMORY_LISTENER_PRIORITY_ACCEL,
};

void whpx_memory_init(void)
{
    memory_listener_register(&whpx_memory_listener, &address_space_memory);
}

/*
 * Load the functions from the given library, using the given handle. If a
 * handle is provided, it is used, otherwise the library is opened. The
 * handle will be updated on return with the opened one.
 */
static bool load_whp_dispatch_fns(HMODULE *handle,
    WHPFunctionList function_list)
{
    HMODULE hLib = *handle;

    #define WINHV_PLATFORM_DLL "WinHvPlatform.dll"
    #define WHP_LOAD_FIELD_OPTIONAL(return_type, function_name, signature) \
        whp_dispatch.function_name = \
            (function_name ## _t)GetProcAddress(hLib, #function_name); \

    #define WHP_LOAD_FIELD(return_type, function_name, signature) \
        whp_dispatch.function_name = \
            (function_name ## _t)GetProcAddress(hLib, #function_name); \
        if (!whp_dispatch.function_name) { \
            error_report("Could not load function %s", #function_name); \
            goto error; \
        } \

    #define WHP_LOAD_LIB(lib_name, handle_lib) \
    if (!handle_lib) { \
        handle_lib = LoadLibrary(lib_name); \
        if (!handle_lib) { \
            error_report("Could not load library %s.", lib_name); \
            goto error; \
        } \
    } \

    switch (function_list) {
    case WINHV_PLATFORM_FNS_DEFAULT:
        WHP_LOAD_LIB(WINHV_PLATFORM_DLL, hLib)
        LIST_WINHVPLATFORM_FUNCTIONS(WHP_LOAD_FIELD)
        break;
    case WINHV_PLATFORM_FNS_SUPPLEMENTAL:
        WHP_LOAD_LIB(WINHV_PLATFORM_DLL, hLib)
        LIST_WINHVPLATFORM_FUNCTIONS_SUPPLEMENTAL(WHP_LOAD_FIELD_OPTIONAL)
        break;
    }

    *handle = hLib;
    return true;

error:
    if (hLib) {
        FreeLibrary(hLib);
    }

    return false;
}

static void whpx_set_kernel_irqchip(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    struct whpx_state *whpx = &whpx_global;
    OnOffSplit mode;

    if (!visit_type_OnOffSplit(v, name, &mode, errp)) {
        return;
    }

    switch (mode) {
    case ON_OFF_SPLIT_ON:
        whpx->kernel_irqchip_allowed = true;
        whpx->kernel_irqchip_required = true;
        break;

    case ON_OFF_SPLIT_OFF:
        whpx->kernel_irqchip_allowed = false;
        whpx->kernel_irqchip_required = false;
        break;

    case ON_OFF_SPLIT_SPLIT:
        error_setg(errp, "WHPX: split irqchip currently not supported");
        error_append_hint(errp,
            "Try without kernel-irqchip or with kernel-irqchip=on|off");
        break;

    default:
        /*
         * The value was checked in visit_type_OnOffSplit() above. If
         * we get here, then something is wrong in QEMU.
         */
        abort();
    }
}

static void whpx_set_hyperv(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    struct whpx_state *whpx = &whpx_global;
    OnOffAuto mode;

    if (!visit_type_OnOffAuto(v, name, &mode, errp)) {
        return;
    }

    switch (mode) {
    case ON_OFF_AUTO_ON:
        whpx->hyperv_enlightenments_allowed = true;
        whpx->hyperv_enlightenments_required = true;
        break;

    case ON_OFF_AUTO_OFF:
        whpx->hyperv_enlightenments_allowed = false;
        whpx->hyperv_enlightenments_required = false;
        break;

    case ON_OFF_AUTO_AUTO:
        whpx->hyperv_enlightenments_allowed = true;
        whpx->hyperv_enlightenments_required = false;
        break;
    default:
        /*
         * The value was checked in visit_type_OnOffAuto() above. If
         * we get here, then something is wrong in QEMU.
         */
        abort();
    }
}

static void whpx_cpu_accel_class_init(ObjectClass *oc, void *data)
{
    AccelCPUClass *acc = ACCEL_CPU_CLASS(oc);

    acc->cpu_instance_init = whpx_cpu_instance_init;
}

static const TypeInfo whpx_cpu_accel_type = {
    .name = ACCEL_CPU_NAME("whpx"),

    .parent = TYPE_ACCEL_CPU,
    .class_init = whpx_cpu_accel_class_init,
    .abstract = true,
};

/* 9.0 base: AccelClass::init_machine takes no AccelState argument */
static int whpx_accel_init_compat(MachineState *ms)
{
    return whpx_accel_init(NULL, ms);
}

static void whpx_accel_class_init(ObjectClass *oc, void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);
    ac->name = "WHPX";
    ac->init_machine = whpx_accel_init_compat;
    ac->allowed = &whpx_allowed;

    object_class_property_add(oc, "kernel-irqchip", "on|off|split",
        NULL, whpx_set_kernel_irqchip,
        NULL, NULL);
    object_class_property_set_description(oc, "kernel-irqchip",
        "Configure WHPX in-kernel irqchip");
    object_class_property_add(oc, "hyperv", "OnOffAuto",
        NULL, whpx_set_hyperv,
        NULL, NULL);
    object_class_property_set_description(oc, "hyperv",
        "Configure Hyper-V enlightenments");

    whpx_arch_accel_class_init(oc);
}

static void whpx_accel_instance_init(Object *obj)
{
    struct whpx_state *whpx = &whpx_global;

    memset(whpx, 0, sizeof(struct whpx_state));
    /* Turn on kernel-irqchip, by default */
    whpx->kernel_irqchip_allowed = true;

    whpx->hyperv_enlightenments_allowed = true;
    whpx->hyperv_enlightenments_required = false;
    /* Value determined at whpx_accel_init */
    whpx->hyperv_enlightenments_enabled = false;
    whpx->ignore_unknown_msr = true;
    whpx->intercept_msr_gp = false;
    whpx->separate_security_domain = true;
}

static const TypeInfo whpx_accel_type = {
    .name = ACCEL_CLASS_NAME("whpx"),
    .parent = TYPE_ACCEL,
    .instance_init = whpx_accel_instance_init,
    .class_init = whpx_accel_class_init,
};

static void whpx_type_init(void)
{
    type_register_static(&whpx_accel_type);
    type_register_static(&whpx_cpu_accel_type);
}

bool init_whp_dispatch(void)
{
    if (whp_dispatch_initialized) {
        return true;
    }

    if (!load_whp_dispatch_fns(&hWinHvPlatform, WINHV_PLATFORM_FNS_DEFAULT)) {
        goto error;
    }
    assert(load_whp_dispatch_fns(&hWinHvPlatform,
        WINHV_PLATFORM_FNS_SUPPLEMENTAL));
    whp_dispatch_initialized = true;

    return true;
error:
    if (hWinHvPlatform) {
        FreeLibrary(hWinHvPlatform);
    }
    return false;
}

type_init(whpx_type_init);
