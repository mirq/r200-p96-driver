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

    /*
     * Consecutive longwords into the framebuffer aperture, modelling the
     * upload a VRAM-staged BlitTemplate would perform. Uses the tail of the
     * board pool, which Picasso96 has not begun allocating from at InitCard
     * time, so it cannot disturb the startup screen.
     */
    if (bi->MemoryBase && bi->MemorySize > 32768UL) {
        volatile ULONG *target = (volatile ULONG *)
            ((UBYTE *)bi->MemoryBase + bi->MemorySize - 16384UL);

        start = Clock();
        for (index = 0; index < MMIO_SAMPLE_COUNT; ++index)
            target[index & 2047UL] = 0;
        stats->VramWriteTicks = Clock() - start;
    }

    stats->MmioSamples = MMIO_SAMPLE_COUNT;
}

void RadeonDebugOpen(struct BoardInfo *bi, ULONG cpRequested,
                     ULONG dmaRequested, ULONG spriteExperiment)
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
    stats->DmaReserved = data->DmaArena != NULL;
    stats->BoardMemorySize = bi->MemorySize;
    stats->SpriteExperiment = spriteExperiment;
    stats->MonoFromMemory = RadeonMonoProbeResult;
    stats->MonoProbeSample = RadeonMonoProbeSample;
    stats->MonoProbeSampleAlt = RadeonMonoProbeSampleAlt;
    stats->EClockRate = OpenTimer(SysBase) ? ReadEClock(&value) : 0;

    MeasureMmio(bi, stats);

    /* Counting starts clean so the first RectFill run is not polluted. */
    RadeonDebugReads = 0;
    RadeonDebugWrites = 0;

    AddPort(&DebugNode->Port);
    RLOG("Radeon9200: debug port at %lx stats at %lx\n",
         (ULONG)&DebugNode->Port, (ULONG)stats);
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
