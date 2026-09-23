/*
 * Phase-3 sustained-lease gate, 68k host (layout v2, expert-review redesign).
 * docs/09-ppc-direct-ring-design.md
 *
 *   1. open the chip by resident name, open an interface-20 session,
 *      require RADEON3D_CAP_PPC_RING;
 *   2. prove the CP with a fenced PACKET2 batch (before the grant);
 *   3. lease a 4 KiB control-block segment and publish the host-owned
 *      region (payload first, HOST_READY last, each store drained by a
 *      readback through the posted-write CI window);
 *   4. wait for the child: PPC_DONE first (a late host reports "child
 *      arrived and timed out" instead of "never arrived"), then
 *      PPC_PRESENT (sticky, never erased by the child's exit);
 *   5. grant the lease, re-check PPC_DONE (the child may have timed out
 *      during the grant), publish the descriptors (drain), then wait for
 *      the child's results while renewing the lease with
 *      Radeon3DHeartbeatLease() on EClock-measured 500 ms cadence;
 *   6. release with the PPC's last fence; cross-check SCRATCH_REG0.
 *
 * The host writes ONLY its own region (dwords 0..31); the child writes only
 * dwords 32..63. This ends the 2026-09-23 failure where the child's exit
 * publish erased the host's pending arrival signal and dirty cache lines
 * mixed ownership.
 */

#include <devices/timer.h>
#include <exec/memory.h>
#include <hardware/cia.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/radeon3d.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "phase3_regs.h"

struct Library *Radeon9200Base;

struct Phase3Timer {
    struct MsgPort Port;
    struct timerequest Request;
    ULONG Rate;
};

static BOOL OpenEclock(struct Phase3Timer *timer)
{
    struct EClockVal value;
    struct Device *TimerBase;

    timer->Port.mp_Node.ln_Type = NT_MSGPORT;
    timer->Port.mp_Flags = PA_IGNORE;
    timer->Port.mp_MsgList.lh_Head =
        (struct Node *)&timer->Port.mp_MsgList.lh_Tail;
    timer->Port.mp_MsgList.lh_Tail = NULL;
    timer->Port.mp_MsgList.lh_TailPred =
        (struct Node *)&timer->Port.mp_MsgList.lh_Head;
    timer->Request.tr_node.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    timer->Request.tr_node.io_Message.mn_ReplyPort = &timer->Port;
    timer->Request.tr_node.io_Message.mn_Length =
        sizeof(timer->Request);
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK,
                   (struct IORequest *)&timer->Request, 0))
        return FALSE;
    timer->Rate = 0;
    TimerBase = timer->Request.tr_node.io_Device;
    timer->Rate = ReadEClock(&value);
    return timer->Rate != 0;
}

static void CloseEclock(struct Phase3Timer *timer)
{
    if (timer->Rate)
        CloseDevice((struct IORequest *)&timer->Request);
    timer->Rate = 0;
}

static ULONG Now(struct Phase3Timer *timer)
{
    struct EClockVal value;
    struct Device *TimerBase = timer->Request.tr_node.io_Device;

    ReadEClock(&value);
    return value.ev_lo;
}

/* EClock-paced delay in microseconds (replaces the mis-calibrated
 * BusyUs spin; see the layout comment). */
static void WaitUs(struct Phase3Timer *timer, ULONG microseconds)
{
    ULONG start = Now(timer);
    ULONG ticks = (ULONG)(((unsigned long long)microseconds *
                           timer->Rate) / 1000000ULL);

    if (!timer->Rate) {
        volatile struct CIA *cia = (volatile struct CIA *)0x00bfe001UL;
        ULONG count = (microseconds << 4) + 15UL;

        count /= 22UL;
        while (count--) {
            volatile UBYTE value = cia->ciapra;

            (void)value;
        }
        return;
    }
    while ((ULONG)(Now(timer) - start) < ticks)
        ;
}

/* 68k publication: payload stores first, then a same-window readback to
 * drain the posted writes, then the flag dword with its own readback.
 * The aperture is uncached on this side; no cache barriers are used. */
static void PublishHostPayload(volatile ULONG *block, const ULONG *values)
{
    ULONG index;

    for (index = 0; index < P3_I_HOST_END; ++index)
        block[index] = values[index];
    __asm__ __volatile__ ("" ::: "memory");
    (void)block[P3_I_HOST_END - 1UL];
    __asm__ __volatile__ ("" ::: "memory");
}

static ULONG ReadMmio(ULONG bar2, ULONG reg)
{
    volatile ULONG *address = (volatile ULONG *)(bar2 + reg);
    ULONG value = *address;

    __asm__ __volatile__ ("" ::: "memory");
    return __builtin_bswap32(value);
}

int main(int argc, char **argv)
{
    struct Phase3Timer timer;
    struct Radeon3DInfo info;
    struct Radeon3DDevice *device = NULL;
    struct Radeon3DSegment control;
    struct Radeon3DLease lease;
    volatile ULONG *block;
    ULONG values[P3_I_HOST_END];
    ULONG fence = 0;
    ULONG lastFence;
    ULONG index;
    ULONG batchCount;
    ULONG paceUs;
    ULONG now;
    ULONG generation;
    ULONG arrivalPolls;
    ULONG arrivalLogged;
    ULONG arrivalGen;
    ULONG arrivalDone;
    ULONG arrivalPresent;
    ULONG scratch68k;
    ULONG start;
    ULONG deadline;
    ULONG lastRenew;
    BOOL timerOpen = FALSE;
    BOOL leased = FALSE;
    BOOL controlLeased = FALSE;
    int result = 20;

    static const ULONG noopBatch[4] = {
        0x80000000UL, 0x80000000UL, 0x80000000UL, 0x80000000UL
    };

    memset(&timer, 0, sizeof(timer));
    memset(&control, 0, sizeof(control));
    memset(&lease, 0, sizeof(lease));
    lease.Size = sizeof(lease);

    /* Defaults: the phase2-validated shape. The paced/wrap runs are
     * opt-in (the first paced attempt wedged the machine and stays
     * suspect until understood). */
    batchCount = P3_BATCH_COUNT;
    paceUs = P3_DEFAULT_PACE_US;
    if (argc >= 3 && !strcmp(argv[1], "--batches")) {
        batchCount = strtoul(argv[2], NULL, 0);
        if (batchCount > P3_MAX_BATCHES)
            batchCount = P3_MAX_BATCHES;
        if (argc >= 5 && !strcmp(argv[3], "--pace-ms"))
            paceUs = strtoul(argv[4], NULL, 0) * 1000UL;
    }
    if (paceUs > 1000000UL)
        paceUs = 1000000UL;

    Radeon9200Base = OpenLibrary((CONST_STRPTR)"Radeon9200.chip",
                                 RADEON3D_LIBRARY_VERSION);
    if (!Radeon9200Base) {
        printf("P3HOST status=no_chip_library\n");
        return 20;
    }

    info.Size = sizeof(info);
    device = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
    if (!device) {
        printf("P3HOST status=service_open_failed\n");
        goto done;
    }
    if (info.Version < 20UL || !(info.Caps & RADEON3D_CAP_PPC_RING)) {
        printf("P3HOST status=ppc_ring_unavailable caps=%08lx iface=%lu\n",
               (unsigned long)info.Caps,
               (unsigned long)info.Version);
        goto done;
    }
    printf("P3HOST caps=%08lx iface=%lu batches=%lu pace_us=%lu\n",
           (unsigned long)info.Caps, (unsigned long)info.Version,
           (unsigned long)batchCount, (unsigned long)paceUs);

    if (!OpenEclock(&timer)) {
        printf("P3HOST status=no_timer\n");
        goto done;
    }
    timerOpen = TRUE;

    /* CP proof before the grant: the lease blocks service submissions. */
    if (!Radeon3DSubmit(device, noopBatch, 4UL, 1UL, &fence) || !fence) {
        printf("P3HOST status=cp_probe_submit_failed\n");
        goto done;
    }
    if (!Radeon3DWaitFence(device, fence, 2000UL)) {
        printf("P3HOST status=cp_probe_fence_timeout fence=%lu\n",
               (unsigned long)fence);
        goto done;
    }

    control.Size = sizeof(control);
    if (!Radeon3DAllocSegment(device, P0_CONTROL_BYTES, &control) ||
        control.Bytes != P0_CONTROL_BYTES) {
        printf("P3HOST status=control_segment_failed\n");
        goto done;
    }
    controlLeased = TRUE;

    /* Per-run token: the child echoes it into its region before its
     * signals, so the host can discard a previous run's stale DONE and
     * PRESENT (this host must not write the child's region). */
    block = (volatile ULONG *)control.CpuAddress;
    for (index = 0; index < P3_I_HOST_END; ++index)
        values[index] = 0;
    generation = Now(&timer) | 1UL;
    values[P3_I_MAGIC] = P3_MAGIC;
    values[P3_I_VERSION] = P3_VERSION;
    values[P3_I_GENERATION] = generation;
    values[P3_I_BATCHES] = batchCount;
    values[P3_I_PACE_US] = paceUs;
    values[P3_I_CONTROL_BYTES] = control.Bytes;
    PublishHostPayload(block, values);

    /* Wait for the child: DONE first (a child that timed out while this
     * host was initializing is a terminal result, not "never arrived"),
     * then the sticky PRESENT marker. */
    start = Now(&timer);
    deadline = start + timer.Rate * 120UL;
    arrivalPolls = 0UL;
    arrivalLogged = 0UL;
    arrivalGen = 0UL;
    arrivalDone = 0UL;
    arrivalPresent = 0UL;
    for (;;) {
        ULONG echo;
        ULONG done;
        ULONG present;

        now = Now(&timer);
        if ((LONG)(now - deadline) >= 0) {
            printf("P3HOST status=ppc_never_arrived done=%08lx "
                   "present=%08lx echo=%08lx gen=%08lx\n",
                   (unsigned long)block[P3_I_PPC_DONE],
                   (unsigned long)block[P3_I_PPC_PRESENT],
                   (unsigned long)block[P3_I_PPC_GEN_ECHO],
                   (unsigned long)generation);
            result = 10;
            goto done;
        }
        echo = block[P3_I_PPC_GEN_ECHO];
        done = block[P3_I_PPC_DONE];
        present = block[P3_I_PPC_PRESENT];
        ++arrivalPolls;
        if (echo != arrivalGen || done != arrivalDone ||
            present != arrivalPresent) {
            arrivalGen = echo;
            arrivalDone = done;
            arrivalPresent = present;
            if (arrivalLogged < 12UL) {
                printf("P3HOST arrival poll=%lu us=%lu echo=%08lx "
                       "done=%08lx present=%08lx\n",
                       (unsigned long)arrivalPolls,
                       (unsigned long)(now - start),
                       (unsigned long)echo,
                       (unsigned long)done,
                       (unsigned long)present);
            }
            ++arrivalLogged;
        }
        if (echo == generation) {
            /* This run's child is speaking (the generation echo matches).
             * DONE is trusted only alongside a cleared field set - the
             * child clears its region before signalling. */
            if (done == P3_PPC_DONE && present != P3_PPC_PRESENT) {
                printf("P3HOST status=child_done_before_lease "
                       "stage=%lu\n",
                       (unsigned long)block[P3_I_ERROR_STAGE]);
                result = 10;
                goto done;
            }
            if (present == P3_PPC_PRESENT && done != P3_PPC_DONE)
                break;
        }
        WaitUs(&timer, 20000UL);
    }
    (void)block[P3_I_PPC_PRESENT];

    /* From here to release, the PPC is the only ring writer. */
    if (!Radeon3DAcquireLease(device, &lease)) {
        printf("P3HOST status=lease_refused\n");
        goto done;
    }
    if (lease.Version != RADEON3D_LEASE_VERSION ||
        !lease.RingCpuAddress || !lease.RingDwords ||
        !lease.RingMask || !lease.Bar2Base) {
        printf("P3HOST status=bad_lease\n");
        goto done;
    }
    leased = TRUE;
    printf("P3HOST lease ring_cpu=%08lx ring_gpu=%08lx dwords=%lu "
           "mask=%08lx bar2=%08lx next_fence=%lu\n",
           (unsigned long)lease.RingCpuAddress,
           (unsigned long)lease.RingGpuAddress,
           (unsigned long)lease.RingDwords,
           (unsigned long)lease.RingMask,
           (unsigned long)lease.Bar2Base,
           (unsigned long)lease.NextFence);
    lastFence = lease.NextFence + batchCount - 1UL;
    if (!lastFence)
        ++lastFence;

    /* The child may have timed out while the lease was being granted:
     * re-check DONE before publishing the grant, and release if set. The
     * generation echo filters any stale completion from an earlier run. */
    if (block[P3_I_PPC_GEN_ECHO] == generation &&
        block[P3_I_PPC_DONE] == P3_PPC_DONE) {
        printf("P3HOST status=child_done_during_grant stage=%lu\n",
               (unsigned long)block[P3_I_ERROR_STAGE]);
        result = 10;
        goto done;
    }

    /* Publish the lease descriptors: payload stores, then a readback
     * drain of the posted writes. HOST_READY was published before the
     * child arrived and stays sticky; the child re-reads the descriptor
     * lines after accepting it. */
    for (index = 0; index < P3_I_HOST_END; ++index)
        values[index] = 0;
    values[P3_I_MAGIC] = P3_MAGIC;
    values[P3_I_VERSION] = P3_VERSION;
    values[P3_I_GENERATION] = generation;
    values[P3_I_RING_CPU] = (ULONG)lease.RingCpuAddress;
    values[P3_I_RING_GPU] = lease.RingGpuAddress;
    values[P3_I_RING_DWORDS] = lease.RingDwords;
    values[P3_I_RING_MASK] = lease.RingMask;
    values[P3_I_BAR2] = lease.Bar2Base;
    values[P3_I_NEXT_FENCE] = lease.NextFence;
    values[P3_I_BATCHES] = batchCount;
    values[P3_I_PACE_US] = paceUs;
    values[P3_I_CONTROL_BYTES] = control.Bytes;
    PublishHostPayload(block, values);

    /* The descriptor publication is the grant signal: HOST_READY last,
     * drained by its own readback. The child re-reads the descriptor
     * lines after accepting it. */
    block[P3_I_HOST_READY] = P3_HOST_READY;
    __asm__ __volatile__ ("" ::: "memory");
    (void)block[P3_I_HOST_READY];
    __asm__ __volatile__ ("" ::: "memory");

    /* Wait for the child's terminal result while renewing the lease on
     * EClock-measured 500 ms cadence (the paced run outlives the 5 s
     * expiry, so only the heartbeat can hold it). */
    start = Now(&timer);
    deadline = start + timer.Rate * 300UL;
    lastRenew = start;
    while (block[P3_I_PPC_DONE] != P3_PPC_DONE) {
        now = Now(&timer);
        if ((LONG)(now - deadline) >= 0) {
            printf("P3HOST status=ppc_timeout stage=%lu done=%08lx\n",
                   (unsigned long)block[P3_I_ERROR_STAGE],
                   (unsigned long)block[P3_I_PPC_DONE]);
            result = 10;
            goto done;
        }
        if ((ULONG)(now - lastRenew) > timer.Rate / 2UL) {
            (void)Radeon3DHeartbeatLease(device);
            lastRenew = Now(&timer);
        }
        WaitUs(&timer, 10000UL);
    }
    (void)block[P3_I_PPC_DONE];

    printf("P3PPC submits=%lu last_fence=%lu retired=%lu stage=%lu\n",
           (unsigned long)block[P3_I_SUBMITS],
           (unsigned long)block[P3_I_LAST_FENCE],
           (unsigned long)block[P3_I_RETIRED],
           (unsigned long)block[P3_I_ERROR_STAGE]);
    if (block[P3_I_ERROR_STAGE] ||
        block[P3_I_SUBMITS] != batchCount ||
        block[P3_I_LAST_FENCE] != lastFence ||
        block[P3_I_RETIRED] != 1UL) {
        result = 10;
        goto done;
    }
    lastFence = block[P3_I_LAST_FENCE];

    if (!Radeon3DReleaseLease(device, lastFence)) {
        struct Radeon3DInfo failinfo;

        memset(&failinfo, 0, sizeof(failinfo));
        failinfo.Size = sizeof(failinfo);
        if (Radeon3DGetInfo(device, &failinfo) &&
            failinfo.Size >= RADEON3D_INFO_V3_SIZE)
            printf("P3HOST status=release_failed "
                   "commit_fail_stage=%lu\n",
                   (unsigned long)failinfo.CommitFailStage);
        else
            printf("P3HOST status=release_failed "
                   "commit_fail_stage=unreadable\n");
        result = 10;
        goto done;
    }
    leased = FALSE;

    scratch68k = ReadMmio(lease.Bar2Base, P2_SCRATCH_REG0);
    printf("P3HOST release=ok scratch68k=%08lx expected=%08lx "
           "scratch_match=%lu\n",
           (unsigned long)scratch68k, (unsigned long)lastFence,
           (unsigned long)(scratch68k == lastFence));
    if (scratch68k != lastFence)
        result = 10;
    else {
        printf("P3HOST status=ok submits=%lu last_fence=%lu\n",
               (unsigned long)block[P3_I_SUBMITS],
               (unsigned long)lastFence);
        result = 0;
    }

done:
    (void)leased;
    if (controlLeased && device)
        (void)Radeon3DFreeSegment(device, control.Id);
    if (device)
        Radeon3DClose(device);
    if (Radeon9200Base) {
        CloseLibrary(Radeon9200Base);
        Radeon9200Base = NULL;
    }
    if (timerOpen)
        CloseEclock(&timer);
    return result;
}
