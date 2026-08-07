// MediaTek `hwtcon` e-ink driver. See driver_hwtcon.h.

#include <cstdint>
#include <cstring>
#include <unistd.h>

#include "config.h"
#include "driver_hwtcon.h"
#include "util.h"

// HWTCON flags@28, HWTCON_FLAG_CFA_SKIP = 0x8000, CFA colour-mode field = HWTCON_FLAG_CFA_FLDS_MASK
// 0x7f00 (G1=0x100, AIE_S4=0x200, ..., NTX=0xa00, NTX_SF=0xb00; 0 = no colour processing).
// Reading turn = FULL + GLR16; flash = FULL + GC16, so a flash is told by its waveform.
// Paced by WAIT_FOR_UPDATE_SUBMISSION between strips, so the default delay is 0.
static const struct nds_platform NDS_HWTCON = {
    "hwtcon", 0x4024462EUL, 0x40044637UL, 0xC008462FUL, 36, 28, 0x8000UL, 0x7f00UL, false, 0, 32, 0, true
};

const struct nds_platform *nds_hwtcon_match(unsigned long request) {
    return request == NDS_HWTCON.send ? &NDS_HWTCON : nullptr;
}

// THE colour guard, validated against libkobo's KoboScreenMTK::handleAutoWfm: it calls fbIsGray(rect)
// and picks a COLOUR waveform (GCC16=10 / GLRC16=11 / GCK16=8 / GLKW16=9) for non-grey content and a
// greyscale one (GLR16=4, GL16=3) for text. So a colour page never reaches us as GL16/GLR16, and
// gating on the waveform skips colour pages, GC16 flashes, AUTO(257) and A2/DU menu updates in one
// test. Because those are also the waveforms the home screen and menus use, it doubles as the
// reader-context guard. Dark mode emits its turn as GLKW16 ("REAGL DARK"), so that sweeps too.
bool nds_hwtcon_wf_sweepable(uint32_t wf) {
    if (nds_global_config_bool("nds_debug_sweep_any_waveform", false)) return wf != WF_GC16;
    return wf == WF_GL16 || wf == WF_GLR16 || wf == WF_GLKW16;
}

const char *nds_hwtcon_wf_name(uint32_t wf) {
    switch (wf) {
        case 0: return "INIT"; case WF_DU: return "DU"; case WF_GC16: return "GC16";
        case WF_GL16: return "GL16"; case WF_GLR16: return "GLR16"; case 6: return "REAGL";
        case WF_GCK16: return "GCK16"; case WF_GLKW16: return "GLKW16"; case WF_GCC16: return "GCC16";
        case WF_GLRC16: return "GLRC16"; case 257: return "AUTO"; default: return "?";
    }
}

// Replace one full-screen update with N swept partial strips.
void nds_hwtcon_sweep(int fd, const struct nds_platform *plat, const uint8_t *orig,
                      const struct nds_sweep_env *env) {
    if (!plat || !orig || !env || !env->real_ioctl) return;

    const uint32_t M  = nds_rd32(orig, OFF_MARKER);
    const uint32_t x0 = nds_rd32(orig, OFF_LEFT), y0 = nds_rd32(orig, OFF_TOP);
    const uint32_t W  = nds_rd32(orig, OFF_WIDTH), H = nds_rd32(orig, OFF_HEIGHT);
    const int N = env->bands < 2 ? 2 : (env->bands > 32 ? 32 : env->bands);
    const uint32_t sw = W / (uint32_t)N;

    uint8_t v[64];
    if (plat->size > sizeof(v) || sw == 0) { env->real_ioctl(fd, plat->send, (void *)orig); return; }

    // Kaleido seam avoidance: the per-region CFA colour pass leaves visible boundaries between
    // strips, so the strips opt out of it. Correct because only B&W reading turns are ever swept.
    const uint32_t cfa_skip = env->cfa_skip ? plat->cfa_skip : 0u;

    // Cold-wake lead-in: if the panel has powered down, the first update after it stalls on the
    // power-on. Spend that stall on a thin sliver at the sweep's leading edge so the freeze is a
    // hairline rather than a full band; the real bands then run warm. The submission wait both
    // absorbs the power-on and stops the driver merging the sliver into band 0.
    if (env->prewarm_ms > 0 && env->idle_any_us >= (uint64_t)env->prewarm_ms * 1000ull) {
        memcpy(v, orig, plat->size);
        const uint32_t lx = env->rtl ? (x0 + W - NDS_PREWARM_PX) : x0;
        nds_wr32(v, OFF_TOP, y0);   nds_wr32(v, OFF_LEFT, lx);
        nds_wr32(v, OFF_WIDTH, NDS_PREWARM_PX); nds_wr32(v, OFF_HEIGHT, H);
        nds_wr32(v, OFF_UPDMODE, UPD_PARTIAL);
        if (env->strip_wf) nds_wr32(v, OFF_WAVEFORM, env->strip_wf);
        if (cfa_skip) nds_wr32(v, plat->flags_off, nds_rd32(v, plat->flags_off) | cfa_skip);
        const uint32_t mk = *env->ephemeral_marker + 0x200u;   // clear of the strip markers below
        nds_wr32(v, OFF_MARKER, mk);
        const uint64_t st0 = nds_now_us();
        const int rc = env->real_ioctl(fd, plat->send, v);
        if (rc >= 0) { uint32_t wm = mk; env->real_ioctl(fd, plat->wait_sub, &wm); }
        if (nds_verbose_enabled)
            NDS_LOG("PREWARM [hwtcon] idle_any=%lums -> %dpx %s-edge (rc=%d) sliver=%luus, then warm sweep",
                    (unsigned long)(env->idle_any_us / 1000ull), NDS_PREWARM_PX,
                    env->rtl ? "right" : "left", rc, (unsigned long)(nds_now_us() - st0));
    }

    bool m_ok = false;
    for (int k = 0; k < N; k++) {
        const int col = env->rtl ? (N - 1 - k) : k;
        const uint32_t left  = x0 + (uint32_t)col * sw;
        const uint32_t width = (col == N - 1) ? (x0 + W - left) : sw;   // last column takes the remainder
        const bool last = (k == N - 1);

        memcpy(v, orig, plat->size);
        nds_wr32(v, OFF_TOP, y0);    nds_wr32(v, OFF_LEFT, left);
        nds_wr32(v, OFF_WIDTH, width); nds_wr32(v, OFF_HEIGHT, H);
        nds_wr32(v, OFF_UPDMODE, UPD_PARTIAL);
        if (env->strip_wf) nds_wr32(v, OFF_WAVEFORM, env->strip_wf);
        if (cfa_skip) nds_wr32(v, plat->flags_off, nds_rd32(v, plat->flags_off) | cfa_skip);
        // The last strip carries Nickel's marker M (the strips ARE the final render); earlier strips
        // get throwaway ephemeral markers.
        const uint32_t mk = last ? M : (*env->ephemeral_marker + (uint32_t)k);
        nds_wr32(v, OFF_MARKER, mk);

        if (env->real_ioctl(fd, plat->send, v) < 0) continue;
        if (mk == M) m_ok = true;

        if (env->wait_complete) {                    // settle fully between strips: slower, more discrete
            uint32_t md[2] = { mk, 0 };
            env->real_ioctl(fd, plat->wait_cmpl, md);
        } else {                                     // the normal path: wait for submission only, which
            uint32_t wm = mk;                        // is what keeps the MDP from merging the strips
            env->real_ioctl(fd, plat->wait_sub, &wm);
        }
        if (env->delay_us > 0 && !last) usleep((useconds_t)env->delay_us);
    }
    // Hang-safety: marker M must reach the driver, else Nickel's WAIT_COMPLETE(M) never returns.
    if (!m_ok) env->real_ioctl(fd, plat->send, (void *)orig);
    *env->ephemeral_marker += (uint32_t)N;
}
