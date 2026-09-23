#ifndef PHASE3_REGS_H
#define PHASE3_REGS_H

/*
 * Shared constants for the Phase-3 sustained-lease gate
 * (docs/09-ppc-direct-ring-design.md): the 68k host grants an interface-20
 * engine lease and renews it with Radeon3DHeartbeatLease() while the WarpOS
 * PPC client paces 20000 fenced batches through the ring wrap - proving
 * heartbeats keep the lease alive past the expiry bound under real desktop
 * 2D load. Reuses the Phase-2 control-block layout and MMIO helpers.
 */

#include "../phase2/phase2_regs.h"

#define P3_MAGIC            0x50334342UL    /* "P3CB" */
#define P3_VERSION          1UL
#define P3_HOST_DONE        0x484f5354UL    /* "HOST" */
#define P3_PPC_ACK          0x50504333UL    /* "PPC3" */

/* P3 extends the P2 block with one pacing dword at index P2_I_COUNT. */
#define P3_I_PACE_SPINS     16UL
#define P3_I_COUNT          17UL

/* Probe shape: 20000 paced batches = 320000 ring dwords, which crosses the
 * 262144-dword ring wrap once. The child delays PACE_SPINS compute
 * iterations between batches (~250 us at MPC7410 clocks), so the whole run
 * takes ~5-6 s and outlives the 5 s expiry unless the host heartbeats. */
#define P3_BATCH_COUNT      20000UL
#define P3_MAX_BATCHES      100000UL
#define P3_PACE_SPINS       50000UL

#endif /* PHASE3_REGS_H */
