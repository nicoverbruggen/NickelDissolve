// settingsui.cc — inserts a "Page turn animations" row into the Reading settings page.
//
// The Reading settings page (N3SettingsReadingView) is a hand-built Qt Designer form. Reversing its
// setupUi shows there is no reusable "setting row" widget here: each row is assembled by hand as a
// QWidget wrapping a QHBoxLayout of a QLabel (the left label, e.g. "Dark Mode:") and a TouchCheckBox
// (the "On" checkbox), with a QFrame divider below it. TouchCheckBox derives from QCheckBox, so once
// we construct one we can drive it entirely through the public QAbstractButton/QCheckBox API.
//
// We hook the view's constructor, let it build the native rows, then build the same three widgets and
// copy their look (font, padding, indent, alignment, dynamic style properties, divider) straight off an
// existing native row — so ours matches whatever the firmware's theme happens to be instead of hard-
// coding a style. The row is inserted into the Page Appearance section, right after an anchor row (see
// NDS_ANCHOR_ROWS: "Show Adobe EPUB page numbers" first, since it exists on every 4.x firmware, with
// Dark Mode / Reduce rainbow as fallbacks). Toggling it flips the mod's runtime enable flag immediately
// and persists nds_mode to the config file.
//
// Everything fails closed: the only hard requirements are one known anchor row (as insertion point and
// style template) and the TouchCheckBox constructor. On any firmware where neither is found, the native
// page is left exactly as Kobo built it and no row is added.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

#include <QBoxLayout>
#include <QByteArray>
#include <QCheckBox>
#include <QEvent>
#include <QFont>
#include <QFontInfo>
#include <QFrame>
#include <QList>
#include <QMargins>
#include <QHBoxLayout>
#include <QLabel>
#include <QObject>
#include <QString>
#include <QStyle>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>

#include "config.h"
#include "ndsbridge.h"
#include "settingsui.h"
#include "util.h"

// ---- symbols resolved by NickelHook (defined here, listed in the tables in nickeldissolve.cc) ----
void (*real_settings_ctor)(void *self, void *parent)   = nullptr;  // N3SettingsReadingView ctor (hooked)
void (*nds_touchcheckbox_ctor)(void *self, void *parent) = nullptr; // TouchCheckBox(QWidget*) ctor

// From nickeldissolve.cc: the live enable flag and the current effective state.
extern "C" volatile int nds_runtime_animate;           // -1 = follow config, 0 = off, 1 = on
extern "C" int nds_animations_enabled(void);           // current effective state (config or runtime)
extern "C" int nds_device_supported(void);             // 1 = modern (hwtcon) device, officially supported

static NdsBridge *g_nds_bridge = nullptr;

// The native left label is a RegularLabelVPadded (a QLabel subclass with no public constructor we can
// call), whose content margins — the 25px left indent and top/bottom vertical padding — are applied by
// the style only at polish/show time, not at construction. So we build a plain QLabel and, the first
// time the page is shown, copy the native label's now-resolved margins/font/indent/alignment onto it.
// This runs on Show (before the first Paint), so the row is painted correct with no visible reflow. A
// no-Q_OBJECT QObject is enough here: we only override the inherited eventFilter virtual.
class NdsRowSync : public QObject {
public:
    QLabel *mine; QWidget *ref; bool secondary;
    NdsRowSync(QWidget *parent, QLabel *m, QWidget *r, bool sec = false)
        : QObject(parent), mine(m), ref(r), secondary(sec) {}
    bool eventFilter(QObject *, QEvent *e) override {
        if (e->type() == QEvent::Show && mine && ref) {
            ref->ensurePolished();                       // make sure the native label is styled first
            const QMargins m = ref->contentsMargins();   // the resolved (25,25,0,25) indent + padding
            const QFontInfo fi(ref->font());             // the resolved font (family + pixel size)
            // Bake the padding AND font into our own stylesheet. Setting contentsMargins directly does
            // not survive a later re-polish (a plain QLabel matches no QSS padding rule and resets to
            // 0), whereas the widget's own stylesheet does. The font must be included because assigning
            // a stylesheet otherwise resets the widget font to the large default.
            if (!secondary) {
                // Main row label: the extra top/bottom padding compensates for the taller cell
                // RegularLabelVPadded reserves that a plain QLabel does not, so our row ends up the
                // same height as its neighbours (eyeballed on device).
                const int kExtraVPad = 5;
                mine->setStyleSheet(QString("padding: %1px %2px %3px %4px; font-family: \"%5\"; font-size: %6px;")
                                        .arg(m.top() + kExtraVPad).arg(m.right())
                                        .arg(m.bottom() + kExtraVPad).arg(m.left())
                                        .arg(fi.family()).arg(fi.pixelSize()));
            } else {
                // Secondary description line: same left indent, a smaller font and tight top padding so
                // it reads as a caption tucked under the row rather than a second full-height row.
                const int fs = fi.pixelSize() > 0 ? (fi.pixelSize() * 82) / 100 : fi.pixelSize();
                mine->setStyleSheet(QString("padding: 0px %1px %2px %3px; font-family: \"%4\"; font-size: %5px;")
                                        .arg(m.right()).arg(m.bottom()).arg(m.left())
                                        .arg(fi.family()).arg(fs));
            }
            if (const QLabel *rl = qobject_cast<const QLabel *>(ref)) {
                mine->setAlignment(rl->alignment());
                mine->setIndent(rl->indent());   // native uses 0; a QLabel's -1 default adds a stray
            }                                    // font-metric indent once a stylesheet is applied
            mine->removeEventFilter(this);
            deleteLater();
        }
        return false;
    }
};

// TouchCheckBox is a private Kobo class with no public factory, so we provide its storage. Over-
// allocate a zeroed block so a larger layout on a future firmware writes inside our buffer instead of
// the heap; Qt frees it later via the global operator delete (its own chunk size), matching the
// pointer we allocate here.
static void *nds_alloc_touchcheckbox(void) {
    const size_t ALLOC = 4096;
    void *p = ::operator new(ALLOC, std::nothrow);
    if (p) memset(p, 0, ALLOC);
    return p;
}

// Copy the theme-driven look shared by every widget: dynamic properties, size policy, stylesheet,
// size hints. Kobo styles these rows through the app-wide QSS, which selects on dynamic properties
// (notably "class"); copying them is what makes the same QSS rules apply to our widgets, so they pick
// up the theme's font and borders when the page is polished. We deliberately do NOT copy font() here:
// at constructor time (before the page is shown) the template's font is the unstyled default, not the
// QSS one, so freezing it would fight the stylesheet. Letting QSS set the font keeps us theme-correct.
static void nds_copy_common_style(QWidget *dst, const QWidget *src) {
    if (!dst || !src) return;
    const QList<QByteArray> props = src->dynamicPropertyNames();
    for (const QByteArray &name : props)
        dst->setProperty(name.constData(), src->property(name.constData()));
    dst->setSizePolicy(src->sizePolicy());
    dst->setMinimumSize(src->minimumSize());
    dst->setMaximumSize(src->maximumSize());
    dst->setContentsMargins(src->contentsMargins());
    const QString ss = src->styleSheet();
    if (!ss.isEmpty()) dst->setStyleSheet(ss);
}

// Persist the master on/off to the config file's nds_mode key, preserving every other key. Read the
// existing lines, drop any nds_mode line, append the new one, write to a sibling and rename.
static void nds_settings_persist_mode(bool on) {
    mkdir(NDS_CONFIG_DIR, 0755);

    // Slurp existing config lines, skipping any that set nds_mode.
    char *kept = nullptr;
    size_t kept_len = 0, kept_cap = 0;
    FILE *src = fopen(NDS_CONFIG_DIR "/config", "r");
    if (src) {
        char *line = nullptr; size_t lsz = 0; ssize_t n;
        while ((n = getline(&line, &lsz, src)) != -1) {
            const char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (!strncmp(p, "nds_mode", 8) && (p[8] == ':' || p[8] == ' ' || p[8] == '\t'))
                continue;                                   // drop the old mode line
            if (kept_len + (size_t)n + 1 > kept_cap) {
                kept_cap = (kept_len + (size_t)n + 1) * 2 + 256;
                char *grow = (char *)realloc(kept, kept_cap);
                if (!grow) { free(kept); kept = nullptr; break; }
                kept = grow;
            }
            if (kept) { memcpy(kept + kept_len, line, (size_t)n); kept_len += (size_t)n; kept[kept_len] = '\0'; }
        }
        free(line);
        fclose(src);
    }

    char tmp[1024];
    int tn = snprintf(tmp, sizeof(tmp), NDS_CONFIG_DIR "/config.tmp.%ld", (long)getpid());
    if (tn < 0 || (size_t)tn >= sizeof(tmp)) { free(kept); return; }
    FILE *dst = fopen(tmp, "w");
    if (!dst) { free(kept); return; }
    if (kept && kept_len) {
        fwrite(kept, 1, kept_len, dst);
        if (kept_len && kept[kept_len - 1] != '\n') fputc('\n', dst);
    }
    fprintf(dst, "nds_mode:%s\n", on ? "sweep" : "off");
    bool ok = fflush(dst) == 0 && fsync(fileno(dst)) == 0;
    if (fclose(dst) != 0) ok = false;
    free(kept);
    if (ok) rename(tmp, NDS_CONFIG_DIR "/config"); else unlink(tmp);
}

void NdsBridge::onAnimationToggled(bool on) {
    nds_runtime_animate = on ? 1 : 0;     // takes effect on the very next page turn, no reboot
    nds_settings_persist_mode(on);        // and survives a reboot
    NDS_LOG("settings: page-turn animations toggled %s", on ? "on" : "off");
}

// Build a divider that matches the native row separators. Each native separator is a plain QWidget
// drawn as a line by the *view's* stylesheet, which lists the separators by objectName ID selector
// (#darkModeSeparator, #showAdobePageNumbersSeparator, ...) and gives them a fixed height + background.
// So we make a QWidget and give it the native separator's objectName: the same ID rule then matches
// ours and draws the identical line (a duplicate objectName is harmless — the view is already built
// and never looks it up again). On firmware with no separator widget to copy, fall back to a thin
// QFrame HLine so the row is still visually closed off. Returns nullptr only if allocation fails.
static QWidget *nds_make_separator(QWidget *parent, const QWidget *tmpl) {
    if (tmpl) {
        QWidget *sep = new (std::nothrow) QWidget(parent);
        if (!sep) return nullptr;
        sep->setObjectName(tmpl->objectName());    // #<sep> ID rule in the view QSS draws the line
        nds_copy_common_style(sep, tmpl);
        int h = tmpl->minimumHeight() > 0 ? tmpl->minimumHeight()
              : (tmpl->maximumHeight() < QWIDGETSIZE_MAX ? tmpl->maximumHeight() : 0);
        if (h > 0) sep->setFixedHeight(h);
        return sep;
    }
    QFrame *line = new (std::nothrow) QFrame(parent);
    if (!line) return nullptr;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setFixedHeight(1);
    return line;
}

// Candidate anchor rows in the Reading-settings "Page Appearance" section, most-universal first. Each
// is an existing checkbox row we copy the look from (label + checkbox + divider) and insert after.
// Rather than depend on Dark Mode — whose separator is absent on some firmware, and which the colour-
// only "Reduce rainbow" row mirrors — we prefer "Show Adobe EPUB page numbers", present on every 4.x
// firmware checked. The others are fallbacks in case a future build renames the Adobe row.
struct NdsAnchorRow { const char *label; const char *toggle; const char *separator; };
static const NdsAnchorRow NDS_ANCHOR_ROWS[] = {
    { "labelAdobePageNumbers", "showAdobePageNumbersToggle", "showAdobePageNumbersSeparator" },
    { "darkModeLabel",         "darkModeToggle",             "darkModeSeparator" },
    { "reduceRainbowLabel",    "reduceRainbowToggle",        "reduceRainbowSeparator" },
};

// N3SettingsReadingView constructor hook: after the native page is built, assemble our row from the
// same widget types the form uses and insert it into the Page Appearance section, right after an
// existing checkbox row. Every step fails closed, so the native page is untouched on any firmware we
// can't fit into.
extern "C" __attribute__((visibility("default")))
void _nds_settings_ctor(void *self, void *parent) {
    if (real_settings_ctor) real_settings_ctor(self, parent);   // build the native rows first
    if (!self || !nds_touchcheckbox_ctor) return;
    QWidget *view = static_cast<QWidget *>(self);

    // Pick the first anchor row that is fully present: we need its label and checkbox (style templates)
    // and its separator (style template + the insertion anchor — our row goes right after it).
    QLabel  *tmplLabel = nullptr;
    QWidget *tmplTgl = nullptr, *tmplSep = nullptr;
    const char *anchorName = nullptr;
    for (const NdsAnchorRow &a : NDS_ANCHOR_ROWS) {
        QLabel  *l  = view->findChild<QLabel  *>(QLatin1String(a.label));
        QWidget *tg = view->findChild<QWidget *>(QLatin1String(a.toggle));
        QWidget *sp = view->findChild<QWidget *>(QLatin1String(a.separator));
        if (l && tg && sp) { tmplLabel = l; tmplTgl = tg; tmplSep = sp; anchorName = a.label; break; }
    }
    if (!tmplSep) { NDS_LOG("settings: no known Page Appearance row found; row not added"); return; }

    // The separator sits directly in the section's box layout; insert our row immediately after it.
    QWidget *section = tmplSep->parentWidget();
    QBoxLayout *sectionLayout = section ? qobject_cast<QBoxLayout *>(section->layout()) : nullptr;
    if (!sectionLayout) { NDS_LOG("settings: section has no box layout; row not added"); return; }
    const int sepIdx = sectionLayout->indexOf(tmplSep);
    if (sepIdx < 0) { NDS_LOG("settings: anchor separator not in section layout; row not added"); return; }

    // Our row: a container holding the label + checkbox line and a divider below, matching the natives.
    // The section lays out each native row's content and its divider as two separate items with the
    // layout's spacing between them; since we wrap both in one container, we reproduce that same gap as
    // our inner spacing, otherwise our cell ends up shorter than the native rows by exactly one gap.
    int vspace = sectionLayout->spacing();
    if (vspace < 0)
        vspace = section->style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing, nullptr, section);
    QWidget *row = new (std::nothrow) QWidget(section);
    if (!row) return;
    QVBoxLayout *outer = new QVBoxLayout(row);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(vspace > 0 ? vspace : 0);

    // Left label expands; the checkbox sits at the right like the natives.
    QHBoxLayout *line = new QHBoxLayout();
    line->setContentsMargins(0, 0, 0, 0);

    QLabel *label = new (std::nothrow) QLabel(row);
    if (!label) { row->deleteLater(); return; }
    nds_copy_common_style(label, tmplLabel);
    label->setWordWrap(tmplLabel->wordWrap());
    label->setTextFormat(tmplLabel->textFormat());
    // The native label's margins/indent/font are resolved by the style only at show time; the
    // NdsRowSync installed below copies those onto this QLabel on the first Show.
    label->setText(QStringLiteral("Page turn animations:"));   // trailing colon like the native rows

    // Only the modern (hwtcon) devices are officially supported. On those we build the native "On"
    // checkbox; on the others (i.MX/sunxi) we put an "Unsupported" label where the checkbox would be.
    // The animation code still runs if forced from the config file, but the GUI doesn't offer a toggle
    // where it isn't supported. Either way an explanatory caption is added below (see desc).
    const bool supported = nds_device_supported() != 0;

    QWidget *rightWidget = nullptr;
    if (supported) {
        void *cbMem = nds_alloc_touchcheckbox();
        if (!cbMem) { row->deleteLater(); return; }
        nds_touchcheckbox_ctor(cbMem, row);
        // TouchCheckBox derives from QCheckBox with the QCheckBox subobject at offset 0, so we can treat
        // the storage as a QCheckBox and use the public API for the text, state and toggled(bool) signal.
        QCheckBox *checkbox = reinterpret_cast<QCheckBox *>(cbMem);
        nds_copy_common_style(checkbox, tmplTgl);
        checkbox->setText(QStringLiteral("On"));
        checkbox->setChecked(nds_animations_enabled() != 0);

        if (!g_nds_bridge) g_nds_bridge = new NdsBridge(nullptr);
        // Fail closed: a visible-but-dead row is worse than none, so tear it all down if it won't connect.
        if (!QObject::connect(checkbox, SIGNAL(toggled(bool)), g_nds_bridge, SLOT(onAnimationToggled(bool)))) {
            NDS_LOG("settings: toggled(bool) did not connect; omitting the row");
            row->deleteLater();
            return;
        }
        rightWidget = checkbox;
    } else {
        QLabel *unsup = new (std::nothrow) QLabel(row);
        if (!unsup) { row->deleteLater(); return; }
        nds_copy_common_style(unsup, tmplLabel);   // theme font, so it matches the row
        unsup->setText(QStringLiteral("Unsupported"));
        rightWidget = unsup;
    }

    line->addWidget(label, 1);
    line->addWidget(rightWidget, 0, Qt::AlignRight | Qt::AlignVCenter);
    outer->addLayout(line);

    // Explanatory caption, ALWAYS shown, below the row and above the divider: says why the animation is
    // or isn't available on this hardware. Styled as a smaller sub-caption (NdsRowSync secondary mode).
    QLabel *desc = new (std::nothrow) QLabel(row);
    if (desc) {
        nds_copy_common_style(desc, tmplLabel);
        desc->setWordWrap(true);
        desc->setTextFormat(Qt::PlainText);
        desc->setText(supported
            ? QStringLiteral("Your device's hardware revision supports page turn animations.")
            : QStringLiteral("Your device's hardware revision is unfortunately too old, and page turn animations are not possible."));
        outer->addWidget(desc);
        desc->installEventFilter(new NdsRowSync(desc, desc, tmplLabel, /*secondary=*/true));
    }

    if (QWidget *sep = nds_make_separator(row, tmplSep))
        outer->addWidget(sep);

    sectionLayout->insertWidget(sepIdx + 1, row);

    // Sync the label's resolved padding/indent/font from the native anchor label on first Show (they
    // are applied by the style only then). The sync object parents itself to our label, so it dies with it.
    label->installEventFilter(new NdsRowSync(label, label, tmplLabel));

    NDS_LOG("settings: inserted 'Page turn animations' row after '%s' (supported=%d on=%d)",
            anchorName, supported, nds_animations_enabled() != 0);
}
