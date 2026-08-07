// AllWinner (sunxi) e-ink driver: the Kobo Sage and Elipsa.
//
// This is the one Kobo display interface that does not fit the flat update struct the rest of the mod
// drives. Nickel talks to it through /dev/disp with the AllWinner disp2 layer API, where the geometry
// sits behind a pointer and the flush mode is a separate word, so it lives in its own file rather than
// as another row in the platform table. See "Kobo hardware map" in ABOUT.md for how it was decoded.
//
// Nothing here reaches back into the core: everything the sweep needs is passed in, so this file can be
// read, changed or removed without tracing globals through nickeldissolve.cc.

#ifndef NDS_DRIVER_SUNXI_H
#define NDS_DRIVER_SUNXI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NDS_SUNXI_UPDATE 0x406UL     // DISP_EINK_UPDATE2, from Disp::flush(const QRect&, int, ulong)
#define NDS_SUNXI_WAIT   0x4014UL    // frame-sync wait, from Disp::waitFor(int); arg[0] = frame id

// Open the address-validation fd used before reading any pointer this interface hands over. Safe to
// call more than once; if it fails the driver simply stays quiet.
void nds_sunxi_init(void);

// Log what the device issues. Read-only. turn_pending is passed purely so a line can say whether a
// page turn was armed when the update fired.
void nds_sunxi_probe(unsigned long request, const void *argp, int turn_pending);

// Read the update's rect. Returns false unless this is a decodable full-screen page render, which is
// what lets the caller size the sweep with the same band rule every other device uses.
bool nds_sunxi_page_rect(const void *argp, int32_t *out_w, int32_t *out_h);

// Target band width for this interface, in thousandths of an inch. Much wider than the ~354 the
// MediaTek and i.MX devices use, because a band costs far more here: each one is a full
// completion-waited refresh on an older controller driving a large panel. Set from what actually
// looks right on an Elipsa (6.19in wide, 9 bands), rather than derived, because the number that
// matters is how the wipe reads and only the hardware can say. A Sage lands on 7 by the same rule.
#define NDS_SUNXI_BAND_MILS 687

// What the sweep needs from the core. Passed explicitly so this file holds no hidden state.
struct nds_sunxi_env {
    int (*real_ioctl)(int fd, unsigned long request, void *argp);
    int      bands;        // resolved by the caller's band rule
    int      delay_us;     // extra pause between bands; 0 = rely on the per-band wait alone
    bool     rtl;          // sweep direction, already flipped for a backward turn
    uint32_t band_mode;    // flush mode for the bands; 0 = the driver's flashless default
};

// Sweep a full-screen page render. True when it handled the update (result in *out_rc); false when the
// caller should pass the original through untouched.
bool nds_sunxi_sweep(int fd, void *argp, const struct nds_sunxi_env *env, int *out_rc);

#ifdef __cplusplus
}
#endif
#endif
