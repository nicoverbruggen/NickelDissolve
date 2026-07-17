// NickelDissolve — a Kobo-agnostic page-turn "band wipe".
//
// A Kindle page turn sweeps the new page in as a directional wipe (a native MediaTek hardware
// "swipe" the Kobo drivers don't expose). NickelDissolve approximates it from userspace: it takes
// the single full-screen e-ink update Nickel issues per page turn and replaces it with a SEQUENCE
// of partial updates over vertical strips, swept across the screen, so the new page appears
// strip-by-strip. Only the driver's public update ioctl is used — no driver patching.
//
// PLATFORM-AGNOSTIC. Kobo has two e-ink update interfaces with the SAME update-struct prefix
// (region@0, waveform_mode@16, update_mode@20, update_marker@24 — hwtcon was modelled on mxcfb):
//   * hwtcon (MTK)  — Clara BW/Colour, Libra Colour, Elipsa 2E : HWTCON_SEND_UPDATE 0x4024462E
//   * mxcfb  (i.MX) — Libra 2 & most pre-2024                  : MXCFB_SEND_UPDATE  0x4048462E
// We recognise either by its ioctl number and drive it by field offset, so no per-struct code.
//
// TRIGGER. Instead of gating on a specific waveform (MTK-only, and wrong for colour/CFA pages), we
// mark a turn "pending" from ReadingView::goToNextPage/goToPrevPage — the single sink every turn
// (tap, swipe, button) funnels into, so it also gives the true direction — and sweep the next
// full-screen update. We REUSE that update's own waveform, so whatever the device/page uses (GLR16,
// REAGL, a Kaleido CFA mode…) is preserved — the wipe is just that update, chopped into swept strips.
//
// NO HANG. Nickel does WAIT_FOR_UPDATE_COMPLETE(M) after the turn; the LAST strip reuses its marker
// M, so that wait resolves. If the last strip fails to submit, we resubmit the original update.
//
// SAFETY. Default mode is "sweep" (the wipe is on out of the box), with "observe" (pure passthrough +
// logging) and "off" (fully inert) as fallbacks. Only the hwtcon and mxcfb interfaces are handled; any
// other device (e.g. AllWinner/sunxi) is left untouched and reads as unsupported. In "sweep",
// only a full-screen update that immediately follows a page turn is ever touched; everything else
// passes byte-for-byte. Worst case of a bad sweep is a garbled frame fixed by the next refresh —
// recoverable, not a brick. The config file is optional; without one the defaults apply.

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <NickelHook.h>

#include "config.h"
#include "gesture.h"
#include "settingsui.h"
#include "util.h"

static const char *const NDS_LIBKOBO   = "/usr/local/Kobo/platforms/libkobo.so";
static const char *const NDS_LIBNICKEL = "/usr/local/Kobo/libnickel.so.1.0.0";

// ---- update-struct field offsets (common to hwtcon & mxcfb) ----
enum { OFF_TOP = 0, OFF_LEFT = 4, OFF_WIDTH = 8, OFF_HEIGHT = 12,
       OFF_WAVEFORM = 16, OFF_UPDMODE = 20, OFF_MARKER = 24 };
enum { UPD_PARTIAL = 0, UPD_FULL = 1 };
#define WF_GC16 2u                    // GC16 (full black-flash mode) — id 2 on BOTH hwtcon and mxcfb
#define NDS_IOC_MAGIC(req) (((req) >> 8) & 0xFFU)
#define NDS_TURN_WINDOW_S  2          // a pending turn is stale after this many seconds
#define NDS_PREWARM_PX     2          // width of the cold-wake lead-in sliver (a hairline; the driver
                                      // validates region bounds only, no min width — see hwtcon dump)

// ---- platform table: which ioctls carry an e-ink update, per driver ----
struct nds_platform {
    const char   *name;
    unsigned long send;        // *_SEND_UPDATE (intercept + reissue partial strips)
    unsigned long wait_sub;    // *_WAIT_FOR_UPDATE_SUBMISSION (0 = none, e.g. mxcfb)
    unsigned long wait_cmpl;   // *_WAIT_FOR_UPDATE_COMPLETE (for logging / optional per-strip wait)
    uint32_t      size;        // bytes to copy when reissuing an update
    uint32_t      flags_off;   // byte offset of the `flags` field in the update struct
    uint32_t      cfa_skip;    // CFA-skip flag bit (skips the per-region colour pass; 0 = n/a)
    uint32_t      cfa_field;   // mask of the CFA colour-mode field inside `flags` (0 = no CFA;
                               // hwtcon: HWTCON_FLAG_CFA_FLDS_MASK 0x7f00 — non-zero means the
                               // update runs the driver's CFA colour pass, i.e. a COLOUR page)
    bool          flash_full;  // how a full-flash refresh is flagged: true = update_mode==FULL (i.MX,
                               // where the reading turn is PARTIAL); false = by the GC16 waveform
                               // (hwtcon, where the reading turn is itself FULL)
    uint32_t      def_delay_us;// per-platform default inter-strip delay when nds_delay_us is unset:
                               // hwtcon paces itself via the submission-wait (0); mxcfb has no
                               // submission-wait, so it needs an explicit delay or the wipe is instant
    uint32_t      dith_off;    // byte offset of dither_mode in the update struct (0 = don't log it):
                               // hwtcon_update_data has int dither_mode right after flags (@0x20)
    uint32_t      read_wf;     // platform's flashless greyscale reading waveform (hwtcon: GLR16=4;
                               // mxcfb: REAGL=6). Currently unused: it was the target of the
                               // first-turn GC16->sweep conversion, which has been removed.
    uint32_t      def_strip_wf;// waveform to draw the swept bands with when nds_debug_strip_waveform is
                               // unset. 0 = reuse the update's own waveform. Where reusing the native
                               // reading waveform for every band is too slow (i.MX), use a fast wipe
                               // waveform (DU) here and set settle_native below; the fast bands are then
                               // just throwaway motion. nds_debug_strip_waveform overrides per device.
};
static const struct nds_platform NDS_PLATFORMS[] = {
    // MTK (Clara BW/Colour, Libra Colour): HWTCON flags@28, HWTCON_FLAG_CFA_SKIP = 0x8000,
    // CFA colour-mode field = HWTCON_FLAG_CFA_FLDS_MASK 0x7f00 (G1=0x100, AIE_S4=0x200, ...,
    // NTX=0xa00, NTX_SF=0xb00 — 0 means no colour processing for this update).
    // Reading turn = FULL + GLR16; flash = FULL + GC16 → detect flash by waveform.
    // Paced by WAIT_FOR_UPDATE_SUBMISSION between strips, so default delay 0.
    { "hwtcon", 0x4024462EUL, 0x40044637UL, 0xC008462FUL, 36, 28, 0x8000UL, 0x7f00UL, false, 0,     32, 4, 0 },
    // i.MX (Libra 2 & most pre-2024): mxcfb flags@32, no CFA (mono panels only).
    // Reading turn = PARTIAL + REAGL(6); flash = FULL + AUTO(257)/GC16 → detect flash by mode==FULL.
    // No submission-wait to pace strips, so default to ~30 ms/strip. def_strip_wf = 0: the strips REUSE
    // the turn's own waveform, i.e. whatever high-quality greyscale mode Nickel picked for the page
    // (REAGL on panels that support it), so each band is a full-quality, flashless render and we use
    // the panel's best refresh automatically without hardcoding one (this is the v0.3 behaviour). Where
    // reusing REAGL per band is too slow, nds_debug_strip_waveform:1 forces the faster DU wipe.
    { "mxcfb",  0x4048462EUL, 0UL,          0xC008462FUL, 72, 32, 0UL,      0UL,      true,  30000, 0,  6, 0 },
};
static const int NDS_NPLAT = (int)(sizeof(NDS_PLATFORMS) / sizeof(NDS_PLATFORMS[0]));

static const struct nds_platform *nds_match(unsigned long request) {
    for (int i = 0; i < NDS_NPLAT; i++)
        if (request == NDS_PLATFORMS[i].send) return &NDS_PLATFORMS[i];
    return nullptr;
}

// unaligned-safe field access
static inline uint32_t rd32(const uint8_t *b, unsigned off) { uint32_t v; memcpy(&v, b + off, 4); return v; }
static inline void     wr32(uint8_t *b, unsigned off, uint32_t v) { memcpy(b + off, &v, 4); }

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

static long nds_now() { return (long)time(nullptr); }

// Idle tracking: the gap since the last e-ink SEND gates the cold-wake mitigations (the prewarm
// sliver and the cold-skip). Wall-clock µs; only differences are used, so a clock step (unlikely on
// a reading device) at worst mis-gates one turn.
static uint64_t nds_now_us() {
    struct timeval tv; gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}
static uint64_t nds_last_send_us = 0;        // last e-ink SEND the hook saw (turn, footer, clock, ...)
static uint64_t nds_turn_idle_any_us  = 0;   // gap before this update since the previous update of any kind

// ---- config -----------------------------------------------------------------------------
// The config file is OPTIONAL: no file (or a missing key) means the defaults below. User keys
// (nds_mode, nds_strips, nds_delay_us, nds_direction, nds_animate_on_*) are documented in the
// on-device doc; nds_debug_* keys are for debugging and documented in ABOUT.md only.
//
// nds_mode: "sweep" (DEFAULT — the wipe is on out of the box), "observe" (passthrough + log the
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
//   0 = not supported: sunxi (Elipsa/Sage) or an unrecognised / not-yet-detected platform.
extern "C" int nds_device_support(void) {
    if (nds_active && !strcmp(nds_active->name, "hwtcon")) return 2;
    if (nds_active && !strcmp(nds_active->name, "mxcfb"))  return 1;
    return 0;
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
    // In "sweep" mode, stay quiet unless the user opts in with nds_log:1, or the config had a
    // problem (force-enabled so mistakes self-diagnose).
    return nds_global_config_bool("nds_log", false) || nds_config_problem_seen();
}
// ---- per-device animation tuning ---------------------------------------------------------
// Two independent axes, chosen so same-size / same-panel devices tune identically with no model list
// to maintain. Config keys (nds_strips, nds_debug_strip_waveform) still win over both.
//   * Band count follows the panel's longest edge (learned from the full-screen updates below): the
//     7" panels (Libra Colour / Libra 2, 1264x1680) get 12 bands; the 6" panels (Clara BW / Colour /
//     2E / HD, 1072x1448) and anything unknown get 10.
//   * The band waveform is glkw16 (a Kaleido B&W-optimised mode, crisper bands) on colour panels,
//     decided by Device::getCurrentDevice()->hasColorDisplay(); mono panels keep their reading wf.
static uint32_t nds_panel_max_dim = 0;   // longest panel edge seen so far (px); 0 until the first update
static int nds_default_strips() {
    return nds_panel_max_dim >= 1600 ? 12 : 10;   // 7" (…x1680) -> 12; 6" (…x1448) and unknown -> 10
}
// Device colour-panel query, via libnickel symbols resolved in NickelDissolveDlsym. Cached (the panel
// can't change at runtime). Defaults to mono — no glkw16 — if the symbols are missing on an unfamiliar
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
// Band waveform when nds_debug_strip_waveform is unset. glkw16 is the raw kernel EPD id 8 (validated
// by eye on the Libra Colour and Clara Colour); cfa_field gates it to the hwtcon/Kaleido interface,
// since id 8 means something else on mxcfb. 0 = reuse the update's own waveform (hwtcon GLR16, mxcfb
// REAGL). libkobo's KoboScreenMTK numbers modes differently (see nds_wf_name) — this is intentional.
static uint32_t nds_default_strip_wf(const struct nds_platform *plat) {
    if (plat->cfa_field && nds_panel_is_color()) return 8u;   // glkw16, colour Kaleido panels only
    return plat->def_strip_wf;
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
// the caller's per-platform default (hwtcon 0 — paced by the submission-wait; mxcfb ~30 ms — no
// submission-wait, else the wipe is instant). Returns µs, clamped to [0, 50000].
static int  nds_delay_us(int def) { const char *v = nds_global_config_get("nds_delay_us"); int d = (v && *v) ? atoi(v) : def; if (d < 0) d = 0; if (d > 50000) d = 50000; return d; }
// Per-gesture control: which page-turn gestures animate. DEFAULT: all of them — always use the
// transition when possible. Every real next/prev turn arms via the goToNextPage/goToPrevPage
// hooks (taps, swipes, and physical buttons all funnel through them), which also record the true
// direction; the app-wide Qt event filter (gesture.cc) only supplies the input TYPE, so each
// turn's animation can be enabled or disabled per gesture.
static bool nds_animate_swipe()  { return nds_global_config_bool("nds_animate_on_swipe", true); }
static bool nds_animate_tap()    { return nds_global_config_bool("nds_animate_on_tap", true); }
static bool nds_animate_button() { return nds_global_config_bool("nds_animate_on_button", true); }
// Set HWTCON_FLAG_CFA_SKIP on the strips (Kaleido): skips the per-region CFA colour pass whose
// region boundaries produce the seams. Correct for B&W content (no colour to convert) — and we
// only ever sweep B&W reading turns (see nds_wf_sweepable), so this is exactly right.
static bool nds_cfa_skip()    { return nds_global_config_bool("nds_debug_cfa_skip", true); }
// Force the WHOLE device to greyscale: OR HWTCON_FLAG_CFA_SKIP into every hwtcon update (menus,
// home, covers, reading), so the panel's colour pass is skipped everywhere. A debugging /
// full-B&W-mode switch, independent of the animation. No-op on mono/i.MX. See nds_force_bw use.
static bool nds_force_bw()    { return nds_global_config_bool("nds_debug_force_bw", false); }
// 0 = keep the turn's own waveform (platform-agnostic default); >0 = force a raw waveform id
// (PLATFORM-SPECIFIC — e.g. GLR16 is 4 on hwtcon but 6 on mxcfb; use with care).
static uint32_t nds_strip_wf() { const char *v = nds_global_config_get("nds_debug_strip_waveform"); if (!v || !*v) return 0; int w = atoi(v); return w > 0 ? (uint32_t)w : 0; }

// hwtcon waveform ids (from KoboScreenMTK::idToWaveform in libkobo). The greyscale reading
// waveforms are GL16/GLR16; the colour ones (GCC16/GLRC16/GCK16/GLKW16) and the GC16 flash /
// A2·DU menu modes are everything else.
#define WF_DU 1u
#define WF_GL16 3u
#define WF_GLR16 4u
#define WF_GCC16 10u
#define WF_GLRC16 11u
// Whether an update carrying this waveform should be swept. THE colour guard, validated against
// libkobo's KoboScreenMTK::handleAutoWfm: it calls fbIsGray(rect) and picks a COLOUR waveform
// (GCC16=10 / GLRC16=11 / GCK16=8 / GLKW16=9) for non-grey content, a greyscale one (GLR16=4,
// GL16=3) for text. So a colour page never reaches us as GL16/GLR16; gating on the waveform skips
// colour pages, GC16 flashes, AUTO(257), and A2/DU menu updates in a single test, and (unlike the
// always-set CFA flag field) it is genuinely content-driven. On a mono panel (mxcfb, no CFA) the
// reading waveform varies by board revision: logs show one Libra 2 turning with REAGL(6) and an older
// board with GLKW16(10), both using AUTO(257)/DU for menus and GC16 / full-screen AUTO for flashes. So
// we allow the flashless greyscale reading set (GL16=5, REAGL=6, REAGLD=7, GLKW16=10, i.MX ids) and
// reject everything else, so menus and the toolbar never sweep. The animation is best-effort on i.MX: a
// REAGL board looks great, a GLKW16 board is slow, which is why the settings warn it may not work.
// nds_debug_sweep_any_waveform bypasses the allowlist (sweep anything non-GC16) for experiments.
static bool nds_wf_sweepable(const struct nds_platform *plat, uint32_t wf) {
    if (nds_global_config_bool("nds_debug_sweep_any_waveform", false)) return wf != WF_GC16;
    if (plat->cfa_field == 0)                              // mono (i.MX): flashless greyscale reading modes
        return wf == 5u || wf == 6u || wf == 7u || wf == 10u;  // GL16 / REAGL / REAGLD / GLKW16 (i.MX ids)
    return wf == WF_GL16 || wf == WF_GLR16;                // Kaleido-capable: greyscale reading only
}
// i.MX (mxcfb) and MTK (hwtcon) number their waveform modes differently, so the name is
// platform-dependent. i.MX numbering is from koreader's mxcfb-kobo.h; the WF_* constants below are
// the MTK numbering. i.MX mono panels carry cfa_field == 0, which selects the right table.
static const char *nds_wf_name(uint32_t wf, const struct nds_platform *plat) {
    if (plat && plat->cfa_field == 0u) {   // i.MX / mxcfb
        switch (wf) {
            case 0:  return "INIT";  case 1: return "DU";    case 2:  return "GC16";   case 3:   return "GC4";
            case 4:  return "A2";    case 5: return "GL16";  case 6:  return "REAGL";  case 7:   return "REAGLD";
            case 8:  return "DU4";   case 9: return "GCK16"; case 10: return "GLKW16"; case 257: return "AUTO";
            default: return "?";
        }
    }
    switch (wf) {   // MTK / hwtcon
        case 0: return "INIT"; case WF_DU: return "DU"; case WF_GC16: return "GC16";
        case WF_GL16: return "GL16"; case WF_GLR16: return "GLR16"; case 6: return "REAGL";
        case 8: return "GCK16"; case 9: return "GLKW16"; case WF_GCC16: return "GCC16";
        case WF_GLRC16: return "GLRC16"; case 257: return "AUTO"; default: return "?";
    }
}
static uint32_t nds_dither(const uint8_t *u, const struct nds_platform *plat) {
    return plat->dith_off ? rd32(u, plat->dith_off) : 0u;
}

// ---- the sweep: replace one full-screen update with N swept partial strips ----------------
static void nds_do_sweep(int fd, const struct nds_platform *plat, const uint8_t *orig) {
    const uint32_t M  = rd32(orig, OFF_MARKER);
    const uint32_t x0 = rd32(orig, OFF_LEFT), y0 = rd32(orig, OFF_TOP);
    const uint32_t W  = rd32(orig, OFF_WIDTH), H = rd32(orig, OFF_HEIGHT);
    const int N = nds_strips();
    const uint32_t sw = W / (uint32_t)N;
    uint32_t wf_override = nds_strip_wf();                     // config override wins
    if (wf_override == 0) wf_override = nds_default_strip_wf(plat);   // else per-device default (colour: glkw16)
    const int delay = nds_delay_us((int)plat->def_delay_us);   // config wins; else per-platform default
    const bool want_complete = nds_wait_complete();
    bool rtl = nds_rtl();
    if (nds_turn_dir == 1) rtl = !rtl;               // backward turn sweeps the opposite way

    uint8_t v[128];
    if (plat->size > sizeof(v) || sw == 0) { real_ioctl(fd, plat->send, (void *)orig); return; }

    const uint32_t cfa_skip = (plat->cfa_skip && nds_cfa_skip()) ? plat->cfa_skip : 0u;

    // Cold-wake lead-in: if the panel has powered down (idle_any past nds_prewarm_ms), the first
    // update after it stalls ~40ms on the power-on. Spend that stall on a thin sliver at the sweep's
    // leading edge (left for L->R, right for R->L) so the freeze is a hairline instead of a full band;
    // the real bands below then run warm and the wipe is smooth. The submission-wait both absorbs the
    // power-on and keeps the driver from merging the sliver into band 0. Self-gates on idle_any, so
    // warm turns pay nothing. rc is logged so the device confirms the driver accepts a sliver width.
    {
        int pw = nds_prewarm_ms();
        if (pw > 0 && nds_turn_idle_any_us >= (uint64_t)pw * 1000ull) {
            memcpy(v, orig, plat->size);
            uint32_t lx = rtl ? (x0 + W - NDS_PREWARM_PX) : x0;
            wr32(v, OFF_TOP, y0); wr32(v, OFF_LEFT, lx); wr32(v, OFF_WIDTH, NDS_PREWARM_PX); wr32(v, OFF_HEIGHT, H);
            wr32(v, OFF_UPDMODE, UPD_PARTIAL);
            if (wf_override) wr32(v, OFF_WAVEFORM, wf_override);
            if (cfa_skip) wr32(v, plat->flags_off, rd32(v, plat->flags_off) | cfa_skip);
            uint32_t mk = nds_ephemeral_marker + 0x200u;   // ephemeral, clear of the strip markers below
            wr32(v, OFF_MARKER, mk);
            uint64_t st0 = nds_now_us();                        // time the sliver's own submit+wait, so the
            int rc = real_ioctl(fd, plat->send, v);             // PREWARM log shows it warm (cheap) vs cold (~wake).
            if (rc >= 0 && plat->wait_sub) { uint32_t wm = mk; real_ioctl(fd, plat->wait_sub, &wm); }
            unsigned long sliver_us = (unsigned long)(nds_now_us() - st0);
            if (nds_verbose_enabled)   // uncapped: a whole reading session is what we want to sample
                NDS_LOG("PREWARM [%s] idle_any=%lums -> %dpx %s-edge (rc=%d) sliver=%luus, then warm sweep",
                        plat->name, (unsigned long)(nds_turn_idle_any_us / 1000ull), NDS_PREWARM_PX,
                        rtl ? "right" : "left", rc, sliver_us);
        }
    }

    bool m_ok = false;
    for (int k = 0; k < N; k++) {
        int col = rtl ? (N - 1 - k) : k;
        uint32_t left  = x0 + (uint32_t)col * sw;
        uint32_t width = (col == N - 1) ? (x0 + W - left) : sw;   // last column takes the remainder
        bool last = (k == N - 1);

        memcpy(v, orig, plat->size);
        wr32(v, OFF_TOP, y0); wr32(v, OFF_LEFT, left); wr32(v, OFF_WIDTH, width); wr32(v, OFF_HEIGHT, H);
        wr32(v, OFF_UPDMODE, UPD_PARTIAL);
        if (wf_override) wr32(v, OFF_WAVEFORM, wf_override);
        if (cfa_skip) wr32(v, plat->flags_off, rd32(v, plat->flags_off) | cfa_skip);  // no per-region CFA seam
        // The last strip carries Nickel's marker M (the strips are the final render); the earlier
        // strips get throwaway ephemeral markers.
        uint32_t mk = last ? M : (nds_ephemeral_marker + (uint32_t)k);
        wr32(v, OFF_MARKER, mk);

        if (real_ioctl(fd, plat->send, v) < 0) continue;
        if (mk == M) m_ok = true;

        if (want_complete) {                         // wait for full render between strips
            uint32_t md[2] = { mk, 0 };
            if (plat->wait_cmpl) real_ioctl(fd, plat->wait_cmpl, md);
        } else if (plat->wait_sub) {                 // hwtcon: wait for submission (beats the MDP merge)
            uint32_t wm = mk;
            real_ioctl(fd, plat->wait_sub, &wm);
        }                                            // mxcfb: no wait needed (no MDP merge)
        if (delay > 0 && !last) usleep((useconds_t)delay);
    }
    // Hang-safety: guarantee marker M is submitted, else Nickel's WAIT_COMPLETE(M) hangs.
    if (!m_ok) real_ioctl(fd, plat->send, (void *)orig);
    nds_ephemeral_marker += (uint32_t)N;

    // The strips bypass this hook (they call real_ioctl directly), so anchor the next turn's idle gap
    // to now; otherwise idle_any would measure from before this sweep and mis-fire the cold-wake path.
    nds_last_send_us = nds_now_us();
}

// ---- per-gesture gating -------------------------------------------------------------------
// Whether the turn that just fired should animate, given its input type. The goToNextPage/
// goToPrevPage hooks already armed the turn and recorded the true direction; this only maps the
// last input the Qt filter saw (gesture.cc) to that gesture's on/off toggle. A center tap that
// merely raises the reading menu never reaches goToNextPage, so it never arms and never gets
// here — no tap-position or zone guessing is involved. If the filter isn't up yet, or the input
// is stale/unknown, treat it as a swipe (the common case) so the turn still animates by default.
static bool nds_gesture_animate() {
    long gts = 0; int tap_x = -1;
    enum nds_gesture g = nds_gesture_last(&gts, &tap_x);
    bool fresh = g != NDS_GESTURE_NONE && (nds_now() - gts) <= NDS_TURN_WINDOW_S;
    if (fresh && g == NDS_GESTURE_BUTTON) return nds_animate_button();
    if (fresh && g == NDS_GESTURE_TAP)    return nds_animate_tap();
    return nds_animate_swipe();
}

// ---- ioctl hook -------------------------------------------------------------------------
extern "C" __attribute__((visibility("default")))
int _nds_ioctl(int fd, unsigned long request, void *argp) {
    const struct nds_platform *plat = nds_match(request);
    if (plat && argp) nds_active = plat;

    // Idle tracking: record the gap before every e-ink SEND since the previous update of any kind
    // (footer/clock keep the controller warm). This gates the cold-wake mitigations (prewarm sliver,
    // cold-skip). Our own sweep strips bypass this hook, so nds_do_sweep re-anchors nds_last_send_us.
    if (plat && argp && request == plat->send) {
        uint64_t now_us = nds_now_us();
        nds_turn_idle_any_us = nds_last_send_us ? (now_us - nds_last_send_us) : 0;
        nds_last_send_us = now_us;
        // Learn the panel's longest edge from full-screen updates; it drives the band count.
        const uint8_t *u = (const uint8_t *)argp;
        uint32_t w = rd32(u, OFF_WIDTH), h = rd32(u, OFF_HEIGHT), md = w > h ? w : h;
        if (md >= 512 && md > nds_panel_max_dim) nds_panel_max_dim = md;
    }

    // Force-B&W: OR CFA_SKIP into every hwtcon update so the whole device renders greyscale.
    // Applies in every mode except "off" (it's not tied to the animation), in place, once.
    if (plat && argp && real_ioctl && !nds_off() && !nds_safety_disabled && plat->cfa_skip && nds_force_bw()) {
        uint8_t *m = (uint8_t *)argp;
        uint32_t f = rd32(m, plat->flags_off);
        if (!(f & plat->cfa_skip)) wr32(m, plat->flags_off, f | plat->cfa_skip);
    }

    if (plat && argp && real_ioctl && !nds_safety_disabled && nds_sweep_mode()) {
        // Expire a stale pending-turn so we never sweep an unrelated later full-screen update.
        if (nds_turn_pending && (nds_now() - nds_turn_ts) > NDS_TURN_WINDOW_S) nds_turn_pending = 0;

        const uint8_t *u = (const uint8_t *)argp;
        uint32_t w = rd32(u, OFF_WIDTH), h = rd32(u, OFF_HEIGHT);
        // Process a full-screen render when a real next/prev turn just armed it (goToNextPage/
        // goToPrevPage hook), or when a book is open (so book-open / chapter-jump flashes can be
        // tracked for the settle below). Menus, home and library are neither, so they never enter.
        if ((nds_turn_pending || nds_reader_is_open()) && w >= 512 && h >= 512) {
            bool armed = nds_turn_pending != 0;             // a genuine next/prev turn; nds_turn_dir is set
            nds_turn_pending = 0;                           // consume the trigger (one per turn)
            // A flash is a GC16 update (hwtcon) or, on i.MX where the reading turn is PARTIAL, any
            // update_mode==FULL refresh (the AUTO/GC16 chapter/ghost-clear).
            uint32_t wf = rd32(u, OFF_WAVEFORM), md = rd32(u, OFF_UPDMODE);
            bool is_flash = (wf == WF_GC16) || (plat->flash_full && md == UPD_FULL);
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
                                wf, nds_wf_name(wf, plat), (nds_strip_wf() ? nds_strip_wf() : nds_default_strip_wf(plat)),
                                nds_dither(u, plat), rd32(u, plat->flags_off), nds_strips(), rtl ? "R->L" : "L->R",
                                (plat->cfa_skip && nds_cfa_skip()) ? 1 : 0);
                    }
                    nds_do_sweep(fd, plat, u);
                    return 0;   // suppress the original; Nickel's WAIT_COMPLETE(M) resolves via the last strip
                }
            } else {
                // Passed through: a real turn whose gesture is disabled or isn't a B&W reading turn,
                // or an unarmed menu/open render. Log the armed cases so a "turn didn't animate"
                // report is diagnosable (armed=1 but want_anim=0 means the gesture toggle is off or
                // the filter mis-classified; a flash/colour turn shows wf).
                if (nds_verbose_enabled && armed) {
                    static int n = 0;
                    if (n < 40) { n++;
                        long gts = 0; int tx = -1;
                        enum nds_gesture g = nds_gesture_last(&gts, &tx);
                        NDS_LOG("no-anim [%s] %ux%u dir=%s gesture=%s age=%lds wf=%u(%s) want_anim=%d -> passthrough",
                                plat->name, w, h, nds_turn_dir ? "back" : "fwd", nds_gesture_name(g),
                                nds_now() - gts, wf, nds_wf_name(wf, plat), want_anim);
                    }
                }
            }
        }
    }

    // observe: log the e-ink ioctl stream (first 160), passthrough unchanged
    if (!nds_off() && !nds_safety_disabled && nds_verbose_enabled && NDS_IOC_MAGIC(request) == 0x46) {
        static int logged = 0;
        if (logged < 160) { logged++;
            if (plat && argp) {
                const uint8_t *u = (const uint8_t *)argp;
                uint32_t owf = rd32(u, OFF_WAVEFORM);
                NDS_LOG("ioctl #%d SEND [%s] marker=%u region=(t%u,l%u,%ux%u) wf=%u(%s) mode=%s dither=0x%x flags=0x%x", logged, plat->name,
                        rd32(u, OFF_MARKER), rd32(u, OFF_TOP), rd32(u, OFF_LEFT), rd32(u, OFF_WIDTH), rd32(u, OFF_HEIGHT),
                        owf, nds_wf_name(owf, plat), rd32(u, OFF_UPDMODE) == UPD_FULL ? "FULL" : "PARTIAL", nds_dither(u, plat), rd32(u, plat->flags_off));
            } else if (nds_active && argp && request == nds_active->wait_cmpl) {
                NDS_LOG("ioctl #%d WAIT_COMPLETE marker=%u", logged, *(const uint32_t *)argp);
            } else if (nds_active && argp && nds_active->wait_sub && request == nds_active->wait_sub) {
                NDS_LOG("ioctl #%d WAIT_SUBMISSION marker=%u", logged, *(const uint32_t *)argp);
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
// and records its true direction — no tap-position or tap-zone guessing. Chapter jumps and book
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
    // Startup block (always logged): mod version, firmware version, then the effective settings.
    NDS_LOG("startup: NickelDissolve " NH_VERSION);
    nds_log_firmware();
    nds_log_hwconfig();
    NDS_LOG("startup: auto-tuning -> band count by panel size (7\"=12 bands, 6\"=10), glkw16 band waveform on colour panels");
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
    // the .optional lookup is the device gate — no firmware-version check needed. Pre-4.38 / mono
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
