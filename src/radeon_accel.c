/*
 * Legacy Radeon direct-MMIO 2D command sequences follow the Xorg Radeon
 * driver's ACCEL_MMIO path and Linux radeonfb's bounded engine reset model.
 */

#include <graphics/rastport.h>
#include <hardware/byteswap.h>
#include <proto/exec.h>

#include "radeon9200.h"
#include "radeon_debug.h"
#include "radeon_regs.h"

#define ACCEL_TIMEOUT_POLLS 100000UL
#define ACCEL_SPIN_POLLS    256UL
#define ACCEL_MAX_PITCH     16320UL
#define ACCEL_MAX_COORD     8191UL
/*
 * Hidden above bi->MemorySize so Picasso96 never allocates from it. 64 KiB
 * covers any template the driver accepts: the widest observed run is 630
 * pixels and the tallest 15 rows, which needs 1280 bytes.
 */
#define TEMPLATE_STAGE_SIZE 65536UL

/*
 * The 2D engine consumes a memory-sourced monochrome bitmap LSB-first within
 * each byte, measured on RV280 by expanding 0xC0 and reading back
 * 00 00 00 00 00 00 FF FF. GMC_BYTE_PIX_ORDER does not change this. Amiga
 * templates are MSB-first, so each staged byte is reversed on the way in.
 */
static const UBYTE ReverseByte[256] = {
    0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0,
    0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
    0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8,
    0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
    0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4,
    0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
    0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC,
    0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
    0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2,
    0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
    0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA,
    0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
    0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6,
    0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
    0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE,
    0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
    0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1,
    0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
    0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9,
    0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
    0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5,
    0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
    0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED,
    0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
    0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3,
    0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
    0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB,
    0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
    0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7,
    0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
    0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF,
    0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF
};

#ifdef DEBUG
static UBYTE FillLogged;
static UBYTE CopyLogged;
static UBYTE CompleteCopyLogged;
static UBYTE LineLogged;
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

struct LineSurfaceCache {
    struct BoardInfo *Board;
    APTR Memory;
    WORD BytesPerRow;
    RGBFTYPE Format;
    ULONG PitchOffset;
    ULONG Datatype;
    ULONG BytesPerPixel;
    ULONG MaxX;
    ULONG MaxY;
    UWORD XBias;
    UWORD YBias;
    BOOL Valid;
};

struct LineEngineCache {
    struct BoardInfo *Board;
    ULONG Master;
    ULONG PitchOffset;
    ULONG WriteMask;
    ULONG Pen;
    BOOL Valid;
};

struct TemplateEngineCache {
    struct BoardInfo *Board;
    ULONG Master;
    ULONG PitchOffset;
    ULONG WriteMask;
    ULONG FgPen;
    ULONG BgPen;
    BOOL Valid;
};

static struct LineSurfaceCache LineSurface;
static struct LineEngineCache LineEngine;
static struct TemplateEngineCache TemplateEngine;

static void InvalidateLineEngine(void)
{
    LineEngine.Valid = FALSE;
}

static void InvalidateTemplateEngine(void)
{
    TemplateEngine.Valid = FALSE;
}

static __inline__ ULONG AccelRead32(struct BoardInfo *bi, ULONG reg)
{
    RDEBUG_COUNT_READ();
    return SWAPLONG(*(volatile ULONG *)
                    ((UBYTE *)bi->MemoryIOBase + reg));
}

static __inline__ BOOL AccelWrite32(struct BoardInfo *bi, ULONG reg,
                                    ULONG value)
{
    RDEBUG_COUNT_WRITE();
    *(volatile ULONG *)((UBYTE *)bi->MemoryIOBase + reg) =
        SWAPLONG(value);
    return TRUE;
}

static __inline__ BOOL AccelWriteHostData(struct BoardInfo *bi, ULONG reg,
                                          ULONG value)
{
    RDEBUG_COUNT_WRITE();
    *(volatile ULONG *)((UBYTE *)bi->MemoryIOBase + reg) = value;
    return TRUE;
}

#define RadeonRead32(bi, reg) AccelRead32((bi), (reg))
#define RadeonWrite32(bi, reg, value) \
    AccelWrite32((bi), (reg), (value))

static BOOL WaitFifo(struct BoardInfo *bi, ULONG entries)
{
#ifdef DEBUG
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
#endif
    ULONG count;
    ULONG status = 0;

    if (!entries || entries > 64)
        return FALSE;
    for (count = 0; count < ACCEL_TIMEOUT_POLLS; ++count) {
        status = RadeonRead32(bi, RADEON_RBBM_STATUS);
        if ((status & RADEON_RBBM_FIFOCNT_MASK) >= entries) {
            RDEBUG_WAIT(RADEON_DEBUG_WAIT_FIFO, count + 1UL, TRUE, status,
                        data ? data->AccelPending : RADEON_PENDING_NONE);
            return TRUE;
        }
        if (count >= ACCEL_SPIN_POLLS)
            RadeonDelayUs(1);
    }
    RDEBUG_WAIT(RADEON_DEBUG_WAIT_FIFO, count, FALSE, status,
                data ? data->AccelPending : RADEON_PENDING_NONE);
    return FALSE;
}

static BOOL WaitIdleAndFlush(struct BoardInfo *bi)
{
#ifdef DEBUG
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
#endif
    ULONG count;
    ULONG status = 0;

    if (!WaitFifo(bi, 64))
        return FALSE;
    for (count = 0; count < ACCEL_TIMEOUT_POLLS; ++count) {
        status = RadeonRead32(bi, RADEON_RBBM_STATUS);
        if (!(status & RADEON_RBBM_ACTIVE))
            break;
        if (count >= ACCEL_SPIN_POLLS)
            RadeonDelayUs(1);
    }
    if (count == ACCEL_TIMEOUT_POLLS) {
        RDEBUG_WAIT(RADEON_DEBUG_WAIT_IDLE, count, FALSE, status,
                    data ? data->AccelPending : RADEON_PENDING_NONE);
        return FALSE;
    }

    if (!RadeonWrite32(bi, RADEON_DSTCACHE_CTLSTAT,
                       RADEON_RB2D_DC_FLUSH_ALL))
        return FALSE;
    for (count = 0; count < ACCEL_TIMEOUT_POLLS; ++count) {
        status = RadeonRead32(bi, RADEON_DSTCACHE_CTLSTAT);
        if (!(status & RADEON_RB2D_DC_BUSY)) {
            RDEBUG_WAIT(RADEON_DEBUG_WAIT_IDLE, count + 1UL, TRUE, status,
                        data ? data->AccelPending : RADEON_PENDING_NONE);
            return TRUE;
        }
        if (count >= ACCEL_SPIN_POLLS)
            RadeonDelayUs(1);
    }
    RDEBUG_WAIT(RADEON_DEBUG_WAIT_IDLE, count, FALSE, status,
                data ? data->AccelPending : RADEON_PENDING_NONE);
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
    InvalidateLineEngine();
    InvalidateTemplateEngine();
    /* All later HDP invalidates restore this driver-owned baseline. */
    return RadeonWrite32(bi, RADEON_HOST_PATH_CNTL, 0) &&
           RadeonWrite32(bi, RADEON_RB3D_CNTL, 0) &&
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
    data->AccelPending = RADEON_PENDING_NONE;
    RadeonCpAbort(bi);

    if (!ResetEngine(bi) || !RestoreEngineState(bi) ||
        !WaitIdleAndFlush(bi)) {
        data->AccelState = RADEON_ACCEL_UNSAFE;
        RDEBUG_RECOVERY(FALSE, data->AccelState);
        RLOG("Radeon9200: 2D engine recovery failed\n");
        return FALSE;
    }

    data->AccelState = RADEON_ACCEL_FALLBACK;
    RDEBUG_RECOVERY(TRUE, data->AccelState);
    RLOG("Radeon9200: 2D engine timed out; using software fallbacks\n");
    return TRUE;
}

static BOOL SynchronizeEngine(struct BoardInfo *bi)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    if (!data || !data->AccelPending)
        return !data || data->AccelState != RADEON_ACCEL_UNSAFE;
    if ((data->AccelPending == RADEON_PENDING_CP && RadeonCpWait(bi)) ||
        (data->AccelPending == RADEON_PENDING_MMIO &&
         WaitIdleAndFlush(bi))) {
        data->AccelPending = RADEON_PENDING_NONE;
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

static ULONG HardwarePen(ULONG pen, RGBFTYPE format)
{
    switch (format) {
    case RGBFB_CLUT:
        return pen & 0xffUL;
    case RGBFB_R5G6B5PC:
        return ((pen & 0x00ffUL) << 8) |
               ((pen & 0xff00UL) >> 8);
    case RGBFB_B8G8R8A8:
        return ((pen & 0x000000ffUL) << 24) |
               ((pen & 0x0000ff00UL) << 8) |
               ((pen & 0x00ff0000UL) >> 8) |
               ((pen & 0xff000000UL) >> 24);
    default:
        return pen;
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
    ULONG lineEnd;
    ULONG lastRowOffset;
    ULONG rectangleStart;
    ULONG rectangleEnd;
    ULONG remaining;
    ULONG xEnd;
    ULONG yEnd;

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
        memoryAddress - memoryBase >= apertureLimit)
        return SURFACE_SOFTWARE;
    surfaceOffset = memoryAddress - memoryBase;
    if (surfaceOffset >= memoryLimit)
        return SURFACE_REJECT;

    pitch = (ULONG)(UWORD)render->BytesPerRow;
    if ((pitch & 63UL) || pitch > ACCEL_MAX_PITCH)
        return SURFACE_SOFTWARE;

    xEnd = (ULONG)(UWORD)x + (UWORD)width;
    yEnd = (ULONG)(UWORD)y + (UWORD)height;
    if (xEnd - 1UL > ACCEL_MAX_COORD ||
        yEnd - 1UL > ACCEL_MAX_COORD)
        return SURFACE_SOFTWARE;

    lineEnd = xEnd * bytesPerPixel;
    if (lineEnd > pitch)
        return SURFACE_REJECT;
    lastRowOffset = (yEnd - 1UL) * pitch;
    remaining = memoryLimit - surfaceOffset;
    if (lineEnd > remaining ||
        lastRowOffset > remaining - lineEnd)
        return SURFACE_REJECT;
    rectangleEnd = surfaceOffset + lastRowOffset + lineEnd;
    rectangleStart = surfaceOffset + (ULONG)(UWORD)y * pitch +
                     (ULONG)(UWORD)x * bytesPerPixel;

    if (data->FramebufferGpuBase > 0xffffffffUL - surfaceOffset)
        return SURFACE_REJECT;
    gpuAddress = data->FramebufferGpuBase + surfaceOffset;
    alignedGpuAddress = gpuAddress & ~1023UL;
    if (alignedGpuAddress > 0xfffffc00UL)
        return SURFACE_SOFTWARE;
    addressBias = gpuAddress - alignedGpuAddress;
    if (addressBias) {
        yBias = addressBias / pitch;
        addressBias -= yBias * pitch;
        if (addressBias % bytesPerPixel)
            return SURFACE_SOFTWARE;
        xBias = addressBias / bytesPerPixel;
    } else {
        xBias = 0;
        yBias = 0;
    }
    if (xBias + (UWORD)x + (UWORD)width - 1UL > ACCEL_MAX_COORD ||
        yBias + (UWORD)y + (UWORD)height - 1UL > ACCEL_MAX_COORD)
        return SURFACE_SOFTWARE;

    surface->PitchOffset = ((pitch >> 6) << 22) |
                           (alignedGpuAddress >> 10);
    surface->StartOffset = rectangleStart;
    surface->EndOffset = rectangleEnd;
    surface->XBias = (UWORD)xBias;
    surface->YBias = (UWORD)yBias;
    return SURFACE_HARDWARE;
}

static enum SurfaceResult ValidateLineSurface(
    struct BoardInfo *bi, const struct RenderInfo *render,
    LONG left, LONG top, LONG right, LONG bottom, RGBFTYPE format,
    struct LineSurfaceCache **surface)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    ULONG memoryBase;
    ULONG memoryAddress;
    ULONG memoryLimit;
    ULONG surfaceOffset;
    ULONG gpuAddress;
    ULONG alignedGpuAddress;
    ULONG addressBias;
    ULONG remaining;
    ULONG rows;

    if (!data || !render || !render->Memory || !surface)
        return SURFACE_SOFTWARE;

    if (!LineSurface.Valid || LineSurface.Board != bi ||
        LineSurface.Memory != render->Memory ||
        LineSurface.BytesPerRow != render->BytesPerRow ||
        LineSurface.Format != format) {
        LineSurface.Valid = FALSE;
        LineSurface.Datatype = HardwareDatatype(
            format, &LineSurface.BytesPerPixel);
        if (!LineSurface.BytesPerPixel || render->BytesPerRow <= 0)
            return SURFACE_SOFTWARE;

        memoryBase = (ULONG)bi->MemoryBase;
        memoryAddress = (ULONG)render->Memory;
        if (memoryAddress < memoryBase ||
            memoryAddress - memoryBase >= bi->MemorySpaceSize)
            return SURFACE_SOFTWARE;
        surfaceOffset = memoryAddress - memoryBase;

        memoryLimit = bi->MemorySize;
        if (data->InstalledVram < memoryLimit)
            memoryLimit = data->InstalledVram;
        if (bi->MemorySpaceSize < memoryLimit)
            memoryLimit = bi->MemorySpaceSize;
        if (surfaceOffset >= memoryLimit ||
            data->FramebufferGpuBase > 0xffffffffUL - surfaceOffset)
            return SURFACE_REJECT;

        LineSurface.BytesPerRow = render->BytesPerRow;
        if (((ULONG)(UWORD)LineSurface.BytesPerRow & 63UL) ||
            (ULONG)(UWORD)LineSurface.BytesPerRow > ACCEL_MAX_PITCH)
            return SURFACE_SOFTWARE;
        LineSurface.MaxX = (ULONG)(UWORD)LineSurface.BytesPerRow /
                           LineSurface.BytesPerPixel;
        if (!LineSurface.MaxX)
            return SURFACE_REJECT;
        --LineSurface.MaxX;
        if (LineSurface.MaxX > ACCEL_MAX_COORD)
            LineSurface.MaxX = ACCEL_MAX_COORD;

        remaining = memoryLimit - surfaceOffset;
        rows = remaining / (ULONG)(UWORD)LineSurface.BytesPerRow;
        if (!rows)
            return SURFACE_REJECT;
        LineSurface.MaxY = rows - 1UL;
        if (LineSurface.MaxY > ACCEL_MAX_COORD)
            LineSurface.MaxY = ACCEL_MAX_COORD;

        gpuAddress = data->FramebufferGpuBase + surfaceOffset;
        alignedGpuAddress = gpuAddress & ~1023UL;
        if (alignedGpuAddress > 0xfffffc00UL)
            return SURFACE_SOFTWARE;
        addressBias = gpuAddress - alignedGpuAddress;
        LineSurface.YBias = addressBias /
                            (ULONG)(UWORD)LineSurface.BytesPerRow;
        addressBias -= (ULONG)LineSurface.YBias *
                       (ULONG)(UWORD)LineSurface.BytesPerRow;
        if (addressBias % LineSurface.BytesPerPixel)
            return SURFACE_SOFTWARE;
        LineSurface.XBias = addressBias / LineSurface.BytesPerPixel;
        if (LineSurface.XBias > ACCEL_MAX_COORD ||
            LineSurface.YBias > ACCEL_MAX_COORD)
            return SURFACE_SOFTWARE;
        if (LineSurface.MaxX > ACCEL_MAX_COORD - LineSurface.XBias)
            LineSurface.MaxX = ACCEL_MAX_COORD - LineSurface.XBias;
        if (LineSurface.MaxY > ACCEL_MAX_COORD - LineSurface.YBias)
            LineSurface.MaxY = ACCEL_MAX_COORD - LineSurface.YBias;

        LineSurface.Board = bi;
        LineSurface.Memory = render->Memory;
        LineSurface.Format = format;
        LineSurface.PitchOffset =
            (((ULONG)(UWORD)LineSurface.BytesPerRow >> 6) << 22) |
            (alignedGpuAddress >> 10);
        LineSurface.Valid = TRUE;
    }

    if (left < 0 || top < 0 || right < left || bottom < top)
        return SURFACE_SOFTWARE;
    if ((ULONG)right > LineSurface.MaxX ||
        (ULONG)bottom > LineSurface.MaxY)
        return SURFACE_REJECT;

    *surface = &LineSurface;
    return SURFACE_HARDWARE;
}

static BOOL PrepareMonoPattern(struct BoardInfo *bi,
                               const struct Pattern *pattern,
                               ULONG *data0, ULONG *data1)
{
    const UBYTE *source;
    UBYTE rows[8];
    ULONG memoryAddress;
    ULONG memoryBase;
    ULONG patternHeight;
    ULONG row;

    if (!bi || !pattern || !pattern->Memory || !data0 || !data1 ||
        pattern->Size > 3 || pattern->DrawMode != JAM2)
        return FALSE;

    memoryAddress = (ULONG)pattern->Memory;
    memoryBase = (ULONG)bi->MemoryBase;
    if (memoryAddress >= memoryBase &&
        (unsigned long long)memoryAddress <
            (unsigned long long)memoryBase + bi->MemorySpaceSize)
        return FALSE;

    source = (const UBYTE *)pattern->Memory;
    patternHeight = 1UL << pattern->Size;
    for (row = 0; row < patternHeight; ++row) {
        if (source[row * 2UL] != source[row * 2UL + 1UL])
            return FALSE;
    }
    for (row = 0; row < 8; ++row)
        rows[row] = source[(row & (patternHeight - 1UL)) * 2UL];

    *data0 = (ULONG)rows[0] | ((ULONG)rows[1] << 8) |
             ((ULONG)rows[2] << 16) | ((ULONG)rows[3] << 24);
    *data1 = (ULONG)rows[4] | ((ULONG)rows[5] << 8) |
             ((ULONG)rows[6] << 16) | ((ULONG)rows[7] << 24);
    return TRUE;
}

static BOOL SubmitSolidRect(struct BoardInfo *bi,
                            const struct AccelSurface *surface,
                            WORD x, WORD y, WORD width, WORD height,
                            ULONG pen, UBYTE mask, RGBFTYPE format,
                            ULONG rop)
{
    ULONG bytesPerPixel;
    ULONG datatype = HardwareDatatype(format, &bytesPerPixel);
    ULONG writeMask = format == RGBFB_CLUT ? (ULONG)mask :
                                                    0xffffffffUL;
    ULONG master = RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                    RADEON_GMC_BRUSH_SOLID_COLOR | datatype |
                     RADEON_GMC_SRC_DATATYPE_COLOR | rop |
                     RADEON_GMC_CLR_CMP_CNTL_DIS;
    (void)bytesPerPixel;
    InvalidateLineEngine();
    InvalidateTemplateEngine();
    pen = HardwarePen(pen, format);
    x = (WORD)(x + surface->XBias);
    y = (WORD)(y + surface->YBias);
    return RadeonWrite32(bi, RADEON_DP_GUI_MASTER_CNTL, master) &&
           RadeonWrite32(bi, RADEON_DP_BRUSH_FRGD_CLR, pen) &&
           RadeonWrite32(bi, RADEON_DP_WRITE_MASK, writeMask) &&
           RadeonWrite32(bi, RADEON_DP_CNTL,
                          RADEON_DST_X_LEFT_TO_RIGHT |
                              RADEON_DST_Y_TOP_TO_BOTTOM) &&
           RadeonWrite32(bi, RADEON_DST_PITCH_OFFSET,
                          surface->PitchOffset) &&
           RadeonWrite32(bi, RADEON_DST_Y_X,
                         ((ULONG)(UWORD)y << 16) | (UWORD)x) &&
           RadeonWrite32(bi, RADEON_DST_WIDTH_HEIGHT,
                         ((ULONG)(UWORD)width << 16) |
                              (UWORD)height);
}

static BOOL SubmitPattern(struct BoardInfo *bi,
                          const struct AccelSurface *surface,
                          const struct Pattern *pattern,
                          WORD x, WORD y, WORD width, WORD height,
                          UBYTE mask, RGBFTYPE format,
                          ULONG data0, ULONG data1)
{
    ULONG bytesPerPixel;
    ULONG datatype = HardwareDatatype(format, &bytesPerPixel);
    ULONG writeMask = format == RGBFB_CLUT ? (ULONG)mask :
                                                   0xffffffffUL;
    ULONG master = RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                   RADEON_GMC_BRUSH_8X8_MONO_FG_BG | datatype |
                   RADEON_ROP3_P | RADEON_GMC_CLR_CMP_CNTL_DIS;
    ULONG destinationX = (ULONG)(UWORD)(x + surface->XBias);
    ULONG destinationY = (ULONG)(UWORD)(y + surface->YBias);
    ULONG phaseX = ((pattern->XOffset & 7UL) + 8UL -
                    (destinationX & 7UL)) & 7UL;
    ULONG phaseY = ((pattern->YOffset & 7UL) + 8UL -
                    (destinationY & 7UL)) & 7UL;
    (void)bytesPerPixel;
    InvalidateLineEngine();
    InvalidateTemplateEngine();
    return WaitFifo(bi, 13) &&
           RadeonWrite32(bi, RADEON_DP_GUI_MASTER_CNTL, master) &&
           RadeonWrite32(bi, RADEON_DP_WRITE_MASK, writeMask) &&
           RadeonWrite32(bi, RADEON_DP_BRUSH_FRGD_CLR,
                          HardwarePen(pattern->FgPen, format)) &&
           RadeonWrite32(bi, RADEON_DP_BRUSH_BKGD_CLR,
                          HardwarePen(pattern->BgPen, format)) &&
           RadeonWrite32(bi, RADEON_BRUSH_DATA0, data0) &&
           RadeonWrite32(bi, RADEON_BRUSH_DATA1, data1) &&
           RadeonWrite32(bi, RADEON_DP_CNTL,
                          RADEON_DST_X_LEFT_TO_RIGHT |
                              RADEON_DST_Y_TOP_TO_BOTTOM) &&
           RadeonWrite32(bi, RADEON_DSTCACHE_CTLSTAT,
                          RADEON_RB2D_DC_FLUSH_ALL) &&
           RadeonWrite32(bi, RADEON_WAIT_UNTIL,
                          RADEON_WAIT_2D_IDLECLEAN |
                              RADEON_WAIT_DMA_GUI_IDLE) &&
           RadeonWrite32(bi, RADEON_DST_PITCH_OFFSET,
                          surface->PitchOffset) &&
           RadeonWrite32(bi, RADEON_BRUSH_Y_X,
                          (phaseY << 8) | phaseX) &&
           RadeonWrite32(bi, RADEON_DST_Y_X,
                          (destinationY << 16) | destinationX) &&
           RadeonWrite32(bi, RADEON_DST_HEIGHT_WIDTH,
                          ((ULONG)(UWORD)height << 16) |
                               (UWORD)width);
}

static __inline__ ULONG TemplateFullWord(const UBYTE **sourceAddress,
                                         ULONG bitOffset)
{
    const UBYTE *source = *sourceAddress;
    ULONG bits;

    __asm__ volatile ("bfextu (%1){%2:32},%0\n\t"
                      "addq.l #4,%1"
                      : "=d" (bits), "+a" (source)
                      : "d" (bitOffset)
                      : "cc");
    *sourceAddress = source;
    return bits;
}

static __inline__ ULONG TemplatePartialWord(const UBYTE *source,
                                            ULONG bitOffset,
                                            ULONG fieldWidth)
{
    ULONG bits;

    __asm__ volatile ("bfextu (%1){%2:%3},%0"
                      : "=d" (bits)
                      : "a" (source), "d" (bitOffset), "d" (fieldWidth)
                      : "cc");
    return bits << (32UL - fieldWidth);
}

static BOOL PrepareTemplate(struct BoardInfo *bi,
                            const struct Template *template,
                            WORD width, WORD height)
{
    ULONG memoryAddress;
    ULONG memoryBase;
    ULONG rowBits;

    if (!bi || !template || !template->Memory ||
        template->BytesPerRow <= 0 || template->XOffset > 15 ||
        (template->DrawMode != JAM1 && template->DrawMode != JAM2))
        return FALSE;

    memoryAddress = (ULONG)template->Memory;
    memoryBase = (ULONG)bi->MemoryBase;
    if (memoryAddress >= memoryBase &&
        (unsigned long long)memoryAddress <
            (unsigned long long)memoryBase + bi->MemorySpaceSize)
        return FALSE;

    rowBits = (ULONG)(UWORD)template->BytesPerRow * 8UL;
    return width > 0 && height > 0 &&
           (ULONG)template->XOffset + (UWORD)width <= rowBits;
}

/*
 * Packs the template into the hidden VRAM staging buffer and returns its
 * monochrome pitch, or 0 if it does not fit.
 *
 * This replaces about 94 non-burstable HOST_DATA register writes per call
 * with the same number of framebuffer-aperture writes, measured at 0.61 us
 * against 1.33 us, plus a byte reversal each. Rows are packed contiguously at
 * a 64-byte pitch so the writes stay sequential.
 */
static ULONG StageTemplateRows(struct BoardInfo *bi,
                               const struct Template *template,
                               WORD width, WORD height, ULONG stageOffset)
{
    volatile UBYTE *base = (volatile UBYTE *)bi->MemoryBase;
    ULONG rowBytes = ((ULONG)(UWORD)width + 7UL) >> 3;
    ULONG monoPitch = (rowBytes + 63UL) & ~63UL;
    ULONG words = (rowBytes + 3UL) >> 2;
    const UBYTE *rowSource = (const UBYTE *)template->Memory +
                             (template->XOffset >> 3);
    ULONG bitOffset = template->XOffset & 7UL;
    UWORD row;

    if (!monoPitch || (monoPitch >> 6) > 0x3ffUL ||
        monoPitch > TEMPLATE_STAGE_SIZE / (ULONG)(UWORD)height)
        return 0;

    for (row = 0; row < (UWORD)height; ++row) {
        const UBYTE *source = rowSource;
        volatile ULONG *target = (volatile ULONG *)
            (base + stageOffset + (ULONG)row * monoPitch);
        ULONG left = (ULONG)(UWORD)width;
        ULONG index;

        for (index = 0; index < words; ++index) {
            /*
             * Extract exactly the bits this row still owns. A full 32-bit
             * bfextu on the final word could read past the template, which
             * is why the tail is taken at its real width.
             */
            ULONG bits = left >= 32UL ?
                TemplateFullWord(&source, bitOffset) :
                TemplatePartialWord(source, bitOffset, left);

            *target++ = ((ULONG)ReverseByte[(bits >> 24) & 0xffUL] << 24) |
                        ((ULONG)ReverseByte[(bits >> 16) & 0xffUL] << 16) |
                        ((ULONG)ReverseByte[(bits >> 8) & 0xffUL] << 8) |
                        (ULONG)ReverseByte[bits & 0xffUL];
            left = left >= 32UL ? left - 32UL : 0UL;
        }
        rowSource += (UWORD)template->BytesPerRow;
    }
    /* Aperture read drains the card's host write path before the engine reads. */
    (void)*(volatile ULONG *)(base + stageOffset);
    return monoPitch;
}

static BOOL SubmitStagedTemplate(struct BoardInfo *bi,
                                 const struct AccelSurface *surface,
                                 const struct Template *template,
                                 WORD x, WORD y, WORD width, WORD height,
                                 UBYTE mask, RGBFTYPE format,
                                 ULONG stageOffset, ULONG monoPitch)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    ULONG bytesPerPixel;
    ULONG datatype = HardwareDatatype(format, &bytesPerPixel);
    ULONG sourceType = template->DrawMode == JAM2 ?
        RADEON_GMC_SRC_DATATYPE_MONO_FG_BG :
        RADEON_GMC_SRC_DATATYPE_MONO_FG_LA;
    ULONG writeMask = format == RGBFB_CLUT ? (ULONG)mask : 0xffffffffUL;
    ULONG destinationX = (ULONG)(UWORD)(x + surface->XBias);
    ULONG destinationY = (ULONG)(UWORD)(y + surface->YBias);
    ULONG gpuAddress = data->FramebufferGpuBase + stageOffset;
    ULONG master = RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                   RADEON_GMC_SRC_PITCH_OFFSET_CNTL |
                   RADEON_GMC_DST_CLIPPING |
                   RADEON_GMC_BRUSH_NONE | datatype | sourceType |
                   RADEON_GMC_BYTE_MSB_TO_LSB | RADEON_ROP3_S |
                   RADEON_DP_SRC_SOURCE_MEMORY |
                   RADEON_GMC_CLR_CMP_CNTL_DIS;

    (void)bytesPerPixel;
    /* Both cached engine states program the same registers this does. */
    InvalidateLineEngine();
    InvalidateTemplateEngine();

    return WaitFifo(bi, 12) &&
           RadeonWrite32(bi, RADEON_DP_GUI_MASTER_CNTL, master) &&
           RadeonWrite32(bi, RADEON_DP_WRITE_MASK, writeMask) &&
           RadeonWrite32(bi, RADEON_DP_SRC_FRGD_CLR,
                         HardwarePen(template->FgPen, format)) &&
           RadeonWrite32(bi, RADEON_DP_SRC_BKGD_CLR,
                         HardwarePen(template->BgPen, format)) &&
           RadeonWrite32(bi, RADEON_DP_CNTL,
                         RADEON_DST_X_LEFT_TO_RIGHT |
                             RADEON_DST_Y_TOP_TO_BOTTOM) &&
           RadeonWrite32(bi, RADEON_SRC_PITCH_OFFSET,
                         ((monoPitch >> 6) << 22) | (gpuAddress >> 10)) &&
           RadeonWrite32(bi, RADEON_DST_PITCH_OFFSET,
                         surface->PitchOffset) &&
           RadeonWrite32(bi, RADEON_SC_TOP_LEFT,
                         (destinationY << 16) | destinationX) &&
           RadeonWrite32(bi, RADEON_SC_BOTTOM_RIGHT,
                         ((destinationY + (UWORD)height) << 16) |
                             (destinationX + (UWORD)width)) &&
           RadeonWrite32(bi, RADEON_SRC_Y_X, 0) &&
           RadeonWrite32(bi, RADEON_DST_Y_X,
                         (destinationY << 16) | destinationX) &&
           RadeonWrite32(bi, RADEON_DST_HEIGHT_WIDTH,
                         ((ULONG)(UWORD)height << 16) | (UWORD)width);
}

static BOOL SubmitTemplate(struct BoardInfo *bi,
                           const struct AccelSurface *surface,
                           const struct Template *template,
                           WORD x, WORD y, WORD width, WORD height,
                           UBYTE mask, RGBFTYPE format)
{
    ULONG bytesPerPixel;
    ULONG datatype = HardwareDatatype(format, &bytesPerPixel);
    ULONG sourceType = template->DrawMode == JAM2 ?
        RADEON_GMC_SRC_DATATYPE_MONO_FG_BG :
        RADEON_GMC_SRC_DATATYPE_MONO_FG_LA;
    ULONG writeMask = format == RGBFB_CLUT ? (ULONG)mask :
                                                   0xffffffffUL;
    ULONG fgPen = HardwarePen(template->FgPen, format);
    ULONG bgPen = HardwarePen(template->BgPen, format);
    ULONG destinationX = (ULONG)(UWORD)(x + surface->XBias);
    ULONG destinationY = (ULONG)(UWORD)(y + surface->YBias);
    ULONG paddedWidth = ((ULONG)(UWORD)width + 31UL) & ~31UL;
    ULONG master = RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                   RADEON_GMC_DST_CLIPPING |
                   RADEON_GMC_BRUSH_NONE | datatype | sourceType |
                   RADEON_GMC_BYTE_MSB_TO_LSB | RADEON_ROP3_S |
                   RADEON_DP_SRC_SOURCE_HOST_DATA |
                   RADEON_GMC_CLR_CMP_CNTL_DIS;
    ULONG words = paddedWidth >> 5;
    ULONG tailWidth = (UWORD)width & 31UL;
#ifdef DEBUG
    ULONG uploadWords = words * (UWORD)height;
#endif
    BOOL valid = TemplateEngine.Valid && TemplateEngine.Board == bi;
    BOOL masterChanged = !valid || TemplateEngine.Master != master;
    BOOL pitchChanged = !valid ||
                        TemplateEngine.PitchOffset != surface->PitchOffset;
    BOOL maskChanged = !valid || TemplateEngine.WriteMask != writeMask;
    BOOL fgChanged = !valid || TemplateEngine.FgPen != fgPen;
    BOOL bgChanged = !valid || TemplateEngine.BgPen != bgPen;
    ULONG setupWrites = 4UL + masterChanged + pitchChanged + maskChanged +
                        fgChanged + bgChanged + (!valid ? 4UL : 0UL);
    const UBYTE *rowSource = (const UBYTE *)template->Memory +
                              (template->XOffset >> 3);
    UWORD row;

    (void)bytesPerPixel;
    InvalidateLineEngine();
    if (!WaitFifo(bi, setupWrites) ||
        (!valid &&
         !RadeonWrite32(bi, RADEON_RBBM_GUICNTL,
                        RADEON_HOST_DATA_SWAP_NONE)) ||
        (masterChanged &&
         !RadeonWrite32(bi, RADEON_DP_GUI_MASTER_CNTL, master)) ||
        (maskChanged &&
         !RadeonWrite32(bi, RADEON_DP_WRITE_MASK, writeMask)) ||
        (fgChanged &&
         !RadeonWrite32(bi, RADEON_DP_SRC_FRGD_CLR, fgPen)) ||
        (bgChanged &&
         !RadeonWrite32(bi, RADEON_DP_SRC_BKGD_CLR, bgPen)) ||
        (!valid &&
         (!RadeonWrite32(bi, RADEON_DP_CNTL,
                         RADEON_DST_X_LEFT_TO_RIGHT |
                             RADEON_DST_Y_TOP_TO_BOTTOM) ||
          !RadeonWrite32(bi, RADEON_DSTCACHE_CTLSTAT,
                         RADEON_RB2D_DC_FLUSH_ALL) ||
          !RadeonWrite32(bi, RADEON_WAIT_UNTIL,
                         RADEON_WAIT_2D_IDLECLEAN |
                             RADEON_WAIT_DMA_GUI_IDLE))) ||
        (pitchChanged &&
         !RadeonWrite32(bi, RADEON_DST_PITCH_OFFSET,
                        surface->PitchOffset)) ||
        !RadeonWrite32(bi, RADEON_SC_TOP_LEFT,
                       (destinationY << 16) | destinationX) ||
        !RadeonWrite32(bi, RADEON_SC_BOTTOM_RIGHT,
                       ((destinationY + (UWORD)height) << 16) |
                           (destinationX + (UWORD)width)) ||
        !RadeonWrite32(bi, RADEON_DST_Y_X,
                       (destinationY << 16) | destinationX) ||
        !RadeonWrite32(bi, RADEON_DST_HEIGHT_WIDTH,
                       ((ULONG)(UWORD)height << 16) | paddedWidth))
        return FALSE;

    for (row = 0; row < (UWORD)height; ++row) {
        const UBYTE *source = rowSource;
        ULONG bitOffset = template->XOffset & 7UL;
        ULONG fullCount = words - (tailWidth ? 1UL : 0UL);
        ULONG index;

        /* RV280 throttles the host-data stream itself. The shipping closed
         * drivers use this same repeated HOST_DATA0 sequence without polling
         * the command FIFO between words. */
        for (index = 0; index < fullCount; ++index)
            AccelWriteHostData(bi, RADEON_HOST_DATA0,
                               TemplateFullWord(&source, bitOffset));
        if (tailWidth)
            AccelWriteHostData(bi, RADEON_HOST_DATA0,
                               TemplatePartialWord(source, bitOffset,
                                                   tailWidth));
        rowSource += (UWORD)template->BytesPerRow;
    }
    TemplateEngine.Board = bi;
    TemplateEngine.Master = master;
    TemplateEngine.PitchOffset = surface->PitchOffset;
    TemplateEngine.WriteMask = writeMask;
    TemplateEngine.FgPen = fgPen;
    TemplateEngine.BgPen = bgPen;
    TemplateEngine.Valid = TRUE;
    RDEBUG_TEMPLATE_HARDWARE(valid && !masterChanged && !pitchChanged &&
                             !maskChanged && !fgChanged && !bgChanged,
                             uploadWords);
    return TRUE;
}

static BOOL SubmitCopy(struct BoardInfo *bi,
                       const struct AccelSurface *source,
                       const struct AccelSurface *destination,
                       WORD srcX, WORD srcY, WORD dstX, WORD dstY,
                       WORD width, WORD height, UBYTE mask,
                       RGBFTYPE format, BOOL sameLayout,
                       BOOL reverseSurface, ULONG rop)
{
    ULONG bytesPerPixel;
    ULONG datatype = HardwareDatatype(format, &bytesPerPixel);
    ULONG writeMask = format == RGBFB_CLUT ? (ULONG)mask :
                                                   0xffffffffUL;
    ULONG master = RADEON_GMC_SRC_PITCH_OFFSET_CNTL |
                   RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                   RADEON_GMC_BRUSH_NONE | datatype |
                   RADEON_GMC_SRC_DATATYPE_COLOR | rop |
                   RADEON_DP_SRC_SOURCE_MEMORY |
                   RADEON_GMC_CLR_CMP_CNTL_DIS;
    ULONG direction = 0;
    (void)bytesPerPixel;
    InvalidateLineEngine();
    InvalidateTemplateEngine();
    srcX = (WORD)(srcX + source->XBias);
    dstX = (WORD)(dstX + destination->XBias);
    srcY = (WORD)(srcY + source->YBias);
    dstY = (WORD)(dstY + destination->YBias);
    if ((!sameLayout && !reverseSurface) ||
        (sameLayout && dstX <= srcX))
        direction |= RADEON_DST_X_LEFT_TO_RIGHT;
    else {
        srcX = (WORD)(srcX + width - 1);
        dstX = (WORD)(dstX + width - 1);
    }
    if ((!sameLayout && !reverseSurface) ||
        (sameLayout && dstY <= srcY))
        direction |= RADEON_DST_Y_TOP_TO_BOTTOM;
    else {
        srcY = (WORD)(srcY + height - 1);
        dstY = (WORD)(dstY + height - 1);
    }

    return WaitFifo(bi, 10) &&
           RadeonWrite32(bi, RADEON_DP_GUI_MASTER_CNTL, master) &&
           RadeonWrite32(bi, RADEON_DP_WRITE_MASK, writeMask) &&
           RadeonWrite32(bi, RADEON_DP_CNTL, direction) &&
           RadeonWrite32(bi, RADEON_DSTCACHE_CTLSTAT,
                          RADEON_RB2D_DC_FLUSH_ALL) &&
           RadeonWrite32(bi, RADEON_WAIT_UNTIL,
                          RADEON_WAIT_2D_IDLECLEAN |
                              RADEON_WAIT_DMA_GUI_IDLE) &&
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

#ifdef DEBUG
/*
 * Can the 2D engine colour-expand a monochrome source read from VRAM, rather
 * than streamed a longword at a time through HOST_DATA?
 *
 * BlitTemplate spends about 94 non-burstable register writes per call on that
 * stream and it is 75 percent of all PCI time. An aperture write measured
 * 2.19x cheaper than a register write, so staging the glyph bits in VRAM and
 * expanding from memory would cut a template blit from roughly 126 us to 72
 * us - but only if this mode works on RV280. Two earlier BlitTemplate
 * prototypes were rejected on this hardware, so the capability is proven here
 * before anything is built on it.
 *
 * The source pattern is byte-symmetric (0xAA in every byte) so that the
 * result is independent of source byte order: this answers "is the mode
 * supported", not "what is the bit ordering", which is a separate question.
 * Every wait is bounded, and both buffers sit in the tail of the board pool
 * which Picasso96 has not begun allocating from at InitCard time.
 */
static ULONG ProbeMonoExpand(struct BoardInfo *bi, ULONG byteOrder,
                             ULONG *sampleOut)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    volatile UBYTE *base = (volatile UBYTE *)bi->MemoryBase;
    ULONG probeBase;
    ULONG sourceOffset;
    ULONG destinationOffset;
    ULONG master;
    ULONG count;
    ULONG index;
    ULONG sample = 0;
    ULONG result = RADEON_PROBE_OK;

    *sampleOut = 0;
    if (!data || !base || bi->MemorySize < 65536UL ||
        data->FramebufferGpuBase > 0xffffffffUL - bi->MemorySize)
        return RADEON_PROBE_SKIPPED;

    probeBase = (bi->MemorySize - 32768UL) & ~1023UL;
    sourceOffset = probeBase;
    destinationOffset = probeBase + 1024UL;

    /*
     * Asymmetric on purpose: 0xC0 is 11000000, so the eight expanded bytes
     * distinguish MSB-first from LSB-first and from swapped fg/bg, which a
     * byte-symmetric pattern such as 0xAA cannot.
     */
    *(volatile ULONG *)(base + sourceOffset) = 0xC0000000UL;
    for (index = 0; index < 32UL; ++index)
        base[destinationOffset + index] = 0x55U;
    /* Aperture read drains the card's host write path before the engine reads. */
    (void)*(volatile ULONG *)(base + sourceOffset);

    master = RADEON_GMC_DST_PITCH_OFFSET_CNTL |
             RADEON_GMC_SRC_PITCH_OFFSET_CNTL |
             RADEON_GMC_BRUSH_NONE |
             RADEON_GMC_DST_8BPP_CI |
             RADEON_GMC_SRC_DATATYPE_MONO_FG_BG |
             byteOrder |
             RADEON_ROP3_S |
             RADEON_DP_SRC_SOURCE_MEMORY |
             RADEON_GMC_CLR_CMP_CNTL_DIS;

    if (!WaitFifo(bi, 12) ||
        !RadeonWrite32(bi, RADEON_RBBM_GUICNTL,
                       RADEON_HOST_DATA_SWAP_NONE) ||
        !RadeonWrite32(bi, RADEON_DP_GUI_MASTER_CNTL, master) ||
        !RadeonWrite32(bi, RADEON_DP_WRITE_MASK, 0xffffffffUL) ||
        !RadeonWrite32(bi, RADEON_DP_SRC_FRGD_CLR, 0xffUL) ||
        !RadeonWrite32(bi, RADEON_DP_SRC_BKGD_CLR, 0x00UL) ||
        !RadeonWrite32(bi, RADEON_DP_CNTL,
                       RADEON_DST_X_LEFT_TO_RIGHT |
                           RADEON_DST_Y_TOP_TO_BOTTOM) ||
        !RadeonWrite32(bi, RADEON_SRC_PITCH_OFFSET,
                       ((64UL >> 6) << 22) |
                           ((data->FramebufferGpuBase + sourceOffset) >> 10)) ||
        !RadeonWrite32(bi, RADEON_DST_PITCH_OFFSET,
                       ((64UL >> 6) << 22) |
                           ((data->FramebufferGpuBase +
                             destinationOffset) >> 10)) ||
        !RadeonWrite32(bi, RADEON_SRC_Y_X, 0) ||
        !RadeonWrite32(bi, RADEON_DST_Y_X, 0) ||
        !RadeonWrite32(bi, RADEON_DST_HEIGHT_WIDTH, (1UL << 16) | 32UL) ||
        !WaitIdleAndFlush(bi))
        result = RADEON_PROBE_FAILED;

    if (result == RADEON_PROBE_OK) {
        (void)RadeonWrite32(bi, RADEON_DSTCACHE_CTLSTAT,
                            RADEON_RB2D_DC_FLUSH_ALL);
        for (count = 0; count < ACCEL_TIMEOUT_POLLS; ++count) {
            if (!(RadeonRead32(bi, RADEON_DSTCACHE_CTLSTAT) &
                  RADEON_RB2D_DC_BUSY))
                break;
            if (count >= ACCEL_SPIN_POLLS)
                RadeonDelayUs(1);
        }
        for (index = 0; index < 4UL; ++index)
            sample = (sample << 8) | base[destinationOffset + index];
        for (index = 4UL; index < 8UL; ++index)
            RadeonMonoProbeSampleAlt =
                (RadeonMonoProbeSampleAlt << 8) |
                base[destinationOffset + index];
        /* Every expanded byte must be one of the two pens, whatever the
         * ordering turns out to be. */
        for (index = 0; index < 32UL; ++index) {
            UBYTE value = base[destinationOffset + index];

            if (value != 0x00U && value != 0xffU) {
                result = RADEON_PROBE_WRONG;
                break;
            }
        }
    }

    *sampleOut = sample;
    (void)RestoreEngineState(bi);
    (void)WaitIdleAndFlush(bi);
    return result;
}

static ULONG ProbeMonoFromMemory(struct BoardInfo *bi)
{
    ULONG sample = 0;
    ULONG result;

    /*
     * GMC_BYTE_PIX_ORDER was measured to make no difference to a memory
     * source: both settings produced the same expansion. Only one run is
     * needed, and the eight reported bytes identify the fixed ordering.
     */
    RadeonMonoProbeSampleAlt = 0;
    result = ProbeMonoExpand(bi, RADEON_GMC_BYTE_MSB_TO_LSB, &sample);
    RadeonMonoProbeSample = sample;
    return result;
}
#endif

BOOL RadeonInitializeAcceleration(struct BoardInfo *bi, BOOL enableCp,
                                  BOOL stageTemplates)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    ULONG location;

    if (!data || !bi->MemoryBase || !bi->MemoryIOBase)
        return FALSE;
    LineSurface.Valid = FALSE;
    InvalidateLineEngine();
    InvalidateTemplateEngine();
    data->AccelState = RADEON_ACCEL_OFF;
    data->AccelPending = RADEON_PENDING_NONE;
    data->AccelRecoveryTried = FALSE;

    location = (RadeonRead32(bi, RADEON_MC_FB_LOCATION) & 0xffffUL) << 16;
    RLOG("Radeon9200: 2D init fb=%lx mc=%lx status=%lx\n",
         data->FramebufferGpuBase, location,
         RadeonRead32(bi, RADEON_RBBM_STATUS));
    if (location != data->FramebufferGpuBase || !ResetEngine(bi) ||
        !RestoreEngineState(bi) || !WaitIdleAndFlush(bi)) {
        RLOG("Radeon9200: 2D engine initialization failed\n");
        return FALSE;
    }

    /*
     * Hide the staging buffer above the pool Picasso96 manages. InitCard runs
     * before rtg.library allocates anything, so lowering MemorySize here is
     * enough to keep every bitmap below it; ValidateSurface already clamps to
     * the same value.
     */
    data->TemplateStaging = 0;
    if (stageTemplates) {
        ULONG limit = bi->MemorySize & ~1023UL;

        if (limit > TEMPLATE_STAGE_SIZE + 1048576UL) {
            bi->MemorySize = limit - TEMPLATE_STAGE_SIZE;
            data->TemplateStaging = 1;
            RLOG("Radeon9200: template staging buffer at %lx\n",
                 bi->MemorySize);
        }
    }

    data->AccelState = RADEON_ACCEL_READY;
#ifdef DEBUG
    RadeonMonoProbeResult = ProbeMonoFromMemory(bi);
    RLOG("Radeon9200: mono-from-memory probe=%lu sample=%lx\n",
         RadeonMonoProbeResult, RadeonMonoProbeSample);
#endif
    if (!enableCp || !RadeonCpInitialize(bi))
        RLOG("Radeon9200: direct-MMIO 2D engine ready, status=%lx\n",
              RadeonRead32(bi, RADEON_RBBM_STATUS));
    else
        RLOG("Radeon9200: CP ready; Picasso96 2D uses direct MMIO\n");
    return TRUE;
}

void RadeonShutdownAcceleration(struct BoardInfo *bi)
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    if (!SysBase || !data)
        return;
    ObtainSemaphore(&bi->BoardLock);
    if (data->AccelPending)
        (void)SynchronizeEngine(bi);
    RadeonCpShutdown(bi);
    LineSurface.Valid = FALSE;
    InvalidateLineEngine();
    InvalidateTemplateEngine();
    data->AccelPending = RADEON_PENDING_NONE;
    data->AccelState = RADEON_ACCEL_OFF;
    ReleaseSemaphore(&bi->BoardLock);
}

void RadeonWaitBlitter(__REGA0(struct BoardInfo *bi))
{
    RDEBUG_SAMPLE

    if (!bi)
        return;
    RDEBUG_BEGIN();
    (void)SynchronizeEngine(bi);
    RDEBUG_END_DRAIN();
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
    RDEBUG_SAMPLE
    RDEBUG_SAMPLE_OUTER

    if (!SysBase || !data)
        return;
    result = ValidateSurface(bi, render, x, y, width, height,
                             format, &surface);
    if (result == SURFACE_REJECT)
        return;
    RDEBUG_BEGIN_OUTER();

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
        RDEBUG_BEGIN();
        if (SubmitSolidRect(bi, &surface, x, y, width, height,
                            pen, mask, format, RADEON_ROP3_P)) {
            data->AccelPending = RADEON_PENDING_MMIO;
            RDEBUG_MARK_HARDWARE();
        } else {
            ObtainSemaphore(&bi->BoardLock);
            software = RecoverEngine(bi);
            ReleaseSemaphore(&bi->BoardLock);
        }
        RDEBUG_END_FILL();
    } else {
        ObtainSemaphore(&bi->BoardLock);
        software = SynchronizeEngine(bi) &&
                   data->AccelState != RADEON_ACCEL_UNSAFE;
        ReleaseSemaphore(&bi->BoardLock);
    }

    if (software && bi->FillRectDefault &&
        bi->FillRectDefault != RadeonFillRect)
        bi->FillRectDefault(bi, render, x, y, width, height,
                            pen, mask, format);
    RDEBUG_END_CALL();
}

void RadeonInvertRect(__REGA0(struct BoardInfo *bi),
                      __REGA1(struct RenderInfo *render),
                      __REGD0(WORD x), __REGD1(WORD y),
                      __REGD2(WORD width), __REGD3(WORD height),
                      __REGD4(UBYTE mask),
                      __REGD7(RGBFTYPE format))
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct AccelSurface surface;
    enum SurfaceResult result;
    BOOL software = FALSE;
    RDEBUG_OP_SAMPLE

    if (!SysBase || !data)
        return;
    RDEBUG_OP_BEGIN();
    result = ValidateSurface(bi, render, x, y, width, height,
                             format, &surface);
    if (result == SURFACE_REJECT) {
        RDEBUG_OP_END(RADEON_DEBUG_OP_INVERT);
        return;
    }

    if (result == SURFACE_HARDWARE &&
        data->AccelState == RADEON_ACCEL_READY) {
        if (SubmitSolidRect(bi, &surface, x, y, width, height,
                            0, mask, format, RADEON_ROP3_Dn)) {
            data->AccelPending = RADEON_PENDING_MMIO;
            RDEBUG_OP_HARDWARE();
        } else {
            ObtainSemaphore(&bi->BoardLock);
            software = RecoverEngine(bi);
            ReleaseSemaphore(&bi->BoardLock);
        }
    } else {
        ObtainSemaphore(&bi->BoardLock);
        software = SynchronizeEngine(bi) &&
                   data->AccelState != RADEON_ACCEL_UNSAFE;
        ReleaseSemaphore(&bi->BoardLock);
    }

    if (software && bi->InvertRectDefault &&
        bi->InvertRectDefault != RadeonInvertRect)
        bi->InvertRectDefault(bi, render, x, y, width, height,
                              mask, format);
    RDEBUG_OP_END(RADEON_DEBUG_OP_INVERT);
}

void RadeonBlitPattern(__REGA0(struct BoardInfo *bi),
                       __REGA1(struct RenderInfo *render),
                       __REGA2(struct Pattern *pattern),
                       __REGD0(WORD x), __REGD1(WORD y),
                       __REGD2(WORD width), __REGD3(WORD height),
                       __REGD4(UBYTE mask),
                       __REGD7(RGBFTYPE format))
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct AccelSurface surface;
    enum SurfaceResult result;
    ULONG patternData0 = 0;
    ULONG patternData1 = 0;
    BOOL hardware;
    BOOL software = FALSE;
    RDEBUG_OP_SAMPLE

    if (!SysBase || !data || !pattern || !pattern->Memory)
        return;
    RDEBUG_OP_BEGIN();
    result = ValidateSurface(bi, render, x, y, width, height,
                             format, &surface);
    if (result == SURFACE_REJECT) {
        RDEBUG_OP_END(RADEON_DEBUG_OP_PATTERN);
        return;
    }
    hardware = PrepareMonoPattern(bi, pattern,
                                  &patternData0, &patternData1);

    if (hardware && result == SURFACE_HARDWARE &&
        data->AccelState == RADEON_ACCEL_READY) {
        if (SubmitPattern(bi, &surface, pattern, x, y, width, height,
                          mask, format, patternData0, patternData1)) {
            data->AccelPending = RADEON_PENDING_MMIO;
            RDEBUG_OP_HARDWARE();
#ifdef DEBUG
            RLOG("Radeon9200: HW BlitPattern mem=%lx pitchoff=%lx "
                 "xy=%ld,%ld size=%ldx%ld fmt=%ld mask=%lx "
                 "offset=%ld,%ld patsize=%ld data=%lx,%lx\n",
                 (ULONG)render->Memory, surface.PitchOffset,
                 (ULONG)(UWORD)x, (ULONG)(UWORD)y,
                 (ULONG)(UWORD)width, (ULONG)(UWORD)height,
                 (ULONG)format, (ULONG)mask,
                 (ULONG)pattern->XOffset, (ULONG)pattern->YOffset,
                 (ULONG)pattern->Size, patternData0, patternData1);
#endif
        } else {
            ObtainSemaphore(&bi->BoardLock);
            software = RecoverEngine(bi);
            ReleaseSemaphore(&bi->BoardLock);
        }
    } else {
        ObtainSemaphore(&bi->BoardLock);
        software = SynchronizeEngine(bi) &&
                   data->AccelState != RADEON_ACCEL_UNSAFE;
        ReleaseSemaphore(&bi->BoardLock);
    }

    if (software && bi->BlitPatternDefault &&
        bi->BlitPatternDefault != RadeonBlitPattern)
        bi->BlitPatternDefault(bi, render, pattern, x, y,
                               width, height, mask, format);
    RDEBUG_OP_END(RADEON_DEBUG_OP_PATTERN);
}

void RadeonBlitTemplate(__REGA0(struct BoardInfo *bi),
                        __REGA1(struct RenderInfo *render),
                        __REGA2(struct Template *template),
                        __REGD0(WORD x), __REGD1(WORD y),
                        __REGD2(WORD width), __REGD3(WORD height),
                        __REGD4(UBYTE mask),
                        __REGD7(RGBFTYPE format))
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct AccelSurface surface;
    enum SurfaceResult result;
    BOOL hardware;
    BOOL software = FALSE;
    RDEBUG_OP_SAMPLE

    if (!SysBase || !data)
        return;
    RDEBUG_OP_BEGIN();
    RDEBUG_TEMPLATE_CALL((UWORD)width, (UWORD)height,
                         template ? template->DrawMode : 0xffU);
    result = ValidateSurface(bi, render, x, y, width, height,
                             format, &surface);
    if (result == SURFACE_REJECT) {
        RDEBUG_OP_END(RADEON_DEBUG_OP_TEMPLATE);
        return;
    }
    hardware = PrepareTemplate(bi, template, width, height);
    if (format == RGBFB_CLUT && mask != 0xffU)
        hardware = FALSE;
    if (hardware && result == SURFACE_HARDWARE) {
        ULONG destinationX = (ULONG)(UWORD)(x + surface.XBias);
        ULONG paddedWidth = ((ULONG)(UWORD)width + 31UL) & ~31UL;

        hardware = destinationX + paddedWidth <= ACCEL_MAX_COORD + 1UL;
    }

    if (hardware && result == SURFACE_HARDWARE &&
        data->AccelState == RADEON_ACCEL_READY) {
        BOOL submitted = FALSE;

        if (data->TemplateStaging) {
            ULONG monoPitch = StageTemplateRows(bi, template, width, height,
                                                bi->MemorySize);

            if (monoPitch)
                submitted = SubmitStagedTemplate(bi, &surface, template,
                                                 x, y, width, height, mask,
                                                 format, bi->MemorySize,
                                                 monoPitch);
        }
        if (!submitted)
            submitted = SubmitTemplate(bi, &surface, template, x, y,
                                       width, height, mask, format);
        if (submitted) {
            data->AccelPending = RADEON_PENDING_MMIO;
            RDEBUG_OP_HARDWARE();
        } else {
            ObtainSemaphore(&bi->BoardLock);
            software = RecoverEngine(bi);
            ReleaseSemaphore(&bi->BoardLock);
        }
    } else {
        ObtainSemaphore(&bi->BoardLock);
        software = SynchronizeEngine(bi) &&
                   data->AccelState != RADEON_ACCEL_UNSAFE;
        ReleaseSemaphore(&bi->BoardLock);
    }

    if (software)
        RDEBUG_TEMPLATE_SOFTWARE();
    if (software && bi->BlitTemplateDefault &&
        bi->BlitTemplateDefault != RadeonBlitTemplate)
        bi->BlitTemplateDefault(bi, render, template, x, y,
                                width, height, mask, format);
    RDEBUG_OP_END(RADEON_DEBUG_OP_TEMPLATE);
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
    RDEBUG_OP_SAMPLE

    if (!SysBase || !data)
        return;
    RDEBUG_OP_BEGIN();
    srcResult = ValidateSurface(bi, render, srcX, srcY, width, height,
                                format, &source);
    dstResult = ValidateSurface(bi, render, dstX, dstY, width, height,
                                format, &destination);
    if (srcResult == SURFACE_REJECT || dstResult == SURFACE_REJECT) {
        RDEBUG_OP_END(RADEON_DEBUG_OP_COPY);
        return;
    }

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
                       width, height, mask, format, TRUE,
                       FALSE, RADEON_ROP3_S)) {
            data->AccelPending = RADEON_PENDING_MMIO;
            RDEBUG_OP_HARDWARE();
        } else {
            ObtainSemaphore(&bi->BoardLock);
            software = RecoverEngine(bi);
            ReleaseSemaphore(&bi->BoardLock);
        }
    } else {
        ObtainSemaphore(&bi->BoardLock);
        software = SynchronizeEngine(bi) &&
                   data->AccelState != RADEON_ACCEL_UNSAFE;
        ReleaseSemaphore(&bi->BoardLock);
    }

    if (software && bi->BlitRectDefault &&
        bi->BlitRectDefault != RadeonBlitRect)
        bi->BlitRectDefault(bi, render, srcX, srcY, dstX, dstY,
                            width, height, mask, format);
    RDEBUG_OP_END(RADEON_DEBUG_OP_COPY);
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
    BOOL copySafe;
    BOOL software = FALSE;
#ifdef DEBUG
    ULONG debugFlags = 0;
    ULONG phaseStart;
#endif
    RDEBUG_OP_SAMPLE

    if (!SysBase || !data)
        return;
    RDEBUG_OP_BEGIN();
#ifdef DEBUG
    phaseStart = RDEBUG_PHASE_BEGIN();
#endif
    srcResult = ValidateSurface(bi, sourceRender, srcX, srcY,
                                width, height, format, &source);
    dstResult = ValidateSurface(bi, destinationRender, dstX, dstY,
                                 width, height, format, &destination);
    RDEBUG_COMPLETE_PHASE(RADEON_DEBUG_COMPLETE_VALIDATE, phaseStart);
    if (srcResult == SURFACE_REJECT || dstResult == SURFACE_REJECT) {
        RDEBUG_COMPLETE_CALL(RDEBUG_COMPLETE_SURFACE_REJECT, opcode);
        RDEBUG_OP_END(RADEON_DEBUG_OP_COMPLETE);
        return;
    }

    disjoint = srcResult == SURFACE_HARDWARE &&
               dstResult == SURFACE_HARDWARE &&
               (source.EndOffset <= destination.StartOffset ||
                destination.EndOffset <= source.StartOffset);
    copySafe = srcResult == SURFACE_HARDWARE &&
               dstResult == SURFACE_HARDWARE &&
               (disjoint || sourceRender->BytesPerRow ==
                                destinationRender->BytesPerRow);
#ifdef DEBUG
    if (opcode != 0x06U && opcode != 0x0cU)
        debugFlags |= RDEBUG_COMPLETE_OPCODE_REJECT;
    if (srcResult != SURFACE_HARDWARE || dstResult != SURFACE_HARDWARE)
        debugFlags |= RDEBUG_COMPLETE_SURFACE_SOFTWARE;
    else {
        if (!copySafe)
            debugFlags |= RDEBUG_COMPLETE_OVERLAP_REJECT;
        if (sourceRender->BytesPerRow != destinationRender->BytesPerRow)
            debugFlags |= RDEBUG_COMPLETE_UNEQUAL_PITCH;
    }
    if (data->AccelState != RADEON_ACCEL_READY)
        debugFlags |= RDEBUG_COMPLETE_ACCEL_UNAVAILABLE;
    RDEBUG_COMPLETE_CALL(debugFlags, opcode);
#endif
    if ((opcode == 0x06U || opcode == 0x0cU) &&
        copySafe && sourceRender &&
        destinationRender &&
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
#ifdef DEBUG
        phaseStart = RDEBUG_PHASE_BEGIN();
#endif
        BOOL submitted = SubmitCopy(bi, &source, &destination,
                                    srcX, srcY, dstX, dstY,
                                    width, height, 0xffU, format,
                                    source.PitchOffset ==
                                        destination.PitchOffset,
                                    !disjoint &&
                                        destination.StartOffset >
                                            source.StartOffset,
                                    opcode == 0x06U ? RADEON_ROP3_S_XOR_D :
                                                     RADEON_ROP3_S);

        RDEBUG_COMPLETE_PHASE(RADEON_DEBUG_COMPLETE_SUBMIT, phaseStart);

        RDEBUG_COMPLETE_SUBMIT(submitted);
        if (submitted) {
            data->AccelPending = RADEON_PENDING_MMIO;
            RDEBUG_COMPLETE_HARDWARE();
            RDEBUG_OP_HARDWARE();
        } else {
            ObtainSemaphore(&bi->BoardLock);
            software = RecoverEngine(bi);
            ReleaseSemaphore(&bi->BoardLock);
        }
    } else {
        ObtainSemaphore(&bi->BoardLock);
        software = SynchronizeEngine(bi) &&
                   data->AccelState != RADEON_ACCEL_UNSAFE;
        ReleaseSemaphore(&bi->BoardLock);
    }

    if (software && bi->BlitRectNoMaskCompleteDefault &&
        bi->BlitRectNoMaskCompleteDefault !=
            RadeonBlitRectNoMaskComplete) {
#ifdef DEBUG
        phaseStart = RDEBUG_PHASE_BEGIN();
#endif
        RDEBUG_COMPLETE_SOFTWARE();
        bi->BlitRectNoMaskCompleteDefault(
            bi, sourceRender, destinationRender,
            srcX, srcY, dstX, dstY, width, height, opcode, format);
        RDEBUG_COMPLETE_PHASE(RADEON_DEBUG_COMPLETE_DEFAULT, phaseStart);
    }
    RDEBUG_OP_END(RADEON_DEBUG_OP_COMPLETE);
}

static BOOL SubmitLine(struct BoardInfo *bi,
                       const struct LineSurfaceCache *surface,
                       ULONG start, ULONG end, ULONG pen,
                       ULONG writeMask, ULONG master)
{
    BOOL valid = LineEngine.Valid && LineEngine.Board == bi;
    BOOL masterChanged = !valid || LineEngine.Master != master;
    BOOL pitchOffsetChanged = !valid ||
                              LineEngine.PitchOffset !=
                                  surface->PitchOffset;
    BOOL maskChanged = !valid || LineEngine.WriteMask != writeMask;
    BOOL penChanged = !valid || LineEngine.Pen != pen;
    ULONG entries = 2UL + masterChanged + pitchOffsetChanged +
                    maskChanged + penChanged + (!valid ? 3UL : 0UL);

    InvalidateTemplateEngine();

    if (!WaitFifo(bi, entries) ||
        (masterChanged &&
         !RadeonWrite32(bi, RADEON_DP_GUI_MASTER_CNTL, master)) ||
        (pitchOffsetChanged &&
         !RadeonWrite32(bi, RADEON_DST_PITCH_OFFSET,
                        surface->PitchOffset)) ||
        (maskChanged &&
         !RadeonWrite32(bi, RADEON_DP_WRITE_MASK, writeMask)) ||
        (penChanged &&
         !RadeonWrite32(bi, RADEON_DP_BRUSH_FRGD_CLR, pen)) ||
        (!valid &&
         (!RadeonWrite32(bi, RADEON_DP_CNTL,
                         RADEON_DST_X_LEFT_TO_RIGHT |
                             RADEON_DST_Y_TOP_TO_BOTTOM) ||
          !RadeonWrite32(bi, RADEON_SC_TOP_LEFT, 0) ||
          !RadeonWrite32(bi, RADEON_SC_BOTTOM_RIGHT,
                         RADEON_SCISSOR_MAX))) ||
        !RadeonWrite32(bi, RADEON_DST_LINE_START, start) ||
        !RadeonWrite32(bi, RADEON_DST_LINE_END, end))
        return FALSE;

    LineEngine.Board = bi;
    LineEngine.Master = master;
    LineEngine.PitchOffset = surface->PitchOffset;
    LineEngine.WriteMask = writeMask;
    LineEngine.Pen = pen;
    LineEngine.Valid = TRUE;
    return TRUE;
}

static BOOL SubmitSolidAxisRect(struct BoardInfo *bi,
                                const struct LineSurfaceCache *surface,
                                WORD x, WORD y, WORD width, WORD height,
                                ULONG pen, ULONG writeMask)
{
    ULONG master = RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                   RADEON_GMC_BRUSH_SOLID_COLOR | surface->Datatype |
                   RADEON_GMC_SRC_DATATYPE_COLOR | RADEON_ROP3_P |
                   RADEON_GMC_CLR_CMP_CNTL_DIS;
    BOOL valid = LineEngine.Valid && LineEngine.Board == bi;

    InvalidateTemplateEngine();

    x = (WORD)(x + surface->XBias);
    y = (WORD)(y + surface->YBias);
    if ((!valid || LineEngine.Master != master) &&
        !RadeonWrite32(bi, RADEON_DP_GUI_MASTER_CNTL, master))
        return FALSE;
    if ((!valid || LineEngine.PitchOffset != surface->PitchOffset) &&
        !RadeonWrite32(bi, RADEON_DST_PITCH_OFFSET,
                       surface->PitchOffset))
        return FALSE;
    if ((!valid || LineEngine.WriteMask != writeMask) &&
        !RadeonWrite32(bi, RADEON_DP_WRITE_MASK, writeMask))
        return FALSE;
    if ((!valid || LineEngine.Pen != pen) &&
        !RadeonWrite32(bi, RADEON_DP_BRUSH_FRGD_CLR, pen))
        return FALSE;
    if (!valid &&
        (!RadeonWrite32(bi, RADEON_DP_CNTL,
                        RADEON_DST_X_LEFT_TO_RIGHT |
                            RADEON_DST_Y_TOP_TO_BOTTOM) ||
         !RadeonWrite32(bi, RADEON_SC_TOP_LEFT, 0) ||
         !RadeonWrite32(bi, RADEON_SC_BOTTOM_RIGHT,
                        RADEON_SCISSOR_MAX)))
        return FALSE;
    if (!RadeonWrite32(bi, RADEON_DST_Y_X,
                       ((ULONG)(UWORD)y << 16) | (UWORD)x) ||
        !RadeonWrite32(bi, RADEON_DST_WIDTH_HEIGHT,
                       ((ULONG)(UWORD)width << 16) | (UWORD)height))
        return FALSE;

    LineEngine.Board = bi;
    LineEngine.Master = master;
    LineEngine.PitchOffset = surface->PitchOffset;
    LineEngine.WriteMask = writeMask;
    LineEngine.Pen = pen;
    LineEngine.Valid = TRUE;
    return TRUE;
}

void RadeonDrawLine(__REGA0(struct BoardInfo *bi),
                    __REGA1(struct RenderInfo *render),
                    __REGA2(struct Line *line),
                    __REGD0(UBYTE mask),
                    __REGD7(RGBFTYPE format))
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct LineSurfaceCache *surface;
    enum SurfaceResult result;
    LONG startX, startY, endX, endY;
    LONG left, top, right, bottom;
    ULONG writeMask;
    ULONG master;
    ULONG pen;
    BOOL solidAxis;
    BOOL software = FALSE;
    RDEBUG_OP_SAMPLE

    if (!SysBase || !data || !line)
        return;
    RDEBUG_OP_BEGIN();
    if (line->DrawMode & 0x02)
        goto fallback;

    startX = line->X;
    startY = line->Y;
    endX = startX + line->dX;
    endY = startY + line->dY;
    left = startX < endX ? startX : endX;
    right = startX > endX ? startX : endX;
    top = startY < endY ? startY : endY;
    bottom = startY > endY ? startY : endY;

    result = ValidateLineSurface(bi, render, left, top, right, bottom,
                                 format, &surface);
    if (result == SURFACE_REJECT) {
        RDEBUG_OP_END(RADEON_DEBUG_OP_LINE);
        return;
    }

    if (result == SURFACE_HARDWARE &&
        data->AccelState == RADEON_ACCEL_READY) {
        solidAxis = (line->dX == 0 || line->dY == 0) &&
                    line->LinePtrn == 0xffffU &&
                    !(line->DrawMode & 0x04);
        if (solidAxis) {
            writeMask = format == RGBFB_CLUT ? (ULONG)mask :
                                                  0xffffffffUL;
            pen = HardwarePen(line->FgPen, format);
            if (SubmitSolidAxisRect(bi, surface,
                                    (WORD)left, (WORD)top,
                                    (WORD)(right - left + 1),
                                    (WORD)(bottom - top + 1),
                                    pen, writeMask)) {
                data->AccelPending = RADEON_PENDING_MMIO;
                RDEBUG_OP_HARDWARE();
                goto complete;
            }
            ObtainSemaphore(&bi->BoardLock);
            software = RecoverEngine(bi);
            ReleaseSemaphore(&bi->BoardLock);
            goto complete;
        }

        writeMask = format == RGBFB_CLUT ? (ULONG)mask : 0xffffffffUL;
        pen = HardwarePen(line->FgPen, format);
        master = RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                 RADEON_GMC_BRUSH_SOLID_COLOR | surface->Datatype |
                 RADEON_GMC_SRC_DATATYPE_COLOR |
                 ((line->DrawMode & 0x01) ? RADEON_ROP3_P :
                                            0x007b0000UL) |
                 RADEON_GMC_CLR_CMP_CNTL_DIS;

#ifdef DEBUG
        if (!LineLogged) {
            RLOG("Radeon9200: HW DrawLine mem=%lx pitchoff=%lx "
                 "start=%ld,%ld end=%ld,%ld fmt=%ld mask=%lx mode=%lx\n",
                 (ULONG)render->Memory, surface->PitchOffset,
                 (ULONG)startX, (ULONG)startY,
                 (ULONG)endX, (ULONG)endY, (ULONG)format,
                 (ULONG)mask, (ULONG)line->DrawMode);
            LineLogged = TRUE;
        }
#endif
        if (SubmitLine(bi, surface,
                       ((ULONG)(startY + surface->YBias) << 16) |
                           (UWORD)(startX + surface->XBias),
                       ((ULONG)(endY + surface->YBias) << 16) |
                           (UWORD)(endX + surface->XBias),
                       pen, writeMask, master)) {
            data->AccelPending = RADEON_PENDING_MMIO;
            RDEBUG_OP_HARDWARE();
        } else {
            ObtainSemaphore(&bi->BoardLock);
            software = RecoverEngine(bi);
            ReleaseSemaphore(&bi->BoardLock);
        }
    } else {
fallback:
        ObtainSemaphore(&bi->BoardLock);
        software = SynchronizeEngine(bi) &&
                   data->AccelState != RADEON_ACCEL_UNSAFE;
        ReleaseSemaphore(&bi->BoardLock);
    }

complete:
    if (software && bi->DrawLineDefault &&
        bi->DrawLineDefault != RadeonDrawLine)
        bi->DrawLineDefault(bi, render, line, mask, format);
    RDEBUG_OP_END(RADEON_DEBUG_OP_LINE);
}
