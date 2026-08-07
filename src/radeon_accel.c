/*
 * Legacy Radeon direct-MMIO 2D command sequences follow the Xorg Radeon
 * driver's ACCEL_MMIO path and Linux radeonfb's bounded engine reset model.
 */

#include <proto/exec.h>

#include "radeon9200.h"
#include "radeon_regs.h"

#define ACCEL_TIMEOUT_POLLS 100000UL
#define ACCEL_MAX_PITCH     16320UL
#define ACCEL_MAX_COORD     8191UL

#ifdef DEBUG
static UBYTE FillLogged;
static UBYTE CopyLogged;
static UBYTE CompleteCopyLogged;
#endif

enum SurfaceResult {
    SURFACE_REJECT,
    SURFACE_SOFTWARE,
    SURFACE_HARDWARE
};

struct AccelSurface {
    ULONG PitchOffset;
    ULONG StartOffset;
    ULONG EndOffset;
    UWORD XBias;
    UWORD YBias;
};

static BOOL WaitFifo(struct BoardInfo *bi, ULONG entries)
{
    ULONG count;

    for (count = 0; count < ACCEL_TIMEOUT_POLLS; ++count) {
        if ((RadeonRead32(bi, RADEON_RBBM_STATUS) &
             RADEON_RBBM_FIFOCNT_MASK) >= entries)
            return TRUE;
        RadeonDelayUs(1);
    }
    return FALSE;
}

static BOOL WaitIdleAndFlush(struct BoardInfo *bi)
{
    ULONG count;

    if (!WaitFifo(bi, 64))
        return FALSE;
    for (count = 0; count < ACCEL_TIMEOUT_POLLS; ++count) {
        if (!(RadeonRead32(bi, RADEON_RBBM_STATUS) &
              RADEON_RBBM_ACTIVE))
            break;
        RadeonDelayUs(1);
    }
    if (count == ACCEL_TIMEOUT_POLLS ||
        !RadeonWrite32(bi, RADEON_DSTCACHE_CTLSTAT,
                       RADEON_RB2D_DC_FLUSH_ALL) ||
        !WaitFifo(bi, 64))
        return FALSE;

    for (count = 0; count < ACCEL_TIMEOUT_POLLS; ++count) {
        if (!(RadeonRead32(bi, RADEON_DSTCACHE_CTLSTAT) &
              RADEON_RB2D_DC_BUSY))
            return TRUE;
        RadeonDelayUs(1);
    }
    return FALSE;
}

static BOOL ResetEngine(struct BoardInfo *bi)
{
    const ULONG resetMask = RADEON_SOFT_RESET_CP |
                            RADEON_SOFT_RESET_HI |
                            RADEON_SOFT_RESET_SE |
                            RADEON_SOFT_RESET_RE |
                            RADEON_SOFT_RESET_PP |
                            RADEON_SOFT_RESET_E2 |
                            RADEON_SOFT_RESET_RB;
    ULONG reset = RadeonRead32(bi, RADEON_RBBM_SOFT_RESET);
    ULONG host = RadeonRead32(bi, RADEON_HOST_PATH_CNTL);

    if (!RadeonWrite32(bi, RADEON_RBBM_SOFT_RESET, reset | resetMask))
        return FALSE;
    (void)RadeonRead32(bi, RADEON_RBBM_SOFT_RESET);
    if (!RadeonWrite32(bi, RADEON_RBBM_SOFT_RESET,
                       reset & ~resetMask))
        return FALSE;
    (void)RadeonRead32(bi, RADEON_RBBM_SOFT_RESET);

    if (!RadeonWrite32(bi, RADEON_HOST_PATH_CNTL,
                       host | RADEON_HDP_SOFT_RESET))
        return FALSE;
    (void)RadeonRead32(bi, RADEON_HOST_PATH_CNTL);
    if (!RadeonWrite32(bi, RADEON_HOST_PATH_CNTL, host) ||
        !RadeonWrite32(bi, RADEON_RBBM_SOFT_RESET, reset))
        return FALSE;
    (void)RadeonRead32(bi, RADEON_RBBM_SOFT_RESET);
    return TRUE;
}

static BOOL RestoreEngineState(struct BoardInfo *bi)
{
    return RadeonWrite32(bi, RADEON_RB3D_CNTL, 0) &&
           RadeonWrite32(bi, RADEON_RB2D_DSTCACHE_MODE, 0) &&
           RadeonWrite32(bi, RADEON_DP_DATATYPE,
                         RADEON_HOST_BIG_ENDIAN_EN) &&
           RadeonWrite32(bi, RADEON_DEFAULT_PITCH_OFFSET, 0) &&
           RadeonWrite32(bi, RADEON_DEFAULT_SC_BOTTOM_RIGHT,
                         RADEON_SCISSOR_MAX) &&
           RadeonWrite32(bi, RADEON_SC_TOP_LEFT, 0) &&
           RadeonWrite32(bi, RADEON_SC_BOTTOM_RIGHT,
                         RADEON_SCISSOR_MAX) &&
           RadeonWrite32(bi, RADEON_DP_BRUSH_FRGD_CLR,
                         0xffffffffUL) &&
           RadeonWrite32(bi, RADEON_DP_BRUSH_BKGD_CLR, 0) &&
           RadeonWrite32(bi, RADEON_DP_SRC_FRGD_CLR,
                         0xffffffffUL) &&
           RadeonWrite32(bi, RADEON_DP_SRC_BKGD_CLR, 0) &&
           RadeonWrite32(bi, RADEON_DP_WRITE_MASK,
                         0xffffffffUL) &&
           RadeonWrite32(bi, RADEON_DP_CNTL,
                         RADEON_DST_X_LEFT_TO_RIGHT |
                             RADEON_DST_Y_TOP_TO_BOTTOM);
}

static BOOL RecoverEngine(struct BoardInfo *bi)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    if (!data || data->AccelRecoveryTried)
        return FALSE;
    data->AccelRecoveryTried = TRUE;
    ++data->AccelTimeouts;
    data->AccelPending = FALSE;

    if (!ResetEngine(bi) || !RestoreEngineState(bi) ||
        !WaitIdleAndFlush(bi)) {
        data->AccelState = RADEON_ACCEL_UNSAFE;
        RLOG("Radeon9200: 2D engine recovery failed\n");
        return FALSE;
    }

    data->AccelState = RADEON_ACCEL_FALLBACK;
    RLOG("Radeon9200: 2D engine timed out; using software fallbacks\n");
    return TRUE;
}

static BOOL SynchronizeEngine(struct BoardInfo *bi)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    if (!data || !data->AccelPending)
        return !data || data->AccelState != RADEON_ACCEL_UNSAFE;
    if (WaitIdleAndFlush(bi)) {
        data->AccelPending = FALSE;
        return TRUE;
    }
    return RecoverEngine(bi);
}

static ULONG HardwareDatatype(RGBFTYPE format, ULONG *bytesPerPixel)
{
    switch (format) {
    case RGBFB_CLUT:
        *bytesPerPixel = 1;
        return RADEON_GMC_DST_8BPP_CI;
    case RGBFB_R5G6B5PC:
        *bytesPerPixel = 2;
        return RADEON_GMC_DST_16BPP;
    case RGBFB_B8G8R8A8:
        *bytesPerPixel = 4;
        return RADEON_GMC_DST_32BPP;
    default:
        *bytesPerPixel = 0;
        return 0;
    }
}

static enum SurfaceResult ValidateSurface(
    struct BoardInfo *bi, const struct RenderInfo *render,
    WORD x, WORD y, WORD width, WORD height, RGBFTYPE format,
    struct AccelSurface *surface)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    ULONG bytesPerPixel;
    ULONG datatype = HardwareDatatype(format, &bytesPerPixel);
    ULONG memoryBase;
    ULONG memoryAddress;
    ULONG memoryLimit;
    ULONG apertureLimit;
    ULONG surfaceOffset;
    ULONG gpuAddress;
    ULONG alignedGpuAddress;
    ULONG addressBias;
    ULONG xBias;
    ULONG yBias;
    ULONG pitch;
    unsigned long long lineEnd;
    unsigned long long rectangleStart;
    unsigned long long rectangleEnd;

    (void)datatype;
    if (!data || !render || !render->Memory || !surface ||
        !bytesPerPixel || render->BytesPerRow <= 0)
        return SURFACE_SOFTWARE;
    if (width <= 0 || height <= 0)
        return SURFACE_REJECT;
    if (x < 0 || y < 0)
        return SURFACE_SOFTWARE;

    memoryBase = (ULONG)bi->MemoryBase;
    memoryAddress = (ULONG)render->Memory;
    memoryLimit = bi->MemorySize;
    if (data->InstalledVram < memoryLimit)
        memoryLimit = data->InstalledVram;
    if (bi->MemorySpaceSize < memoryLimit)
        memoryLimit = bi->MemorySpaceSize;
    apertureLimit = bi->MemorySpaceSize;

    if (memoryAddress < memoryBase ||
        (unsigned long long)memoryAddress >=
            (unsigned long long)memoryBase + apertureLimit)
        return SURFACE_SOFTWARE;
    surfaceOffset = memoryAddress - memoryBase;
    if (surfaceOffset >= memoryLimit)
        return SURFACE_REJECT;

    pitch = (ULONG)(UWORD)render->BytesPerRow;
    lineEnd = ((unsigned long long)(UWORD)x + (UWORD)width) *
              bytesPerPixel;
    rectangleEnd = (unsigned long long)surfaceOffset +
                   ((unsigned long long)(UWORD)y +
                    (UWORD)height - 1ULL) * pitch + lineEnd;
    rectangleStart = (unsigned long long)surfaceOffset +
                     (unsigned long long)(UWORD)y * pitch +
                     (unsigned long long)(UWORD)x * bytesPerPixel;
    if (lineEnd > pitch || rectangleEnd > memoryLimit)
        return SURFACE_REJECT;

    if ((pitch & 63UL) || pitch > ACCEL_MAX_PITCH)
        return SURFACE_SOFTWARE;

    if (data->FramebufferGpuBase > 0xffffffffUL - surfaceOffset)
        return SURFACE_REJECT;
    gpuAddress = data->FramebufferGpuBase + surfaceOffset;
    alignedGpuAddress = gpuAddress & ~1023UL;
    if (alignedGpuAddress > 0xfffffc00UL)
        return SURFACE_SOFTWARE;
    addressBias = gpuAddress - alignedGpuAddress;
    yBias = addressBias / pitch;
    addressBias -= yBias * pitch;
    if (addressBias % bytesPerPixel)
        return SURFACE_SOFTWARE;
    xBias = addressBias / bytesPerPixel;
    if (xBias + (UWORD)x + (UWORD)width - 1UL > ACCEL_MAX_COORD ||
        yBias + (UWORD)y + (UWORD)height - 1UL > ACCEL_MAX_COORD)
        return SURFACE_SOFTWARE;

    surface->PitchOffset = ((pitch >> 6) << 22) |
                           (alignedGpuAddress >> 10);
    surface->StartOffset = (ULONG)rectangleStart;
    surface->EndOffset = (ULONG)rectangleEnd;
    surface->XBias = (UWORD)xBias;
    surface->YBias = (UWORD)yBias;
    return SURFACE_HARDWARE;
}

static BOOL SubmitFill(struct BoardInfo *bi,
                       const struct AccelSurface *surface,
                       WORD x, WORD y, WORD width, WORD height,
                       ULONG pen, UBYTE mask, RGBFTYPE format)
{
    ULONG bytesPerPixel;
    ULONG datatype = HardwareDatatype(format, &bytesPerPixel);
    ULONG writeMask = format == RGBFB_CLUT ? (ULONG)mask :
                                                   0xffffffffUL;
    ULONG master = RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                   RADEON_GMC_BRUSH_SOLID_COLOR | datatype |
                   RADEON_GMC_SRC_DATATYPE_COLOR | RADEON_ROP3_P |
                   RADEON_GMC_CLR_CMP_CNTL_DIS;

    (void)bytesPerPixel;
    x = (WORD)(x + surface->XBias);
    y = (WORD)(y + surface->YBias);
    return WaitFifo(bi, 4) &&
           RadeonWrite32(bi, RADEON_DP_GUI_MASTER_CNTL, master) &&
           RadeonWrite32(bi, RADEON_DP_BRUSH_FRGD_CLR, pen) &&
           RadeonWrite32(bi, RADEON_DP_WRITE_MASK, writeMask) &&
           RadeonWrite32(bi, RADEON_DP_CNTL,
                         RADEON_DST_X_LEFT_TO_RIGHT |
                             RADEON_DST_Y_TOP_TO_BOTTOM) &&
           WaitFifo(bi, 2) &&
           RadeonWrite32(bi, RADEON_DSTCACHE_CTLSTAT,
                         RADEON_RB2D_DC_FLUSH_ALL) &&
           RadeonWrite32(bi, RADEON_WAIT_UNTIL,
                         RADEON_WAIT_2D_IDLECLEAN |
                             RADEON_WAIT_DMA_GUI_IDLE) &&
           WaitFifo(bi, 3) &&
           RadeonWrite32(bi, RADEON_DST_PITCH_OFFSET,
                         surface->PitchOffset) &&
           RadeonWrite32(bi, RADEON_DST_Y_X,
                         ((ULONG)(UWORD)y << 16) | (UWORD)x) &&
           RadeonWrite32(bi, RADEON_DST_WIDTH_HEIGHT,
                         ((ULONG)(UWORD)width << 16) |
                             (UWORD)height);
}

static BOOL SubmitCopy(struct BoardInfo *bi,
                       const struct AccelSurface *source,
                       const struct AccelSurface *destination,
                       WORD srcX, WORD srcY, WORD dstX, WORD dstY,
                       WORD width, WORD height, UBYTE mask,
                       RGBFTYPE format, BOOL sameLayout)
{
    ULONG bytesPerPixel;
    ULONG datatype = HardwareDatatype(format, &bytesPerPixel);
    ULONG writeMask = format == RGBFB_CLUT ? (ULONG)mask :
                                                   0xffffffffUL;
    ULONG master = RADEON_GMC_SRC_PITCH_OFFSET_CNTL |
                   RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                   RADEON_GMC_BRUSH_NONE | datatype |
                   RADEON_GMC_SRC_DATATYPE_COLOR | RADEON_ROP3_S |
                   RADEON_DP_SRC_SOURCE_MEMORY |
                   RADEON_GMC_CLR_CMP_CNTL_DIS;
    ULONG direction = 0;

    (void)bytesPerPixel;
    srcX = (WORD)(srcX + source->XBias);
    dstX = (WORD)(dstX + destination->XBias);
    srcY = (WORD)(srcY + source->YBias);
    dstY = (WORD)(dstY + destination->YBias);
    if (!sameLayout || dstX <= srcX)
        direction |= RADEON_DST_X_LEFT_TO_RIGHT;
    else {
        srcX = (WORD)(srcX + width - 1);
        dstX = (WORD)(dstX + width - 1);
    }
    if (!sameLayout || dstY <= srcY)
        direction |= RADEON_DST_Y_TOP_TO_BOTTOM;
    else {
        srcY = (WORD)(srcY + height - 1);
        dstY = (WORD)(dstY + height - 1);
    }

    return WaitFifo(bi, 3) &&
           RadeonWrite32(bi, RADEON_DP_GUI_MASTER_CNTL, master) &&
           RadeonWrite32(bi, RADEON_DP_WRITE_MASK, writeMask) &&
           RadeonWrite32(bi, RADEON_DP_CNTL, direction) &&
           WaitFifo(bi, 2) &&
           RadeonWrite32(bi, RADEON_DSTCACHE_CTLSTAT,
                         RADEON_RB2D_DC_FLUSH_ALL) &&
           RadeonWrite32(bi, RADEON_WAIT_UNTIL,
                         RADEON_WAIT_2D_IDLECLEAN |
                             RADEON_WAIT_DMA_GUI_IDLE) &&
           WaitFifo(bi, 5) &&
           RadeonWrite32(bi, RADEON_SRC_PITCH_OFFSET,
                          source->PitchOffset) &&
           RadeonWrite32(bi, RADEON_DST_PITCH_OFFSET,
                          destination->PitchOffset) &&
           RadeonWrite32(bi, RADEON_SRC_Y_X,
                         ((ULONG)(UWORD)srcY << 16) | (UWORD)srcX) &&
           RadeonWrite32(bi, RADEON_DST_Y_X,
                         ((ULONG)(UWORD)dstY << 16) | (UWORD)dstX) &&
           RadeonWrite32(bi, RADEON_DST_HEIGHT_WIDTH,
                         ((ULONG)(UWORD)height << 16) |
                             (UWORD)width);
}

BOOL RadeonInitializeAcceleration(struct BoardInfo *bi)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    ULONG location;

    if (!data || !bi->MemoryBase || !bi->MemoryIOBase)
        return FALSE;
    data->AccelState = RADEON_ACCEL_OFF;
    data->AccelPending = FALSE;
    data->AccelRecoveryTried = FALSE;
    data->AccelTimeouts = 0;

    location = (RadeonRead32(bi, RADEON_MC_FB_LOCATION) & 0xffffUL) << 16;
    RLOG("Radeon9200: 2D init fb=%lx mc=%lx status=%lx\n",
         data->FramebufferGpuBase, location,
         RadeonRead32(bi, RADEON_RBBM_STATUS));
    if (location != data->FramebufferGpuBase || !ResetEngine(bi) ||
        !RestoreEngineState(bi) || !WaitIdleAndFlush(bi)) {
        RLOG("Radeon9200: 2D engine initialization failed\n");
        return FALSE;
    }

    data->AccelState = RADEON_ACCEL_READY;
    RLOG("Radeon9200: direct-MMIO 2D engine ready, status=%lx\n",
         RadeonRead32(bi, RADEON_RBBM_STATUS));
    return TRUE;
}

void RadeonShutdownAcceleration(struct BoardInfo *bi)
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    if (!SysBase || !data)
        return;
    ObtainSemaphore(&bi->BoardLock);
    if (data->AccelPending && !WaitIdleAndFlush(bi))
        (void)ResetEngine(bi);
    data->AccelPending = FALSE;
    data->AccelState = RADEON_ACCEL_OFF;
    ReleaseSemaphore(&bi->BoardLock);
}

void RadeonWaitBlitter(__REGA0(struct BoardInfo *bi))
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;

    if (!SysBase)
        return;
    ObtainSemaphore(&bi->BoardLock);
    (void)SynchronizeEngine(bi);
    ReleaseSemaphore(&bi->BoardLock);
}

void RadeonFillRect(__REGA0(struct BoardInfo *bi),
                    __REGA1(struct RenderInfo *render),
                    __REGD0(WORD x), __REGD1(WORD y),
                    __REGD2(WORD width), __REGD3(WORD height),
                    __REGD4(ULONG pen), __REGD5(UBYTE mask),
                    __REGD7(RGBFTYPE format))
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct AccelSurface surface;
    enum SurfaceResult result;
    BOOL software = FALSE;

    if (!SysBase || !data)
        return;
    result = ValidateSurface(bi, render, x, y, width, height,
                             format, &surface);
    if (result == SURFACE_REJECT)
        return;
    if (format != RGBFB_CLUT)
        result = SURFACE_SOFTWARE;

    ObtainSemaphore(&bi->BoardLock);
    if (result == SURFACE_HARDWARE &&
        data->AccelState == RADEON_ACCEL_READY) {
#ifdef DEBUG
        if (!FillLogged) {
            RLOG("Radeon9200: HW FillRect mem=%lx pitchoff=%lx "
                 "xy=%ld,%ld size=%ldx%ld fmt=%ld mask=%lx\n",
                 (ULONG)render->Memory, surface.PitchOffset,
                 (ULONG)(UWORD)x, (ULONG)(UWORD)y,
                 (ULONG)(UWORD)width, (ULONG)(UWORD)height,
                 (ULONG)format, (ULONG)mask);
            FillLogged = TRUE;
        }
#endif
        if (SubmitFill(bi, &surface, x, y, width, height,
                       pen, mask, format))
            data->AccelPending = TRUE;
        else
            software = RecoverEngine(bi);
    } else {
        software = SynchronizeEngine(bi) &&
                   data->AccelState != RADEON_ACCEL_UNSAFE;
    }
    ReleaseSemaphore(&bi->BoardLock);

    if (software && bi->FillRectDefault &&
        bi->FillRectDefault != RadeonFillRect)
        bi->FillRectDefault(bi, render, x, y, width, height,
                            pen, mask, format);
}

void RadeonBlitRect(__REGA0(struct BoardInfo *bi),
                    __REGA1(struct RenderInfo *render),
                    __REGD0(WORD srcX), __REGD1(WORD srcY),
                    __REGD2(WORD dstX), __REGD3(WORD dstY),
                    __REGD4(WORD width), __REGD5(WORD height),
                    __REGD6(UBYTE mask), __REGD7(RGBFTYPE format))
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct AccelSurface source;
    struct AccelSurface destination;
    enum SurfaceResult srcResult;
    enum SurfaceResult dstResult;
    BOOL software = FALSE;

    if (!SysBase || !data)
        return;
    srcResult = ValidateSurface(bi, render, srcX, srcY, width, height,
                                format, &source);
    dstResult = ValidateSurface(bi, render, dstX, dstY, width, height,
                                format, &destination);
    if (srcResult == SURFACE_REJECT || dstResult == SURFACE_REJECT)
        return;

    ObtainSemaphore(&bi->BoardLock);
    if (srcResult == SURFACE_HARDWARE &&
        dstResult == SURFACE_HARDWARE &&
        source.PitchOffset == destination.PitchOffset &&
        data->AccelState == RADEON_ACCEL_READY) {
#ifdef DEBUG
        if (!CopyLogged) {
            RLOG("Radeon9200: HW BlitRect mem=%lx pitchoff=%lx "
                 "src=%ld,%ld dst=%ld,%ld size=%ldx%ld fmt=%ld mask=%lx\n",
                 (ULONG)render->Memory, source.PitchOffset,
                 (ULONG)(UWORD)srcX, (ULONG)(UWORD)srcY,
                 (ULONG)(UWORD)dstX, (ULONG)(UWORD)dstY,
                 (ULONG)(UWORD)width, (ULONG)(UWORD)height,
                 (ULONG)format, (ULONG)mask);
            CopyLogged = TRUE;
        }
#endif
        if (SubmitCopy(bi, &source, &destination,
                       srcX, srcY, dstX, dstY,
                       width, height, mask, format, TRUE))
            data->AccelPending = TRUE;
        else
            software = RecoverEngine(bi);
    } else {
        software = SynchronizeEngine(bi) &&
                   data->AccelState != RADEON_ACCEL_UNSAFE;
    }
    ReleaseSemaphore(&bi->BoardLock);

    if (software && bi->BlitRectDefault &&
        bi->BlitRectDefault != RadeonBlitRect)
        bi->BlitRectDefault(bi, render, srcX, srcY, dstX, dstY,
                            width, height, mask, format);
}

void RadeonBlitRectNoMaskComplete(
    __REGA0(struct BoardInfo *bi),
    __REGA1(struct RenderInfo *sourceRender),
    __REGA2(struct RenderInfo *destinationRender),
    __REGD0(WORD srcX), __REGD1(WORD srcY),
    __REGD2(WORD dstX), __REGD3(WORD dstY),
    __REGD4(WORD width), __REGD5(WORD height),
    __REGD6(UBYTE opcode), __REGD7(RGBFTYPE format))
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct AccelSurface source;
    struct AccelSurface destination;
    enum SurfaceResult srcResult;
    enum SurfaceResult dstResult;
    BOOL disjoint;
    BOOL software = FALSE;

    if (!SysBase || !data)
        return;
    srcResult = ValidateSurface(bi, sourceRender, srcX, srcY,
                                width, height, format, &source);
    dstResult = ValidateSurface(bi, destinationRender, dstX, dstY,
                                width, height, format, &destination);
    if (srcResult == SURFACE_REJECT || dstResult == SURFACE_REJECT)
        return;

    disjoint = srcResult == SURFACE_HARDWARE &&
               dstResult == SURFACE_HARDWARE &&
               (source.EndOffset <= destination.StartOffset ||
                destination.EndOffset <= source.StartOffset);
    ObtainSemaphore(&bi->BoardLock);
    if (opcode == 0x0cU && disjoint && sourceRender &&
        destinationRender &&
        sourceRender->BytesPerRow == destinationRender->BytesPerRow &&
        srcResult == SURFACE_HARDWARE &&
        dstResult == SURFACE_HARDWARE &&
        data->AccelState == RADEON_ACCEL_READY) {
#ifdef DEBUG
        if (!CompleteCopyLogged) {
            RLOG("Radeon9200: HW BlitRectNoMaskComplete "
                 "srcmem=%lx dstmem=%lx srcpo=%lx dstpo=%lx "
                 "src=%ld,%ld dst=%ld,%ld size=%ldx%ld fmt=%ld op=%lx\n",
                 (ULONG)sourceRender->Memory,
                 (ULONG)destinationRender->Memory,
                 source.PitchOffset, destination.PitchOffset,
                 (ULONG)(UWORD)srcX, (ULONG)(UWORD)srcY,
                 (ULONG)(UWORD)dstX, (ULONG)(UWORD)dstY,
                 (ULONG)(UWORD)width, (ULONG)(UWORD)height,
                 (ULONG)format, (ULONG)opcode);
            CompleteCopyLogged = TRUE;
        }
#endif
        if (SubmitCopy(bi, &source, &destination,
                       srcX, srcY, dstX, dstY,
                       width, height, 0xffU, format, FALSE))
            data->AccelPending = TRUE;
        else
            software = RecoverEngine(bi);
    } else {
        software = SynchronizeEngine(bi) &&
                   data->AccelState != RADEON_ACCEL_UNSAFE;
    }
    ReleaseSemaphore(&bi->BoardLock);

    if (software && bi->BlitRectNoMaskCompleteDefault &&
        bi->BlitRectNoMaskCompleteDefault !=
            RadeonBlitRectNoMaskComplete)
        bi->BlitRectNoMaskCompleteDefault(
            bi, sourceRender, destinationRender,
            srcX, srcY, dstX, dstY, width, height, opcode, format);
}
