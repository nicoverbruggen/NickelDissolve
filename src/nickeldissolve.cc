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
// SAFETY. Default mode is "observe" (pure passthrough + logging). In "sweep", only a full-screen
// update that immediately follows a page turn is ever touched; everything else passes byte-for-byte.
// Worst case of a bad sweep is a garbled frame fixed by the next refresh — recoverable, not a brick.

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

// ---- platform table: which ioctls carry an e-ink update, per driver ----
struct nds_platform {
    const char   *name;
    unsigned long send;        // *_SEND_UPDATE (intercept + reissue partial strips)
    unsigned long wait_sub;    // *_WAIT_FOR_UPDATE_SUBMISSION (0 = none, e.g. mxcfb)
    unsigned long wait_cmpl;   // *_WAIT_FOR_UPDATE_COMPLETE (for logging / optional per-strip wait)
    uint32_t      size;        // bytes to copy when reissuing an update
    uint32_t      flags_off;   // byte offset of the `flags` field in the update struct
    uint32_t      cfa_skip;    // CFA-skip flag bit (skips the per-region colour pass; 0 = n/a)
};
static const struct nds_platform NDS_PLATFORMS[] = {
    // MTK (Clara BW/Colour, Libra Colour): HWTCON flags@28, HWTCON_FLAG_CFA_SKIP = 0x8000
    { "hwtcon", 0x4024462EUL, 0x40044637UL, 0xC008462FUL, 36, 28, 0x8000UL },
    // i.MX (Libra 2 & most pre-2024): mxcfb flags@32, no CFA
    { "mxcfb",  0x4048462EUL, 0UL,          0xC008462FUL, 72, 32, 0UL },
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
static int  nds_turn_dir = 0;                 // 0 = forward (next), 1 = backward (prev)
static volatile int  nds_turn_pending = 0;    // a page turn just fired; sweep its next full update
static volatile long nds_turn_ts = 0;         // time() when the turn fired (staleness guard)
static uint32_t nds_ephemeral_marker = 0xF0000000u;   // markers for non-final strips (never collide with Nickel)
static const struct nds_platform *nds_active = nullptr;   // detected from the first update ioctl (for logging)

static long nds_now() { return (long)time(nullptr); }

// ---- config -----------------------------------------------------------------------------
static bool nds_enabled()     { return nds_global_config_bool("nds_enabled", true); }
static bool nds_log_ioctl()   { return nds_global_config_bool("nds_log_ioctl", true); }
static bool nds_sweep_mode()  { const char *m = nds_global_config_get("nds_mode"); return m && !strcasecmp(m, "sweep"); }
static int  nds_strips()      { const char *v = nds_global_config_get("nds_strips"); int n = (v && *v) ? atoi(v) : 8; if (n < 2) n = 2; if (n > 32) n = 32; return n; }
static bool nds_rtl()         { const char *d = nds_global_config_get("nds_direction"); return !(d && !strcasecmp(d, "ltr")); }
static bool nds_wait_complete(){ const char *w = nds_global_config_get("nds_wait"); return w && !strcasecmp(w, "complete"); }
static int  nds_delay_us()    { const char *v = nds_global_config_get("nds_delay_us"); int d = (v && *v) ? atoi(v) : 0; if (d < 0) d = 0; if (d > 50000) d = 50000; return d; }
// Set HWTCON_FLAG_CFA_SKIP on the strips (Kaleido): skips the per-region CFA colour pass whose
// region boundaries produce the seams. Correct for B&W content (no colour to convert).
static bool nds_cfa_skip()    { return nds_global_config_bool("nds_cfa_skip", true); }
// 0 = keep the turn's own waveform (platform-agnostic default); >0 = force a raw waveform id
// (PLATFORM-SPECIFIC — e.g. GLR16 is 4 on hwtcon but 6 on mxcfb; use with care).
static uint32_t nds_strip_wf() { const char *v = nds_global_config_get("nds_strip_waveform"); if (!v || !*v) return 0; int w = atoi(v); return w > 0 ? (uint32_t)w : 0; }

// ---- the sweep: replace one full-screen update with N swept partial strips ----------------
static void nds_do_sweep(int fd, const struct nds_platform *plat, const uint8_t *orig) {
    const uint32_t M  = rd32(orig, OFF_MARKER);
    const uint32_t x0 = rd32(orig, OFF_LEFT), y0 = rd32(orig, OFF_TOP);
    const uint32_t W  = rd32(orig, OFF_WIDTH), H = rd32(orig, OFF_HEIGHT);
    const int N = nds_strips();
    const uint32_t sw = W / (uint32_t)N;
    const uint32_t wf_override = nds_strip_wf();     // 0 = keep original
    const int delay = nds_delay_us();
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

// ---- ioctl hook -------------------------------------------------------------------------
extern "C" __attribute__((visibility("default")))
int _nds_ioctl(int fd, unsigned long request, void *argp) {
    const struct nds_platform *plat = nds_match(request);
    if (plat && argp) nds_active = plat;

    if (plat && argp && real_ioctl && nds_enabled() && !nds_safety_disabled && nds_sweep_mode()) {
        // Expire a stale pending-turn so we never sweep an unrelated later full-screen update.
        if (nds_turn_pending && (nds_now() - nds_turn_ts) > NDS_TURN_WINDOW_S) nds_turn_pending = 0;

        const uint8_t *u = (const uint8_t *)argp;
        uint32_t w = rd32(u, OFF_WIDTH), h = rd32(u, OFF_HEIGHT);
        if (nds_turn_pending && w >= 512 && h >= 512) {     // the page-turn update
            nds_turn_pending = 0;                           // consume the trigger (one per turn)
            // Only animate FLASHLESS turns. A GC16 update is Nickel's full black-flash ghost-clear;
            // leave it alone so we never turn a flashless turn into a flash.
            if (rd32(u, OFF_WAVEFORM) != WF_GC16) {
                bool rtl = nds_rtl(); if (nds_turn_dir == 1) rtl = !rtl;
                static int swept = 0;
                if (nds_log_ioctl() && swept < 40) { swept++;
                    NDS_LOG("SWEEP [%s] %ux%u wf=%u flags=0x%x -> %d strips %s cfa_skip=%d", plat->name, w, h,
                            rd32(u, OFF_WAVEFORM), rd32(u, plat->flags_off), nds_strips(), rtl ? "R->L" : "L->R",
                            (plat->cfa_skip && nds_cfa_skip()) ? 1 : 0);
                }
                nds_do_sweep(fd, plat, u);
                return 0;   // suppress the original; Nickel's WAIT_COMPLETE(M) resolves via the last strip
            }
        }
    }

    // observe: log the e-ink ioctl stream (first 160), passthrough unchanged
    if (nds_enabled() && !nds_safety_disabled && nds_log_ioctl() && NDS_IOC_MAGIC(request) == 0x46) {
        static int logged = 0;
        if (logged < 160) { logged++;
            if (plat && argp) {
                const uint8_t *u = (const uint8_t *)argp;
                NDS_LOG("ioctl #%d SEND [%s] marker=%u region=(t%u,l%u,%ux%u) wf=%u mode=%s flags=0x%x", logged, plat->name,
                        rd32(u, OFF_MARKER), rd32(u, OFF_TOP), rd32(u, OFF_LEFT), rd32(u, OFF_WIDTH), rd32(u, OFF_HEIGHT),
                        rd32(u, OFF_WAVEFORM), rd32(u, OFF_UPDMODE) == UPD_FULL ? "FULL" : "PARTIAL", rd32(u, plat->flags_off));
            } else if (nds_active && argp && request == nds_active->wait_cmpl) {
                NDS_LOG("ioctl #%d WAIT_COMPLETE marker=%u", logged, *(const uint32_t *)argp);
            } else if (nds_active && argp && nds_active->wait_sub && request == nds_active->wait_sub) {
                NDS_LOG("ioctl #%d WAIT_SUBMISSION marker=%u", logged, *(const uint32_t *)argp);
            }
        }
    }

    if (real_ioctl) return real_ioctl(fd, request, argp);
    if (!nds_safety_disabled) { nds_safety_disabled = true; NDS_LOG("SAFETY: real ioctl NULL"); }
    return -1;
}

// ---- page-turn hooks: arm the sweep + record direction ----------------------------------
extern "C" __attribute__((visibility("default")))
void _nds_nextPageWithTimer(void *self) {
    nds_turn_dir = 0; nds_turn_pending = 1; nds_turn_ts = nds_now();
    static int n = 0; if (nds_log_ioctl() && n < 16) { n++; NDS_LOG("turn: forward"); }
    if (real_nextPageWithTimer) real_nextPageWithTimer(self);
}
extern "C" __attribute__((visibility("default")))
void _nds_prevPageWithTimer(void *self) {
    nds_turn_dir = 1; nds_turn_pending = 1; nds_turn_ts = nds_now();
    static int n = 0; if (nds_log_ioctl() && n < 16) { n++; NDS_LOG("turn: backward"); }
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
    NDS_LOG("startup: enabled=%d mode=%s strips=%d strip_wf=%u(0=keep) dir=%s wait=%s delay=%dus cfa_skip=%d",
            nds_enabled(), nds_sweep_mode() ? "SWEEP" : "observe", nds_strips(), nds_strip_wf(),
            nds_rtl() ? "R->L" : "L->R", nds_wait_complete() ? "complete" : "submission", nds_delay_us(), nds_cfa_skip());
    return 0;
}
static bool nds_del(const char *p) { return access(p, F_OK) != 0 ? true : nh_delete_file(p); }
static bool nds_uninstall() {
    NDS_LOG("uninstall: removing NickelDissolve files");
    bool ok = true;
    ok = nds_del(NDS_CONFIG_DIR "/doc") && ok;
    ok = nds_del(NDS_CONFIG_DIR "/default") && ok;
    ok = nds_del(NDS_CONFIG_DIR "/config") && ok;
    ok = nds_del(NDS_CONFIG_DIR "/nickeldissolve.log") && ok;
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
    { .sym = "_ZN11ReadingView17nextPageWithTimerEv", .sym_new = "_nds_nextPageWithTimer",
      .lib = NDS_LIBNICKEL, .out = nh_symoutptr(real_nextPageWithTimer),
      .desc = "arm sweep, forward", .optional = true },
    { .sym = "_ZN11ReadingView17prevPageWithTimerEv", .sym_new = "_nds_prevPageWithTimer",
      .lib = NDS_LIBNICKEL, .out = nh_symoutptr(real_prevPageWithTimer),
      .desc = "arm sweep, backward", .optional = true },
    {0},
};

NickelHook(
    .init      = &nds_init,
    .info      = &NickelDissolveInfo,
    .hook      = &NickelDissolveHooks[0],
    .dlsym     = NULL,
    .uninstall = &nds_uninstall,
)
