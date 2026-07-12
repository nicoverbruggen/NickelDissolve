// Shared declarations for the Reading-settings "Page turn animations" row (settingsui.cc).
// nickeldissolve.cc includes this to reference the hook function and the resolved symbol pointers
// in its NickelHook tables, without pulling in Qt: a forward-declared QWidget is enough for the
// pointer types (they're only ever address-of'd here, never called or dereferenced).
//
// The Reading-settings page (N3SettingsReadingView) is a hand-built Qt Designer form. Reversing it
// shows each row is a QWidget wrapping a QHBoxLayout of a QLabel (the left "Dark Mode:" label) and a
// TouchCheckBox (the "On" checkbox), followed by a QFrame divider. We build the exact same widgets
// and copy their look from the live Dark Mode row, so ours is visually a first-class Kobo setting.
#ifndef NDS_SETTINGSUI_H
#define NDS_SETTINGSUI_H

class QWidget;   // forward declaration — sufficient for the pointer declaration below

// N3SettingsReadingView constructor (hooked) — original saved here.
extern void (*real_settings_ctor)(void *self, void *parent);

// TouchCheckBox(QWidget*) — the private Kobo checkbox class the native rows use. It derives from
// QCheckBox, so once constructed we drive it through the public QAbstractButton/QCheckBox API
// (setText, setChecked, toggled(bool)); this is the only settings symbol we need to resolve.
extern void (*nds_touchcheckbox_ctor)(void *self, void *parent);

// The N3SettingsReadingView constructor hook (inserts our row).
extern "C" void _nds_settings_ctor(void *self, void *parent);

#endif
