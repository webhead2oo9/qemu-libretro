	.intel_syntax noprefix
	.code16
	.global _start

/* Deliberately concurrent multi-vCPU Mesa doorbell stress fixture.
 *
 * The Step 2c hardening marshals every Mesa MMIO write to the libretro
 * main-loop thread and latches register replies per vCPU. The real-XP
 * runs exercise that machinery with one well-behaved client on the BSP;
 * this fixture is the deliberately concurrent submitter test they defer:
 * the BSP performs honest activation/bind/release protocol cycles while
 * the application processor first hammers reads (its reply latches must
 * stay untouched by the BSP's traffic) and then contends with tokened
 * reservation writes against the BSP's untokened binds.
 *
 * Four-sector floppy image. The BSP prints all letters to debug port
 * 0xE9; the AP posts error counters through shared memory at 0x5000.
 *
 *   L  AP came up after INIT-SIPI-SIPI       (l = AP never ready, halt)
 *   I  AP reader latches stayed virgin        (i = cross-vCPU latch leak)
 *   C  all BSP P1 bind/release cycles exact   (c = wrong readback/state)
 *   W  BSP P2 readbacks all MAGIC-or-refused  (w = garbage readback)
 *   P  AP P2 probe words all well-formed      (p = malformed probe)
 *   F  post-contention recovery bind + FREE   (f = wedged, halt)
 *   T  teardown poked
 *
 *   expected: LICWPFT
 *
 * A deadlock in the doorbell marshal shows up as a missing tail of the
 * string (the harness timeout, not a letter, reports it).
 *
 * Build: as --32 -o x.o smpstress_boot.S
 *        ld -m elf_i386 -Ttext 0x7c00 -e _start --oformat binary -o x.bin x.o
 * Run under -smp 2 with tcg,thread=multi (host-thread concurrency) and
 * again under whpx (the production accelerator) via run-fixtures.ps1.
 */

/* Shared memory layout (low RAM, below the boot sector):
 *   0x5000 ap_ready     0x5004 phase        0x5008 ap_p1_err
 *   0x500C ap_p2_err    0x5010 ap_done_p1   0x5014 ap_done_p2
 * Phases: 1 = P1 reader, 15 = P1 quiesce, 2 = P2 writer,
 *         16 = P2 quiesce, 3 = park. */

_start:
	cli
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, 0x7000

	/* Load sectors 2-4 (this file's 32-bit body) to 0x7e00. */
	mov bx, 0x7e00
	mov ah, 0x02
	mov al, 3
	mov cx, 0x0002
	xor dx, dx
	int 0x13
	jnc load_ok
	mov ah, 0x02
	mov al, 3
	mov cx, 0x0002
	xor dx, dx
	int 0x13
	jc hang16
load_ok:
	lgdt [gdt_desc]
	mov eax, cr0
	or eax, 1
	mov cr0, eax
	ljmp 0x08, bsp_main

hang16:
	hlt
	jmp hang16

/* AP trampoline: copied to physical 0x6000 by the BSP. Runs in real
 * mode at CS=0x0600 IP=0; position-independent (absolute operands
 * only). */
ap_tramp:
	cli
	xor ax, ax
	mov ds, ax
	mov byte ptr [0x5018], 1        /* AP reached real-mode trampoline */
	lgdt [gdt_desc]
	mov eax, cr0
	or eax, 1
	mov cr0, eax
	ljmp 0x08, ap_main
ap_tramp_end:

	.p2align 3
gdt:
	.quad 0x0000000000000000
	.quad 0x00cf9a000000ffff
	.quad 0x00cf92000000ffff
gdt_end:
gdt_desc:
	.word gdt_end - gdt - 1
	.long gdt

	.org 510
	.word 0xaa55

/* ---- sector 2 onward: 32-bit code, loaded at 0x7e00 ---- */
	.code32

/* Emit AL to the debug console (BSP only). */
putc:
	push dx
	mov dx, 0x00e9
	out dx, al
	pop dx
	ret

/* Read RTC seconds into AL (transition counting for coarse waits). */
rtc_read:
	mov al, 0x00
	out 0x70, al
	in al, 0x71
	ret

/* Wait BL RTC seconds-register transitions. */
rtc_wait:
	call rtc_read
	mov bh, al
1:	call rtc_read
	cmp al, bh
	je 1b
	mov bh, al
	dec bl
	jnz 1b
	ret

/* Poke 0xFE4 with MAGIC and return the readback in EAX. */
bind_fe4:
	mov dword ptr [0xefffefe4], 0x5b5eb5e5
	mov eax, dword ptr [0xefffefe4]
	ret

bsp_main:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov esp, 0x7000
	cld

	/* Clear the shared mailbox (incl. the AP aliveness marker). */
	xor eax, eax
	mov edi, 0x5000
	mov ecx, 7
	rep stosd

	/* Copy the AP trampoline to 0x6000. (offset: gas Intel syntax
	 * otherwise assembles a load of the bytes AT the label.) */
	mov esi, offset ap_tramp
	mov edi, 0x6000
	mov ecx, ap_tramp_end - ap_tramp
	rep movsb

	/* Building with --defsym CONTROL=1 strips every mesapt access so
	 * the AP startup mechanics can run on stock QEMU as a control. */
.ifndef CONTROL
	/* qemu-3dfx revision signature, then device activation. */
	mov dword ptr [0xebfffff0], 0x66373462
	mov dword ptr [0xebfffff4], 0x2d613033
	mov dword ptr [0xefffefbc], 0x000a0320
	mov eax, dword ptr [0xefffefbc]
	cmp eax, 0x000320a0
	je 1f
	mov al, 'x'                     /* activation failed, not the AP */
	call putc
	jmp hang
1:

	/* Claim the ring as the single legacy client. */
	mov dword ptr [0xec000000], 24
	mov dword ptr [0xec000004], 0xefffefc0
	mov dword ptr [0xec300000], 2
	mov dword ptr [0xec300004], 1

	/* SetPixelFormat scratch payload: fmt, ptm VA, zero PFD/token. */
	mov dword ptr [0xefffb000], 1
	mov dword ptr [0xefffb004], 0xefffe000
	xor eax, eax
	mov edi, 0xefffb008
	mov ecx, 11
	rep stosd
.endif

	/* INIT + double SIPI (vector 0x06 -> 0x6000) to APIC ID 1 by
	 * explicit destination, with RTC-quantified delays between the
	 * IPIs — deliberately conservative so a slow accelerator cannot
	 * outrun the sequence. */
	mov dword ptr [0xfee00310], 0x01000000
	mov dword ptr [0xfee00300], 0x00004500
	mov bl, 1
	call rtc_wait
	mov dword ptr [0xfee00310], 0x01000000
	mov dword ptr [0xfee00300], 0x00004606
	mov bl, 1
	call rtc_wait
	mov dword ptr [0xfee00310], 0x01000000
	mov dword ptr [0xfee00300], 0x00004606

	/* Wait for the AP with a ~3 RTC-second ceiling. */
	call rtc_read
	mov bh, al
	xor bl, bl
1:	cmp dword ptr [0x5000], 1
	je ap_up
	call rtc_read
	cmp al, bh
	je 1b
	mov bh, al
	inc bl
	cmp bl, 3
	jb 1b
fail_l:
	mov al, 'l'
	call putc
	mov al, '0'                     /* did the AP reach the trampoline? */
	add al, byte ptr [0x5018]
	call putc
	jmp hang
ap_up:
	mov al, 'L'
	call putc
.ifdef CONTROL
	jmp hang
.endif

	/* ---- Phase 1: AP hammers reads while the BSP runs 24 honest
	 * bind/release cycles. Every BSP readback is exact. ---- */
	mov dword ptr [0x5004], 1
	xor esi, esi                    /* BSP error count */
	mov edi, 24                     /* cycles */
p1_cycle:
	call bind_fe4
	cmp eax, 0x5b5eb5e5
	jne p1_err
	mov eax, dword ptr [0xefffefb4]
	cmp eax, 0x3d120000
	jne p1_err
	mov dword ptr [0xefffeff4], 0x5b5eb5e5
	mov eax, dword ptr [0xefffefb4]
	cmp eax, 0x3d100000
	jne p1_err
	jmp p1_next
p1_err:
	inc esi
p1_next:
	dec edi
	jnz p1_cycle

	/* Quiesce the AP and collect its P1 verdict. */
	mov dword ptr [0x5004], 15
1:	cmp dword ptr [0x5010], 1
	jne 1b
	mov al, 'I'
	cmp dword ptr [0x5008], 0
	je 1f
	mov al, 'i'
1:	call putc
	mov al, 'C'
	test esi, esi
	jz 1f
	mov al, 'c'
1:	call putc

	/* ---- Phase 2: AP contends with tokened reservations while the
	 * BSP keeps cycling. Grants and refusals are both legitimate;
	 * anything else is a broken readback. A granted bind is released
	 * honestly; a refused bind is not followed by a release. ---- */
	mov dword ptr [0x5004], 2
	xor esi, esi
	mov edi, 24
p2_cycle:
	call bind_fe4
	cmp eax, 0x5b5eb5e5
	je p2_release
	test eax, eax
	jz p2_next                      /* clean refusal */
	inc esi                         /* garbage readback */
	jmp p2_next
p2_release:
	mov dword ptr [0xefffeff4], 0x5b5eb5e5
p2_next:
	dec edi
	jnz p2_cycle

	mov dword ptr [0x5004], 16
1:	cmp dword ptr [0x5014], 1
	jne 1b
	mov al, 'W'
	test esi, esi
	jz 1f
	mov al, 'w'
1:	call putc
	mov al, 'P'
	cmp dword ptr [0x500c], 0
	je 1f
	mov al, 'p'
1:	call putc
	mov dword ptr [0x5004], 3

	/* ---- Recovery: let any live reservation expire (2 s TTL), then
	 * one exact bind/release cycle must succeed. ---- */
	mov bl, 3
	call rtc_wait
	call bind_fe4
	cmp eax, 0x5b5eb5e5
	jne fail_f
	mov dword ptr [0xefffeff4], 0x5b5eb5e5
	mov eax, dword ptr [0xefffefb4]
	cmp eax, 0x3d100000
	jne fail_f
	mov al, 'F'
	call putc
	jmp teardown
fail_f:
	mov al, 'f'
	call putc
	jmp hang

teardown:
	mov dword ptr [0xefffefbc], 0x000d0320
	mov al, 'T'
	call putc
hang:
	hlt
	jmp hang

/* ---- Application processor ---- */
ap_main:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov esp, 0x6800
	mov dword ptr [0x5000], 1
.ifdef CONTROL
	jmp ap_park
.endif

	/* Wait for phase 1. */
1:	cmp dword ptr [0x5004], 1
	jne 1b

	/* P1: pure reader. This vCPU has never written a Mesa register,
	 * so every one of its reply latches must read zero no matter what
	 * the BSP's concurrent cycles latch for themselves; the window
	 * poll may read only 0 or 1 (never the refusal sentinel); the
	 * probe word must always be well-formed with no token. */
ap_p1:
	cmp dword ptr [0x5004], 1
	jne ap_p1_done
	mov eax, dword ptr [0xefffefbc]
	test eax, eax
	jnz ap_p1_err
	mov eax, dword ptr [0xefffefe4]
	test eax, eax
	jnz ap_p1_err
	mov eax, dword ptr [0xefffefec]
	test eax, eax
	jnz ap_p1_err
	mov eax, dword ptr [0xefffefe8]
	test eax, eax
	jnz ap_p1_err
	mov eax, dword ptr [0xefffefc0]
	test eax, eax
	jnz ap_p1_err
	mov eax, dword ptr [0xefffefb8]
	cmp eax, 1
	ja ap_p1_err
	mov eax, dword ptr [0xefffefb4]
	cmp eax, 0x3d100000
	je ap_p1
	cmp eax, 0x3d120000
	je ap_p1
ap_p1_err:
	inc dword ptr [0x5008]
	jmp ap_p1
ap_p1_done:
	mov dword ptr [0x5010], 1

	/* Wait for phase 2. */
1:	cmp dword ptr [0x5004], 2
	jne 1b

	/* P2: contending writer. Reserve with token 0xB7 and require every
	 * probe readback to be one of the three legitimate words. */
ap_p2:
	cmp dword ptr [0x5004], 2
	jne ap_p2_done
	mov dword ptr [0xefffefb4], 0x000000b7
	mov eax, dword ptr [0xefffefb4]
	cmp eax, 0x3d100000
	je ap_p2
	cmp eax, 0x3d1100b7
	je ap_p2
	cmp eax, 0x3d120000
	je ap_p2
	inc dword ptr [0x500c]
	jmp ap_p2
ap_p2_done:
	mov dword ptr [0x5014], 1
ap_park:
	hlt
	jmp ap_park

	.org 2048
