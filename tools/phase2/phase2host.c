/*
 * Phase-2 68k host: grants an interface-20 PPC engine lease and verifies a
 * WarpOS PPC ring writer end to end (docs/09-ppc-direct-ring-design.md).
 *
 *   1. open the chip by resident name, open an interface-20 session,
 *      require RADEON3D_CAP_PPC_RING;
 *   2. prove the CP with a fenced PACKET2 batch (service submissions are
 *      blocked during the lease, so this happens before the grant);
 *   3. lease a 4 KiB control-block segment, Radeon3DAcquireLease(), and
 *      publish the ring/MMIO descriptors plus the batch count;
 *   4. print the exact command line for Work:ppcphase2 and wait (bounded)
 *      for the PPC to submit its batches and acknowledge;
 *   5. Radeon3DReleaseLease(lastFence) drains via the PPC's fence and
 *      re-arms the 68k state;
 *   6. cross-check SCRATCH_REG0 through the 68k aperture: it must equal
 *      the PPC's last fence.
 *
 * All exit paths release the segment and the session; the lease dies with
 * the session on any abort path.
 */

#include <devices/timer.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <hardware/cia.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/radeon3d.h>
#include <proto/timer.h>
#include <stdio.h>
#include <string.h>

#include "phase2_regs.h"

struct Library *Radeon9200Base;
struct DosLibrary *DOSBase;

struct Phase2Timer {
    struct MsgPort Port;
    struct timerequest Request;
    ULONG Rate;
};

static BOOL OpenEclock(struct Phase2Timer *timer)
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

static void CloseEclock(struct Phase2Timer *timer)
{
    if (timer->Rate)
        CloseDevice((struct IORequest *)&timer->Request);
    timer->Rate = 0;
}

static ULONG Now(struct Phase2Timer *timer)
{
    struct EClockVal value;
    struct Device *TimerBase = timer->Request.tr_node.io_Device;

    ReadEClock(&value);
    return value.ev_lo;
}

/* Bounded CPU delay modeled on RadeonDelayUs(). */
static void BusyUs(ULONG microseconds)
{
    volatile struct CIA *cia = (volatile struct CIA *)0x00bfe001UL;

    while (microseconds--) {
        ULONG count = 22UL;

        while (count--) {
            volatile UBYTE value = cia->ciapra;

            (void)value;
        }
    }
}

static void PublishBlock(volatile ULONG *block, const ULONG *values)
{
    ULONG index;

    for (index = 0; index < P2_I_COUNT; ++index)
        block[index] = values[index];
    (void)block[P2_I_COUNT - 1UL];
}

static void PublishHostDone(volatile ULONG *block)
{
    block[P2_I_HOST_DONE] = P2_HOST_DONE;
    (void)block[P2_I_HOST_DONE];
}

static ULONG ReadMmio(ULONG bar2, ULONG reg)
{
    volatile ULONG *address = (volatile ULONG *)(bar2 + reg);
    ULONG value = *address;

    __asm__ __volatile__ ("" ::: "memory");
    return __builtin_bswap32(value);
}

int main(void)
{
    struct Phase2Timer timer;
    struct Radeon3DInfo info;
    struct Radeon3DDevice *device = NULL;
    struct Radeon3DSegment control;
    struct Radeon3DLease lease;
    volatile ULONG *block;
    ULONG values[P2_I_COUNT];
    ULONG fence = 0;
    ULONG lastFence;
    ULONG index;
    ULONG now;
    ULONG deadline;
    ULONG scratch68k;
    ULONG start;
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

    Radeon9200Base = OpenLibrary((CONST_STRPTR)"Radeon9200.chip",
                                 RADEON3D_LIBRARY_VERSION);
    if (!Radeon9200Base) {
        printf("P2HOST status=no_chip_library\n");
        return 20;
    }

    info.Size = sizeof(info);
    device = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
    if (!device) {
        printf("P2HOST status=service_open_failed\n");
        goto done;
    }
    if (info.Version < 20UL || !(info.Caps & RADEON3D_CAP_PPC_RING)) {
        printf("P2HOST status=ppc_ring_unavailable caps=%08lx iface=%lu "
               "(needs interface 20 + CAP_PPC_RING; a PPCRING=0 build "
               "never offers it)\n",
               (unsigned long)info.Caps,
               (unsigned long)info.Version);
        goto done;
    }
    printf("P2HOST caps=%08lx iface=%lu\n",
           (unsigned long)info.Caps, (unsigned long)info.Version);

    if (!OpenEclock(&timer)) {
        printf("P2HOST status=no_timer\n");
        goto done;
    }
    timerOpen = TRUE;

    /* CP proof before the grant: the lease blocks service submissions. */
    if (!Radeon3DSubmit(device, noopBatch, 4UL, 1UL, &fence) || !fence) {
        printf("P2HOST status=cp_probe_submit_failed\n");
        goto done;
    }
    if (!Radeon3DWaitFence(device, fence, 2000UL)) {
        printf("P2HOST status=cp_probe_fence_timeout fence=%lu\n",
               (unsigned long)fence);
        goto done;
    }

    control.Size = sizeof(control);
    if (!Radeon3DAllocSegment(device, P0_CONTROL_BYTES, &control) ||
        control.Bytes != P0_CONTROL_BYTES) {
        printf("P2HOST status=control_segment_failed\n");
        goto done;
    }
    controlLeased = TRUE;

    block = (volatile ULONG *)control.CpuAddress;
    for (index = 0; index < P2_I_COUNT; ++index)
        values[index] = 0;
    values[P2_I_MAGIC] = P2_MAGIC;
    values[P2_I_VERSION] = P2_VERSION;
    values[P2_I_CONTROL_BYTES] = control.Bytes;
    PublishBlock(block, values);

    /* Wait for the PPC writer to announce itself (stage 0xffffffff), then
     * grant the lease and publish: no operator gap between grant and
     * release, and the 5 s expiry reclaim cannot fire on a healthy pair. */
    start = Now(&timer);
    deadline = start + timer.Rate * 120UL;
    while (block[P2_I_ERROR_STAGE] != 0xffffffffUL) {
        now = Now(&timer);
        if ((LONG)(now - deadline) >= 0) {
            printf("P2HOST status=ppc_never_arrived stage=%08lx\n",
                   (unsigned long)block[P2_I_ERROR_STAGE]);
            result = 10;
            goto done;
        }
        BusyUs(20000UL);
    }
    (void)block[P2_I_ERROR_STAGE];

    /* From here to release, the PPC is the only ring writer. */
    if (!Radeon3DAcquireLease(device, &lease)) {
        printf("P2HOST status=lease_refused stage=%lu\n",
               (unsigned long)0UL);
        goto done;
    }
    if (lease.Version != RADEON3D_LEASE_VERSION ||
        !lease.RingCpuAddress || !lease.RingDwords ||
        !lease.RingMask || !lease.Bar2Base) {
        printf("P2HOST status=bad_lease\n");
        goto done;
    }
    leased = TRUE;
    printf("P2HOST lease ring_cpu=%08lx ring_gpu=%08lx dwords=%lu "
           "mask=%08lx bar2=%08lx next_fence=%lu\n",
           (unsigned long)lease.RingCpuAddress,
           (unsigned long)lease.RingGpuAddress,
           (unsigned long)lease.RingDwords,
           (unsigned long)lease.RingMask,
           (unsigned long)lease.Bar2Base,
           (unsigned long)lease.NextFence);
    lastFence = lease.NextFence + P2_BATCH_COUNT - 1UL;
    if (!lastFence)
        ++lastFence;

    for (index = 0; index < P2_I_COUNT; ++index)
        values[index] = 0;
    values[P2_I_MAGIC] = P2_MAGIC;
    values[P2_I_VERSION] = P2_VERSION;
    values[P2_I_RING_CPU] = (ULONG)lease.RingCpuAddress;
    values[P2_I_RING_GPU] = lease.RingGpuAddress;
    values[P2_I_RING_DWORDS] = lease.RingDwords;
    values[P2_I_RING_MASK] = lease.RingMask;
    values[P2_I_BAR2] = lease.Bar2Base;
    values[P2_I_NEXT_FENCE] = lease.NextFence;
    values[P2_I_BATCHES] = P2_BATCH_COUNT;
    values[P2_I_CONTROL_BYTES] = control.Bytes;
    PublishBlock(block, values);
    PublishHostDone(block);

    start = Now(&timer);
    deadline = start + timer.Rate * 120UL;
    while (block[P2_I_PPC_ACK] != P2_PPC_ACK) {
        now = Now(&timer);
        if ((LONG)(now - deadline) >= 0) {
            printf("P2HOST status=ppc_timeout stage=%lu ack=%08lx\n",
                   (unsigned long)block[P2_I_ERROR_STAGE],
                   (unsigned long)block[P2_I_PPC_ACK]);
            result = 10;
            goto done;
        }
        BusyUs(20000UL);
    }
    (void)block[P2_I_PPC_ACK];

    printf("P2PPC submits=%lu last_fence=%lu retired=%lu stage=%lu\n",
           (unsigned long)block[P2_I_SUBMITS],
           (unsigned long)block[P2_I_LAST_FENCE],
           (unsigned long)block[P2_I_RETIRED],
           (unsigned long)block[P2_I_ERROR_STAGE]);
    if (block[P2_I_ERROR_STAGE] ||
        block[P2_I_SUBMITS] != P2_BATCH_COUNT ||
        block[P2_I_LAST_FENCE] != lastFence ||
        block[P2_I_RETIRED] != 1UL) {
        result = 10;
        goto done;
    }
    lastFence = block[P2_I_LAST_FENCE];

    /* Pre-release sanity dump: proves the session is still usable and
     * records the generation the lease was granted under. */
    {
        struct Radeon3DInfo preinfo;

        memset(&preinfo, 0, sizeof(preinfo));
        preinfo.Size = sizeof(preinfo);
        if (Radeon3DGetInfo(device, &preinfo) &&
            preinfo.Size >= RADEON3D_INFO_V3_SIZE)
            printf("P2HOST prerelease iface=%lu gen=%lu "
                   "commit_fail_stage=%lu\n",
                   (unsigned long)preinfo.Version,
                   (unsigned long)preinfo.Generation,
                   (unsigned long)preinfo.CommitFailStage);
        else
            printf("P2HOST prerelease=getinfo_failed\n");
    }

    /* Release consumes the PPC's last fence: the service re-arms the ring
     * state and marks the 3D state dirty for the next 2D operation. */
    if (!Radeon3DReleaseLease(device, lastFence)) {
        struct Radeon3DInfo failinfo;

        /* Direct GPU evidence at failure time: scratch sequence and ring
         * pointers through the 68k's own aperture. */
        printf("P2HOST gpu rptr=%08lx wptr=%08lx scratch=%08lx\n",
               (unsigned long)__builtin_bswap32(
                   *(volatile ULONG *)(lease.Bar2Base + P0_CP_RB_RPTR)),
               (unsigned long)__builtin_bswap32(
                   *(volatile ULONG *)(lease.Bar2Base + P0_CP_RB_WPTR)),
               (unsigned long)__builtin_bswap32(
                   *(volatile ULONG *)(lease.Bar2Base + P2_SCRATCH_REG0)));
        memset(&failinfo, 0, sizeof(failinfo));
        failinfo.Size = sizeof(failinfo);
        if (Radeon3DGetInfo(device, &failinfo) &&
            failinfo.Size >= RADEON3D_INFO_V3_SIZE)
            printf("P2HOST status=release_failed gen=%lu "
                   "commit_fail_stage=%lu\n",
                   (unsigned long)failinfo.Generation,
                   (unsigned long)failinfo.CommitFailStage);
        else
            printf("P2HOST status=release_failed "
                   "commit_fail_stage=unreadable\n");
        result = 10;
        goto done;
    }
    leased = FALSE;

    /* Cross-proof: the PPC's fence tails wrote the sequences through
     * SCRATCH_REG0 on the GPU; the 68k must read the last one back through
     * its own aperture. */
    scratch68k = ReadMmio(lease.Bar2Base, P2_SCRATCH_REG0);
    printf("P2HOST release=ok scratch68k=%08lx expected=%08lx "
           "scratch_match=%lu\n",
           (unsigned long)scratch68k, (unsigned long)lastFence,
           (unsigned long)(scratch68k == lastFence));
    if (scratch68k != lastFence)
        result = 10;
    else {
        printf("P2HOST status=ok submits=%lu last_fence=%lu\n",
               (unsigned long)block[P2_I_SUBMITS],
               (unsigned long)lastFence);
        result = 0;
    }

done:
    if (leased && device)
        (void)Radeon3DReleaseLease(device, 0UL);
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
