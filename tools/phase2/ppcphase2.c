/*
 * Phase-2 PPC ring writer (WarpOS). docs/09-ppc-direct-ring-design.md.
 *
 * Usage (after Work:phase2host prints the address):
 *
 *     Work:ppcphase2 <controlSegmentAddressHex>
 *
 * The 68k host holds an interface-20 engine lease: during the lease the
 * PPC is the only ring writer. This probe replicates CpCommitStream()'s
 * invariants from the PPC side, entirely local (no library calls and no
 * 68k involvement between the host's ack and its release):
 *
 *   1. sync the write-pointer shadow from CP_RB_WPTR (the ring is drained
 *      at grant; space math uses RB_RPTR);
 *   2. per batch: reserve 16 dwords by polling RB_RPTR (bounded), write
 *      8 PACKET2 dwords + the 6-dword fence tail + 2 pad PACKET2s with
 *      byte-reversed bursts (wrap split), read back the final dword
 *      (posted-write ordering), then publish WPTR with a committed
 *      (write + readback) MMIO write;
 *   3. the tails carry an increasing SCRATCH_REG0 sequence, so the final
 *      local poll proves every lease batch drained with zero 68k calls;
 *   4. publish results into the control block and acknowledge.
 *
 * Exit codes: 0 ok, 10 stage failure, 20 usage.
 */

#include <exec/types.h>
#include <stdio.h>
#include <string.h>

#include "phase2_regs.h"

struct PPCBase;
extern struct PPCBase *PowerPCBase;

struct timeval {
    ULONG tv_secs;
    ULONG tv_micro;
};

VOID __GetSysTimePPC(void *, struct timeval *) =
    "\tlwz\tr0,-682(r3)\n\tmtlr\tr0\n\tblrl";
#define GetSysTimePPC(value) __GetSysTimePPC(PowerPCBase, (value))

extern ULONG P0MmioRead32(volatile void *address);
extern void P0MmioWrite32(volatile void *address, ULONG value);
extern ULONG P0MmioWriteCommit(volatile void *address, ULONG value);
extern void P0WriteBarrier(void);
extern void P0FlushRange(void *address, ULONG bytes);
extern void P0FillBurstBr(volatile ULONG *destination, ULONG value,
                          ULONG dwords);
extern void P0CopyBr(volatile ULONG *destination, const ULONG *source,
                     ULONG dwords);
extern ULONG P0SumBurst(volatile ULONG *source, ULONG dwords);

static ULONG ElapsedMicros(const struct timeval *start,
                           const struct timeval *end)
{
    ULONG seconds = end->tv_secs - start->tv_secs;
    LONG micros = (LONG)end->tv_micro - (LONG)start->tv_micro;

    if (micros < 0) {
        --seconds;
        micros += 1000000L;
    }
    return seconds * 1000000UL + (ULONG)micros;
}

static ULONG ParseAddress(const char *text)
{
    ULONG value = 0;
    ULONG index = 0;

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        index = 2;
    for (; text[index]; ++index) {
        char digit = text[index];

        if (digit >= '0' && digit <= '9')
            value = (value << 4) | (ULONG)(digit - '0');
        else if (digit >= 'a' && digit <= 'f')
            value = (value << 4) | (ULONG)(digit - 'a' + 10);
        else if (digit >= 'A' && digit <= 'F')
            value = (value << 4) | (ULONG)(digit - 'A' + 10);
        else
            return 0;
    }
    return value;
}

/* One lease batch: 8 PACKET2 dwords, then the 6-dword fence tail
 * (cache flush, full idle wait, scratch sequence), padded to 16 dwords
 * with PACKET2 for the CP's 16-byte ring alignment. The tail ordering
 * matches CpStreamDword() in src/radeon_cp.c. */
static void BuildBatch(ULONG *stream, ULONG sequence)
{
    ULONG index;

    for (index = 0; index < P2_BATCH_DWORDS; ++index)
        stream[index] = P2_PACKET2;
    stream[8UL] = P2_PACKET0(P2_DSTCACHE_CTLSTAT);
    stream[9UL] = P2_CACHE_FLUSH_ALL;
    stream[10UL] = P2_PACKET0(P2_WAIT_UNTIL);
    stream[11UL] = P2_WAIT_IDLE;
    stream[12UL] = P2_PACKET0(P2_SCRATCH_REG0);
    stream[13UL] = sequence;
    for (index = 14UL; index < P2_PADDED_DWORDS; ++index)
        stream[index] = P2_PACKET2;
}

int main(int argc, char **argv)
{
    volatile ULONG *block = NULL;
    volatile UBYTE *bar2;
    volatile void *rbRptr;
    volatile void *rbWptr;
    volatile void *scratch0;
    volatile ULONG *ringBase;
    volatile ULONG *ring;
    struct timeval start;
    struct timeval end;
    ULONG controlAddress = 0;
    ULONG ringCpu = 0;
    ULONG ringDwords = 0;
    ULONG ringMask = 0;
    ULONG wptr;
    ULONG nextFence = 0;
    ULONG sequence = 0;
    ULONG batches = 0;
    ULONG lastFence = 0;
    ULONG submits = 0;
    ULONG retired = 0;
    ULONG submitMicros = 0;
    ULONG stage = P2_STAGE_BLOCK;
    ULONG validated = 0;
    ULONG poll;
    ULONG index;
    ULONG batch;
    int result = 20;

    if (argc != 2)
        goto publish;
    controlAddress = ParseAddress(argv[1]);
    if (!controlAddress || (controlAddress & 3UL))
        goto publish;

    block = (volatile ULONG *)controlAddress;
    stage = P2_STAGE_BLOCK;
    if (block[P2_I_MAGIC] != P2_MAGIC)
        goto publish;
    if (block[P2_I_VERSION] != P2_VERSION)
        goto publish;
    validated = 1;
    /* Announce presence so the 68k host can grant the lease without a
     * timing race, then wait for the lease block (bounded; reads of VRAM
     * are always safe). */
    block[P2_I_ERROR_STAGE] = 0xffffffffUL;
    P0WriteBarrier();
    (void)block[P2_I_ERROR_STAGE];
    stage = P2_STAGE_RESERVE;
    for (poll = 0; poll < 600000UL; ++poll) {
        volatile ULONG sink = 0;

        /* The PPC's loads of the aliased VRAM window are cache-served
         * (Phase-0 run 1 measured 29 ns cache hits): invalidate the block's
         * line before every poll so the HOST_DONE store the 68k published
         * becomes visible. dcbf also writes the marker line back. */
        P0FlushRange((void *)block, 64UL);
        if (block[P2_I_HOST_DONE] == P2_HOST_DONE)
            break;
        for (index = 0; index < 2000UL; ++index)
            sink ^= index;
        (void)sink;
        if (poll >= 600000UL - 1UL)
            goto publish;
    }
    if (block[P2_I_HOST_DONE] != P2_HOST_DONE)
        goto publish;
    stage = P2_STAGE_RING_RANGE;
    if (!block[P2_I_RING_CPU] || !block[P2_I_RING_DWORDS] ||
        !block[P2_I_RING_MASK] || !block[P2_I_BAR2])
        goto publish;

    ringCpu = block[P2_I_RING_CPU];
    ringDwords = block[P2_I_RING_DWORDS];
    ringMask = block[P2_I_RING_MASK];
    bar2 = (volatile UBYTE *)(ULONG)block[P2_I_BAR2];
    rbRptr = bar2 + P0_CP_RB_RPTR;
    rbWptr = bar2 + P0_CP_RB_WPTR;
    scratch0 = bar2 + P2_SCRATCH_REG0;
    nextFence = block[P2_I_NEXT_FENCE];
    if (!nextFence)
        nextFence = 1UL;
    batches = block[P2_I_BATCHES];
    if (!batches || batches > 4096UL)
        batches = P2_BATCH_COUNT;
    ring = (volatile ULONG *)ringCpu;

    /* The ring is drained at grant; WPTR is where the next dword goes. */
    wptr = P0MmioRead32(rbWptr) & ringMask;

    GetSysTimePPC(&start);
    for (batch = 0; batch < batches; ++batch) {
        ULONG stream[P2_PADDED_DWORDS];
        ULONG space;
        ULONG newWptr;
        ULONG readback;
        ULONG expectedLast;
        ULONG firstRun;

        sequence = nextFence + batch;
        if (!sequence)
            ++sequence;
        BuildBatch(stream, sequence);
        expectedLast = stream[P2_PADDED_DWORDS - 1UL];

        /* Space: available = (rptr - wptr - 1) & mask. */
        for (poll = 0;; ++poll) {
            space = (P0MmioRead32(rbRptr) & ringMask);
            space = (space - wptr - 1UL) & ringMask;
            if (space >= P2_PADDED_DWORDS)
                break;
            if (poll >= P2_RESERVE_TIMEOUT) {
                stage = P2_STAGE_RESERVE;
                goto publish;
            }
        }

        /* Write the stream at wptr, splitting at the ring wrap. */
        firstRun = ringDwords - wptr;
        if (firstRun > P2_PADDED_DWORDS)
            firstRun = P2_PADDED_DWORDS;
        P0CopyBr(ring + wptr, stream, firstRun);
        if (firstRun < P2_PADDED_DWORDS)
            P0CopyBr(ring, stream + firstRun,
                     P2_PADDED_DWORDS - firstRun);

        /* Posted-write ordering: the final dword must read back. */
        readback = P0MmioRead32((volatile void *)
                                (ring + ((wptr + P2_PADDED_DWORDS - 1UL) &
                                         ringMask)));
        if (readback != expectedLast) {
            stage = P2_STAGE_READBACK;
            goto publish;
        }

        /* Publish WPTR with the driver's committed shape. */
        newWptr = (wptr + P2_PADDED_DWORDS) & ringMask;
        if (P0MmioWriteCommit(rbWptr, newWptr) != newWptr) {
            stage = P2_STAGE_WPTR;
            goto publish;
        }
        wptr = newWptr;
        lastFence = sequence;
        ++submits;
    }
    GetSysTimePPC(&end);
    submitMicros = ElapsedMicros(&start, &end);

    /* Local fence retirement: SCRATCH_REG0 advances in ring order, so the
     * last sequence proves every lease batch drained. Bounded poll with a
     * compute delay between reads. */
    if (lastFence) {
        for (poll = 0; poll < P2_RESERVE_TIMEOUT; ++poll) {
            ULONG current = P0MmioRead32(scratch0);
            volatile ULONG sink = 0;

            if ((LONG)(current - lastFence) >= 0) {
                retired = 1UL;
                break;
            }
            for (index = 0; index < 2000UL; ++index)
                sink ^= index;
            (void)sink;
        }
        if (!retired) {
            stage = P2_STAGE_FENCE;
            goto publish;
        }
    } else {
        retired = 1UL;
    }

    stage = 0UL;
    result = 0;

publish:
    if (block && validated) {
        block[P2_I_SUBMITS] = submits;
        block[P2_I_LAST_FENCE] = lastFence;
        block[P2_I_RETIRED] = retired;
        block[P2_I_ERROR_STAGE] = stage;
        P0WriteBarrier();
        (void)block[P2_I_ERROR_STAGE];
        P0WriteBarrier();
        block[P2_I_PPC_ACK] = P2_PPC_ACK;
        P0WriteBarrier();
        (void)block[P2_I_PPC_ACK];
    }

    printf("PPCPHASE2 status=%s stage=%lu submits=%lu last_fence=%lu "
           "retired=%lu us=%lu\n",
           result ? "fail" : "ok",
           (unsigned long)stage,
           (unsigned long)submits,
           (unsigned long)lastFence,
           (unsigned long)retired,
           (unsigned long)submitMicros);
    if (result == 20)
        printf("PPCPHASE2 hint=usage: ppcphase2 <hexAddress>\n");
    return result;
}
