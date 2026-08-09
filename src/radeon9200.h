#ifndef RADEON9200_H
#define RADEON9200_H

#include <exec/types.h>
#include <libraries/openpci.h>

#include <boardinfo.h>

#define RADEON_VENDOR_ATI 0x1002U

#define RADEON_DEVICE_RV280_5960 0x5960U
#define RADEON_DEVICE_RV280_5961 0x5961U
#define RADEON_DEVICE_RV280_5964 0x5964U

struct RadeonDmaArena;

struct RadeonBoardData {
    struct pci_dev *Device;
    ULONG MmioSize;
    ULONG InstalledVram;
    struct RadeonDmaArena *DmaArena;
    struct ModeInfo *StartupMode;
    UWORD DeviceId;
    UWORD SavedCommand;
    UBYTE Obtained;
    UBYTE CommandSaved;
    UBYTE Initialized;
    UBYTE ModeValid;
    ULONG RefClockKHz;
    ULONG MinPllKHz;
    ULONG MaxPllKHz;
    ULONG MemoryClockHz;
    ULONG CurrentPitch;
    UWORD RefDivider;
    UWORD FeedbackDivider;
    UBYTE PostDividerCode;
    UBYTE PostDivider;
    UBYTE DisplayEnabled;
    UBYTE DpmsLevel;
    ULONG FramebufferGpuBase;
    UBYTE AccelState;
    UBYTE AccelPending;
    UBYTE AccelRecoveryTried;
    UBYTE AccelTimeouts;
};

#define RADEON_ACCEL_OFF      0U
#define RADEON_ACCEL_READY    1U
#define RADEON_ACCEL_FALLBACK 2U
#define RADEON_ACCEL_UNSAFE   3U

struct RadeonCardBase {
    struct CardBase Card;
    struct Library *OpenPciBase;
    struct BoardInfo *BoardInfo;
};

typedef char RadeonBoardDataFitsCardData[
    sizeof(struct RadeonBoardData) <= sizeof(((struct BoardInfo *)0)->CardData)
        ? 1
        : -1];

struct RadeonBoardData *RadeonGetBoardData(struct BoardInfo *bi);
void RadeonReleaseBoard(struct RadeonCardBase *base);

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
BOOL RadeonInitializeAcceleration(struct BoardInfo *bi);
void RadeonShutdownAcceleration(struct BoardInfo *bi);
void RadeonWaitBlitter(__REGA0(struct BoardInfo *bi));
void RadeonFillRect(__REGA0(struct BoardInfo *bi),
                    __REGA1(struct RenderInfo *render),
                    __REGD0(WORD x), __REGD1(WORD y),
                    __REGD2(WORD width), __REGD3(WORD height),
                    __REGD4(ULONG pen), __REGD5(UBYTE mask),
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
void RadeonBlitRectNoMaskComplete(
    __REGA0(struct BoardInfo *bi),
    __REGA1(struct RenderInfo *sourceRender),
    __REGA2(struct RenderInfo *destinationRender),
    __REGD0(WORD srcX), __REGD1(WORD srcY),
    __REGD2(WORD dstX), __REGD3(WORD dstY),
    __REGD4(WORD width), __REGD5(WORD height),
    __REGD6(UBYTE opcode), __REGD7(RGBFTYPE format));
void RadeonInstallCallbacks(struct BoardInfo *bi);
BOOL RadeonShowStartupScreen(struct BoardInfo *bi);
BOOL RadeonReserveDmaMemory(struct BoardInfo *bi, ULONG requestedSize);
void RadeonDestroyDmaMemory(struct BoardInfo *bi);

#ifdef DEBUG
#include <clib/debug_protos.h>
#define RLOG(format, ...) \
    KPrintF((CONST_STRPTR)(format), ##__VA_ARGS__)
#else
#define RLOG(...) ((void)0)
#endif

BOOL FindCard(__REGA0(struct BoardInfo *bi),
              __REGA6(struct RadeonCardBase *base));
BOOL InitCard(__REGA0(struct BoardInfo *bi),
              __REGA1(char **toolTypes),
              __REGA6(struct RadeonCardBase *base));

#endif
