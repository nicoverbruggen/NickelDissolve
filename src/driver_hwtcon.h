// MediaTek `hwtcon` e-ink driver: Clara BW, Clara Colour, Libra Colour, Elipsa 2E.
//
// The modern interface, and the one the animation was built and tested on. What makes it its own
// driver rather than a variant of mxcfb: it is the only Kobo interface with a CFA colour pass (so it
// is the only one where a colour page must be kept out of the sweep), the only one with a submission
// wait to pace strips against the MDP merge, and it numbers its waveform modes differently.

#ifndef NDS_DRIVER_HWTCON_H
#define NDS_DRIVER_HWTCON_H

#include "driver_common.h"

#ifdef __cplusplus
extern "C" {
#endif

// hwtcon waveform ids, from KoboScreenMTK::idToWaveform in libkobo.
#define WF_DU      1u
#define WF_GL16    3u
#define WF_GLR16   4u
#define WF_GCK16   8u    // dark-mode flash (the GC16 counterpart)
#define WF_GLKW16  9u    // dark-mode reading turn (the GLR16 counterpart)
#define WF_GCC16  10u
#define WF_GLRC16 11u

const struct nds_platform *nds_hwtcon_match(unsigned long request);
bool        nds_hwtcon_wf_sweepable(uint32_t wf);
const char *nds_hwtcon_wf_name(uint32_t wf);
void        nds_hwtcon_sweep(int fd, const struct nds_platform *plat, const uint8_t *orig,
                             const struct nds_sweep_env *env);

#ifdef __cplusplus
}
#endif
#endif
