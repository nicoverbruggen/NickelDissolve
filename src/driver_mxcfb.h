// NXP i.MX `mxcfb` e-ink driver: everything from the 2011 Touch to the Clara 2E.
//
// Three generations of one interface, told apart by the ioctl number, which encodes the size of its
// own argument struct: 72 bytes on the current one (Libra 2, Clara 2E, Clara HD, Forma, Nia, Libra
// H2O), 68 on the i.MX6SL and the older i.MX50 boards, 64 on the i.MX50 Aura and Aura H2O. What makes
// this its own driver rather than a variant of hwtcon: no CFA colour pass at all (these are mono
// panels), no submission wait to pace strips (so it needs an explicit delay), a flash is recognised by
// update_mode rather than by waveform, and the waveform ids mean different things.

#ifndef NDS_DRIVER_MXCFB_H
#define NDS_DRIVER_MXCFB_H

#include "driver_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// i.MX waveform ids (koreader's mxcfb-kobo.h numbering). Note these collide with the hwtcon names:
// id 8 is DU4 here but GCK16 there, which is why the two drivers keep separate tables.
#define MX_WF_DU      1u
#define MX_WF_GL16    5u
#define MX_WF_REAGL   6u
#define MX_WF_REAGLD  7u
#define MX_WF_GLKW16 10u
#define MX_WF_AUTO  257u

const struct nds_platform *nds_mxcfb_match(unsigned long request);
bool        nds_mxcfb_wf_sweepable(const struct nds_platform *plat, uint32_t wf);
const char *nds_mxcfb_wf_name(uint32_t wf);
void        nds_mxcfb_sweep(int fd, const struct nds_platform *plat, const uint8_t *orig,
                            const struct nds_sweep_env *env);

#ifdef __cplusplus
}
#endif
#endif
