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

    if (!DebugNode)
        return;
    stats = &DebugNode->Stats;
    ++stats->FillCalls;
    stats->FillTotalTicks += Clock() - sample->Ticks;
    if (hardware)
        ++stats->FillHardware;
    else
        ++stats->FillSoftware;
}

void RadeonDebugEndDrain(const struct RadeonDebugSample *sample)
{
    struct RadeonDebugStats *stats;

    if (!DebugNode)
        return;
    stats = &DebugNode->Stats;
    ++stats->DrainCount;
    stats->DrainTicks += Clock() - sample->Ticks;
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

#else

extern int RadeonDebugTranslationUnitNotEmpty;

#endif
