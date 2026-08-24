#ifndef RADEON9200_H
#define RADEON9200_H

#include <exec/types.h>
#include <prometheus.h>

#include <boardinfo.h>
#include <prometheus_radeon.h>
#include <radeon3d.h>

#define RADEON_VENDOR_ATI 0x1002U

#define RADEON_DEVICE_RV280_5960 0x5960U
#define RADEON_DEVICE_RV280_5961 0x5961U
#define RADEON_DEVICE_RV280_5964 0x5964U

struct RadeonDmaArena;
struct RadeonCpState;
struct RadeonCursorState;

#define RADEON3D_SERVICE_EMPTY        0UL
#define RADEON3D_SERVICE_INITIALIZING 1UL
#define RADEON3D_SERVICE_ATTACHED     2UL
#define RADEON3D_SERVICE_READY        3UL
#define RADEON3D_SERVICE_DETACHING    4UL

struct RadeonTmdsPll {
    UWORD Limit10KHz;
    UWORD Reserved;
    ULONG Value;
};

struct RadeonDviInfo {
    struct RadeonTmdsPll Pll[4];
    UBYTE PllCount;
    UBYTE ConnectorType;
    UBYTE DdcType;
    UBYTE Reserved;
};

struct RadeonBoardData {
    PCIBoard *Device;
    ULONG MmioSize;
    ULONG InstalledVram;
    struct ModeInfo *StartupMode;
    UWORD DeviceId;
    ULONG Initialized : 1;
    ULONG ModeValid : 1;
    ULONG DisplayEnabled : 1;
    ULONG AccelRecoveryTried : 1;
    ULONG AccelPending : 2;
    /* A staging buffer was hidden above bi->MemorySize for VRAM-staged
     * BlitTemplate expands; its offset is bi->MemorySize itself. */
    ULONG TemplateStaging : 1;
    ULONG RequestedOutput : 2;
    ULONG DviTimingReady : 1;
    ULONG DviFormatReady : 1;
    ULONG ReservedFlags : 21;
    ULONG RefClockKHz;
    ULONG MinPllKHz;
    ULONG MaxPllKHz;
    ULONG MemoryClockHz;
    struct RadeonCpState *CpState;
    UWORD RefDivider;
    UBYTE DpmsLevel;
    UBYTE AccelState;
    UBYTE Need2DRestore;
    ULONG FramebufferGpuBase;
    struct RadeonCursorState *CursorState;
    struct RadeonDviInfo *DviInfo;
    BOOL ASM (*FreeCardMemDefault)(__REGA0(struct BoardInfo *bi),
                                   __REGA1(APTR memory));
};

#define RADEON_ACCEL_OFF      0U
#define RADEON_ACCEL_READY    1U
#define RADEON_ACCEL_FALLBACK 2U
#define RADEON_ACCEL_UNSAFE   3U

#define RADEON_PENDING_NONE 0U
#define RADEON_PENDING_MMIO 1U
#define RADEON_PENDING_CP   2U

struct RadeonChipBase {
    struct Library Library;
    UBYTE Flags;
    UBYTE Pad;
    struct ExecBase *ExecBase;
    APTR SegList;
    struct Library *PrometheusBase;
    struct BoardInfo *BoardInfo;
    struct SignalSemaphore ServiceLock;
    struct MinList ServiceDevices;
    struct MinList RetiredServiceDevices;
    ULONG ServiceGeneration;
    ULONG ServiceSessions;
    ULONG ServiceNextHandle;
    ULONG ServiceState;
    APTR StreamSegmentPool;
    ULONG StreamSegmentGpuBase;
    ULONG StreamSegmentMask;
    /* Cumulative Radeon3DExecute phase attribution. The timer pair opens
     * lazily on the first session; totals are microseconds across every
     * Execute since the driver loaded and survive session closes, so a
     * later Radeon3DGetInfo can attribute a finished workload. */
    APTR ExecTimerPort;
    APTR ExecTimerIO;
    UBYTE ExecTimerFailed;
    ULONG ExecCopyMicros;
    ULONG ExecBuildMicros;
    ULONG ExecSubmitMicros;
    ULONG CommitFailStage;
    ULONG ExecCalls;
    ULONG ExecRecordDwords;
    ULONG ExecGeneratedDwords;
};

typedef char RadeonChipBaseExecBaseOffsetCheck[
    __builtin_offsetof(struct RadeonChipBase, ExecBase) ==
            __builtin_offsetof(struct ChipBase, ExecBase)
        ? 1
        : -1];
typedef char RadeonChipBaseSegListOffsetCheck[
    __builtin_offsetof(struct RadeonChipBase, SegList) ==
            __builtin_offsetof(struct ChipBase, SegList)
        ? 1
        : -1];

typedef char RadeonBoardDataFitsChipData[
    sizeof(struct RadeonBoardData) <= sizeof(((struct BoardInfo *)0)->ChipData)
        ? 1
        : -1];

static __inline__ struct RadeonBoardData *RadeonGetBoardData(
    struct BoardInfo *bi)
{
    return bi ? (struct RadeonBoardData *)bi->ChipData : NULL;
}
BOOL RadeonReleaseBoard(struct RadeonChipBase *base,
                        struct BoardInfo *expectedBoard,
                        BOOL allowInitializing);
void Radeon3DAdvanceGeneration(struct RadeonChipBase *base);
void Radeon3DInvalidateService(struct BoardInfo *bi);
BOOL Radeon3DRearmService(struct BoardInfo *bi);
void Radeon3DFreeRetiredDevices(struct RadeonChipBase *base);

ULONG RadeonRead32(struct BoardInfo *bi, ULONG reg);
BOOL RadeonWrite32(struct BoardInfo *bi, ULONG reg, ULONG value);
BOOL RadeonMask32(struct BoardInfo *bi, ULONG reg, ULONG clear, ULONG set);
ULONG RadeonReadIndexed(struct BoardInfo *bi, ULONG index);
BOOL RadeonWriteIndexed(struct BoardInfo *bi, ULONG index, ULONG value);
BOOL RadeonMaskIndexed(struct BoardInfo *bi, ULONG index, ULONG andMask,
                       ULONG orMask);
ULONG RadeonReadPll(struct BoardInfo *bi, UBYTE index);
BOOL RadeonWritePll(struct BoardInfo *bi, UBYTE index, ULONG value);
BOOL RadeonMaskPll(struct BoardInfo *bi, UBYTE index, ULONG andMask,
                   ULONG orMask);
void RadeonDelayUs(ULONG microseconds);

BOOL RadeonInitializeHardware(struct BoardInfo *bi);
BOOL RadeonInitializeAcceleration(struct BoardInfo *bi, BOOL enableCp,
                                   BOOL stageTemplates);
void RadeonShutdownAcceleration(struct BoardInfo *bi);
BOOL RadeonPrepare3D(struct BoardInfo *bi);
void RadeonMark3DSubmitted(struct BoardInfo *bi);
BOOL RadeonRecoverAcceleration(struct BoardInfo *bi);
void RadeonWaitBlitter(__REGA0(struct BoardInfo *bi));
void RadeonFillRect(__REGA0(struct BoardInfo *bi),
                    __REGA1(struct RenderInfo *render),
                    __REGD0(WORD x), __REGD1(WORD y),
                    __REGD2(WORD width), __REGD3(WORD height),
                    __REGD4(ULONG pen), __REGD5(UBYTE mask),
                    __REGD7(RGBFTYPE format));
void RadeonDrawLine(__REGA0(struct BoardInfo *bi),
                    __REGA1(struct RenderInfo *render),
                    __REGA2(struct Line *line),
                    __REGD0(UBYTE mask),
                    __REGD7(RGBFTYPE format));
void RadeonInvertRect(__REGA0(struct BoardInfo *bi),
                      __REGA1(struct RenderInfo *render),
                      __REGD0(WORD x), __REGD1(WORD y),
                      __REGD2(WORD width), __REGD3(WORD height),
                      __REGD4(UBYTE mask),
                      __REGD7(RGBFTYPE format));
void RadeonBlitRect(__REGA0(struct BoardInfo *bi),
                    __REGA1(struct RenderInfo *render),
                    __REGD0(WORD srcX), __REGD1(WORD srcY),
                    __REGD2(WORD dstX), __REGD3(WORD dstY),
                    __REGD4(WORD width), __REGD5(WORD height),
                    __REGD6(UBYTE mask), __REGD7(RGBFTYPE format));
void RadeonBlitPattern(__REGA0(struct BoardInfo *bi),
                       __REGA1(struct RenderInfo *render),
                       __REGA2(struct Pattern *pattern),
                       __REGD0(WORD x), __REGD1(WORD y),
                       __REGD2(WORD width), __REGD3(WORD height),
                       __REGD4(UBYTE mask),
                       __REGD7(RGBFTYPE format));
void RadeonBlitTemplate(__REGA0(struct BoardInfo *bi),
                        __REGA1(struct RenderInfo *render),
                        __REGA2(struct Template *template),
                        __REGD0(WORD x), __REGD1(WORD y),
                        __REGD2(WORD width), __REGD3(WORD height),
                        __REGD4(UBYTE mask),
                        __REGD7(RGBFTYPE format));
void RadeonBlitRectNoMaskComplete(
    __REGA0(struct BoardInfo *bi),
    __REGA1(struct RenderInfo *sourceRender),
    __REGA2(struct RenderInfo *destinationRender),
    __REGD0(WORD srcX), __REGD1(WORD srcY),
    __REGD2(WORD dstX), __REGD3(WORD dstY),
    __REGD4(WORD width), __REGD5(WORD height),
    __REGD6(UBYTE opcode), __REGD7(RGBFTYPE format));
void RadeonInstallCallbacks(struct BoardInfo *bi, BOOL hardwareSprite,
                            BOOL hardwareText);
BOOL RadeonInitializeCursor(struct BoardInfo *bi);
void RadeonShutdownCursor(struct BoardInfo *bi);
void RadeonRefreshCursorPosition(struct BoardInfo *bi);
BOOL RadeonSetSprite(__REGA0(struct BoardInfo *bi),
                     __REGD0(BOOL active),
                     __REGD7(RGBFTYPE format));
BOOL RadeonEnableSoftSprite(__REGA0(struct BoardInfo *bi),
                            __REGD0(ULONG formatFlags),
                            __REGA1(struct ModeInfo *mode));
void RadeonSetSpritePosition(__REGA0(struct BoardInfo *bi),
                             __REGD0(WORD x), __REGD1(WORD y),
                             __REGD7(RGBFTYPE format));
void RadeonSetSpriteImage(__REGA0(struct BoardInfo *bi),
                          __REGD7(RGBFTYPE format));
void RadeonSetSpriteColor(__REGA0(struct BoardInfo *bi),
                          __REGD0(UBYTE index),
                          __REGD1(UBYTE red),
                          __REGD2(UBYTE green),
                          __REGD3(UBYTE blue),
                          __REGD7(RGBFTYPE format));
BOOL RadeonShowStartupScreen(struct BoardInfo *bi);
APTR RadeonAllocatePrivateVram(struct BoardInfo *bi, ULONG requestedSize);
BOOL RadeonFreePrivateVram(struct BoardInfo *bi, APTR memory,
                           ULONG requestedSize);
BOOL RadeonCpInitialize(struct BoardInfo *bi);
BOOL RadeonCpRecover(struct BoardInfo *bi);
void RadeonCpShutdown(struct BoardInfo *bi);
void RadeonCpAbort(struct BoardInfo *bi);
BOOL RadeonCpIsReady(struct BoardInfo *bi);
BOOL RadeonCpSubmit(struct BoardInfo *bi, const ULONG *commands,
                    ULONG commandCount);
BOOL RadeonCpSubmitStream(struct BoardInfo *bi, const ULONG *commands,
                          ULONG commandCount, BOOL addFence,
                          ULONG *fenceOut);
BOOL RadeonCpTestFence(struct BoardInfo *bi, ULONG fence);
BOOL RadeonCpWaitFence(struct BoardInfo *bi, ULONG fence,
                       ULONG timeoutMs);
BOOL RadeonCpWait(struct BoardInfo *bi);
#ifdef DEBUG
struct RadeonCpDebugResult {
    ULONG WrapBefore;
    ULONG WrapAfter;
    ULONG WrapSuccess;
    ULONG NearFullSuccess;
    ULONG ReserveTimeoutSuccess;
    ULONG FirstFence;
    ULONG SecondFence;
    ULONG FenceOrderSuccess;
    ULONG FenceZeroPollSuccess;
    ULONG FenceZeroPollTicks;
    ULONG FenceTimeoutSuccess;
    ULONG FenceTimeoutTicks;
};

BOOL RadeonCpDebugSubmitNoops(struct BoardInfo *bi, ULONG commandCount,
                               BOOL direct);
BOOL RadeonCpDebugRunTests(struct BoardInfo *bi,
                           struct RadeonCpDebugResult *result);
#endif

#ifdef DEBUG
#include <clib/debug_protos.h>
#define RLOG(format, ...) \
    KPrintF((CONST_STRPTR)(format), ##__VA_ARGS__)
#else
#define RLOG(...) ((void)0)
#endif

BOOL InitChip(__REGA0(struct BoardInfo *bi),
              __REGA6(struct RadeonChipBase *base));
BOOL InitRadeonFeatures(__REGA0(struct BoardInfo *bi),
                         __REGD0(ULONG features),
                         __REGA6(struct RadeonChipBase *base));
struct Radeon3DDevice *Radeon3DOpen(
    __REGD0(ULONG requestedVersion),
    __REGA0(struct Radeon3DInfo *info),
    __REGA6(struct RadeonChipBase *base));
void Radeon3DClose(__REGA0(struct Radeon3DDevice *device),
                   __REGA6(struct RadeonChipBase *base));
BOOL Radeon3DGetInfo(__REGA0(struct Radeon3DDevice *device),
                     __REGA1(struct Radeon3DInfo *info),
                     __REGA6(struct RadeonChipBase *base));
BOOL Radeon3DSubmit(__REGA0(struct Radeon3DDevice *device),
                    __REGA1(const ULONG *commands),
                    __REGD0(ULONG commandCount),
                    __REGD1(ULONG flags),
                    __REGA2(ULONG *fenceOut),
                    __REGA6(struct RadeonChipBase *base));
BOOL Radeon3DTestFence(__REGA0(struct Radeon3DDevice *device),
                       __REGD0(ULONG fence),
                       __REGA6(struct RadeonChipBase *base));
BOOL Radeon3DWaitFence(__REGA0(struct Radeon3DDevice *device),
                       __REGD0(ULONG fence),
                       __REGD1(ULONG timeoutMs),
                       __REGA6(struct RadeonChipBase *base));
BOOL Radeon3DImportBitMap(__REGA0(struct Radeon3DDevice *device),
                          __REGA1(struct BitMap *bitmap),
                          __REGA2(struct Radeon3DSurface *surface),
                          __REGA6(struct RadeonChipBase *base));
void Radeon3DReleaseSurface(__REGA0(struct Radeon3DDevice *device),
                            __REGA1(struct Radeon3DSurface *surface),
                             __REGA6(struct RadeonChipBase *base));
BOOL Radeon3DExecute(__REGA0(struct Radeon3DDevice *device),
                     __REGA1(const ULONG *records),
                     __REGD0(ULONG recordDwords),
                     __REGD1(ULONG flags),
                     __REGA2(ULONG *fenceOut),
                      __REGA6(struct RadeonChipBase *base));
BOOL Radeon3DInvalidateForTest(__REGA0(struct Radeon3DDevice *device),
                               __REGA6(struct RadeonChipBase *base));
BOOL Radeon3DAllocSegment(__REGA0(struct Radeon3DDevice *device),
                          __REGD0(ULONG bytes),
                          __REGA1(struct Radeon3DSegment *segment),
                          __REGA6(struct RadeonChipBase *base));
BOOL Radeon3DFreeSegment(__REGA0(struct Radeon3DDevice *device),
                         __REGD0(ULONG segmentId),
                         __REGA6(struct RadeonChipBase *base));
BOOL Radeon3DCommitDraw(__REGA0(struct Radeon3DDevice *device),
                        __REGA1(const struct Radeon3DCommit *commit),
                        __REGA2(ULONG *fenceOut),
                        __REGA6(struct RadeonChipBase *base));
BOOL Radeon3DCommitBatch(__REGA0(struct Radeon3DDevice *device),
                         __REGA1(const struct Radeon3DCommitBatch *commit),
                         __REGA2(ULONG *fenceOut),
                         __REGA6(struct RadeonChipBase *base));
BOOL Radeon3DDetachOwner(__REGA0(struct BoardInfo *bi),
                         __REGA6(struct RadeonChipBase *base));

#endif
