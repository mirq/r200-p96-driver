/*
 * Phase-0 68k companion (docs/09-ppc-direct-ring-design.md, section 7).
 *
 * Prepares the shared control block that the WarpOS PPC probe
 * (tools/phase0/ppcphase0.c) reads through the proven 1:1 CPU alias:
 *
 *   1. locate the RV280 board through prometheus.library and read the
 *      BAR0/BAR2 addresses exactly the way Prometheus.card does;
 *   2. open Radeon9200.chip by resident name, open an interface-19 session,
 *      require CP_READY and STREAM_SEGMENTS;
 *   3. prove the command processor alive with a fenced PACKET2 batch;
 *   4. lease one control-block segment and one arena segment;
 *   5. measure the 68k baseline: MMIO read, MMIO posted write,
 *      MMIO write+readback, aperture store with a final readback drain;
 *   6. publish the control block (last dword published with a readback),
 *      print the exact command line for the PPC side, then wait for the
 *      PPC acknowledgement and print the joint summary.
 *
 * The tool never touches engine state beyond the harmless scratch register,
 * and it frees both segments (and closes the session) on every exit path.
 */

#include <devices/timer.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <hardware/cia.h>
#include <proto/exec.h>
#include <proto/radeon3d.h>
#include <proto/timer.h>
#include <stdio.h>
#include <string.h>
#include <utility/tagitem.h>

#include <prometheus.h>
#include <proto/prometheus.h>

#include "phase0_regs.h"

#ifndef PCI_VENDOR_ATI
#define PCI_VENDOR_ATI 0x1002
#endif

#define RADEON_BAR_FRAMEBUFFER 0UL
#define RADEON_BAR_MMIO        2UL

#define P0_PACKET2 0x80000000UL

struct Library *Radeon9200Base;
struct Library *PrometheusBase;

struct Phase0Timer {
    struct MsgPort Port;
    struct timerequest Request;
    ULONG Rate;
};

static const ULONG SupportedDevices[] = {
    0x5960UL, 0x5961UL, 0x5964UL
};

static BOOL OpenEclock(struct Phase0Timer *timer)
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

static void CloseEclock(struct Phase0Timer *timer)
{
    if (timer->Rate)
        CloseDevice((struct IORequest *)&timer->Request);
    timer->Rate = 0;
}

/* EClock low word; callers take unsigned deltas. */
static ULONG Now(struct Phase0Timer *timer)
{
    struct EClockVal value;
    struct Device *TimerBase = timer->Request.tr_node.io_Device;

    ReadEClock(&value);
    return value.ev_lo;
}

static ULONG TicksToNanos(struct Phase0Timer *timer, ULONG ticks)
{
    if (!timer->Rate)
        return 0;
    return (ULONG)(((unsigned long long)ticks * 1000000000ULL) /
                   timer->Rate);
}

/* Bounded CPU delay modeled on RadeonDelayUs(): CIA polling keeps the
 * aperture idle between control-block polls. */
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

static ULONG ReadMmio(ULONG bar2, ULONG reg)
{
    volatile ULONG *address = (volatile ULONG *)(bar2 + reg);
    ULONG value = *address;

    __asm__ __volatile__ ("" ::: "memory");
    return __builtin_bswap32(value);
}

static void WriteMmio(ULONG bar2, ULONG reg, ULONG value)
{
    volatile ULONG *address = (volatile ULONG *)(bar2 + reg);

    *address = __builtin_bswap32(value);
    __asm__ __volatile__ ("" ::: "memory");
}

static ULONG ReadWriteMmio(ULONG bar2, ULONG reg, ULONG value)
{
    volatile ULONG *address = (volatile ULONG *)(bar2 + reg);

    *address = __builtin_bswap32(value);
    __asm__ __volatile__ ("" ::: "memory");
    return __builtin_bswap32(*address);
}

static void FillControlBlock(volatile ULONG *block, const ULONG *values)
{
    ULONG index;

    for (index = 0; index < P0_I_COUNT; ++index)
        block[index] = values[index];
    (void)block[P0_I_COUNT - 1UL];
}

static void PublishHostDone(volatile ULONG *block)
{
    block[P0_I_HOST_DONE] = P0_HOST_DONE;
    (void)block[P0_I_HOST_DONE];
}

int main(void)
{
    struct Phase0Timer timer;
    struct Radeon3DInfo info;
    struct Radeon3DDevice *device = NULL;
    struct Radeon3DSegment control;
    struct Radeon3DSegment arena;
    PCIBoard *board = NULL;
    volatile ULONG *block;
    volatile ULONG *aperture;
    ULONG bar0 = 0;
    ULONG bar0Bytes = 0;
    ULONG bar2 = 0;
    ULONG bar2Bytes = 0;
    ULONG values[P0_I_COUNT];
    ULONG mmioReadTicks;
    ULONG mmioWriteTicks;
    ULONG mmioCommitTicks;
    ULONG apertureTicks;
    ULONG fence = 0;
    ULONG index;
    ULONG pass;
    ULONG start;
    BOOL timerOpen = FALSE;
    BOOL controlLeased = FALSE;
    BOOL arenaLeased = FALSE;
    int result = 20;

    static const ULONG noopBatch[4] = {
        P0_PACKET2, P0_PACKET2, P0_PACKET2, P0_PACKET2
    };

    memset(&timer, 0, sizeof(timer));
    memset(&control, 0, sizeof(control));
    memset(&arena, 0, sizeof(arena));

    PrometheusBase = OpenLibrary((CONST_STRPTR)"prometheus.library", 2);
    if (!PrometheusBase) {
        printf("P0HOST status=no_prometheus_library\n");
        return 20;
    }

    while ((board = Prm_FindBoardTags(board,
                                      PRM_Vendor, PCI_VENDOR_ATI,
                                      TAG_END)) != NULL) {
        ULONG boardDevice = 0;
        ULONG boardFrame = 0;
        ULONG boardFrameSize = 0;
        ULONG boardMmio = 0;
        ULONG boardMmioSize = 0;

        Prm_GetBoardAttrsTags(board,
            PRM_Device, (ULONG)&boardDevice,
            PRM_MemoryAddr0 + RADEON_BAR_FRAMEBUFFER,
                (ULONG)&boardFrame,
            PRM_MemorySize0 + RADEON_BAR_FRAMEBUFFER,
                (ULONG)&boardFrameSize,
            PRM_MemoryAddr0 + RADEON_BAR_MMIO, (ULONG)&boardMmio,
            PRM_MemorySize0 + RADEON_BAR_MMIO, (ULONG)&boardMmioSize,
            TAG_END);
        for (index = 0; index < sizeof(SupportedDevices) /
                             sizeof(SupportedDevices[0]); ++index) {
            if (boardDevice == SupportedDevices[index] && boardFrame &&
                boardMmio && boardFrameSize >= 0x00400000UL &&
                boardMmioSize >= 0x00010000UL) {
                bar0 = boardFrame;
                bar0Bytes = boardFrameSize;
                bar2 = boardMmio;
                bar2Bytes = boardMmioSize;
                break;
            }
        }
        if (bar0)
            break;
    }
    if (!bar0) {
        printf("P0HOST status=no_radeon_board\n");
        goto done;
    }
    printf("P0HOST bar0=%08lx bar0_bytes=%lu bar2=%08lx bar2_bytes=%lu\n",
           (unsigned long)bar0, (unsigned long)bar0Bytes,
           (unsigned long)bar2, (unsigned long)bar2Bytes);

    if (!OpenEclock(&timer)) {
        printf("P0HOST status=no_timer\n");
        goto done;
    }
    timerOpen = TRUE;

    Radeon9200Base = OpenLibrary((CONST_STRPTR)"Radeon9200.chip",
                                 RADEON3D_LIBRARY_VERSION);
    if (!Radeon9200Base) {
        printf("P0HOST status=no_chip_library\n");
        goto done;
    }

    memset(&info, 0, sizeof(info));
    info.Size = sizeof(info);
    device = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
    if (!device) {
        printf("P0HOST status=service_open_failed\n");
        goto done;
    }
    if (!(info.Caps & (1UL << 0)) || !(info.Caps & (1UL << 21))) {
        printf("P0HOST status=missing_caps caps=%08lx "
               "(needs CP_READY + STREAM_SEGMENTS)\n",
               (unsigned long)info.Caps);
        goto done;
    }

    /* Prove the command processor alive before any MMIO measurement or PPC
     * access: one PACKET2 batch with a fence, retired before publishing. */
    if (!Radeon3DSubmit(device, noopBatch, 4UL, 1UL, &fence) || !fence) {
        printf("P0HOST status=cp_probe_submit_failed\n");
        goto done;
    }
    if (!Radeon3DWaitFence(device, fence, 2000UL)) {
        printf("P0HOST status=cp_probe_fence_timeout fence=%lu\n",
               (unsigned long)fence);
        goto done;
    }

    control.Size = sizeof(control);
    if (!Radeon3DAllocSegment(device, P0_CONTROL_BYTES, &control) ||
        control.Bytes != P0_CONTROL_BYTES) {
        printf("P0HOST status=control_segment_failed\n");
        goto done;
    }
    controlLeased = TRUE;
    arena.Size = sizeof(arena);
    if (!Radeon3DAllocSegment(device, P0_ARENA_BYTES, &arena) ||
        arena.Bytes != P0_ARENA_BYTES) {
        printf("P0HOST status=arena_segment_failed\n");
        goto done;
    }
    arenaLeased = TRUE;

    if ((ULONG)arena.CpuAddress < bar0 ||
        (ULONG)arena.CpuAddress + arena.Bytes > bar0 + bar0Bytes) {
        printf("P0HOST status=arena_outside_bar0\n");
        goto done;
    }

    /* 68k baseline. Each loop keeps the shape the driver uses, so the
     * numbers compare against the PPC probe on the same boot. */
    start = Now(&timer);
    {
        ULONG value = 0;

        for (index = 0; index < P0_MMIO_ITERATIONS; ++index)
            value ^= ReadMmio(bar2, P0_RBBM_STATUS);
        mmioReadTicks = Now(&timer) - start;
        (void)value;
    }
    start = Now(&timer);
    for (index = 0; index < P0_MMIO_ITERATIONS; ++index)
        WriteMmio(bar2, P0_SCRATCH_REG1, 0UL);
    mmioWriteTicks = Now(&timer) - start;
    {
        ULONG readback = 0;

        start = Now(&timer);
        for (index = 0; index < P0_MMIO_COMMIT_ITERS; ++index)
            readback ^= ReadWriteMmio(bar2, P0_SCRATCH_REG1,
                                      (ULONG)index);
        mmioCommitTicks = Now(&timer) - start;
        (void)readback;
    }
    /* Aperture store with the driver's posting discipline: sequential
     * stores, one final readback of the last dword per pass. */
    aperture = (volatile ULONG *)arena.CpuAddress;
    {
        ULONG dwords = P0_APER_BURST_BYTES / sizeof(ULONG);

        for (index = 0; index < dwords; ++index)
            aperture[index] = P0_APER_PATTERN ^ index;
        start = Now(&timer);
        for (pass = 0; pass < 32UL; ++pass) {
            for (index = 0; index < dwords; ++index)
                aperture[index] = P0_APER_PATTERN ^ pass;
            (void)aperture[dwords - 1UL];
        }
        apertureTicks = Now(&timer) - start;
    }

    printf("P0HOST status=ok eclock=%lu "
           "mmio_read_ns=%lu mmio_write_ns=%lu mmio_commit_ns=%lu "
           "aperture_dword_ns=%lu\n",
           (unsigned long)timer.Rate,
           (unsigned long)(TicksToNanos(&timer, mmioReadTicks) /
                           P0_MMIO_ITERATIONS),
           (unsigned long)(TicksToNanos(&timer, mmioWriteTicks) /
                           P0_MMIO_ITERATIONS),
           (unsigned long)(TicksToNanos(&timer, mmioCommitTicks) /
                           P0_MMIO_COMMIT_ITERS),
           (unsigned long)(TicksToNanos(&timer, apertureTicks) /
                           (32UL * (P0_APER_BURST_BYTES /
                                    sizeof(ULONG)))));

    for (index = 0; index < P0_I_COUNT; ++index)
        values[index] = 0;
    values[P0_I_MAGIC] = P0_MAGIC;
    values[P0_I_VERSION] = P0_VERSION;
    values[P0_I_BAR0_CPU] = bar0;
    values[P0_I_BAR0_BYTES] = bar0Bytes;
    values[P0_I_BAR2_CPU] = bar2;
    values[P0_I_BAR2_BYTES] = bar2Bytes;
    values[P0_I_ARENA_CPU] = (ULONG)arena.CpuAddress;
    values[P0_I_ARENA_BYTES] = arena.Bytes;
    values[P0_I_CONTROL_BYTES] = P0_CONTROL_BYTES;
    values[P0_I_HOST_MMIO_READ_NS] =
        TicksToNanos(&timer, mmioReadTicks) / P0_MMIO_ITERATIONS;
    values[P0_I_HOST_MMIO_WRITE_NS] =
        TicksToNanos(&timer, mmioWriteTicks) / P0_MMIO_ITERATIONS;
    values[P0_I_HOST_MMIO_COMMIT_NS] =
        TicksToNanos(&timer, mmioCommitTicks) / P0_MMIO_COMMIT_ITERS;
    values[P0_I_HOST_APER_NS_DWORD] =
        TicksToNanos(&timer, apertureTicks) /
        (32UL * (P0_APER_BURST_BYTES / sizeof(ULONG)));
    values[P0_I_HOST_ECLK_HZ] = timer.Rate;
    values[P0_I_FLAGS] = P0_FLAG_MMIO_WRITE_OK;
    values[P0_I_HOST_SEG_ID] = control.Id;
    values[P0_I_ARENA_SEG_ID] = arena.Id;
    values[P0_I_PPC_SELFREAD] = P0_APER_PATTERN;

    block = (volatile ULONG *)control.CpuAddress;
    FillControlBlock(block, values);
    PublishHostDone(block);

    printf("P0HOST waiting for ppcphase0. Run:\n");
    printf("    Work:ppcphase0 %08lx\n", (unsigned long)control.CpuAddress);
    fflush(stdout);

    {
        ULONG deadline = Now(&timer) + timer.Rate * 120UL;

        while (block[P0_I_PPC_ACK] != P0_PPC_ACK) {
            if ((LONG)(Now(&timer) - deadline) >= 0) {
                printf("P0HOST status=ppc_timeout ack=%08lx stage=%lu\n",
                       (unsigned long)block[P0_I_PPC_ACK],
                       (unsigned long)block[P0_I_PPC_STAGE]);
                result = 10;
                goto done;
            }
            BusyUs(20000UL);
        }
        (void)block[P0_I_PPC_ACK];
    }

    printf("P0PPC status=%s stage=%lu "
           "mmio_read_ns=%lu mmio_write_ns=%lu mmio_commit_ns=%lu "
           "aper_native_kbps=%lu aper_br_kbps=%lu aper_read_ns=%lu "
           "selfread=%08lx stwbrx_ok=%lu scratch=%08lx\n",
           block[P0_I_PPC_STATUS] ? "fail" : "ok",
           (unsigned long)block[P0_I_PPC_STAGE],
           (unsigned long)block[P0_I_PPC_MMIO_READ_NS],
           (unsigned long)block[P0_I_PPC_MMIO_WRITE_NS],
           (unsigned long)block[P0_I_PPC_MMIO_COMMIT_NS],
           (unsigned long)block[P0_I_PPC_APER_NATIVE_KBPS],
           (unsigned long)block[P0_I_PPC_APER_BR_KBPS],
           (unsigned long)block[P0_I_PPC_APER_READ_NS],
           (unsigned long)block[P0_I_PPC_SELFREAD],
           (unsigned long)block[P0_I_PPC_STWBRX_OK],
           (unsigned long)block[P0_I_PPC_MMIO_SCRATCH]);
    /* Cross-CPU MMIO write proof: the host reads SCRATCH_REG1 through its
     * own aperture; equal published values prove the PPC byte-reversed
     * store reached the register in the shape both sides expect. */
    {
        ULONG hostScratch = ReadMmio(bar2, P0_SCRATCH_REG1);

        printf("P0PPC scratch68k=%08lx scratch_match=%lu\n",
               (unsigned long)hostScratch,
               (unsigned long)(hostScratch ==
                               block[P0_I_PPC_MMIO_SCRATCH]));
        if (block[P0_I_PPC_MMIO_SCRATCH] &&
            hostScratch != block[P0_I_PPC_MMIO_SCRATCH] && !result)
            result = 10;
    }
    /* Arena visibility proof: the PPC's timed store loops ended with one
     * byte-reversed pass writing P0_APER_PATTERN^P0_APER_PASSES to every
     * dword (the byte-order test restores dword 0 afterwards). The host
     * reads VRAM through its big-endian view, so the raw dword must be
     * byte-swapped before comparison. Stale memory here means the measured
     * bandwidth absorbed stores in the PPC cache. */
    if (!block[P0_I_PPC_STATUS]) {
        volatile ULONG *aperture = (volatile ULONG *)arena.CpuAddress;
        ULONG dwords = P0_APER_BURST_BYTES / sizeof(ULONG);
        ULONG expected = P0_APER_PATTERN ^ P0_APER_PASSES;
        ULONG first = __builtin_bswap32(aperture[0]);
        ULONG middle = __builtin_bswap32(aperture[dwords / 2UL]);
        ULONG last = __builtin_bswap32(aperture[dwords - 1UL]);
        ULONG match = first == expected && middle == expected &&
                      last == expected;

        printf("P0PPC arena68k=%08lx,%08lx,%08lx arena_expected=%08lx "
               "arena_match=%lu\n",
               (unsigned long)first, (unsigned long)middle,
               (unsigned long)last, (unsigned long)expected,
               (unsigned long)match);
        if (!match && !result)
            result = 10;
    }
    result = block[P0_I_PPC_STATUS] ? 10 : result;

done:
    if (controlLeased && device)
        (void)Radeon3DFreeSegment(device, control.Id);
    if (arenaLeased && device)
        (void)Radeon3DFreeSegment(device, arena.Id);
    if (device)
        Radeon3DClose(device);
    if (Radeon9200Base) {
        CloseLibrary(Radeon9200Base);
        Radeon9200Base = NULL;
    }
    if (PrometheusBase) {
        CloseLibrary(PrometheusBase);
        PrometheusBase = NULL;
    }
    if (timerOpen)
        CloseEclock(&timer);
    return result;
}
