/*
 * CRTC, PLL, and palette sequences are derived from NetBSD's radeonfb.
 *
 * Copyright (c) 2006 Itronix Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of Itronix Inc. may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ITRONIX INC. "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL ITRONIX INC. BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE.
 */

#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/openpci.h>

#include "radeon9200.h"
#include "radeon_regs.h"

#define STARTUP_WIDTH  640U
#define STARTUP_HEIGHT 480U
#define STARTUP_PITCH  640U

#define RADEON_SCANOUT_FORMATS \
    (RGBFF_CLUT | RGBFF_R5G6B5PC | RGBFF_B8G8R8A8)

static BOOL RadeonSetSwitch(__REGA0(struct BoardInfo *bi),
                            __REGD0(BOOL state));
static void RadeonSetColorArray(__REGA0(struct BoardInfo *bi),
                                __REGD0(UWORD start),
                                __REGD1(UWORD count));
static void RadeonSetDAC(__REGA0(struct BoardInfo *bi),
                         __REGD0(UWORD mode),
                         __REGD7(RGBFTYPE format));
static void RadeonSetGC(__REGA0(struct BoardInfo *bi),
                        __REGA1(struct ModeInfo *mode),
                        __REGD0(BOOL border));
static void RadeonSetPanning(__REGA0(struct BoardInfo *bi),
                             __REGA1(UBYTE *memory),
                             __REGD0(UWORD width),
                             __REGD3(UWORD height),
                             __REGD1(WORD xOffset),
                             __REGD2(WORD yOffset),
                             __REGD7(RGBFTYPE format));
static UWORD RadeonCalculateBytesPerRow(__REGA0(struct BoardInfo *bi),
                                        __REGD0(UWORD width),
                                        __REGD1(UWORD height),
                                        __REGA1(struct ModeInfo *mode),
                                        __REGD7(RGBFTYPE format));
static APTR RadeonCalculateMemory(__REGA0(struct BoardInfo *bi),
                                  __REGA1(APTR memory),
                                  __REGD0(struct RenderInfo *render),
                                  __REGD7(RGBFTYPE format));
static ULONG RadeonGetCompatibleFormats(__REGA0(struct BoardInfo *bi),
                                         __REGD7(RGBFTYPE format));
static BOOL RadeonSetDisplay(__REGA0(struct BoardInfo *bi),
                             __REGD0(BOOL state));
static LONG RadeonResolvePixelClock(__REGA0(struct BoardInfo *bi),
                                    __REGA1(struct ModeInfo *mode),
                                    __REGD0(ULONG clock),
                                    __REGD7(RGBFTYPE format));
static ULONG RadeonGetPixelClock(__REGA0(struct BoardInfo *bi),
                                  __REGA1(struct ModeInfo *mode),
                                  __REGD0(ULONG index),
                                  __REGD7(RGBFTYPE format));
static void RadeonSetClock(__REGA0(struct BoardInfo *bi));
static void RadeonSetMemoryMode(__REGA0(struct BoardInfo *bi),
                                 __REGD7(RGBFTYPE format));
static void RadeonSetWriteMask(__REGA0(struct BoardInfo *bi),
                               __REGD0(UBYTE mask));
static void RadeonSetClearMask(__REGA0(struct BoardInfo *bi),
                               __REGD0(UBYTE mask));
static void RadeonSetReadPlane(__REGA0(struct BoardInfo *bi),
                               __REGD0(UBYTE plane));
static void RadeonWaitVerticalSync(__REGA0(struct BoardInfo *bi),
                                    __REGD0(BOOL end));
static BOOL RadeonGetVSyncState(__REGA0(struct BoardInfo *bi),
                                __REGD0(BOOL expected));
static ULONG RadeonGetVBeamPos(__REGA0(struct BoardInfo *bi));
static void RadeonSetDPMSLevel(__REGA0(struct BoardInfo *bi),
                                __REGD0(ULONG level));

static BOOL GetFormatInfo(RGBFTYPE format, ULONG *bytesPerPixel,
                          ULONG *pixelWidth)
{
    ULONG bytes;
    ULONG width;

    switch (format) {
    case RGBFB_CLUT:
        bytes = 1;
        width = RADEON_CRTC_PIX_WIDTH_8BPP;
        break;
    case RGBFB_R5G6B5PC:
        bytes = 2;
        width = RADEON_CRTC_PIX_WIDTH_16BPP;
        break;
    case RGBFB_B8G8R8A8:
        bytes = 4;
        width = RADEON_CRTC_PIX_WIDTH_32BPP;
        break;
    default:
        return FALSE;
    }

    if (bytesPerPixel)
        *bytesPerPixel = bytes;
    if (pixelWidth)
        *pixelWidth = width;
    return TRUE;
}

static BOOL GetDepthPixelWidth(UBYTE depth, ULONG *pixelWidth)
{
    ULONG width;

    switch (depth) {
    case 8:
        width = RADEON_CRTC_PIX_WIDTH_8BPP;
        break;
    case 15:
    case 16:
        width = RADEON_CRTC_PIX_WIDTH_16BPP;
        break;
    case 32:
        width = RADEON_CRTC_PIX_WIDTH_32BPP;
        break;
    default:
        return FALSE;
    }

    if (pixelWidth)
        *pixelWidth = width;
    return TRUE;
}

static BOOL FormatMatchesDepth(RGBFTYPE format, UBYTE depth)
{
    switch (format) {
    case RGBFB_CLUT:
        return depth == 8;
    case RGBFB_R5G6B5PC:
        return depth == 15 || depth == 16;
    case RGBFB_B8G8R8A8:
        return depth == 32;
    default:
        return FALSE;
    }
}

static BOOL CalculatePitch(UWORD width, RGBFTYPE format,
                           ULONG *bytesPerRow, ULONG *crtcPitch)
{
    ULONG bytesPerPixel;
    ULONG pitch;
    ULONG pixels;

    if (!width || width > 4096 ||
        !GetFormatInfo(format, &bytesPerPixel, NULL))
        return FALSE;

    pitch = ((ULONG)width * bytesPerPixel + 63UL) & ~63UL;
    pixels = pitch / bytesPerPixel;
    if (!pitch || pitch > 65535UL || (pixels & 7UL))
        return FALSE;

    if (bytesPerRow)
        *bytesPerRow = pitch;
    if (crtcPitch)
        *crtcPitch = pixels / 8UL;
    return TRUE;
}

static const struct ModeInfo StartupModeTemplate = {
    .Width = STARTUP_WIDTH,
    .Height = STARTUP_HEIGHT,
    .Depth = 8,
    .Flags = GMF_HPOLARITY | GMF_VPOLARITY,
    .HorTotal = 800,
    .HorBlankSize = 0,
    .HorSyncStart = 16,
    .HorSyncSize = 96,
    .HorSyncSkew = 0,
    .HorEnableSkew = 0,
    .VerTotal = 525,
    .VerBlankSize = 0,
    .VerSyncStart = 10,
    .VerSyncSize = 2,
    .PixelClock = 25175000UL
};

static BOOL CalculatePll(struct BoardInfo *bi, struct ModeInfo *mode,
                         ULONG requested)
{
    static const UBYTE divisors[] = {16, 12, 8, 6, 4, 3, 2, 1};
    static const UBYTE codes[] = {5, 7, 3, 6, 2, 4, 1, 0};
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    ULONG targetKHz;
    ULONG chosenVco = 0;
    ULONG actualHz;
    ULONG difference;
    UWORD feedback;
    ULONG feedbackValue;
    UBYTE divisor = 0;
    UBYTE code = 0;
    UWORD index;
    unsigned long long numerator;
    unsigned long long denominator;

    if (!data || !mode || !requested || !data->RefClockKHz ||
        data->RefDivider < 2)
        return FALSE;

    targetKHz = (requested + 500UL) / 1000UL;
    for (index = 0; index < sizeof(divisors); ++index) {
        ULONG vco;
        if (targetKHz > 0xffffffffUL / divisors[index])
            continue;
        vco = targetKHz * divisors[index];
        if (vco >= data->MinPllKHz && vco <= data->MaxPllKHz) {
            divisor = divisors[index];
            code = codes[index];
            chosenVco = vco;
            break;
        }
    }
    if (!divisor)
        return FALSE;

    numerator = (unsigned long long)data->RefDivider * chosenVco +
                data->RefClockKHz / 2UL;
    feedbackValue = (ULONG)(numerator / data->RefClockKHz);
    if (!feedbackValue || feedbackValue > RADEON_PPLL_FB3_DIV_MASK)
        return FALSE;
    feedback = (UWORD)feedbackValue;

    numerator = (unsigned long long)data->RefClockKHz * 1000ULL * feedback;
    denominator = (unsigned long long)data->RefDivider * divisor;
    actualHz = (ULONG)((numerator + denominator / 2ULL) / denominator);
    difference = actualHz > requested ? actualHz - requested :
                                        requested - actualHz;
    if ((unsigned long long)difference * 200ULL > requested)
        return FALSE;

    mode->pll1.Numerator = (UBYTE)(feedback & 0xffU);
    mode->pll2.Denominator =
        (UBYTE)(((feedback >> 8) & 0x07U) | ((code & 0x07U) << 3));
    mode->PixelClock = actualHz;
    data->FeedbackDivider = feedback;
    data->PostDivider = divisor;
    data->PostDividerCode = code;
    return TRUE;
}

static BOOL DecodePll(const struct ModeInfo *mode, UWORD *feedback,
                      UBYTE *code)
{
    if (!mode || !feedback || !code)
        return FALSE;
    *feedback = (UWORD)mode->pll1.Numerator |
                ((UWORD)(mode->pll2.Denominator & 0x07U) << 8);
    *code = (mode->pll2.Denominator >> 3) & 0x07U;
    return *feedback != 0 && *feedback <= RADEON_PPLL_FB3_DIV_MASK;
}

static BOOL WaitAtomicUpdate(struct BoardInfo *bi)
{
    ULONG count;

    for (count = 0; count < 10000; ++count) {
        if (!(RadeonReadPll(bi, RADEON_PPLL_REF_DIV) &
              RADEON_PPLL_ATOMIC_UPDATE_R))
            return TRUE;
        RadeonDelayUs(1);
    }
    return FALSE;
}

static BOOL ProgramClock(struct BoardInfo *bi, struct ModeInfo *mode)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    UWORD feedback;
    UBYTE code;
    ULONG refdiv;
    ULONG divider;
    ULONG vclk;

    if (!data || !DecodePll(mode, &feedback, &code))
        return FALSE;

    refdiv = RadeonReadPll(bi, RADEON_PPLL_REF_DIV);
    refdiv = (refdiv & ~RADEON_PPLL_REF_DIV_MASK) | data->RefDivider;
    divider = RadeonReadPll(bi, RADEON_PPLL_DIV_0);
    divider &= ~(RADEON_PPLL_FB3_DIV_MASK |
                 RADEON_PPLL_POST3_DIV_MASK);
    divider |= feedback | ((ULONG)code << 16);
    vclk = RadeonReadPll(bi, RADEON_VCLK_ECP_CNTL);

    if (refdiv == RadeonReadPll(bi, RADEON_PPLL_REF_DIV) &&
        divider == RadeonReadPll(bi, RADEON_PPLL_DIV_0) &&
        (vclk & RADEON_VCLK_SRC_SEL_MASK) ==
            RADEON_VCLK_SRC_SEL_PPLLCLK)
        return RadeonMask32(bi, RADEON_CLOCK_CNTL_INDEX,
                            RADEON_PLL_DIV_SEL, 0);

    if (!RadeonMaskPll(bi, RADEON_VCLK_ECP_CNTL,
                       ~RADEON_VCLK_SRC_SEL_MASK,
                       RADEON_VCLK_SRC_SEL_CPUCLK) ||
        !RadeonMaskPll(bi, RADEON_PPLL_CNTL, 0xffffffffUL,
                       RADEON_PPLL_REFCLK_SEL |
                           RADEON_PPLL_FBCLK_SEL |
                           RADEON_PPLL_RESET |
                           RADEON_PPLL_ATOMIC_UPDATE_EN |
                           RADEON_PPLL_VGA_ATOMIC_UPDATE_EN) ||
        !RadeonMask32(bi, RADEON_CLOCK_CNTL_INDEX,
                      RADEON_PLL_DIV_SEL, 0) ||
        !RadeonWritePll(bi, RADEON_PPLL_REF_DIV, refdiv) ||
        !RadeonWritePll(bi, RADEON_PPLL_DIV_0, divider) ||
        !RadeonWritePll(bi, RADEON_PPLL_DIV_0, divider) ||
        !WaitAtomicUpdate(bi) ||
        !RadeonMaskPll(bi, RADEON_PPLL_REF_DIV, 0xffffffffUL,
                       RADEON_PPLL_ATOMIC_UPDATE_W) ||
        !WaitAtomicUpdate(bi) ||
        !RadeonWritePll(bi, RADEON_HTOTAL_CNTL, 0) ||
        !RadeonMaskPll(bi, RADEON_PPLL_CNTL,
                       ~(RADEON_PPLL_RESET | RADEON_PPLL_SLEEP |
                         RADEON_PPLL_ATOMIC_UPDATE_EN |
                         RADEON_PPLL_VGA_ATOMIC_UPDATE_EN), 0))
        return FALSE;

    RadeonDelayUs(50000);
    if (!RadeonMaskPll(bi, RADEON_VCLK_ECP_CNTL,
                       ~RADEON_VCLK_SRC_SEL_MASK,
                       RADEON_VCLK_SRC_SEL_PPLLCLK) ||
        !RadeonMaskPll(bi, RADEON_VCLK_ECP_CNTL, 0xffffffffUL,
                       RADEON_PIXCLK_ALWAYS_ONb |
                           RADEON_PIXCLK_DAC_ALWAYS_ONb))
        return FALSE;

    data->FeedbackDivider = feedback;
    data->PostDividerCode = code;
    return TRUE;
}

static BOOL ValidateMode(const struct ModeInfo *mode)
{
    ULONG hsyncStart;
    ULONG hsyncEnd;
    ULONG vsyncStart;
    ULONG vsyncEnd;

    if (!mode || mode->Width < 8 || mode->Width > 4096 ||
        (mode->Width & 7U) || mode->HorTotal <= mode->Width ||
        mode->HorTotal > 8192 || (mode->HorTotal & 7U) ||
        mode->HorSyncSize < 8 || mode->HorSyncSize > 504 ||
        (mode->HorSyncSize & 7U) || !mode->Height ||
        mode->Height > 2048 || mode->VerTotal <= mode->Height ||
        mode->VerTotal > 2048 || !mode->VerSyncSize ||
        mode->VerSyncSize > 31 ||
        (mode->Flags & (GMF_DOUBLECLOCK | GMF_DOUBLESCAN |
                        GMF_INTERLACE | GMF_DOUBLEVERTICAL)))
        return FALSE;

    hsyncStart = (ULONG)mode->Width + mode->HorSyncStart;
    hsyncEnd = hsyncStart + mode->HorSyncSize;
    vsyncStart = (ULONG)mode->Height + mode->VerSyncStart;
    vsyncEnd = vsyncStart + mode->VerSyncSize;
    return hsyncStart >= mode->Width && hsyncStart >= 8 &&
           hsyncStart <= 0x2007UL && hsyncEnd <= mode->HorTotal &&
           vsyncStart >= mode->Height && vsyncEnd <= mode->VerTotal;
}

static void ApplyDisplayState(struct BoardInfo *bi)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    ULONG disabled = 0;

    if (!data || !data->ModeValid || !data->DisplayEnabled ||
        data->DpmsLevel != DPMS_ON)
        disabled |= RADEON_CRTC_DISPLAY_DIS;
    if (data && data->DpmsLevel == DPMS_STANDBY)
        disabled |= RADEON_CRTC_HSYNC_DIS;
    else if (data && data->DpmsLevel == DPMS_SUSPEND)
        disabled |= RADEON_CRTC_VSYNC_DIS;
    else if (data && data->DpmsLevel == DPMS_OFF)
        disabled |= RADEON_CRTC_HSYNC_DIS | RADEON_CRTC_VSYNC_DIS;

    RadeonMask32(bi, RADEON_CRTC_EXT_CNTL,
                 RADEON_CRTC_DISPLAY_DIS | RADEON_CRTC_HSYNC_DIS |
                     RADEON_CRTC_VSYNC_DIS,
                  disabled);
}

static BOOL DisableUnsupportedBlocks(struct BoardInfo *bi)
{
    return RadeonWrite32(bi, RADEON_OVR_CLR, 0) &&
           RadeonWrite32(bi, RADEON_OVR_WID_LEFT_RIGHT, 0) &&
           RadeonWrite32(bi, RADEON_OVR_WID_TOP_BOTTOM, 0) &&
           RadeonWrite32(bi, RADEON_OV0_SCALE_CNTL, 0) &&
           RadeonWrite32(bi, RADEON_SUBPIC_CNTL, 0) &&
           RadeonWrite32(bi, RADEON_VIPH_CONTROL, 0) &&
           RadeonWrite32(bi, RADEON_I2C_CNTL_1, 0) &&
           RadeonWrite32(bi, RADEON_GEN_INT_CNTL, 0) &&
           RadeonWrite32(bi, RADEON_CAP0_TRIG_CNTL, 0) &&
           RadeonWrite32(bi, RADEON_CAP1_TRIG_CNTL, 0) &&
           RadeonWrite32(bi, RADEON_SURFACE0_INFO, 0) &&
           RadeonWrite32(bi, RADEON_SURFACE1_INFO, 0) &&
           RadeonWrite32(bi, RADEON_SURFACE2_INFO, 0) &&
           RadeonWrite32(bi, RADEON_SURFACE3_INFO, 0) &&
           RadeonWrite32(bi, RADEON_SURFACE4_INFO, 0) &&
           RadeonWrite32(bi, RADEON_SURFACE5_INFO, 0) &&
           RadeonWrite32(bi, RADEON_SURFACE6_INFO, 0) &&
           RadeonWrite32(bi, RADEON_SURFACE7_INFO, 0);
}

static BOOL ConfigureDac(struct BoardInfo *bi, RGBFTYPE format)
{
    ULONG dac = RadeonRead32(bi, RADEON_DAC_CNTL);
    ULONG pixelWidth;

    if (!GetFormatInfo(format, NULL, &pixelWidth))
        return FALSE;

    dac &= RADEON_DAC_RANGE_CNTL_MASK | RADEON_DAC_BLANKING;
    dac |= RADEON_DAC_MASK_ALL | RADEON_DAC_8BIT_EN;

    return RadeonWrite32(bi, RADEON_SURFACE_CNTL, 0) &&
           RadeonMask32(bi, RADEON_DISP_OUTPUT_CNTL,
                        RADEON_DISP_DAC_SOURCE_MASK, 0) &&
           RadeonMask32(bi, RADEON_DAC_CNTL2,
                        RADEON_DAC2_DAC_CLK_SEL |
                            RADEON_DAC2_PALETTE_ACC_CTL,
                        0) &&
           RadeonMask32(bi, RADEON_DAC_EXT_CNTL,
                        RADEON_DAC_FORCE_BLANK_OFF_EN |
                            RADEON_DAC_FORCE_DATA_EN,
                        0) &&
           RadeonWrite32(bi, RADEON_DAC_CNTL, dac) &&
           RadeonMask32(bi, RADEON_CRTC_GEN_CNTL,
                         RADEON_CRTC_PIX_WIDTH_MASK,
                         pixelWidth) &&
           RadeonMask32(bi, RADEON_DISP_MERGE_CNTL,
                         RADEON_DISP_RGB_OFFSET_EN, 0);
}

static BOOL ProgramPalette(struct BoardInfo *bi, UWORD start, UWORD count,
                           BOOL linear)
{
    ULONG savedClock;
    BOOL success;
    UWORD index;

    if (start >= 256 || count > 256U - start)
        return FALSE;

    savedClock = RadeonReadPll(bi, RADEON_VCLK_ECP_CNTL);
    success = RadeonWritePll(bi, RADEON_VCLK_ECP_CNTL,
                             savedClock &
                                 ~RADEON_PIXCLK_DAC_ALWAYS_ONb) &&
              RadeonMask32(bi, RADEON_DAC_CNTL2,
                           RADEON_DAC2_PALETTE_ACC_CTL, 0);

    for (index = start; success && index < (UWORD)(start + count);
         ++index) {
        UBYTE red = linear ? (UBYTE)index : bi->CLUT[index].Red;
        UBYTE green = linear ? (UBYTE)index : bi->CLUT[index].Green;
        UBYTE blue = linear ? (UBYTE)index : bi->CLUT[index].Blue;
        ULONG value = ((ULONG)red << 22) |
                      ((ULONG)green << 12) |
                      ((ULONG)blue << 2);
        success = RadeonWrite32(bi, RADEON_PALETTE_INDEX, index) &&
                  RadeonWrite32(bi, RADEON_PALETTE_30_DATA, value);
    }

    if (!RadeonWritePll(bi, RADEON_VCLK_ECP_CNTL, savedClock))
        success = FALSE;
    return success;
}

static BOOL ProgramTiming(struct BoardInfo *bi, struct ModeInfo *mode)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    ULONG hsyncStart;
    ULONG vsyncStart;
    ULONG hTotalDisplay;
    ULONG hSync;
    ULONG vTotalDisplay;
    ULONG vSync;
    ULONG crtc;
    ULONG pixelWidth;

    if (!data)
        return FALSE;

    data->ModeValid = FALSE;
    bi->ModeInfo = NULL;
    if (!RadeonMask32(bi, RADEON_CRTC_EXT_CNTL, 0,
                      RADEON_CRTC_DISPLAY_DIS |
                          RADEON_CRTC_HSYNC_DIS |
                          RADEON_CRTC_VSYNC_DIS) ||
        !ValidateMode(mode) ||
        !GetDepthPixelWidth(mode->Depth, &pixelWidth))
        return FALSE;

    hsyncStart = (ULONG)mode->Width + mode->HorSyncStart;
    vsyncStart = (ULONG)mode->Height + mode->VerSyncStart;
    hTotalDisplay = ((mode->HorTotal / 8UL) - 1UL) |
                    (((mode->Width / 8UL) - 1UL) << 16);
    hSync = (hsyncStart - 8UL) |
            (((ULONG)mode->HorSyncSize / 8UL) << 16);
    vTotalDisplay = ((ULONG)mode->VerTotal - 1UL) |
                    (((ULONG)mode->Height - 1UL) << 16);
    vSync = (vsyncStart - 1UL) |
            ((ULONG)mode->VerSyncSize << 16);
    if (mode->Flags & GMF_HPOLARITY)
        hSync |= RADEON_CRTC_H_SYNC_POL;
    if (mode->Flags & GMF_VPOLARITY)
        vSync |= RADEON_CRTC_V_SYNC_POL;

    crtc = RADEON_CRTC_EXT_DISP_EN | RADEON_CRTC_EN |
           pixelWidth;
    if (!DisableUnsupportedBlocks(bi) ||
        !RadeonMask32(bi, RADEON_CRTC2_GEN_CNTL, RADEON_CRTC2_EN,
                      RADEON_CRTC2_DISP_DIS |
                          RADEON_CRTC2_DISP_REQ_EN_B) ||
        !RadeonWrite32(bi, RADEON_CRTC_GEN_CNTL, crtc) ||
        !RadeonWrite32(bi, RADEON_CRTC_EXT_CNTL,
                       RADEON_VGA_ATI_LINEAR | RADEON_XCRT_CNT_EN |
                           RADEON_CRTC_CRT_ON |
                           RADEON_CRTC_DISPLAY_DIS |
                           RADEON_CRTC_HSYNC_DIS |
                           RADEON_CRTC_VSYNC_DIS) ||
        !RadeonWrite32(bi, RADEON_CRTC_H_TOTAL_DISP, hTotalDisplay) ||
        !RadeonWrite32(bi, RADEON_CRTC_H_SYNC_STRT_WID, hSync) ||
        !RadeonWrite32(bi, RADEON_CRTC_V_TOTAL_DISP, vTotalDisplay) ||
        !RadeonWrite32(bi, RADEON_CRTC_V_SYNC_STRT_WID, vSync) ||
        !RadeonWrite32(bi, RADEON_CRTC_OFFSET_CNTL, 0))
        return FALSE;

    data->ModeValid = TRUE;
    bi->ModeInfo = mode;
    bi->Depth = mode->Depth;
    return TRUE;
}

static BOOL RadeonSetSwitch(__REGA0(struct BoardInfo *bi),
                             __REGD0(BOOL state))
{
    BOOL previous = bi->MoniSwitch != 0;

    bi->MoniSwitch = state != FALSE;
    return previous;
}

static void RadeonSetColorArray(__REGA0(struct BoardInfo *bi),
                                __REGD0(UWORD start),
                                __REGD1(UWORD count))
{
    if (bi->RGBFormat == RGBFB_CLUT)
        (void)ProgramPalette(bi, start, count, FALSE);
}

static void RadeonSetDAC(__REGA0(struct BoardInfo *bi),
                         __REGD0(UWORD mode),
                         __REGD7(RGBFTYPE format))
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    (void)mode;
    if (!data || !ConfigureDac(bi, format) ||
        !ProgramPalette(bi, 0, 256, format != RGBFB_CLUT)) {
        if (data)
            data->ModeValid = FALSE;
        ApplyDisplayState(bi);
        return;
    }
    bi->RGBFormat = format;
}

static void RadeonSetGC(__REGA0(struct BoardInfo *bi),
                        __REGA1(struct ModeInfo *mode),
                        __REGD0(BOOL border))
{
    bi->Border = border;
    if (!ProgramTiming(bi, mode))
        RLOG("Radeon9200: rejected mode %ldx%ld @ %ldHz\n",
             mode ? (ULONG)mode->Width : 0,
             mode ? (ULONG)mode->Height : 0,
             mode ? mode->PixelClock : 0);
}

static void RadeonSetPanning(__REGA0(struct BoardInfo *bi),
                             __REGA1(UBYTE *memory),
                             __REGD0(UWORD width),
                             __REGD3(UWORD height),
                             __REGD1(WORD xOffset),
                             __REGD2(WORD yOffset),
                             __REGD7(RGBFTYPE format))
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    ULONG bytesPerPixel;
    ULONG pitch;
    ULONG crtcPitch;
    ULONG memoryLimit;
    ULONG memoryBase;
    ULONG memoryAddress;
    ULONG base;
    ULONG pixelGranularity;
    UWORD actualX;
    unsigned long long offset;
    unsigned long long last;

    (void)height;
    if (!data || !data->ModeValid || !bi->ModeInfo ||
        !FormatMatchesDepth(format, bi->ModeInfo->Depth) ||
        !GetFormatInfo(format, &bytesPerPixel, NULL) ||
        !CalculatePitch(width, format, &pitch, &crtcPitch) ||
        xOffset < 0 || yOffset < 0 || !memory)
        return;

    memoryBase = (ULONG)bi->MemoryBase;
    memoryAddress = (ULONG)memory;
    if (memoryAddress < memoryBase)
        return;
    base = memoryAddress - memoryBase;
    memoryLimit = data->InstalledVram;
    if (bi->MemorySize < memoryLimit)
        memoryLimit = bi->MemorySize;
    if (base >= memoryLimit)
        return;

    if ((ULONG)xOffset + bi->ModeInfo->Width > width)
        return;

    /* Linear CRTC offsets have eight-byte granularity on legacy Radeon. */
    pixelGranularity = 8UL / bytesPerPixel;
    actualX = (UWORD)xOffset & (UWORD)~(pixelGranularity - 1UL);
    offset = (unsigned long long)base +
              (unsigned long long)(UWORD)yOffset * pitch +
              (unsigned long long)actualX * bytesPerPixel;
    last = offset +
           (unsigned long long)(bi->ModeInfo->Height - 1U) * pitch +
           (unsigned long long)bi->ModeInfo->Width * bytesPerPixel;
    if ((offset & 7ULL) || offset > 0x07fffff8ULL ||
        last > memoryLimit)
        return;

    if (!RadeonWrite32(bi, RADEON_CRTC_PITCH,
                       crtcPitch | (crtcPitch << 16)) ||
        !RadeonWrite32(bi, RADEON_CRTC_OFFSET, (ULONG)offset)) {
        data->ModeValid = FALSE;
        ApplyDisplayState(bi);
        return;
    }
    data->CurrentPitch = pitch;
    bi->XOffset = (WORD)actualX;
    bi->YOffset = yOffset;
}

static UWORD RadeonCalculateBytesPerRow(__REGA0(struct BoardInfo *bi),
                                        __REGD0(UWORD width),
                                        __REGD1(UWORD height),
                                        __REGA1(struct ModeInfo *mode),
                                        __REGD7(RGBFTYPE format))
{
    ULONG pitch;

    (void)bi;
    (void)height;
    (void)mode;
    if (!CalculatePitch(width, format, &pitch, NULL))
        return 0;
    return (UWORD)pitch;
}

static APTR RadeonCalculateMemory(__REGA0(struct BoardInfo *bi),
                                  __REGA1(APTR memory),
                                  __REGD0(struct RenderInfo *render),
                                  __REGD7(RGBFTYPE format))
{
    (void)bi;
    (void)render;
    return GetFormatInfo(format, NULL, NULL) ? memory : NULL;
}

static ULONG RadeonGetCompatibleFormats(__REGA0(struct BoardInfo *bi),
                                         __REGD7(RGBFTYPE format))
{
    (void)bi;
    return GetFormatInfo(format, NULL, NULL) ? RADEON_SCANOUT_FORMATS : 0;
}

static BOOL RadeonSetDisplay(__REGA0(struct BoardInfo *bi),
                             __REGD0(BOOL state))
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    if (!data)
        return FALSE;
    data->DisplayEnabled = state != FALSE;
    ApplyDisplayState(bi);
    return TRUE;
}

static LONG RadeonResolvePixelClock(__REGA0(struct BoardInfo *bi),
                                    __REGA1(struct ModeInfo *mode),
                                    __REGD0(ULONG clock),
                                    __REGD7(RGBFTYPE format))
{
    if (!mode || !FormatMatchesDepth(format, mode->Depth) ||
        !ValidateMode(mode) ||
        !CalculatePll(bi, mode, clock))
        return -1;
    return 0;
}

static ULONG RadeonGetPixelClock(__REGA0(struct BoardInfo *bi),
                                  __REGA1(struct ModeInfo *mode),
                                  __REGD0(ULONG index),
                                  __REGD7(RGBFTYPE format))
{
    (void)bi;
    return mode && FormatMatchesDepth(format, mode->Depth) && index == 0 ?
               mode->PixelClock :
               0;
}

static void RadeonSetClock(__REGA0(struct BoardInfo *bi))
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    if (data && bi->ModeInfo && ProgramClock(bi, bi->ModeInfo)) {
        ApplyDisplayState(bi);
    } else {
        if (data)
            data->ModeValid = FALSE;
        ApplyDisplayState(bi);
    }
}

static void RadeonSetMemoryMode(__REGA0(struct BoardInfo *bi),
                                 __REGD7(RGBFTYPE format))
{
    (void)bi;
    (void)format;
}

static void RadeonSetWriteMask(__REGA0(struct BoardInfo *bi),
                               __REGD0(UBYTE mask))
{
    (void)bi;
    (void)mask;
}

static void RadeonSetClearMask(__REGA0(struct BoardInfo *bi),
                               __REGD0(UBYTE mask))
{
    bi->ClearMask = mask;
}

static void RadeonSetReadPlane(__REGA0(struct BoardInfo *bi),
                               __REGD0(UBYTE plane))
{
    (void)bi;
    (void)plane;
}

static ULONG RadeonGetVBeamPos(__REGA0(struct BoardInfo *bi))
{
    return (RadeonRead32(bi, RADEON_CRTC_VLINE_CRNT_VLINE) &
            RADEON_CRTC_CRNT_VLINE_MASK) >> 16;
}

static BOOL InVerticalBlank(struct BoardInfo *bi)
{
    ULONG display =
        ((RadeonRead32(bi, RADEON_CRTC_V_TOTAL_DISP) >> 16) & 0x7ffUL) + 1UL;
    return RadeonGetVBeamPos(bi) >= display;
}

static void RadeonWaitVerticalSync(__REGA0(struct BoardInfo *bi),
                                   __REGD0(BOOL end))
{
    ULONG count;

    for (count = 0; count < 100000; ++count) {
        BOOL blank = InVerticalBlank(bi);
        if ((!end && blank) || (end && !blank))
            return;
        RadeonDelayUs(1);
    }
}

static BOOL RadeonGetVSyncState(__REGA0(struct BoardInfo *bi),
                                __REGD0(BOOL expected))
{
    (void)expected;
    return InVerticalBlank(bi);
}

static void RadeonSetDPMSLevel(__REGA0(struct BoardInfo *bi),
                               __REGD0(ULONG level))
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    if (!data || level > DPMS_OFF)
        return;
    data->DpmsLevel = (UBYTE)level;
    ApplyDisplayState(bi);
}

void RadeonInstallCallbacks(struct BoardInfo *bi)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    UWORD index;

    for (index = 0; index < MAXMODES; ++index) {
        bi->MaxHorValue[index] = 0;
        bi->MaxVerValue[index] = 0;
        bi->MaxHorResolution[index] = 0;
        bi->MaxVerResolution[index] = 0;
        bi->PixelClockCount[index] = 0;
    }

    bi->BitsPerCannon = 8;
    bi->RGBFormats = RADEON_SCANOUT_FORMATS;
    bi->SoftSpriteFlags = RADEON_SCANOUT_FORMATS;
    for (index = CHUNKY; index <= TRUEALPHA; ++index) {
        if (index == TRUECOLOR)
            continue;
        bi->MaxHorValue[index] = 8184;
        bi->MaxVerValue[index] = 2047;
        bi->MaxHorResolution[index] = 4096;
        bi->MaxVerResolution[index] = 2048;
        bi->PixelClockCount[index] = 1;
    }
    bi->MaxBMWidth = 4096;
    bi->MaxBMHeight = 4096;
    bi->MaxPlanarMemory = 0;
    bi->Flags &= ~(BIF_HARDWARESPRITE | BIF_HASSPRITEBUFFER |
                   BIF_VBLANKINTERRUPT | BIF_VGASCREENSPLIT |
                   BIF_FLICKERFIXER | BIF_BLITTER |
                   BIF_INDISPLAYCHAIN | BIF_PALETTESWITCH |
                   BIF_DACSWITCH);
    bi->Flags |= BIF_GRANTDIRECTACCESS;

    bi->SetSwitch = RadeonSetSwitch;
    bi->SetColorArray = RadeonSetColorArray;
    bi->SetDAC = RadeonSetDAC;
    bi->SetGC = RadeonSetGC;
    bi->SetPanning = RadeonSetPanning;
    bi->CalculateBytesPerRow = RadeonCalculateBytesPerRow;
    bi->CalculateMemory = RadeonCalculateMemory;
    bi->GetCompatibleFormats = RadeonGetCompatibleFormats;
    bi->SetDisplay = RadeonSetDisplay;
    bi->ResolvePixelClock = RadeonResolvePixelClock;
    bi->GetPixelClock = RadeonGetPixelClock;
    bi->SetClock = RadeonSetClock;
    bi->SetMemoryMode = RadeonSetMemoryMode;
    bi->SetWriteMask = RadeonSetWriteMask;
    bi->SetClearMask = RadeonSetClearMask;
    bi->SetReadPlane = RadeonSetReadPlane;
    bi->WaitVerticalSync = RadeonWaitVerticalSync;
    bi->WaitBlitter = RadeonWaitBlitter;
    bi->GetVSyncState = RadeonGetVSyncState;
    bi->GetVBeamPos = RadeonGetVBeamPos;
    bi->SetDPMSLevel = RadeonSetDPMSLevel;

    if (data && data->AccelState == RADEON_ACCEL_READY &&
        !(bi->Flags & BIF_NOBLITTER)) {
        bi->FillRect = RadeonFillRect;
        bi->BlitRect = RadeonBlitRect;
        bi->BlitPattern = RadeonBlitPattern;
        bi->BlitRectNoMaskComplete = RadeonBlitRectNoMaskComplete;
        bi->Flags |= BIF_BLITTER;
        RLOG("Radeon9200: rectangle acceleration enabled\n");
    }

    if (data && !data->ModeValid) {
        data->DisplayEnabled = TRUE;
        data->DpmsLevel = DPMS_ON;
    }
}

BOOL RadeonShowStartupScreen(struct BoardInfo *bi)
{
    static const UBYTE colors[8][3] = {
        {0x00, 0x00, 0x00},
        {0xff, 0x00, 0x00},
        {0x00, 0xff, 0x00},
        {0x00, 0x00, 0xff},
        {0x00, 0xff, 0xff},
        {0xff, 0x00, 0xff},
        {0xff, 0xff, 0x00},
        {0xff, 0xff, 0xff}
    };
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct ExecBase *SysBase = bi->ExecBase;
    struct ModeInfo *mode;
    UBYTE *line;
    UWORD x;
    UWORD y;
    UWORD index;

    if (!data || data->StartupMode || data->InstalledVram <
                      (ULONG)STARTUP_PITCH * STARTUP_HEIGHT)
        return FALSE;

    mode = AllocMem(sizeof(*mode), MEMF_PUBLIC | MEMF_CLEAR);
    if (!mode)
        return FALSE;
    *mode = StartupModeTemplate;
    data->StartupMode = mode;

    data->DisplayEnabled = FALSE;
    data->DpmsLevel = DPMS_ON;
    bi->RGBFormat = RGBFB_CLUT;
    if (!CalculatePll(bi, mode, mode->PixelClock) ||
        !ProgramTiming(bi, mode) || !ConfigureDac(bi, RGBFB_CLUT) ||
        !ProgramClock(bi, mode) ||
        !RadeonWrite32(bi, RADEON_CRTC_PITCH,
                       (STARTUP_WIDTH / 8UL) |
                           ((STARTUP_WIDTH / 8UL) << 16)) ||
        !RadeonWrite32(bi, RADEON_CRTC_OFFSET, 0))
        goto failed;

    data->CurrentPitch = STARTUP_PITCH;
    bi->XOffset = 0;
    bi->YOffset = 0;

    for (index = 0; index < 256; ++index) {
        bi->CLUT[index].Red = (UBYTE)index;
        bi->CLUT[index].Green = (UBYTE)index;
        bi->CLUT[index].Blue = (UBYTE)index;
    }
    for (index = 0; index < 8; ++index) {
        bi->CLUT[index].Red = colors[index][0];
        bi->CLUT[index].Green = colors[index][1];
        bi->CLUT[index].Blue = colors[index][2];
    }
    if (!ProgramPalette(bi, 0, 256, FALSE))
        goto failed;

    line = AllocMem(STARTUP_PITCH, MEMF_PUBLIC);
    if (!line)
        goto failed;
    for (x = 0; x < STARTUP_WIDTH; ++x)
        line[x] = (UBYTE)(x / (STARTUP_WIDTH / 8U));
    for (y = 0; y < STARTUP_HEIGHT; ++y)
        host_to_pcicpy(line, (UBYTE *)bi->MemoryBase +
                                 (ULONG)y * STARTUP_PITCH,
                       STARTUP_PITCH);
    FreeMem(line, STARTUP_PITCH);

    data->DisplayEnabled = TRUE;
    ApplyDisplayState(bi);
    return TRUE;

failed:
    data->DisplayEnabled = FALSE;
    ApplyDisplayState(bi);
    if (bi->ModeInfo == mode)
        bi->ModeInfo = NULL;
    FreeMem(mode, sizeof(*mode));
    data->StartupMode = NULL;
    data->ModeValid = FALSE;
    return FALSE;
}
