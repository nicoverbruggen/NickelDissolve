// NdsBridge — a moc'd QObject giving the "Page turn animations" settings toggle a real slot.
// The ToggleSwitch's toggled(bool) signal connects here; the slot body lives in settingsui.cc so
// it can reach the mod's runtime enable flag and config. Kept tiny on purpose.
#ifndef NDS_BRIDGE_H
#define NDS_BRIDGE_H

#include <QObject>

class NdsBridge : public QObject {
    Q_OBJECT
public:
    explicit NdsBridge(QObject *parent = nullptr) : QObject(parent) {}
public slots:
    void onAnimationToggled(bool on);
    void showUnsupportedAlert();
};

#endif
