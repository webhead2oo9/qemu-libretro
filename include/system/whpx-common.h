/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SYSTEM_WHPX_COMMON_H
#define SYSTEM_WHPX_COMMON_H

/*
 * Per-instruction GVA->GPA cache for the instruction emulator. Every
 * translation is otherwise a WHvTranslateGva hypercall, and string
 * instructions translate per element (a REP INSW issues 256 per
 * sector). Entries live for one emulated instruction only — reset on
 * entry to the emulator — which makes staleness impossible and matches
 * the x86 rule that a TLB may serve stale entries until invalidation.
 */
#define WHPX_MMU_CACHE_SIZE 8

struct whpx_mmu_cache_entry {
    uint64_t gva_page;
    uint64_t gpa_page;
    bool valid;
    /*
     * Sticky upgrade bits. write_ok: a write-validated translation
     * succeeded for this page, meaning the hypervisor (or hardware, for
     * the exit-context seed) has set the guest PTE dirty bit — pageable
     * guests like Win9x discard emulator-filled pages without it, so a
     * cached read-only entry must never satisfy a write. priv_ok: the
     * translation passed privilege checks (was not PrivilegeExempt).
     */
    bool write_ok;
    bool priv_ok;
};

struct AccelCPUState {
    bool window_registered;
    int window_priority;
    bool interruptable;
    bool ready_for_pic_interrupt;
    uint64_t tpr;
    bool interruption_pending;
    struct whpx_mmu_cache_entry mmu_cache[WHPX_MMU_CACHE_SIZE];
    unsigned mmu_cache_next;    /* round-robin eviction cursor */
    /* Must be the last field as it may have a tail */
    WHV_RUN_VP_EXIT_CONTEXT exit_ctx;
};

int whpx_first_vcpu_starting(CPUState *cpu);
int whpx_last_vcpu_stopping(CPUState *cpu);
void whpx_memory_init(void);
struct whpx_breakpoint *whpx_lookup_breakpoint_by_addr(uint64_t address);
void whpx_flush_cpu_state(CPUState *cpu);
void whpx_get_reg(CPUState *cpu, WHV_REGISTER_NAME reg, WHV_REGISTER_VALUE* val);
void whpx_set_reg(CPUState *cpu, WHV_REGISTER_NAME reg, WHV_REGISTER_VALUE val);

/* On x64: same as WHvX64ExceptionTypeDebugTrapOrFault */
#define WHPX_INTERCEPT_DEBUG_TRAPS 1
#endif
