// NXP i.MX `mxcfb` e-ink driver. See driver_mxcfb.h.

#include <cstdint>
#include <cstring>
#include <unistd.h>

#include "config.h"
#include "driver_mxcfb.h"
#include "util.h"

// Three generations of the same interface. The ioctl number encodes the size of its own argument, so
// the size in each row is not a guess: it is read out of the request. Everything this driver touches
// sits at the same offset in all three (waveform@16, update_mode@20, marker@24, flags@32), because the
// generations differ only in trailing fields.
//
// The current one has an 8-byte WAIT_FOR_UPDATE_COMPLETE; the older two take a bare 4-byte marker.
// None has a submission wait, so strips are paced by an explicit delay instead.
//
// sweep_proven is false on the legacy rows: what a reading turn looks like on that hardware is not
// known, and the i.MX50 kernels have no REAGL waveform at all, so the allowlist below cannot be right
// for them. Recognising the ioctl is still what makes nds_mode:observe produce a usable log there.
static const struct nds_platform NDS_MXCFB[] = {
    // Libra 2, Clara 2E, and the whole Clara HD / Forma / Nia / Libra H2O group, which issue the
    // identical ioctl. Reading turn = PARTIAL + REAGL(6); a flash is FULL + AUTO(257)/GC16, so a flash
    // is told by update_mode rather than by waveform. def_strip_wf = 0: the strips REUSE the turn's own
    // waveform, so each band renders at whatever quality Nickel picked for the page.
    { "mxcfb",   0x4048462EUL, 0UL, 0xC008462FUL, 72, 32, 0UL, 0UL, true, 30000, 0, 0, true  },
    // i.MX6SL (Glo HD, Touch 2.0, Aura ONE, the Edition 2 v1 boards) and the older i.MX50 boards.
    { "mxcfb68", 0x4044462EUL, 0UL, 0x4004462FUL, 68, 32, 0UL, 0UL, true, 30000, 0, 0, false },
    // i.MX50 Aura and Aura H2O.
    { "mxcfb64", 0x4040462EUL, 0UL, 0x4004462FUL, 64, 32, 0UL, 0UL, true, 30000, 0, 0, false },
};
static const int NDS_NMXCFB = (int)(sizeof(NDS_MXCFB) / sizeof(NDS_MXCFB[0]));

const struct nds_platform *nds_mxcfb_match(unsigned long request) {
    for (int i = 0; i < NDS_NMXCFB; i++)
        if (request == NDS_MXCFB[i].send) return &NDS_MXCFB[i];
    return nullptr;
}

// These are mono panels, so there is no colour page to guard against; the test is only whether the
// update is a flashless greyscale reading turn. Which ids qualify depends on the generation: on-device
// logs show one Libra 2 turning with REAGL(6) and an older board with GLKW16(10), while both use
// AUTO(257)/DU for menus and GC16 or full-screen AUTO for flashes.
bool nds_mxcfb_wf_sweepable(const struct nds_platform *plat, uint32_t wf) {
    if (nds_global_config_bool("nds_debug_sweep_any_waveform", false)) return wf != WF_GC16;
    // On a generation with no page-turn evidence, require REAGL or REAGLD specifically. The i.MX50
    // kernels of the Touch, Glo, Mini and Aura HD carry no REAGL waveform at all, so those devices
    // exclude themselves by what they emit and need no model list. GL16 is deliberately not accepted
    // there, since it is the one flashless mode those panels do have.
    if (!plat->sweep_proven) return wf == MX_WF_REAGL || wf == MX_WF_REAGLD;
    return wf == MX_WF_GL16 || wf == MX_WF_REAGL || wf == MX_WF_REAGLD || wf == MX_WF_GLKW16;
}

const char *nds_mxcfb_wf_name(uint32_t wf) {
    switch (wf) {
        case 0:  return "INIT";  case 1: return "DU";    case 2:  return "GC16";   case 3:   return "GC4";
        case 4:  return "A2";    case 5: return "GL16";  case 6:  return "REAGL";  case 7:   return "REAGLD";
        case 8:  return "DU4";   case 9: return "GCK16"; case 10: return "GLKW16"; case 257: return "AUTO";
        default: return "?";
    }
}

// Replace one full-screen update with N swept partial strips.
void nds_mxcfb_sweep(int fd, const struct nds_platform *plat, const uint8_t *orig,
                     const struct nds_sweep_env *env) {
    if (!plat || !orig || !env || !env->real_ioctl) return;

    const uint32_t M  = nds_rd32(orig, OFF_MARKER);
    const uint32_t x0 = nds_rd32(orig, OFF_LEFT), y0 = nds_rd32(orig, OFF_TOP);
    const uint32_t W  = nds_rd32(orig, OFF_WIDTH), H = nds_rd32(orig, OFF_HEIGHT);
    const int N = env->bands < 2 ? 2 : (env->bands > 32 ? 32 : env->bands);
    const uint32_t sw = W / (uint32_t)N;

    uint8_t v[128];
    if (plat->size > sizeof(v) || sw == 0) { env->real_ioctl(fd, plat->send, (void *)orig); return; }

    // Cold-wake lead-in: if the panel has powered down, the first update after it stalls on the
    // power-on. Spend that stall on a thin sliver at the sweep's leading edge so the freeze is a
    // hairline rather than a full band; the real bands then run warm. With no submission wait here,
    // the sliver is simply submitted and the stall is paid on it.
    if (env->prewarm_ms > 0 && env->idle_any_us >= (uint64_t)env->prewarm_ms * 1000ull) {
        memcpy(v, orig, plat->size);
        const uint32_t lx = env->rtl ? (x0 + W - NDS_PREWARM_PX) : x0;
        nds_wr32(v, OFF_TOP, y0);   nds_wr32(v, OFF_LEFT, lx);
        nds_wr32(v, OFF_WIDTH, NDS_PREWARM_PX); nds_wr32(v, OFF_HEIGHT, H);
        nds_wr32(v, OFF_UPDMODE, UPD_PARTIAL);
        if (env->strip_wf) nds_wr32(v, OFF_WAVEFORM, env->strip_wf);
        nds_wr32(v, OFF_MARKER, *env->ephemeral_marker + 0x200u);
        const uint64_t st0 = nds_now_us();
        const int rc = env->real_ioctl(fd, plat->send, v);
        if (nds_verbose_enabled)
            NDS_LOG("PREWARM [%s] idle_any=%lums -> %dpx %s-edge (rc=%d) sliver=%luus, then warm sweep",
                    plat->name, (unsigned long)(env->idle_any_us / 1000ull), NDS_PREWARM_PX,
                    env->rtl ? "right" : "left", rc, (unsigned long)(nds_now_us() - st0));
    }

    bool m_ok = false;
    for (int k = 0; k < N; k++) {
        const int col = env->rtl ? (N - 1 - k) : k;
        const uint32_t left  = x0 + (uint32_t)col * sw;
        const uint32_t width = (col == N - 1) ? (x0 + W - left) : sw;   // last column takes the remainder
        const bool last = (k == N - 1);

        memcpy(v, orig, plat->size);
        nds_wr32(v, OFF_TOP, y0);      nds_wr32(v, OFF_LEFT, left);
        nds_wr32(v, OFF_WIDTH, width); nds_wr32(v, OFF_HEIGHT, H);
        nds_wr32(v, OFF_UPDMODE, UPD_PARTIAL);
        if (env->strip_wf) nds_wr32(v, OFF_WAVEFORM, env->strip_wf);
        // The last strip carries Nickel's marker M (the strips ARE the final render); earlier strips
        // get throwaway ephemeral markers.
        const uint32_t mk = last ? M : (*env->ephemeral_marker + (uint32_t)k);
        nds_wr32(v, OFF_MARKER, mk);

        if (env->real_ioctl(fd, plat->send, v) < 0) continue;
        if (mk == M) m_ok = true;

        // No submission wait on this interface, and no MDP to merge the strips, so the pacing is the
        // delay alone. Waiting for completion is available but is the slow, discrete option.
        if (env->wait_complete) {
            uint32_t md[2] = { mk, 0 };
            env->real_ioctl(fd, plat->wait_cmpl, md);
        }
        if (env->delay_us > 0 && !last) usleep((useconds_t)env->delay_us);
    }
    // Hang-safety: marker M must reach the driver, else Nickel's WAIT_COMPLETE(M) never returns.
    if (!m_ok) env->real_ioctl(fd, plat->send, (void *)orig);
    *env->ephemeral_marker += (uint32_t)N;
}
