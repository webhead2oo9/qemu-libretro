/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Embedding API of the QD3D host executor, used by the loopback harness
 * (qd3d/host/loopback) and the qemu-libretro device. Except for
 * qd3dx_request_release, every function must be called on the same thread:
 * the executor owns Direct3D 9Ex objects and a window of that thread.
 *
 * Embedder loop:
 *
 *     ex = qd3dx_create(base, size, &hooks);
 *     for (;;) {
 *         if (qd3dx_poll(ex, 256) > 0)
 *             continue;
 *         if (qd3dx_prepare_wait(ex))
 *             continue;              // work arrived while going idle
 *         wait for the doorbell (bounded, e.g. 2 ms) or a stop request;
 *         qd3dx_wake(ex);
 *     }
 *     qd3dx_destroy(ex);
 *
 * Channel slots (qd3d_proto.h, Transports) run one executor per slot, all
 * on one thread with one doorbell: each round polls every executor, calls
 * qd3dx_prepare_wait on every one whose poll returned 0, waits only when
 * none of those returned 1, then calls qd3dx_wake on each of them. What to
 * show of several slots (and when the guest's desktop) is the display
 * policy at the end of this header.
 */
#ifndef QD3DX_H
#define QD3DX_H

#include <stdint.h>

struct qd3dx;

/* A frame: top-down rows of 32-bit pixels in B, G, R, X byte order
 * (D3DFMT_X8R8G8B8 memory layout). Valid only during the callback. */
struct qd3dx_frame {
    const void *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t repeat;    /* QD3DX_REPEAT_* */
};

/* struct qd3dx_frame repeat: only presented frames are new pictures; a
 * repeat is the last published frame again (its system-memory copy, never
 * read back again) with the cursor where the guest reported it last. */
#define QD3DX_REPEAT_NONE   0u /* a frame the guest presented */
/* Only the cursor over it moved, appeared, went away or changed between
 * presents (QD3D_OP_CURSOR_POS; at most 125 a second, a faster burst ends
 * with its last state): show it only where its source's frames are shown. */
#define QD3DX_REPEAT_CURSOR 1u
/* Its source's window shows again (QD3D_OP_SOURCE_SHOWN), or the embedder
 * asked for it (qd3dx_show_again): show it like a presented frame. */
#define QD3DX_REPEAT_SHOWN  2u

struct qd3dx_hooks {
    void *user;
    /* Called on the executor thread for every presented frame, in present
     * order and at most one frame after its PRESENT executed: from
     * qd3dx_poll (the next PRESENT, Reset, GetFrontBufferData, the release
     * of the frame's device, GOODBYE) or from qd3dx_prepare_wait (the frame
     * still in flight when the guest went quiet); and for every repeat of
     * the last published frame (struct qd3dx_frame repeat). */
    void (*frame)(void *user, const struct qd3dx_frame *frame);
    /* One diagnostic line without a trailing newline. May be NULL. */
    void (*log)(void *user, const char *line);
    /* Called on the executor thread when the source of the last frame this
     * executor published is gone: its device, or the additional swap chain
     * it was presented through, was released (by the guest, or because its
     * session ended: GOODBYE, the next HELLO on the channel, qd3dx_reset,
     * qd3dx_request_release). Every frame presented before an ordinary
     * release has reached the frame hook by then; a new session, reset or
     * requested release drops the frame still in flight instead. The
     * embedder can stop showing frames at once. The implicit swap chain ends
     * only with its device. Not called by qd3dx_destroy. May be NULL. */
    void (*source_released)(void *user);
    /* Called on the executor thread when the window of the source of the
     * last frame this executor published was minimized, or hidden after it
     * showed (PRESENT hidden, QD3D_OP_SOURCE_SHOWN): natively the desktop
     * shows then. Every frame presented before has reached the frame hook.
     * Nothing of that source is published again until its window shows:
     * then its last frame comes again (QD3DX_REPEAT_SHOWN) or a new one is
     * presented. May be NULL. */
    void (*source_hidden)(void *user);
};

/*
 * Initialises the channel in base[0, size) (control page written with
 * magic last) and the executor. size must be at least QD3D_DEFAULT_SIZE;
 * extra bytes are unused. Direct3D is created lazily by the first HELLO.
 * Returns NULL on failure (nothing is left initialised).
 */
struct qd3dx *qd3dx_create(void *base, uint32_t size, const struct qd3dx_hooks *hooks);

/*
 * Executes at most budget commands, servicing requested releases before
 * commands and before returning (even with budget 0). Returns the number
 * executed (0 when the ring is empty), or -1 when the session is faulted:
 * host_state is QD3D_HOST_FAULT until reset or a requested release.
 */
int qd3dx_poll(struct qd3dx *ex, uint32_t budget);

/*
 * Services requested releases, publishes the frame still in flight if any
 * (waiting for its readback), then host_idle = 1, and re-checks the ring
 * and release request. Returns 1 (and clears host_idle) if work arrived,
 * in which case the caller must not wait; 0 if it may wait for a doorbell.
 */
int qd3dx_prepare_wait(struct qd3dx *ex);

/* Clears host_idle after a wait. */
void qd3dx_wake(struct qd3dx *ex);

/*
 * Releases every object of the current session and returns the channel to
 * QD3D_HOST_READY, discarding queued packets, for example after a fault or
 * system reset. A frame still in flight is dropped, not published. A
 * pending ownership release takes precedence, preserving packets of its
 * successor (fault recovery may race a new claim). For ownership handoff
 * always use qd3dx_request_release.
 */
void qd3dx_reset(struct qd3dx *ex);

/*
 * The sole cross-thread API: atomically requests the current slot owner's
 * release, without calling Direct3D or any hook, waiting for the executor,
 * or changing the shared channel. The caller must keep ex alive, serialize
 * release notifications in ownership order, and kick the executor after
 * the call. Before it, the old producer must be unmapped/quiescent; only
 * after it returns may the slot be claimed again.
 *
 * Captures the current ring head. poll/prepare_wait on the executor thread
 * drop the abandoned frame, release all session objects, notify
 * source_released if applicable, clear fault/liveness state and discard
 * packets through that captured head, including any stale queued HELLO.
 * Native work or a hook already begun can finish before teardown.
 * The next owner appends (never resets cursors), starting with a synchronous
 * HELLO: its packets after the captured head survive. It cannot submit
 * frames until HELLO replies, so release can retire the old frame count.
 * Pending requests coalesce to the latest boundary; repeated releases of
 * an unchanged owner/idle slot are harmless.
 *
 * The VM's QD3D_VM_REG_RELEASE handler calls this then signals its kick.
 * Loopback provides manual-reset NAME_RELEASE_<i> events, including slot
 * 0: quiesce the producer, SetEvent, then wait until the event is reset
 * before closing its claim or appending a new owner's HELLO. The harness
 * resets it only after this call captured the boundary (not necessarily
 * after D3D teardown); the event also wakes its executor loop.
 */
void qd3dx_request_release(struct qd3dx *ex);

/*
 * 1 when the guest showed signs of life since the last call: a command
 * executed, or its heartbeat (struct qd3d_ctrl) moved, which a guest
 * advances while a frame source that presented lives, however static its
 * screen. For the display policy below; call it after every poll.
 */
int qd3dx_alive(struct qd3dx *ex);

/*
 * Publishes the last published frame again (QD3DX_REPEAT_SHOWN) when its
 * source still lives and shows and no frame is in flight; nothing
 * otherwise. For an embedder that stopped showing it and wants it back
 * (qd3dx_display_alive).
 */
void qd3dx_show_again(struct qd3dx *ex);

/* Guest process id of the current session (0 before the first HELLO). */
uint32_t qd3dx_session_pid(const struct qd3dx *ex);

/* Releases everything (dropping a frame still in flight, calling no hook),
 * stores host_state = QD3D_HOST_DOWN, frees ex. */
void qd3dx_destroy(struct qd3dx *ex);

/* ------------------------------------------------------------------------ */
/* Display policy (qd3dx_display.c)                                         */
/* ------------------------------------------------------------------------ */
/*
 * What to show when the display is either the frames of one executor (a
 * channel slot) as the whole screen, or the guest's desktop: the VM device
 * follows it, the loopback harness logs it. Times are microseconds of one
 * monotonic clock.
 *
 * - A presented frame (QD3DX_REPEAT_NONE), or one shown again
 *   (QD3DX_REPEAT_SHOWN), of any slot shows: 3D output on.
 * - A cursor re-publication (QD3DX_REPEAT_CURSOR) shows only while 3D
 *   output is on with its slot's frames.
 * - The desktop comes back at once when the source of the frames shown is
 *   released or its window minimized or hidden (source_released,
 *   source_hidden of that slot), and once the slot shown gave no sign of
 *   life (qd3dx_alive) for QD3DX_DISPLAY_IDLE_US: its process died without
 *   GOODBYE. A live guest beats its heartbeat while a frame source that
 *   presented lives, so a screen that redraws only on change stays up.
 * - When that slot shows life again (a stalled or paused guest resumed),
 *   the embedder asks for its frames again (qd3dx_show_again).
 */
#define QD3DX_DISPLAY_IDLE_US 3000000u

struct qd3dx_display {
    int on;             /* 3D output on: the display shows slot's frames */
    int idle_off;       /* the desktop replaced them for slot's silence */
    unsigned slot;      /* slot of the last frame shown */
    uint64_t alive_us;  /* its last sign of life */
};

/* qd3dx_display_frame results */
#define QD3DX_DISPLAY_DROP 0 /* not shown */
#define QD3DX_DISPLAY_SHOW 1 /* shown, 3D output was on */
#define QD3DX_DISPLAY_ON   2 /* shown, 3D output turns on with it */

void qd3dx_display_init(struct qd3dx_display *d);
/* A frame of slot reached the embedder's frame hook at now. */
int qd3dx_display_frame(struct qd3dx_display *d, unsigned slot, uint32_t repeat, uint64_t now);
/* source_released or source_hidden of slot: 1 when the desktop comes back
 * now (3D output was on with slot's frames). */
int qd3dx_display_gone(struct qd3dx_display *d, unsigned slot);
/* qd3dx_alive of slot returned 1 at now: 1 when the desktop replaced its
 * frames for silence; the embedder then asks for them (qd3dx_show_again),
 * once. */
int qd3dx_display_alive(struct qd3dx_display *d, unsigned slot, uint64_t now);
/* Once per poll round: 1 when 3D output turns off now because the slot
 * shown was silent for QD3DX_DISPLAY_IDLE_US. */
int qd3dx_display_idle(struct qd3dx_display *d, uint64_t now);
/* The desktop comes back whatever was shown (system reset, shutdown): 1
 * when 3D output was on. */
int qd3dx_display_off(struct qd3dx_display *d);

#endif /* QD3DX_H */
