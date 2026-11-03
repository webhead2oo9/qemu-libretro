/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * QD3D wire protocol: guest d3d9.dll proxy <-> host Direct3D 9Ex executor.
 *
 * This header is the single source of truth for the channel layout, the
 * command and reply framing, the argument encoding of generated method
 * operations, and the hand-specified extension operations. It is compiled
 * by the 32-bit guest proxy (i686 MinGW, Windows XP) and by the 64-bit host
 * executor (x86_64 MinGW, inside qemu-libretro or the loopback harness), so
 * every structure uses fixed-width fields with identical layout on both.
 *
 * Generated method opcodes live in qd3d_ops.h (do not edit by hand).
 */
#ifndef QD3D_PROTO_H
#define QD3D_PROTO_H

#include <stdint.h>

#define QD3D_MAGIC       0x44334451u /* 'QD3D' little-endian */
#define QD3D_ABI_VERSION 3u

/*
 * IID_QD3D_REMOTE {5f3e1c2a-8b47-4d16-9e03-7a51c26b8814}. Every IDirect3D9
 * object the proxy returns for a connected QD3D session answers
 * QueryInterface(IID_QD3D_REMOTE) with S_OK and itself (AddRef'd); stock
 * IDirect3D9 objects, including the ones the proxy forwards to when there is
 * no channel, answer E_NOINTERFACE. The Direct3D 8 front end uses it to
 * decide whether to fall back to the stock d3d8.dll. Guest-only: never on
 * the wire. The C/C++ helper exists once the Windows GUID type is declared
 * (include <windows.h> first); IID_QD3D_REMOTE is an lvalue, so C passes
 * &IID_QD3D_REMOTE and C++ passes IID_QD3D_REMOTE as REFIID.
 */
#define QD3D_IID_REMOTE_INIT \
    { 0x5f3e1c2a, 0x8b47, 0x4d16, { 0x9e, 0x03, 0x7a, 0x51, 0xc2, 0x6b, 0x88, 0x14 } }
#ifdef GUID_DEFINED
static inline const GUID *qd3d_iid_remote(void)
{
    static const GUID iid = QD3D_IID_REMOTE_INIT;
    return &iid;
}
#define IID_QD3D_REMOTE (*qd3d_iid_remote())
#endif

/* ------------------------------------------------------------------------ */
/* Channel                                                                  */
/* ------------------------------------------------------------------------ */
/*
 * One channel is one contiguous shared-memory region:
 *
 *   [0, QD3D_CTRL_SIZE)                     struct qd3d_ctrl
 *   [ring_offset, ring_offset + ring_size)  command ring (guest -> host)
 *   [reply_offset, reply_offset+reply_size) reply area   (host -> guest)
 *
 * The host initialises every qd3d_ctrl field and stores magic last. A guest
 * MUST NOT use a channel whose magic, abi, or geometry is invalid.
 *
 * Ordering: x86 and x86-64 keep store order and load order, so a producer
 * publishes a cursor with a compiler barrier followed by a plain 32-bit
 * store, and a consumer reads the cursor before reading the payload. Both
 * sides still issue a full barrier (MemoryBarrier / __sync_synchronize)
 * around cursor publication so the code stays correct if this ever moves.
 */
#define QD3D_CTRL_SIZE      4096u
#define QD3D_DEFAULT_RING   (16u * 1024u * 1024u)
#define QD3D_DEFAULT_REPLY  (4u * 1024u * 1024u)
#define QD3D_DEFAULT_SIZE   (QD3D_CTRL_SIZE + QD3D_DEFAULT_RING + QD3D_DEFAULT_REPLY)

/*
 * Transports.
 *
 * Both transports offer channel slots: independent channels, each with its
 * own host executor session, one guest process per slot. A process owns a
 * slot while it holds the slot's claim, a named event it created itself
 * (CreateEvent did not report ERROR_ALREADY_EXISTS); an existing claim, or
 * one the process may not open, belongs to another live process, so the
 * slot is busy. The claim handle stays open while the process uses the
 * slot and is never inheritable. The kernel closes it when the process
 * ends, however it ends, so a dead owner's slot is free at once and the
 * next owner's HELLO releases what the dead one left on the host. Handles
 * belong to processes, not threads: no thread exit releases a slot. A
 * process that finds every slot busy uses the stock runtime.
 *
 * Loopback (development on a Windows host, no VM): the 64-bit harness
 * creates one named file mapping of QD3D_DEFAULT_SIZE bytes per slot (at
 * most QD3D_VM_SLOTS), NAME for slot 0 and NAME_<i> for slot i > 0, and one
 * auto-reset doorbell event NAME_KICK for all of them (NAME defaults to
 * QD3D_LOOPBACK_MAPPING). Slot i's claim is NAME QD3D_LOOPBACK_SLOT_CLAIM
 * <i>. The 32-bit proxy claims the lowest free slot whose mapping exists.
 * Kick = SetEvent.
 *
 * VM: qemu-libretro maps slot i as RAM at QD3D_VM_CHANNEL_GPA +
 * i * QD3D_VM_CHANNEL_SIZE (outside guest RAM) plus one MMIO register page
 * at QD3D_VM_MMIO_GPA; QD3D_VM_REG_SLOTS reports how many slots it serves
 * (the guest uses at most QD3D_VM_SLOTS). Slot i's claim is
 * QD3D_VM_SLOT_CLAIM <i>, in the global namespace because the slots are
 * shared by every logon session. The guest maps the register page and its
 * slot into the process with the qemu-3dfx MAPMEM driver (\\.\MAPMEM). That
 * device is exclusive: while one process has it open, every other open
 * fails with ERROR_ACCESS_DENIED whatever the share mode. So the guest
 * opens it only to map, retrying briefly while another process connects,
 * and closes it at once: the views outlive the handle, and the process
 * unmaps them itself with UnmapViewOfFile (the driver's unmap request is
 * the same ZwUnmapViewOfSection on the calling process). Kick = any 32-bit
 * write to QD3D_VM_MMIO_GPA + QD3D_VM_REG_KICK: it wakes the one executor
 * thread, which serves every slot.
 */
#define QD3D_LOOPBACK_MAPPING    "Local\\QD3D_LOOPBACK"
#define QD3D_LOOPBACK_KICK       "Local\\QD3D_LOOPBACK_KICK"
#define QD3D_LOOPBACK_SLOT_CLAIM "_SLOT_"                /* NAME + this + slot */
#define QD3D_VM_SLOT_CLAIM       "Global\\QD3D_VM_SLOT_" /* + slot (decimal) */

#define QD3D_VM_SLOTS         4u
#define QD3D_VM_CHANNEL_GPA   0xd0000000u /* slot i at + i * QD3D_VM_CHANNEL_SIZE */
#define QD3D_VM_CHANNEL_SIZE  (32u * 1024u * 1024u) /* >= QD3D_DEFAULT_SIZE */
#define QD3D_VM_MMIO_GPA      0xd8000000u /* after the last slot */
#define QD3D_VM_MMIO_SIZE     4096u
#define QD3D_VM_REG_KICK      0x000u  /* write: doorbell, every slot */
#define QD3D_VM_REG_ID        0x004u  /* read: QD3D_MAGIC when present */
#define QD3D_VM_REG_SLOTS     0x008u  /* read: channel slots served */

/* host_state */
#define QD3D_HOST_DOWN   0u /* executor not running; guest must not submit */
#define QD3D_HOST_READY  1u
#define QD3D_HOST_FAULT  2u /* executor rejected a command; session is dead */

/* Each group sits on its own 64-byte line to avoid producer/consumer
 * false sharing. */
struct qd3d_ctrl {
    /* line 0: immutable after host init */
    uint32_t magic;          /* QD3D_MAGIC, stored last by the host */
    uint32_t abi;            /* QD3D_ABI_VERSION */
    uint32_t total_size;     /* bytes in the whole channel */
    uint32_t ring_offset;    /* multiple of 4096 */
    uint32_t ring_size;      /* power of two, >= 64 KiB */
    uint32_t reply_offset;   /* multiple of 4096 */
    uint32_t reply_size;     /* >= 64 KiB */
    uint32_t pad0[9];

    /* line 1: guest-written */
    volatile uint32_t head;          /* bytes ever written to the ring */
    volatile uint32_t sync_req;      /* seq of the last sync command */
    volatile uint32_t frames_submitted;
    volatile uint32_t guest_pid;     /* informational */
    volatile uint32_t session;       /* incremented by HELLO, see below */
    /* Advanced (any amount, wrapping) by a live guest at least once a
     * second while a frame source of the session that presented lives,
     * however static its screen: a slot whose frames are shown and which
     * neither beats nor sends commands for a few seconds lost its process
     * without GOODBYE (qd3dx.h, display policy). */
    volatile uint32_t heartbeat;
    uint32_t pad1[10];

    /* line 2: host-written */
    volatile uint32_t tail;          /* bytes ever consumed from the ring */
    volatile uint32_t sync_done;     /* seq whose reply is valid */
    volatile uint32_t frames_done;
    volatile uint32_t host_state;    /* QD3D_HOST_* */
    volatile uint32_t host_idle;     /* 1 while the host waits for a kick */
    volatile uint32_t fault_op;      /* op that faulted (diagnostic) */
    volatile uint32_t fault_code;    /* QD3D_FAULT_* (diagnostic) */
    uint32_t pad2[9];
};

/* fault_code */
#define QD3D_FAULT_NONE        0u
#define QD3D_FAULT_FRAMING     1u /* bad size/alignment/op */
#define QD3D_FAULT_DECODE      2u /* payload shorter/longer than encoded */
#define QD3D_FAULT_OBJECT      3u /* unknown id or wrong interface type */
#define QD3D_FAULT_SIZE        4u /* data length disagrees with arguments */
#define QD3D_FAULT_REPLY       5u /* reply would exceed reply_size */
#define QD3D_FAULT_UNEXPECTED  6u /* guest-local method or unknown ext op */
#define QD3D_FAULT_ID          7u /* NEWOBJ id outside guest range / reused */

/* ------------------------------------------------------------------------ */
/* Command framing                                                          */
/* ------------------------------------------------------------------------ */
/*
 * Commands start on 16-byte boundaries and never straddle the end of the
 * ring. cmd->size is the exact command length (header plus encoded payload,
 * no trailing padding); the ring cursor advances by QD3D_CMD_STRIDE(size).
 * The host requires the encoded payload to consume exactly size - 16 bytes.
 *
 * If the space left before the end of the ring is smaller than the stride of
 * the next command, the guest first writes a QD3D_OP_PAD command whose size
 * is exactly that space (always a multiple of 16, so the header fits). A
 * command stride is at most ring_size / 2.
 *
 * The guest may write at most ring_size bytes ahead of tail. It publishes a
 * command by advancing head by its stride; the host consumes it and then
 * advances tail by the same stride. If host_idle is 1 after publication the
 * guest kicks the host (transport-specific doorbell).
 */
#define QD3D_CMD_ALIGN 16u
#define QD3D_CMD_STRIDE(size) (((size) + (QD3D_CMD_ALIGN - 1u)) & ~(QD3D_CMD_ALIGN - 1u))

struct qd3d_cmd {
    uint32_t size;  /* exact bytes including this header */
    uint32_t op;    /* QD3D_OP_* (extension) or generated method opcode */
    uint32_t obj;   /* target object id ("this"), 0 when not applicable */
    uint32_t seq;   /* nonzero: synchronous, the host must write a reply */
};

/* DATA items are padded to 4 bytes; floats travel as their IEEE bits. */
#define QD3D_PAD4(n) (((n) + 3u) & ~(uint64_t)3u)

static inline uint32_t qd3d_f2u(float f)
{
    union { float f; uint32_t u; } c;
    c.f = f;
    return c.u;
}

static inline float qd3d_u2f(uint32_t u)
{
    union { float f; uint32_t u; } c;
    c.u = u;
    return c.f;
}

/*
 * Replies. At most one synchronous command is outstanding per channel (the
 * guest serialises every call under one lock). The host writes the payload
 * and header at reply_offset, then stores ctrl->sync_done = seq. The guest
 * waits for sync_done == seq and only then reads the reply. A new owner of
 * a slot continues after the channel's sync_req, so no command a dead
 * predecessor left in the ring answers one of its own.
 */
struct qd3d_reply {
    uint32_t seq;
    uint32_t ret;   /* method return value bits (HRESULT, UINT, DWORD, BOOL,
                       float bits); extension ops document their meaning */
    uint32_t size;  /* payload bytes after this header */
    uint32_t reserved;
};

/* ------------------------------------------------------------------------ */
/* Object identifiers                                                       */
/* ------------------------------------------------------------------------ */
/*
 * 0 is NULL. Guest-assigned ids are 1..0x7fffffff, allocated monotonically
 * per session and never reused within a session; a guest process runs one
 * session (one HELLO) for its lifetime, and every IDirect3D9 it hands out
 * names the HELLO's d3d_id without owning it (GOODBYE releases it).
 * Host-assigned ids carry QD3D_ID_HOST_BIT and name objects the host
 * returns that the guest never created (implicit back buffers, surface
 * levels, swap chain 0, the auto depth-stencil surface, FVF-derived
 * declarations); they are unique over the executor's lifetime (HELLO and
 * GOODBYE do not restart them; after 2^31-1 ids they wrap, skipping live
 * ones).
 *
 * Every live id holds exactly one host reference. The guest sends
 * QD3D_OP_RELEASE when its proxy for an id is destroyed; the host then
 * releases that reference and forgets the id. When a getter returns an
 * object the host already maps, the host returns the existing id and drops
 * the extra reference the getter added.
 */
#define QD3D_ID_HOST_BIT 0x80000000u
#define QD3D_ID_IS_HOST(id) (((id) & QD3D_ID_HOST_BIT) != 0u)

/* ------------------------------------------------------------------------ */
/* Generated method argument encoding                                       */
/* ------------------------------------------------------------------------ */
/*
 * Arguments follow the command header in declaration order ("this" travels
 * in cmd->obj). Every field is a little-endian u32 or a DATA item:
 *
 *   U32      scalar (integers, enums, flags, BOOL, D3DCOLOR, float bits)
 *   OBJ      input interface pointer: object id (0 = NULL)
 *   NEWOBJ   output of a creating method (Create*, EndStateBlock): the
 *            guest-assigned id of the new object, 0 if the caller passed a
 *            NULL out pointer. Nothing is returned for it in the reply.
 *   OUTOBJ   output interface pointer of a getter: u32 presence flag;
 *            when present the reply carries the u32 id (0 = NULL) and the
 *            u32 concrete interface type (enum qd3d_type), because getters
 *            such as GetTexture declare an abstract interface
 *   DATA     input pointer/array/blob: u32 byte count, then the bytes,
 *            zero-padded to 4. QD3D_DATA_NULL as the count means NULL.
 *   OUTDATA  output pointer/array: u32 presence flag; the reply carries a
 *            DATA item (QD3D_DATA_NULL when absent)
 *   INOUT    DATA in the command, DATA in the reply
 *   IGNORED  u32 presence flag; the host substitutes NULL (pSharedHandle)
 *
 * Reply payload: one item per OUTOBJ/OUTDATA/INOUT argument, in declaration
 * order. Generated code on both sides is the only producer and consumer.
 *
 * Methods whose arguments cannot be encoded this way (window, DC and
 * monitor handles, lock pointers, GUIDs, opaque blobs, present parameters)
 * are either guest-local or travel as extension operations below.
 */
#define QD3D_DATA_NULL 0xffffffffu

/* ------------------------------------------------------------------------ */
/* Extension operations (hand-specified)                                    */
/* ------------------------------------------------------------------------ */
/* Generated method opcodes start at QD3D_OP_METHOD_BASE (qd3d_ops.h). */
#define QD3D_OP_PAD          0u  /* skip cmd->size bytes */
#define QD3D_OP_HELLO        1u  /* sync */
#define QD3D_OP_GOODBYE      2u  /* async */
#define QD3D_OP_RELEASE      3u  /* async, cmd->obj = id */
#define QD3D_OP_CREATE_DEVICE 4u /* sync */
#define QD3D_OP_RESET        5u  /* sync, cmd->obj = device id */
#define QD3D_OP_PRESENT      6u  /* async, cmd->obj = device or swap chain */
#define QD3D_OP_UPLOAD       7u  /* async, cmd->obj = resource id */
#define QD3D_OP_READBACK     8u  /* sync, cmd->obj = resource id */
#define QD3D_OP_QUERY_DATA   9u  /* sync, cmd->obj = query id */
#define QD3D_OP_CURSOR       10u /* async, cmd->obj = 0 */
#define QD3D_OP_CREATE_SWAPCHAIN 11u /* sync, cmd->obj = device id */
#define QD3D_OP_CURSOR_POS   12u /* async, cmd->obj = 0 */
#define QD3D_OP_SOURCE_SHOWN 13u /* async, cmd->obj = device or swap chain */
#define QD3D_OP_METHOD_BASE  64u

/*
 * HELLO: starts a session. The host releases every object of the channel's
 * previous session (a crashed or detached owner of the slot), then binds
 * the new IDirect3D9 id to its IDirect3D9Ex and replies.
 *   payload: struct qd3d_hello
 *   reply.ret: HRESULT; payload: struct qd3d_hello_reply
 */
struct qd3d_hello {
    uint32_t abi;          /* QD3D_ABI_VERSION */
    uint32_t pid;          /* guest process id, informational */
    uint32_t sdk_version;  /* Direct3DCreate9 argument */
    uint32_t d3d_id;       /* guest id for the IDirect3D9 proxy */
};

struct qd3d_hello_reply {
    uint32_t abi;          /* host QD3D_ABI_VERSION */
    uint32_t flags;        /* reserved, 0 */
    uint32_t max_cmd;      /* largest accepted command (ring_size / 2) */
    uint32_t max_reply;    /* reply_size - sizeof(struct qd3d_reply) */
};

/*
 * GOODBYE: the guest process is detaching. The host releases every object of
 * the session. No payload, no reply.
 */

/*
 * Present parameters on the wire. hDeviceWindow is the guest HWND value and
 * is informational only; the host renders off screen. The guest resolves
 * zero width/height to its window's client size before sending.
 */
struct qd3d_wire_pp {
    uint32_t BackBufferWidth;
    uint32_t BackBufferHeight;
    uint32_t BackBufferFormat;
    uint32_t BackBufferCount;
    uint32_t MultiSampleType;
    uint32_t MultiSampleQuality;
    uint32_t SwapEffect;
    uint32_t hDeviceWindow;
    uint32_t Windowed;
    uint32_t EnableAutoDepthStencil;
    uint32_t AutoDepthStencilFormat;
    uint32_t Flags;
    uint32_t FullScreen_RefreshRateInHz;
    uint32_t PresentationInterval;
};

/*
 * CREATE_DEVICE: IDirect3D9::CreateDevice.
 *   cmd->obj: IDirect3D9 id
 *   payload: struct qd3d_create_device
 *   reply.ret: HRESULT; payload: struct qd3d_wire_pp (as accepted)
 * On failure the host binds nothing and device_id stays unused.
 */
struct qd3d_create_device {
    uint32_t device_id;       /* guest id for the new device */
    uint32_t adapter;
    uint32_t device_type;     /* D3DDEVTYPE as requested by the guest */
    uint32_t behavior_flags;  /* D3DCREATE_* as requested by the guest */
    struct qd3d_wire_pp pp;
};

/*
 * RESET: IDirect3DDevice9::Reset.
 *   payload: struct qd3d_wire_pp
 *   reply.ret: HRESULT; payload: struct qd3d_wire_pp (as accepted)
 */

/*
 * CREATE_SWAPCHAIN: IDirect3DDevice9::CreateAdditionalSwapChain. The guest
 * resolves the defaults (window, size, count) and refuses what the native
 * runtime refuses (a fullscreen swap chain or device) before sending.
 *   cmd->obj: device id
 *   payload: struct qd3d_create_swapchain
 *   reply.ret: HRESULT; payload: struct qd3d_wire_pp (as accepted)
 * On failure the host binds nothing and swapchain_id stays unused.
 */
struct qd3d_create_swapchain {
    uint32_t swapchain_id;    /* guest id for the new swap chain */
    struct qd3d_wire_pp pp;
};

/*
 * PRESENT: Device::Present or SwapChain::Present. The host publishes the
 * presented image (the source rectangle of the back buffer, all of it
 * without one) to its frame sink, then increments ctrl->frames_done.
 * Publication may trail by one frame: a frame reaches the sink at the latest
 * when the host executes the next PRESENT or goes idle, and frames_done
 * counts frames that reached the sink (or that the host decided not to
 * show). The guest increments frames_submitted before publishing the
 * command and bounds frames_submitted - frames_done.
 *
 * screen is where the image lands in guest screen coordinates, as the
 * application sees its desktop: windowed, the destination rectangle (the
 * presentation window's client area unless a destination rectangle was
 * given) mapped to the screen; fullscreen, 0, 0 and the back buffer size.
 * It places the image in GetFrontBufferData and maps the cursor into it;
 * empty (right <= left or bottom <= top) when the window shows nothing.
 * When cursor is 1 the host draws the session's cursor image (CURSOR)
 * with its hot spot at cursor_pos (guest screen coordinates) over the
 * published frame only; the back buffer and GetFrontBufferData never
 * contain it, as with a hardware cursor. cursor and cursor_pos report the
 * guest's cursor whatever the screen rectangle.
 *
 * hidden is 1 when the presentation window (the override, else the swap
 * chain's device window, else the focus window) is minimized, or hidden
 * after it showed at an earlier present of this source: natively nothing
 * of the frame shows and the desktop does. The host does not publish such
 * a frame (frames_done still counts it); when its source is the one whose
 * frames were published last, the embedder learns that the desktop shows
 * (qd3dx.h, source_hidden) until the window shows again. A window that
 * never showed (a helper window a front end presents through) never
 * hides its frames.
 */
struct qd3d_present {
    uint32_t flags;           /* D3DPRESENT_* (swap chain form) */
    uint32_t has_src;
    int32_t  src[4];          /* RECT left, top, right, bottom */
    uint32_t has_dst;
    int32_t  dst[4];
    uint32_t dest_window;     /* guest HWND override, informational */
    int32_t  screen[4];       /* RECT, guest screen coordinates */
    uint32_t cursor;          /* 1: draw the cursor image */
    int32_t  cursor_pos[2];   /* x, y of the hot spot, guest screen */
    uint32_t hidden;          /* 1: the window is minimized or hidden */
};

/*
 * CURSOR: the guest's current cursor image for the session (the Windows
 * cursor the guest shows, which a Direct3D device cursor becomes as with
 * the native runtime). Every later PRESENT with cursor = 1 draws it.
 *   payload: struct qd3d_cursor, then width * height 32-bit pixels, rows
 *            top-down, bytes B, G, R, A: A = 255 opaque; 0 < A < 255
 *            blended (straight alpha); A = 0 with B = G = R = 0
 *            transparent; A = 0 otherwise XORs B, G, R into the frame
 *            (inverting cursor pixels)
 * width = height = 0 clears the image (hot spots 0, no pixels).
 */
#define QD3D_CURSOR_MAX 256u /* largest width and height */

struct qd3d_cursor {
    uint32_t width;
    uint32_t height;
    uint32_t hot_x;           /* < width (0 when width is 0) */
    uint32_t hot_y;           /* < height */
};

/*
 * CURSOR_POS: the cursor the guest shows moved, appeared, went away or got
 * another image (CURSOR first) between presents. While a frame source that
 * presented lives, a guest thread samples the cursor about every 8 ms and
 * sends this when it differs from what the last PRESENT or CURSOR_POS
 * said. The host shows it like a hardware cursor plane: a frame still in
 * flight is published with it; otherwise the last published frame goes to
 * the frame sink again with the cursor there (qd3dx.h, QD3DX_REPEAT_CURSOR;
 * never read back again; at most 125 a second, a faster burst ending with
 * its last state; nothing when neither that frame's previous publication
 * nor this one draws the cursor, or while its window is hidden). A
 * PRESENT supersedes it.
 *   payload: struct qd3d_cursor_pos
 */
struct qd3d_cursor_pos {
    uint32_t visible;         /* 1: the CURSOR image shows at pos */
    int32_t  pos[2];          /* x, y of the hot spot, guest screen */
};

/*
 * SOURCE_SHOWN: the presentation window of a frame source (cmd->obj: a
 * device, for its implicit swap chain, or an additional swap chain) was
 * minimized or hidden (shown = 0), or shows again (shown = 1), between its
 * presents; the same rule as PRESENT hidden, sampled with the cursor. When
 * the source is the one whose frames were published last, the host
 * publishes what was presented before, then tells the embedder that the
 * desktop shows (shown = 0), or publishes that source's last frame again
 * (shown = 1, QD3DX_REPEAT_SHOWN). Other sources: nothing.
 *   payload: struct qd3d_source_shown
 */
struct qd3d_source_shown {
    uint32_t shown;
};

/*
 * UPLOAD: guest-written contents of a locked region, applied by the host
 * after a guest Unlock (managed resources: before their first use). Large
 * regions arrive as several UPLOAD commands, each describing a disjoint
 * sub-range; the first carries the guest lock flags and later ones carry
 * QD3D_UPLOAD_CONTINUE.
 *
 * Addressing:
 *   QD3D_UPLOAD_BUFFER   x = byte offset, w = byte count
 *   QD3D_UPLOAD_SURFACE  x, y = top-left texel, w, h = size in texels, in
 *                        the format's lock view (below); for block views
 *                        x, y, w, h are multiples of the block size (w, h
 *                        may be rounded up to the level size) and rows are
 *                        block rows
 *   QD3D_UPLOAD_VOLUME   x, y, z and w, h, d in texels
 *
 * Lock views are those of the native runtime, chosen by the pool the guest
 * created the resource in (runtime memory for MANAGED, SYSTEMMEM and
 * SCRATCH; driver memory for DEFAULT):
 *   DXT1..DXT5           4x4 blocks of 8/16 bytes, every pool
 *   ATI1N, ATI2N         runtime pools: 1x1 blocks of 1 byte; DEFAULT: 4x4
 *                        blocks of 8/16 bytes
 *   YV12, NV12           runtime pools: 1x1 blocks of 1 byte, luma only (the
 *                        runtime allocates no chroma there); DEFAULT: planar,
 *                        even W and H only (the driver does not round-trip
 *                        odd sizes), sent whole (x = y = 0, w, h = the
 *                        surface size, row_pitch P even): H luma rows of W
 *                        bytes at P, then YV12: V plane and U plane, each
 *                        H/2 rows of W/2 bytes at P/2; NV12: H/2 interleaved
 *                        UV rows of W bytes at P; data_size = P*H*3/2
 *   UYVY, YUY2, R8G8_B8G8, G8R8_G8B8   2x1 blocks of 4 bytes
 *   other formats        1x1 blocks of their bytes per texel; other FOURCCs
 *                        are never sent
 *
 * cmd->obj names a vertex/index buffer, a surface, a volume, or a texture
 * (then level/face select the sub-resource).
 */
#define QD3D_UPLOAD_BUFFER   1u
#define QD3D_UPLOAD_SURFACE  2u
#define QD3D_UPLOAD_VOLUME   3u

#define QD3D_UPLOAD_CONTINUE 0x80000000u /* in lock_flags: not the first chunk */

struct qd3d_region {
    uint32_t kind;        /* QD3D_UPLOAD_* */
    uint32_t level;       /* texture mip level, else 0 */
    uint32_t face;        /* D3DCUBEMAP_FACES for cube textures, else 0 */
    uint32_t lock_flags;  /* D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE subset,
                             D3DLOCK_NO_DIRTY_UPDATE (SYSTEMMEM: the write
                             records no dirty region for UpdateTexture),
                             plus QD3D_UPLOAD_CONTINUE */
    uint32_t x, y, z;
    uint32_t w, h, d;
    uint32_t row_pitch;   /* bytes per (block) row in the payload */
    uint32_t slice_pitch; /* bytes per slice in the payload (volumes) */
    uint32_t data_size;   /* payload bytes following (UPLOAD) or requested
                             reply bytes (READBACK) */
    uint32_t reserved;
};

/*
 * READBACK: host-side contents of a region, for guest locks of resources the
 * GPU wrote (render-target data, GetRenderTargetData destinations,
 * ProcessVertices destinations).
 *   payload: struct qd3d_region (data_size = bytes requested)
 *   reply.ret: HRESULT; payload: data_size bytes laid out with the
 *              requested row_pitch / slice_pitch
 * data_size must not exceed hello_reply.max_reply; the guest splits larger
 * regions into several READBACK commands.
 */

/*
 * QUERY_DATA: IDirect3DQuery9::GetData.
 *   payload: struct qd3d_query_data
 *   reply.ret: HRESULT (S_OK, S_FALSE, or an error); payload: size bytes
 *              when ret == S_OK and size > 0, otherwise empty
 */
struct qd3d_query_data {
    uint32_t size;        /* dwSize, at most 256 */
    uint32_t flags;       /* D3DGETDATA_* */
};

#endif /* QD3D_PROTO_H */
