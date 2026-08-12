#include <devices/timer.h>
#include <dos/dos.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <graphics/clip.h>
#include <graphics/displayinfo.h>
#include <graphics/gfxbase.h>
#include <graphics/layers.h>
#include <graphics/rastport.h>
#include <inline/macros.h>
#include <intuition/intuition.h>
#include <libraries/Picasso96.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#include <proto/timer.h>
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
#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480
#define TARGET_LEFT 24
#define TARGET_TOP 24
#define TARGET_WIDTH 592
#define TARGET_HEIGHT 432
#define OCCLUDER_COUNT 3
#define TRIAL_COUNT 7
#define WARMUP_COUNT 2
#define ITERATIONS 12

struct P96Mode {
    struct Node Node;
    char Description[MODENAMELENGTH];
    UWORD Width;
    UWORD Height;
    UWORD Depth;
    ULONG DisplayID;
};

struct ClipCounts {
    ULONG Total;
    ULONG Visible;
    ULONG Obscured;
};

typedef void (*DrawFunction)(struct Window *, ULONG);

static void Sort(unsigned long long *values, UWORD count);

struct Library *P96Base;
struct IntuitionBase *IntuitionBase;
struct Library *LayersBase;
struct GfxBase *GfxBase;
struct Device *TimerBase;

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

static void InitList(struct List *list)
{
    list->lh_Head = (struct Node *)&list->lh_Tail;
    list->lh_Tail = NULL;
    list->lh_TailPred = (struct Node *)&list->lh_Head;
}

static BOOL OpenTimerDevice(struct timerequest *request,
                            struct MsgPort *port)
{
    memset(port, 0, sizeof(*port));
    memset(request, 0, sizeof(*request));
    port->mp_Node.ln_Type = NT_MSGPORT;
    port->mp_Flags = PA_IGNORE;
    InitList(&port->mp_MsgList);
    request->tr_node.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    request->tr_node.io_Message.mn_ReplyPort = port;
    request->tr_node.io_Message.mn_Length = sizeof(*request);
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK,
                   (struct IORequest *)request, 0))
        return FALSE;
    TimerBase = request->tr_node.io_Device;
    return TRUE;
}

static unsigned long long ReadClock(void)
{
    struct EClockVal value;

    ReadEClock(&value);
    return ((unsigned long long)value.ev_hi << 32) | value.ev_lo;
}

static ULONG FindMode(UWORD depth, RGBFTYPE format)
{
    struct TagItem tags[] = {
        {P96MA_MinWidth, SCREEN_WIDTH},
        {P96MA_MinHeight, SCREEN_HEIGHT},
        {P96MA_MinDepth, depth},
        {P96MA_MaxWidth, SCREEN_WIDTH},
        {P96MA_MaxHeight, SCREEN_HEIGHT},
        {P96MA_MaxDepth, depth},
        {P96MA_FormatsAllowed, 1UL << format},
        {TAG_DONE, 0}
    };
    struct List *modes = p96AllocModeListTagList(tags);
    struct P96Mode *mode;
    ULONG result = INVALID_ID;

    if (!modes)
        return INVALID_ID;
    for (mode = (struct P96Mode *)modes->lh_Head;
         mode->Node.ln_Succ;
         mode = (struct P96Mode *)mode->Node.ln_Succ) {
        char *board = (char *)p96GetModeIDAttr(mode->DisplayID,
                                               P96IDA_BOARDNAME);

        if (mode->Width == SCREEN_WIDTH && mode->Height == SCREEN_HEIGHT &&
            mode->Depth == depth && board &&
            (strcmp(board, "Radeon9200") == 0 ||
             strcmp(board, "Radeon") == 0 ||
             strcmp(board, "RV280-PrmPCI") == 0)) {
            result = mode->DisplayID;
            break;
        }
    }
    p96FreeModeList(modes);
    return result;
}

static struct Window *OpenBenchWindow(struct Screen *screen, LONG left,
                                      LONG top, LONG width, LONG height,
                                      const char *title)
{
    struct TagItem tags[] = {
        {WA_CustomScreen, (ULONG)screen},
        {WA_Left, (ULONG)left},
        {WA_Top, (ULONG)top},
        {WA_Width, (ULONG)width},
        {WA_Height, (ULONG)height},
        {WA_Title, (ULONG)title},
        {WA_Borderless, TRUE},
        {WA_Activate, FALSE},
        {WA_SmartRefresh, TRUE},
        {WA_BackFill, (ULONG)LAYERS_NOBACKFILL},
        {TAG_DONE, 0}
    };

    return OpenWindowTagList(NULL, tags);
}

static struct ClipCounts CountClipRects(struct Window *window)
{
    struct ClipCounts counts = {0, 0, 0};
    struct ClipRect *clip;

    if (!window || !window->WLayer)
        return counts;
    LockLayer(0, window->WLayer);
    for (clip = window->WLayer->ClipRect; clip; clip = clip->Next) {
        ++counts.Total;
        if (clip->obscured)
            ++counts.Obscured;
        else
            ++counts.Visible;
    }
    UnlockLayer(window->WLayer);
    return counts;
}

static void DrawFillWorkload(struct Window *target, ULONG iteration)
{
    struct RastPort *rp = target->RPort;
    WORD width = target->Width;
    WORD height = target->Height;

    SetDrMd(rp, JAM2);
    SetBPen(rp, 0);
    SetAPen(rp, 1 + (iteration & 1UL));
    RectFill(rp, 0, 0, width - 1, height - 1);
}

static void DrawTextWorkload(struct Window *target, ULONG iteration)
{
    static const char text[] =
        "Picasso96 Radeon overlap redraw benchmark 0123456789";
    struct RastPort *rp = target->RPort;
    WORD height = target->Height;
    WORD row;

    (void)iteration;
    SetDrMd(rp, JAM2);
    SetBPen(rp, 0);
    SetAPen(rp, 3);
    for (row = 12; row < height - 8; row += 12) {
        Move(rp, 8 + ((row >> 2) & 7), row);
        Text(rp, (CONST_STRPTR)text, sizeof(text) - 1U);
    }
}

static void DrawBarsWorkload(struct Window *target, ULONG iteration)
{
    struct RastPort *rp = target->RPort;
    WORD width = target->Width;
    WORD height = target->Height;
    WORD row;

    (void)iteration;
    SetAPen(rp, 2);
    for (row = 0; row < height; row += 16)
        RectFill(rp, (row * 7) % (width - 80), row, width - 1, row + 2);
}

static void DrawCombinedWorkload(struct Window *target, ULONG iteration)
{
    DrawFillWorkload(target, iteration);
    DrawTextWorkload(target, iteration);
    DrawBarsWorkload(target, iteration);
}

static unsigned long long RunTrial(struct Window *target, ULONG iterations,
                                   DrawFunction draw)
{
    unsigned long long start;
    unsigned long long end;
    ULONG index;

    WaitBlit();
    start = ReadClock();
    for (index = 0; index < iterations; ++index)
        draw(target, index);
    WaitBlit();
    end = ReadClock();
    return end - start;
}

static void MeasurePhase(struct Window *target, ULONG iterations,
                         DrawFunction draw, unsigned long long *values)
{
    UWORD index;

    for (index = 0; index < WARMUP_COUNT; ++index)
        (void)RunTrial(target, iterations, draw);
    for (index = 0; index < TRIAL_COUNT; ++index)
        values[index] = RunTrial(target, iterations, draw);
    Sort(values, TRIAL_COUNT);
}

static void Sort(unsigned long long *values, UWORD count)
{
    UWORD i;

    for (i = 1; i < count; ++i) {
        unsigned long long value = values[i];
        UWORD j = i;

        while (j && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
}

static unsigned long long Median(unsigned long long *values, UWORD count)
{
    Sort(values, count);
    return values[count / 2U];
}

static void PrintTrials(const char *name, unsigned long long *values,
                        UWORD count, ULONG rate)
{
    UWORD index;

    printf("TRIALS %s", name);
    for (index = 0; index < count; ++index)
        printf(" %llu", values[index]);
    printf(" ticks median=%llu us=%lu\n", values[count / 2U],
           (unsigned long)((values[count / 2U] * 1000000ULL) / rate));
}

int main(int argc, char **argv)
{
    static WORD pens[] = {-1};
    static const LONG occluders[OCCLUDER_COUNT][4] = {
        {120, 84, 300, 124},
        {260, 184, 300, 124},
        {72, 292, 350, 116}
    };
    struct TagItem screenTags[] = {
        {P96SA_DisplayID, INVALID_ID},
        {P96SA_Width, SCREEN_WIDTH},
        {P96SA_Height, SCREEN_HEIGHT},
        {P96SA_Depth, 16},
        {P96SA_RGBFormat, RGBFB_R5G6B5PC},
        {P96SA_Pens, (ULONG)pens},
        {P96SA_Title, (ULONG)"P96 overlap benchmark"},
        {P96SA_ShowTitle, FALSE},
        {P96SA_Quiet, TRUE},
        {P96SA_ErrorCode, 0},
        {TAG_DONE, 0}
    };
    struct timerequest timerRequest;
    struct MsgPort timerPort;
    struct Screen *screen = NULL;
    struct Window *target = NULL;
    struct Window *cover[OCCLUDER_COUNT] = {NULL, NULL, NULL};
    struct ClipCounts emptyClips;
    struct ClipCounts overlapClips;
    unsigned long long empty[TRIAL_COUNT];
    unsigned long long overlap[TRIAL_COUNT];
    unsigned long long emptyFill[TRIAL_COUNT];
    unsigned long long overlapFill[TRIAL_COUNT];
    unsigned long long emptyText[TRIAL_COUNT];
    unsigned long long overlapText[TRIAL_COUNT];
    unsigned long long emptyBars[TRIAL_COUNT];
    unsigned long long overlapBars[TRIAL_COUNT];
    unsigned long long emptyMedian;
    unsigned long long overlapMedian;
    ULONG error = 0;
    ULONG rate = 0;
    ULONG displayId;
    ULONG iterations = ITERATIONS;
    UWORD index;
    int result = RETURN_FAIL;

    if (argc > 1) {
        iterations = (ULONG)atoi(argv[1]);
        if (!iterations || iterations > 1000)
            iterations = ITERATIONS;
    }

    P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 39);
    LayersBase = OpenLibrary((CONST_STRPTR)"layers.library", 39);
    if (!P96Base || !GfxBase || !IntuitionBase || !LayersBase ||
        !OpenTimerDevice(&timerRequest, &timerPort)) {
        printf("ERROR libraries/timer unavailable\n");
        goto done;
    }
    {
        struct EClockVal value;

        rate = ReadEClock(&value);
    }
    if (!rate) {
        printf("ERROR EClock unavailable\n");
        goto done;
    }

    displayId = FindMode(16, RGBFB_R5G6B5PC);
    if (displayId == (ULONG)INVALID_ID) {
        printf("ERROR 640x480x16 Radeon mode unavailable\n");
        goto done;
    }
    screenTags[0].ti_Data = displayId;
    screenTags[9].ti_Data = (ULONG)&error;
    screen = p96OpenScreenTagList(screenTags);
    if (!screen) {
        printf("ERROR screen open failed code=%lu\n", (unsigned long)error);
        goto done;
    }
    target = OpenBenchWindow(screen, TARGET_LEFT, TARGET_TOP,
                             TARGET_WIDTH, TARGET_HEIGHT, "target");
    if (!target) {
        printf("ERROR target window open failed\n");
        goto done;
    }

    emptyClips = CountClipRects(target);
    MeasurePhase(target, iterations, DrawCombinedWorkload, empty);
    MeasurePhase(target, iterations, DrawFillWorkload, emptyFill);
    MeasurePhase(target, iterations, DrawTextWorkload, emptyText);
    MeasurePhase(target, iterations, DrawBarsWorkload, emptyBars);

    for (index = 0; index < OCCLUDER_COUNT; ++index) {
        cover[index] = OpenBenchWindow(screen, occluders[index][0],
                                       occluders[index][1],
                                       occluders[index][2],
                                       occluders[index][3], "cover");
        if (!cover[index]) {
            printf("ERROR occluder %u open failed\n", index);
            goto done;
        }
        SetAPen(cover[index]->RPort, 4 + index);
        RectFill(cover[index]->RPort, 0, 0,
                 cover[index]->Width - 1, cover[index]->Height - 1);
    }
    WaitBlit();
    overlapClips = CountClipRects(target);
    MeasurePhase(target, iterations, DrawCombinedWorkload, overlap);
    MeasurePhase(target, iterations, DrawFillWorkload, overlapFill);
    MeasurePhase(target, iterations, DrawTextWorkload, overlapText);
    MeasurePhase(target, iterations, DrawBarsWorkload, overlapBars);

    emptyMedian = Median(empty, TRIAL_COUNT);
    overlapMedian = Median(overlap, TRIAL_COUNT);
    printf("P96OVERLAP version=2 mode=640x480x16 iterations=%lu "
           "trials=%u eclock=%lu\n", (unsigned long)iterations,
           TRIAL_COUNT, (unsigned long)rate);
    printf("CLIPRECT empty total=%lu visible=%lu obscured=%lu\n",
           (unsigned long)emptyClips.Total,
           (unsigned long)emptyClips.Visible,
           (unsigned long)emptyClips.Obscured);
    printf("CLIPRECT overlap total=%lu visible=%lu obscured=%lu\n",
           (unsigned long)overlapClips.Total,
           (unsigned long)overlapClips.Visible,
           (unsigned long)overlapClips.Obscured);
    PrintTrials("empty", empty, TRIAL_COUNT, rate);
    PrintTrials("overlap", overlap, TRIAL_COUNT, rate);
    PrintTrials("empty-fill", emptyFill, TRIAL_COUNT, rate);
    PrintTrials("overlap-fill", overlapFill, TRIAL_COUNT, rate);
    PrintTrials("empty-text", emptyText, TRIAL_COUNT, rate);
    PrintTrials("overlap-text", overlapText, TRIAL_COUNT, rate);
    PrintTrials("empty-bars", emptyBars, TRIAL_COUNT, rate);
    PrintTrials("overlap-bars", overlapBars, TRIAL_COUNT, rate);
    printf("RESULT empty_us=%lu overlap_us=%lu ratio_milli=%lu "
           "delta_us=%lu\n",
           (unsigned long)((emptyMedian * 1000000ULL) / rate),
           (unsigned long)((overlapMedian * 1000000ULL) / rate),
           (unsigned long)((overlapMedian * 1000ULL) / emptyMedian),
           overlapMedian >= emptyMedian ?
               (unsigned long)(((overlapMedian - emptyMedian) *
                                1000000ULL) / rate) : 0UL);
    result = RETURN_OK;

done:
    for (index = OCCLUDER_COUNT; index > 0; --index) {
        if (cover[index - 1])
            CloseWindow(cover[index - 1]);
    }
    if (target)
        CloseWindow(target);
    if (screen)
        p96CloseScreen(screen);
    if (TimerBase) {
        CloseDevice((struct IORequest *)&timerRequest);
        TimerBase = NULL;
    }
    if (LayersBase)
        CloseLibrary(LayersBase);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (P96Base)
        CloseLibrary(P96Base);
    return result;
}
