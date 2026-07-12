#ifndef NDS_GESTURE_H
#define NDS_GESTURE_H

// Classification of the most recent finished input gesture (see gesture.cc).
enum nds_gesture {
    NDS_GESTURE_NONE = 0,
    NDS_GESTURE_TAP,
    NDS_GESTURE_SWIPE,
    NDS_GESTURE_BUTTON,
};

// Human-readable name for a gesture, for logging.
__attribute__((unused)) static inline const char *nds_gesture_name(enum nds_gesture g) {
    switch (g) {
        case NDS_GESTURE_TAP:    return "TAP";
        case NDS_GESTURE_SWIPE:  return "SWIPE";
        case NDS_GESTURE_BUTTON: return "BUTTON";
        default:                 return "NONE";
    }
}

// Install the app-wide Qt event filter. Idempotent, and a no-op (returning false) until a
// QCoreApplication exists and the caller is on its thread — call it from UI-thread hooks.
bool nds_gesture_filter_install(void);
bool nds_gesture_filter_installed(void);

// The most recent finished gesture, its time() stamp, and — for a TAP — the tap x position
// (widget-local; -1 otherwise). Safe to call from any thread.
enum nds_gesture nds_gesture_last(long *ts_out, int *tap_x_out);

// A monotonic counter bumped once per finished gesture (tap/swipe/button). Lets the sweep logic
// tell "the flash and the render came from the same input" (book open = flash+content pair) from
// "a later, separate turn". Safe to call from any thread.
long nds_gesture_seq(void);

// 1 while the reader (ReadingView) is on screen, i.e. a book is open; 0 on the home/library. The
// filter tracks it from the ReadingView widget's Show/Hide events. Safe to call from any thread.
int nds_reader_is_open(void);

#endif
