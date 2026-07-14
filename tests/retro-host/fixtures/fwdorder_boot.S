	.intel_syntax noprefix
	.code16
	.global _start

/* Forward-order refusal + legacy crash-repair regression fixture.
 *
 * The already-proven direction, kept as the regression guard for the
 * reverse-order fix: the FIRST-loaded process binds first, a live
 * ring-sharing second process is refused, and after the bound owner
 * goes silent past the legacy crash timeout the successor's
 * SetPixelFormat both binds and repairs the shared refcount (the
 * historical dead-owner path, which must keep working). Letters to
 * debug port 0xE9:
 *
 *   L  activation readback ok               (l = failed, halt)
 *   B  first process bound                  (b = refused, halt)
 *   R  live second process refused          (x = misgranted, halt)
 *   D  successor granted after crash expiry (d = refused, halt)
 *   1  shared refcount repaired to 1        (2 = repair lost)
 *   T  teardown poked
 *
 *   expected (before and after the reverse-order fix):  LBRD1T
 *
 * Build: as --32 -o x.o fwdorder_boot.S
 *        ld -m elf_i386 -Ttext 0x7c00 -e _start --oformat binary -o x.bin x.o
 */

_start:
	cli
	xor ax, ax
	mov ds, ax
	lgdt [gdt_desc]
	mov eax, cr0
	or eax, 1
	mov cr0, eax
	ljmp 0x08, protected_mode

	.code32
protected_mode:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov esp, 0x7000
	mov dx, 0x00e9

	/* qemu-3dfx revision signature at the end of MESA_FBTM_BASE. */
	mov dword ptr [0xebfffff0], 0x66373462
	mov dword ptr [0xebfffff4], 0x2d613033

	/* Process A activates. */
	mov dword ptr [0xefffefbc], 0x000a0320
	mov eax, dword ptr [0xefffefbc]
	cmp eax, 0x000320a0
	je 1f
	mov al, 'l'
	out dx, al
	jmp hang
1:	mov al, 'L'
	out dx, al

	/* Process A claims the ring: cursors, claim word, refcount 1. */
	mov dword ptr [0xec000000], 24
	mov dword ptr [0xec000004], 0xefffefc0
	mov dword ptr [0xec300000], 2
	mov dword ptr [0xec300004], 1

	/* SetPixelFormat scratch payload: fmt 1, ptm VA, zeroed PFD/token. */
	mov dword ptr [0xefffb000], 1
	mov dword ptr [0xefffb004], 0xefffe000
	xor eax, eax
	mov edi, 0xefffb008
	mov ecx, 11
	rep stosd

	/* Process A binds first: must be granted. */
	call set_pixel_format
	cmp eax, 0x5b5eb5e5
	je 1f
	mov al, 'b'
	out dx, al
	jmp hang
1:	mov al, 'B'
	out dx, al

	/* Process B loads second (sniff-share, refcount 2) and tries to
	 * bind while the owner is fresh: must be refused. */
	inc dword ptr [0xec300004]
	call set_pixel_format
	cmp eax, 0x5b5eb5e5
	jne 1f
	mov al, 'x'
	out dx, al
	jmp hang
1:	mov al, 'R'
	out dx, al

	/* Owner goes silent past MESAGL_CRASH_RC (3 s): wait for five RTC
	 * seconds-register transitions so the wall-clock timeout expires. */
	mov bl, 5
	call rtc_read
	mov bh, al
wait_tick:
	call rtc_read
	cmp al, bh
	je wait_tick
	mov bh, al
	dec bl
	jnz wait_tick

	/* Successor SetPixelFormat: legacy crash release must free the
	 * session, grant this request, and repair the shared refcount. */
	call set_pixel_format
	cmp eax, 0x5b5eb5e5
	je 1f
	mov al, 'd'
	out dx, al
	jmp hang
1:	mov al, 'D'
	out dx, al

	/* Refcount must have been patched down for the dead claimant. */
	mov eax, dword ptr [0xec300004]
	and al, 0x0f
	add al, '0'
	out dx, al

	/* Clean release + DLL-unload teardown. */
	mov dword ptr [0xefffeff4], 0x5b5eb5e5
	mov dword ptr [0xefffefbc], 0x000d0320
	mov al, 'T'
	out dx, al
hang:
	hlt
	jmp hang

set_pixel_format:
	mov dword ptr [0xefffefe4], 0x5b5eb5e5
	mov eax, dword ptr [0xefffefe4]
	ret

rtc_read:
	mov al, 0x00
	out 0x70, al
	in al, 0x71
	ret

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
