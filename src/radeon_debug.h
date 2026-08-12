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
#define RADEON_DEBUG_VERSION 11UL

/* Result of the monochrome-source-from-memory capability probe. */
#define RADEON_PROBE_NOTRUN  0UL
#define RADEON_PROBE_OK      1UL
#define RADEON_PROBE_WRONG   2UL
#define RADEON_PROBE_FAILED  3UL
#define RADEON_PROBE_SKIPPED 4UL
#define RADEON_DEBUG_PORT    "Radeon9200.Debug"

/* Index into the version 6 OpCalls/OpHardware/OpSoftware/OpTicks arrays. */
#define RADEON_DEBUG_OP_FILL     0
#define RADEON_DEBUG_OP_INVERT   1
#define RADEON_DEBUG_OP_COPY     2
#define RADEON_DEBUG_OP_PATTERN  3
#define RADEON_DEBUG_OP_TEMPLATE 4
#define RADEON_DEBUG_OP_COMPLETE 5
#define RADEON_DEBUG_OP_LINE     6
#define RADEON_DEBUG_OP_DRAIN    7
#define RADEON_DEBUG_OP_COUNT    8

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
    /* Version 4: BlitTemplate path selection and workload shape. */
    ULONG TemplateCalls;
    ULONG TemplateHardware;
    ULONG TemplateSoftware;
    ULONG TemplateCacheHits;
    ULONG TemplateJam1;
    ULONG TemplateJam2;
    ULONG TemplateOtherMode;
    ULONG TemplateWidthTotal;
    ULONG TemplateUploadWords;
    ULONG TemplateMaxWidth;
    ULONG TemplateMaxHeight;
    /* Version 5: cross-surface copy path, attributes, and rejection reasons. */
    ULONG CompleteCalls;
    ULONG CompleteHardware;
    ULONG CompleteSoftware;
    ULONG CompleteUnequalPitch;
    ULONG CompleteOpcodeReject;
    ULONG CompleteOverlapReject;
    ULONG CompleteSurfaceSoftware;
    ULONG CompleteSurfaceReject;
    ULONG CompleteAccelUnavailable;
    /*
     * Version 6: whole-callback accounting for every 2D entry point.
     *
     * Versions 2-5 could only attribute time inside RectFill, which is what
     * P96Speed measures. Interactive work - opening a window over another one -
     * is a mix of every callback below, and nothing recorded how that mix is
     * shaped. Reading these deltas around a single window open says directly
     * which callback the time belongs to, and how much of it fell back to the
     * CPU, without guessing from a benchmark that never issues that mix.
     */
    ULONG OpCalls[RADEON_DEBUG_OP_COUNT];
    ULONG OpHardware[RADEON_DEBUG_OP_COUNT];
    ULONG OpSoftware[RADEON_DEBUG_OP_COUNT];
    ULONG OpTicks[RADEON_DEBUG_OP_COUNT];
    /*
     * Version 7: cost of writing consecutive longwords into the framebuffer
     * aperture, for the same sample count as MmioWriteTicks. BlitTemplate
     * currently streams every glyph row through a HOST_DATA register at
     * MMIO cost; expanding from a VRAM staging buffer instead is only worth
     * building if aperture writes are materially cheaper.
     */
    ULONG VramWriteTicks;
    /*
     * Version 8: can the 2D engine colour-expand a monochrome source read
     * from VRAM instead of streamed through HOST_DATA? If it can, the
     * template upload can move to the aperture, which measured 2.19x
     * cheaper per longword than a register write.
     */
    ULONG MonoFromMemory;
    /* First four expanded destination bytes, to tell a wrong expansion from
     * a blit that never landed (the probe pre-fills the target with 0x55). */
    ULONG MonoProbeSample;
    /* Same four bytes with GMC_BYTE_LSB_TO_MSB, to identify which flag
     * yields Amiga MSB-first ordering. */
    ULONG MonoProbeSampleAlt;
    /* Version 9: bounded-wait and recovery attribution for long UI stalls. */
    ULONG FifoWaitCalls;
    ULONG FifoWaitPolls;
    ULONG FifoWaitMaxPolls;
    ULONG FifoWaitFailures;
    ULONG IdleWaitCalls;
    ULONG IdleWaitPolls;
    ULONG IdleWaitMaxPolls;
    ULONG IdleWaitFailures;
    ULONG RecoveryCalls;
    ULONG RecoverySuccess;
    ULONG RecoveryFailure;
    ULONG CompleteSubmitCalls;
    ULONG CompleteSubmitSuccess;
    ULONG LastWaitStatus;
    ULONG LastWaitKind;
    ULONG LastWaitPending;
    ULONG FinalAccelState;
    /* Version 10: complete-copy minterms selected by layers.library. */
    ULONG CompleteOpcode[16];
    /* Version 11: direct phase timing inside complete-copy dispatch. */
    ULONG CompleteValidateTicks;
    ULONG CompleteSubmitTicks;
    ULONG CompleteDefaultTicks;
    ULONG CompleteValidateMaxTicks;
    ULONG CompleteSubmitMaxTicks;
    ULONG CompleteDefaultMaxTicks;
};

#define RADEON_DEBUG_WAIT_FIFO 1UL
#define RADEON_DEBUG_WAIT_IDLE 2UL

#define RDEBUG_COMPLETE_UNEQUAL_PITCH   (1UL << 0)
#define RDEBUG_COMPLETE_OPCODE_REJECT   (1UL << 1)
#define RDEBUG_COMPLETE_OVERLAP_REJECT  (1UL << 2)
#define RDEBUG_COMPLETE_SURFACE_SOFTWARE (1UL << 3)
#define RDEBUG_COMPLETE_SURFACE_REJECT  (1UL << 4)
#define RDEBUG_COMPLETE_ACCEL_UNAVAILABLE (1UL << 5)

struct RadeonDebugSample {
    ULONG Ticks;
    ULONG Reads;
    ULONG Writes;
};

#ifdef DEBUG

extern ULONG RadeonDebugReads;
extern ULONG RadeonDebugWrites;
/* Set by the acceleration probe before RadeonDebugOpen() publishes the port. */
extern ULONG RadeonMonoProbeResult;
extern ULONG RadeonMonoProbeSample;
extern ULONG RadeonMonoProbeSampleAlt;

void RadeonDebugOpen(struct BoardInfo *bi, ULONG cpRequested,
                     ULONG dmaRequested, ULONG spriteExperiment);
void RadeonDebugClose(struct BoardInfo *bi);
void RadeonDebugBegin(struct RadeonDebugSample *sample);
void RadeonDebugEndFill(const struct RadeonDebugSample *sample);
void RadeonDebugEndDrain(const struct RadeonDebugSample *sample);
void RadeonDebugEndCall(const struct RadeonDebugSample *sample,
                        ULONG hardware);
void RadeonDebugSpriteCall(ULONG function);
void RadeonDebugTemplateCall(UWORD width, UWORD height, UBYTE drawMode);
void RadeonDebugTemplateHardware(ULONG cacheHit, ULONG uploadWords);
void RadeonDebugTemplateSoftware(void);
void RadeonDebugCompleteCall(ULONG flags, UBYTE opcode);
void RadeonDebugCompleteHardware(void);
void RadeonDebugCompleteSoftware(void);
void RadeonDebugOpEnd(const struct RadeonDebugSample *sample, ULONG op,
                      ULONG hardware);
void RadeonDebugWait(ULONG kind, ULONG polls, ULONG success, ULONG status,
                     ULONG pending);
void RadeonDebugRecovery(ULONG success, ULONG accelState);
void RadeonDebugCompleteSubmit(ULONG success);
ULONG RadeonDebugPhaseBegin(void);
void RadeonDebugCompletePhase(ULONG phase, ULONG start);

#define RADEON_DEBUG_COMPLETE_VALIDATE 0UL
#define RADEON_DEBUG_COMPLETE_SUBMIT   1UL
#define RADEON_DEBUG_COMPLETE_DEFAULT  2UL

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
#define RDEBUG_TEMPLATE_CALL(w, h, mode) \
    RadeonDebugTemplateCall((w), (h), (mode))
#define RDEBUG_TEMPLATE_HARDWARE(hit, words) \
    RadeonDebugTemplateHardware((hit), (words))
#define RDEBUG_TEMPLATE_SOFTWARE() RadeonDebugTemplateSoftware()
#define RDEBUG_COMPLETE_CALL(flags, opcode) \
    RadeonDebugCompleteCall((flags), (opcode))
#define RDEBUG_COMPLETE_HARDWARE() RadeonDebugCompleteHardware()
#define RDEBUG_COMPLETE_SOFTWARE() RadeonDebugCompleteSoftware()
#define RDEBUG_OP_SAMPLE         struct RadeonDebugSample rdOp; \
                                 ULONG rdOpHardware = 0;
#define RDEBUG_OP_BEGIN()        RadeonDebugBegin(&rdOp)
#define RDEBUG_OP_HARDWARE()     (rdOpHardware = 1)
#define RDEBUG_OP_END(op)        RadeonDebugOpEnd(&rdOp, (op), rdOpHardware)
#define RDEBUG_WAIT(kind, polls, success, status, pending) \
    RadeonDebugWait((kind), (polls), (success), (status), (pending))
#define RDEBUG_RECOVERY(success, state) \
    RadeonDebugRecovery((success), (state))
#define RDEBUG_COMPLETE_SUBMIT(success) \
    RadeonDebugCompleteSubmit((success))
#define RDEBUG_PHASE_BEGIN() RadeonDebugPhaseBegin()
#define RDEBUG_COMPLETE_PHASE(phase, start) \
    RadeonDebugCompletePhase((phase), (start))

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
#define RDEBUG_TEMPLATE_CALL(w, h, mode) ((void)0)
#define RDEBUG_TEMPLATE_HARDWARE(hit, words) ((void)0)
#define RDEBUG_TEMPLATE_SOFTWARE() ((void)0)
#define RDEBUG_COMPLETE_CALL(flags, opcode) \
    do { (void)(flags); (void)(opcode); } while (0)
#define RDEBUG_COMPLETE_HARDWARE() ((void)0)
#define RDEBUG_COMPLETE_SOFTWARE() ((void)0)
#define RDEBUG_OP_SAMPLE
#define RDEBUG_OP_BEGIN()        ((void)0)
#define RDEBUG_OP_HARDWARE()     ((void)0)
#define RDEBUG_OP_END(op)        ((void)0)
#define RDEBUG_WAIT(kind, polls, success, status, pending) ((void)0)
#define RDEBUG_RECOVERY(success, state) ((void)0)
#define RDEBUG_COMPLETE_SUBMIT(success) ((void)0)
#define RDEBUG_PHASE_BEGIN() 0UL
#define RDEBUG_COMPLETE_PHASE(phase, start) ((void)0)

#endif

#endif
