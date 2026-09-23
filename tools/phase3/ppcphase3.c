/*
 * Phase-3 sustained-lease gate, WarpOS PPC writer (layout v2, expert-review
 * redesign). docs/09-ppc-direct-ring-design.md
 *
 * Usage (after Work:phase3host prints the address):
 *
 *     Work:ppcphase3 <controlSegmentAddressHex>
 *
 * The 68k host holds an interface-20 engine lease: during the lease the
 * PPC is the only ring writer. This probe replicates CpCommitStream()'s
 * invariants from the PPC side, entirely local:
 *
 *   1. dcbf-invalidate, read and validate the host-owned magic/version;
 *   2. publish PPC_PRESENT once (sticky, never erased) through the
 *      review's publish helper (store, eieio/sync, dcbf, fresh readback);
 *   3. wait for HOST_READY with dcbf'd polls, then invalidate the
 *      descriptor lines again and read the lease descriptors;
 *   4. per batch: reserve 16 dwords by polling RB_RPTR (bounded), write
 *      8 PACKET2 dwords + the 6-dword fence tail + 2 pad PACKET2s with
 *      stwbrx bursts (wrap split), read back the final dword, then publish
 *      WPTR with a committed (write + readback) MMIO write;
 *   5. the tails carry an increasing SCRATCH_REG0 sequence, so the final
 *      local poll proves every lease batch drained;
 *   6. publish results in the PPC-owned region, then PPC_DONE last.
 *
 * The child writes ONLY dwords 32..63 (its own cache-line-separated
 * region); the host writes only dwords 0..31.
 *
 * Exit codes: 0 ok, 10 stage failure, 20 usage.
 */

#include <exec/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phase3_regs.h"

struct PPCBase;
extern struct PPCBase *PowerPCBase;

struct P3TimeVal {
    ULONG tv_secs;
    ULONG tv_micro;
};

VOID __GetSysTimePPC(void *, struct P3TimeVal *) =
    "\tlwz\tr0,-682(r3)\n\tmtlr\tr0\n\tblrl";
#define GetSysTimePPC(value) __GetSysTimePPC(PowerPCBase, (value))

extern ULONG P0MmioRead32(volatile void *address);
extern void P0MmioWrite32(volatile void *address, ULONG value);
extern ULONG P0MmioWriteCommit(volatile void *address, ULONG value);
extern void P0WriteBarrier(void);
extern void P0FlushRange(void *address, ULONG bytes);
extern void P0CopyBr(volatile ULONG *destination, const ULONG *source,
                     ULONG dwords);
extern ULONG P0SumBurst(volatile ULONG *source, ULONG dwords);

static ULONG ElapsedMicros(const struct P3TimeVal *start,
                           const struct P3TimeVal *end)
{
    ULONG seconds = end->tv_secs - start->tv_secs;
    LONG micros = (LONG)end->tv_micro - (LONG)start->tv_micro;

    if (micros < 0) {
        --seconds;
        micros += 1000000L;
    }
    return seconds * 1000000UL + (ULONG)micros;
}

/* Fresh read of a host-owned dword: dcbf (write-back + invalidate) then a
 * real load, with an acquire barrier after the terminal observation. */
static ULONG PpcReadFresh(volatile ULONG *address)
{
    ULONG value;

    P0FlushRange((void *)address, sizeof(ULONG));
    value = *address;
    return value;
}

/* Publication of a single PPC-owned dword: store, eieio/sync, dcbf
 * (write-back + invalidate), fresh readback, eieio/sync. */
static void PpcPublishDword(volatile ULONG *address, ULONG value)
{
    *address = value;
    P0WriteBarrier();
    P0FlushRange((void *)address, sizeof(ULONG));
    (void)*address;
    P0WriteBarrier();
}

/* Publish the whole PPC-owned region: payload stores, barrier, dcbf over
 * the region, fresh readback, then PPC_DONE last. */
static void PpcPublishResults(volatile ULONG *block, ULONG stage,
                              ULONG generation, ULONG submits,
                              ULONG lastFence, ULONG retired)
{
    ULONG index;

    block[P3_I_SUBMITS] = submits;
    block[P3_I_LAST_FENCE] = lastFence;
    block[P3_I_RETIRED] = retired;
    block[P3_I_ERROR_STAGE] = stage;
    block[P3_I_PPC_GEN_ECHO] = generation;
    P0WriteBarrier();
    P0FlushRange((void *)(block + P3_I_PPC_PRESENT), 128UL);
    for (index = P3_I_SUBMITS; index < P3_I_PPC_END; ++index)
        (void)block[index];
    P0WriteBarrier();
    PpcPublishDword(block + P3_I_PPC_DONE, P3_PPC_DONE);
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
 * with PACKET2 for the CP's 16-byte ring alignment. */
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
    volatile ULONG *ring;
    struct P3TimeVal start;
    struct P3TimeVal end;
    ULONG controlAddress = 0;
    ULONG ringCpu = 0;
    ULONG ringDwords = 0;
    ULONG ringMask = 0;
    ULONG wptr;
    ULONG nextFence = 0;
    ULONG sequence = 0;
    ULONG batches = 0;
    ULONG paceUs = 0;
    ULONG lastFence = 0;
    ULONG submits = 0;
    ULONG retired = 0;
    ULONG submitMicros = 0;
    ULONG stage = P3_STAGE_BLOCK;
    ULONG validated = 0;
    ULONG ppcGeneration = 0;
    ULONG poll;
    ULONG index;
    ULONG batch;
    volatile ULONG paceSink = 0;
    int result = 20;

    if (argc != 2)
        goto publish;
    controlAddress = ParseAddress(argv[1]);
    if (!controlAddress || (controlAddress & 3UL))
        goto publish;

    block = (volatile ULONG *)controlAddress;
    stage = P3_STAGE_BLOCK;
    P0FlushRange((void *)block, sizeof(ULONG));
    if (block[P3_I_MAGIC] != P3_MAGIC)
        goto publish;
    P0FlushRange((void *)block, sizeof(ULONG));
    if (block[P3_I_VERSION] != P3_VERSION)
        goto publish;
    validated = 1;

    /* Clear any stale PPC-owned state left by an earlier run (VRAM
     * persists across a reboot, and this region is child-owned: only the
     * child may clear it). Then echo this run's generation before any
     * signal, so the host discards a previous run's stale DONE/PRESENT. */
    P0FlushRange((void *)(block + P3_I_PPC_PRESENT), 128UL);
    block[P3_I_PPC_DONE] = 0UL;
    block[P3_I_ERROR_STAGE] = 0UL;
    block[P3_I_SUBMITS] = 0UL;
    block[P3_I_LAST_FENCE] = 0UL;
    block[P3_I_RETIRED] = 0UL;
    P0WriteBarrier();
    P0FlushRange((void *)(block + P3_I_PPC_PRESENT), 128UL);
    for (index = P3_I_PPC_PRESENT; index < P3_I_PPC_END; ++index)
        (void)block[index];
    P0WriteBarrier();
    {
        ULONG generation;

        P0FlushRange((void *)(block + P3_I_GENERATION),
                     sizeof(ULONG));
        generation = block[P3_I_GENERATION];
        PpcPublishDword(block + P3_I_PPC_GEN_ECHO, generation);
        ppcGeneration = generation;
    }

    /* Announce presence once (sticky, never erased by this child), then
     * wait for the host's descriptors. Every poll invalidates the host
     * lines first: PPC loads of this window are cache-served. */
    PpcPublishDword(block + P3_I_PPC_PRESENT, P3_PPC_PRESENT);
    stage = P3_STAGE_HOST_WAIT;
    for (poll = 0; poll < 600000UL; ++poll) {
        volatile ULONG sink = 0;

        P0FlushRange((void *)(block + P3_I_HOST_READY),
                     sizeof(ULONG));
        if (block[P3_I_HOST_READY] == P3_HOST_READY)
            break;
        for (index = 0; index < 2000UL; ++index)
            sink ^= index;
        (void)sink;
        if (poll >= 600000UL - 1UL)
            goto publish;
    }
    if (block[P3_I_HOST_READY] != P3_HOST_READY)
        goto publish;
    stage = P3_STAGE_RING_RANGE;
    P0FlushRange((void *)(block + P3_I_RING_CPU), 32UL);
    if (!block[P3_I_RING_CPU] || !block[P3_I_RING_DWORDS] ||
        !block[P3_I_RING_MASK] || !block[P3_I_BAR2])
        goto publish;
    P0WriteBarrier();

    ringCpu = block[P3_I_RING_CPU];
    ringDwords = block[P3_I_RING_DWORDS];
    ringMask = block[P3_I_RING_MASK];
    bar2 = (volatile UBYTE *)(ULONG)block[P3_I_BAR2];
    rbRptr = (volatile void *)((volatile UBYTE *)bar2 + 0x0710UL);
    rbWptr = (volatile void *)((volatile UBYTE *)bar2 + 0x0714UL);
    scratch0 = (volatile void *)((volatile UBYTE *)bar2 + P2_SCRATCH_REG0);
    nextFence = block[P3_I_NEXT_FENCE];
    if (!nextFence)
        nextFence = 1UL;
    batches = block[P3_I_BATCHES];
    if (!batches || batches > P3_MAX_BATCHES)
        batches = P3_BATCH_COUNT;
    paceUs = block[P3_I_PACE_US];
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
        struct P3TimeVal paceStart;
        struct P3TimeVal paceEnd;

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
                stage = P3_STAGE_RESERVE;
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
            stage = P3_STAGE_READBACK;
            goto publish;
        }

        /* Publish WPTR with the driver's committed shape. */
        newWptr = (wptr + P2_PADDED_DWORDS) & ringMask;
        if (P0MmioWriteCommit(rbWptr, newWptr) != newWptr) {
            stage = P3_STAGE_WPTR;
            goto publish;
        }
        wptr = newWptr;
        lastFence = sequence;
        ++submits;

        /* EClock-paced delay between batches (layout v2: the old spin
         * loop was mis-calibrated ~30x). */
        if (paceUs) {
            GetSysTimePPC(&paceStart);
            for (;;) {
                GetSysTimePPC(&paceEnd);
                if (ElapsedMicros(&paceStart, &paceEnd) >= paceUs)
                    break;
                for (index = 0; index < 2000UL; ++index)
                    paceSink ^= index;
            }
            (void)paceSink;
        }
    }
    GetSysTimePPC(&end);
    submitMicros = ElapsedMicros(&start, &end);

    /* Local fence retirement: SCRATCH_REG0 advances in ring order, so the
     * last sequence proves every lease batch drained. Bounded poll. */
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
            stage = P3_STAGE_FENCE;
            goto publish;
        }
    } else {
        retired = 1UL;
    }

    stage = 0UL;
    result = 0;

publish:
    if (block && validated) {
        PpcPublishResults(block, stage, ppcGeneration, submits,
                          lastFence, retired);
    }

    printf("PPCPHASE3 status=%s stage=%lu submits=%lu last_fence=%lu "
           "retired=%lu us=%lu\n",
           result ? "fail" : "ok",
           (unsigned long)stage,
           (unsigned long)submits,
           (unsigned long)lastFence,
           (unsigned long)retired,
           (unsigned long)submitMicros);
    if (result == 20)
        printf("PPCPHASE3 hint=usage: ppcphase3 <hexAddress>\n");
    return result;
}
