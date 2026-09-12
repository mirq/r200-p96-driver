#include "radeon_debug.h"

#ifdef DEBUG

#include <devices/timer.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include "radeon9200.h"
#include "radeon_regs.h"

/*
 * Enough iterations that a single EClock tick (about 1.4 microseconds) is
 * noise, but short enough not to stall the boot noticeably even if a PCI
 * access turns out to cost tens of microseconds.
 */
#define MMIO_SAMPLE_COUNT 2000UL
#define VRAM_SMALL_BYTES  (8UL * 1024UL)
#define VRAM_BURST_BYTES  (64UL * 1024UL)
#define CP_PROBE_DWORDS    4096UL
#define FALLBACK_PROBE_CALLS 8192UL

ULONG RadeonDebugReads;
ULONG RadeonDebugWrites;
ULONG RadeonMonoProbeResult;
ULONG RadeonMonoProbeSample;
ULONG RadeonMonoProbeSampleAlt;

struct RadeonDebugNode {
    struct MsgPort Port;
    struct RadeonDebugStats Stats;
};

static const char DebugPortName[] = RADEON_DEBUG_PORT;

static struct RadeonDebugNode *DebugNode;
struct Device *TimerBase; /* proto/timer.h declares this global by name */
static struct timerequest TimerRequest;
static struct MsgPort TimerPort;

static void InitList(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

/*
 * PA_IGNORE with no signal bit and no task: the port is a passive data anchor,
 * never messaged, so it stays valid no matter which task called InitCard.
 */
static void InitPassivePort(struct MsgPort *port, const char *name)
{
    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Node.ln_Pri = 0;
    port->mp_Node.ln_Name = (char *)name;
    port->mp_Flags = PA_IGNORE;
    port->mp_SigBit = 0;
    port->mp_SigTask = NULL;
    InitList(&port->mp_MsgList);
}

static BOOL OpenTimer(struct ExecBase *SysBase)
{
    if (TimerBase)
        return TRUE;

    InitPassivePort(&TimerPort, NULL);
    TimerRequest.tr_node.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    TimerRequest.tr_node.io_Message.mn_ReplyPort = &TimerPort;
    TimerRequest.tr_node.io_Message.mn_Length = sizeof(TimerRequest);

    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_ECLOCK,
                   (struct IORequest *)&TimerRequest, 0))
        return FALSE;
    TimerBase = TimerRequest.tr_node.io_Device;
    return TRUE;
}

static void CloseTimer(struct ExecBase *SysBase)
{
    if (!TimerBase)
        return;
    CloseDevice((struct IORequest *)&TimerRequest);
    TimerBase = NULL;
}

static ULONG Clock(void)
{
    struct EClockVal value;

    if (!TimerBase)
        return 0;
    ReadEClock(&value);
    return value.ev_lo;
}

/*
 * Separates posted-write cost from non-posted read cost. Reads use
 * RBBM_STATUS because that is what the hot poll loops read. Writes use
 * SCRATCH_REG1, which is a general-purpose register outside the GUI FIFO, so
 * hammering it cannot overflow the 2D command FIFO or disturb the CP fence in
 * SCRATCH_REG0.
 */
static void MeasureMmio(struct BoardInfo *bi,
                         struct RadeonDebugStats *stats)
{
    ULONG index;
    ULONG start;

    if (!TimerBase)
        return;

    start = Clock();
    for (index = 0; index < MMIO_SAMPLE_COUNT; ++index)
        (void)RadeonRead32(bi, RADEON_RBBM_STATUS);
    stats->MmioReadTicks = Clock() - start;

    start = Clock();
    for (index = 0; index < MMIO_SAMPLE_COUNT; ++index)
        (void)RadeonWrite32(bi, RADEON_SCRATCH_REG1, 0);
    stats->MmioWriteTicks = Clock() - start;

    start = Clock();
    for (index = 0; index < MMIO_SAMPLE_COUNT; ++index)
        (void)Clock();
    stats->ClockTicks = Clock() - start;

    /* Reserve the measured range so no later private allocation can alias it. */
    if (bi->MemoryBase) {
        APTR scratch = RadeonAllocatePrivateVram(bi, VRAM_BURST_BYTES);
        volatile ULONG *target = (volatile ULONG *)scratch;
        ULONG words;
        ULONG drain = 0;

        if (target) {
            start = Clock();
            for (index = 0; index < MMIO_SAMPLE_COUNT; ++index)
                target[index & 2047UL] = 0;
            drain = target[2047UL];
            stats->VramWriteTicks = Clock() - start;

            words = VRAM_SMALL_BYTES / sizeof(ULONG);
            start = Clock();
            for (index = 0; index < words; ++index)
                target[index] = index;
            drain = target[words - 1UL];
            stats->VramSmallTicks = Clock() - start;
            stats->VramSmallBytes = VRAM_SMALL_BYTES;

            words = VRAM_BURST_BYTES / sizeof(ULONG);
            start = Clock();
            for (index = 0; index < words; ++index)
                target[index] = index;
            drain = target[words - 1UL];
            stats->VramBurstTicks = Clock() - start;
            stats->VramBurstBytes = VRAM_BURST_BYTES;
            stats->VramDrainValue = drain;

            if (!RadeonFreePrivateVram(bi, scratch, VRAM_BURST_BYTES))
                RLOG("Radeon9200: debug VRAM probe reservation leaked\n");
        }
    }

    stats->MmioSamples = MMIO_SAMPLE_COUNT;
}

static void MeasureCpBatch(struct BoardInfo *bi,
                            struct RadeonDebugStats *stats)
{
    ULONG start;

    if (!TimerBase || !RadeonCpIsReady(bi))
        return;

    stats->CpProbeDwords = CP_PROBE_DWORDS;
    /* Warm both complete paths before recording either result. */
    if (!RadeonCpDebugSubmitNoops(bi, CP_PROBE_DWORDS, FALSE) ||
        !RadeonCpDebugSubmitNoops(bi, CP_PROBE_DWORDS, TRUE))
        return;

    start = Clock();
    stats->CpBufferedSuccess =
        RadeonCpDebugSubmitNoops(bi, CP_PROBE_DWORDS, FALSE);
    stats->CpBufferedTicks = Clock() - start;
    if (!stats->CpBufferedSuccess)
        return;

    start = Clock();
    stats->CpDirectSuccess =
        RadeonCpDebugSubmitNoops(bi, CP_PROBE_DWORDS, TRUE);
    stats->CpDirectTicks = Clock() - start;
}

static ULONG ByteReverse(ULONG value)
{
    return ((value & 0x000000ffUL) << 24) |
           ((value & 0x0000ff00UL) << 8) |
           ((value & 0x00ff0000UL) >> 8) |
           ((value & 0xff000000UL) >> 24);
}

struct IbCase {
    ULONG Gpu;
    ULONG Dwords;
    ULONG Accepted;
    ULONG CsqSubmit;
    ULONG Fence;
    ULONG CsqAfter;
    ULONG Rbbm;
    ULONG Scratch;
    ULONG FenceTicks;
    ULONG NextReady;
    ULONG CsqMode;
};

/*
 * One indirect-buffer case, kernel r100_ib_test shape: dwords of a
 * scratch-write packet followed by PACKET2, stored CP-native
 * (byte-reversed, matching the software-pre-swapped ring), dispatched
 * with PACKET0(CP_IB_BASE,1) and retired by the ring fence. On a fence
 * timeout the engine is recovered so the next case starts clean; NextReady
 * records whether that recovery left the CP usable.
 */
static void RunIbCase(struct BoardInfo *bi, APTR cpu, ULONG gpu,
                      ULONG dwords, struct IbCase *out)
{
    volatile ULONG *ib = (volatile ULONG *)cpu;
    ULONG commands[3];
    ULONG fence = 0;
    ULONG start;
    ULONG index;
    BOOL submitted;

    out->Gpu = gpu;
    out->Dwords = dwords;
    ib[0] = ByteReverse(RADEON_CP_PACKET0(RADEON_SCRATCH_REG1, 0));
    ib[1] = ByteReverse(0xdeadbeefUL);
    for (index = 2; index < dwords; ++index)
        ib[index] = ByteReverse(RADEON_CP_PACKET2);
    (void)ib[dwords - 1UL];
    (void)RadeonWrite32(bi, RADEON_SCRATCH_REG1, 0);
    out->CsqMode = RadeonRead32(bi, RADEON_CP_CSQ_MODE);
    commands[0] = RADEON_CP_PACKET0(RADEON_CP_IB_BASE, 1);
    commands[1] = gpu;
    commands[2] = dwords;
    start = Clock();
    submitted = RadeonCpSubmitStream(bi, commands, 3UL, TRUE, &fence);
    out->CsqSubmit = RadeonRead32(bi, RADEON_CP_CSQ_STAT);
    out->Accepted = submitted ? 1UL : 0UL;
    if (submitted && fence) {
        out->Fence = RadeonCpWaitFence(bi, fence, 3000UL) ? 1UL : 0UL;
        if (!out->Fence)
            (void)RadeonRecoverAcceleration(bi);
    }
    out->FenceTicks = Clock() - start;
    out->CsqAfter = RadeonRead32(bi, RADEON_CP_CSQ_STAT);
    out->Rbbm = RadeonRead32(bi, RADEON_RBBM_STATUS);
    if (out->Fence) {
        for (index = 0; index < 50000UL; ++index) {
            if (RadeonRead32(bi, RADEON_SCRATCH_REG1) == 0xdeadbeefUL)
                break;
        }
        out->Scratch = RadeonRead32(bi, RADEON_SCRATCH_REG1);
    }
    out->NextReady = RadeonCpIsReady(bi) ? 1UL : 0UL;
}

static void FillIb2(struct RadeonDebugStats *stats,
                    const struct IbCase *out)
{
    stats->Ib2Gpu = out->Gpu;
    stats->Ib2Dwords = out->Dwords;
    stats->Ib2Accepted = out->Accepted;
    stats->Ib2CsqSubmit = out->CsqSubmit;
    stats->Ib2Fence = out->Fence;
    stats->Ib2CsqAfter = out->CsqAfter;
    stats->Ib2Rbbm = out->Rbbm;
    stats->Ib2Scratch = out->Scratch;
    stats->Ib2FenceTicks = out->FenceTicks;
    stats->Ib2NextReady = out->NextReady;
    stats->Ib2CsqMode = out->CsqMode;
}

static void FillIb3(struct RadeonDebugStats *stats,
                    const struct IbCase *out)
{
    stats->Ib3Gpu = out->Gpu;
    stats->Ib3Dwords = out->Dwords;
    stats->Ib3Accepted = out->Accepted;
    stats->Ib3CsqSubmit = out->CsqSubmit;
    stats->Ib3Fence = out->Fence;
    stats->Ib3CsqAfter = out->CsqAfter;
    stats->Ib3Rbbm = out->Rbbm;
    stats->Ib3Scratch = out->Scratch;
    stats->Ib3FenceTicks = out->FenceTicks;
    stats->Ib3NextReady = out->NextReady;
    stats->Ib3CsqMode = out->CsqMode;
}

/*
 * Indirect-buffer bring-up probe. Case 1 uses a private-VRAM block from
 * the low bump area with 8 dwords (the kernel-proven shape). Case 2
 * repeats it from the streaming-segment pool, isolating the address
 * region. Case 3 then varies the size: 64 dwords at the pool when case 2
 * passed, otherwise 64 dwords at the low block, isolating the size at
 * whichever address region works.
 */
static void TestIndirectBuffer(struct BoardInfo *bi,
                               struct RadeonDebugStats *stats,
                               APTR segmentPool)
{
    struct RadeonBoardData *data;
    struct IbCase caseTwo;
    APTR scratch;
    volatile ULONG *ib;
    ULONG memoryBase;
    ULONG gpuAddress;
    ULONG index;

    stats->IbProbeRun = 1UL;
    if (!RadeonCpIsReady(bi))
        return;
    data = RadeonGetBoardData(bi);
    if (!data || !bi->MemoryBase)
        return;
    scratch = RadeonAllocatePrivateVram(bi, 4096UL);
    if (!scratch)
        return;
    stats->IbAllocSuccess = 1UL;
    memoryBase = (ULONG)bi->MemoryBase;
    if ((ULONG)scratch < memoryBase ||
        data->FramebufferGpuBase >
            ~0UL - ((ULONG)scratch - memoryBase)) {
        (void)RadeonFreePrivateVram(bi, scratch, 4096UL);
        return;
    }
    gpuAddress = data->FramebufferGpuBase +
                 ((ULONG)scratch - memoryBase);
    stats->IbGpuAddress = gpuAddress;
    ib = (volatile ULONG *)scratch;
    ib[0] = ByteReverse(RADEON_CP_PACKET0(RADEON_SCRATCH_REG1, 0));
    ib[1] = ByteReverse(0xdeadbeefUL);
    for (index = 2; index < 8UL; ++index)
        ib[index] = ByteReverse(RADEON_CP_PACKET2);
    (void)ib[7UL];
    (void)RadeonWrite32(bi, RADEON_SCRATCH_REG1, 0);
    RunIbCase(bi, scratch, gpuAddress, 8UL, &caseTwo);
    stats->IbCsqStatSubmit = caseTwo.CsqSubmit;
    stats->IbDispatchAccepted = caseTwo.Accepted;
    stats->IbFenceRetired = caseTwo.Fence;
    stats->IbCsqStatAfter = caseTwo.CsqAfter;
    stats->IbRbbmStatusAfter = caseTwo.Rbbm;
    stats->IbScratchValue = caseTwo.Scratch;
    stats->IbFenceTicks = caseTwo.FenceTicks;
    RLOG("Radeon9200: IB case1 gpu=%08lx dwords=8 accepted=%lu "
         "csq_submit=%08lx fence=%lu csq_after=%08lx rbbm=%08lx "
         "scratch=%08lx ticks=%lu ready=%lu\n",
         (unsigned long)gpuAddress,
         (unsigned long)caseTwo.Accepted,
         (unsigned long)caseTwo.CsqSubmit,
         (unsigned long)caseTwo.Fence,
         (unsigned long)caseTwo.CsqAfter,
         (unsigned long)caseTwo.Rbbm,
         (unsigned long)caseTwo.Scratch,
         (unsigned long)caseTwo.FenceTicks,
         (unsigned long)caseTwo.NextReady);

    if (!caseTwo.NextReady)
        goto done;
    if (segmentPool) {
        struct IbCase poolCase;
        struct IbCase bigCase;
        struct IbCase bisectCase;
        ULONG poolGpu = data->FramebufferGpuBase +
                        ((ULONG)segmentPool - memoryBase);

        RunIbCase(bi, segmentPool, poolGpu, 8UL, &poolCase);
        FillIb2(stats, &poolCase);
        RLOG("Radeon9200: IB case2 pool gpu=%08lx dwords=8 accepted=%lu "
             "csq_submit=%08lx fence=%lu csq_after=%08lx rbbm=%08lx "
             "scratch=%08lx ticks=%lu ready=%lu\n",
             (unsigned long)poolCase.Gpu,
             (unsigned long)poolCase.Accepted,
             (unsigned long)poolCase.CsqSubmit,
             (unsigned long)poolCase.Fence,
             (unsigned long)poolCase.CsqAfter,
             (unsigned long)poolCase.Rbbm,
             (unsigned long)poolCase.Scratch,
             (unsigned long)poolCase.FenceTicks,
             (unsigned long)poolCase.NextReady);
        if (!poolCase.NextReady)
            goto done;
        if (poolCase.Fence) {
            /* Case 3: pool, 64 dwords, default partition (known to fail
             * on the first matrix boot). */
            RunIbCase(bi, segmentPool, poolGpu, 64UL, &bigCase);
            FillIb3(stats, &bigCase);
            RLOG("Radeon9200: IB case3 gpu=%08lx dwords=64 accepted=%lu "
                 "csq_submit=%08lx fence=%lu csq_after=%08lx rbbm=%08lx "
                 "scratch=%08lx ticks=%lu ready=%lu\n",
                 (unsigned long)bigCase.Gpu,
                 (unsigned long)bigCase.Accepted,
                 (unsigned long)bigCase.CsqSubmit,
                 (unsigned long)bigCase.Fence,
                 (unsigned long)bigCase.CsqAfter,
                 (unsigned long)bigCase.Rbbm,
                 (unsigned long)bigCase.Scratch,
                 (unsigned long)bigCase.FenceTicks,
                 (unsigned long)bigCase.NextReady);
        } else {
            /* Case 3 fallback: pool failed at 8 dwords; test the size at
             * the known-good low address instead. */
            RunIbCase(bi, scratch, gpuAddress, 64UL, &bigCase);
            FillIb3(stats, &bigCase);
            RLOG("Radeon9200: IB case3 bump gpu=%08lx dwords=64 "
                 "accepted=%lu csq_submit=%08lx fence=%lu "
                 "csq_after=%08lx rbbm=%08lx scratch=%08lx ticks=%lu "
                 "ready=%lu\n",
                 (unsigned long)bigCase.Gpu,
                 (unsigned long)bigCase.Accepted,
                 (unsigned long)bigCase.CsqSubmit,
                 (unsigned long)bigCase.Fence,
                 (unsigned long)bigCase.CsqAfter,
                 (unsigned long)bigCase.Rbbm,
                 (unsigned long)bigCase.Scratch,
                 (unsigned long)bigCase.FenceTicks,
                 (unsigned long)bigCase.NextReady);
        }
        if (!bigCase.NextReady)
            goto done;
        /* Case 4 (overwrites the Ib2 slots): pool, 64 dwords, with the
         * CSQ cache partition the kernel intended before the 0x4d4d
         * magic: INDIRECT1_START=16, INDIRECT2_START=80. Restored after. */
        if (1) {
            /* Case 4: pool, 64 dwords, with the CSQ cache partition the
             * kernel intended before the 0x4d4d magic: INDIRECT1_START=16
             * (bits 0-7), INDIRECT2_START=80 (bits 8-15). Restored after. */
            (void)RadeonWrite32(bi, RADEON_CP_CSQ_MODE, 0x00005010UL);
            RunIbCase(bi, segmentPool, poolGpu, 64UL, &bigCase);
            FillIb2(stats, &bigCase);
            RLOG("Radeon9200: IB case4 partition=5010 gpu=%08lx "
                 "dwords=64 accepted=%lu csq_submit=%08lx fence=%lu "
                 "csq_after=%08lx rbbm=%08lx scratch=%08lx ticks=%lu "
                 "ready=%lu\n",
                 (unsigned long)bigCase.Gpu,
                 (unsigned long)bigCase.Accepted,
                 (unsigned long)bigCase.CsqSubmit,
                 (unsigned long)bigCase.Fence,
                 (unsigned long)bigCase.CsqAfter,
                 (unsigned long)bigCase.Rbbm,
                 (unsigned long)bigCase.Scratch,
                 (unsigned long)bigCase.FenceTicks,
                 (unsigned long)bigCase.NextReady);
            (void)RadeonWrite32(bi, RADEON_CP_CSQ_MODE, 0x00004d4dUL);
        }
        /* Case 5 (overwrites the Ib3 slots): pool, 48 dwords, default
         * partition. Bisects the size threshold if case 4 still fails. */
        if (bigCase.NextReady) {
            RunIbCase(bi, segmentPool, poolGpu, 48UL, &bisectCase);
            FillIb3(stats, &bisectCase);
            RLOG("Radeon9200: IB case5 gpu=%08lx dwords=48 accepted=%lu "
                 "csq_submit=%08lx fence=%lu csq_after=%08lx rbbm=%08lx "
                 "scratch=%08lx ticks=%lu ready=%lu\n",
                 (unsigned long)bisectCase.Gpu,
                 (unsigned long)bisectCase.Accepted,
                 (unsigned long)bisectCase.CsqSubmit,
                 (unsigned long)bisectCase.Fence,
                 (unsigned long)bisectCase.CsqAfter,
                 (unsigned long)bisectCase.Rbbm,
                 (unsigned long)bisectCase.Scratch,
                 (unsigned long)bisectCase.FenceTicks,
                 (unsigned long)bisectCase.NextReady);
        }
    }
done:
    (void)RadeonFreePrivateVram(bi, scratch, 4096UL);
}

static void TestCpFunction(struct BoardInfo *bi,
                           struct RadeonDebugStats *stats)
{
    struct RadeonCpDebugResult result;
    UBYTE *byte = (UBYTE *)&result;
    ULONG count = sizeof(result);

    while (count--)
        *byte++ = 0;
    (void)RadeonCpDebugRunTests(bi, &result);
    stats->CpWrapBefore = result.WrapBefore;
    stats->CpWrapAfter = result.WrapAfter;
    stats->CpWrapSuccess = result.WrapSuccess;
    stats->CpNearFullSuccess = result.NearFullSuccess;
    stats->CpReserveTimeoutSuccess = result.ReserveTimeoutSuccess;
    stats->CpFirstFence = result.FirstFence;
    stats->CpSecondFence = result.SecondFence;
    stats->CpFenceOrderSuccess = result.FenceOrderSuccess;
    stats->CpFenceZeroPollSuccess = result.FenceZeroPollSuccess;
    stats->CpFenceZeroPollTicks = result.FenceZeroPollTicks;
    stats->CpFenceTimeoutSuccess = result.FenceTimeoutSuccess;
    stats->CpFenceTimeoutTicks = result.FenceTimeoutTicks;
}

void RadeonDebugOpen(struct BoardInfo *bi, ULONG cpRequested,
                     ULONG dmaRequested, ULONG spriteExperiment,
                     APTR segmentPool)
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct RadeonDebugStats *stats;
    struct EClockVal value;

    if (!SysBase || !data || DebugNode)
        return;

    DebugNode = AllocMem(sizeof(*DebugNode), MEMF_PUBLIC | MEMF_CLEAR);
    if (!DebugNode)
        return;
    InitPassivePort(&DebugNode->Port, DebugPortName);

    stats = &DebugNode->Stats;
    stats->Magic = RADEON_DEBUG_MAGIC;
    stats->Version = RADEON_DEBUG_VERSION;
    stats->CpRequested = cpRequested;
    stats->CpActive = RadeonCpIsReady(bi);
    stats->DmaRequested = dmaRequested;
    stats->DmaReserved = bi->MemorySize < bi->MemorySpaceSize;
    stats->BoardMemorySize = bi->MemorySize;
    stats->SpriteExperiment = spriteExperiment;
    stats->MonoFromMemory = RadeonMonoProbeResult;
    stats->MonoProbeSample = RadeonMonoProbeSample;
    stats->MonoProbeSampleAlt = RadeonMonoProbeSampleAlt;
    stats->EClockRate = OpenTimer(SysBase) ? ReadEClock(&value) : 0;

    MeasureMmio(bi, stats);
    MeasureCpBatch(bi, stats);
    TestCpFunction(bi, stats);
    TestIndirectBuffer(bi, stats, segmentPool);
    stats->CpActive = RadeonCpIsReady(bi);

    /* Counting starts clean so the first RectFill run is not polluted. */
    RadeonDebugReads = 0;
    RadeonDebugWrites = 0;

    AddPort(&DebugNode->Port);
    RLOG("Radeon9200: debug port at %lx stats at %lx\n",
         (ULONG)&DebugNode->Port, (ULONG)stats);
}

void RadeonDebugBoardLock(struct BoardInfo *bi)
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct Task *task;

    if (!DebugNode || !SysBase)
        return;
    task = FindTask(NULL);
    ++DebugNode->Stats.BoardLockChecks;
    if (bi->BoardLock.ss_Owner == task)
        ++DebugNode->Stats.BoardLockOwned;
    else if (bi->BoardLock.ss_Owner)
        ++DebugNode->Stats.BoardLockOwnedByOther;
}

void RadeonDebugExecutePhase(ULONG phase, ULONG start)
{
    struct RadeonDebugStats *stats;
    ULONG elapsed;

    if (!DebugNode)
        return;
    elapsed = Clock() - start;
    stats = &DebugNode->Stats;
    if (phase == RADEON_DEBUG_EXEC_COPY)
        stats->ExecuteCopyTicks += elapsed;
    else if (phase == RADEON_DEBUG_EXEC_BUILD)
        stats->ExecuteBuildTicks += elapsed;
    else if (phase == RADEON_DEBUG_EXEC_SUBMIT)
        stats->ExecuteSubmitTicks += elapsed;
}

void RadeonDebugExecuteSample(ULONG recordDwords, ULONG generatedDwords)
{
    struct RadeonDebugStats *stats;

    if (!DebugNode)
        return;
    stats = &DebugNode->Stats;
    ++stats->ExecuteCalls;
    stats->ExecuteRecordDwords += recordDwords;
    stats->ExecuteGeneratedDwords += generatedDwords;
}

void RadeonDebugFallbackDrain(ULONG skipped)
{
    if (!DebugNode)
        return;
    if (skipped)
        ++DebugNode->Stats.FallbackDrainSkipped;
    else
        ++DebugNode->Stats.FallbackDrainRequired;
}

void RadeonDebugFallbackProbe(struct BoardInfo *bi)
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RenderInfo systemRender;
    struct RenderInfo vramRender;
    volatile UBYTE *vram;
    UBYTE *memory;
    UBYTE saved;
    UBYTE savedNext;
    ULONG start;
    ULONG index;
    BOOL success;

    if (!DebugNode || !SysBase || !bi->MemoryBase ||
        !bi->FillRectDefault || !bi->BlitRectNoMaskCompleteDefault)
        return;
    memory = AllocMem(64UL * 64UL, MEMF_PUBLIC | MEMF_CLEAR);
    if (!memory)
        return;
    systemRender.Memory = memory;
    systemRender.BytesPerRow = 64;
    systemRender.pad = 0;
    systemRender.RGBFormat = RGBFB_CLUT;

    start = Clock();
    for (index = 0; index < FALLBACK_PROBE_CALLS; ++index)
        RadeonFillRect(bi, &systemRender,
                       (WORD)(index & 63UL),
                       (WORD)((index >> 6) & 63UL),
                       1, 1, index, 0xffU, RGBFB_CLUT);
    DebugNode->Stats.FallbackProbeTicks = Clock() - start;
    DebugNode->Stats.FallbackProbeCalls = FALLBACK_PROBE_CALLS;
    success = memory[0] == 0 && memory[1] == 1 && memory[63] == 63;

    vram = (volatile UBYTE *)bi->MemoryBase;
    saved = *vram;
    vramRender.Memory = (APTR)vram;
    vramRender.BytesPerRow = 64;
    vramRender.pad = 0;
    vramRender.RGBFormat = RGBFB_CLUT;
    memory[0] = 0x5aU;
    RadeonBlitRectNoMaskComplete(bi, &systemRender, &vramRender,
                                 0, 0, 0, 0, 1, 1, 0x0cU, RGBFB_CLUT);
    success = success && *vram == 0x5aU;
    *vram = 0xa5U;
    RadeonBlitRectNoMaskComplete(bi, &vramRender, &systemRender,
                                 0, 0, 0, 0, 1, 1, 0x0cU, RGBFB_CLUT);
    success = success && memory[0] == 0xa5U;
    *vram = saved;
    DebugNode->Stats.FallbackProbeSuccess = success;

    savedNext = vram[64];
    vram[0] = 0x5aU;
    vram[64] = 0xa5U;
    start = Clock();
    for (index = 0; index < FALLBACK_PROBE_CALLS; ++index)
        RadeonBlitRect(bi, &vramRender, 0, 0, 64, 0,
                       1, 1, 0xffU, RGBFB_CLUT);
    DebugNode->Stats.BlitRectProbeTicks = Clock() - start;
    DebugNode->Stats.BlitRectProbeCalls = FALLBACK_PROBE_CALLS;
    DebugNode->Stats.BlitRectBoundsSuccess = vram[64] == 0xa5U;
    vram[0] = saved;
    vram[64] = savedNext;
    FreeMem(memory, 64UL * 64UL);
}

void RadeonDebugClose(struct BoardInfo *bi)
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;

    if (!SysBase || !DebugNode)
        return;
    RemPort(&DebugNode->Port);
    CloseTimer(SysBase);
    FreeMem(DebugNode, sizeof(*DebugNode));
    DebugNode = NULL;
}

void RadeonDebugBegin(struct RadeonDebugSample *sample)
{
    sample->Ticks = Clock();
    sample->Reads = RadeonDebugReads;
    sample->Writes = RadeonDebugWrites;
}

void RadeonDebugEndFill(const struct RadeonDebugSample *sample)
{
    struct RadeonDebugStats *stats;

    if (!DebugNode)
        return;
    stats = &DebugNode->Stats;
    ++stats->FillCount;
    stats->FillTicks += Clock() - sample->Ticks;
    stats->FillReads += RadeonDebugReads - sample->Reads;
    stats->FillWrites += RadeonDebugWrites - sample->Writes;
    stats->Reads = RadeonDebugReads;
    stats->Writes = RadeonDebugWrites;
}

void RadeonDebugEndCall(const struct RadeonDebugSample *sample,
                        ULONG hardware)
{
    struct RadeonDebugStats *stats;
    ULONG elapsed;

    if (!DebugNode)
        return;
    elapsed = Clock() - sample->Ticks;
    stats = &DebugNode->Stats;
    ++stats->FillCalls;
    stats->FillTotalTicks += elapsed;
    if (hardware)
        ++stats->FillHardware;
    else
        ++stats->FillSoftware;
    /*
     * Feed the version 6 table from the sample RectFill already takes, rather
     * than bracketing the call a second time: a ReadEClock pair costs more
     * than a hardware fill submission does.
     */
    ++stats->OpCalls[RADEON_DEBUG_OP_FILL];
    stats->OpTicks[RADEON_DEBUG_OP_FILL] += elapsed;
    if (hardware)
        ++stats->OpHardware[RADEON_DEBUG_OP_FILL];
    else
        ++stats->OpSoftware[RADEON_DEBUG_OP_FILL];
}

void RadeonDebugEndDrain(const struct RadeonDebugSample *sample)
{
    struct RadeonDebugStats *stats;

    if (!DebugNode)
        return;
    stats = &DebugNode->Stats;
    ++stats->DrainCount;
    stats->DrainTicks += Clock() - sample->Ticks;
    /* No hardware/software split applies: WaitBlitter never falls back. */
    ++stats->OpCalls[RADEON_DEBUG_OP_DRAIN];
    stats->OpTicks[RADEON_DEBUG_OP_DRAIN] = stats->DrainTicks;
    stats->DrainReads += RadeonDebugReads - sample->Reads;
    stats->DrainWrites += RadeonDebugWrites - sample->Writes;
    stats->Reads = RadeonDebugReads;
    stats->Writes = RadeonDebugWrites;
}

void RadeonDebugSpriteCall(ULONG function)
{
    struct RadeonDebugStats *stats;

    if (!DebugNode)
        return;
    stats = &DebugNode->Stats;
    switch (function) {
    case 0:
        ++stats->SetSpriteCalls;
        break;
    case 1:
        ++stats->SetSpritePositionCalls;
        break;
    case 2:
        ++stats->SetSpriteImageCalls;
        break;
    case 3:
        ++stats->SetSpriteColorCalls;
        break;
    }
}

void RadeonDebugTemplateCall(UWORD width, UWORD height, UBYTE drawMode)
{
    struct RadeonDebugStats *stats;

    if (!DebugNode)
        return;
    stats = &DebugNode->Stats;
    ++stats->TemplateCalls;
    stats->TemplateWidthTotal += width;
    if (width > stats->TemplateMaxWidth)
        stats->TemplateMaxWidth = width;
    if (height > stats->TemplateMaxHeight)
        stats->TemplateMaxHeight = height;
    if (drawMode == 0)
        ++stats->TemplateJam1;
    else if (drawMode == 1)
        ++stats->TemplateJam2;
    else
        ++stats->TemplateOtherMode;
}

void RadeonDebugTemplateHardware(ULONG cacheHit, ULONG uploadWords)
{
    if (!DebugNode)
        return;
    ++DebugNode->Stats.TemplateHardware;
    DebugNode->Stats.TemplateCacheHits += cacheHit != 0;
    DebugNode->Stats.TemplateUploadWords += uploadWords;
}

void RadeonDebugTemplateSoftware(void)
{
    if (DebugNode)
        ++DebugNode->Stats.TemplateSoftware;
}

void RadeonDebugCompleteCall(ULONG flags, UBYTE opcode)
{
    struct RadeonDebugStats *stats;

    if (!DebugNode)
        return;
    stats = &DebugNode->Stats;
    ++stats->CompleteCalls;
    ++stats->CompleteOpcode[opcode & 0x0fU];
    stats->CompleteUnequalPitch +=
        (flags & RDEBUG_COMPLETE_UNEQUAL_PITCH) != 0;
    stats->CompleteOpcodeReject +=
        (flags & RDEBUG_COMPLETE_OPCODE_REJECT) != 0;
    stats->CompleteOverlapReject +=
        (flags & RDEBUG_COMPLETE_OVERLAP_REJECT) != 0;
    stats->CompleteSurfaceSoftware +=
        (flags & RDEBUG_COMPLETE_SURFACE_SOFTWARE) != 0;
    stats->CompleteSurfaceReject +=
        (flags & RDEBUG_COMPLETE_SURFACE_REJECT) != 0;
    stats->CompleteAccelUnavailable +=
        (flags & RDEBUG_COMPLETE_ACCEL_UNAVAILABLE) != 0;
}

void RadeonDebugCompleteHardware(void)
{
    if (DebugNode)
        ++DebugNode->Stats.CompleteHardware;
}

void RadeonDebugCompleteSoftware(void)
{
    if (DebugNode)
        ++DebugNode->Stats.CompleteSoftware;
}

/*
 * One entry point for every 2D callback, so a single pair of host reads taken
 * around an interactive action attributes it across the whole callback set
 * rather than to RectFill alone.
 */
void RadeonDebugOpEnd(const struct RadeonDebugSample *sample, ULONG op,
                      ULONG hardware)
{
    struct RadeonDebugStats *stats;

    if (!DebugNode || op >= RADEON_DEBUG_OP_COUNT)
        return;
    stats = &DebugNode->Stats;
    ++stats->OpCalls[op];
    stats->OpTicks[op] += Clock() - sample->Ticks;
    if (hardware)
        ++stats->OpHardware[op];
    else
        ++stats->OpSoftware[op];
}

void RadeonDebugWait(ULONG kind, ULONG polls, ULONG success, ULONG status,
                     ULONG pending)
{
    struct RadeonDebugStats *stats;
    ULONG *calls;
    ULONG *total;
    ULONG *maximum;
    ULONG *failures;

    if (!DebugNode)
        return;
    stats = &DebugNode->Stats;
    if (kind == RADEON_DEBUG_WAIT_FIFO) {
        calls = &stats->FifoWaitCalls;
        total = &stats->FifoWaitPolls;
        maximum = &stats->FifoWaitMaxPolls;
        failures = &stats->FifoWaitFailures;
    } else {
        calls = &stats->IdleWaitCalls;
        total = &stats->IdleWaitPolls;
        maximum = &stats->IdleWaitMaxPolls;
        failures = &stats->IdleWaitFailures;
    }
    ++*calls;
    *total += polls;
    if (polls > *maximum)
        *maximum = polls;
    if (!success) {
        ++*failures;
        stats->LastWaitStatus = status;
        stats->LastWaitKind = kind;
        stats->LastWaitPending = pending;
    }
}

void RadeonDebugRecovery(ULONG success, ULONG accelState)
{
    if (!DebugNode)
        return;
    ++DebugNode->Stats.RecoveryCalls;
    if (success)
        ++DebugNode->Stats.RecoverySuccess;
    else
        ++DebugNode->Stats.RecoveryFailure;
    DebugNode->Stats.FinalAccelState = accelState;
}

void RadeonDebugCompleteSubmit(ULONG success)
{
    if (!DebugNode)
        return;
    ++DebugNode->Stats.CompleteSubmitCalls;
    DebugNode->Stats.CompleteSubmitSuccess += success != 0;
}

ULONG RadeonDebugPhaseBegin(void)
{
    return Clock();
}

void RadeonDebugCompletePhase(ULONG phase, ULONG start)
{
    struct RadeonDebugStats *stats;
    ULONG elapsed;
    ULONG *total;
    ULONG *maximum;

    if (!DebugNode)
        return;
    elapsed = Clock() - start;
    stats = &DebugNode->Stats;
    if (phase == RADEON_DEBUG_COMPLETE_VALIDATE) {
        total = &stats->CompleteValidateTicks;
        maximum = &stats->CompleteValidateMaxTicks;
    } else if (phase == RADEON_DEBUG_COMPLETE_SUBMIT) {
        total = &stats->CompleteSubmitTicks;
        maximum = &stats->CompleteSubmitMaxTicks;
    } else {
        total = &stats->CompleteDefaultTicks;
        maximum = &stats->CompleteDefaultMaxTicks;
    }
    *total += elapsed;
    if (elapsed > *maximum)
        *maximum = elapsed;
}

#else

extern int RadeonDebugTranslationUnitNotEmpty;

#endif
