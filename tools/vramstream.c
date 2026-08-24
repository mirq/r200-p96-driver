#include <devices/timer.h>
#include <dos/dos.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <graphics/gfx.h>
#include <inline/macros.h>
#include <intuition/screens.h>
#include <libraries/Picasso96.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/timer.h>
#include <stdio.h>

#define P96MA_Dummy            (TAG_USER + 0x10000UL + 96UL)
#define P96MA_MinWidth         (P96MA_Dummy + 0x0001UL)
#define P96MA_MinHeight        (P96MA_Dummy + 0x0002UL)
#define P96MA_MinDepth         (P96MA_Dummy + 0x0003UL)
#define P96MA_MaxWidth         (P96MA_Dummy + 0x0004UL)
#define P96MA_MaxHeight        (P96MA_Dummy + 0x0005UL)
#define P96MA_MaxDepth         (P96MA_Dummy + 0x0006UL)

#define P96SA_Dummy            (TAG_USER + 0x20000UL + 96UL)
#define P96SA_Width            (P96SA_Dummy + 0x0003UL)
#define P96SA_Height           (P96SA_Dummy + 0x0004UL)
#define P96SA_Depth            (P96SA_Dummy + 0x0005UL)
#define P96SA_Title            (P96SA_Dummy + 0x0008UL)
#define P96SA_ErrorCode        (P96SA_Dummy + 0x000aUL)
#define P96SA_DisplayID        (P96SA_Dummy + 0x0012UL)
#define P96SA_ShowTitle        (P96SA_Dummy + 0x0014UL)
#define P96SA_Pens             (P96SA_Dummy + 0x0018UL)

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
    LP1NR(0x60, p96CloseScreen, struct Screen *, screen, a0, , P96Base)
#define p96GetBitMapAttr(bitmap, attribute) \
    LP2(0x2a, ULONG, p96GetBitMapAttr, struct BitMap *, bitmap, a0, \
        ULONG, attribute, d0, , P96Base)

#define BLOCK_WORDS_8K   2048UL
#define BLOCK_WORDS_64K  16384UL
#define BLOCK_WORDS_256K 65536UL
#define BLOCK_WORDS_1M   262144UL

typedef void (*CaseFunction)(volatile ULONG *destination,
                             const ULONG *source, ULONG words, ULONG salt);

struct Device *TimerBase;
static struct timerequest TimerRequest;
static struct MsgPort TimerPort;
static ULONG ClockRate;

static BOOL OpenTimer(void)
{
    TimerRequest.tr_node.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    TimerRequest.tr_node.io_Message.mn_ReplyPort = &TimerPort;
    TimerRequest.tr_node.io_Message.mn_Length = sizeof(TimerRequest);
    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_ECLOCK,
                   (struct IORequest *)&TimerRequest, 0))
        return FALSE;
    TimerBase = TimerRequest.tr_node.io_Device;
    return TRUE;
}

static ULONG NowTicks(void)
{
    struct EClockVal value;

    ReadEClock(&value);
    return value.ev_lo;
}

static ULONG EClockRate(void)
{
    struct EClockVal value;

    return ReadEClock(&value);
}

static void StoreSequential(volatile ULONG *destination,
                            const ULONG *source, ULONG words, ULONG salt)
{
    ULONG index;

    (void)source;
    for (index = 0; index < words; ++index)
        destination[index] = index ^ salt;
}

static void StoreByteSwapped(volatile ULONG *destination,
                             const ULONG *source, ULONG words, ULONG salt)
{
    ULONG index;

    (void)source;
    for (index = 0; index < words; ++index)
        destination[index] = __builtin_bswap32(index ^ salt);
}

static void CopyFromChip(volatile ULONG *destination,
                         const ULONG *source, ULONG words, ULONG salt)
{
    ULONG index;

    (void)salt;
    for (index = 0; index < words; ++index)
        destination[index] = source[index];
}

static void RunCase(const char *name, CaseFunction function,
                    volatile ULONG *destination, const ULONG *source,
                    ULONG words, ULONG maximumPasses)
{
    ULONG salt = 1UL;
    ULONG passes;
    ULONG start;
    ULONG end;
    volatile ULONG drain = 0;
    unsigned long long elapsedTicks;
    unsigned long long microseconds;
    unsigned long long bytes;
    unsigned long long kilobytesPerSecond;
    ULONG pass;

    start = NowTicks();
    function(destination, source, words, salt++);
    drain ^= destination[words - 1UL];
    end = NowTicks();

    if (end > start && maximumPasses > 1UL) {
        unsigned long long singlePassTicks = end - start;
        unsigned long long budgetTicks = ClockRate;

        passes = (ULONG)(budgetTicks / (singlePassTicks ? singlePassTicks : 1U));
        if (passes < 2UL)
            passes = 2UL;
        if (passes > maximumPasses)
            passes = maximumPasses;
    } else {
        passes = maximumPasses;
    }

    start = NowTicks();
    for (pass = 0; pass < passes; ++pass)
        function(destination, source, words, salt++);
    drain ^= destination[words - 1UL];
    end = NowTicks();

    elapsedTicks = (end >= start) ? (unsigned long long)(end - start) : 1ULL;
    microseconds = elapsedTicks * 1000000ULL / ClockRate;
    if (!microseconds)
        microseconds = 1ULL;
    bytes = (unsigned long long)passes * words * sizeof(ULONG);
    kilobytesPerSecond = bytes * 1000000ULL / (microseconds * 1024ULL);

    printf("VRAMSTREAM case=%s words=%lu passes=%llu us=%llu kbps=%llu "
           "drain=%08lx\n",
           name, (unsigned long)words, (unsigned long long)passes,
           (unsigned long long)microseconds,
           (unsigned long long)kilobytesPerSecond,
           (unsigned long)drain);
}

int main(void)
{
    static WORD pens[] = {-1};
    struct TagItem modeTags[] = {
        {P96MA_MinWidth, 640},
        {P96MA_MinHeight, 480},
        {P96MA_MinDepth, 16},
        {P96MA_MaxWidth, 640},
        {P96MA_MaxHeight, 480},
        {P96MA_MaxDepth, 16},
        {TAG_DONE, 0}
    };
    struct TagItem screenTags[10];
    struct List *modes;
    struct P96Mode *mode;
    struct Screen *screen = NULL;
    struct BitMap *scratchBitmap = NULL;
    volatile ULONG *vramMemory;
    ULONG *chipMemory = NULL;
    ULONG displayId = INVALID_ID;
    ULONG errorCode = 0;
    ULONG memoryPointer;
    ULONG screenMemory;
    ULONG startTicks;
    ULONG endTicks;
    int result = RETURN_FAIL;

    P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library",
                                            39);
    if (!P96Base || !GfxBase) {
        printf("VRAMSTREAM status=open_library_failed\n");
        goto done;
    }
    if (!OpenTimer()) {
        printf("VRAMSTREAM status=timer_failed\n");
        goto done;
    }
    ClockRate = EClockRate();
    if (!ClockRate) {
        printf("VRAMSTREAM status=eclock_failed\n");
        goto done;
    }
    startTicks = NowTicks();
    Delay(50UL);
    endTicks = NowTicks() - startTicks;
    printf("VRAMSTREAM eclock_reported=%lu delay50_ticks=%lu "
           "delay50_implied=%lu\n",
           (unsigned long)ClockRate, (unsigned long)endTicks,
           (unsigned long)endTicks);

    modes = p96AllocModeListTagList(modeTags);
    if (!modes) {
        printf("VRAMSTREAM status=modelist_failed\n");
        goto done;
    }
    for (mode = (struct P96Mode *)modes->lh_Head;
         mode->Node.ln_Succ;
         mode = (struct P96Mode *)mode->Node.ln_Succ) {
        char *board = (char *)p96GetModeIDAttr(mode->DisplayID,
                                               P96IDA_BOARDNAME);

        if (mode->Width == 640 && mode->Height == 480 &&
            mode->Depth == 16 && board &&
            (strcmp(board, "Radeon9200") == 0 ||
             strcmp(board, "Prometheus") == 0))
            displayId = mode->DisplayID;
    }
    p96FreeModeList(modes);
    if (displayId == (ULONG)INVALID_ID) {
        printf("VRAMSTREAM status=no_mode\n");
        goto done;
    }

    screenTags[0].ti_Tag = P96SA_DisplayID;
    screenTags[0].ti_Data = displayId;
    screenTags[1].ti_Tag = P96SA_Width;
    screenTags[1].ti_Data = 640;
    screenTags[2].ti_Tag = P96SA_Height;
    screenTags[2].ti_Data = 480;
    screenTags[3].ti_Tag = P96SA_Depth;
    screenTags[3].ti_Data = 16;
    screenTags[4].ti_Tag = P96SA_Pens;
    screenTags[4].ti_Data = (ULONG)pens;
    screenTags[5].ti_Tag = P96SA_Title;
    screenTags[5].ti_Data = (ULONG)"vramstream";
    screenTags[6].ti_Tag = P96SA_ShowTitle;
    screenTags[6].ti_Data = FALSE;
    screenTags[7].ti_Tag = P96SA_ErrorCode;
    screenTags[7].ti_Data = (ULONG)&errorCode;
    screenTags[8].ti_Tag = TAG_DONE;
    screenTags[8].ti_Data = 0;

    screen = p96OpenScreenTagList(screenTags);
    if (!screen) {
        printf("VRAMSTREAM status=openscreen_failed error=%lu\n",
               (unsigned long)errorCode);
        goto done;
    }

    screenMemory = p96GetBitMapAttr(screen->RastPort.BitMap, P96BMA_MEMORY);
    printf("VRAMSTREAM screen memory=%08lx bpr=%lu\n",
           (unsigned long)screenMemory,
           (unsigned long)p96GetBitMapAttr(screen->RastPort.BitMap,
                                           P96BMA_BYTESPERROW));

    /* 640x832x16bpp covers the 1 MiB block with margin; an offscreen bitmap
     * is never scanned out, so trashing it is harmless. */
    scratchBitmap = AllocBitMap(640, 832, 16,
                                BMF_MINPLANES | BMF_CLEAR,
                                screen->RastPort.BitMap);
    if (!scratchBitmap) {
        printf("VRAMSTREAM status=scratch_alloc_failed\n");
        goto done;
    }
    memoryPointer = p96GetBitMapAttr(scratchBitmap, P96BMA_MEMORY);
    printf("VRAMSTREAM scratch memory=%08lx onboard=%lu bpr=%lu\n",
           (unsigned long)memoryPointer,
           (unsigned long)p96GetBitMapAttr(scratchBitmap, P96BMA_ISONBOARD),
           (unsigned long)p96GetBitMapAttr(scratchBitmap, P96BMA_BYTESPERROW));
    if (!p96GetBitMapAttr(scratchBitmap, P96BMA_ISONBOARD) ||
        memoryPointer == screenMemory) {
        printf("VRAMSTREAM status=scratch_not_vram\n");
        goto done;
    }
    vramMemory = (volatile ULONG *)memoryPointer;

    chipMemory = AllocMem(BLOCK_WORDS_1M * sizeof(ULONG), MEMF_PUBLIC);
    if (!chipMemory) {
        printf("VRAMSTREAM status=alloc_failed\n");
        goto done;
    }
    {
        ULONG index;

        for (index = 0; index < BLOCK_WORDS_1M; ++index)
            chipMemory[index] = index ^ 0x5a5aa5a5UL;
    }

    RunCase("fastmem_store_256k", StoreSequential, chipMemory, NULL,
            BLOCK_WORDS_256K, 128UL);
    RunCase("fastmem_copy_256k", CopyFromChip, chipMemory, chipMemory,
            BLOCK_WORDS_256K, 128UL);
    RunCase("vram_store_8k", StoreSequential, vramMemory, NULL,
            BLOCK_WORDS_8K, 2048UL);
    RunCase("vram_store_64k", StoreSequential, vramMemory, NULL,
            BLOCK_WORDS_64K, 256UL);
    RunCase("vram_store_256k", StoreSequential, vramMemory, NULL,
            BLOCK_WORDS_256K, 64UL);
    RunCase("vram_store_1m", StoreSequential, vramMemory, NULL,
            BLOCK_WORDS_1M, 16UL);
    RunCase("vram_swap_256k", StoreByteSwapped, vramMemory, NULL,
            BLOCK_WORDS_256K, 64UL);
    RunCase("vram_swap_1m", StoreByteSwapped, vramMemory, NULL,
            BLOCK_WORDS_1M, 16UL);
    RunCase("vram_copy_256k", CopyFromChip, vramMemory, chipMemory,
            BLOCK_WORDS_256K, 64UL);
    RunCase("vram_copy_1m", CopyFromChip, vramMemory, chipMemory,
            BLOCK_WORDS_1M, 16UL);

    printf("VRAMSTREAM status=ok\n");
    result = RETURN_OK;

done:
    if (chipMemory)
        FreeMem(chipMemory, BLOCK_WORDS_1M * sizeof(ULONG));
    if (scratchBitmap)
        FreeBitMap(scratchBitmap);
    if (screen)
        p96CloseScreen(screen);
    if (TimerBase)
        CloseDevice((struct IORequest *)&TimerRequest);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (P96Base)
        CloseLibrary(P96Base);
    return result;
}
