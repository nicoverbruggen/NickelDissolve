#ifndef NDS_GESTURE_H
#define NDS_GESTURE_H

// Classification of the most recent finished input gesture (see gesture.cc).
enum nds_gesture {
    NDS_GESTURE_NONE = 0,
    NDS_GESTURE_TAP,
    NDS_GESTURE_SWIPE,
    NDS_GESTURE_BUTTON,
};

// Install the app-wide Qt event filter. Idempotent, and a no-op (returning false) until a
// QCoreApplication exists and the caller is on its thread — call it from UI-thread hooks.
bool nds_gesture_filter_install(void);
bool nds_gesture_filter_installed(void);

// The most recent finished gesture, its time() stamp, and — for a TAP — the tap x position
// (widget-local; -1 otherwise). Safe to call from any thread.
enum nds_gesture nds_gesture_last(long *ts_out, int *tap_x_out);

#endif
