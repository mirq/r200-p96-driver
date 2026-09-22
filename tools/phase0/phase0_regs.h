#ifndef PHASE0_REGS_H
#define PHASE0_REGS_H

/*
 * Shared constants for the Phase-0 PPC reachability probe
 * (docs/09-ppc-direct-ring-design.md, section 7).
 *
 * Both sides of the probe (the 68k companion tools/phase0/phase0_control.c
 * and the WarpOS PPC client tools/phase0/ppcphase0.c) include this header.
 * It must contain only fixed-width types: the control block crosses CPUs
 * with no shared compiler ABI. All dwords are plain ULONG and the block
 * lives in Radeon VRAM, which both CPUs access uncached through their
 * apertures.
 */

/* Control block magic / stage markers ("P0CB", "HOST", "PPC1"). */
#define P0_MAGIC            0x50304342UL
#define P0_VERSION          1UL
#define P0_HOST_DONE        0x484f5354UL
#define P0_PPC_ACK          0x50504331UL

/* Control-block dword indexes. See docs/09 for the field table. */
#define P0_I_MAGIC              0UL
#define P0_I_VERSION            1UL
#define P0_I_HOST_DONE          2UL
#define P0_I_PPC_ACK            3UL
#define P0_I_BAR0_CPU           4UL
#define P0_I_BAR0_BYTES         5UL
#define P0_I_BAR2_CPU           6UL
#define P0_I_BAR2_BYTES         7UL
#define P0_I_ARENA_CPU          8UL
#define P0_I_ARENA_BYTES        9UL
#define P0_I_CONTROL_BYTES      10UL
#define P0_I_HOST_MMIO_READ_NS  11UL
#define P0_I_HOST_MMIO_WRITE_NS 12UL
#define P0_I_HOST_MMIO_COMMIT_NS 13UL
#define P0_I_HOST_APER_NS_DWORD 14UL
#define P0_I_HOST_ECLK_HZ       15UL
#define P0_I_FLAGS              16UL
#define P0_I_HOST_SEG_ID        17UL
#define P0_I_ARENA_SEG_ID       18UL
#define P0_I_PPC_STATUS         19UL
#define P0_I_PPC_STAGE          20UL
#define P0_I_PPC_MMIO_READ_NS   21UL
#define P0_I_PPC_MMIO_WRITE_NS  22UL
#define P0_I_PPC_MMIO_COMMIT_NS 23UL
#define P0_I_PPC_APER_NATIVE_KBPS 24UL
#define P0_I_PPC_APER_BR_KBPS   25UL
#define P0_I_PPC_APER_READ_NS   26UL
#define P0_I_PPC_SELFREAD       27UL
#define P0_I_PPC_STWBRX_OK      28UL
#define P0_I_PPC_MMIO_SCRATCH   29UL
#define P0_I_COUNT              32UL

/* P0_I_FLAGS bits. */
#define P0_FLAG_MMIO_WRITE_OK   (1UL << 0)

/* PPC failure stages (written to P0_I_PPC_STAGE). */
#define P0_STAGE_ARGS           1UL
#define P0_STAGE_BLOCK_MAGIC    2UL
#define P0_STAGE_VERSION        3UL
#define P0_STAGE_HOST_DONE      4UL
#define P0_STAGE_BAR2_RANGE     5UL
#define P0_STAGE_ARENA_RANGE    6UL
#define P0_STAGE_MMIO_READ      7UL
#define P0_STAGE_MMIO_WRITE     8UL
#define P0_STAGE_APER_STORE     9UL
#define P0_STAGE_APER_READ      10UL
#define P0_STAGE_STWBRX         11UL
#define P0_STAGE_PUBLISH        12UL

/*
 * RV280 register offsets used by the probe (copies of the constants in
 * src/radeon_regs.h; the probe is standalone and does not link the driver).
 *
 * Read targets are always safe: the driver itself polls RBBM_STATUS,
 * CP_RB_RPTR and SCRATCH_REG0 in normal operation.
 *
 * The only PPC-side MMIO write target is SCRATCH_REG1. The deployed driver's
 * fence path uses SCRATCH_REG0 only, and the CP reads SCRATCH_REG1 solely
 * when a ring packet addresses it, so a bare register write cannot disturb
 * ring work in flight. The 68k companion proves CP quiescence with a fenced
 * PACKET2 batch before publishing the block.
 */
#define P0_RBBM_STATUS          0x0e40UL
#define P0_CP_RB_CNTL           0x0704UL
#define P0_CP_RB_RPTR           0x0710UL
#define P0_CP_RB_WPTR           0x0714UL
#define P0_CP_CSQ_STAT          0x07f8UL
#define P0_SCRATCH_REG0         0x15e0UL
#define P0_SCRATCH_REG1         0x15e4UL

/* Probe sizes. The control segment is small; the arena matches one full
 * streaming segment (RADEON3D_MAX_SEGMENT_BYTES) so bursts are not bounded
 * by the lease. */
#define P0_CONTROL_BYTES        4096UL
#define P0_ARENA_BYTES          (256UL * 1024UL)

/* Measurement loop sizes. */
#define P0_MMIO_ITERATIONS      4096UL
#define P0_MMIO_COMMIT_ITERS    1024UL
#define P0_APER_BURST_BYTES     (8UL * 1024UL)
#define P0_APER_PASSES          64UL

#define P0_APER_PATTERN         0x5a5aa5a5UL

#endif /* PHASE0_REGS_H */
