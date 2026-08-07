// AllWinner (sunxi) e-ink driver. See driver_sunxi.h for why this is a separate file.

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "driver_sunxi.h"
#include "util.h"

// ---- the update argument -----------------------------------------------------------------
// ioctl(fd, 0x406, ulong ubuffer[8]). Decoded on a Kobo Elipsa (fw 4.38.23697) against its
// 1404x1872 panel, cross-checked with the libkobo disassembly of Disp::flush, whose stores cover
// exactly r7+0x40..0x5c:
//
//   ubuffer[0] -> update descriptor (rect and flags)
//   ubuffer[2]  = flush mode (see below)
//   ubuffer[3] -> disp_layer_config2, carrying the full panel size
//   ubuffer[4] -> word the frame id is returned in (starts as 0xffffffff)
//
// The descriptor's rect is a QRect in Qt's inclusive x1,y1,x2,y2 form, so width is right-left+1.
#define NDS_SUNXI_WORDS 8            // ubuffer length
#define NDS_SUNXI_DESC  24           // bytes of the descriptor this file reads
#define SX_UB_MODE      8            // byte offset of ubuffer[2] within the buffer

enum { SX_LEFT = 0, SX_TOP = 4, SX_RIGHT = 8, SX_BOTTOM = 12, SX_F16 = 16, SX_F20 = 20 };

// AllWinner flush modes, from the kernel's own help text. A bitmask, not an enum, with 0x400 marking
// the regional ("RECT") variants:
//     EINK_INIT_MODE 0x01   EINK_DU_MODE 0x02   EINK_GC16_MODE 0x04   EINK_GC16_LOCAL_MODE 0x84
//     EINK_DU_RECT_MODE 0x402   EINK_GC16_RECT_MODE 0x404   EINK_A2_RECT_MODE 0x410
//     EINK_GC16_LOCAL_RECT_MODE 0x484
//
// GC16 (0x04) is the full black-flash refresh. Reissuing a page turn's own update per band while it
// says GC16 gives one flash PER BAND: slow, and it looks broken. That is what the first Elipsa attempt
// did. The bands need a flashless regional mode instead, the same idea as def_strip_wf on hwtcon.
// 0x420 is the default because the device demonstrably uses it for status-bar redraws that never
// flash, so it is known-good on this panel rather than guessed from the bitmask.
#define SX_MODE_BAND_DEF 0x420u

// ---- address validation ------------------------------------------------------------------
// This interface hands over pointers, and nothing guarantees how much is mapped behind one. write()
// copies from user space, so an unmapped address comes back as -EFAULT rather than a signal: that
// makes /dev/null a way to test an address before reading it, instead of faulting inside Nickel.
static int nds_probe_fd = -1;

void nds_sunxi_init(void) {
    if (nds_probe_fd < 0)
        nds_probe_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
}

static bool nds_readable(const void *p, size_t n) {
    if (nds_probe_fd < 0 || !p) return false;
    return write(nds_probe_fd, p, n) == (ssize_t)n;
}

// Resolve ubuffer -> descriptor, validating both. Returns nullptr if anything is not readable.
static uint8_t *nds_sunxi_desc(const void *argp) {
    if (!argp || !nds_readable(argp, NDS_SUNXI_WORDS * 4)) return nullptr;
    uint32_t w = nds_rd32((const uint8_t *)argp, 0);
    if (w < 0x10000u || (w & 3u)) return nullptr;         // too low or misaligned to be a pointer
    uint8_t *d = (uint8_t *)(uintptr_t)w;
    return nds_readable(d, NDS_SUNXI_DESC) ? d : nullptr;
}

bool nds_sunxi_page_rect(const void *argp, int32_t *out_w, int32_t *out_h) {
    const uint8_t *d = nds_sunxi_desc(argp);
    if (!d) return false;
    const int32_t l = (int32_t)nds_rd32(d, SX_LEFT),  t = (int32_t)nds_rd32(d, SX_TOP);
    const int32_t r = (int32_t)nds_rd32(d, SX_RIGHT), b = (int32_t)nds_rd32(d, SX_BOTTOM);
    const int32_t w = r - l + 1, h = b - t + 1;
    if (w < 512 || h < 512) return false;                 // not a page render
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return true;
}

// ---- discovery logging -------------------------------------------------------------------
void nds_sunxi_probe(unsigned long request, const void *argp, int turn_pending) {
    static int upd = 0, wait = 0;
    if (!argp) return;

    if (request == NDS_SUNXI_WAIT) {
        if (wait >= 40 || !nds_readable(argp, 8)) return;
        wait++;
        NDS_LOG("sunxi WAIT frame=%u", nds_rd32((const uint8_t *)argp, 0));
        return;
    }
    if (request != NDS_SUNXI_UPDATE || upd >= 200) return;
    if (!nds_readable(argp, NDS_SUNXI_WORDS * 4)) return;
    upd++;

    const uint8_t *u = (const uint8_t *)argp;
    // The first couple get the whole buffer, so the decode above stays checkable from a log alone.
    if (upd <= 2)
        NDS_LOG("sunxi UPDATE #%d ubuffer = %08x %08x %08x %08x %08x %08x %08x %08x", upd,
                nds_rd32(u, 0), nds_rd32(u, 4), nds_rd32(u, 8), nds_rd32(u, 12),
                nds_rd32(u, 16), nds_rd32(u, 20), nds_rd32(u, 24), nds_rd32(u, 28));

    const uint8_t *d = nds_sunxi_desc(argp);
    if (!d) return;
    const int32_t l = (int32_t)nds_rd32(d, SX_LEFT),  t = (int32_t)nds_rd32(d, SX_TOP);
    const int32_t r = (int32_t)nds_rd32(d, SX_RIGHT), b = (int32_t)nds_rd32(d, SX_BOTTOM);
    NDS_LOG("sunxi UPD #%d rect=(%d,%d)-(%d,%d) %dx%d mode=0x%x f16=%u f20=%u turn=%d",
            upd, l, t, r, b, r - l + 1, b - t + 1,
            nds_rd32(u, SX_UB_MODE), nds_rd32(d, SX_F16), nds_rd32(d, SX_F20), turn_pending);
}

// ---- the sweep ---------------------------------------------------------------------------

// Band boundary k of N across [l, l+W). Snapped down to an 8-pixel boundary because e-ink controllers
// commonly want an update region's x and width aligned, and a plain W/N split does not give that at
// every band count. The first and last edges stay exact so the sweep always covers the whole rect.
static int32_t nds_band_edge(int32_t l, int32_t W, int k, int N) {
    if (k <= 0) return l;
    if (k >= N)  return l + W;
    return l + ((int32_t)(((int64_t)W * (int64_t)k) / N) & ~(int32_t)7);
}

bool nds_sunxi_sweep(int fd, void *argp, const struct nds_sunxi_env *env, int *out_rc) {
    if (!env || !env->real_ioctl || !out_rc) return false;
    uint8_t *d = nds_sunxi_desc(argp);
    if (!d) return false;

    const int32_t l = (int32_t)nds_rd32(d, SX_LEFT);
    const int32_t r = (int32_t)nds_rd32(d, SX_RIGHT);
    const int32_t W = r - l + 1;
    int32_t h = 0;
    if (!nds_sunxi_page_rect(argp, nullptr, &h)) return false;

    const int N = env->bands < 2 ? 2 : (env->bands > 32 ? 32 : env->bands);
    const uint32_t band_mode = env->band_mode ? env->band_mode : SX_MODE_BAND_DEF;

    // Only the words this changes are touched, and each is saved first, so the caller's buffer comes
    // back exactly as it arrived. That avoids copying a struct whose total size is unknown.
    const uint32_t save_l = nds_rd32(d, SX_LEFT), save_r = nds_rd32(d, SX_RIGHT);
    const uint32_t save_mode = nds_rd32((const uint8_t *)argp, SX_UB_MODE);

    int rc = -1;
    bool any = false;
    for (int k = 0; k < N; k++) {
        const int col = env->rtl ? (N - 1 - k) : k;
        const int32_t left  = nds_band_edge(l, W, col, N);
        const int32_t right = nds_band_edge(l, W, col + 1, N) - 1;
        if (right < left) continue;                       // a band the alignment collapsed to nothing

        nds_wr32(d, SX_LEFT,  (uint32_t)left);
        nds_wr32(d, SX_RIGHT, (uint32_t)right);
        nds_wr32((uint8_t *)argp, SX_UB_MODE, band_mode);
        rc = env->real_ioctl(fd, NDS_SUNXI_UPDATE, argp);
        if (rc >= 0) {
            any = true;
            // The update ioctl RETURNS the frame id (the ids climb by exactly one per band), so each
            // band can be waited on by its own id. Without this the driver drops bands: the first
            // attempt submitted them back to back and only some reached the panel.
            uint32_t frame[2] = { (uint32_t)rc, 0 };
            env->real_ioctl(fd, NDS_SUNXI_WAIT, frame);
        }
        if (env->delay_us > 0 && k != N - 1) usleep((useconds_t)env->delay_us);
    }

    nds_wr32(d, SX_LEFT,  save_l);
    nds_wr32(d, SX_RIGHT, save_r);
    nds_wr32((uint8_t *)argp, SX_UB_MODE, save_mode);

    if (!any) {                                           // every band refused: draw the page normally
        NDS_LOG("sunxi sweep: all %d bands failed, falling back to the original update", N);
        *out_rc = env->real_ioctl(fd, NDS_SUNXI_UPDATE, argp);
        return true;
    }
    if (nds_verbose_enabled)
        NDS_LOG("sunxi sweep: %d bands over %dx%d %s, first=[%d..%d] last=[%d..%d], "
                "mode 0x%x->0x%x, delay=%dus, last frame=%d",
                N, (int)W, (int)h, env->rtl ? "R->L" : "L->R",
                (int)nds_band_edge(l, W, 0, N), (int)(nds_band_edge(l, W, 1, N) - 1),
                (int)nds_band_edge(l, W, N - 1, N), (int)(nds_band_edge(l, W, N, N) - 1),
                save_mode, band_mode, env->delay_us, rc);
    *out_rc = rc;
    return true;
}

