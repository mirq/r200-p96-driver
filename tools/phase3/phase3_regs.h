#ifndef PHASE3_REGS_H
#define PHASE3_REGS_H

/*
 * Shared constants for the Phase-3 sustained-lease gate
 * (docs/09-ppc-direct-ring-design.md). Layout version 2, redesigned after
 * the 2026-09-23 handshake failure and expert review:
 *
 * - The control block is split at cache-line granularity: dwords 0..31 are
 *   HOST-OWNED (the 68k writes, the PPC only reads), dwords 32..63 are
 *   PPC-OWNED (the PPC writes, the 68k only reads). A dirty PPC line can
 *   then never write back over host-owned words, and vice versa.
 * - Presence and completion are separate sticky signals: PPC_PRESENT is
 *   written once and never erased; PPC_DONE is the terminal publication
 *   (success or failure) written last. The child's exit can no longer
 *   destroy the host's pending arrival signal.
 * - All pacing is EClock-based: the review measured the old BusyUs loop at
 *   ~30x its intended delay (22 CIA reads per "microsecond"), which made
 *   every host poll interval and heartbeat cadence wrong.
 *
 * The PPC's loads of the aliased VRAM window are cache-served (proven on
 * hardware): the child must dcbf-invalidate host-owned lines before every
 * read. The PPC's stores commit; publication uses eieio/sync plus a dcbf
 * over the PPC-owned lines before PPC_DONE. The 68k side is uncached
 * (CI aperture): plain volatile loads/stores with a readback drain.
 */

#include "../phase2/phase2_regs.h"   /* P2_* field indexes are reused below;
                                     * P0_* barrier helpers come from phase0 */

#define P3_MAGIC            0x50334342UL    /* "P3CB" */
#define P3_VERSION          2UL
#define P3_HOST_READY       0x484f5354UL    /* "HOST" */
#define P3_PPC_PRESENT      0xffffffffUL
#define P3_PPC_DONE         0x50504333UL    /* "PPC3" */

/* Host-owned dwords (offsets 0x00-0x7f). Only the 68k writes these. */
#define P3_I_MAGIC              0UL
#define P3_I_VERSION            1UL
#define P3_I_HOST_READY         2UL
#define P3_I_GENERATION         3UL
#define P3_I_RING_CPU           4UL
#define P3_I_RING_GPU           5UL
#define P3_I_RING_DWORDS        6UL
#define P3_I_RING_MASK          7UL
#define P3_I_BAR2               8UL
#define P3_I_NEXT_FENCE         9UL
#define P3_I_BATCHES            10UL
#define P3_I_PACE_US            11UL
#define P3_I_CONTROL_BYTES      12UL
#define P3_I_HOST_END           32UL     /* first PPC-owned dword */

/* PPC-owned dwords (offsets 0x80-0xff). The PPC writes only these. The
 * generation echo (P3_I_PPC_GEN_ECHO) is written by the child before its
 * signals and lets the host discard a previous run's stale DONE/PRESENT:
 * only values whose echo matches this run's generation are trusted. */
#define P3_I_PPC_PRESENT        32UL
#define P3_I_PPC_DONE           33UL
#define P3_I_ERROR_STAGE        34UL
#define P3_I_PPC_GEN_ECHO       35UL
#define P3_I_SUBMITS            36UL
#define P3_I_LAST_FENCE         37UL
#define P3_I_RETIRED            38UL
#define P3_I_PPC_END            64UL

/* PPC failure stages (P3_I_ERROR_STAGE). */
#define P3_STAGE_BLOCK          1UL
#define P3_STAGE_RING_RANGE     2UL
#define P3_STAGE_HOST_WAIT      3UL
#define P3_STAGE_RESERVE        4UL
#define P3_STAGE_READBACK       5UL
#define P3_STAGE_WPTR           6UL
#define P3_STAGE_FENCE          7UL
#define P3_STAGE_PUBLISH        8UL

/* Probe shape. The default is the phase2-validated 32-batch shape; the
 * paced runs are opt-in via phase3host arguments. */
#define P3_BATCH_COUNT          32UL
#define P3_MAX_BATCHES          100000UL
#define P3_DEFAULT_PACE_US      0UL

/* The fence tail shape (unchanged from Phase 2). */
#define P2_BATCH_DWORDS             8UL
#define P2_FENCE_TAIL_DWORDS        6UL
#define P2_PADDED_DWORDS            16UL
#define P2_PACKET2                  0x80000000UL
#define P2_PACKET0(reg)             ((ULONG)(reg) >> 2)
#define P2_DSTCACHE_CTLSTAT         0x1714UL
#define P2_WAIT_UNTIL               0x1720UL
#define P2_CACHE_FLUSH_ALL          0x0000000fUL
#define P2_WAIT_IDLE \
    ((1UL << 9) | (1UL << 16) | (1UL << 17) | (1UL << 18))
#define P2_SCRATCH_REG0             0x15e0UL
#define P2_RESERVE_TIMEOUT          200000UL

#endif /* PHASE3_REGS_H */
