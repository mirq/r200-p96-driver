#include <devices/timer.h>
#include <dos/dos.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <graphics/displayinfo.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <inline/macros.h>
#include <intuition/intuition.h>
#include <libraries/Picasso96.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
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
#define SCREEN_WIDTH 1024
#define SCREEN_HEIGHT 768
#define TARGET_LEFT 120
#define TARGET_TOP 110
#define TARGET_WIDTH 784
#define TARGET_HEIGHT 548
#define MOVER_LEFT 160
#define MOVER_TOP 260
#define MOVER_WIDTH 320
#define MOVER_HEIGHT 220
#define MOVE_DISTANCE 400
#define TRIAL_COUNT 3
#define WARMUP_COUNT 0
#define MOVE_CYCLES 1
#define MAX_CYCLES 1
#define TEXT_ROWS 3

struct P96Mode {
    struct Node Node;
    char Description[MODENAMELENGTH];
    UWORD Width;
    UWORD Height;
    UWORD Depth;
    ULONG DisplayID;
};

struct Library *P96Base;
struct IntuitionBase *IntuitionBase;
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

static ULONG FindMode(RGBFTYPE *format)
{
    struct TagItem tags[] = {
        {P96MA_MinWidth, SCREEN_WIDTH},
        {P96MA_MinHeight, SCREEN_HEIGHT},
        {P96MA_MinDepth, 16},
        {P96MA_MaxWidth, SCREEN_WIDTH},
        {P96MA_MaxHeight, SCREEN_HEIGHT},
        {P96MA_MaxDepth, 16},
        {P96MA_FormatsAllowed, RGBFF_HICOLOR},
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
            mode->Depth == 16 && board &&
            (strcmp(board, "Radeon9200") == 0 ||
             strcmp(board, "Prometheus") == 0 ||
             strcmp(board, "Radeon") == 0 ||
             strcmp(board, "RV280-PrmPCI") == 0)) {
            result = mode->DisplayID;
            *format = (RGBFTYPE)p96GetModeIDAttr(mode->DisplayID,
                                                 P96IDA_RGBFORMAT);
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
        {WA_DragBar, TRUE},
        {WA_DepthGadget, TRUE},
        {WA_CloseGadget, TRUE},
        {WA_Activate, FALSE},
        {WA_SmartRefresh, TRUE},
        {TAG_DONE, 0}
    };

    return OpenWindowTagList(NULL, tags);
}

static void DrawWindow(struct Window *window, UBYTE basePen,
                       const char *text)
{
    struct RastPort *rp = window->RPort;
    WORD left = window->BorderLeft;
    WORD top = window->BorderTop;
    WORD right = window->Width - window->BorderRight - 1;
    WORD bottom = window->Height - window->BorderBottom - 1;
    WORD row;
    UWORD textRows = 0;

    SetDrMd(rp, JAM2);
    SetBPen(rp, 0);
    SetAPen(rp, basePen);
    RectFill(rp, left, top, right, bottom);
    SetAPen(rp, basePen + 1U);
    for (row = top + 10;
         row < bottom - 7 && textRows < TEXT_ROWS;
         row += 13, ++textRows) {
        Move(rp, left + 8 + ((row >> 2) & 7), row);
        Text(rp, (CONST_STRPTR)text, strlen(text));
    }
    SetAPen(rp, basePen + 2U);
    for (row = top; row < bottom; row += 18)
        RectFill(rp, left + ((row * 3) % 70), row, right, row + 2);
    WaitBlit();
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

static unsigned long long RunTrial(struct Window *mover, ULONG cycles)
{
    unsigned long long start;
    unsigned long long end;
    ULONG cycle;

    WaitBlit();
    start = ReadClock();
    for (cycle = 0; cycle < cycles; ++cycle) {
        MoveWindow(mover, MOVE_DISTANCE, 0);
        WaitBlit();
        MoveWindow(mover, -MOVE_DISTANCE, 0);
        WaitBlit();
    }
    end = ReadClock();
    return end - start;
}

static unsigned long long Measure(struct Window *mover, ULONG cycles,
                                  unsigned long long *values,
                                  unsigned long long *measuredTotal)
{
    unsigned long long total = 0;
    UWORD index;

#if WARMUP_COUNT > 0
    for (index = 0; index < WARMUP_COUNT; ++index)
        total += RunTrial(mover, cycles);
#endif
    *measuredTotal = 0;
    for (index = 0; index < TRIAL_COUNT; ++index) {
        values[index] = RunTrial(mover, cycles);
        *measuredTotal += values[index];
    }
    total += *measuredTotal;
    Sort(values, TRIAL_COUNT);
    return total;
}

static void PrintTrials(const char *name, unsigned long long *values,
                        ULONG rate, ULONG moves)
{
    UWORD index;
    unsigned long long median = values[TRIAL_COUNT / 2U];

    printf("TRIALS %s", name);
    for (index = 0; index < TRIAL_COUNT; ++index)
        printf(" %llu", values[index]);
    printf(" ticks median=%llu us=%lu us_per_move=%lu\n", median,
           (unsigned long)((median * 1000000ULL) / rate),
           (unsigned long)((median * 1000000ULL) / rate / moves));
}

int main(int argc, char **argv)
{
    static WORD pens[] = {-1};
    struct TagItem screenTags[] = {
        {P96SA_DisplayID, INVALID_ID},
        {P96SA_Width, SCREEN_WIDTH},
        {P96SA_Height, SCREEN_HEIGHT},
        {P96SA_Depth, 16},
        {P96SA_RGBFormat, RGBFB_R5G6B5PC},
        {P96SA_Pens, (ULONG)pens},
        {P96SA_Title, (ULONG)"P96 window movement benchmark"},
        {P96SA_ShowTitle, FALSE},
        {P96SA_Quiet, TRUE},
        {P96SA_ErrorCode, 0},
        {TAG_DONE, 0}
    };
    struct timerequest timerRequest;
    struct MsgPort timerPort;
    struct Screen *screen = NULL;
    struct Window *target = NULL;
    struct Window *mover = NULL;
    unsigned long long empty[TRIAL_COUNT];
    unsigned long long overlap[TRIAL_COUNT];
    unsigned long long emptyMedian;
    unsigned long long overlapMedian;
    unsigned long long emptyTotal;
    unsigned long long overlapTotal;
    unsigned long long emptyMeasuredTotal;
    unsigned long long overlapMeasuredTotal;
    unsigned long long benchmarkStart;
    unsigned long long benchmarkEnd;
    unsigned long long stageStart;
    unsigned long long modeTicks;
    unsigned long long screenOpenTicks;
    unsigned long long emptyOpenTicks;
    unsigned long long emptyDrawTicks;
    unsigned long long emptyCloseTicks;
    unsigned long long targetOpenTicks;
    unsigned long long targetDrawTicks;
    unsigned long long overlapOpenTicks;
    unsigned long long overlapDrawTicks;
    ULONG error = 0;
    ULONG rate = 0;
    ULONG displayId;
    RGBFTYPE format = RGBFB_NONE;
    ULONG cycles = MOVE_CYCLES;
    ULONG moves;
    int result = RETURN_FAIL;

    if (argc > 1) {
        cycles = (ULONG)atoi(argv[1]);
        if (!cycles)
            cycles = MOVE_CYCLES;
        if (cycles > MAX_CYCLES)
            cycles = MAX_CYCLES;
    }
    moves = cycles * 2UL;

    P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 39);
    if (!P96Base || !GfxBase || !IntuitionBase ||
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

    benchmarkStart = ReadClock();
    stageStart = benchmarkStart;
    displayId = FindMode(&format);
    modeTicks = ReadClock() - stageStart;
    if (displayId == (ULONG)INVALID_ID) {
        printf("ERROR 1024x768x16 Radeon mode unavailable\n");
        goto done;
    }
    screenTags[0].ti_Data = displayId;
    screenTags[4].ti_Data = format;
    screenTags[9].ti_Data = (ULONG)&error;
    stageStart = ReadClock();
    screen = p96OpenScreenTagList(screenTags);
    screenOpenTicks = ReadClock() - stageStart;
    if (!screen) {
        printf("ERROR screen open failed code=%lu\n", (unsigned long)error);
        goto done;
    }

    stageStart = ReadClock();
    mover = OpenBenchWindow(screen, MOVER_LEFT, MOVER_TOP,
                            MOVER_WIDTH, MOVER_HEIGHT, "Moving window");
    emptyOpenTicks = ReadClock() - stageStart;
    if (!mover) {
        printf("ERROR baseline mover open failed\n");
        goto done;
    }
    stageStart = ReadClock();
    DrawWindow(mover, 2, "Real Intuition window movement");
    emptyDrawTicks = ReadClock() - stageStart;
    emptyTotal = Measure(mover, cycles, empty, &emptyMeasuredTotal);
    stageStart = ReadClock();
    CloseWindow(mover);
    emptyCloseTicks = ReadClock() - stageStart;
    mover = NULL;

    stageStart = ReadClock();
    target = OpenBenchWindow(screen, TARGET_LEFT, TARGET_TOP,
                             TARGET_WIDTH, TARGET_HEIGHT, "Target window");
    targetOpenTicks = ReadClock() - stageStart;
    if (!target) {
        printf("ERROR target window open failed\n");
        goto done;
    }
    stageStart = ReadClock();
    DrawWindow(target, 5, "Picasso96 target under moving window 0123456789");
    targetDrawTicks = ReadClock() - stageStart;
    stageStart = ReadClock();
    mover = OpenBenchWindow(screen, MOVER_LEFT, MOVER_TOP,
                            MOVER_WIDTH, MOVER_HEIGHT, "Moving window");
    overlapOpenTicks = ReadClock() - stageStart;
    if (!mover) {
        printf("ERROR overlap mover open failed\n");
        goto done;
    }
    stageStart = ReadClock();
    DrawWindow(mover, 2, "Real Intuition window movement");
    overlapDrawTicks = ReadClock() - stageStart;
    overlapTotal = Measure(mover, cycles, overlap, &overlapMeasuredTotal);
    benchmarkEnd = ReadClock();

    emptyMedian = empty[TRIAL_COUNT / 2U];
    overlapMedian = overlap[TRIAL_COUNT / 2U];
    printf("P96WINDOWMOVE version=7 mode=1024x768x16 format=%lu "
           "cycles=%lu moves=%lu distance=%u trials=%u eclock=%lu\n",
           (unsigned long)format, (unsigned long)cycles,
           (unsigned long)moves, MOVE_DISTANCE, TRIAL_COUNT,
           (unsigned long)rate);
    printf("WINDOW target=%ux%u@%u,%u mover=%ux%u@%u,%u->%u,%u "
           "refresh=smart borders=normal text_rows=%u\n",
           TARGET_WIDTH, TARGET_HEIGHT,
           TARGET_LEFT, TARGET_TOP, MOVER_WIDTH, MOVER_HEIGHT, MOVER_LEFT,
           MOVER_TOP, MOVER_LEFT + MOVE_DISTANCE, MOVER_TOP, TEXT_ROWS);
    PrintTrials("empty", empty, rate, moves);
    PrintTrials("overlap", overlap, rate, moves);
    printf("TOTAL whole_us=%lu empty_us=%lu overlap_us=%lu "
           "empty_measured_us=%lu overlap_measured_us=%lu\n",
           (unsigned long)(((benchmarkEnd - benchmarkStart) * 1000000ULL) /
                           rate),
           (unsigned long)((emptyTotal * 1000000ULL) / rate),
           (unsigned long)((overlapTotal * 1000000ULL) / rate),
           (unsigned long)((emptyMeasuredTotal * 1000000ULL) / rate),
           (unsigned long)((overlapMeasuredTotal * 1000000ULL) / rate));
    printf("AVERAGE empty_loop_us=%lu overlap_loop_us=%lu "
           "empty_trial_us=%lu overlap_trial_us=%lu\n",
           (unsigned long)((emptyTotal * 1000000ULL) / rate /
                           ((WARMUP_COUNT + TRIAL_COUNT) * cycles)),
           (unsigned long)((overlapTotal * 1000000ULL) / rate /
                           ((WARMUP_COUNT + TRIAL_COUNT) * cycles)),
           (unsigned long)((emptyMeasuredTotal * 1000000ULL) / rate /
                           TRIAL_COUNT),
           (unsigned long)((overlapMeasuredTotal * 1000000ULL) / rate /
                           TRIAL_COUNT));
    printf("STAGES mode_us=%lu screen_open_us=%lu empty_open_us=%lu "
           "empty_draw_us=%lu empty_close_us=%lu target_open_us=%lu "
           "target_draw_us=%lu overlap_open_us=%lu overlap_draw_us=%lu\n",
           (unsigned long)((modeTicks * 1000000ULL) / rate),
           (unsigned long)((screenOpenTicks * 1000000ULL) / rate),
           (unsigned long)((emptyOpenTicks * 1000000ULL) / rate),
           (unsigned long)((emptyDrawTicks * 1000000ULL) / rate),
           (unsigned long)((emptyCloseTicks * 1000000ULL) / rate),
           (unsigned long)((targetOpenTicks * 1000000ULL) / rate),
           (unsigned long)((targetDrawTicks * 1000000ULL) / rate),
           (unsigned long)((overlapOpenTicks * 1000000ULL) / rate),
           (unsigned long)((overlapDrawTicks * 1000000ULL) / rate));
    printf("RESULT empty_us=%lu overlap_us=%lu ratio_milli=%lu "
           "delta_us=%lu empty_us_per_move=%lu overlap_us_per_move=%lu\n",
           (unsigned long)((emptyMedian * 1000000ULL) / rate),
           (unsigned long)((overlapMedian * 1000000ULL) / rate),
           (unsigned long)((overlapMedian * 1000ULL) / emptyMedian),
           overlapMedian >= emptyMedian ?
               (unsigned long)(((overlapMedian - emptyMedian) *
                                1000000ULL) / rate) : 0UL,
           (unsigned long)((emptyMedian * 1000000ULL) / rate / moves),
           (unsigned long)((overlapMedian * 1000000ULL) / rate / moves));
    result = RETURN_OK;

done:
    if (mover)
        CloseWindow(mover);
    if (target)
        CloseWindow(target);
    if (screen)
        p96CloseScreen(screen);
    if (TimerBase) {
        CloseDevice((struct IORequest *)&timerRequest);
        TimerBase = NULL;
    }
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (P96Base)
        CloseLibrary(P96Base);
    return result;
}
