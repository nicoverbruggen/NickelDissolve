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
// mark a turn "pending" from ReadingView::next/prevPageWithTimer and sweep the next full-screen
// update. We REUSE that update's own waveform, so whatever the device/page uses (GLR16, REAGL, a
// Kaleido CFA mode…) is preserved — the wipe is just that update, chopped into swept strips.
//
// NO HANG. Nickel does WAIT_FOR_UPDATE_COMPLETE(M) after the turn; the LAST strip reuses its marker
// M, so that wait resolves. If the last strip fails to submit, we resubmit the original update.
//
// SAFETY. Default mode is "sweep" (the wipe is on out of the box; sunxi needs an explicit opt-in),
// with "observe" (pure passthrough + logging) and "off" (fully inert) as fallbacks. In "sweep",
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

// sunxi (AllWinner B300: Elipsa, Sage) DISP_EINK ioctls — a different family (raw numbers, via
// /dev/disp), swept through its own code path (not the platform table). Because the wipe is slow
// on these big panels, sunxi only sweeps on an EXPLICIT nds_mode:sweep (see nds_sweep_explicit).
#define NDS_DISP_EINK_UPDATE   0x0402UL   // arg = ubuffer[7]; ubuffer[0] -> area_info{x_top,y_top,x_bottom,y_bottom}
#define NDS_DISP_EINK_UPDATE2  0x0406UL   // ditto (newer); ubuffer[4] -> frame_id (out)
#define NDS_DISP_EINK_WAIT     0x4014UL   // DISP_EINK_WAIT_FRAME_SYNC_COMPLETE
// On the Elipsa (id 387), every page-turn hook is followed by a full-screen UPDATE2 with this
// update_mode; the occasional 0x80000004 is the full-flash refresh and 0x4 is book-open. We only
// sweep the reading-turn mode (mirrors the hwtcon "skip GC16" rule: never turn a flashless turn
// into a flash). Field is ubuffer[2].
#define NDS_SUNXI_TURN_MODE    0x80440UL

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
    uint32_t      read_wf;     // the flashless greyscale reading waveform for this platform, used
                               // when converting a forced-GC16 first-turn refresh into an animated
                               // sweep (hwtcon: GLR16=4; mxcfb: REAGL=6)
};
static const struct nds_platform NDS_PLATFORMS[] = {
    // MTK (Clara BW/Colour, Libra Colour): HWTCON flags@28, HWTCON_FLAG_CFA_SKIP = 0x8000,
    // CFA colour-mode field = HWTCON_FLAG_CFA_FLDS_MASK 0x7f00 (G1=0x100, AIE_S4=0x200, ...,
    // NTX=0xa00, NTX_SF=0xb00 — 0 means no colour processing for this update).
    // Reading turn = FULL + GLR16; flash = FULL + GC16 → detect flash by waveform.
    // Paced by WAIT_FOR_UPDATE_SUBMISSION between strips, so default delay 0.
    { "hwtcon", 0x4024462EUL, 0x40044637UL, 0xC008462FUL, 36, 28, 0x8000UL, 0x7f00UL, false, 0,     32, 4 },
    // i.MX (Libra 2 & most pre-2024): mxcfb flags@32, no CFA (mono panels only).
    // Reading turn = PARTIAL + REAGL(6); flash = FULL + AUTO(257)/GC16 → detect flash by mode==FULL.
    // No submission-wait to pace strips, so default to ~30 ms/strip: the slower REAGL bands need the
    // room to settle before the next one (tuned on a Libra 2), or the wipe is instant / steppy.
    { "mxcfb",  0x4048462EUL, 0UL,          0xC008462FUL, 72, 32, 0UL,      0UL,      true,  30000, 0,  6 },
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
static void (*real_nextPageWithTimer)(void *self) = nullptr;
static void (*real_prevPageWithTimer)(void *self) = nullptr;

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
static volatile int  nds_first_turn_pending = 0;  // set when a book-open settle fires; the next turn is the first turn, whose forced GC16 we animate instead of flashing
static uint32_t nds_ephemeral_marker = 0xF0000000u;   // markers for non-final strips (never collide with Nickel)
static const struct nds_platform *nds_active = nullptr;   // detected from the first update ioctl (for logging)

static long nds_now() { return (long)time(nullptr); }

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
static bool nds_sweep_mode()  { return !nds_off() && !nds_mode_is("observe"); }
// sunxi (Elipsa/Sage) is functional but NOT recommended (huge panels, slow bands), so it never
// sweeps by default: it requires nds_mode:sweep to be set explicitly in the config.
static bool nds_sweep_explicit() { return nds_mode_is("sweep"); }
// Whether verbose per-ioctl / per-turn tracing should be on for this boot. Tied to the MODE so a
// supported device running the animation (sweep) stays quiet: "observe" exists to log the ioctl
// stream, "off" logs nothing, and "sweep" traces only when the user opts in with nds_log:1 (or
// the config had a problem, which force-enables it so mistakes self-diagnose). Computed once and
// published to nds_verbose_enabled by nds_init.
static bool nds_verbose_compute() {
    if (nds_off()) return false;
    if (nds_mode_is("observe")) return true;
    return nds_global_config_bool("nds_log", false) || nds_config_problem_seen();
}
static int  nds_strips()      { const char *v = nds_global_config_get("nds_strips"); int n = (v && *v) ? atoi(v) : 8; if (n < 2) n = 2; if (n > 32) n = 32; return n; }
static bool nds_rtl()         { const char *d = nds_global_config_get("nds_direction"); return !(d && !strcasecmp(d, "ltr")); }
static bool nds_wait_complete(){ const char *w = nds_global_config_get("nds_debug_wait"); return w && !strcasecmp(w, "complete"); }
// Inter-strip delay. If nds_delay_us is set in config it always wins; if it's absent, fall back to
// the caller's per-platform default (hwtcon 0 — paced by the submission-wait; mxcfb ~30 ms — no
// submission-wait, else the wipe is instant). Returns µs, clamped to [0, 50000].
static int  nds_delay_us(int def) { const char *v = nds_global_config_get("nds_delay_us"); int d = (v && *v) ? atoi(v) : def; if (d < 0) d = 0; if (d > 50000) d = 50000; return d; }
// Per-gesture control: which page-turn gestures animate. DEFAULT: all of them — always use the
// transition when possible. Swipes and physical page-turn buttons arm via the ReadingView hooks
// (told apart by the last input the gesture filter saw); taps are recognised by the app-wide Qt
// event filter (gesture.cc), since Nickel's tap path isn't hookable.
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
// Animate the first page turn after opening a book. Kobo renders that turn as a forced GC16 full
// flash (it can't be talked out of it via settings); when this is on we rewrite it to the
// platform's flashless reading waveform and sweep it, so the first turn animates like the rest.
// The book already did a full clear on open, so skipping this one costs little. On by default.
static bool nds_animate_first_turn() { return nds_global_config_bool("nds_animate_first_turn", true); }
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
// GL16=3) for text. So a colour page never reaches us as GL16/GLR16 — gating on the waveform
// skips colour pages, GC16 flashes, AUTO(257), and A2/DU menu updates in a single test, and
// (unlike the always-set CFA flag field) it is genuinely content-driven. On a mono panel
// (mxcfb, no CFA) there is no colour to avoid, so sweep any non-flash waveform there.
// nds_debug_sweep_any_waveform bypasses the allowlist (sweep anything non-GC16) for experiments.
static bool nds_wf_sweepable(const struct nds_platform *plat, uint32_t wf) {
    if (nds_global_config_bool("nds_debug_sweep_any_waveform", false)) return wf != WF_GC16;
    if (plat->cfa_field == 0) return wf != WF_GC16;          // mono panel: no colour to skip
    return wf == WF_GL16 || wf == WF_GLR16;                  // Kaleido-capable: greyscale reading only
}
static const char *nds_wf_name(uint32_t wf) {
    switch (wf) {
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
    const uint32_t wf_override = nds_strip_wf();     // 0 = keep original
    const int delay = nds_delay_us((int)plat->def_delay_us);   // config wins; else per-platform default
    const bool want_complete = nds_wait_complete();
    bool rtl = nds_rtl();
    if (nds_turn_dir == 1) rtl = !rtl;               // backward turn sweeps the opposite way

    uint8_t v[128];
    if (plat->size > sizeof(v) || sw == 0) { real_ioctl(fd, plat->send, (void *)orig); return; }

    const uint32_t cfa_skip = (plat->cfa_skip && nds_cfa_skip()) ? plat->cfa_skip : 0u;
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
        uint32_t mk = last ? M : (nds_ephemeral_marker + (uint32_t)k);   // last strip carries Nickel's marker M
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
}

// sunxi strip update_mode: a FAST partial-rect refresh. The turn's own mode (0x80440) is GLR16
// (REAGL) — the slowest, highest-quality waveform; 8 REAGL bands on the big Elipsa panel crawl
// (~1 s each). We don't need REAGL quality mid-wipe, so strips use GL16 | RECT (0x420) by default:
// full 16-level greyscale but far faster, and it's exactly the mode Nickel uses for its own partial
// updates. nds_debug_strip_waveform (non-zero) overrides the waveform with a raw *sunxi* id
// (2=DU 4=GC16 0x10=A2 0x20=GL16 0x40=GLR16); 0 = the GL16 default. The 0x80000 "global" flag from
// the turn mode is intentionally dropped.
static uint32_t nds_sunxi_strip_mode() {
    uint32_t wf = nds_strip_wf();
    if (wf == 0) wf = 0x20;            // GL16: fast + full greyscale
    return (wf & 0xFFFu) | 0x400u;     // | RECT
}

// ---- sunxi (AllWinner B300: Elipsa/Sage) band-wipe --------------------------------------
// A very different interface from hwtcon/mxcfb, but *simpler* for our purposes:
//   - the update arg is a `ubuffer[7]` and ub[0] is a *pointer* to an area_info
//     {x_top,y_top,x_bottom,y_bottom} (inclusive bbox), ub[2] is the update_mode.
//   - the wait is generic (DISP_EINK_WAIT_FRAME_SYNC_COMPLETE) — NOT keyed on a per-update marker.
// So there is no marker contract to preserve: we just re-issue N partial UPDATE2s, each a copy of
// Nickel's ubuffer with ub[0] pointed at our own strip rect. Nickel's own frame-sync wait then
// covers all of them. We return the last strip's frame_id so any read of it stays valid.
static int nds_do_sweep_sunxi(int fd, unsigned long request, void *argp) {
    uint32_t *ub = (uint32_t *)argp;
    const uint32_t area_ptr = ub[0];
    if (area_ptr < 0x1000u || area_ptr >= 0xC0000000u) return real_ioctl(fd, request, argp);
    const uint32_t *a = (const uint32_t *)(uintptr_t)area_ptr;
    const uint32_t x0 = a[0], y0 = a[1], x1 = a[2], y1 = a[3];   // inclusive bbox
    const uint32_t W = (x1 >= x0) ? (x1 - x0 + 1) : 0;
    const int N = nds_strips();
    const uint32_t sw = (N > 0) ? W / (uint32_t)N : 0;
    if (sw == 0) return real_ioctl(fd, request, argp);
    const int delay = nds_delay_us(0);       // sunxi is already slow on the big panel; no extra default
    bool rtl = nds_rtl();
    if (nds_turn_dir == 1) rtl = !rtl;                          // backward turn sweeps the opposite way

    uint32_t myub[8];
    memcpy(myub, ub, 7 * sizeof(uint32_t));                     // copy Nickel's ubuffer (7 longs)
    myub[2] = nds_sunxi_strip_mode();                           // fast partial waveform (REAGL too slow)
    uint32_t strip[4];                                          // our own area_info; ub[0] points here
    int rv = 0;
    for (int k = 0; k < N; k++) {
        int col = rtl ? (N - 1 - k) : k;
        uint32_t left  = x0 + (uint32_t)col * sw;
        uint32_t right = (col == N - 1) ? x1 : (left + sw - 1); // last column takes the remainder
        strip[0] = left; strip[1] = y0; strip[2] = right; strip[3] = y1;
        myub[0] = (uint32_t)(uintptr_t)strip;                  // ioctl copies *strip synchronously
        int r = real_ioctl(fd, request, myub);
        if (r >= 0) rv = r;                                    // keep the last valid frame_id
        if (delay > 0 && k != N - 1) usleep((useconds_t)delay);
    }
    return rv;
}

// ---- per-gesture gating -------------------------------------------------------------------
// Decide whether this page render should animate, from the per-gesture flags and the last input
// gesture the Qt event filter saw (gesture.cc). `armed` = the render was announced by a
// ReadingView turn hook, which is the path swipes AND physical page-turn buttons take — the
// filter's last input tells them apart. An unarmed render can only be a tap turn (Nickel's tap
// path isn't hookable), so it must be backed by a fresh TAP; the tap's position also gives the
// direction (left half = backward, right half = forward — Kobo's reading tap zones), overriding
// the last swipe's direction. Without the filter (no Qt app yet), fall back to the old
// behaviour: armed renders count as swipes, unarmed ones animate whenever tap turns are enabled.
static bool nds_gesture_animate(bool armed, uint32_t width) {
    long gts = 0; int tap_x = -1;
    enum nds_gesture g = nds_gesture_last(&gts, &tap_x);
    bool fresh = g != NDS_GESTURE_NONE && (nds_now() - gts) <= NDS_TURN_WINDOW_S;

    if (armed) {
        if (fresh && g == NDS_GESTURE_BUTTON) return nds_animate_button();
        if (fresh && g == NDS_GESTURE_TAP) return nds_animate_tap();
        return nds_animate_swipe();
    }
    if (!nds_animate_tap()) return false;
    if (!nds_gesture_filter_installed()) return true;   // legacy: any full-screen page render
    if (!fresh || g != NDS_GESTURE_TAP) return false;   // not caused by a recent tap
    if (tap_x >= 0 && width > 0) nds_turn_dir = ((uint32_t)tap_x >= width / 2u) ? 0 : 1;
    return true;
}

// ---- ioctl hook -------------------------------------------------------------------------
extern "C" __attribute__((visibility("default")))
int _nds_ioctl(int fd, unsigned long request, void *argp) {
    const struct nds_platform *plat = nds_match(request);
    if (plat && argp) nds_active = plat;

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
        // Armed by a swipe/button (turn hook) — or, when tap turns animate, by any full-screen
        // page render (Nickel's tap path isn't hookable; nds_gesture_animate narrows it down).
        if ((nds_turn_pending || nds_animate_tap()) && w >= 512 && h >= 512) {
            bool armed = nds_turn_pending != 0;
            nds_turn_pending = 0;                           // consume the trigger (one per turn)
            // A flash is a GC16 update (hwtcon) or, on i.MX where the reading turn is PARTIAL, any
            // update_mode==FULL refresh (the AUTO/GC16 chapter/ghost-clear).
            uint32_t wf = rd32(u, OFF_WAVEFORM), md = rd32(u, OFF_UPDMODE);
            bool is_flash = (wf == WF_GC16) || (plat->flash_full && md == UPD_FULL);
            // Book-open / chapter-jump settle (see nds_flash_seq): a full-screen flash records the
            // gesture that caused it; the next full-screen content render from that SAME gesture is
            // the open/jump settle and must not animate. A later turn (new gesture) animates, so
            // turn-1's own GC16 flash does not suppress turn-2. Only unarmed flashes arm this — an
            // armed (swipe/button) ghost-clear is a real turn, not an open.
            long gseq = nds_gesture_seq();
            bool settle = false;
            if (is_flash) {
                if (!armed) { nds_flash_seq = gseq; nds_flash_ts = nds_now(); }
            } else {
                settle = (nds_flash_seq >= 0 && gseq == nds_flash_seq && (nds_now() - nds_flash_ts) <= NDS_SETTLE_WINDOW_S);
                nds_flash_seq = -1;   // consume: only the first full render after a flash can settle
            }
            // First turn after a book open: Kobo forces GC16 on it. nds_first_turn_pending was armed
            // by the open settle; if this is that turn's GC16, animate it as a Regal sweep instead.
            // Converting bypasses the waveform gate (which normally keeps home/menus from sweeping),
            // so we only do it when we KNOW we're in a book: either the render is armed (a
            // *PageWithTimer forward/back turn) or the ReadingView is on screen. Home / menu / opening
            // another book are neither armed nor in-reader, so they can't be mistaken for the first
            // turn even while the flag is still pending. Using the reader flag (not just armed) also
            // lets an unarmed tap first-turn convert.
            bool convert_first_turn = nds_first_turn_pending && (armed || nds_reader_is_open())
                                      && is_flash && wf == WF_GC16 && nds_animate_first_turn();
            // Sweep only a genuine B&W reading turn whose gesture is enabled. The waveform gate is
            // the colour guard AND the reader-context guard in one: colour pages get a colour
            // waveform, flashes/menus/home get GC16/A2/AUTO — none are GL16/GLR16, so none sweep.
            if (!nds_gesture_animate(armed, w)) {
                nds_first_turn_pending = 0;
                // Gesture disabled, or the render wasn't caused by a fresh tap: pass through.
                // Log why (verbose), so a "tap didn't animate" report is diagnosable from a log:
                // armed=0 gesture=NONE means the filter never classified the tap (the turn likely
                // fired before the touch release); gesture=SWIPE means it was mis-thresholded.
                if (nds_verbose_enabled) {
                    static int n = 0;
                    if (n < 40) { n++;
                        long gts = 0; int tx = -1;
                        enum nds_gesture g = nds_gesture_last(&gts, &tx);
                        NDS_LOG("no-anim [%s] %ux%u armed=%d gesture=%s age=%lds tap_x=%d filter=%d animate_tap=%d -> passthrough",
                                plat->name, w, h, armed, nds_gesture_name(g), nds_now() - gts, tx,
                                nds_gesture_filter_installed(), nds_animate_tap());
                    }
                }
            } else if (convert_first_turn) {
                nds_first_turn_pending = 0;
                uint8_t conv[128];
                if (plat->size <= sizeof(conv)) {
                    memcpy(conv, u, plat->size);
                    wr32(conv, OFF_WAVEFORM, plat->read_wf);   // GC16 flash -> flashless Regal
                    bool rtl = nds_rtl(); if (nds_turn_dir == 1) rtl = !rtl;
                    if (nds_verbose_enabled)
                        NDS_LOG("FIRST-TURN [%s] %ux%u armed=%d reader_open=%d: forced GC16 -> wf=%u(%s) sweep %s (animate first turn)",
                                plat->name, w, h, armed, nds_reader_is_open(), plat->read_wf, nds_wf_name(plat->read_wf), rtl ? "R->L" : "L->R");
                    nds_do_sweep(fd, plat, conv);
                    return 0;
                }
                // update larger than our buffer: leave it as the original GC16 flash (passthrough)
            } else if (is_flash || !nds_wf_sweepable(plat, wf)) {
                nds_first_turn_pending = 0;
                static int cskip = 0;
                if (nds_verbose_enabled && cskip < 40 && !is_flash) { cskip++;
                    NDS_LOG("SKIP [%s] %ux%u wf=%u(%s) mode=%s dither=0x%x flags=0x%x -> not a B&W reading turn",
                            plat->name, w, h, wf, nds_wf_name(wf), md == UPD_FULL ? "FULL" : "PARTIAL",
                            nds_dither(u, plat), rd32(u, plat->flags_off));
                }
            } else if (settle) {
                nds_first_turn_pending = 1;   // the next full-screen turn is the first turn after the open
                static int nset = 0;
                if (nds_verbose_enabled && nset < 40) { nset++;
                    NDS_LOG("SETTLE [%s] %ux%u wf=%u(%s) -> render right after a full-screen flash (book open / chapter / ghost-clear), not animated",
                            plat->name, w, h, wf, nds_wf_name(wf));
                }
            } else {
                nds_first_turn_pending = 0;
                bool rtl = nds_rtl(); if (nds_turn_dir == 1) rtl = !rtl;
                static int swept = 0;
                if (nds_verbose_enabled && swept < 40) { swept++;
                    NDS_LOG("SWEEP [%s] %ux%u wf=%u(%s) dither=0x%x flags=0x%x -> %d strips %s cfa_skip=%d", plat->name, w, h,
                            wf, nds_wf_name(wf), nds_dither(u, plat), rd32(u, plat->flags_off), nds_strips(), rtl ? "R->L" : "L->R",
                            (plat->cfa_skip && nds_cfa_skip()) ? 1 : 0);
                }
                nds_do_sweep(fd, plat, u);
                return 0;   // suppress the original; Nickel's WAIT_COMPLETE(M) resolves via the last strip
            }
        }
    }

    // sunxi (Elipsa/Sage) sweep: turn-armed, full-screen UPDATE2 at the reading-turn mode.
    // No platform-table match here (0x0406 isn't an 'F'-magic ioctl); gate on the ioctl number.
    // sunxi never sweeps by default (not recommended on those panels): explicit opt-in only.
    if (!nds_safety_disabled && nds_sweep_explicit() && real_ioctl && argp &&
            request == NDS_DISP_EINK_UPDATE2) {
        if (nds_turn_pending && (nds_now() - nds_turn_ts) > NDS_TURN_WINDOW_S) nds_turn_pending = 0;
        const uint32_t *ub = (const uint32_t *)argp;
        const uint32_t area_ptr = ub[0], mode = ub[2];
        // Armed by a swipe/button (turn hook) — or, when tap turns animate, by any full-screen
        // turn render (nds_gesture_animate narrows it down).
        if ((nds_turn_pending || nds_animate_tap()) && mode == NDS_SUNXI_TURN_MODE &&
                area_ptr >= 0x1000u && area_ptr < 0xC0000000u) {
            const uint32_t *a = (const uint32_t *)(uintptr_t)area_ptr;
            uint32_t w = (a[2] >= a[0]) ? a[2] - a[0] + 1 : 0, h = (a[3] >= a[1]) ? a[3] - a[1] + 1 : 0;
            if (w >= 512 && h >= 512) {                       // full-screen page render
                bool armed = nds_turn_pending != 0;
                nds_turn_pending = 0;                         // consume the trigger (one per turn)
                if (nds_gesture_animate(armed, w)) {
                    bool rtl = nds_rtl(); if (nds_turn_dir == 1) rtl = !rtl;
                    static int sswept = 0;
                    if (nds_verbose_enabled && sswept < 40) { sswept++;
                        NDS_LOG("SWEEP [sunxi] %ux%u turn_mode=0x%x -> %d strips @ mode=0x%x %s", w, h, mode,
                                nds_strips(), nds_sunxi_strip_mode(), rtl ? "R->L" : "L->R");
                    }
                    return nds_do_sweep_sunxi(fd, request, argp);  // return the last strip's frame_id
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
                        owf, nds_wf_name(owf), rd32(u, OFF_UPDMODE) == UPD_FULL ? "FULL" : "PARTIAL", nds_dither(u, plat), rd32(u, plat->flags_off));
            } else if (nds_active && argp && request == nds_active->wait_cmpl) {
                NDS_LOG("ioctl #%d WAIT_COMPLETE marker=%u", logged, *(const uint32_t *)argp);
            } else if (nds_active && argp && nds_active->wait_sub && request == nds_active->wait_sub) {
                NDS_LOG("ioctl #%d WAIT_SUBMISSION marker=%u", logged, *(const uint32_t *)argp);
            }
        }
    }

    // observe (sunxi/AllWinner): log the DISP_EINK update stream (Elipsa/Sage). Passthrough unchanged.
    if (!nds_off() && !nds_safety_disabled && nds_verbose_enabled &&
            (request == NDS_DISP_EINK_UPDATE || request == NDS_DISP_EINK_UPDATE2 || request == NDS_DISP_EINK_WAIT)) {
        static int slog = 0;
        if (slog < 120) { slog++;
            if (request == NDS_DISP_EINK_WAIT) {
                NDS_LOG("sunxi #%d WAIT_FRAME_SYNC", slog);
            } else if (argp) {
                const uint32_t *ub = (const uint32_t *)argp;   // ubuffer[0..6] (7 longs)
                uint32_t area_ptr = ub[0];
                const char *kind = (request == NDS_DISP_EINK_UPDATE2) ? "UPDATE2" : "UPDATE";
                // area is a *pointer* to {x_top,y_top,x_bottom,y_bottom}; deref only if it looks like a userspace ptr
                if (area_ptr >= 0x1000u && area_ptr < 0xC0000000u) {
                    const uint32_t *a = (const uint32_t *)(uintptr_t)area_ptr;
                    NDS_LOG("sunxi #%d %s area=(%u,%u -> %u,%u) mode=0x%x ub=[%x %x %x %x %x %x %x]", slog, kind,
                            a[0], a[1], a[2], a[3], ub[2], ub[0], ub[1], ub[2], ub[3], ub[4], ub[5], ub[6]);
                } else {
                    NDS_LOG("sunxi #%d %s (area@0x%x not deref'd) mode=0x%x ub=[%x %x %x %x %x %x %x]", slog, kind,
                            area_ptr, ub[2], ub[0], ub[1], ub[2], ub[3], ub[4], ub[5], ub[6]);
                }
            }
        }
    }

    if (real_ioctl) return real_ioctl(fd, request, argp);
    if (!nds_safety_disabled) { nds_safety_disabled = true; NDS_LOG("SAFETY: real ioctl NULL"); }
    return -1;
}

// ---- page-turn hooks: arm the sweep + record direction ----------------------------------
// These run on the UI thread, so they double as a safe retry point for installing the gesture
// filter in case the Qt application didn't exist yet at plugin init.
extern "C" __attribute__((visibility("default")))
void _nds_nextPageWithTimer(void *self) {
    nds_gesture_filter_install();
    nds_turn_dir = 0; nds_turn_pending = 1; nds_turn_ts = nds_now();
    static int n = 0; if (nds_verbose_enabled && n < 16) { n++; NDS_LOG("turn: forward"); }
    if (real_nextPageWithTimer) real_nextPageWithTimer(self);
}
extern "C" __attribute__((visibility("default")))
void _nds_prevPageWithTimer(void *self) {
    nds_gesture_filter_install();
    nds_turn_dir = 1; nds_turn_pending = 1; nds_turn_ts = nds_now();
    static int n = 0; if (nds_verbose_enabled && n < 16) { n++; NDS_LOG("turn: backward"); }
    if (real_prevPageWithTimer) real_prevPageWithTimer(self);
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
    // The plugin normally loads with the Qt application already up (Qt scans imageformats
    // plugins on the main thread), so this usually succeeds right here; if not, the page-turn
    // hooks retry, and tap turns fall back to the legacy any-big-render behaviour until then.
    bool gf = nds_gesture_filter_install();
    NDS_LOG("startup: mode=%s strips=%d dir=%s delay=%s animate(swipe/tap/button)=%d/%d/%d gesture_filter=%d verbose=%d "
            "| debug: strip_wf=%u(0=keep) wait=%s cfa_skip=%d force_bw=%d sweep_any_wf=%d",
            nds_off() ? "off" : (nds_sweep_mode() ? "SWEEP" : "observe"), nds_strips(),
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
    //libnickel 4.6.9960 * _ZN11ReadingView17nextPageWithTimerEv
    { .sym = "_ZN11ReadingView17nextPageWithTimerEv", .sym_new = "_nds_nextPageWithTimer",
      .lib = NDS_LIBNICKEL, .out = nh_symoutptr(real_nextPageWithTimer),
      .desc = "arm sweep, forward", .optional = true },
    //libnickel 4.6.9960 * _ZN11ReadingView17prevPageWithTimerEv
    { .sym = "_ZN11ReadingView17prevPageWithTimerEv", .sym_new = "_nds_prevPageWithTimer",
      .lib = NDS_LIBNICKEL, .out = nh_symoutptr(real_prevPageWithTimer),
      .desc = "arm sweep, backward", .optional = true },
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
    {0},
};

NickelHook(
    .init      = &nds_init,
    .info      = &NickelDissolveInfo,
    .hook      = &NickelDissolveHooks[0],
    .dlsym     = &NickelDissolveDlsym[0],
    .uninstall = &nds_uninstall,
)
