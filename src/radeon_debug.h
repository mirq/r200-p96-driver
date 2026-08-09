/*
 * DEBUG-only observability surface.
 *
 * Three rounds of performance testing were invalidated because there was no
 * way to tell from the host what the driver was actually doing: whether
 * ToolTypes reached InitCard, whether the CP came up, and where the time in a
 * RectFill went. This publishes a public exec message port whose allocation
 * carries a stats block immediately after it, so the host can find the port
 * with a port listing and then read the numbers straight out of memory.
 *
 * Release builds compile every hook below to nothing.
 */
#ifndef RADEON_DEBUG_H
#define RADEON_DEBUG_H

#include <exec/types.h>

struct BoardInfo;

#define RADEON_DEBUG_MAGIC   0x52393244UL /* 'R92D' */
#define RADEON_DEBUG_VERSION 3UL
#define RADEON_DEBUG_PORT    "Radeon9200.Debug"

/*
 * Laid out immediately after the struct MsgPort in one allocation. The host
 * reads it at portAddress + sizeof(struct MsgPort) and checks Magic first.
 */
struct RadeonDebugStats {
    ULONG Magic;
    ULONG Version;
    ULONG CpRequested;     /* CP=YES seen in the icon ToolTypes */
    ULONG CpActive;        /* RadeonCpInitialize() succeeded */
    ULONG DmaRequested;    /* bytes parsed from DMASIZE, 0 if absent */
    ULONG DmaReserved;     /* arena actually created */
    ULONG BoardMemorySize; /* bi->MemorySize after any reservation */
    ULONG EClockRate;      /* EClock ticks per second, 0 if no timer */
    ULONG MmioSamples;     /* iterations in the MMIO microbenchmark */
    ULONG MmioReadTicks;   /* EClock ticks for MmioSamples reads */
    ULONG MmioWriteTicks;  /* EClock ticks for MmioSamples writes */
    ULONG ClockTicks;      /* EClock ticks for MmioSamples ReadEClock calls,
                            * i.e. the cost the per-op samples themselves add */
    ULONG Reads;           /* lifetime RadeonRead32 calls */
    ULONG Writes;          /* lifetime RadeonWrite32 calls */
    ULONG FillCount;
    ULONG FillTicks;       /* time inside SubmitFill only */
    ULONG FillReads;
    ULONG FillWrites;
    ULONG DrainCount;
    ULONG DrainTicks;      /* time inside WaitBlitter's SynchronizeEngine */
    ULONG DrainReads;
    ULONG DrainWrites;
    /* Version 2: whole-call accounting, to find time outside SubmitFill. */
    ULONG FillCalls;       /* every RadeonFillRect entry */
    ULONG FillTotalTicks;  /* whole call, including ValidateSurface */
    ULONG FillHardware;    /* reached SubmitFill */
    ULONG FillSoftware;    /* fell back to FillRectDefault */
    /* Version 3: temporary software-sprite elimination experiment. */
    ULONG SpriteExperiment;
    ULONG SetSpriteCalls;
    ULONG SetSpritePositionCalls;
    ULONG SetSpriteImageCalls;
    ULONG SetSpriteColorCalls;
};

struct RadeonDebugSample {
    ULONG Ticks;
    ULONG Reads;
    ULONG Writes;
};

#ifdef DEBUG

extern ULONG RadeonDebugReads;
extern ULONG RadeonDebugWrites;

void RadeonDebugOpen(struct BoardInfo *bi, ULONG cpRequested,
                     ULONG dmaRequested, ULONG spriteExperiment);
void RadeonDebugClose(struct BoardInfo *bi);
void RadeonDebugBegin(struct RadeonDebugSample *sample);
void RadeonDebugEndFill(const struct RadeonDebugSample *sample);
void RadeonDebugEndDrain(const struct RadeonDebugSample *sample);
void RadeonDebugEndCall(const struct RadeonDebugSample *sample,
                        ULONG hardware);
void RadeonDebugSpriteCall(ULONG function);

#define RDEBUG_COUNT_READ()      (++RadeonDebugReads)
#define RDEBUG_COUNT_WRITE()     (++RadeonDebugWrites)
#define RDEBUG_OPEN(bi, cp, dma, sprite) \
    RadeonDebugOpen((bi), (cp), (dma), (sprite))
#define RDEBUG_CLOSE(bi)         RadeonDebugClose(bi)
#define RDEBUG_SAMPLE            struct RadeonDebugSample rdSample;
#define RDEBUG_BEGIN()           RadeonDebugBegin(&rdSample)
#define RDEBUG_END_FILL()        RadeonDebugEndFill(&rdSample)
#define RDEBUG_END_DRAIN()       RadeonDebugEndDrain(&rdSample)
#define RDEBUG_SAMPLE_OUTER      struct RadeonDebugSample rdOuter; \
                                 ULONG rdHardware = 0;
#define RDEBUG_BEGIN_OUTER()     RadeonDebugBegin(&rdOuter)
#define RDEBUG_MARK_HARDWARE()   (rdHardware = 1)
#define RDEBUG_END_CALL()        RadeonDebugEndCall(&rdOuter, rdHardware)
#define RDEBUG_SPRITE_CALL(fn)   RadeonDebugSpriteCall(fn)

#else

#define RDEBUG_COUNT_READ()      ((void)0)
#define RDEBUG_COUNT_WRITE()     ((void)0)
#define RDEBUG_OPEN(bi, cp, dma, sprite) ((void)0)
#define RDEBUG_CLOSE(bi)         ((void)0)
#define RDEBUG_SAMPLE
#define RDEBUG_BEGIN()           ((void)0)
#define RDEBUG_END_FILL()        ((void)0)
#define RDEBUG_END_DRAIN()       ((void)0)
#define RDEBUG_SAMPLE_OUTER
#define RDEBUG_BEGIN_OUTER()     ((void)0)
#define RDEBUG_MARK_HARDWARE()   ((void)0)
#define RDEBUG_END_CALL()        ((void)0)
#define RDEBUG_SPRITE_CALL(fn)   ((void)0)

#endif

#endif
