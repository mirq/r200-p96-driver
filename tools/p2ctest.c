/*
 * p2ctest - planar-to-chunky correctness and timing check for the Radeon9200
 * Picasso96 driver (BlitPlanar2Chunky / BlitPlanar2Direct).
 *
 * Every case blits a random planar bitmap (depth 1-8, every mask class, NULL
 * and all-ones planes, unaligned source offsets and widths) into a window on
 * an RTG screen and compares each pixel with a reference produced by
 * graphics.library into a planar bitmap (CLUT screens) or by a pen-to-pixel
 * table (direct-colour screens). Output goes to stdout, one line per case;
 * the last line is "P2CTEST cases=N failures=M ok|FAIL" (exit 10 on FAIL).
 *
 * Usage: p2ctest [mode] [w|b]
 *   (none)  front screen, then 640x480 8-bit and 16-bit screens
 *   f       front screen only;  e  the 640x480 8-bit and 16-bit screens only
 *   s       the historical suspect sequence (four cases)
 *   x       pair experiments (a 1x1 blit then a 32x5 blit one pixel right)
 *   y       the same pair through RectFill / BltTemplate / planar mixes
 *   z       the aperture read-line recipe through RectFill and planar, with
 *           CPU cache flushes and a dummy aperture read before the readback
 *   w       WaitBlit() before every readback;  b  WaitBlit() before each fill
 *
 * Run it detached with a large stack when driven over the bridge:
 *   stack 262144
 *   run >NIL: p2ctest >T:p2c.log
 */
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

#include <exec/memory.h>
#include <devices/timer.h>
#include <intuition/intuition.h>
#include <proto/intuition.h>
#include <proto/timer.h>

struct Device *TimerBase;
struct IntuitionBase *IntuitionBase;
static ULONG us(void) { struct timeval t; GetSysTime(&t); return t.tv_secs * 1000000UL + t.tv_micro; }
static ULONG seed = 0x1234567;
static ULONG rnd(void) { seed = seed * 1103515245UL + 12345UL; return seed >> 8; }

#define SW 352   /* source bitmap width (bits) */
#define SH 70
#define AW 340   /* test area */
#define AH 66

static ULONG failures, cases;
static ULONG pen2pixel[256];
static UBYTE bgpen[AW * AH];
static BOOL waitBlit, verbose, waitBefore;
static ULONG skipFlags; /* 1=no bg fill, 2=no reference blit, 4=no readback, 8=read twice */

static ULONG PlaneBit(const struct BitMap *bm, ULONG plane, ULONG x, ULONG y)
{
    PLANEPTR p = bm->Planes[plane];
    if (!p) return 0;
    if (p == (PLANEPTR)~0UL) return 1;
    return (p[y * bm->BytesPerRow + (x >> 3)] >> (7 - (x & 7))) & 1;
}

static void FillRandom(struct BitMap *bm, ULONG planeMask)
{
    ULONG p, i;
    for (p = 0; p < bm->Depth; p++) {
        PLANEPTR pl = bm->Planes[p];
        if (!pl || pl == (PLANEPTR)~0UL || !(planeMask & (1 << p))) continue;
        for (i = 0; i < (ULONG)bm->BytesPerRow * bm->Rows; i++) pl[i] = (UBYTE)(rnd() >> 3);
    }
}

/* one case: blit src (depth d) at sx,sy size w,h to the screen at ax,ay with mask; verify */
static void RunCase(struct Screen *screen, struct RastPort *refrp, struct BitMap *src,
                    LONG ax, LONG ay, LONG sx, LONG sy, LONG w, LONG h, UBYTE mask, ULONG depth, BOOL clut)
{
    struct RastPort *srp = &screen->RastPort;
    LONG x, y;
    ULONG bad = 0, first = 0xffffffff, firstExp = 0, firstGot = 0, t;
    cases++;
    printf("case %lu d%lu sx%ld w%ld h%ld m%02lx [bg]", (unsigned long)cases, (unsigned long)depth, (long)sx, (long)w, (long)h, (unsigned long)mask);
    /* background: random pens in the area, both on screen and in the planar reference */
    if (waitBefore) WaitBlit();
    for (y = 0; y < h && !(skipFlags & 1); y++)
        for (x = 0; x < w; x++) {
            ULONG pen = rnd() & 0xff;
            bgpen[y * AW + x] = (UBYTE)pen;
            SetAPen(srp, pen); WritePixel(srp, ax + x, ay + y);
            SetAPen(refrp, pen); WritePixel(refrp, x, y);
        }
    printf("[blit]");
    t = us();
    BltBitMap(src, sx, sy, srp->BitMap, ax, ay, w, h, 0xC0, mask, NULL);
    t = us() - t;
    printf("[ref]");
    if (!(skipFlags & 2)) BltBitMap(src, sx, sy, refrp->BitMap, 0, 0, w, h, 0xC0, mask, NULL);
    if (waitBlit) WaitBlit();
    printf("[read]");
    if (skipFlags & 4) { printf("skipped\n"); return; }
    if (skipFlags & 8) { ULONG g1 = p96ReadPixel(srp, ax, ay); Delay(2); printf("(x0y0 first read %08lx, again %08lx) ", (unsigned long)g1, (unsigned long)p96ReadPixel(srp, ax, ay)); }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            ULONG got = p96ReadPixel(srp, ax + x, ay + y);
            ULONG exp;
            if (clut) {
                /* p96ReadPixel on a CLUT screen returns the high byte of x
                 * in bits 8-15 (0x1nn for x >= 256); only the pen matters */
                got &= 0xffUL;
                exp = ReadPixel(refrp, x, y);
            } else {
                ULONG idx = 0, p;
                for (p = 0; p < src->Depth; p++)
                    if (mask & (1 << p)) idx |= PlaneBit(src, p, sx + x, sy + y) << p;
                /* no plane selected: BltBitMap leaves the destination alone */
                exp = (mask & ((1U << src->Depth) - 1U)) ? pen2pixel[idx] : pen2pixel[bgpen[y * AW + x]];
            }
            if (got != exp) { if (!bad) { first = y * 100000UL + x; firstExp = exp; firstGot = got; } bad++; }
        }
    if (bad) {
        failures++;
        printf("FAIL depth=%lu sx=%ld sy=%ld w=%ld h=%ld ax=%ld mask=%02lx: %lu bad px, first at y=%lu x=%lu exp=%08lx got=%08lx\n",
               (unsigned long)depth, (long)sx, (long)sy, (long)w, (long)h, (long)ax, (unsigned long)mask, (unsigned long)bad,
               (unsigned long)(first / 100000UL), (unsigned long)(first % 100000UL), (unsigned long)firstExp, (unsigned long)firstGot);
    } else {
        printf("ok   depth=%lu sx=%2ld w=%3ld h=%2ld ax=%ld mask=%02lx  %5lu us\n", (unsigned long)depth, (long)sx, (long)w, (long)h, (long)ax, (unsigned long)mask, (unsigned long)t);
    }
}

static void TestScreen(struct Screen *screen, struct Window *w, BOOL clut)
{
    struct RastPort *srp = &screen->RastPort;
    struct BitMap *ref = AllocBitMap(AW + 32, AH + 8, 8, BMF_CLEAR, NULL);
    struct RastPort refrp;
    LONG ax = w->LeftEdge + 4, ay = w->TopEdge + 14;
    static const ULONG depths[] = { 1, 2, 3, 4, 5, 8 };
    static const LONG geo[][3] = { {0, 330, 64}, {3, 100, 10}, {7, 1, 1}, {8, 32, 5}, {13, 33, 3}, {17, 31, 64}, {31, 200, 7}, {5, 8, 64} };
    ULONG d, g, m, i;

    InitRastPort(&refrp); refrp.BitMap = ref;
    /* pen -> pixel table through the screen's own conversion */
    for (i = 0; i < 256; i++) { SetAPen(srp, i); WritePixel(srp, ax, ay); pen2pixel[i] = p96ReadPixel(srp, ax, ay); }

    for (d = 0; d < 6; d++) {
        struct BitMap *src = AllocBitMap(SW, SH, depths[d], BMF_CLEAR, NULL);
        UBYTE depthMask = (UBYTE)((1U << depths[d]) - 1U);
        static const UBYTE masks[] = { 0xff, 0x01, 0x05, 0xfe, 0x00 };
        FillRandom(src, 0xff);
        for (g = 0; g < 8; g++)
            for (m = 0; m < 3; m++)
                RunCase(screen, &refrp, src, ax + (g & 1), ay, geo[g][0], g & 3, geo[g][1], geo[g][2], masks[m], depths[d], clut);
        RunCase(screen, &refrp, src, ax, ay, 2, 1, 150, 20, 0xfe, depths[d], clut);
        RunCase(screen, &refrp, src, ax, ay, 2, 1, 150, 20, 0x00, depths[d], clut);
        if (depths[d] >= 3) {
            PLANEPTR save1 = src->Planes[1], save2 = src->Planes[2];
            src->Planes[1] = NULL; src->Planes[2] = (PLANEPTR)~0UL;
            RunCase(screen, &refrp, src, ax, ay, 9, 2, 120, 30, 0xff, depths[d], clut);
            RunCase(screen, &refrp, src, ax, ay, 9, 2, 120, 30, 0x06, depthMask, clut);
            src->Planes[1] = save1; src->Planes[2] = save2;
        }
        (void)depthMask;
        FreeBitMap(src);
    }
    /* throughput as HippoPlayer does it: through the window's layered rastport */
    for (d = 0; d < 6; d++) {
        struct BitMap *src = AllocBitMap(SW, SH, depths[d], BMF_CLEAR, NULL);
        ULONG t;
        FillRandom(src, 0xff);
        t = us();
        for (i = 0; i < 20; i++) BltBitMapRastPort(src, 0, 0, w->RPort, 4, 14, 330, 64, 0xC0);
        t = us() - t;
        printf("BltBitMapRastPort 330x64 depth %lu: %lu us/op\n", (unsigned long)depths[d], (unsigned long)(t / 20));
        FreeBitMap(src);
    }
    FreeBitMap(ref);
}

int main(int argc, char **argv)
{
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *req = (struct timerequest *)CreateIORequest(port, sizeof(*req));
    static UWORD pens[] = {(UWORD)~0};
    ULONG depths[] = {8, 16};
    ULONG d;
    struct Window *w;
    struct Screen *front;
    BOOL frontOnly = argc > 1 && argv[1][0] == 'f';
    BOOL extraOnly = argc > 1 && argv[1][0] == 'e';
    waitBlit = argc > 2 && argv[2][0] == 'w';
    waitBefore = argc > 2 && argv[2][0] == 'b';
    verbose = argc > 1 && argv[1][0] == 's';
    setvbuf(stdout, NULL, _IONBF, 0);

    OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK, (struct IORequest *)req, 0);
    TimerBase = req->tr_node.io_Device;
    P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    if (!P96Base || !GfxBase || !IntuitionBase) { printf("libs\n"); return 20; }

    front = LockPubScreen(NULL);
    w = extraOnly ? NULL : OpenWindowTags(NULL, WA_PubScreen, (ULONG)front, WA_Left, 600, WA_Top, 640, WA_Width, AW + 10, WA_Height, AH + 20,
                       WA_Title, (ULONG)"p2ctest", WA_Flags, WFLG_SIMPLE_REFRESH | WFLG_NOCAREREFRESH | WFLG_DRAGBAR,
                       WA_Activate, TRUE, TAG_DONE);
    if (w) {
        ULONG depth = GetBitMapAttr(w->RPort->BitMap, BMA_DEPTH);
        printf("== front screen depth %lu ==\n", (unsigned long)depth);
        if (argc > 1 && argv[1][0] == 'y') {
            /* same pair through other engine paths: A = 1x1 at ax, B = 32x5 at ax+1 */
            struct RastPort *srp = &front->RastPort;
            LONG ax = w->LeftEdge + 4, ay = w->TopEdge + 14;
            ULONG i, x, y, bad, kind;
            static UWORD tmpl[8];
            for (i = 0; i < 256; i++) { SetAPen(srp, i); WritePixel(srp, ax, ay); pen2pixel[i] = p96ReadPixel(srp, ax, ay); }
            for (i = 0; i < 8; i++) tmpl[i] = (UWORD)rnd();
            {
            struct BitMap *pl = AllocBitMap(64, 8, 1, BMF_CLEAR, NULL);
            ULONG r;
            for (r = 0; r < 8; r++) { ((UWORD *)pl->Planes[0])[r * (pl->BytesPerRow / 2)] = tmpl[r]; ((UWORD *)pl->Planes[0])[r * (pl->BytesPerRow / 2) + 1] = tmpl[(r + 1) & 7]; }
            for (kind = 0; kind < 6; kind++) {
                for (y = 0; y < 5; y++) for (x = 0; x < 40; x++) { SetAPen(srp, 7); WritePixel(srp, ax + x, ay + y); }
                if (kind == 3) {
                    BltBitMap(pl, 0, 0, srp->BitMap, ax, ay, 1, 1, 0xC0, 0xff, NULL);
                    SetAPen(srp, 2); SetBPen(srp, 4); SetDrMd(srp, JAM2);
                    BltTemplate((PLANEPTR)tmpl, 0, 2, srp, ax + 1, ay, 32, 5);
                } else if (kind == 4) {
                    SetAPen(srp, 1); SetBPen(srp, 3); SetDrMd(srp, JAM2);
                    BltTemplate((PLANEPTR)tmpl, 0, 2, srp, ax, ay, 1, 1);
                    BltBitMap(pl, 0, 0, srp->BitMap, ax + 1, ay, 32, 5, 0xC0, 0xff, NULL);
                } else if (kind == 5) {
                    BltBitMap(pl, 0, 0, srp->BitMap, ax, ay, 1, 1, 0xC0, 0xff, NULL);
                    BltBitMap(pl, 0, 0, srp->BitMap, ax + 1, ay, 32, 5, 0xC0, 0xff, NULL);
                } else if (kind == 0) {
                    SetAPen(srp, 1); RectFill(srp, ax, ay, ax, ay);
                    SetAPen(srp, 2); RectFill(srp, ax + 1, ay, ax + 32, ay + 4);
                } else if (kind == 1) {
                    SetAPen(srp, 1); SetBPen(srp, 3); SetDrMd(srp, JAM2);
                    BltTemplate((PLANEPTR)tmpl, 0, 2, srp, ax, ay, 1, 1);
                    SetAPen(srp, 2); SetBPen(srp, 4);
                    BltTemplate((PLANEPTR)tmpl, 0, 2, srp, ax + 1, ay, 32, 5);
                } else {
                    SetAPen(srp, 1); RectFill(srp, ax, ay, ax, ay);
                    SetAPen(srp, 2); SetBPen(srp, 4); SetDrMd(srp, JAM2);
                    BltTemplate((PLANEPTR)tmpl, 0, 2, srp, ax + 1, ay, 32, 5);
                }
                bad = 0;
                for (y = 0; y < 5; y++) for (x = 0; x < 32; x++) {
                    ULONG got = p96ReadPixel(srp, ax + 1 + x, ay + y), exp;
                    if (kind == 0) exp = pen2pixel[2];
                    else if (kind >= 4) exp = pen2pixel[((tmpl[(y + x / 16) & 7] >> (15 - x % 16)) & 1)];
                    else exp = ((tmpl[(y + x / 16) & 7] >> (15 - x % 16)) & 1) ? pen2pixel[2] : pen2pixel[4];
                    if (got != exp) { if (bad < 4) printf("  bad y=%lu x=%lu exp=%08lx got=%08lx\n", (unsigned long)y, (unsigned long)x, (unsigned long)exp, (unsigned long)got); bad++; }
                }
                printf("%s: %lu bad\n", kind == 0 ? "RectFill 1x1 then RectFill 32x5" : kind == 1 ? "BltTemplate 1x1 then BltTemplate 32x5" : kind == 2 ? "RectFill 1x1 then BltTemplate 32x5" : kind == 3 ? "planar 1x1 then BltTemplate 32x5" : kind == 4 ? "BltTemplate 1x1 then planar 32x5" : "planar 1x1 then planar 32x5", (unsigned long)bad);
            }
            FreeBitMap(pl);
            }
            CloseWindow(w); UnlockPubScreen(NULL, front);
            return 0;
        }
        if (argc > 1 && argv[1][0] == 'z') {
            /* the failing recipe through each engine path:
             * A 1x1 at ax -> read A back -> random bg via WritePixel over B's rect -> B 32x5 at ax+1 -> read B back */
            struct RastPort *srp = &front->RastPort;
            LONG ax = w->LeftEdge + 4, ay = w->TopEdge + 14;
            ULONG i, x, y, bad, kind, readA, rep;
            static UWORD ones[8] = { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff };
            struct BitMap *pl = AllocBitMap(64, 8, 1, BMF_CLEAR, NULL);
            for (i = 0; i < 256; i++) { SetAPen(srp, i); WritePixel(srp, ax, ay); pen2pixel[i] = p96ReadPixel(srp, ax, ay); }
            memset(pl->Planes[0], 0xff, pl->BytesPerRow * pl->Rows);
            for (rep = 0; rep < 4; rep++)
            for (kind = 0; kind < 3; kind += 2) for (readA = 1; readA < 2; readA++) {
                ULONG got0, bgcol[3];
                if (kind == 0) { SetAPen(srp, 1); RectFill(srp, ax, ay, ax, ay); }
                else if (kind == 1) { SetAPen(srp, 1); SetBPen(srp, 3); SetDrMd(srp, JAM2); BltTemplate((PLANEPTR)ones, 0, 2, srp, ax, ay, 1, 1); }
                else BltBitMap(pl, 0, 0, srp->BitMap, ax, ay, 1, 1, 0xC0, 0xff, NULL);
                got0 = readA ? p96ReadPixel(srp, ax, ay) : 0;
                for (y = 0; y < 5; y++) for (x = 0; x < 32; x++) { ULONG pen = rnd() & 0xff; if (y == 0 && x < 3) bgcol[x] = pen2pixel[pen]; SetAPen(srp, pen); WritePixel(srp, ax + 1 + x, ay + y); }
                if (kind == 0) { SetAPen(srp, 2); RectFill(srp, ax + 1, ay, ax + 32, ay + 4); }
                else if (kind == 1) { SetAPen(srp, 2); SetBPen(srp, 4); SetDrMd(srp, JAM2); BltTemplate((PLANEPTR)ones, 0, 2, srp, ax + 1, ay, 32, 5); }
                else BltBitMap(pl, 0, 0, srp->BitMap, ax + 1, ay, 32, 5, 0xC0, 0xff, NULL);
                bad = 0;
                if (rep == 1) CacheClearU();
                if (rep == 2) CacheClearE((APTR)0x0, 0xffffffff, CACRF_ClearD);
                if (rep == 3) { volatile UBYTE *fb = (volatile UBYTE *)p96GetBitMapAttr(srp->BitMap, P96BMA_MEMORY); if (fb) (void)fb[0]; }
                for (y = 0; y < 5; y++) for (x = 0; x < 32; x++) {
                    ULONG got = p96ReadPixel(srp, ax + 1 + x, ay + y);
                    ULONG exp = pen2pixel[kind == 2 ? 1 : 2];
                    if (got != exp) { if (bad < 3) printf("  bad y=%lu x=%lu exp=%08lx got=%08lx%s\n", (unsigned long)y, (unsigned long)x, (unsigned long)exp, (unsigned long)got, (y == 0 && x < 3 && got == bgcol[x]) ? " (=bg)" : ""); bad++; }
                }
                printf("%s %s: %lu bad (A read %08lx)\n", kind == 0 ? "RectFill" : kind == 1 ? "BltTemplate" : "planar", rep == 0 ? "plain" : rep == 1 ? "CacheClearU before read" : rep == 2 ? "CacheClearE D before read" : "touch fb[0] before read", (unsigned long)bad, (unsigned long)got0);
            }
            FreeBitMap(pl);
            CloseWindow(w); UnlockPubScreen(NULL, front);
            printf("P2CTEST z done\n");
            return 0;
        }
        if (argc > 1 && argv[1][0] == 'x') {
            /* pairwise experiments: A then B, B is the known-failing 32x5 at ax+1 */
            struct BitMap *ref = AllocBitMap(AW + 32, AH + 8, 8, BMF_CLEAR, NULL);
            struct BitMap *src = AllocBitMap(SW, SH, 1, BMF_CLEAR, NULL);
            struct RastPort refrp;
            LONG ax = w->LeftEdge + 4, ay = w->TopEdge + 14;
            ULONG i;
            static const struct { LONG asx, aw, ah, aoff, bsx, bw, bh, boff; const char *name; } v[] = {
                { 7, 1, 1, 0,  8, 32, 5, 1, "V1 A=1x1@+0  B=32x5@+1 (original)" },
                { 7, 1, 1, 0,  8, 33, 5, 1, "V3 B width 33" },
                { 7, 32, 1, 0, 8, 32, 5, 1, "V4 A width 32" },
                { 7, 1, 1, 0,  8, 32, 5, 0, "V5 B at +0" },
                { 7, 1, 1, 1,  8, 32, 5, 1, "V6 A at +1" },
                { 0, 1, 1, 0,  0, 32, 5, 1, "V7 sx 0 both" },
                { 7, 1, 2, 0,  8, 32, 5, 1, "V8 A height 2" },
                { 7, 2, 1, 0,  8, 32, 5, 1, "V9 A width 2" },
                { 7, 1, 1, 0,  8, 64, 5, 1, "V10 B width 64" },
                { 7, 1, 1, 0,  8, 32, 1, 1, "V11 B height 1" },
                { 7, 1, 1, 0,  8, 32, 5, 1, "V12 fill between A and B" },
                { 7, 1, 1, 0,  8, 32, 5, 1, "V13 WaitBlit between A and B" },
                { 7, 1, 1, 0,  8, 32, 5, 1, "V14 Delay between A and B" },
                { 7, 1, 1, 0,  8, 32, 5, 1, "V15 no readback of A" },
                { 7, 1, 1, 0,  8, 32, 5, 1, "V16 no bg fill for A" },
                { 7, 1, 1, 0,  8, 32, 5, 1, "V17 no reference blits" },
                { 7, 1, 1, 0,  8, 32, 5, 1, "V18 B read twice" },
                { 7, 1, 1, 0,  8, 32, 5, 1, "V19 no bg fill for B" },
            };
            InitRastPort(&refrp); refrp.BitMap = ref;
            for (i = 0; i < 256; i++) { SetAPen(&front->RastPort, i); WritePixel(&front->RastPort, ax, ay); pen2pixel[i] = p96ReadPixel(&front->RastPort, ax, ay); }
            FillRandom(src, 0xff);
            for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
                printf("%s\n", v[i].name);
                skipFlags = i == 13 ? 4 : i == 14 ? 1 : i == 16 ? 2 : 0;
                RunCase(front, &refrp, src, ax + v[i].aoff, ay, v[i].asx, 0, v[i].aw, v[i].ah, 0xff, 1, depth <= 8);
                skipFlags = i == 16 ? 2 : i == 17 ? 8 : i == 18 ? 1 : 0;
                if (i == 10) { SetAPen(&front->RastPort, 5); RectFill(&front->RastPort, ax + 100, ay + 60, ax + 100, ay + 60); }
                if (i == 11) WaitBlit();
                if (i == 12) Delay(5);
                RunCase(front, &refrp, src, ax + v[i].boff, ay, v[i].bsx, 3, v[i].bw, v[i].bh, 0xff, 1, depth <= 8);
                skipFlags = 0;
                /* separator: a wide blit resets whatever state the pair left */
                RunCase(front, &refrp, src, ax, ay, 0, 0, 330, 64, 0xff, 1, depth <= 8);
            }
            FreeBitMap(src); FreeBitMap(ref);
            CloseWindow(w); UnlockPubScreen(NULL, front);
            printf("P2CTEST pair experiments done failures=%lu\n", (unsigned long)failures);
            return 0;
        }
        if (verbose) {
            /* suspect sequence only */
            struct BitMap *ref = AllocBitMap(AW + 32, AH + 8, 8, BMF_CLEAR, NULL);
            struct BitMap *src = AllocBitMap(SW, SH, 3, BMF_CLEAR, NULL);
            struct RastPort refrp;
            LONG ax = w->LeftEdge + 4, ay = w->TopEdge + 14;
            ULONG i;
            InitRastPort(&refrp); refrp.BitMap = ref;
            for (i = 0; i < 256; i++) { SetAPen(&front->RastPort, i); WritePixel(&front->RastPort, ax, ay); pen2pixel[i] = p96ReadPixel(&front->RastPort, ax, ay); }
            FillRandom(src, 0xff);
            printf("case A\n"); RunCase(front, &refrp, src, ax, ay, 31, 2, 200, 7, 0xff, 3, depth <= 8);
            printf("case B\n"); RunCase(front, &refrp, src, ax, ay, 31, 2, 200, 7, 0x01, 3, depth <= 8);
            printf("case C\n"); RunCase(front, &refrp, src, ax, ay, 31, 2, 200, 7, 0x05, 3, depth <= 8);
            printf("case D\n"); RunCase(front, &refrp, src, ax, ay, 5, 3, 8, 64, 0xff, 3, depth <= 8);
            FreeBitMap(src); FreeBitMap(ref);
            CloseWindow(w); UnlockPubScreen(NULL, front);
            printf("P2CTEST suspect sequence done failures=%lu\n", (unsigned long)failures);
            return 0;
        }
        TestScreen(front, w, depth <= 8);
        CloseWindow(w);
    }
    UnlockPubScreen(NULL, front);

    for (d = 0; d < 2 && !frontOnly; d++) {
        struct TagItem modeTags[] = {
            {P96MA_MinWidth, 640}, {P96MA_MinHeight, 480}, {P96MA_MinDepth, depths[d]},
            {P96MA_MaxWidth, 640}, {P96MA_MaxHeight, 480}, {P96MA_MaxDepth, depths[d]}, {TAG_DONE, 0} };
        struct List *modes = p96AllocModeListTagList(modeTags);
        struct P96Mode *mode;
        ULONG displayId = INVALID_ID, error = 0;
        struct Screen *screen;
        if (!modes) continue;
        for (mode = (struct P96Mode *)modes->lh_Head; mode->Node.ln_Succ; mode = (struct P96Mode *)mode->Node.ln_Succ)
            if (mode->Width == 640 && mode->Height == 480 && (ULONG)mode->Depth == depths[d]) { displayId = mode->DisplayID; break; }
        p96FreeModeList(modes);
        if (displayId == (ULONG)INVALID_ID) { printf("no 640x480x%lu mode\n", (unsigned long)depths[d]); continue; }
        {
            struct TagItem screenTags[] = {
                {P96SA_DisplayID, displayId}, {P96SA_Width, 640}, {P96SA_Height, 480}, {P96SA_Depth, depths[d]},
                {P96SA_Pens, (ULONG)pens}, {P96SA_Title, (ULONG)"p2ctest"}, {P96SA_ShowTitle, FALSE},
                {P96SA_ErrorCode, (ULONG)&error}, {TAG_DONE, 0} };
            screen = p96OpenScreenTagList(screenTags);
        }
        if (!screen) { printf("open screen %lu failed %lu\n", (unsigned long)depths[d], (unsigned long)error); continue; }
        if (depths[d] == 8) { ULONG i; for (i = 0; i < 256; i++) SetRGB32(&screen->ViewPort, i, i * 0x01010101UL, (i * 7) * 0x01010101UL, (i * 13) * 0x01010101UL); }
        w = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)screen, WA_Left, 100, WA_Top, 100, WA_Width, AW + 10, WA_Height, AH + 20,
                           WA_Flags, WFLG_SIMPLE_REFRESH | WFLG_NOCAREREFRESH, WA_Activate, TRUE, TAG_DONE);
        if (w) {
            printf("== 640x480 depth %lu screen ==\n", (unsigned long)depths[d]);
            TestScreen(screen, w, depths[d] <= 8);
            CloseWindow(w);
        }
        p96CloseScreen(screen);
    }
    printf("P2CTEST cases=%lu failures=%lu %s\n", (unsigned long)cases, (unsigned long)failures, failures ? "FAIL" : "ok");
    CloseLibrary((struct Library *)IntuitionBase); CloseLibrary((struct Library *)GfxBase); CloseLibrary(P96Base);
    CloseDevice((struct IORequest *)req); DeleteIORequest((struct IORequest *)req); DeleteMsgPort(port);
    return failures ? 10 : 0;
}
