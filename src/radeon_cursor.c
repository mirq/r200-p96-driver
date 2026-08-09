/*
 * RV280 legacy CRTC hardware cursor.
 *
 * Register sequencing follows the MIT-licensed Xorg Radeon driver and Linux
 * DRM legacy-cursor path. The cursor image is a 64x64 ARGB surface in private
 * VRAM; Picasso96 supplies classic two-plane sprite data, converted here to
 * transparent pixels plus its three configured pointer colours.
 */
#include <exec/memory.h>
#include <hardware/byteswap.h>
#include <proto/exec.h>
#include <proto/openpci.h>

#include "radeon9200.h"
#include "radeon_debug.h"
#include "radeon_regs.h"

#define CURSOR_WIDTH  64U
#define CURSOR_HEIGHT 64U
#define CURSOR_STRIDE (CURSOR_WIDTH * sizeof(ULONG))
#define CURSOR_SIZE   (CURSOR_STRIDE * CURSOR_HEIGHT)
#define SOURCE_WIDTH  16U

struct RadeonCursorState {
    APTR Memory;
    ULONG Offset;
    ULONG Colors[3];
    UBYTE Visible;
    UBYTE UploadStorage[CURSOR_STRIDE + 7U];
};

static struct RadeonCursorState *GetCursor(struct BoardInfo *bi)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    return data ? data->CursorState : NULL;
}

static void UploadCursor(struct BoardInfo *bi)
{
    struct RadeonCursorState *cursor = GetCursor(bi);
    const UWORD *source = (const UWORD *)bi->MouseImage;
    ULONG *row;
    UWORD displayHeight = bi->MouseHeight;
    UWORD displayWidth = bi->MouseWidth;
    UWORD sourceHeight;
    UWORD sourceWidth;
    BOOL zoom;
    UWORD y;

    if (!cursor || !cursor->Memory)
        return;
    row = (ULONG *)(((ULONG)cursor->UploadStorage + 7UL) & ~7UL);
    if (displayHeight > CURSOR_HEIGHT)
        displayHeight = CURSOR_HEIGHT;
    if (displayWidth > CURSOR_WIDTH)
        displayWidth = CURSOR_WIDTH;
    zoom = (bi->Flags & BIF_BIGSPRITE) != 0 ||
           displayWidth > SOURCE_WIDTH;
    sourceHeight = zoom ? (displayHeight + 1U) / 2U : displayHeight;
    sourceWidth = zoom ? (displayWidth + 1U) / 2U : displayWidth;
    if (sourceWidth > SOURCE_WIDTH)
        sourceWidth = SOURCE_WIDTH;

    for (y = 0; y < CURSOR_HEIGHT; ++y) {
        UWORD plane0 = 0;
        UWORD plane1 = 0;
        UWORD sourceY = zoom ? y / 2U : y;
        UWORD x;

        if (source && y < displayHeight && sourceY < sourceHeight) {
            plane0 = source[2U + sourceY * 2U];
            plane1 = source[3U + sourceY * 2U];
        }
        for (x = 0; x < CURSOR_WIDTH; ++x) {
            ULONG pixel = 0;

            if (x < displayWidth) {
                UWORD sourceX = zoom ? x / 2U : x;
                UWORD bit = sourceX < sourceWidth ?
                                (UWORD)(0x8000U >> sourceX) : 0;
                UWORD value = (plane0 & bit ? 1U : 0U) |
                              (plane1 & bit ? 2U : 0U);

                if (value)
                    pixel = cursor->Colors[value - 1U];
            }
            row[x] = SWAPLONG(pixel);
        }
        host_to_pcicpy(row,
                       (UBYTE *)cursor->Memory + (ULONG)y * CURSOR_STRIDE,
                       CURSOR_STRIDE);
    }

    /* Flush aperture writes before the display engine reads the image. */
    (void)pci_inl((ULONG)cursor->Memory + CURSOR_SIZE - sizeof(ULONG));
}

static void UpdateCursorPosition(struct BoardInfo *bi)
{
    struct RadeonCursorState *cursor = GetCursor(bi);
    LONG x = (LONG)bi->MouseX - bi->XOffset;
    LONG y = (LONG)bi->MouseY - bi->YOffset;
    ULONG xOrigin = 0;
    ULONG yOrigin = 0;

    if (!cursor || !cursor->Memory)
        return;
    if (x < 0) {
        xOrigin = (ULONG)-x;
        if (xOrigin >= CURSOR_WIDTH)
            xOrigin = CURSOR_WIDTH - 1U;
        x = 0;
    }
    if (y < 0) {
        yOrigin = (ULONG)-y;
        if (yOrigin >= CURSOR_HEIGHT)
            yOrigin = CURSOR_HEIGHT - 1U;
        y = 0;
    }

    (void)RadeonWrite32(bi, RADEON_CUR_HORZ_VERT_OFF,
                        RADEON_CUR_LOCK | (xOrigin << 16) | yOrigin);
    (void)RadeonWrite32(bi, RADEON_CUR_HORZ_VERT_POSN,
                        RADEON_CUR_LOCK | ((ULONG)x << 16) | (ULONG)y);
    (void)RadeonWrite32(bi, RADEON_CUR_OFFSET,
                        cursor->Offset + yOrigin * CURSOR_STRIDE);
}

BOOL RadeonInitializeCursor(struct BoardInfo *bi)
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct RadeonCursorState *cursor;
    ULONG memoryBase;
    ULONG cursorAddress;

    if (!SysBase || !data || data->CursorState)
        return FALSE;
    cursor = AllocMem(sizeof(*cursor), MEMF_PUBLIC | MEMF_CLEAR);
    if (!cursor)
        return FALSE;
    cursor->Memory = RadeonAllocateDmaMemory(bi, CURSOR_SIZE);
    if (!cursor->Memory) {
        FreeMem(cursor, sizeof(*cursor));
        return FALSE;
    }

    memoryBase = (ULONG)bi->MemoryBase;
    cursorAddress = (ULONG)cursor->Memory;
    if (bi->MemorySpaceSize < CURSOR_SIZE || cursorAddress < memoryBase ||
        cursorAddress - memoryBase > bi->MemorySpaceSize - CURSOR_SIZE) {
        (void)RadeonFreeDmaMemory(bi, cursor->Memory, CURSOR_SIZE);
        FreeMem(cursor, sizeof(*cursor));
        return FALSE;
    }

    cursor->Offset = cursorAddress - memoryBase;
    cursor->Colors[0] = 0xff000000UL;
    cursor->Colors[1] = 0xffffffffUL;
    cursor->Colors[2] = 0xff808080UL;
    cursor->Visible = FALSE;
    data->CursorState = cursor;
    UploadCursor(bi);
    UpdateCursorPosition(bi);
    (void)RadeonMask32(bi, RADEON_CRTC_GEN_CNTL,
                       RADEON_CRTC_CUR_EN | RADEON_CRTC_CUR_MODE_MASK, 0);
    return TRUE;
}

void RadeonShutdownCursor(struct BoardInfo *bi)
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct RadeonCursorState *cursor = GetCursor(bi);

    if (!SysBase || !data || !cursor)
        return;
    (void)RadeonMask32(bi, RADEON_CRTC_GEN_CNTL,
                       RADEON_CRTC_CUR_EN | RADEON_CRTC_CUR_MODE_MASK, 0);
    (void)RadeonFreeDmaMemory(bi, cursor->Memory, CURSOR_SIZE);
    data->CursorState = NULL;
    FreeMem(cursor, sizeof(*cursor));
}

BOOL RadeonSetSprite(__REGA0(struct BoardInfo *bi),
                     __REGD0(BOOL active),
                     __REGD7(RGBFTYPE format))
{
    struct RadeonCursorState *cursor = GetCursor(bi);

    (void)format;
    RDEBUG_SPRITE_CALL(0);
    if (!cursor || !cursor->Memory)
        return FALSE;

    cursor->Visible = active;
    if (active) {
        UpdateCursorPosition(bi);
        return RadeonMask32(bi, RADEON_CRTC_GEN_CNTL,
                            RADEON_CRTC_CUR_EN |
                                RADEON_CRTC_CUR_MODE_MASK,
                            RADEON_CRTC_CUR_EN |
                                RADEON_CRTC_CUR_MODE_24BPP);
    }
    return RadeonMask32(bi, RADEON_CRTC_GEN_CNTL,
                        RADEON_CRTC_CUR_EN, 0);
}

void RadeonSetSpritePosition(__REGA0(struct BoardInfo *bi),
                             __REGD0(WORD x), __REGD1(WORD y),
                             __REGD7(RGBFTYPE format))
{
    struct RadeonCursorState *cursor = GetCursor(bi);

    (void)x;
    (void)y;
    (void)format;
    RDEBUG_SPRITE_CALL(1);
    if (cursor)
        UpdateCursorPosition(bi);
}

void RadeonSetSpriteImage(__REGA0(struct BoardInfo *bi),
                          __REGD7(RGBFTYPE format))
{
    struct RadeonCursorState *cursor = GetCursor(bi);

    (void)format;
    RDEBUG_SPRITE_CALL(2);
    if (cursor) {
        UploadCursor(bi);
        UpdateCursorPosition(bi);
    }
}

void RadeonSetSpriteColor(__REGA0(struct BoardInfo *bi),
                          __REGD0(UBYTE index),
                          __REGD1(UBYTE red),
                          __REGD2(UBYTE green),
                          __REGD3(UBYTE blue),
                          __REGD7(RGBFTYPE format))
{
    struct RadeonCursorState *cursor = GetCursor(bi);

    (void)format;
    RDEBUG_SPRITE_CALL(3);
    if (!cursor || index >= 3U)
        return;
    cursor->Colors[index] = 0xff000000UL | ((ULONG)red << 16) |
                            ((ULONG)green << 8) | blue;
    UploadCursor(bi);
}
