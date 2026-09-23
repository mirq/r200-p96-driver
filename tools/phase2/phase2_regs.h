#ifndef PHASE2_REGS_H
#define PHASE2_REGS_H

/*
 * Shared constants for the Phase-2 PPC ring-writer prototype
 * (docs/09-ppc-direct-ring-design.md): the 68k host grants an interface-20
 * engine lease, the WarpOS PPC client submits fenced PACKET2 batches
 * directly to the CP ring through the 1:1 alias, and both sides verify the
 * scratch fences. Includes phase0_regs.h for the barrier helpers and the
 * MMIO register offsets.
 */

#include "../phase0/phase0_regs.h"   /* P0_RBBM_STATUS, P0_CP_RB_RPTR/WPTR,
                              P0_SCRATCH_REG0 and the barrier helpers */

#define P2_MAGIC            0x50324342UL    /* "P2CB" */
#define P2_VERSION          1UL
#define P2_HOST_DONE        0x484f5354UL    /* "HOST" */
#define P2_PPC_ACK          0x50504332UL    /* "PPC2" */

/* Control-block dword indexes. */
#define P2_I_MAGIC          0UL
#define P2_I_VERSION        1UL
#define P2_I_HOST_DONE      2UL
#define P2_I_PPC_ACK        3UL
#define P2_I_RING_CPU       4UL
#define P2_I_RING_GPU       5UL
#define P2_I_RING_DWORDS    6UL
#define P2_I_RING_MASK      7UL
#define P2_I_BAR2           8UL
#define P2_I_NEXT_FENCE     9UL
#define P2_I_BATCHES        10UL
#define P2_I_CONTROL_BYTES  11UL
#define P2_I_SUBMITS        12UL
#define P2_I_LAST_FENCE     13UL
#define P2_I_RETIRED        14UL
#define P2_I_ERROR_STAGE    15UL
#define P2_I_COUNT          16UL

/* PPC failure stages (P2_I_ERROR_STAGE). */
#define P2_STAGE_BLOCK      1UL
#define P2_STAGE_RING_RANGE 2UL
#define P2_STAGE_RESERVE    3UL
#define P2_STAGE_READBACK   4UL
#define P2_STAGE_WPTR       5UL
#define P2_STAGE_FENCE      6UL
#define P2_STAGE_PUBLISH    7UL

/*
 * The fence tail is the six-dword shape every 68k submission uses
 * (src/radeon_cp.c CpStreamDword): cache flush, full engine idle wait,
 * then the scratch sequence write. The CP executes it in ring order, so
 * SCRATCH_REG0 carrying a sequence proves everything before it drained.
 * Register encodings match src/radeon_regs.h: PACKET0(reg, count) =
 * (count << 16) | (reg >> 2), PACKET2 = 0x80000000.
 */
#define P2_PACKET2                  0x80000000UL
#define P2_PACKET0(reg)             ((ULONG)(reg) >> 2)
#define P2_DSTCACHE_CTLSTAT         0x1714UL
#define P2_WAIT_UNTIL               0x1720UL
#define P2_CACHE_FLUSH_ALL          0x0000000fUL
#define P2_WAIT_IDLE \
    ((1UL << 9) | (1UL << 16) | (1UL << 17) | (1UL << 18))
#define P2_SCRATCH_REG0             0x15e0UL
#define P2_FENCE_TAIL_DWORDS        6UL

/* Probe shape: 32 batches of 8 PACKET2 dwords + the fence tail, padded to
 * the CP's 16-byte ring alignment (4 dwords). */
#define P2_BATCH_DWORDS             8UL
#define P2_BATCH_COUNT              32UL
#define P2_STREAM_DWORDS            (P2_BATCH_DWORDS + P2_FENCE_TAIL_DWORDS)
#define P2_PADDED_DWORDS 16UL /* (13 + 3) & ~3: 14 -> 16 dwords */
#define P2_RESERVE_TIMEOUT          200000UL

#endif /* PHASE2_REGS_H */
