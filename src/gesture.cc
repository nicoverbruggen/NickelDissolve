// gesture.cc — app-wide input-gesture classification for per-gesture control.
//
// Nickel's tap page-turn path (ReadingView::nextPage/prevPage) is not PLT-hookable, so hooks
// alone can't tell gestures apart: swipes and physical page-turn buttons both arrive via the
// hookable *PageWithTimer pair, and a tap turn only shows up as an un-announced full-screen
// render. This installs a Qt event filter on the application object (every event passes through
// it) and classifies the most recent *finished* input:
//   - a key press                                    -> BUTTON (physical page-turn buttons)
//   - press->release that moved less than a threshold -> TAP (also records the tap x, so a tap
//     turn can take its sweep direction from the tap position)
//   - press->release that moved further               -> SWIPE
// Touch events and their synthesized mouse events are both handled (whichever Nickel delivers);
// they classify identically, so the last writer winning is harmless. The filter only observes —
// every event is passed through unchanged.
//
// The sweep decision in nickeldissolve.cc pairs this with the arming hooks: an armed render with
// a fresh BUTTON is a button turn, armed with anything else is a swipe, and an unarmed render
// with a fresh TAP is a tap turn.

#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QTouchEvent>
#include <QThread>
#include <time.h>

#include "gesture.h"
#include "util.h"

// A finger can wobble a few pixels during a tap; anything travelling further than this between
// press and release is a swipe. ~40 px is ≈3 mm on the ~300 DPI panels — comfortably above tap
// jitter, comfortably below any deliberate swipe.
#define NDS_SWIPE_DIST_PX 40

namespace {

// Written on the UI thread by the filter, read from the e-ink update path (same volatile
// convention as the turn state in nickeldissolve.cc).
volatile int  g_last  = NDS_GESTURE_NONE;
volatile long g_ts    = 0;
volatile int  g_tap_x = -1;

// Press-position bookkeeping (UI thread only).
int  g_press_x = 0, g_press_y = 0;
bool g_installed = false;

void finish_gesture(int x, int y) {
    const int dx = x - g_press_x, dy = y - g_press_y;
    const bool swipe = (dx * dx + dy * dy) > (NDS_SWIPE_DIST_PX * NDS_SWIPE_DIST_PX);
    g_tap_x = swipe ? -1 : x;
    g_last  = swipe ? NDS_GESTURE_SWIPE : NDS_GESTURE_TAP;
    g_ts    = (long)time(nullptr);
}

class NdsGestureFilter final : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        switch (ev->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick: {
            const QMouseEvent *me = static_cast<QMouseEvent *>(ev);
            g_press_x = me->x(); g_press_y = me->y();
            break;
        }
        case QEvent::MouseButtonRelease: {
            const QMouseEvent *me = static_cast<QMouseEvent *>(ev);
            finish_gesture(me->x(), me->y());
            break;
        }
        case QEvent::TouchBegin: {
            const QTouchEvent *te = static_cast<QTouchEvent *>(ev);
            if (!te->touchPoints().isEmpty()) {
                const QPointF p = te->touchPoints().first().pos();
                g_press_x = (int)p.x(); g_press_y = (int)p.y();
            }
            break;
        }
        case QEvent::TouchEnd: {
            const QTouchEvent *te = static_cast<QTouchEvent *>(ev);
            if (!te->touchPoints().isEmpty()) {
                const QPointF p = te->touchPoints().first().pos();
                finish_gesture((int)p.x(), (int)p.y());
            }
            break;
        }
        case QEvent::KeyPress:
            // Any physical key: on the reading screen that's a page-turn button. Other keys
            // (e.g. power) never lead to a page render, so misclassifying them is harmless.
            g_tap_x = -1;
            g_last  = NDS_GESTURE_BUTTON;
            g_ts    = (long)time(nullptr);
            break;
        default:
            break;
        }
        return QObject::eventFilter(obj, ev);   // observe only; never consume
    }
};

} // namespace

bool nds_gesture_filter_install(void) {
    if (g_installed) return true;
    QCoreApplication *app = QCoreApplication::instance();
    if (!app || QThread::currentThread() != app->thread()) return false;
    app->installEventFilter(new NdsGestureFilter(/* no parent; lives as long as the app */));
    g_installed = true;
    NDS_LOG("gesture: app-wide event filter installed");
    return true;
}

bool nds_gesture_filter_installed(void) {
    return g_installed;
}

enum nds_gesture nds_gesture_last(long *ts_out, int *tap_x_out) {
    if (ts_out) *ts_out = g_ts;
    if (tap_x_out) *tap_x_out = g_tap_x;
    return (enum nds_gesture)g_last;
}
