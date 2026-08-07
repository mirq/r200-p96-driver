#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <graphics/displayinfo.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <inline/macros.h>
#include <intuition/screens.h>
#include <libraries/Picasso96.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P96MA_Dummy            (TAG_USER + 0x10000UL + 96UL)
#define P96MA_MinWidth         (P96MA_Dummy + 0x0001UL)
#define P96MA_MinHeight        (P96MA_Dummy + 0x0002UL)
#define P96MA_MinDepth         (P96MA_Dummy + 0x0003UL)
#define P96MA_MaxWidth         (P96MA_Dummy + 0x0004UL)
#define P96MA_MaxHeight        (P96MA_Dummy + 0x0005UL)
#define P96MA_MaxDepth         (P96MA_Dummy + 0x0006UL)
#define P96MA_FormatsAllowed   (P96MA_Dummy + 0x0008UL)

#define P96SA_Dummy            (TAG_USER + 0x20000UL + 96UL)
#define P96SA_Width            (P96SA_Dummy + 0x0003UL)
#define P96SA_Height           (P96SA_Dummy + 0x0004UL)
#define P96SA_Depth            (P96SA_Dummy + 0x0005UL)
#define P96SA_Title            (P96SA_Dummy + 0x0008UL)
#define P96SA_ErrorCode        (P96SA_Dummy + 0x000aUL)
#define P96SA_DisplayID        (P96SA_Dummy + 0x0012UL)
#define P96SA_ShowTitle        (P96SA_Dummy + 0x0014UL)
#define P96SA_Quiet            (P96SA_Dummy + 0x0016UL)
#define P96SA_Pens             (P96SA_Dummy + 0x0018UL)
#define P96SA_RGBFormat        (P96SA_Dummy + 0x001dUL)

enum {
    P96BMA_WIDTH,
    P96BMA_HEIGHT,
    P96BMA_DEPTH,
    P96BMA_MEMORY,
    P96BMA_BYTESPERROW,
    P96BMA_BYTESPERPIXEL,
    P96BMA_BITSPERPIXEL,
    P96BMA_RGBFORMAT,
    P96BMA_ISP96,
    P96BMA_ISONBOARD
};

enum {
    P96IDA_WIDTH,
    P96IDA_HEIGHT,
    P96IDA_DEPTH,
    P96IDA_BYTESPERPIXEL,
    P96IDA_BITSPERPIXEL,
    P96IDA_RGBFORMAT,
    P96IDA_ISP96,
    P96IDA_BOARDNUMBER,
    P96IDA_STDBYTESPERROW,
    P96IDA_BOARDNAME
};

#define MODENAMELENGTH 48

struct P96Mode {
    struct Node Node;
    char Description[MODENAMELENGTH];
    UWORD Width;
    UWORD Height;
    UWORD Depth;
    ULONG DisplayID;
};

struct Library *P96Base;
struct GfxBase *GfxBase;

#define p96AllocModeListTagList(tags) \
    LP1(0x48, struct List *, p96AllocModeListTagList, \
        struct TagItem *, tags, a0, , P96Base)
#define p96FreeModeList(list) \
    LP1NR(0x4e, p96FreeModeList, struct List *, list, a0, , P96Base)
#define p96GetModeIDAttr(mode, attribute) \
    LP2(0x54, ULONG, p96GetModeIDAttr, ULONG, mode, d0, \
        ULONG, attribute, d1, , P96Base)
#define p96OpenScreenTagList(tags) \
    LP1(0x5a, struct Screen *, p96OpenScreenTagList, \
        struct TagItem *, tags, a0, , P96Base)
#define p96CloseScreen(screen) \
    LP1(0x60, BOOL, p96CloseScreen, struct Screen *, screen, a0, , P96Base)
#define p96GetBitMapAttr(bitmap, attribute) \
    LP2(0x2a, ULONG, p96GetBitMapAttr, struct BitMap *, bitmap, a0, \
        ULONG, attribute, d0, , P96Base)
#define p96RectFill(rp, minX, minY, maxX, maxY, color) \
    LP6NR(0x7e, p96RectFill, struct RastPort *, rp, a1, \
          UWORD, minX, d0, UWORD, minY, d1, UWORD, maxX, d2, \
          UWORD, maxY, d3, ULONG, color, d4, , P96Base)
#define p96ReadPixel(rp, x, y) \
    LP3(0x78, ULONG, p96ReadPixel, struct RastPort *, rp, a1, \
        UWORD, x, d0, UWORD, y, d1, , P96Base)

static void DrawBars(struct Screen *screen, RGBFTYPE format)
{
    static const ULONG colors[8][3] = {
        {0x00000000UL, 0x00000000UL, 0x00000000UL},
        {0xffffffffUL, 0x00000000UL, 0x00000000UL},
        {0x00000000UL, 0xffffffffUL, 0x00000000UL},
        {0x00000000UL, 0x00000000UL, 0xffffffffUL},
        {0x00000000UL, 0xffffffffUL, 0xffffffffUL},
        {0xffffffffUL, 0x00000000UL, 0xffffffffUL},
        {0xffffffffUL, 0xffffffffUL, 0x00000000UL},
        {0xffffffffUL, 0xffffffffUL, 0xffffffffUL}
    };
    static const ULONG directColors[8] = {
        0x00000000UL,
        0x00ff0000UL,
        0x0000ff00UL,
        0x000000ffUL,
        0x0000ffffUL,
        0x00ff00ffUL,
        0x00ffff00UL,
        0x00ffffffUL
    };
    UWORD index;

    for (index = 0; index < 8; ++index) {
        LONG left = (LONG)index * screen->Width / 8;
        LONG right = (LONG)(index + 1U) * screen->Width / 8 - 1;

        if (format == RGBFB_CLUT) {
            SetRGB32(&screen->ViewPort, index, colors[index][0],
                     colors[index][1], colors[index][2]);
            SetAPen(&screen->RastPort, index);
            RectFill(&screen->RastPort, left, 0, right,
                     screen->Height - 1);
        } else {
            p96RectFill(&screen->RastPort, (UWORD)left, 0,
                        (UWORD)right, screen->Height - 1,
                        directColors[index]);
        }
    }
}

static BOOL CheckPixel(struct RastPort *rastPort, UWORD x, UWORD y,
                       ULONG expected, RGBFTYPE format, const char *name)
{
    ULONG actual = p96ReadPixel(rastPort, x, y);

    if (format == RGBFB_CLUT)
        actual &= 0xffUL;

    printf("TEST %-12s expected=%08x actual=%08x %s\n", name,
           (unsigned int)expected, (unsigned int)actual,
           actual == expected ? "PASS" : "FAIL");
    return actual == expected;
}

static void TestFill(struct RastPort *rastPort, RGBFTYPE format,
                     UWORD minX, UWORD minY, UWORD maxX, UWORD maxY,
                     ULONG color)
{
    if (format == RGBFB_CLUT) {
        SetWriteMask(rastPort, 0xff);
        SetAPen(rastPort, (ULONG)(UBYTE)color);
        RectFill(rastPort, minX, minY, maxX, maxY);
    } else {
        p96RectFill(rastPort, minX, minY, maxX, maxY, color);
    }
}

static ULONG ExpectedColor(RGBFTYPE format, ULONG color)
{
    if (format == RGBFB_R5G6B5PC)
        return color & 0x00f8fcf8UL;
    return color;
}

static BOOL RunAccelerationTests(struct Screen *screen, RGBFTYPE format)
{
    struct RastPort *rastPort = &screen->RastPort;
    struct BitMap *bitmap = rastPort->BitMap;
    ULONG colorA = format == RGBFB_CLUT ? 3UL : 0x00ff0000UL;
    ULONG colorB = format == RGBFB_CLUT ? 4UL : 0x0000ffffUL;
    ULONG expectedA;
    ULONG expectedB;
    BOOL success = TRUE;

    TestFill(rastPort, format, 10, 10, 74, 32, colorA);
    expectedA = ExpectedColor(format, colorA);
    expectedB = ExpectedColor(format, colorB);
    success &= CheckPixel(rastPort, 74, 32, expectedA, format,
                          "fill-edge");

    TestFill(rastPort, format, 250, 40, 449, 59, colorB);
    TestFill(rastPort, format, 300, 45, 399, 54, colorA);
    success &= CheckPixel(rastPort, 300, 45, expectedA, format,
                          "rebase-start");
    success &= CheckPixel(rastPort, 399, 54, expectedA, format,
                          "rebase-end");
    success &= CheckPixel(rastPort, 299, 45, expectedB, format,
                          "rebase-left");
    success &= CheckPixel(rastPort, 400, 54, expectedB, format,
                          "rebase-right");

    TestFill(rastPort, format, 20, 80, 39, 99, colorA);
    TestFill(rastPort, format, 40, 80, 59, 99, colorB);
    expectedA = ExpectedColor(format, colorA);
    expectedB = ExpectedColor(format, colorB);
    success &= expectedA != expectedB;
    BltBitMap(bitmap, 20, 80, bitmap, 30, 80, 40, 20,
              0xc0, 0xff, NULL);
    success &= CheckPixel(rastPort, 35, 90, expectedA, format,
                          "copy-right-a");
    success &= CheckPixel(rastPort, 55, 90, expectedB, format,
                          "copy-right-b");

    TestFill(rastPort, format, 20, 190, 39, 209, colorA);
    TestFill(rastPort, format, 40, 190, 59, 209, colorB);
    BltBitMap(bitmap, 30, 190, bitmap, 20, 190, 30, 20,
              0xc0, 0xff, NULL);
    success &= CheckPixel(rastPort, 25, 200, expectedA, format,
                          "copy-left-a");
    success &= CheckPixel(rastPort, 45, 200, expectedB, format,
                          "copy-left-b");

    TestFill(rastPort, format, 80, 120, 99, 129, colorA);
    TestFill(rastPort, format, 80, 130, 99, 139, colorB);
    expectedA = p96ReadPixel(rastPort, 90, 123);
    expectedB = p96ReadPixel(rastPort, 90, 133);
    BltBitMap(bitmap, 80, 120, bitmap, 80, 125, 20, 20,
              0xc0, 0xff, NULL);
    success &= CheckPixel(rastPort, 90, 128, expectedA, format,
                          "copy-down-a");
    success &= CheckPixel(rastPort, 90, 138, expectedB, format,
                          "copy-down-b");

    TestFill(rastPort, format, 80, 230, 99, 239, colorA);
    TestFill(rastPort, format, 80, 240, 99, 254, colorB);
    BltBitMap(bitmap, 80, 235, bitmap, 80, 230, 20, 20,
              0xc0, 0xff, NULL);
    success &= CheckPixel(rastPort, 90, 233, expectedA, format,
                          "copy-up-a");
    success &= CheckPixel(rastPort, 90, 248, expectedB, format,
                          "copy-up-b");

    if (format == RGBFB_CLUT) {
        SetWriteMask(rastPort, 0xff);
        SetAPen(rastPort, 0xaa);
        RectFill(rastPort, 120, 160, 139, 179);
        SetWriteMask(rastPort, 0x0f);
        SetAPen(rastPort, 0x55);
        RectFill(rastPort, 120, 160, 139, 179);
        SetWriteMask(rastPort, 0xff);
        success &= CheckPixel(rastPort, 130, 170, 0xa5, format,
                              "fill-mask");

        SetWriteMask(rastPort, 0xff);
        SetAPen(rastPort, 0x55);
        RectFill(rastPort, 200, 280, 219, 299);
        SetAPen(rastPort, 0xaa);
        RectFill(rastPort, 240, 280, 259, 299);
        BltBitMap(bitmap, 200, 280, bitmap, 240, 280, 20, 20,
                  0xc0, 0x0f, NULL);
        success &= CheckPixel(rastPort, 250, 290, 0xa5, format,
                              "copy-mask");
    }

    printf("ACCELTEST depth=%u %s\n",
           (unsigned int)p96GetBitMapAttr(bitmap, P96BMA_DEPTH),
           success ? "PASS" : "FAIL");
    return success;
}

static BOOL RunCompleteCopyTests(struct Screen *screen, RGBFTYPE format)
{
    struct RastPort *destinationPort = &screen->RastPort;
    struct BitMap *destination = destinationPort->BitMap;
    struct BitMap *source;
    struct RastPort sourcePort;
    ULONG colorA = format == RGBFB_CLUT ? 3UL : 0x00ff0000UL;
    ULONG colorB = format == RGBFB_CLUT ? 4UL : 0x0000ffffUL;
    ULONG expectedA;
    ULONG expectedB;
    BOOL success = TRUE;

    source = AllocBitMap(screen->Width, 64,
                         (ULONG)p96GetBitMapAttr(destination,
                                                P96BMA_DEPTH),
                         BMF_MINPLANES | BMF_CLEAR, destination);
    if (!source) {
        printf("COMPLETECOPY alloc failed\n");
        return FALSE;
    }

    InitRastPort(&sourcePort);
    sourcePort.BitMap = source;
    printf("COMPLETECOPY srcmem=%08x dstmem=%08x srcbpr=%u dstbpr=%u "
           "onboard=%u\n",
           (unsigned int)p96GetBitMapAttr(source, P96BMA_MEMORY),
           (unsigned int)p96GetBitMapAttr(destination, P96BMA_MEMORY),
           (unsigned int)p96GetBitMapAttr(source, P96BMA_BYTESPERROW),
           (unsigned int)p96GetBitMapAttr(destination,
                                          P96BMA_BYTESPERROW),
           (unsigned int)p96GetBitMapAttr(source, P96BMA_ISONBOARD));
    success &= p96GetBitMapAttr(source, P96BMA_ISONBOARD) != 0;
    success &= p96GetBitMapAttr(source, P96BMA_BYTESPERROW) ==
               p96GetBitMapAttr(destination, P96BMA_BYTESPERROW);

    TestFill(&sourcePort, format, 16, 8, 79, 39, colorA);
    TestFill(destinationPort, format, 490, 310, 573, 361, colorB);
    expectedA = p96ReadPixel(&sourcePort, 20, 10);
    expectedB = p96ReadPixel(destinationPort, 495, 315);
    if (format == RGBFB_CLUT) {
        expectedA &= 0xffUL;
        expectedB &= 0xffUL;
    }

    BltBitMap(source, 16, 8, destination, 500, 320,
              64, 32, 0xc0, 0xff, NULL);
    WaitBlit();
    success &= CheckPixel(destinationPort, 500, 320, expectedA,
                          format, "complete-start");
    success &= CheckPixel(destinationPort, 563, 351, expectedA,
                          format, "complete-end");
    success &= CheckPixel(destinationPort, 499, 319, expectedB,
                          format, "complete-edge");
    success &= CheckPixel(&sourcePort, 20, 10, expectedA,
                          format, "complete-source");

    FreeBitMap(source);
    printf("COMPLETECOPY depth=%u %s\n",
           (unsigned int)p96GetBitMapAttr(destination, P96BMA_DEPTH),
           success ? "PASS" : "FAIL");
    return success;
}

static unsigned long long StampTicks(const struct DateStamp *stamp)
{
    return (((unsigned long long)stamp->ds_Days * 24ULL * 60ULL +
             (ULONG)stamp->ds_Minute) * 60ULL * 50ULL) +
           (ULONG)stamp->ds_Tick;
}

static void BenchmarkClut(struct Screen *screen)
{
    struct RastPort *rastPort = &screen->RastPort;
    struct BitMap *bitmap = rastPort->BitMap;
    struct DateStamp start;
    struct DateStamp end;
    unsigned long long elapsed;
    UWORD index;

    DateStamp(&start);
    for (index = 0; index < 256; ++index) {
        SetAPen(rastPort, index & 1U);
        RectFill(rastPort, 0, 0, screen->Width - 1,
                 screen->Height - 1);
    }
    (void)p96ReadPixel(rastPort, 0, 0);
    DateStamp(&end);
    elapsed = StampTicks(&end) - StampTicks(&start);
    printf("BENCH fill256 ticks=%lu\n", (unsigned long)elapsed);

    SetAPen(rastPort, 1);
    RectFill(rastPort, 0, 0, screen->Width / 2U - 1,
             screen->Height - 1);
    DateStamp(&start);
    for (index = 0; index < 256; ++index) {
        UWORD source = (index & 1U) ? screen->Width / 2U : 0;
        UWORD destination = (index & 1U) ? 0 : screen->Width / 2U;

        BltBitMap(bitmap, source, 0, bitmap, destination, 0,
                  screen->Width / 2U, screen->Height,
                  0xc0, 0xff, NULL);
    }
    (void)p96ReadPixel(rastPort, 0, 0);
    DateStamp(&end);
    elapsed = StampTicks(&end) - StampTicks(&start);
    printf("BENCH copy256 ticks=%lu\n", (unsigned long)elapsed);
}

int main(int argc, char **argv)
{
    static WORD pens[] = {-1};
    struct TagItem modeTags[] = {
        {P96MA_MinWidth, 640},
        {P96MA_MinHeight, 480},
        {P96MA_MinDepth, 8},
        {P96MA_MaxWidth, 640},
        {P96MA_MaxHeight, 480},
        {P96MA_MaxDepth, 8},
        {P96MA_FormatsAllowed, RGBFF_CLUT},
        {TAG_DONE, 0}
    };
    struct TagItem screenTags[10];
    struct List *modes;
    struct P96Mode *mode;
    struct Screen *screen;
    ULONG displayId = INVALID_ID;
    ULONG error = 0;
    ULONG formatMask;
    ULONG seconds = 30;
    UWORD depth = 8;
    RGBFTYPE format;
    BOOL runTests = FALSE;
    int result = RETURN_FAIL;

    if (argc > 1)
        depth = (UWORD)atoi(argv[1]);
    if (argc > 2)
        seconds = (ULONG)atoi(argv[2]);
    if (argc > 3 && strcmp(argv[3], "test") == 0)
        runTests = TRUE;
    if (!seconds || seconds > 120)
        seconds = 30;

    switch (depth) {
    case 8:
        format = RGBFB_CLUT;
        formatMask = RGBFF_CLUT;
        break;
    case 16:
        format = RGBFB_R5G6B5PC;
        formatMask = RGBFF_R5G6B5PC;
        break;
    case 32:
        format = RGBFB_B8G8R8A8;
        formatMask = RGBFF_B8G8R8A8;
        break;
    default:
        printf("USAGE p96screen [8|16|32] [seconds] [test]\n");
        goto done;
    }

    modeTags[2].ti_Data = depth;
    modeTags[5].ti_Data = depth;
    modeTags[6].ti_Data = formatMask;

    P96Base = OpenLibrary((STRPTR)"Picasso96API.library", 2);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39);
    if (!P96Base || !GfxBase) {
        printf("OPENLIB p96=%08x graphics=%08x\n",
               (unsigned int)(ULONG)P96Base,
               (unsigned int)(ULONG)GfxBase);
        goto done;
    }

    modes = p96AllocModeListTagList(modeTags);
    if (!modes) {
        printf("MODELIST failed\n");
        goto done;
    }

    for (mode = (struct P96Mode *)modes->lh_Head;
         mode->Node.ln_Succ;
         mode = (struct P96Mode *)mode->Node.ln_Succ) {
        char *board = (char *)p96GetModeIDAttr(mode->DisplayID,
                                               P96IDA_BOARDNAME);

        printf("MODE id=%08x %ux%ux%u %s\n",
               (unsigned int)mode->DisplayID,
               (unsigned int)mode->Width, (unsigned int)mode->Height,
               (unsigned int)mode->Depth,
               mode->Description);
        if (mode->Width == 640 && mode->Height == 480 &&
            mode->Depth == depth && board &&
            strcmp(board, "Radeon9200") == 0)
            displayId = mode->DisplayID;
    }
    p96FreeModeList(modes);

    if (displayId == (ULONG)INVALID_ID) {
        printf("SELECT failed\n");
        goto done;
    }

    printf("SELECT id=%08x attr=%ux%ux%u format=%u board=%s\n",
           (unsigned int)displayId,
           (unsigned int)p96GetModeIDAttr(displayId, P96IDA_WIDTH),
           (unsigned int)p96GetModeIDAttr(displayId, P96IDA_HEIGHT),
           (unsigned int)p96GetModeIDAttr(displayId, P96IDA_DEPTH),
           (unsigned int)p96GetModeIDAttr(displayId, P96IDA_RGBFORMAT),
           (char *)p96GetModeIDAttr(displayId, P96IDA_BOARDNAME));

    screenTags[0].ti_Tag = P96SA_DisplayID;
    screenTags[0].ti_Data = displayId;
    screenTags[1].ti_Tag = P96SA_Width;
    screenTags[1].ti_Data = 640;
    screenTags[2].ti_Tag = P96SA_Height;
    screenTags[2].ti_Data = 480;
    screenTags[3].ti_Tag = P96SA_Depth;
    screenTags[3].ti_Data = depth;
    screenTags[4].ti_Tag = P96SA_RGBFormat;
    screenTags[4].ti_Data = format;
    screenTags[5].ti_Tag = P96SA_Pens;
    screenTags[5].ti_Data = (ULONG)pens;
    screenTags[6].ti_Tag = P96SA_Title;
    screenTags[6].ti_Data = (ULONG)"Radeon9200 P96 Test";
    screenTags[7].ti_Tag = P96SA_ShowTitle;
    screenTags[7].ti_Data = FALSE;
    screenTags[8].ti_Tag = P96SA_ErrorCode;
    screenTags[8].ti_Data = (ULONG)&error;
    screenTags[9].ti_Tag = TAG_DONE;
    screenTags[9].ti_Data = 0;

    screen = p96OpenScreenTagList(screenTags);
    if (!screen) {
        printf("OPENSCREEN failed error=%u\n", (unsigned int)error);
        goto done;
    }

    printf("OPENSCREEN screen=%08x bitmap=%08x memory=%08x bpr=%u "
           "bpp=%u format=%u onboard=%u\n",
           (unsigned int)(ULONG)screen,
           (unsigned int)(ULONG)screen->RastPort.BitMap,
           (unsigned int)p96GetBitMapAttr(screen->RastPort.BitMap,
                                          P96BMA_MEMORY),
           (unsigned int)p96GetBitMapAttr(screen->RastPort.BitMap,
                                          P96BMA_BYTESPERROW),
           (unsigned int)p96GetBitMapAttr(screen->RastPort.BitMap,
                                          P96BMA_BYTESPERPIXEL),
           (unsigned int)p96GetBitMapAttr(screen->RastPort.BitMap,
                                          P96BMA_RGBFORMAT),
           (unsigned int)p96GetBitMapAttr(screen->RastPort.BitMap,
                                          P96BMA_ISONBOARD));
    DrawBars(screen, format);
    if (runTests) {
        if (!RunAccelerationTests(screen, format))
            result = RETURN_WARN;
        if (!RunCompleteCopyTests(screen, format))
            result = RETURN_WARN;
        if (format == RGBFB_CLUT)
            BenchmarkClut(screen);
    }
    if (format != RGBFB_CLUT) {
        UWORD index;

        printf("READBACK");
        for (index = 0; index < 8; ++index) {
            UWORD x = (UWORD)(((2UL * index + 1UL) * screen->Width) /
                              16UL);
            printf(" %08x", (unsigned int)p96ReadPixel(
                       &screen->RastPort, x, screen->Height / 2U));
        }
        printf("\n");
    }
    printf("DISPLAYING depth=%u format=%u for %u seconds\n",
           (unsigned int)depth, (unsigned int)format,
           (unsigned int)seconds);
    Delay(seconds * 50UL);
    p96CloseScreen(screen);
    printf("CLOSED\n");
    if (result != RETURN_WARN)
        result = RETURN_OK;

done:
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (P96Base)
        CloseLibrary(P96Base);
    return result;
}
