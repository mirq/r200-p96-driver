# Phase-0 PPC helpers (WarpOS, MPC7410). Shapes copied from the proven
# MiniGL WarpOS barriers in minigl_ppc/examples/minigl_warpos_pci_barrier.s
# and minigl_warpos_cache_flush.s.
#
# All Radeon registers are little-endian; the PPC is big-endian, so every
# register access goes through lwbrx/stwbrx (load/store word byte-reversed).
# The same stwbrx instruction is what a direct ring writer would use to
# publish CP dwords without a separate swap pass.

        .text
        .balign 16

# ULONG P0MmioRead32(volatile void *address)
# Load word byte-reversed (little-endian register), eieio to keep the
# access ordered against following I/O.
        .global _P0MmioRead32
_P0MmioRead32:
        lwbrx   3, 0, 3
        eieio
        blr

# void P0MmioWrite32(volatile void *address, ULONG value)
# Posted store, no readback: measures the pure posting cost.
        .global _P0MmioWrite32
_P0MmioWrite32:
        stwbrx  4, 0, 3
        eieio
        blr

# ULONG P0MmioWriteCommit(volatile void *address, ULONG value)
# Posted store plus readback of the same register: the committed cost the
# driver pays before publishing a ring pointer.
        .global _P0MmioWriteCommit
_P0MmioWriteCommit:
        stwbrx  4, 0, 3
        eieio
        lwbrx   3, 0, 3
        eieio
        blr

# void P0WriteBarrier(void)
# eieio + sync: orders all prior stores against following device access.
        .global _P0WriteBarrier
_P0WriteBarrier:
        eieio
        sync
        blr

# void P0FlushRange(void *address, ULONG bytes)
# dcbf over the range then sync: makes PPC cache-resident stores visible
# before the data is consumed by another agent (the 68k or the GPU).
        .global _P0FlushRange
_P0FlushRange:
        cmpwi   4, 0
        beqlr
        rlwinm  5, 3, 0, 0, 26
        add     6, 3, 4
.flush_loop:
        dcbf    0, 5
        addi    5, 5, 32
        cmplw   5, 6
        blt     .flush_loop
        sync
        blr

# void P0FillBurstNative(volatile ULONG *destination, ULONG value,
#                        ULONG dwords)
# Plain big-endian stores: the raw posted-store floor of the aperture.
        .global _P0FillBurstNative
_P0FillBurstNative:
        mtctr   5
.burst_native:
        stw     4, 0(3)
        addi    3, 3, 4
        bdnz    .burst_native
        blr

# void P0FillBurstBr(volatile ULONG *destination, ULONG value, ULONG dwords)
# stwbrx burst: the little-endian ring-dword shape, swap included.
        .global _P0FillBurstBr
_P0FillBurstBr:
        mtctr   5
.burst_br:
        stwbrx  4, 0, 3
        addi    3, 3, 4
        bdnz    .burst_br
        eieio
        blr

# ULONG P0CopyBr(volatile ULONG *destination, const ULONG *source,
#                ULONG dwords)
# Load source dwords and store them byte-reversed: the generic little-
# endian burst a ring writer uses (a ring dword must arrive on the bus in
# the little-endian shape the CP expects).
        .global _P0CopyBr
_P0CopyBr:
        mtctr   5
.copy_br:
        lwz     6, 0(4)
        stwbrx  6, 0, 3
        addi    4, 4, 4
        addi    3, 3, 4
        bdnz    .copy_br
        eieio
        blr

# ULONG P0SumBurst(volatile ULONG *source, ULONG dwords)
        .global _P0SumBurst
_P0SumBurst:
        mtctr   4
        li      5, 0
.sum_loop:
        lwz     6, 0(3)
        xor     5, 5, 6
        addi    3, 3, 4
        bdnz    .sum_loop
        mr      3, 5
        eieio
        blr
