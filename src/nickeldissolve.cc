// NickelDissolve: a Kobo-agnostic page-turn "band wipe".
//
// A Kindle page turn sweeps the new page in as a directional wipe (a native MediaTek hardware
// "swipe" the Kobo drivers don't expose). NickelDissolve approximates it from userspace: it takes
// the single full-screen e-ink update Nickel issues per page turn and replaces it with a SEQUENCE
// of partial updates over vertical strips, swept across the screen, so the new page appears
// strip-by-strip. Only the driver's public update ioctl is used, with no driver patching.
//
// PLATFORM-AGNOSTIC. Kobo has two e-ink update interfaces with the SAME update-struct prefix
// (region@0, waveform_mode@16, update_mode@20, update_marker@24; hwtcon was modelled on mxcfb):
//   * hwtcon (MTK), Clara BW/Colour, Libra Colour, Elipsa 2E: HWTCON_SEND_UPDATE 0x4024462E
//   * mxcfb (i.MX), Libra 2 & most pre-2024: MXCFB_SEND_UPDATE 0x4048462E
// We recognise either by its ioctl number and drive it by field offset, so no per-struct code.
//
// TRIGGER. Instead of gating on a specific waveform (MTK-only, and wrong for colour/CFA pages), we
// mark a turn "pending" from ReadingView::goToNextPage/goToPrevPage, the single sink every turn
// (tap, swipe, button) funnels into, so it also gives the true direction, and sweep the next
// full-screen update. We REUSE that update's own waveform, so whatever the device/page uses (GLR16,
// REAGL, a Kaleido CFA mode…) is preserved; the wipe is just that update, chopped into swept strips.
//
// NO HANG. Nickel does WAIT_FOR_UPDATE_COMPLETE(M) after the turn; the LAST strip reuses its marker
// M, so that wait resolves. If the last strip fails to submit, we resubmit the original update.
//
// SAFETY. Default mode is "sweep" (the wipe is on out of the box), with "observe" (pure passthrough +
// logging) and "off" (fully inert) as fallbacks. Only the hwtcon and mxcfb interfaces are handled; any
// other device (e.g. AllWinner/sunxi) is left untouched and reads as unsupported. In "sweep",
// only a full-screen update that immediately follows a page turn is ever touched; everything else
// passes byte-for-byte. Worst case of a bad sweep is a garbled frame fixed by the next refresh,
// recoverable, not a brick. The config file is optional; without one the defaults apply.

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <cmath>

#include <QGuiApplication>
#include <QScreen>
#include <QSize>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <NickelHook.h>

#include "config.h"
#include "driver_common.h"
#include "driver_hwtcon.h"
#include "driver_mxcfb.h"
#include "driver_sunxi.h"
#include "gesture.h"
#include "settingsui.h"
#include "util.h"

static const char *const NDS_LIBKOBO   = "/usr/local/Kobo/platforms/libkobo.so";
static const char *const NDS_LIBNICKEL = "/usr/local/Kobo/libnickel.so.1.0.0";

#define NDS_IOC_MAGIC(req) (((req) >> 8) & 0xFFU)
#define NDS_TURN_WINDOW_S  2          // a pending turn is stale after this many seconds

// Which driver, if any, owns this ioctl. Asked in turn rather than from one table, so each driver
// keeps its own hardware knowledge and nothing here needs to know what distinguishes them.
static const struct nds_platform *nds_match(unsigned long request) {
    const struct nds_platform *p = nds_hwtcon_match(request);
    return p ? p : nds_mxcfb_match(request);
}

// True when this platform is the MediaTek one. Used for the few core decisions that genuinely differ
// (which waveform table to name ids from, whether the CFA force-B&W switch applies); cfa_field is
// non-zero only on hwtcon.
static bool nds_is_hwtcon(const struct nds_platform *plat) { return plat && plat->cfa_field != 0u; }

// Waveform helpers dispatch to the owning driver: the two number their modes differently, so an id
// only means something once you know which interface produced it.
static bool nds_wf_sweepable(const struct nds_platform *plat, uint32_t wf) {
    return nds_is_hwtcon(plat) ? nds_hwtcon_wf_sweepable(wf) : nds_mxcfb_wf_sweepable(plat, wf);
}
static const char *nds_wf_name(uint32_t wf, const struct nds_platform *plat) {
    return nds_is_hwtcon(plat) ? nds_hwtcon_wf_name(wf) : nds_mxcfb_wf_name(wf);
}
static uint32_t nds_dither(const uint8_t *u, const struct nds_platform *plat) {
    return plat->dith_off ? nds_rd32(u, plat->dith_off) : 0u;
}

// Whether this platform may actually animate. An interface the mod only knows well enough to decode
// (sweep_proven == false) is recognised and logged but never swept, so a device we have no page-turn
// evidence for behaves exactly as it did before the mod was installed. There is deliberately NO config
// key to turn that on: in a released build such a device is simply not supported, and no reader should
// be able to talk themselves into an animation nobody has verified.
//
// The NDS_PRERELEASE developer build is where that changes: every recognised interface animates, best
// effort, and tracing is on from the first boot (see nds_verbose_compute). Finding out whether the wipe
// is viable on that hardware is the entire purpose of that build, so it tries rather than sits inert.
static bool nds_sweep_allowed(const struct nds_platform *plat) {
    if (!plat) return false;
#ifdef NDS_PRERELEASE
    return true;
#else
    return plat->sweep_proven;
#endif
}


static int  (*real_ioctl)(int fd, unsigned long request, void *argp) = nullptr;
static void (*real_goToNextPage)(void *self) = nullptr;
static void (*real_goToPrevPage)(void *self) = nullptr;

static bool nds_safety_disabled = false;
// Verbose tracing switch (declared in util.h, used by NDS_DBG + the hot-path traces). Published
// once by nds_init after the config is parsed; false on a healthy "sweep"/"off" device.
extern "C" { bool nds_verbose_enabled = false; }
static int  nds_turn_dir = 0;                 // 0 = forward (next), 1 = backward (prev)
static volatile int  nds_turn_pending = 0;    // a page turn just fired; sweep its next full update
static volatile long nds_turn_ts = 0;         // time() when the turn fired (staleness guard)
// Book-open / chapter-jump detection: Kobo draws these as a full-screen flash immediately
// followed by a full-screen content render, both from the SAME input gesture. A page turn is a
// single render from its own gesture. So we remember the gesture that produced the last full-screen
// flash; if the next full-screen content render is from that same gesture (within a short window),
// it's the settle after an open/jump and must not animate. A separate later turn has a new gesture
// and animates normally (this is why turn-1's own GC16 flash doesn't suppress turn-2).
static volatile long nds_flash_seq = -1;      // nds_gesture_seq() at the last full-screen flash (-1 = none pending)
static volatile long nds_flash_ts  = 0;       // time() of that flash (staleness bound)
#define NDS_SETTLE_WINDOW_S 2
static uint32_t nds_ephemeral_marker = 0xF0000000u;   // markers for non-final strips (never collide with Nickel)
static const struct nds_platform *nds_active = nullptr;   // detected from the first update ioctl (for logging)
// The AllWinner interface has no platform-table row (its update model does not fit one), so it is
// tracked separately. Set the first time a sunxi update is seen.
static bool nds_sunxi_active = false;

static long nds_now() { return (long)time(nullptr); }

static uint64_t nds_last_send_us = 0;        // last e-ink SEND the hook saw (turn, footer, clock, ...)
static uint64_t nds_turn_idle_any_us  = 0;   // gap before this update since the previous update of any kind

// ---- config -----------------------------------------------------------------------------
// The config file is OPTIONAL: no file (or a missing key) means the defaults below. User keys
// (nds_mode, nds_strips, nds_delay_us, nds_direction, nds_animate_on_*) are documented in the
// on-device doc; nds_debug_* keys are for debugging and documented in ABOUT.md only.
//
// nds_mode: "sweep" (DEFAULT, the wipe is on out of the box), "observe" (passthrough + log the
// ioctl stream), or "off" (fully inert, no logging). Anything else falls back to sweep.
static bool nds_mode_is(const char *m) { const char *v = nds_global_config_get("nds_mode"); return v && !strcasecmp(v, m); }
// Runtime enable flag driven by the Reading-settings toggle (settingsui.cc): -1 = follow the
// config's nds_mode, 0 = the toggle forced it off, 1 = forced on. Lets the toggle take effect on
// the next page turn without a reboot; the toggle also persists nds_mode to the config file.
extern "C" { volatile int nds_runtime_animate = -1; }
static bool nds_off()         { if (nds_runtime_animate == 0) return true; if (nds_runtime_animate == 1) return false; return nds_mode_is("off"); }
// Current effective on/off, for the toggle's initial state (settingsui.cc).
extern "C" int nds_animations_enabled(void) { return !nds_off(); }
// The device's support tier, for the settings UI. Detected from the e-ink driver seen at runtime
// (nds_active is set the first time a page renders through the hwtcon/mxcfb update ioctl, long before
// the Reading-settings panel can open):
//   2 = supported: the modern MTK/hwtcon family (Clara BW/Colour, Libra Colour); a plain toggle.
//   1 = may work: i.MX (mxcfb, Libra 2 / Clara 2E). The animation is best-effort and can look broken on
//                 some board revisions, so the toggle carries a "may not work, turn it off" note.
//   0 = not supported: a legacy i.MX interface the mod only decodes for logging, or an unrecognised
//                 or not-yet-detected platform. AllWinner reports tier 1, not 0.
// Keyed on the platform's own capability rather than its name, so adding a row to NDS_PLATFORMS gets
// the right tier without touching this.
extern "C" int nds_device_support(void) {
    // sunxi is driven by driver_sunxi.cc rather than the platform table, so it reports the same
    // best-effort tier an unofficial i.MX device does. The sweep is confirmed working on an Elipsa,
    // but on one device and one board revision, which is exactly what tier 1 describes.
    if (nds_sunxi_active) return 1;
    if (!nds_sweep_allowed(nds_active)) return 0;
    return nds_active->cfa_field ? 2 : 1;   // cfa_field is non-zero only on hwtcon
}
static bool nds_sweep_mode()  { return !nds_off() && !nds_mode_is("observe"); }
// Whether verbose per-ioctl / per-turn tracing should be on for this boot. Tied to the MODE so a
// supported device running the animation (sweep) stays quiet: "observe" exists to log the ioctl
// stream, "off" logs nothing, and "sweep" traces only when the user opts in with nds_log:1 (or
// the config had a problem, which force-enables it so mistakes self-diagnose). Computed once and
// published to nds_verbose_enabled by nds_init.
static bool nds_verbose_compute() {
    if (nds_off()) return false;
    if (nds_mode_is("observe")) return true;
#ifdef NDS_PRERELEASE
    return true;   // the diagnostics build always traces: collecting the log is its whole purpose
#else
    // In "sweep" mode, stay quiet unless the user opts in with nds_log:1, or the config had a
    // problem (force-enabled so mistakes self-diagnose).
    return nds_global_config_bool("nds_log", false) || nds_config_problem_seen();
#endif
}
// ---- per-device animation tuning ---------------------------------------------------------
// Two independent axes, chosen so same-size / same-panel devices tune identically with no model list
// to maintain. Config keys (nds_strips, nds_debug_strip_waveform) still win over both.
//   * Band count follows the panel's PHYSICAL width, so a band covers about the same distance on
//     every screen whatever its resolution (see nds_strips_for_width), capped at NDS_MAX_AUTO_STRIPS.
//   * The band waveform is glkw16 (a Kaleido B&W-optimised mode, crisper bands) on colour panels,
//     decided by Device::getCurrentDevice()->hasColorDisplay(); mono panels keep their reading wf.
static uint32_t nds_panel_max_dim = 0;   // longest panel edge seen so far (px); 0 until the first update
// Band count scales so a band covers a similar PHYSICAL width on every device, which is what makes the
// wipe read at the same pace regardless of screen size. The two tuned devices set the constant: a
// 6-inch Clara (1072px wide at 300dpi = 3.57in) gets 10 bands and a 7-inch Libra Colour (1264px at
// 300dpi = 4.21in) gets 12, which is ~0.354in of panel per band in both cases.
//
// Pixels are NOT a usable proxy for this. Both tuned devices are 300dpi, so pixel width and physical
// width agree there and the difference is invisible; it stops being invisible on a panel with another
// density. The Elipsa is 10.3in at ~227dpi, so its 1404px are 6.19in wide, physically wider than the
// Sage's 1440px, which are only 4.8in at 300dpi. Counting pixels would give the Sage MORE bands than
// the larger Elipsa and make its wipe crawl over a smaller screen.
#define NDS_BAND_MILS 354                // target physical band width, thousandths of an inch
#define NDS_BAND_PX   105                // pixel fallback, only used when the density is unknown
// Ceiling on the DERIVED band count. Not a size limit but a time one: a band does not cost the same on
// every interface (about 10ms on MediaTek, which pipelines them, against about 28ms on i.MX, which
// paces them with a delay), so past a point more bands only makes a turn slower without looking
// better. 12 is what the old panel-size rule topped out at, so this is a ceiling that was already
// there implicitly. An explicit nds_strips still reaches 32: this bounds the automatic answer only.
#define NDS_MAX_AUTO_STRIPS 12

// Panel density, from Qt, or 0 while it cannot be determined. Taken from the diagonal so it does not
// depend on the current rotation, and sanity-bounded because a wrong answer here would badly mis-tune
// the animation. Cached once it succeeds; the panel cannot change at runtime.
static int nds_panel_dpi() {
    static int cached = 0;
    if (cached) return cached;
    if (!QGuiApplication::instance()) return 0;        // too early to ask; try again on a later turn
    QScreen *s = QGuiApplication::primaryScreen();
    if (!s) return 0;
    const QSize  px = s->size();
    const QSizeF mm = s->physicalSize();
    if (px.width() < 512 || mm.width() < 1.0 || mm.height() < 1.0) return 0;
    const double pxdiag = sqrt((double)px.width() * px.width() + (double)px.height() * px.height());
    const double indiag = sqrt(mm.width() * mm.width() + mm.height() * mm.height()) / 25.4;
    if (indiag < 3.0) return 0;
    const int dpi = (int)(pxdiag / indiag + 0.5);
    if (dpi < 100 || dpi > 400) return 0;              // implausible: fall back to the pixel rule
    cached = dpi;
    return dpi;
}

// band_mils is the target physical band width. It is a parameter rather than a constant because a
// band does not cost the same everywhere: on the AllWinner controller each one is a full
// completion-waited refresh, so that driver asks for wider bands to keep a turn's total time sane.
static int nds_strips_for_width(uint32_t width, int band_mils) {
    if (width < 512) return 10;                        // no usable panel size yet: the old default
    if (band_mils < 50) band_mils = NDS_BAND_MILS;
    const int dpi = nds_panel_dpi();
    int n;
    if (dpi > 0) {
        const uint32_t mils = (uint32_t)(((uint64_t)width * 1000u) / (uint32_t)dpi);
        n = (int)((mils + (uint32_t)band_mils / 2) / (uint32_t)band_mils);
    } else {
        // No density: fall back to pixels, scaled by the same ratio so the two agree in spirit.
        const uint32_t px = (uint32_t)((long)NDS_BAND_PX * band_mils / NDS_BAND_MILS);
        n = (int)((width + px / 2) / (px ? px : NDS_BAND_PX));
    }
    if (n < 2) n = 2;
    if (n > NDS_MAX_AUTO_STRIPS) n = NDS_MAX_AUTO_STRIPS;
    return n;
}

// ---- sweep timing ------------------------------------------------------------------------
// Measured only, never fed back into the band count. Deriving the band count from a target duration
// was tried and pulled: it would have retuned the tested devices toward a target that was picked
// rather than measured, changing hardware that already behaves well. The measurement stays because it
// is what a real target would have to be calibrated FROM, starting with a device known to feel right.
static void nds_note_band_time(int bands, uint64_t took_us) {
    if (!nds_verbose_enabled || bands < 1 || took_us == 0) return;
    NDS_LOG("sweep timing: %d bands in %lums (%luus/band)", bands,
            (unsigned long)(took_us / 1000), (unsigned long)(took_us / (uint64_t)bands));
}

static int nds_default_strips() {
    // The flat interfaces only give us the longest edge. Kobo panels are close to 4:3, so width is
    // about three quarters of it; that approximation reproduces 10 and 12 for the two tuned devices.
    return nds_strips_for_width(nds_panel_max_dim * 3u / 4u, NDS_BAND_MILS);
}
// Device colour-panel query, via libnickel symbols resolved in NickelDissolveDlsym. Cached (the panel
// can't change at runtime). Defaults to mono (no glkw16) if the symbols are missing on an unfamiliar
// firmware, which is the safe choice since glkw16 is only correct on a Kaleido colour panel.
static void *(*real_getCurrentDevice)() = nullptr;
static bool  (*real_hasColorDisplay)(void *self) = nullptr;
static bool nds_panel_is_color() {
    static int cached = -1;                       // -1 = not resolved yet, 0 = mono, 1 = colour
    if (cached < 0 && real_getCurrentDevice && real_hasColorDisplay) {
        void *dev = real_getCurrentDevice();
        if (dev) cached = real_hasColorDisplay(dev) ? 1 : 0;
    }
    return cached == 1;
}
// Band waveform when nds_debug_strip_waveform is unset. On a colour Kaleido panel the crisp band mode
// is kernel EPD id 8 while the reader is in day mode (turn GLR16); in dark mode the turn is GLKW16 and
// id 8 ghosts, so the bands must use GLKW16 to match the dark reading waveform. cfa_field gates this to
// the hwtcon/Kaleido interface, since those ids name other modes on mxcfb. On mono hwtcon and i.MX the
// default is 0: reuse the turn's own waveform, which is already the right day/dark mode.
// Literal ids: the WF_* macros are defined further down; 8 = day band, 9 = GLKW16 dark band.
static uint32_t nds_default_strip_wf(const struct nds_platform *plat, uint32_t turn_wf) {
    if (plat->cfa_field && nds_panel_is_color())
        return (turn_wf == 9u) ? 9u : 8u;   // dark turn (GLKW16) gets the dark band; else the day band
    return plat->def_strip_wf;              // mono hwtcon / i.MX: 0 = reuse the turn's own waveform
}
static int  nds_strips()      { const char *v = nds_global_config_get("nds_strips"); int n = (v && *v) ? atoi(v) : nds_default_strips(); if (n < 2) n = 2; if (n > 32) n = 32; return n; }
// Cold-controller mitigation (default off): when the EPD has been idle long enough to power down,
// the next swept turn stutters on band 0 while the panel re-powers. If the idle since the last
// e-ink update exceeds this many ms, pass Nickel's own full refresh through for that one turn
// (clean, no stutter) instead of animating it. 0 = disabled (always sweep). Superseded by the
// prewarm sliver (nds_prewarm_ms) when that is set.
static int  nds_cold_skip_ms() { const char *v = nds_global_config_get("nds_cold_skip_ms"); int n = (v && *v) ? atoi(v) : 0; if (n < 0) n = 0; return n; }
// Cold-controller pre-warm (default off): the better cold handler. When the panel has been idle this
// many ms it has powered down, so the first update pays a ~40ms wake. Instead of skipping the
// animation (nds_cold_skip_ms), prepend a thin leading-edge sliver to the sweep: the sliver eats the
// wake on a hairline, then the real bands run warm and the wipe is smooth (just a touch late). When
// set, this supersedes nds_cold_skip_ms. 0 = disabled.
static int  nds_prewarm_ms()   { const char *v = nds_global_config_get("nds_prewarm_ms"); int n = (v && *v) ? atoi(v) : 0; if (n < 0) n = 0; return n; }
static bool nds_rtl()         { const char *d = nds_global_config_get("nds_direction"); return !(d && !strcasecmp(d, "ltr")); }
static bool nds_wait_complete(){ const char *w = nds_global_config_get("nds_debug_wait"); return w && !strcasecmp(w, "complete"); }
// Inter-strip delay. If nds_delay_us is set in config it always wins; if it's absent, fall back to
// the caller's per-platform default (hwtcon 0, paced by the submission-wait; mxcfb ~30 ms, no
// submission-wait, else the wipe is instant). Returns µs, clamped to [0, 50000].
static int  nds_delay_us(int def) { const char *v = nds_global_config_get("nds_delay_us"); int d = (v && *v) ? atoi(v) : def; if (d < 0) d = 0; if (d > 50000) d = 50000; return d; }
// Per-gesture control: which page-turn gestures animate. DEFAULT: all of them, always use the
// transition when possible. Every real next/prev turn arms via the goToNextPage/goToPrevPage
// hooks (taps, swipes, and physical buttons all funnel through them), which also record the true
// direction; the app-wide Qt event filter (gesture.cc) only supplies the input TYPE, so each
// turn's animation can be enabled or disabled per gesture.
static bool nds_animate_swipe()  { return nds_global_config_bool("nds_animate_on_swipe", true); }
static bool nds_animate_tap()    { return nds_global_config_bool("nds_animate_on_tap", true); }
static bool nds_animate_button() { return nds_global_config_bool("nds_animate_on_button", true); }
// Set HWTCON_FLAG_CFA_SKIP on the strips (Kaleido): skips the per-region CFA colour pass whose
// region boundaries produce the seams. Correct for B&W content (no colour to convert), and we
// only ever sweep B&W reading turns (see nds_wf_sweepable), so this is exactly right.
static bool nds_cfa_skip()    { return nds_global_config_bool("nds_debug_cfa_skip", true); }
// Force the WHOLE device to greyscale: OR HWTCON_FLAG_CFA_SKIP into every hwtcon update (menus,
// home, covers, reading), so the panel's colour pass is skipped everywhere. A debugging /
// full-B&W-mode switch, independent of the animation. No-op on mono/i.MX. See nds_force_bw use.
static bool nds_force_bw()    { return nds_global_config_bool("nds_debug_force_bw", false); }
// 0 = keep the turn's own waveform (platform-agnostic default); >0 = force a raw waveform id
// (PLATFORM-SPECIFIC: e.g. GLR16 is 4 on hwtcon but 6 on mxcfb; use with care).
static uint32_t nds_strip_wf() { const char *v = nds_global_config_get("nds_debug_strip_waveform"); if (!v || !*v) return 0; int w = atoi(v); return w > 0 ? (uint32_t)w : 0; }

// ---- per-gesture gating -------------------------------------------------------------------
// Whether the turn that just fired should animate, given its input type. The goToNextPage/
// goToPrevPage hooks already armed the turn and recorded the true direction; this only maps the
// last input the Qt filter saw (gesture.cc) to that gesture's on/off toggle. A center tap that
// merely raises the reading menu never reaches goToNextPage, so it never arms and never gets
// here; no tap-position or zone guessing is involved. If the filter isn't up yet, or the input
// is stale/unknown, treat it as a swipe (the common case) so the turn still animates by default.
static bool nds_gesture_animate() {
    long gts = 0; int tap_x = -1;
    enum nds_gesture g = nds_gesture_last(&gts, &tap_x);
    bool fresh = g != NDS_GESTURE_NONE && (nds_now() - gts) <= NDS_TURN_WINDOW_S;
    if (fresh && g == NDS_GESTURE_BUTTON) return nds_animate_button();
    if (fresh && g == NDS_GESTURE_TAP)    return nds_animate_tap();
    return nds_animate_swipe();
}

// ---- interface discovery ----------------------------------------------------------------
// The rest of the tracing only looks at requests whose magic is 'F', which is every interface the mod
// models. That filter hides the one platform it does not: the AllWinner sunxi devices (Sage, Elipsa)
// drive the panel through /dev/disp with DISP_EINK_* commands, which are plain command numbers with no
// 'F' magic, so on those devices the mod currently sees nothing at all.
//
// This logs the FIRST sighting of each distinct request number with no magic filter, which is enough to
// learn what a device actually calls without knowing anything about it in advance. The list is small and
// each number is logged once, so a reading session costs a handful of lines.
//
// The argument is deliberately NOT dereferenced. For a request the mod does not recognise there is
// nothing that says how large the argument is (a sunxi disp command carries no size in its number, the
// way an _IOC-encoded one does), so reading it could read past whatever the caller passed.
static void nds_log_new_request(unsigned long request) {
    static unsigned long seen[96];
    static unsigned nseen = 0;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == request) return;
    if (nseen >= sizeof(seen) / sizeof(seen[0])) return;
    seen[nseen++] = request;
    unsigned long magic = NDS_IOC_MAGIC(request);
    NDS_LOG("ioctl-scan: request=0x%08lx magic=0x%02lx(%c) nr=0x%02lx size=%lu dir=%lu",
            request, magic, (magic >= 32 && magic < 127) ? (char)magic : '.',
            request & 0xFFUL, (request >> 16) & 0x3FFFUL, (request >> 30) & 3UL);
}

// ---- ioctl hook -------------------------------------------------------------------------
extern "C" __attribute__((visibility("default")))
int _nds_ioctl(int fd, unsigned long request, void *argp) {
    const struct nds_platform *plat = nds_match(request);
    if (plat && argp) nds_active = plat;

    // Idle tracking: record the gap before every e-ink SEND since the previous update of any kind
    // (footer/clock keep the controller warm). This gates the cold-wake mitigations (prewarm sliver,
    // cold-skip). Our own sweep strips bypass this hook, so the caller re-anchors nds_last_send_us.
    if (plat && argp && request == plat->send) {
        uint64_t now_us = nds_now_us();
        nds_turn_idle_any_us = nds_last_send_us ? (now_us - nds_last_send_us) : 0;
        nds_last_send_us = now_us;
        // Learn the panel's longest edge from full-screen updates; it drives the band count.
        const uint8_t *u = (const uint8_t *)argp;
        uint32_t w = nds_rd32(u, OFF_WIDTH), h = nds_rd32(u, OFF_HEIGHT), md = w > h ? w : h;
        if (md >= 512 && md > nds_panel_max_dim) nds_panel_max_dim = md;
    }

    // Force-B&W: OR CFA_SKIP into every hwtcon update so the whole device renders greyscale.
    // Applies in every mode except "off" (it's not tied to the animation), in place, once.
    if (plat && argp && real_ioctl && !nds_off() && !nds_safety_disabled && plat->cfa_skip && nds_force_bw()) {
        uint8_t *m = (uint8_t *)argp;
        uint32_t f = nds_rd32(m, plat->flags_off);
        if (!(f & plat->cfa_skip)) nds_wr32(m, plat->flags_off, f | plat->cfa_skip);
    }

    // AllWinner (sunxi), handled by driver_sunxi.cc since its update model does not fit the platform
    // table. Best effort, like the current i.MX interface: the sweep is confirmed on an Elipsa but has
    // not been seen on a Sage or on another board revision. The gate is the armed turn plus a
    // full-screen rect, NOT the waveform, because every update on this hardware carries the same mode
    // whether it is a page turn, the status bar or a menu.
    if (request == NDS_SUNXI_UPDATE && argp) nds_sunxi_active = true;
    if (request == NDS_SUNXI_UPDATE && argp && real_ioctl && !nds_safety_disabled && nds_sweep_mode()) {
        if (nds_turn_pending && (nds_now() - nds_turn_ts) > NDS_TURN_WINDOW_S)
            nds_turn_pending = 0;                      // stale: never sweep an unrelated later render
        int32_t sw = 0, sh = 0;
        if (nds_turn_pending && nds_sunxi_page_rect(argp, &sw, &sh)) {
            const bool want_anim = nds_gesture_animate();   // per-gesture toggles apply here too
            nds_turn_pending = 0;                      // consume the trigger either way, one per turn
            if (!want_anim) {
                if (nds_verbose_enabled)
                    NDS_LOG("no-anim [sunxi] %dx%d -> this gesture's toggle is off, passthrough", sw, sh);
                return real_ioctl(fd, request, argp);
            }
            bool rtl = nds_rtl();
            if (nds_turn_dir == 1) rtl = !rtl;         // a backward turn sweeps the other way
            struct nds_sunxi_env env;
            env.real_ioctl = real_ioctl;
            // Config override first, exactly as the flat path does, so nds_strips tunes both.
            const char *sv = nds_global_config_get("nds_strips");
            int sbands = (sv && *sv) ? atoi(sv) : nds_strips_for_width((uint32_t)sw, NDS_SUNXI_BAND_MILS);
            if (sbands < 2)  sbands = 2;
            if (sbands > 32) sbands = 32;
            env.bands      = sbands;
            env.delay_us   = nds_delay_us(0);          // 0: the per-band wait already paces it
            env.rtl        = rtl;
            env.band_mode  = nds_strip_wf();           // 0 = the driver's flashless default
            int rc = -1;
            const uint64_t t0 = nds_now_us();
            if (nds_sunxi_sweep(fd, argp, &env, &rc)) {
                const uint64_t took = nds_now_us() - t0;
                nds_last_send_us = nds_now_us();       // the bands bypassed this hook; re-anchor idle
                nds_note_band_time(env.bands, took);

                return rc;
            }
        }
    }

    if (plat && argp && real_ioctl && !nds_safety_disabled && nds_sweep_mode() && nds_sweep_allowed(plat)) {
        // Expire a stale pending-turn so we never sweep an unrelated later full-screen update.
        if (nds_turn_pending && (nds_now() - nds_turn_ts) > NDS_TURN_WINDOW_S) nds_turn_pending = 0;

        const uint8_t *u = (const uint8_t *)argp;
        uint32_t w = nds_rd32(u, OFF_WIDTH), h = nds_rd32(u, OFF_HEIGHT);
        // Process a full-screen render when a real next/prev turn just armed it (goToNextPage/
        // goToPrevPage hook), or when a book is open (so book-open / chapter-jump flashes can be
        // tracked for the settle below). Menus, home and library are neither, so they never enter.
        if ((nds_turn_pending || nds_reader_is_open()) && w >= 512 && h >= 512) {
            bool armed = nds_turn_pending != 0;             // a genuine next/prev turn; nds_turn_dir is set
            nds_turn_pending = 0;                           // consume the trigger (one per turn)
            // A flash is a GC16 update (hwtcon) or, on i.MX where the reading turn is PARTIAL, any
            // update_mode==FULL refresh (the AUTO/GC16 chapter/ghost-clear). In the reader's dark mode
            // the hwtcon flash is GCK16 (the GC16 counterpart), so count it too, keeping it out of the
            // sweep and the settle tracking. Gated to hwtcon (cfa_field != 0) because id 8 is DU4 on
            // i.MX, not a flash.
            uint32_t wf = nds_rd32(u, OFF_WAVEFORM), md = nds_rd32(u, OFF_UPDMODE);
            bool is_flash = (wf == WF_GC16) || (plat->cfa_field && wf == WF_GCK16)
                         || (plat->flash_full && md == UPD_FULL);
            // Book-open / chapter-jump settle (see nds_flash_seq): an unarmed full-screen flash
            // records the gesture that caused it; the next full-screen content render from that SAME
            // gesture is the open/jump settle and must not animate. A real turn is armed, so a turn's
            // own GC16 flash never records a settle and never suppresses the next turn.
            long gseq = nds_gesture_seq();
            bool settle = false;
            if (is_flash) {
                if (!armed) { nds_flash_seq = gseq; nds_flash_ts = nds_now(); }
            } else {
                settle = (nds_flash_seq >= 0 && gseq == nds_flash_seq && (nds_now() - nds_flash_ts) <= NDS_SETTLE_WINDOW_S);
                nds_flash_seq = -1;   // consume: only the first full render after a flash can settle
            }
            // Only a genuine turn (armed) whose gesture is enabled animates. Everything unarmed is a
            // menu / open / jump / settle and passes through untouched.
            bool want_anim = armed && nds_gesture_animate();

            // Kobo forces a full GC16 flash on the first turn after a book open / chapter jump / wake.
            // We let that flash happen (it's below via the is_flash passthrough) rather than converting
            // it to a sweep: on a freshly-woken, deeply-cold panel that converted sweep stalls band 0
            // badly, and the flash is what Kobo intends there anyway.
            if (settle) {
                static int nset = 0;
                if (nds_verbose_enabled && nset < 40) { nset++;
                    NDS_LOG("SETTLE [%s] %ux%u wf=%u(%s) -> render right after a full-screen flash (book open / chapter / ghost-clear), not animated",
                            plat->name, w, h, wf, nds_wf_name(wf, plat));
                }
            } else if (want_anim && !is_flash && nds_wf_sweepable(plat, wf)) {
                int cold_ms = nds_cold_skip_ms();
                if (nds_prewarm_ms() == 0 && cold_ms > 0 && nds_turn_idle_any_us >= (uint64_t)cold_ms * 1000ull) {
                    // Cold controller (idle past the EPD power-down): a swept turn would stutter on
                    // band 0 while the panel re-powers. Let Nickel's own full refresh through for this
                    // one turn (clean, no stutter) and skip the animation. Falls through to the
                    // real_ioctl passthrough at the end of the function.
                    static int cs = 0;
                    if (nds_verbose_enabled && cs < 40) { cs++;
                        NDS_LOG("COLD-SKIP [%s] %ux%u idle_any=%lums >= %dms -> plain refresh, not animated",
                                plat->name, w, h, (unsigned long)(nds_turn_idle_any_us / 1000ull), cold_ms);
                    }
                } else {
                    bool rtl = nds_rtl(); if (nds_turn_dir == 1) rtl = !rtl;
                    static int swept = 0;
                    if (nds_verbose_enabled && swept < 40) { swept++;
                        NDS_LOG("SWEEP [%s] %ux%u wf=%u(%s) band_wf=%u dither=0x%x flags=0x%x -> %d strips %s cfa_skip=%d", plat->name, w, h,
                                wf, nds_wf_name(wf, plat), (nds_strip_wf() ? nds_strip_wf() : nds_default_strip_wf(plat, wf)),
                                nds_dither(u, plat), nds_rd32(u, plat->flags_off), nds_strips(), rtl ? "R->L" : "L->R",
                                (plat->cfa_skip && nds_cfa_skip()) ? 1 : 0);
                    }
                    {
                        // Resolve everything the driver needs here, so driver_hwtcon.cc / driver_mxcfb.cc keeps no state
                        // of its own and the config/tuning rules stay in one place.
                        const uint32_t turn_wf = nds_rd32(u, OFF_WAVEFORM);
                        uint32_t swf = nds_strip_wf();                     // config override wins
                        if (swf == 0) swf = nds_default_strip_wf(plat, turn_wf);
                        bool rtl = nds_rtl();
                        if (nds_turn_dir == 1) rtl = !rtl;                 // backward turn sweeps back
                        struct nds_sweep_env env;
                        env.real_ioctl       = real_ioctl;
                        env.bands            = nds_strips();
                        env.delay_us         = nds_delay_us((int)plat->def_delay_us);
                        env.rtl              = rtl;
                        env.wait_complete    = nds_wait_complete();
                        env.cfa_skip         = nds_cfa_skip();
                        env.strip_wf         = swf;
                        env.prewarm_ms       = nds_prewarm_ms();
                        env.idle_any_us      = nds_turn_idle_any_us;
                        env.ephemeral_marker = &nds_ephemeral_marker;

                        const uint64_t t0 = nds_now_us();
                        if (nds_is_hwtcon(plat)) nds_hwtcon_sweep(fd, plat, u, &env);
                        else                     nds_mxcfb_sweep(fd, plat, u, &env);
                        const uint64_t took = nds_now_us() - t0;
                        nds_last_send_us = nds_now_us();   // strips bypassed this hook; re-anchor idle
                        nds_note_band_time(env.bands, took);
                    }
                    return 0;   // suppress the original; Nickel's WAIT_COMPLETE(M) resolves via the last strip
                }
            } else {
                // Passed through: a real turn whose gesture is disabled or isn't a B&W reading turn,
                // or an unarmed menu/open render. The log spells out why an armed turn didn't animate:
                // want_anim=0 means the gesture toggle is off (or the filter mis-classified); is_flash=1
                // is a GC16/GCK16 or full refresh; sweepable=0 means the waveform isn't a reading turn
                // (a colour page, or a mode we don't sweep). flags shows the CFA colour-mode field
                // (0 on a greyscale page), so a mis-detected colour turn is visible too.
                if (nds_verbose_enabled && armed) {
                    static int n = 0;
                    if (n < 40) { n++;
                        long gts = 0; int tx = -1;
                        enum nds_gesture g = nds_gesture_last(&gts, &tx);
                        NDS_LOG("no-anim [%s] %ux%u dir=%s gesture=%s age=%lds wf=%u(%s) mode=%s flags=0x%x is_flash=%d sweepable=%d want_anim=%d -> passthrough",
                                plat->name, w, h, nds_turn_dir ? "back" : "fwd", nds_gesture_name(g),
                                nds_now() - gts, wf, nds_wf_name(wf, plat), md == UPD_FULL ? "FULL" : "PARTIAL",
                                nds_rd32(u, plat->flags_off), is_flash ? 1 : 0, nds_wf_sweepable(plat, wf) ? 1 : 0, want_anim);
                    }
                }
            }
        }
    }

    // Interface discovery, ahead of the 'F'-magic filter so an unmodelled platform still shows up.
    if (!nds_off() && !nds_safety_disabled && nds_verbose_enabled) {
        nds_log_new_request(request);
        nds_sunxi_probe(request, argp, nds_turn_pending);
    }

    // observe: log the e-ink ioctl stream (first 160), passthrough unchanged
    if (!nds_off() && !nds_safety_disabled && nds_verbose_enabled && NDS_IOC_MAGIC(request) == 0x46) {
        static int logged = 0;
        if (logged < 160) {
            // Count only lines actually written. The counter used to advance on every e-ink ioctl,
            // including ones no branch below printed, so the budget could run out in silence.
            int n = logged + 1;
            if (plat && argp) {
                const uint8_t *u = (const uint8_t *)argp;
                uint32_t owf = nds_rd32(u, OFF_WAVEFORM);
                NDS_LOG("ioctl #%d SEND [%s] marker=%u region=(t%u,l%u,%ux%u) wf=%u(%s) mode=%s dither=0x%x flags=0x%x", n, plat->name,
                        nds_rd32(u, OFF_MARKER), nds_rd32(u, OFF_TOP), nds_rd32(u, OFF_LEFT), nds_rd32(u, OFF_WIDTH), nds_rd32(u, OFF_HEIGHT),
                        owf, nds_wf_name(owf, plat), nds_rd32(u, OFF_UPDMODE) == UPD_FULL ? "FULL" : "PARTIAL", nds_dither(u, plat), nds_rd32(u, plat->flags_off));
                logged = n;
            } else if (nds_active && argp && request == nds_active->wait_cmpl) {
                NDS_LOG("ioctl #%d WAIT_COMPLETE marker=%u", n, nds_rd32((const uint8_t *)argp, 0));
                logged = n;
            } else if (nds_active && argp && nds_active->wait_sub && request == nds_active->wait_sub) {
                NDS_LOG("ioctl #%d WAIT_SUBMISSION marker=%u", n, nds_rd32((const uint8_t *)argp, 0));
                logged = n;
            } else {
                // An e-ink ioctl on an interface the mod does not drive. Without this the log was empty
                // on exactly the devices a report is wanted from, which reads the same as "the mod saw
                // nothing". Decode the request itself: the number carries the argument size, so a single
                // line is enough to say which update struct that device uses.
                NDS_LOG("ioctl #%d UNKNOWN request=0x%08lx nr=0x%02lx size=%lu dir=%lu%s", n,
                        request, request & 0xFFUL, (request >> 16) & 0x3FFFUL, (request >> 30) & 3UL,
                        argp ? "" : " (null arg)");
                logged = n;
            }
        }
    }

    if (real_ioctl)
        return real_ioctl(fd, request, argp);
    if (!nds_safety_disabled) { nds_safety_disabled = true; NDS_LOG("SAFETY: real ioctl NULL"); }
    return -1;
}

// ---- page-turn hooks: arm the sweep + record direction ----------------------------------
// ReadingView::goToNextPage / goToPrevPage are the single sink every sequential page turn funnels
// into: a tap goes tapGesture -> nextPage -> goToNextPage, a swipe or physical button goes
// nextPageWithTimer -> goToNextPage, all via the PLT, so hooking this pair catches every turn type
// and records its true direction, with no tap-position or tap-zone guessing. Chapter jumps and book
// opens use other paths, so they never arm here. These run on the UI thread, so they double as a
// safe retry point for installing the gesture filter in case Qt didn't exist yet at plugin init.
extern "C" __attribute__((visibility("default")))
void _nds_goToNextPage(void *self) {
    nds_gesture_filter_install();
    nds_turn_dir = 0; nds_turn_pending = 1; nds_turn_ts = nds_now();
    static int n = 0; if (nds_verbose_enabled && n < 16) { n++; NDS_LOG("turn: forward"); }
    if (real_goToNextPage) real_goToNextPage(self);
}
extern "C" __attribute__((visibility("default")))
void _nds_goToPrevPage(void *self) {
    nds_gesture_filter_install();
    nds_turn_dir = 1; nds_turn_pending = 1; nds_turn_ts = nds_now();
    static int n = 0; if (nds_verbose_enabled && n < 16) { n++; NDS_LOG("turn: backward"); }
    if (real_goToPrevPage) real_goToPrevPage(self);
}

// ---- init / uninstall -------------------------------------------------------------------
static int nds_init() {
    nds_global_config_get("");
    if (access(NDS_CONFIG_DIR "/disabled-by-safety", F_OK) == 0) {
        nds_safety_disabled = true;
        NDS_LOG("startup: disabled-by-safety marker present; passing through");
        return 0;
    }
    const char *dcfg = nds_global_config_get("nds_delay_us");   // platform-dependent when unset
    // Publish verbose tracing for the boot (observe mode / nds_log:1 / config problem) before the
    // hooks can fire, so a broken config's SWEEP/SKIP/turn traces are on when they're needed.
    nds_verbose_enabled = nds_verbose_compute();
    if (nds_verbose_enabled)
        nds_sunxi_init();          // address-validation fd for the sunxi probe; harmless elsewhere
    // Startup block (always logged): mod version, firmware version, then the effective settings.
    NDS_LOG("startup: NickelDissolve " NH_VERSION);
#ifdef NDS_PRERELEASE
    // Say so in the log itself: a report from this build must not be read as a report about a release.
    NDS_LOG("startup: PRERELEASE diagnostics build. Animates every recognised e-ink interface, "
            "including ones with no page-turn evidence, and always traces. Not for general release.");
#endif
    nds_log_firmware();
    nds_log_hwconfig();
    NDS_LOG("startup: auto-tuning -> band count by physical panel width (~0.35in per band; panel dpi=%d, "
            "0=unknown, falls back to pixel width), glkw16 band waveform on colour panels", nds_panel_dpi());
    // The plugin normally loads with the Qt application already up (Qt scans imageformats
    // plugins on the main thread), so this usually succeeds right here; if not, the page-turn
    // hooks retry, and tap turns fall back to the legacy any-big-render behaviour until then.
    bool gf = nds_gesture_filter_install();
    const char *scfg = nds_global_config_get("nds_strips");
    NDS_LOG("startup: mode=%s strips=%s dir=%s delay=%s animate(swipe/tap/button)=%d/%d/%d gesture_filter=%d verbose=%d "
            "| debug: strip_wf=%u(0=keep) wait=%s cfa_skip=%d force_bw=%d sweep_any_wf=%d",
            nds_off() ? "off" : (nds_sweep_mode() ? "SWEEP" : "observe"), (scfg && *scfg) ? scfg : "auto(by panel)",
            nds_rtl() ? "R->L" : "L->R", (dcfg && *dcfg) ? dcfg : "auto(per-platform)",
            nds_animate_swipe(), nds_animate_tap(), nds_animate_button(), gf, nds_verbose_enabled,
            nds_strip_wf(), nds_wait_complete() ? "complete" : "submission",
            nds_cfa_skip(), nds_force_bw(), nds_global_config_bool("nds_debug_sweep_any_waveform", false));
    return 0;
}
static bool nds_del(const char *p) { return access(p, F_OK) != 0 ? true : nh_delete_file(p); }
static bool nds_uninstall() {
    NDS_LOG("uninstall: removing NickelDissolve files");
    bool ok = true;
    ok = nds_del(NDS_CONFIG_DIR "/doc") && ok;
    ok = nds_del(NDS_CONFIG_DIR "/default") && ok;
    ok = nds_del(NDS_CONFIG_DIR "/config") && ok;
    ok = nds_del(NDS_CONFIG_DIR "/nickel-dissolve.log") && ok;
    ok = nds_del(NDS_CONFIG_DIR "/nickel-dissolve.log.old") && ok;
    ok = nds_del(NDS_CONFIG_DIR "/disabled-by-safety") && ok;
    ok = nds_del(NDS_CONFIG_DIR "/uninstall") && ok;
    ok = nds_del(NDS_CONFIG_DIR "/uninstall-now") && ok;
    if (access(NDS_CONFIG_DIR, F_OK) == 0) ok = nh_delete_dir(NDS_CONFIG_DIR) && ok;
    return ok;
}

// ---- NickelHook wiring -------------------------------------------------------------------
static struct nh_info NickelDissolveInfo = {
    .name            = "NickelDissolve",
    .desc            = "Kindle-style directional page-turn band-wipe (Kobo-agnostic).",
    .uninstall_flag  = NDS_CONFIG_DIR "/uninstall-now",
    .uninstall_xflag = NDS_CONFIG_DIR "/uninstall",
    .failsafe_delay  = 3,
};
static struct nh_hook NickelDissolveHooks[] = {
    { .sym = "ioctl", .sym_new = "_nds_ioctl",
      .lib = NDS_LIBKOBO, .out = nh_symoutptr(real_ioctl),
      .desc = "intercept the e-ink page-turn update and sweep it", .optional = true },
    //libnickel 4.6.9960 * _ZN11ReadingView12goToNextPageEv
    { .sym = "_ZN11ReadingView12goToNextPageEv", .sym_new = "_nds_goToNextPage",
      .lib = NDS_LIBNICKEL, .out = nh_symoutptr(real_goToNextPage),
      .desc = "arm sweep + record forward direction (all turn types)", .optional = true },
    //libnickel 4.6.9960 * _ZN11ReadingView12goToPrevPageEv
    { .sym = "_ZN11ReadingView12goToPrevPageEv", .sym_new = "_nds_goToPrevPage",
      .lib = NDS_LIBNICKEL, .out = nh_symoutptr(real_goToPrevPage),
      .desc = "arm sweep + record backward direction (all turn types)", .optional = true },
    //libnickel 4.6.9960 * _ZN21N3SettingsReadingViewC1EP7QWidget
    { .sym = "_ZN21N3SettingsReadingViewC1EP7QWidget", .sym_new = "_nds_settings_ctor",
      .lib = NDS_LIBNICKEL, .out = nh_symoutptr(real_settings_ctor),
      .desc = "add Page-turn-animations toggle to Reading settings", .optional = true },
    {0},
};
static struct nh_dlsym NickelDissolveDlsym[] = {
    //libnickel 4.6.9960 * _ZN13TouchCheckBoxC1EP7QWidget
    { .name = "_ZN13TouchCheckBoxC1EP7QWidget", .out = nh_symoutptr(nds_touchcheckbox_ctor),
      .desc = "TouchCheckBox ctor (native Reading-settings checkbox)", .optional = true },
    // Device colour-panel query, for the glkw16 band-waveform choice. Both optional AND independent:
    // if EITHER is missing, nds_panel_is_color() reports mono and the sweep falls back to the reading
    // waveform. hasColorDisplay was introduced in firmware 4.38.21908 and is absent on anything older;
    // that absence is exactly the "no colour panel here" signal (colour hardware ships with 4.38+), so
    // the .optional lookup is the device gate, with no firmware-version check needed. Pre-4.38 / mono
    // devices simply reuse GLR16, which is what they want anyway.
    //libnickel 4.6.9960 * _ZN6Device16getCurrentDeviceEv
    { .name = "_ZN6Device16getCurrentDeviceEv", .out = nh_symoutptr(real_getCurrentDevice),
      .desc = "Device::getCurrentDevice (the active device singleton)", .optional = true },
    //libnickel 4.38.21908 * _ZNK6Device15hasColorDisplayEv (introduced 4.38.21908; absent on older fw)
    { .name = "_ZNK6Device15hasColorDisplayEv", .out = nh_symoutptr(real_hasColorDisplay),
      .desc = "Device::hasColorDisplay (colour vs mono panel; firmware 4.38.21908+)", .optional = true },
    {0},
};

NickelHook(
    .init      = &nds_init,
    .info      = &NickelDissolveInfo,
    .hook      = &NickelDissolveHooks[0],
    .dlsym     = &NickelDissolveDlsym[0],
    .uninstall = &nds_uninstall,
)
