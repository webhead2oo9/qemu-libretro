/*
 * QD3D Direct3D 9 remoting device for the libretro core.
 *
 * A replacement d3d9.dll in the Windows XP guest serialises Direct3D 9 calls
 * into a channel (XPDriver qd3d/proto/qd3d_proto.h). This device exposes
 * QD3D_VM_SLOTS channels, one per Direct3D process (qxpmap.sys owns the
 * slot claims and notifies releases), as guest-physical RAM from
 * QD3D_VM_CHANNEL_GPA plus a register page at QD3D_VM_MMIO_GPA, and runs
 * one XPDriver executor per slot (hw/qd3d/host, a synced copy, see
 * scripts/qd3d-sync.sh), all on one thread of its own. The executors replay
 * the calls on Direct3D 9Ex;
 * presented frames of every slot go to the libretro frontend through the
 * qemu-3dfx frame sink.
 *
 * The guest writes the ring directly into RAM (no VM exit); the doorbell
 * exits only when the executor reported itself idle. A kernel RELEASE
 * write captures the old owner's ring boundary and kicks that same thread;
 * all Direct3D teardown remains on the executor thread.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/i386/pc.h"
#include "exec/address-spaces.h"
#include "ui/console.h"

#include <windows.h>

#include "proto/qd3d_proto.h"
#include "host/qd3dx.h"

QEMU_BUILD_BUG_ON(QD3D_VM_MMIO_GPA != QD3DPT_MM_BASE);
QEMU_BUILD_BUG_ON(QD3D_VM_CHANNEL_SIZE < QD3D_DEFAULT_SIZE);
QEMU_BUILD_BUG_ON((uint64_t)QD3D_VM_CHANNEL_GPA +
                  (uint64_t)QD3D_VM_SLOTS * QD3D_VM_CHANNEL_SIZE >
                  QD3D_VM_MMIO_GPA);
QEMU_BUILD_BUG_ON(QD3D_VM_REG_SLOTS >= QD3D_VM_MMIO_SIZE);
QEMU_BUILD_BUG_ON(QD3D_VM_REG_RELEASE >= QD3D_VM_MMIO_SIZE);

/* What the frontend shows follows the executors' display policy (qd3dx.h):
 * 3D output ends as soon as the device or swap chain whose frames are shown
 * is released or its window minimized or hidden (source_released,
 * source_hidden; GOODBYE, a kernel slot release and a slot reset release
 * it too), and when the slot shown gives no sign of life for
 * QD3DX_DISPLAY_IDLE_US. A live guest beats its heartbeat however static
 * its screen, so menus that redraw only on change stay up, and a slot that
 * lives again after such silence (a paused VM resumed) shows its frame. */

/* A faulted session is kept for inspection this long, then its slot is
 * reset so later guest processes can use it. */
#define QD3DPT_FAULT_HOLD_US 1000000
#define QD3DPT_WAIT_MS      2
#define QD3DPT_POLL_BUDGET  256
/* Polling window after the last executed command before going idle. */
#define QD3DPT_SPIN_US      200

OBJECT_DECLARE_SIMPLE_TYPE(QD3DPTState, QD3DPT)

/* One channel slot. Only ex's atomic release mailbox is used off-thread. */
typedef struct QD3DPTSlot {
    QD3DPTState *s;
    unsigned index;
    struct qd3dx *ex;
    int n;                /* last qd3dx_poll result */
    int64_t fault_since;
} QD3DPTSlot;

struct QD3DPTState {
    SysBusDevice parent_obj;

    MemoryRegion channel_ram; /* every slot, back to back */
    MemoryRegion iomem;
    uint8_t *channels;

    QemuThread thread;
    HANDLE kick;          /* auto-reset doorbell event, every slot */
    bool thread_started;
    int stop;             /* atomic: thread exit request */
    int reset_req;        /* atomic: system reset request */
    int slots;            /* atomic: slots served (QD3D_VM_REG_SLOTS) */
    int bad_release_logged; /* atomic: report an invalid slot only once */

    /* executor thread only */
    QD3DPTSlot slot[QD3D_VM_SLOTS];
    uint8_t *tight;       /* repacked frame when pitch != width * 4 */
    size_t tight_size;
    bool active;          /* frontend shows our frames */
    struct qd3dx_display disp;
    /* QD3D_FPS: every 2 s while 3D output is on, the game's presented
     * frames per second and, when the cursor moved between presents in
     * that window, the display's updates per second (every frame shown,
     * re-publications included), with the size of the last frame shown.
     * A window starts with a frame shown and counts the frames after it. */
    bool fps_log;
    int64_t fps_t0;        /* window start; 0: none (3D output off) */
    uint32_t fps_frames;   /* presented frames shown since fps_t0 */
    uint32_t fps_updates;  /* frames of any kind shown since fps_t0 */
    uint32_t fps_cursor;   /* cursor re-publications among them */
    uint32_t fps_w, fps_h; /* size of the last frame shown */
};

/* QD3D_FPS: the line for a window of 2 s or more, then the next window. A
 * static screen reads as such ("0.0 fps game"), a menu that only moves the
 * cursor as a slow game with a fast display. */
static void qd3dpt_fps_report(QD3DPTState *s, int64_t now)
{
    int64_t span = now - s->fps_t0;

    if (!s->fps_log || !s->fps_t0 || span < 2000000) {
        return;
    }
    if (s->fps_cursor) {
        info_report("qd3d: %.1f fps game, %.0f display updates/s (cursor), "
                    "%ux%u", s->fps_frames * 1e6 / (double)span,
                    s->fps_updates * 1e6 / (double)span, s->fps_w, s->fps_h);
    } else {
        info_report("qd3d: %.1f fps game, %ux%u",
                    s->fps_frames * 1e6 / (double)span, s->fps_w, s->fps_h);
    }
    s->fps_t0 = now;
    s->fps_frames = 0;
    s->fps_updates = 0;
    s->fps_cursor = 0;
}

static void qd3dpt_set_active(QD3DPTState *s, bool on)
{
    if (s->active == on) {
        return;
    }
    s->active = on;
    s->fps_t0 = 0;
    bql_lock();
    qd3d_set_active(on);
    bql_unlock();
}

/* Frames of every slot go to the one sink as the display policy decides. */
static void qd3dpt_frame(void *user, const struct qd3dx_frame *f)
{
    QD3DPTSlot *sl = user;
    QD3DPTState *s = sl->s;
    size_t row = (size_t)f->width * 4;
    const void *pixels = f->pixels;
    int64_t now = g_get_monotonic_time();

    if (!f->width || !f->height || f->width > QEMU_FX_MAX_WIDTH ||
        f->height > QEMU_FX_MAX_HEIGHT || f->pitch < row) {
        warn_report("qd3d: slot %u: dropping frame %ux%u pitch %u",
                    sl->index, f->width, f->height, f->pitch);
        return;
    }
    if (qd3dx_display_frame(&s->disp, sl->index, f->repeat, (uint64_t)now) ==
        QD3DX_DISPLAY_DROP) {
        return;
    }
    if (f->pitch != row) {
        size_t need = row * f->height;
        uint32_t y;

        if (s->tight_size < need) {
            g_free(s->tight);
            s->tight = g_malloc(need);
            s->tight_size = need;
        }
        for (y = 0; y < f->height; y++) {
            memcpy(s->tight + y * row,
                   (const uint8_t *)f->pixels + (size_t)y * f->pitch, row);
        }
        pixels = s->tight;
    }

    bql_lock();
    if (!s->active) {
        s->active = true;
        s->fps_t0 = 0;
        qd3d_set_active(true);
    }
    qd3d_publish_frame(pixels, (int)f->width, (int)f->height);
    bql_unlock();
    if (s->fps_log) {
        s->fps_w = f->width;
        s->fps_h = f->height;
        if (!s->fps_t0) {
            s->fps_t0 = now;
            s->fps_frames = 0;
            s->fps_updates = 0;
            s->fps_cursor = 0;
        } else {
            s->fps_updates++;
            s->fps_frames += f->repeat == QD3DX_REPEAT_NONE;
            s->fps_cursor += f->repeat == QD3DX_REPEAT_CURSOR;
            qd3dpt_fps_report(s, now);
        }
    }
}

static void qd3dpt_log(void *user, const char *line)
{
    QD3DPTSlot *sl = user;

    info_report("qd3d: slot %u: %s", sl->index, line);
}

/* The device or swap chain that presented this slot's last published frame
 * was released, or its window minimized or hidden. When those are the
 * frames shown, the desktop comes back now, as it reappears natively
 * (leaving exclusive mode, alt-tab, returning to a launcher that lives
 * on). A frame from any source brings 3D back. */
static void qd3dpt_source_gone(void *user)
{
    QD3DPTSlot *sl = user;

    if (qd3dx_display_gone(&sl->s->disp, sl->index)) {
        qd3dpt_set_active(sl->s, false);
    }
}

/* Per-slot bookkeeping after every poll round, busy or not: faulted
 * sessions are reset after the hold, 3D output ends when the slot shown
 * fell silent (a released or hidden source ends it at once), and QD3D_FPS
 * reports a window that ended without a frame (a static screen). */
static void qd3dpt_watch(QD3DPTState *s, int64_t now)
{
    unsigned i;

    for (i = 0; i < QD3D_VM_SLOTS; i++) {
        QD3DPTSlot *sl = &s->slot[i];

        if (sl->n < 0) {
            if (!sl->fault_since) {
                sl->fault_since = now;
            } else if (now - sl->fault_since > QD3DPT_FAULT_HOLD_US) {
                warn_report("qd3d: slot %u: resetting faulted session", i);
                qd3dx_reset(sl->ex);
                sl->fault_since = 0;
            }
        } else {
            sl->fault_since = 0;
        }
    }
    if (qd3dx_display_idle(&s->disp, (uint64_t)now)) {
        info_report("qd3d: slot %u silent for %u s: guest desktop",
                    s->disp.slot, QD3DX_DISPLAY_IDLE_US / 1000000u);
        qd3dpt_set_active(s, false);
    }
    qd3dpt_fps_report(s, now);
}

static void *qd3dpt_thread(void *opaque)
{
    QD3DPTState *s = opaque;
    int64_t last_work = 0;
    unsigned i;

    for (i = 0; i < QD3D_VM_SLOTS; i++) {
        QD3DPTSlot *sl = &s->slot[i];
        const struct qd3dx_hooks hooks = {
            .user = sl,
            .frame = qd3dpt_frame,
            .log = qd3dpt_log,
            .source_released = qd3dpt_source_gone,
            .source_hidden = qd3dpt_source_gone,
        };

        sl->s = s;
        sl->index = i;
        sl->ex = qd3dx_create(s->channels + (size_t)i * QD3D_VM_CHANNEL_SIZE,
                              QD3D_VM_CHANNEL_SIZE, &hooks);
        if (!sl->ex) {
            error_report("qd3d: executor initialisation failed; "
                         "Direct3D remoting is unavailable");
            while (i--) {
                qd3dx_destroy(s->slot[i].ex);
                s->slot[i].ex = NULL;
            }
            return NULL;
        }
    }
    qd3dx_display_init(&s->disp);
    qatomic_store_release(&s->slots, QD3D_VM_SLOTS);

    while (!qatomic_read(&s->stop)) {
        bool work = false, idle = false, busy = false;
        int64_t now;

        if (qatomic_xchg(&s->reset_req, 0)) {
            for (i = 0; i < QD3D_VM_SLOTS; i++) {
                qd3dx_reset(s->slot[i].ex);
                s->slot[i].fault_since = 0;
            }
            qd3dx_display_off(&s->disp);
            qd3dpt_set_active(s, false);
        }

        for (i = 0; i < QD3D_VM_SLOTS; i++) {
            QD3DPTSlot *sl = &s->slot[i];

            sl->n = qd3dx_poll(sl->ex, QD3DPT_POLL_BUDGET);
            work |= sl->n > 0;
            idle |= sl->n == 0;
            if (qd3dx_alive(sl->ex) &&
                qd3dx_display_alive(&s->disp, i,
                                    (uint64_t)g_get_monotonic_time())) {
                qd3dx_show_again(sl->ex);
            }
        }
        now = g_get_monotonic_time();
        qd3dpt_watch(s, now);
        if (work) {
            last_work = now;
            continue;
        }
        /* Keep polling briefly after the last command so a rendering
         * guest never sees host_idle and never takes a doorbell VM exit. */
        if (idle && now - last_work < QD3DPT_SPIN_US) {
            cpu_relax();
            continue;
        }
        /* Sleep only when no slot has work; any doorbell wakes. */
        for (i = 0; i < QD3D_VM_SLOTS; i++) {
            if (s->slot[i].n == 0 && qd3dx_prepare_wait(s->slot[i].ex)) {
                busy = true;
            }
        }
        if (!busy) {
            WaitForSingleObject(s->kick, QD3DPT_WAIT_MS);
        }
        for (i = 0; i < QD3D_VM_SLOTS; i++) {
            if (s->slot[i].n == 0) {
                qd3dx_wake(s->slot[i].ex);
            }
        }
    }

    qatomic_store_release(&s->slots, 0);
    for (i = 0; i < QD3D_VM_SLOTS; i++) {
        qd3dx_destroy(s->slot[i].ex);
        s->slot[i].ex = NULL;
    }
    qd3dx_display_off(&s->disp);
    qd3dpt_set_active(s, false);
    return NULL;
}

static uint64_t qd3dpt_read(void *opaque, hwaddr addr, unsigned size)
{
    QD3DPTState *s = opaque;

    switch (addr) {
    case QD3D_VM_REG_ID:
        return QD3D_MAGIC;
    case QD3D_VM_REG_SLOTS:
        return qatomic_read(&s->slots);
    default:
        return 0;
    }
}

static void qd3dpt_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    QD3DPTState *s = opaque;

    switch (addr) {
    case QD3D_VM_REG_KICK:
        SetEvent(s->kick);
        break;
    case QD3D_VM_REG_RELEASE:
        if (val >= QD3D_VM_SLOTS) {
            if (!qatomic_xchg(&s->bad_release_logged, 1)) {
                warn_report("qd3d: ignoring out-of-range release slot %" PRIu64,
                            val);
            }
            break;
        }
        if (val < (uint64_t)qatomic_load_acquire(&s->slots)) {
            /* The driver quiesced the old producer before this write and
             * frees ownership only after it returns. Capture head now,
             * not when the executor eventually sees the kick: by then a
             * new owner may have appended its synchronous HELLO. */
            qd3dx_request_release(s->slot[val].ex);
            SetEvent(s->kick);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps qd3dpt_ops = {
    .read = qd3dpt_read,
    .write = qd3dpt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void qd3dpt_init(Object *obj)
{
    QD3DPTState *s = QD3DPT(obj);

    memory_region_init_ram(&s->channel_ram, obj, "qd3d-channels",
                           (uint64_t)QD3D_VM_SLOTS * QD3D_VM_CHANNEL_SIZE,
                           &error_fatal);
    s->channels = memory_region_get_ram_ptr(&s->channel_ram);
    memory_region_add_subregion(get_system_memory(), QD3D_VM_CHANNEL_GPA,
                                &s->channel_ram);

    memory_region_init_io(&s->iomem, obj, &qd3dpt_ops, s, TYPE_QD3DPT,
                          QD3D_VM_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void qd3dpt_realize(DeviceState *dev, Error **errp)
{
    QD3DPTState *s = QD3DPT(dev);

    s->kick = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!s->kick) {
        error_setg(errp, "qd3d: CreateEvent failed (%lu)", GetLastError());
        return;
    }
    s->fps_log = g_getenv("QD3D_FPS") != NULL;
    qemu_thread_create(&s->thread, "qd3d-exec", qd3dpt_thread, s,
                       QEMU_THREAD_JOINABLE);
    s->thread_started = true;
}

static void qd3dpt_reset(DeviceState *dev)
{
    QD3DPTState *s = QD3DPT(dev);

    /* The guest rebooted: drop every object of every slot's session. */
    qatomic_set(&s->reset_req, 1);
    if (s->kick) {
        SetEvent(s->kick);
    }
}

static void qd3dpt_finalize(Object *obj)
{
    QD3DPTState *s = QD3DPT(obj);

    if (s->thread_started) {
        qatomic_set(&s->stop, 1);
        SetEvent(s->kick);
        qemu_thread_join(&s->thread);
        s->thread_started = false;
    }
    if (s->kick) {
        CloseHandle(s->kick);
        s->kick = NULL;
    }
    g_free(s->tight);
    s->tight = NULL;
}

static void qd3dpt_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = qd3dpt_realize;
    device_class_set_legacy_reset(dc, qd3dpt_reset);
    dc->user_creatable = false;
}

static const TypeInfo qd3dpt_info = {
    .name = TYPE_QD3DPT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QD3DPTState),
    .instance_init = qd3dpt_init,
    .instance_finalize = qd3dpt_finalize,
    .class_init = qd3dpt_class_init,
};

static void qd3dpt_register_type(void)
{
    type_register_static(&qd3dpt_info);
}

type_init(qd3dpt_register_type)
