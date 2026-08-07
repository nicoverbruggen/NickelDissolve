// Shared vocabulary for the e-ink drivers. Declarations only, no logic.
//
// Each driver (driver_hwtcon, driver_mxcfb, driver_sunxi) owns its own hardware knowledge and its own
// sweep, so they can diverge freely as more hardware is understood. What they share is the small
// descriptor the CORE needs to talk about any of them uniformly: which ioctls carry an update, where
// the fields are, and whether the interface is proven. Keeping that here rather than in one driver
// avoids an arbitrary dependency between siblings.

#ifndef NDS_DRIVER_COMMON_H
#define NDS_DRIVER_COMMON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Field offsets shared by hwtcon and mxcfb. Kobo modelled hwtcon on mxcfb, so the update struct's
// prefix is identical on both; they diverge after it, which is why they are separate drivers.
enum { OFF_TOP = 0, OFF_LEFT = 4, OFF_WIDTH = 8, OFF_HEIGHT = 12,
       OFF_WAVEFORM = 16, OFF_UPDMODE = 20, OFF_MARKER = 24 };
enum { UPD_PARTIAL = 0, UPD_FULL = 1 };

#define WF_GC16 2u          // full black-flash mode; id 2 on BOTH interfaces
#define NDS_PREWARM_PX 2    // width of the cold-wake lead-in sliver (a hairline; the driver validates
                            // region bounds only, no minimum width)

// What the core needs to know about whichever interface a device turned out to have.
struct nds_platform {
    const char   *name;
    unsigned long send;        // *_SEND_UPDATE (intercept + reissue partial strips)
    unsigned long wait_sub;    // *_WAIT_FOR_UPDATE_SUBMISSION (0 = this interface has none)
    unsigned long wait_cmpl;   // *_WAIT_FOR_UPDATE_COMPLETE
    uint32_t      size;        // bytes to copy when reissuing an update
    uint32_t      flags_off;   // byte offset of the `flags` field
    uint32_t      cfa_skip;    // CFA-skip flag bit (0 = interface has no CFA pass)
    uint32_t      cfa_field;   // mask of the CFA colour-mode field inside `flags` (0 = no CFA)
    bool          flash_full;  // true = a flash is update_mode==FULL; false = a flash is the GC16 waveform
    uint32_t      def_delay_us;// default inter-strip delay when nds_delay_us is unset
    uint32_t      dith_off;    // byte offset of dither_mode (0 = the struct has no such field)
    uint32_t      def_strip_wf;// band waveform when unset; 0 = reuse the turn's own
    bool          sweep_proven;// false = decoded and logged, but never animated by a release build
};

// What a sweep needs from the core, passed explicitly so no driver holds hidden state.
struct nds_sweep_env {
    int (*real_ioctl)(int fd, unsigned long request, void *argp);
    int       bands;
    int       delay_us;
    bool      rtl;                // already flipped for a backward turn
    bool      wait_complete;      // wait for each strip to settle rather than merely be queued
    bool      cfa_skip;           // honoured by hwtcon only
    uint32_t  strip_wf;           // 0 = keep the turn's own waveform
    int       prewarm_ms;         // cold-wake lead-in threshold; 0 = disabled
    uint64_t  idle_any_us;        // idle since the previous e-ink update of any kind
    uint32_t *ephemeral_marker;   // in/out: markers for non-final strips, advanced by `bands`
};

#ifdef __cplusplus
}
#endif
#endif
