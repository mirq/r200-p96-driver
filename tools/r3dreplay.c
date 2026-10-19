/*
 * r3dreplay - cpemit submit-path frame replay benchmark.
 *
 * Back-pressure/back-pressure-regression tool for the surviving client
 * submit path. Five fully pre-serialized frames are replayed round-robin
 * into an imported RGB565 surface; the timed loop issues exactly three
 * prebuilt Radeon3DSubmit() calls per frame -- one full-screen clear quad
 * plus two 252-vertex immediate triangle lists -- so per-call fail counts
 * stay directly comparable with the historical fail=N/6000 signature from
 * the state-batch era (2000 unpaced frames = 6000 calls).
 *
 * The immediate triangle list is the interface-1 bounded contract: RGB565
 * target, screen-space vertices, no depth. At -depth 32 the tool still
 * opens the 32-bit screen (same board-pinning behavior as the historical
 * 800x600x32 config) and renders into an offscreen RGB565 bitmap instead.
 *
 * The workload never fences, so a long run can outpace the GPU until the
 * ring-space wait in the service stalls or a submit fails; the service
 * treats a submit failure as CP trouble and runs recovery, which
 * invalidates the session and every later submit then fails. Drain the
 * ring every PACE frames by waiting on the last batch's fence: that only
 * bounds the outstanding queue to a sustainable level, like a real
 * client's present.
 *
 * Output: R3DREPLAY_SUMMARY with EClock-timed FPS. Run Work:radeon3dinfo
 * right after to dump the driver's per-submission sample ring.
 */

#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/radeon3d.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <devices/timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <p96_bitmap_api.h>

#define CHIP_LIBRARY_NAME "Radeon9200.chip"

/* The published interface-1 wire contract: 22 single-register PACKET0
 * writes, then PACKET3(3D_DRAW_IMMD_2) + its parameter dword, then
 * vertexCount * 3 vertex dwords. */
#define STREAM_HEADER_DWORDS 46UL
#define CLEAR_VERTICES       6UL
#define CLEAR_DWORDS         (STREAM_HEADER_DWORDS + CLEAR_VERTICES * 3UL)
#define BATCH_VERTICES       252UL
#define BATCH_DWORDS         (STREAM_HEADER_DWORDS + BATCH_VERTICES * 3UL)
#define BATCH_COLS           12UL
#define BATCH_ROWS           (BATCH_VERTICES / 3UL / BATCH_COLS)
#define FRAME_ROTATIONS      5UL
#define BATCHES_PER_FRAME    2UL
#define WARMUP_FRAMES        30UL
#define DEFAULT_FRAMES       2000UL
#define DEFAULT_PACE         64UL

struct Library *Radeon9200Base;
struct Library *P96Base;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;

#define PACKET0(reg)      ((ULONG)(reg) >> 2)

static void EmitRegister(ULONG *commands, ULONG *count,
                         ULONG reg, ULONG value)
{
    commands[(*count)++] = PACKET0(reg);
    commands[(*count)++] = value;
}

static ULONG FloatBits(ULONG value)
{
    ULONG top = 0;
    ULONG scan = value;

    if (!value)
        return 0;
    while (scan >>= 1)
        ++top;
    return ((127UL + top) << 23) |
           ((value << (23UL - top)) & 0x007fffffUL);
}

/* One complete immediate-triangle-list stream against an imported RGB565
 * surface. The register shape is the interface-1 contract the service
 * validates exactly (docs/02 section 11); phase1 proved it on hardware. */
static void BuildStream(ULONG *commands, ULONG gpuAddress, ULONG pitch,
                        ULONG width, ULONG height,
                        const ULONG *vertices, ULONG vertexCount)
{
    ULONG count = 0;
    ULONG index;

    EmitRegister(commands, &count, 0x2140UL, 0x00000000UL);
    EmitRegister(commands, &count, 0x2080UL, 0x00250000UL);
    EmitRegister(commands, &count, 0x2180UL, 0x00000000UL);
    EmitRegister(commands, &count, 0x20b0UL, 0x00000000UL);
    EmitRegister(commands, &count, 0x2088UL, 0x00000800UL);
    EmitRegister(commands, &count, 0x208cUL, 0x00000000UL);
    EmitRegister(commands, &count, 0x1c4cUL, 0x9800021eUL);
    EmitRegister(commands, &count, 0x1c38UL, 0x00001000UL);
    EmitRegister(commands, &count, 0x2f00UL, 0x00001000UL);
    EmitRegister(commands, &count, 0x2f04UL, 0x00011000UL);
    EmitRegister(commands, &count, 0x2f08UL, 0x00001000UL);
    EmitRegister(commands, &count, 0x2f0cUL, 0x00011000UL);
    EmitRegister(commands, &count, 0x2cc4UL, 0x00000000UL);
    EmitRegister(commands, &count, 0x26f0UL, 0x00000000UL);
    EmitRegister(commands, &count, 0x1c50UL, 0x00000000UL);
    EmitRegister(commands, &count, 0x26c0UL, 0x00000000UL);
    EmitRegister(commands, &count, 0x1c44UL,
                 ((height - 1UL) << 16) | (width - 1UL));
    EmitRegister(commands, &count, 0x1d84UL, 0xffffffffUL);
    EmitRegister(commands, &count, 0x1c20UL, 0x20210000UL);
    EmitRegister(commands, &count, 0x1c3cUL, 0x00001000UL);
    EmitRegister(commands, &count, 0x1c40UL, gpuAddress);
    EmitRegister(commands, &count, 0x1c48UL, pitch / 2UL);

    commands[count++] = 0xc0003500UL | ((vertexCount * 3UL) << 16);
    commands[count++] = (vertexCount << 16) | 0x00000034UL;
    for (index = 0; index < vertexCount * 3UL; ++index)
        commands[count++] = vertices[index];
}

/* Full-screen quad as a two-triangle list, in the clear color. */
static void BuildClearVertices(ULONG *verts, ULONG width, ULONG height,
                               ULONG color)
{
    static const ULONG corners[12] = {
        0, 0, 1, 0, 1, 1,
        0, 0, 1, 1, 0, 1
    };
    ULONG vertex;

    for (vertex = 0; vertex < CLEAR_VERTICES; ++vertex) {
        verts[vertex * 3UL] = FloatBits(corners[vertex * 2UL] ?
                                        width - 1UL : 0UL);
        verts[vertex * 3UL + 1UL] = FloatBits(corners[vertex * 2UL + 1UL] ?
                                              height - 1UL : 0UL);
        verts[vertex * 3UL + 2UL] = color;
    }
}

/* BATCH_VERTICES screen-space triangles arranged as a BATCH_COLS x
 * BATCH_ROWS grid of cells; the five rotations shift the fill phase and
 * hue so consecutive frames differ on screen. Everything stays inside the
 * validated target rectangle. */
static void BuildBatchVertices(ULONG *verts, ULONG rotation, ULONG batch,
                               ULONG width, ULONG height)
{
    ULONG cellW = (width - 32UL) / BATCH_COLS;
    ULONG cellH = (height - 32UL) / BATCH_ROWS;
    ULONG tri;

    if (!cellW)
        cellW = 2UL;
    if (!cellH)
        cellH = 2UL;
    for (tri = 0; tri < BATCH_VERTICES / 3UL; ++tri) {
        ULONG col = tri % BATCH_COLS;
        ULONG row = tri / BATCH_COLS;
        ULONG phase = (row + rotation * 3UL + batch) % BATCH_ROWS;
        ULONG x0 = 16UL + col * cellW;
        ULONG y0 = 16UL + phase * cellH;
        ULONG red = ((phase * 255UL) / (BATCH_ROWS - 1UL)) << 16;
        ULONG green = ((col * 255UL) / (BATCH_COLS - 1UL)) << 8;
        ULONG blue = rotation * 40UL;
        ULONG color = 0xff000000UL | red | green | blue;

        verts[tri * 9UL] = FloatBits(x0);
        verts[tri * 9UL + 1UL] = FloatBits(y0);
        verts[tri * 9UL + 2UL] = color;
        verts[tri * 9UL + 3UL] = FloatBits(x0 + cellW - 2UL);
        verts[tri * 9UL + 4UL] = FloatBits(y0);
        verts[tri * 9UL + 5UL] = color;
        verts[tri * 9UL + 6UL] = FloatBits(x0);
        verts[tri * 9UL + 7UL] = FloatBits(y0 + cellH - 2UL);
        verts[tri * 9UL + 8UL] = color;
    }
}

static ULONG ParseValue(int argc, char **argv, const char *name,
                        ULONG fallback)
{
    int index;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], name) == 0 && index + 1 < argc)
            return (ULONG)atol(argv[index + 1]);
    }
    return fallback;
}

static BOOL HasFlag(int argc, char **argv, const char *name)
{
    int index;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], name) == 0)
            return TRUE;
    }
    return FALSE;
}

static int Fail(const char *check)
{
    printf("R3DREPLAY status=fail check=%s\n", check);
    return 10;
}

/* A 16-bit RGB565 friend bitmap for the 3D target: the immediate
 * triangle list validates its target against an imported R5G6B5PC
 * surface, so at any other screen depth the workload renders offscreen
 * (r200_surface.c AllocateSeedBitMap pattern: the friend bitmap must
 * match the requested depth or P96 upgrades the allocation). */
static struct BitMap *AllocateRgb565BitMap(ULONG width, ULONG height)
{
    struct TagItem modeTags[] = {
        {P96_BESTMODE_FORMATS_ALLOWED, RGBFF_R5G6B5PC},
        {P96_BESTMODE_NOMINAL_WIDTH, 640UL},
        {P96_BESTMODE_NOMINAL_HEIGHT, 480UL},
        {P96_BESTMODE_DEPTH, 16UL},
        {TAG_DONE, 0}
    };
    struct TagItem screenTags[] = {
        {P96_SCREEN_DISPLAY_ID, 0},
        {P96_SCREEN_DEPTH, 16UL},
        {P96_SCREEN_RGB_FORMAT, RGBFB_R5G6B5PC},
        {P96_SCREEN_SHOW_TITLE, FALSE},
        {P96_SCREEN_BEHIND, TRUE},
        {P96_SCREEN_QUIET, TRUE},
        {TAG_DONE, 0}
    };
    struct Screen *friendScreen;
    struct BitMap *bitmap;
    ULONG mode = p96BestModeIDTagList(modeTags);

    if (mode == 0xffffffffUL)
        return NULL;
    screenTags[0].ti_Data = mode;
    friendScreen = p96OpenScreenTagList(screenTags);
    if (!friendScreen)
        return NULL;
    bitmap = p96AllocBitMap(width, height, 16,
                            BMF_CLEAR | BMF_DISPLAYABLE,
                            friendScreen->RastPort.BitMap,
                            RGBFB_R5G6B5PC);
    p96CloseScreen(friendScreen);
    return bitmap;
}

int main(int argc, char **argv)
{
    struct Radeon3DInfo info;
    struct Radeon3DSurface surface;
    struct Screen *screen = NULL;
    struct BitMap *bitmap = NULL;
    struct BitMap *targetBitmap;
    struct MsgPort *timerPort = NULL;
    struct timerequest *timerIO = NULL;
    struct EClockVal startClock;
    struct EClockVal endClock;
    static ULONG clearCommands[CLEAR_DWORDS];
    static ULONG clearVertices[CLEAR_VERTICES * 3UL];
    static ULONG batchCommands[FRAME_ROTATIONS][BATCHES_PER_FRAME]
                              [BATCH_DWORDS];
    static ULONG batchVertices[BATCH_VERTICES * 3UL];
    struct Device *TimerBase;
    struct Radeon3DDevice *device = NULL;
    ULONG width = ParseValue(argc, argv, "-width", 800UL);
    ULONG height = ParseValue(argc, argv, "-height", 600UL);
    ULONG depth = ParseValue(argc, argv, "-depth", 32UL);
    ULONG frames = ParseValue(argc, argv, "-frames", DEFAULT_FRAMES);
    ULONG rgbFormat = depth == 16UL ? RGBFB_R5G6B5PC : RGBFB_B8G8R8A8;
    ULONG rgbFlag = depth == 16UL ? RGBFF_R5G6B5PC : RGBFF_B8G8R8A8;
    ULONG eclockHz;
    ULONG frame;
    ULONG elapsed;
    ULONG fpsX1000;
    ULONG rotation;
    ULONG batchIndex;
    ULONG failedSubmits = 0;
    ULONG clearOnly = HasFlag(argc, argv, "-clearonly");
    ULONG noClear = HasFlag(argc, argv, "-noclear");
    ULONG pace = ParseValue(argc, argv, "-pace", DEFAULT_PACE);
    int result = 0;

    Radeon9200Base = OpenLibrary(
        (CONST_STRPTR)CHIP_LIBRARY_NAME, RADEON3D_LIBRARY_VERSION);
    P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    GfxBase = (struct GfxBase *)OpenLibrary(
        (CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 39);
    if (!Radeon9200Base || !P96Base || !GfxBase || !IntuitionBase) {
        result = Fail("open_library");
        goto out;
    }

    info.Size = sizeof(info);
    device = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
    if (!device) {
        result = Fail("open_service");
        goto out;
    }
    printf("R3DREPLAY iface=%lu caps=%08lx generation=%lu\n",
           (unsigned long)info.Version, (unsigned long)info.Caps,
           (unsigned long)info.Generation);
    if ((info.Caps & (RADEON3D_CAP_CP_READY |
                      RADEON3D_CAP_FENCES |
                      RADEON3D_CAP_BITMAP_IMPORT |
                      RADEON3D_CAP_IMMD_TRI_LIST)) !=
        (RADEON3D_CAP_CP_READY |
         RADEON3D_CAP_FENCES |
         RADEON3D_CAP_BITMAP_IMPORT |
         RADEON3D_CAP_IMMD_TRI_LIST)) {
        result = Fail("capabilities");
        goto out;
    }

    {
        struct TagItem modeTags[] = {
            {P96_BESTMODE_FORMATS_ALLOWED, rgbFlag},
            {P96_BESTMODE_NOMINAL_WIDTH, width},
            {P96_BESTMODE_NOMINAL_HEIGHT, height},
            {P96_BESTMODE_DEPTH, depth},
            {TAG_DONE, 0}
        };
        struct TagItem screenTags[] = {
            {P96_SCREEN_DISPLAY_ID, 0},
            {P96_SCREEN_DEPTH, depth},
            {P96_SCREEN_RGB_FORMAT, rgbFormat},
            {P96_SCREEN_SHOW_TITLE, FALSE},
            {P96_SCREEN_QUIET, TRUE},
            {TAG_DONE, 0}
        };
        ULONG mode = p96BestModeIDTagList(modeTags);

        if (mode == 0xffffffffUL) {
            result = Fail("bestmode");
            goto out;
        }
        screenTags[0].ti_Data = mode;
        screen = p96OpenScreenTagList(screenTags);
        if (!screen) {
            result = Fail("open_screen");
            goto out;
        }
        /* Keep the screen open for the whole run: closing it lets a
         * deferred board release advance the service generation and
         * silently invalidate the session mid-run. */
        if (depth == 16UL) {
            targetBitmap = screen->RastPort.BitMap;
        } else {
            bitmap = AllocateRgb565BitMap(width, height);
            if (!bitmap) {
                result = Fail("alloc_bitmap");
                goto out;
            }
            targetBitmap = bitmap;
        }
    }

    surface.Size = sizeof(surface);
    if (!Radeon3DImportBitMap(device, targetBitmap, &surface) ||
        surface.Version != RADEON3D_SURFACE_VERSION ||
        surface.Format != RADEON3D_FORMAT_R5G6B5PC) {
        result = Fail("import_bitmap");
        goto out;
    }
    printf("R3DREPLAY surface cpu=%08lx gpu=%08lx pitch=%lu "
           "target=%s\n",
           (unsigned long)surface.CpuAddress,
           (unsigned long)surface.GpuAddress,
           (unsigned long)surface.Pitch,
           depth == 16UL ? "screen" : "offscreen");

    /* Pre-serialize all five frames. Nothing in the timed loop builds
     * commands; the client does no per-frame work, so the measured
     * ceiling is the service validate + ring submit + GPU path. */
    BuildClearVertices(clearVertices, width, height, 0xff202040UL);
    BuildStream(clearCommands, surface.GpuAddress, surface.Pitch,
                surface.Width, surface.Height, clearVertices,
                CLEAR_VERTICES);
    for (rotation = 0; rotation < FRAME_ROTATIONS; ++rotation) {
        for (batchIndex = 0; batchIndex < BATCHES_PER_FRAME;
             ++batchIndex) {
            BuildBatchVertices(batchVertices, rotation, batchIndex,
                               surface.Width, surface.Height);
            BuildStream(batchCommands[rotation][batchIndex],
                        surface.GpuAddress, surface.Pitch,
                        surface.Width, surface.Height, batchVertices,
                        BATCH_VERTICES);
        }
    }

    /* Validation frame: fence + wait so a rejected stream fails loudly
     * here instead of silently dropping frames in the loop. */
    {
        ULONG validateFence = 0;

        if (!Radeon3DSubmit(device, clearCommands, CLEAR_DWORDS,
                            RADEON3D_SUBMIT_FENCE, &validateFence) ||
            !validateFence ||
            !Radeon3DWaitFence(device, validateFence, 10000UL)) {
            result = Fail("validate_clear");
            goto out;
        }
        validateFence = 0;
        if (!Radeon3DSubmit(device, batchCommands[0][0], BATCH_DWORDS,
                            RADEON3D_SUBMIT_FENCE, &validateFence) ||
            !validateFence ||
            !Radeon3DWaitFence(device, validateFence, 10000UL)) {
            info.Size = sizeof(info);
            if (Radeon3DGetInfo(device, &info))
                printf("R3DREPLAY submit_fail_stage=%lu fence=%08lx\n",
                       (unsigned long)info.CommitFailStage,
                       (unsigned long)validateFence);
            result = Fail("validate_batch");
            goto out;
        }
        printf("R3DREPLAY validate=ok\n");
    }

    timerPort = CreateMsgPort();
    timerIO = timerPort ? (struct timerequest *)CreateIORequest(
        timerPort, sizeof(struct timerequest)) : NULL;
    if (!timerPort || !timerIO ||
        OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                   (struct IORequest *)timerIO, 0) != 0) {
        result = Fail("timer");
        goto out;
    }
    TimerBase = timerIO->tr_node.io_Device;
    eclockHz = (ULONG)ReadEClock(&endClock);
    if (!eclockHz) {
        result = Fail("eclock");
        goto out;
    }

    for (frame = 0; frame < WARMUP_FRAMES; ++frame) {
        rotation = frame % FRAME_ROTATIONS;
        if (!noClear)
            (void)Radeon3DSubmit(device, clearCommands, CLEAR_DWORDS,
                                 0UL, NULL);
        if (!clearOnly) {
            (void)Radeon3DSubmit(device, batchCommands[rotation][0],
                                 BATCH_DWORDS, 0UL, NULL);
            (void)Radeon3DSubmit(device, batchCommands[rotation][1],
                                 BATCH_DWORDS, 0UL, NULL);
        }
    }

    ReadEClock(&startClock);
    for (frame = 0; frame < frames; ++frame) {
        rotation = frame % FRAME_ROTATIONS;
        if (!noClear &&
            !Radeon3DSubmit(device, clearCommands, CLEAR_DWORDS,
                            0UL, NULL)) {
            ++failedSubmits;
            if (failedSubmits == 1UL && Radeon3DGetInfo(device, &info))
                printf("R3DREPLAY loopfail call=clear frame=%lu "
                       "stage=%lu\n",
                       (unsigned long)frame,
                       (unsigned long)info.CommitFailStage);
        }
        if (!clearOnly) {
            ULONG drain = pace && ((frame % pace) == pace - 1UL);

            for (batchIndex = 0; batchIndex < BATCHES_PER_FRAME;
                 ++batchIndex) {
                ULONG fence = 0;
                BOOL ok = Radeon3DSubmit(
                    device, batchCommands[rotation][batchIndex],
                    BATCH_DWORDS,
                    (drain && batchIndex == BATCHES_PER_FRAME - 1UL) ?
                        RADEON3D_SUBMIT_FENCE : 0UL,
                    (drain && batchIndex == BATCHES_PER_FRAME - 1UL) ?
                        &fence : NULL);

                if (!ok) {
                    ++failedSubmits;
                    if (failedSubmits == 1UL &&
                        Radeon3DGetInfo(device, &info))
                        printf("R3DREPLAY loopfail call=batch "
                               "frame=%lu stage=%lu\n",
                               (unsigned long)frame,
                               (unsigned long)info.CommitFailStage);
                } else if (fence &&
                           !Radeon3DWaitFence(device, fence, 10000UL)) {
                    result = Fail("pace_fence");
                    goto out;
                }
            }
        }
        if ((frame & 255UL) == 255UL && CheckSignal(SIGBREAKF_CTRL_C))
            break;
    }
    ReadEClock(&endClock);

    elapsed = endClock.ev_lo - startClock.ev_lo;
    if (!elapsed) {
        result = Fail("timing");
        goto out;
    }
    fpsX1000 = (ULONG)((unsigned long long)frame *
                       (unsigned long long)eclockHz * 1000ULL /
                       elapsed);
    printf("R3DREPLAY_SUMMARY version=4 mode=%s target=%s z=off pace=%lu "
           "width=%lu height=%lu depth=%lu frames=%lu elapsed_ticks=%lu "
           "eclock_hz=%lu fps_x1000=%lu calls=%lu fail=%lu\n",
           clearOnly ? "clearonly" : (noClear ? "noclear" : "full"),
           depth == 16UL ? "screen" : "offscreen",
           (unsigned long)pace,
           (unsigned long)width, (unsigned long)height,
           (unsigned long)depth, (unsigned long)frame,
           (unsigned long)elapsed,
           (unsigned long)eclockHz,
           (unsigned long)fpsX1000,
           (unsigned long)(frame * (clearOnly ? 1UL :
                                    (noClear ? BATCHES_PER_FRAME :
                                     BATCHES_PER_FRAME + 1UL))),
           (unsigned long)failedSubmits);
    printf("R3DREPLAY status=ok\n");

out:
    if (timerIO) {
        if (timerIO->tr_node.io_Device)
            CloseDevice((struct IORequest *)timerIO);
        DeleteIORequest((struct IORequest *)timerIO);
    }
    if (timerPort)
        DeleteMsgPort(timerPort);
    if (screen)
        p96CloseScreen(screen);
    if (bitmap)
        p96FreeBitMap(bitmap);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    if (P96Base)
        CloseLibrary(P96Base);
    if (Radeon9200Base)
        CloseLibrary(Radeon9200Base);
    return result;
}
