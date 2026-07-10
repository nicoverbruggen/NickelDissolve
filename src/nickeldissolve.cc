// NickelDissolve — a page-turn "band wipe" PoC for the Kobo Clara BW (MTK/hwtcon).
//
// The Kindle's smooth wipe is a native MediaTek mxcfb swipe primitive the Kobo hwtcon driver
// does NOT expose. The only approximation from userspace: take the ONE full-screen update Nickel
// issues per page turn and replace it with a SEQUENCE of partial updates over vertical strips,
// swept across the screen, so the new page appears strip-by-strip. We use only the driver's
// public ioctl interface (HWTCON_SEND_UPDATE) — no driver patching.
//
// Observed page-turn ioctl sequence (from the observe build):
//   SEND_UPDATE  marker=M  region=(0,0, 1072x1448)  wf=4(GLR16)  mode=FULL
//   WAIT_FOR_UPDATE_SUBMISSION
//   WAIT_FOR_UPDATE_COMPLETE  marker=M
// So a turn is a single full-screen GLR16 FULL update Nickel then blocks on (marker M).
//
// Injection (mode:sweep): when we see that full-screen GLR16 FULL update we DON'T forward it;
// instead we issue N partial strip updates sweeping across, waiting for submission between each
// (to beat the driver's MDP merge). The LAST strip reuses Nickel's original marker M, so its
// subsequent WAIT_FOR_UPDATE_COMPLETE(M) resolves normally — no hang. Then we return 0 (success).
//
// SAFETY: default mode is "observe" (pure passthrough + logging — installing changes nothing).
// Only full-screen GLR16 FULL updates are ever touched; everything else passes through byte-for-
// byte. Worst case of a bad sweep is a garbled frame fixed by the next full refresh — recoverable,
// not a brick. Hook is `optional`. Revert with nds_mode:observe or delete the uninstall file.

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include <NickelHook.h>

#include "config.h"
#include "util.h"

static const char *const NDS_LIBKOBO   = "/usr/local/Kobo/platforms/libkobo.so";
static const char *const NDS_LIBNICKEL = "/usr/local/Kobo/libnickel.so.1.0.0";

// ---- hwtcon ioctl interface (FBInk eink/mtk-kobo.h + KOReader, verified vs the on-device log) ----
#define NDS_HWTCON_SEND_UPDATE                0x4024462EUL // _IOW('F',0x2E, hwtcon_update_data[36])
#define NDS_HWTCON_WAIT_FOR_UPDATE_COMPLETE   0xC008462FUL // _IOWR('F',0x2F, marker_data[8])
#define NDS_HWTCON_WAIT_FOR_UPDATE_SUBMISSION 0x40044637UL // _IOW('F',0x37, uint32_t)  (corrected)
#define NDS_IOC_MAGIC(req) (((req) >> 8) & 0xFFU)
#define NDS_IOC_NR(req)    ((req) & 0xFFU)

enum { WF_DU = 1, WF_GC16 = 2, WF_GL16 = 3, WF_GLR16 = 4, WF_A2 = 6 };
enum { UPD_PARTIAL = 0, UPD_FULL = 1 };

struct nds_hwtcon_rect { uint32_t top, left, width, height; };
struct nds_hwtcon_update_data {
    struct nds_hwtcon_rect update_region;
    uint32_t waveform_mode;
    uint32_t update_mode;   // 0 = PARTIAL, 1 = FULL
    uint32_t update_marker;
    uint32_t flags;
    int32_t  dither_mode;
};
struct nds_hwtcon_marker_data { uint32_t update_marker; uint32_t collision_test; };

static const char *wf_name(uint32_t m) {
    switch (m) { case WF_DU: return "DU"; case WF_GC16: return "GC16"; case WF_GL16: return "GL16";
                 case WF_GLR16: return "GLR16"; case WF_A2: return "A2"; default: return "?"; }
}

static int (*real_waveformModeFromQt)(void *self, uint64_t flags) = nullptr;
static int (*real_ioctl)(int fd, unsigned long request, void *argp) = nullptr;
static void (*real_nextPageWithTimer)(void *self) = nullptr;
static void (*real_prevPageWithTimer)(void *self) = nullptr;

static bool nds_safety_disabled = false;
static int  nds_turn_dir = 0;   // 0 = forward (next page), 1 = backward (prev page)
static uint32_t nds_ephemeral_marker = 0xF0000000u;   // for the non-final strips (won't collide with Nickel's)

// ---- config -----------------------------------------------------------------------------
static bool nds_enabled()     { return nds_global_config_bool("nds_enabled", true); }
static bool nds_log_ioctl()   { return nds_global_config_bool("nds_log_ioctl", true); }
static bool nds_sweep_mode()  {
    const char *m = nds_global_config_get("nds_mode");
    return m && !strcasecmp(m, "sweep");
}
static int nds_strips() {
    const char *v = nds_global_config_get("nds_strips");
    int n = (v && *v) ? atoi(v) : 8;
    if (n < 2) n = 2; if (n > 32) n = 32;
    return n;
}
static uint32_t nds_strip_wf() {
    const char *v = nds_global_config_get("nds_strip_waveform");
    int w = (v && *v) ? atoi(v) : WF_DU;   // default DU: fast, makes the sweep obvious (2-level)
    if (w == WF_DU || w == WF_GL16 || w == WF_GLR16 || w == WF_A2) return (uint32_t)w;
    return WF_DU;
}
static bool nds_rtl() {  // right-to-left (Kindle direction) by default
    const char *d = nds_global_config_get("nds_direction");
    return !(d && !strcasecmp(d, "ltr"));
}
static bool nds_wait_complete() {  // wait for full completion between strips (slower, more discrete)
    const char *w = nds_global_config_get("nds_wait");
    return w && !strcasecmp(w, "complete");
}
static int nds_delay_us() {
    const char *v = nds_global_config_get("nds_delay_us");
    int d = (v && *v) ? atoi(v) : 0;
    if (d < 0) d = 0; if (d > 50000) d = 50000;
    return d;
}

// ---- the sweep: replace one full-screen update with N swept partial strips ----------------
static void nds_do_sweep(int fd, const struct nds_hwtcon_update_data *orig) {
    const uint32_t M = orig->update_marker;
    const int N = nds_strips();
    const uint32_t x0 = orig->update_region.left, y0 = orig->update_region.top;
    const uint32_t W  = orig->update_region.width, H = orig->update_region.height;
    const uint32_t sw = W / (uint32_t)N;
    const uint32_t wf = nds_strip_wf();
    bool rtl = nds_rtl();
    if (nds_turn_dir == 1) rtl = !rtl;   // a backward (prev-page) turn sweeps the opposite way
    const int delay = nds_delay_us();

    bool m_submitted = false;   // Nickel will WAIT_COMPLETE(M); M MUST get submitted or it hangs.
    for (int k = 0; k < N; k++) {
        int col = rtl ? (N - 1 - k) : k;                 // k = render order; col = screen column
        uint32_t left  = x0 + (uint32_t)col * sw;
        uint32_t width = (col == N - 1) ? (x0 + W - left) : sw;   // last column takes the remainder
        bool last = (k == N - 1);

        struct nds_hwtcon_update_data v = *orig;
        v.update_region.top = y0; v.update_region.left = left;
        v.update_region.width = width; v.update_region.height = H;
        v.update_mode   = UPD_PARTIAL;
        v.waveform_mode = wf;
        v.update_marker = last ? M : (nds_ephemeral_marker + (uint32_t)k);

        if (real_ioctl(fd, NDS_HWTCON_SEND_UPDATE, &v) < 0) continue;
        if (last) m_submitted = true;

        if (nds_wait_complete()) {
            struct nds_hwtcon_marker_data md = { v.update_marker, 0 };
            real_ioctl(fd, NDS_HWTCON_WAIT_FOR_UPDATE_COMPLETE, &md);
        } else {
            uint32_t wm = v.update_marker;
            real_ioctl(fd, NDS_HWTCON_WAIT_FOR_UPDATE_SUBMISSION, &wm);
        }
        if (delay > 0 && !last) usleep((useconds_t)delay);
    }
    // Hang-safety: if the marker-M strip never submitted, submit the ORIGINAL full update so
    // Nickel's WAIT_COMPLETE(M) resolves (falls back to a normal full refresh, not the wipe).
    if (!m_submitted) real_ioctl(fd, NDS_HWTCON_SEND_UPDATE, (void *)orig);
    nds_ephemeral_marker += (uint32_t)N;   // keep ephemeral markers moving (still far from Nickel's)
}

// ---- ioctl hook -------------------------------------------------------------------------
extern "C" __attribute__((visibility("default")))
int _nds_ioctl(int fd, unsigned long request, void *argp) {
    if (nds_enabled() && !nds_safety_disabled && request == NDS_HWTCON_SEND_UPDATE && argp && real_ioctl) {
        const struct nds_hwtcon_update_data *u = (const struct nds_hwtcon_update_data *)argp;
        // Only a full-screen GLR16 FULL update is a reading page turn. Sweep only that.
        if (nds_sweep_mode() && u->update_mode == UPD_FULL && u->waveform_mode == WF_GLR16
                && u->update_region.width >= 512 && u->update_region.height >= 512) {
            static int swept = 0;
            if (nds_log_ioctl() && swept < 40) { swept++;
                NDS_LOG("SWEEP turn marker=%u %ux%u -> %d strips wf=%u(%s) %s",
                        u->update_marker, u->update_region.width, u->update_region.height,
                        nds_strips(), nds_strip_wf(), wf_name(nds_strip_wf()), nds_rtl() ? "R->L" : "L->R");
            }
            nds_do_sweep(fd, u);
            return 0;   // suppress the original full update; Nickel's WAIT_COMPLETE(M) resolves via the last strip
        }
    }

    // observe: log the hwtcon ioctl stream (first 160), passthrough unchanged
    if (nds_enabled() && !nds_safety_disabled && nds_log_ioctl() && NDS_IOC_MAGIC(request) == 0x46) {
        static int logged = 0;
        if (logged < 160) { logged++;
            if (request == NDS_HWTCON_SEND_UPDATE && argp) {
                const struct nds_hwtcon_update_data *u = (const struct nds_hwtcon_update_data *)argp;
                NDS_LOG("ioctl #%d SEND_UPDATE marker=%u region=(t%u,l%u,%ux%u) wf=%u(%s) mode=%s",
                        logged, u->update_marker, u->update_region.top, u->update_region.left,
                        u->update_region.width, u->update_region.height,
                        u->waveform_mode, wf_name(u->waveform_mode), u->update_mode == UPD_FULL ? "FULL" : "PARTIAL");
            } else if (request == NDS_HWTCON_WAIT_FOR_UPDATE_COMPLETE && argp) {
                NDS_LOG("ioctl #%d WAIT_COMPLETE marker=%u", logged, *(const uint32_t *)argp);
            } else if (request == NDS_HWTCON_WAIT_FOR_UPDATE_SUBMISSION && argp) {
                NDS_LOG("ioctl #%d WAIT_SUBMISSION marker=%u", logged, *(const uint32_t *)argp);
            }
        }
    }

    if (real_ioctl) return real_ioctl(fd, request, argp);
    if (!nds_safety_disabled) { nds_safety_disabled = true; NDS_LOG("SAFETY: real ioctl NULL"); }
    return -1;
}

// ---- waveform hook (log-only context; cheap) --------------------------------------------
extern "C" __attribute__((visibility("default")))
int _nds_waveformModeFromQt(void *self, uint64_t flags) {
    return real_waveformModeFromQt ? real_waveformModeFromQt(self, flags) : WF_GC16;
}

// ---- page-turn direction: record which way the turn goes so the sweep can match ----------
// The ioctl update is identical for forward/back turns, so we learn direction here (called on
// the same thread, just before the update). Forward => configured direction; back => opposite.
extern "C" __attribute__((visibility("default")))
void _nds_nextPageWithTimer(void *self) {
    nds_turn_dir = 0;
    static int n = 0; if (nds_log_ioctl() && n < 16) { n++; NDS_LOG("turn: forward"); }
    if (real_nextPageWithTimer) real_nextPageWithTimer(self);
}
extern "C" __attribute__((visibility("default")))
void _nds_prevPageWithTimer(void *self) {
    nds_turn_dir = 1;
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
    NDS_LOG("startup: enabled=%d mode=%s strips=%d strip_wf=%u(%s) dir=%s wait=%s delay=%dus ioctl=%p",
            nds_enabled(), nds_sweep_mode() ? "SWEEP" : "observe", nds_strips(),
            nds_strip_wf(), wf_name(nds_strip_wf()), nds_rtl() ? "R->L" : "L->R",
            nds_wait_complete() ? "complete" : "submission", nds_delay_us(), (void *)real_ioctl);
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
    .desc            = "Page-turn band-wipe PoC (hwtcon partial-update sweep).",
    .uninstall_flag  = NDS_CONFIG_DIR "/uninstall-now",
    .uninstall_xflag = NDS_CONFIG_DIR "/uninstall",
    .failsafe_delay  = 3,
};
static struct nh_hook NickelDissolveHooks[] = {
    { .sym = "ioctl", .sym_new = "_nds_ioctl",
      .lib = NDS_LIBKOBO, .out = nh_symoutptr(real_ioctl),
      .desc = "intercept the HWTCON page-turn update and sweep it", .optional = true },
    { .sym = "_ZNK13KoboScreenMTK18waveformModeFromQtEy", .sym_new = "_nds_waveformModeFromQt",
      .lib = NDS_LIBKOBO, .out = nh_symoutptr(real_waveformModeFromQt),
      .desc = "waveform context (log-only)", .optional = true },
    { .sym = "_ZN11ReadingView17nextPageWithTimerEv", .sym_new = "_nds_nextPageWithTimer",
      .lib = NDS_LIBNICKEL, .out = nh_symoutptr(real_nextPageWithTimer),
      .desc = "mark forward page turn", .optional = true },
    { .sym = "_ZN11ReadingView17prevPageWithTimerEv", .sym_new = "_nds_prevPageWithTimer",
      .lib = NDS_LIBNICKEL, .out = nh_symoutptr(real_prevPageWithTimer),
      .desc = "mark backward page turn", .optional = true },
    {0},
};

NickelHook(
    .init      = &nds_init,
    .info      = &NickelDissolveInfo,
    .hook      = &NickelDissolveHooks[0],
    .dlsym     = NULL,
    .uninstall = &nds_uninstall,
)
