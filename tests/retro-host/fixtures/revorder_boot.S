	.intel_syntax noprefix
	.code16
	.global _start

/* Reverse-launch-order legacy arbitration fixture.
 *
 * Simulates two kit-v12 wrapper processes sharing one ring where the
 * SECOND-loaded process binds FIRST (the order the forward 2a-acc run
 * never exercised). Both "processes" present the same page-normalized
 * ptm VA, as measured on real XP. Progress letters go to debug port
 * 0xE9 (-debugcon):
 *
 *   L  activation readback ok            (l = failed, halt)
 *   S  second process ring-sniff share simulated
 *   B  second-loaded process bound first (b = refused, halt)
 *   O  probe reads OWNED                 (o = probe wrong, halt)
 *   1/2  shared refcount after that bind (1 = host decremented it)
 *   R  first process REFUSED             (X = MISGRANTED: the bug)
 *   F  probe reads FREE after level-0 release (f = halt)
 *   A  reacquisition after release granted    (a = halt)
 *   T  teardown poked
 *
 *   expected (hardened host):  LSBO2RFAT
 *   reverse-order bug:         LSBO1XFAT
 *
 * Build: as --32 -o x.o revorder_boot.S
 *        ld -m elf_i386 -Ttext 0x7c00 -e _start --oformat binary -o x.bin x.o
 * Run: floppy-boot under the libretro core via tests/retro-host with
 *      -debugcon file:..., see scratchpad run dirs or docs.
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

	/* Process A activates the device (kit v12 fresh-mapping DllMain). */
	mov dword ptr [0xefffefbc], 0x000a0320
	mov eax, dword ptr [0xefffefbc]
	cmp eax, 0x000320a0
	je 1f
	mov al, 'l'
	out dx, al
	jmp hang
1:	mov al, 'L'
	out dx, al

	/* Process A claims the ring exactly as kit v12 DllMain does:
	 * cursors, claim word = ptm VA + 0xFC0, shared refcount 1. */
	mov dword ptr [0xec000000], 24
	mov dword ptr [0xec000004], 0xefffefc0
	mov dword ptr [0xec300000], 2
	mov dword ptr [0xec300004], 1

	/* Process B loads second: DllMain ring-sniff takes the share path
	 * and only increments the live refcount. */
	inc dword ptr [0xec300004]
	mov al, 'S'
	out dx, al

	/* SetPixelFormat scratch payload: fmt 1, same ptm VA, zeroed PFD,
	 * zero token word 12 (legacy clients carry no token). */
	mov dword ptr [0xefffb000], 1
	mov dword ptr [0xefffb004], 0xefffe000
	xor eax, eax
	mov edi, 0xefffb008
	mov ecx, 11
	rep stosd

	/* Process B binds FIRST while the session is FREE: must be granted. */
	call set_pixel_format
	cmp eax, 0x5b5eb5e5
	je 1f
	mov al, 'b'
	out dx, al
	jmp hang
1:	mov al, 'B'
	out dx, al

	/* Probe must now read OWNED (tag 0x3D, ver 1, state 2, token 0). */
	mov eax, dword ptr [0xefffefb4]
	shr eax, 16
	cmp ax, 0x3d12
	je 1f
	mov al, 'o'
	out dx, al
	jmp hang
1:	mov al, 'O'
	out dx, al

	/* Shared refcount after B's bind: 2 = honest (both processes live),
	 * 1 = the host's crash repair just disguised process A as the owner. */
	mov eax, dword ptr [0xec300004]
	and al, 0x0f
	add al, '0'
	out dx, al

	/* Process A now reaches SetPixelFormat with the same VA:
	 * it MUST be refused; MAGIC here is the reverse-order misgrant. */
	call set_pixel_format
	cmp eax, 0x5b5eb5e5
	jne 1f
	mov al, 'X'
	out dx, al
	jmp release
1:	mov al, 'R'
	out dx, al

release:
	/* Owner's level-0 wglDeleteContext releases the session to FREE. */
	mov dword ptr [0xefffeff4], 0x5b5eb5e5
	mov eax, dword ptr [0xefffefb4]
	shr eax, 16
	cmp ax, 0x3d10
	je 1f
	mov al, 'f'
	out dx, al
	jmp hang
1:	mov al, 'F'
	out dx, al

	/* Reacquisition after a clean release must be granted. */
	call set_pixel_format
	cmp eax, 0x5b5eb5e5
	je 1f
	mov al, 'a'
	out dx, al
	jmp hang
1:	mov al, 'A'
	out dx, al

	/* Clean DLL-unload teardown. */
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
