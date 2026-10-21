/*
 * Android display backend (-display android)
 *
 * For hosts that run QEMU in-process (system_shared_lib), such as the
 * ios-on-pixel app: the default graphic console is drawn straight into an
 * ANativeWindow (a Surface) that the host hands over at runtime, and touch
 * and key input arrive through QEMU's input layer, so no VNC round trip is
 * involved. The host drives it through the qemu_android_* functions below,
 * which are exported from libqemu-system-<target>-shared.so.
 *
 * Threading: the host calls in from its own threads. The window is guarded
 * by win_lock (a plain pthread mutex, usable before qemu_init runs); input
 * is queued and delivered by a bottom half on the main loop, which holds the
 * BQL. Drawing happens in dpy_refresh, on the main loop.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-types-ui.h"
#include "qapi/util.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "ui/android.h"
#include "ui/console.h"
#include "ui/input.h"
#include "qemu/timer.h"
#include "trace.h"

#include <android/native_window.h>
#include <pthread.h>

#define QEMU_ANDROID_EXPORT __attribute__((visibility("default")))

typedef struct AndroidInput
{
    bool key;
    int  qcode;
    int  x, y, w, h;
    bool down;
} AndroidInput;

static DisplayChangeListener dcl;
static DisplaySurface*       surface;
/* Dirty rectangle since the last post, [x0, x1) x [y0, y1); main loop only. */
static int                   dirty_x0, dirty_y0, dirty_x1, dirty_y1;
static int64_t               dirty_since_ns; /* when the dirty rectangle became non-empty */
static bool                  ready;
static bool                  touch_down;

static pthread_mutex_t win_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow*  win;
static int             win_w, win_h; /* buffer geometry set on win, 0 = not yet */
static bool            win_full;     /* the next post must redraw everything */
static void (*resize_cb)(int w, int h);

static QEMUBH*         input_bh;
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static GQueue          input_queue = G_QUEUE_INIT;

static void android_dirty_all(void)
{
    dirty_since_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    dirty_x0 = 0;
    dirty_y0 = 0;
    dirty_x1 = surface ? surface_width(surface) : 0;
    dirty_y1 = surface ? surface_height(surface) : 0;
}

static void android_gfx_update(DisplayChangeListener* l, int x, int y, int w, int h)
{
    if (dirty_x1 <= dirty_x0 || dirty_y1 <= dirty_y0) {
        dirty_since_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        dirty_x0 = x;
        dirty_y0 = y;
        dirty_x1 = x + w;
        dirty_y1 = y + h;
        return;
    }
    dirty_x0 = MIN(dirty_x0, x);
    dirty_y0 = MIN(dirty_y0, y);
    dirty_x1 = MAX(dirty_x1, x + w);
    dirty_y1 = MAX(dirty_y1, y + h);
}

static void android_gfx_switch(DisplayChangeListener* l, DisplaySurface* new_surface)
{
    bool resized =
        !surface || !new_surface || surface_width(surface) != surface_width(new_surface) ||
        surface_height(surface) != surface_height(new_surface);
    void (*cb)(int, int);

    surface = new_surface;
    android_dirty_all();

    pthread_mutex_lock(&win_lock);
    win_full = true;
    cb       = resize_cb;
    pthread_mutex_unlock(&win_lock);

    if (resized && cb && surface) { cb(surface_width(surface), surface_height(surface)); }
}

static bool android_gfx_check_format(DisplayChangeListener* l, pixman_format_code_t format)
{
    /* Anything pixman can read; the copy into the window converts. */
    return true;
}

/* Copies the dirty part of the console surface into the window and posts it. */
static void android_post(void)
{
    ANativeWindow_Buffer buf;
    pixman_image_t*      dst;
    ARect                r;
    int                  w, h;
    int64_t              start;

    if (!win || !surface) { return; }
    w = surface_width(surface);
    h = surface_height(surface);
    if (win_w != w || win_h != h) {
        if (ANativeWindow_setBuffersGeometry(win, w, h, WINDOW_FORMAT_RGBX_8888) != 0) { return; }
        win_w    = w;
        win_h    = h;
        win_full = true;
    }
    if (win_full) {
        r = (ARect) { 0, 0, w, h };
    }
    else {
        if (dirty_x1 <= dirty_x0 || dirty_y1 <= dirty_y0) { return; }
        r = (ARect) { dirty_x0, dirty_y0, MIN(dirty_x1, w), MIN(dirty_y1, h) };
    }

    /* lock() may grow r to what this buffer needs redrawn; the rest is copied back. */
    start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (ANativeWindow_lock(win, &buf, &r) != 0) { return; }
    r.right  = MIN(r.right, MIN(buf.width, w));
    r.bottom = MIN(r.bottom, MIN(buf.height, h));
    /* WINDOW_FORMAT_RGBX_8888 is R, G, B, X in memory: pixman's x8b8g8r8. */
    dst = pixman_image_create_bits(PIXMAN_x8b8g8r8, buf.width, buf.height, buf.bits, buf.stride * 4);
    if (r.right > r.left && r.bottom > r.top) {
        pixman_image_composite(PIXMAN_OP_SRC, surface->image, NULL, dst, r.left, r.top, 0, 0, r.left, r.top,
                               r.right - r.left, r.bottom - r.top);
    }
    pixman_image_unref(dst);
    ANativeWindow_unlockAndPost(win);
    /* How long the post took, and how long the oldest change waited for it. */
    trace_android_display_post(r.left, r.top, r.right - r.left, r.bottom - r.top,
                               qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - start,
                               win_full ? 0 : start - dirty_since_ns);

    win_full = false;
    dirty_x0 = dirty_y0 = dirty_x1 = dirty_y1 = 0;
}

static void android_refresh(DisplayChangeListener* l)
{
    graphic_hw_update(l->con);
    pthread_mutex_lock(&win_lock);
    android_post();
    pthread_mutex_unlock(&win_lock);
}

static const DisplayChangeListenerOps android_dcl_ops = {
    .dpy_name             = "android",
    .dpy_refresh          = android_refresh,
    .dpy_gfx_update       = android_gfx_update,
    .dpy_gfx_switch       = android_gfx_switch,
    .dpy_gfx_check_format = android_gfx_check_format,
};

static void android_input_bh(void* opaque)
{
    QemuConsole*  con = dcl.con;
    AndroidInput* ev;
    bool          sync = false;

    for (;;) {
        pthread_mutex_lock(&input_lock);
        ev = g_queue_pop_head(&input_queue);
        pthread_mutex_unlock(&input_lock);
        if (!ev) { break; }
        if (ev->key) {
            qemu_input_event_send_key_qcode(con, ev->qcode, ev->down);
        }
        else {
            /* Same events as a VNC absolute pointer: Inferno's touch device reads them. */
            qemu_input_queue_abs(con, INPUT_AXIS_X, ev->x, 0, ev->w);
            qemu_input_queue_abs(con, INPUT_AXIS_Y, ev->y, 0, ev->h);
            if (ev->down != touch_down) {
                qemu_input_queue_btn(con, INPUT_BUTTON_LEFT, ev->down);
                touch_down = ev->down;
            }
            sync = true;
        }
        g_free(ev);
    }
    if (sync) { qemu_input_event_sync(); }
}

static void android_queue_input(AndroidInput* ev)
{
    if (!qatomic_read(&ready)) {
        g_free(ev);
        return;
    }
    pthread_mutex_lock(&input_lock);
    g_queue_push_tail(&input_queue, ev);
    pthread_mutex_unlock(&input_lock);
    qemu_bh_schedule(input_bh);
}

static void android_display_init(DisplayState* ds, DisplayOptions* opts)
{
    dcl.ops = &android_dcl_ops;
    dcl.con = qemu_console_lookup_default();
    if (!dcl.con) {
        error_report("-display android: no graphic console");
        exit(1);
    }
    input_bh = qemu_bh_new(android_input_bh, NULL);
    register_displaychangelistener(&dcl);
    qatomic_set(&ready, true);
}

static QemuDisplay qemu_display_android = {
    .type = DISPLAY_TYPE_ANDROID,
    .init = android_display_init,
};

static void register_android(void) { qemu_display_register(&qemu_display_android); }

type_init(register_android);

/* ---- host API, exported from the shared library ---- */

/*
 * Draw into @window from now on (it is acquired), or stop drawing if NULL.
 * When this returns the previous window is no longer used, so the host may
 * let its Surface go. Callable before QEMU is initialised.
 */
QEMU_ANDROID_EXPORT void qemu_android_display_set_window(ANativeWindow* window)
{
    pthread_mutex_lock(&win_lock);
    if (win) { ANativeWindow_release(win); }
    win = window;
    if (win) { ANativeWindow_acquire(win); }
    win_w    = 0;
    win_h    = 0;
    win_full = true;
    pthread_mutex_unlock(&win_lock);
}

/* @cb(width, height) is called on QEMU's main loop when the guest framebuffer size changes. */
QEMU_ANDROID_EXPORT void qemu_android_display_set_resize_cb(void (*cb)(int w, int h))
{
    pthread_mutex_lock(&win_lock);
    resize_cb = cb;
    pthread_mutex_unlock(&win_lock);
}

/* A finger at (@x, @y) in a @w x @h area covering the framebuffer; @down = touching. */
QEMU_ANDROID_EXPORT void qemu_android_input_touch(int x, int y, int w, int h, int down)
{
    AndroidInput* ev = g_new0(AndroidInput, 1);

    ev->x    = x;
    ev->y    = y;
    ev->w    = w;
    ev->h    = h;
    ev->down = down;
    android_queue_input(ev);
}

/* Press or release the key named @name (QKeyCode names, as for sendkey, e.g. "f5"). */
QEMU_ANDROID_EXPORT int qemu_android_input_key(const char* name, int down)
{
    int           qcode = qapi_enum_parse(&QKeyCode_lookup, name, -1, NULL);
    AndroidInput* ev;

    if (qcode < 0) { return -1; }
    ev        = g_new0(AndroidInput, 1);
    ev->key   = true;
    ev->qcode = qcode;
    ev->down  = down;
    android_queue_input(ev);
    return 0;
}
