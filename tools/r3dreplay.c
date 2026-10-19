/*
 * r3dreplay - pre-serialized frame replay benchmark.
 *
 * Proves the driver + GPU ceiling when the client does no per-frame work:
 * five fully pre-serialized frames (one clear record plus two TCL state
 * batches each, differing only in the ModelProjection rotation) are
 * replayed round-robin into a visible 800x600 screen. The torus vertices
 * are hardware-TCL-transformed, written once into two 256 KiB vertex
 * segments at startup, and never touched again -- the timed loop only
 * issues three prebuilt API calls per frame with no fences, so ordered
 * commits keep the pipeline full.
 *
 * -z attaches a 16-bit depth surface (gears shape: color clear + Z16
 * clear + depth-tested/written geometry), so the 16 vs 32 bpp comparison
 * includes the depth traffic that the real MiniGL stack always pays.
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

#define HEADER_DWORDS      44UL
#define VERTEX_STRIDE      10UL
#define RING_SEGS          54UL
#define TUBE_SEGS          60UL
#define RINGS_PER_SEGMENT  27UL
#define QUADS_PER_SEGMENT  (RINGS_PER_SEGMENT * TUBE_SEGS)
#define VERTS_PER_SEGMENT  (QUADS_PER_SEGMENT * 4UL)
#define SEGMENT_BYTES      (VERTS_PER_SEGMENT * VERTEX_STRIDE * 4UL)
#define SEGMENT_COUNT      2UL
#define FRAME_ROTATIONS    5UL
#define WARMUP_FRAMES      30UL
#define DEFAULT_FRAMES     2000UL
/* Ordered commits keep the pipeline full, but the tool never fences, so a
 * long run can outpace the GPU until the CP ring space wait expires; the
 * service treats any submit failure as CP death and runs full recovery,
 * which invalidates the session and every later submit then fails. Drain
 * the ring every PACE frames by waiting on the last batch's fence: the
 * internal fence already exists on every submit, so this only bounds the
 * outstanding queue to a sustainable level, like a real client's present. */
#define DEFAULT_PACE       64UL

struct Library *Radeon9200Base;
struct Library *P96Base;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;

static const ULONG kRingCos[54UL] = {
    0x3f800000UL, 0x3f7e44deUL, 0x3f791978UL, 0x3f708fb2UL, 0x3f64c51cUL, 0x3f55e287UL,
    0x3f441b7dUL, 0x3f2fad88UL, 0x3f18df63UL, 0x3f000000UL, 0x3ecacaf8UL, 0x3e92d7e0UL,
    0x3e31d0d4UL, 0x3d6e2946UL, 0xbd6e2946UL, 0xbe31d0d4UL, 0xbe92d7e0UL, 0xbecacaf8UL,
    0xbf000000UL, 0xbf18df63UL, 0xbf2fad88UL, 0xbf441b7dUL, 0xbf55e287UL, 0xbf64c51cUL,
    0xbf708fb2UL, 0xbf791978UL, 0xbf7e44deUL, 0xbf800000UL, 0xbf7e44deUL, 0xbf791978UL,
    0xbf708fb2UL, 0xbf64c51cUL, 0xbf55e287UL, 0xbf441b7dUL, 0xbf2fad88UL, 0xbf18df63UL,
    0xbf000000UL, 0xbecacaf8UL, 0xbe92d7e0UL, 0xbe31d0d4UL, 0xbd6e2946UL, 0x3d6e2946UL,
    0x3e31d0d4UL, 0x3e92d7e0UL, 0x3ecacaf8UL, 0x3f000000UL, 0x3f18df63UL, 0x3f2fad88UL,
    0x3f441b7dUL, 0x3f55e287UL, 0x3f64c51cUL, 0x3f708fb2UL, 0x3f791978UL, 0x3f7e44deUL,
};

static const ULONG kRingSin[54UL] = {
    0x00000000UL, 0x3dedc21fUL, 0x3e6c2691UL, 0x3eaf1d44UL, 0x3ee5c902UL, 0x3f0cac9fUL,
    0x3f248dbbUL, 0x3f3a3529UL, 0x3f4d57f2UL, 0x3f5db3d7UL, 0x3f6b1036UL, 0x3f753ecdUL,
    0x3f7c1c5cUL, 0x3f7f9120UL, 0x3f7f9120UL, 0x3f7c1c5cUL, 0x3f753ecdUL, 0x3f6b1036UL,
    0x3f5db3d7UL, 0x3f4d57f2UL, 0x3f3a3529UL, 0x3f248dbbUL, 0x3f0cac9fUL, 0x3ee5c902UL,
    0x3eaf1d44UL, 0x3e6c2691UL, 0x3dedc21fUL, 0x250d3132UL, 0xbdedc21fUL, 0xbe6c2691UL,
    0xbeaf1d44UL, 0xbee5c902UL, 0xbf0cac9fUL, 0xbf248dbbUL, 0xbf3a3529UL, 0xbf4d57f2UL,
    0xbf5db3d7UL, 0xbf6b1036UL, 0xbf753ecdUL, 0xbf7c1c5cUL, 0xbf7f9120UL, 0xbf7f9120UL,
    0xbf7c1c5cUL, 0xbf753ecdUL, 0xbf6b1036UL, 0xbf5db3d7UL, 0xbf4d57f2UL, 0xbf3a3529UL,
    0xbf248dbbUL, 0xbf0cac9fUL, 0xbee5c902UL, 0xbeaf1d44UL, 0xbe6c2691UL, 0xbdedc21fUL,
};

static const ULONG kTubeCos[60UL] = {
    0x3f800000UL, 0x3f7e98fdUL, 0x3f7a67e2UL, 0x3f737871UL, 0x3f69de1dUL, 0x3f5db3d7UL,
    0x3f4f1bbdUL, 0x3f3e3ebdUL, 0x3f2b4c25UL, 0x3f167918UL, 0x3f000000UL, 0x3ed03fc9UL,
    0x3e9e377aUL, 0x3e54e6cdUL, 0x3dd61305UL, 0x25a34c4cUL, 0xbdd61305UL, 0xbe54e6cdUL,
    0xbe9e377aUL, 0xbed03fc9UL, 0xbf000000UL, 0xbf167918UL, 0xbf2b4c25UL, 0xbf3e3ebdUL,
    0xbf4f1bbdUL, 0xbf5db3d7UL, 0xbf69de1dUL, 0xbf737871UL, 0xbf7a67e2UL, 0xbf7e98fdUL,
    0xbf800000UL, 0xbf7e98fdUL, 0xbf7a67e2UL, 0xbf737871UL, 0xbf69de1dUL, 0xbf5db3d7UL,
    0xbf4f1bbdUL, 0xbf3e3ebdUL, 0xbf2b4c25UL, 0xbf167918UL, 0xbf000000UL, 0xbed03fc9UL,
    0xbe9e377aUL, 0xbe54e6cdUL, 0xbdd61305UL, 0xa553c9caUL, 0x3dd61305UL, 0x3e54e6cdUL,
    0x3e9e377aUL, 0x3ed03fc9UL, 0x3f000000UL, 0x3f167918UL, 0x3f2b4c25UL, 0x3f3e3ebdUL,
    0x3f4f1bbdUL, 0x3f5db3d7UL, 0x3f69de1dUL, 0x3f737871UL, 0x3f7a67e2UL, 0x3f7e98fdUL,
};

static const ULONG kTubeSin[60UL] = {
    0x00000000UL, 0x3dd61305UL, 0x3e54e6cdUL, 0x3e9e377aUL, 0x3ed03fc9UL, 0x3f000000UL,
    0x3f167918UL, 0x3f2b4c25UL, 0x3f3e3ebdUL, 0x3f4f1bbdUL, 0x3f5db3d7UL, 0x3f69de1dUL,
    0x3f737871UL, 0x3f7a67e2UL, 0x3f7e98fdUL, 0x3f800000UL, 0x3f7e98fdUL, 0x3f7a67e2UL,
    0x3f737871UL, 0x3f69de1dUL, 0x3f5db3d7UL, 0x3f4f1bbdUL, 0x3f3e3ebdUL, 0x3f2b4c25UL,
    0x3f167918UL, 0x3f000000UL, 0x3ed03fc9UL, 0x3e9e377aUL, 0x3e54e6cdUL, 0x3dd61305UL,
    0x26234c4cUL, 0xbdd61305UL, 0xbe54e6cdUL, 0xbe9e377aUL, 0xbed03fc9UL, 0xbf000000UL,
    0xbf167918UL, 0xbf2b4c25UL, 0xbf3e3ebdUL, 0xbf4f1bbdUL, 0xbf5db3d7UL, 0xbf69de1dUL,
    0xbf737871UL, 0xbf7a67e2UL, 0xbf7e98fdUL, 0xbf800000UL, 0xbf7e98fdUL, 0xbf7a67e2UL,
    0xbf737871UL, 0xbf69de1dUL, 0xbf5db3d7UL, 0xbf4f1bbdUL, 0xbf3e3ebdUL, 0xbf2b4c25UL,
    0xbf167918UL, 0xbf000000UL, 0xbed03fc9UL, 0xbe9e377aUL, 0xbe54e6cdUL, 0xbdd61305UL,
};

static const ULONG kMatrices[5UL][16UL] = {
    {
        0x3c000000UL, 0x00000000UL, 0x00000000UL, 0x00000000UL,
        0x80000000UL, 0x3bddb3d7UL, 0x3b800000UL, 0x00000000UL,
        0x00000000UL, 0xbb800000UL, 0x3bddb3d7UL, 0x00000000UL,
        0x00000000UL, 0x00000000UL, 0x00000000UL, 0x3f800000UL,
    },
    {
        0x3b1e377aUL, 0x3bf37871UL, 0x00000000UL, 0x00000000UL,
        0xbbd2da03UL, 0x3b09050aUL, 0x3b800000UL, 0x00000000UL,
        0x3b737871UL, 0xba9e377aUL, 0x3bddb3d7UL, 0x00000000UL,
        0x00000000UL, 0x00000000UL, 0x00000000UL, 0x3f800000UL,
    },
    {
        0xbbcf1bbdUL, 0x3b967918UL, 0x00000000UL, 0x00000000UL,
        0xbb82503fUL, 0xbbb35c71UL, 0x3b800000UL, 0x00000000UL,
        0x3b167918UL, 0x3b4f1bbdUL, 0x3bddb3d7UL, 0x00000000UL,
        0x00000000UL, 0x00000000UL, 0x00000000UL, 0x3f800000UL,
    },
    {
        0xbbcf1bbdUL, 0xbb967918UL, 0x00000000UL, 0x00000000UL,
        0x3b82503fUL, 0xbbb35c71UL, 0x3b800000UL, 0x00000000UL,
        0xbb167918UL, 0x3b4f1bbdUL, 0x3bddb3d7UL, 0x00000000UL,
        0x00000000UL, 0x00000000UL, 0x00000000UL, 0x3f800000UL,
    },
    {
        0x3b1e377aUL, 0xbbf37871UL, 0x00000000UL, 0x00000000UL,
        0x3bd2da03UL, 0x3b09050aUL, 0x3b800000UL, 0x00000000UL,
        0xbb737871UL, 0xba9e377aUL, 0x3bddb3d7UL, 0x00000000UL,
        0x00000000UL, 0x00000000UL, 0x00000000UL, 0x3f800000UL,
    },
};

static int Fail(const char *check)
{
    printf("R3DREPLAY status=fail check=%s\n", check);
    return 10;
}

static ULONG Rev(ULONG value)
{
    return (value << 24) | ((value << 8) & 0x00ff0000UL) |
           ((value >> 8) & 0x0000ff00UL) | (value >> 24);
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

static float BitsToFloat(ULONG bits)
{
    union {
        float f;
        ULONG u;
    } cvt;

    cvt.u = bits;
    return cvt.f;
}

static ULONG FloatToBits(float value)
{
    union {
        float f;
        ULONG u;
    } cvt;

    cvt.f = value;
    return cvt.u;
}

static void BuildFrameHeader(ULONG *header, ULONG handle, ULONG depthHandle,
                             const ULONG *matrix, ULONG width,
                             ULONG height, ULONG useDepth)
{
    ULONG index;

    for (index = 0; index < HEADER_DWORDS; ++index)
        header[index] = 0;
    header[0] = RADEON3D_EXEC_DRAW_QUADS;
    header[1] = HEADER_DWORDS;
    header[2] = handle;
    header[3] = depthHandle;
    header[5] = RADEON3D_DRAW_FRAGMENT_STATE |
                RADEON3D_DRAW_EXTENDED_VERTEX |
                RADEON3D_DRAW_HW_TCL;
    if (useDepth)
        header[5] |= RADEON3D_DRAW_DEPTH_LESS |
                     RADEON3D_DRAW_DEPTH_WRITE;
    header[8] = width;
    header[9] = height;
    header[10] = VERTS_PER_SEGMENT;
    for (index = 0; index < 16UL; ++index)
        header[21UL + index] = matrix[index];
    header[37] = FloatBits(width / 2UL);
    header[38] = FloatBits(width / 2UL);
    header[39] = 0x80000000UL | FloatBits(height / 2UL);
    header[40] = FloatBits(height / 2UL);
    header[41] = 0x3f800000UL;
    header[42] = 0x3f800000UL;
    header[43] = 16UL << RADEON3D_TRANSFORM_POINT_SIZE_SHIFT;
}

static void BuildClearRecord(ULONG *record, ULONG handle, ULONG depthHandle,
                             ULONG width, ULONG height, ULONG useDepth)
{
    record[0] = RADEON3D_EXEC_CLEAR;
    record[1] = RADEON3D_EXEC_CLEAR_DWORDS;
    record[2] = handle;
    record[3] = useDepth ? depthHandle : 0;
    record[4] = useDepth ? (RADEON3D_CLEAR_COLOR |
                            RADEON3D_CLEAR_DEPTH) : RADEON3D_CLEAR_COLOR;
    record[5] = 0xff202040UL;
    record[6] = 0x3f800000UL; /* 1.0f */
    record[7] = 0;
    record[8] = 0;
    record[9] = width;
    record[10] = height;
}

/* One closed half-torus per segment: 27 ring steps x 60 tube steps of
 * quads, positions within +/-155 so the GPU clips the rim at 800x600.
 * All dwords are byte-reversed: the VAP fetches the segment
 * little-endian, opposite to the inline Execute copy. */
static void FillSegment(volatile ULONG *seg, ULONG ringBase)
{
    const float outer = 95.0f;
    const float tube = 60.0f;
    ULONG quad;
    ULONG index = 0;

    for (quad = 0; quad < QUADS_PER_SEGMENT; ++quad) {
        ULONG ring = quad / TUBE_SEGS;
        ULONG tube0 = quad % TUBE_SEGS;
        ULONG tube1 = (tube0 + 1UL) % TUBE_SEGS;
        ULONG ring0 = ringBase + ring;
        ULONG ring1 = ringBase + ((ring + 1UL) % RINGS_PER_SEGMENT);
        ULONG cornerRing[4];
        ULONG cornerTube[4];
        ULONG corner;
        ULONG red = ((ring * 255UL) / (RINGS_PER_SEGMENT - 1UL)) << 16;
        ULONG greenBase = (tube0 * 255UL) / (TUBE_SEGS - 1UL);

        cornerRing[0] = ring0;
        cornerTube[0] = tube0;
        cornerRing[1] = ring1;
        cornerTube[1] = tube0;
        cornerRing[2] = ring1;
        cornerTube[2] = tube1;
        cornerRing[3] = ring0;
        cornerTube[3] = tube1;

        for (corner = 0; corner < 4UL; ++corner) {
            float cosT = BitsToFloat(kTubeCos[cornerTube[corner]]);
            float sinT = BitsToFloat(kTubeSin[cornerTube[corner]]);
            float cosR = BitsToFloat(kRingCos[cornerRing[corner]]);
            float sinR = BitsToFloat(kRingSin[cornerRing[corner]]);
            float arm = outer + tube * cosT;
            float x = arm * cosR;
            float y = arm * sinR;
            float z = tube * sinT;
            ULONG green = (greenBase + corner) % 256UL;
            ULONG color = 0xff000000UL | red | (green << 8);
            ULONG words[VERTEX_STRIDE];
            ULONG word;

            words[0] = FloatToBits(x);
            words[1] = FloatToBits(y);
            words[2] = FloatToBits(z);
            words[3] = 0x3f800000UL;
            words[4] = color;
            words[5] = 0;
            words[6] = 0;
            words[7] = 0;
            words[8] = 0;
            words[9] = 0;
            for (word = 0; word < VERTEX_STRIDE; ++word)
                seg[index++] = Rev(words[word]);
        }
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

int main(int argc, char **argv)
{
    struct Radeon3DInfo info;
    struct Radeon3DSurface surface;
    struct Radeon3DSurface depthSurface;
    struct Radeon3DSegment segment[SEGMENT_COUNT];
    struct Screen *screen = NULL;
    struct BitMap *bitmap = NULL;
    struct BitMap *depthBitmap = NULL;
    struct MsgPort *timerPort = NULL;
    struct timerequest *timerIO = NULL;
    struct EClockVal startClock;
    struct EClockVal endClock;
    static ULONG headers[FRAME_ROTATIONS][HEADER_DWORDS];
    static ULONG clearRecord[RADEON3D_EXEC_CLEAR_DWORDS];
    static struct Radeon3DStateBatch batches[FRAME_ROTATIONS][SEGMENT_COUNT];
    static struct Radeon3DStateBatchDraw draws[SEGMENT_COUNT];
    struct Device *TimerBase;
    struct Radeon3DDevice *device = NULL;
    volatile ULONG *vertices;
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
    ULONG segIndex;
    ULONG failedSubmits = 0;
    ULONG clearOnly = HasFlag(argc, argv, "-clearonly");
    ULONG noClear = HasFlag(argc, argv, "-noclear");
    ULONG offscreen = HasFlag(argc, argv, "-offscreen");
    ULONG useZ = HasFlag(argc, argv, "-z");
    ULONG pace = ParseValue(argc, argv, "-pace", DEFAULT_PACE);
    int result = 0;

    memset(segment, 0, sizeof(segment));

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
    memset(&depthSurface, 0, sizeof(depthSurface));
    printf("R3DREPLAY iface=%lu caps=%08lx generation=%lu\n",
           (unsigned long)info.Version, (unsigned long)info.Caps,
           (unsigned long)info.Generation);
    if ((info.Caps & (RADEON3D_CAP_STREAM_SEGMENTS |
                      RADEON3D_CAP_COMMIT_STATE_BATCH |
                      RADEON3D_CAP_ORDERED_COMMITS)) !=
        (RADEON3D_CAP_STREAM_SEGMENTS |
         RADEON3D_CAP_COMMIT_STATE_BATCH |
         RADEON3D_CAP_ORDERED_COMMITS)) {
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
        if (offscreen) {
            /* Keep the screen open for the whole run: closing it lets a
             * deferred board release advance the service generation and
             * silently invalidate the session mid-run. */
            bitmap = p96AllocBitMap(width, height, depth,
                                    BMF_CLEAR | BMF_DISPLAYABLE,
                                    screen->RastPort.BitMap, rgbFormat);
            if (!bitmap) {
                result = Fail("alloc_bitmap");
                goto out;
            }
        } else {
            bitmap = screen->RastPort.BitMap;
        }
    }

    surface.Size = sizeof(surface);
    if (!Radeon3DImportBitMap(device, bitmap, &surface) ||
        surface.Version != RADEON3D_SURFACE_VERSION) {
        result = Fail("import_bitmap");
        goto out;
    }
    printf("R3DREPLAY surface cpu=%08lx gpu=%08lx pitch=%lu\n",
           (unsigned long)surface.CpuAddress,
           (unsigned long)surface.GpuAddress,
           (unsigned long)surface.Pitch);

    if (useZ) {
        /* RGB565 storage like minigl.library's depth bitmaps: width
         * padded so the pitch is whole 64-pixel tile pairs (128 bytes),
         * height identical to the color target.  The friend bitmap must
         * match the requested depth: a 32-bit screen friend makes P96
         * hand out a 32-bit bitmap regardless of the rgbFormat argument
         * (r200_surface.c AllocateSeedBitMap pattern). */
        ULONG depthWidth = (width + 63UL) & ~63UL;
        struct TagItem seedModeTags[] = {
            {P96_BESTMODE_FORMATS_ALLOWED, RGBFF_R5G6B5PC},
            {P96_BESTMODE_NOMINAL_WIDTH, 640UL},
            {P96_BESTMODE_NOMINAL_HEIGHT, 480UL},
            {P96_BESTMODE_DEPTH, 16UL},
            {TAG_DONE, 0}
        };
        struct TagItem seedScreenTags[] = {
            {P96_SCREEN_DISPLAY_ID, 0},
            {P96_SCREEN_DEPTH, 16UL},
            {P96_SCREEN_RGB_FORMAT, RGBFB_R5G6B5PC},
            {P96_SCREEN_SHOW_TITLE, FALSE},
            {P96_SCREEN_BEHIND, TRUE},
            {P96_SCREEN_QUIET, TRUE},
            {TAG_DONE, 0}
        };
        struct Screen *seedScreen;
        struct BitMap *seedFriend;
        ULONG seedMode = p96BestModeIDTagList(seedModeTags);

        if (seedMode == 0xffffffffUL) {
            result = Fail("seed_mode");
            goto out;
        }
        seedScreenTags[0].ti_Data = seedMode;
        seedScreen = p96OpenScreenTagList(seedScreenTags);
        if (!seedScreen) {
            result = Fail("seed_screen");
            goto out;
        }
        seedFriend = p96AllocBitMap(16UL, 16UL, 16UL,
                                    BMF_CLEAR | BMF_DISPLAYABLE,
                                    seedScreen->RastPort.BitMap,
                                    RGBFB_R5G6B5PC);
        p96CloseScreen(seedScreen);
        if (!seedFriend) {
            result = Fail("alloc_seed_bitmap");
            goto out;
        }
        depthBitmap = p96AllocBitMap(depthWidth, height, 16UL,
                                     BMF_CLEAR | BMF_DISPLAYABLE,
                                     seedFriend, RGBFB_R5G6B5PC);
        p96FreeBitMap(seedFriend);
        if (!depthBitmap) {
            result = Fail("alloc_depth_bitmap");
            goto out;
        }
        printf("R3DREPLAY depthattrs w=%lu h=%lu bpr=%lu bpp=%lu "
               "fmt=%lu mem=%08lx onboard=%lu p96=%lu\n",
               (unsigned long)p96GetBitMapAttr(depthBitmap,
                                               P96_BITMAP_WIDTH),
               (unsigned long)p96GetBitMapAttr(depthBitmap,
                                               P96_BITMAP_HEIGHT),
               (unsigned long)p96GetBitMapAttr(depthBitmap,
                                               P96_BITMAP_BYTES_PER_ROW),
               (unsigned long)p96GetBitMapAttr(depthBitmap,
                                               P96_BITMAP_BYTES_PER_PIXEL),
               (unsigned long)p96GetBitMapAttr(depthBitmap,
                                               P96_BITMAP_RGB_FORMAT),
               (unsigned long)p96GetBitMapAttr(depthBitmap,
                                               P96_BITMAP_MEMORY),
               (unsigned long)p96GetBitMapAttr(depthBitmap,
                                               P96_BITMAP_IS_ON_BOARD),
               (unsigned long)p96GetBitMapAttr(depthBitmap,
                                               P96_BITMAP_IS_P96));
        depthSurface.Size = sizeof(depthSurface);
        if (!Radeon3DImportBitMap(device, depthBitmap, &depthSurface)) {
            result = Fail("import_depth_bitmap");
            goto out;
        }
        if (depthSurface.Version != RADEON3D_SURFACE_VERSION) {
            result = Fail("import_depth_version");
            goto out;
        }
        if (depthSurface.Format != RADEON3D_FORMAT_R5G6B5PC) {
            result = Fail("import_depth_format");
            goto out;
        }
        printf("R3DREPLAY depth cpu=%08lx gpu=%08lx pitch=%lu "
               "width=%lu\n",
               (unsigned long)depthSurface.CpuAddress,
               (unsigned long)depthSurface.GpuAddress,
               (unsigned long)depthSurface.Pitch,
               (unsigned long)depthWidth);
    }

    for (segIndex = 0; segIndex < SEGMENT_COUNT; ++segIndex) {
        segment[segIndex].Size = sizeof(segment[segIndex]);
        if (!Radeon3DAllocSegment(device, SEGMENT_BYTES,
                                  &segment[segIndex]) ||
            segment[segIndex].Version != RADEON3D_SEGMENT_VERSION ||
            !segment[segIndex].CpuAddress) {
            result = Fail("alloc_segment");
            goto out;
        }
        vertices = (volatile ULONG *)segment[segIndex].CpuAddress;
        FillSegment(vertices, segIndex * RINGS_PER_SEGMENT);
        printf("R3DREPLAY segment id=%lu gpu=%08lx bytes=%lu "
               "verts=%lu\n",
               (unsigned long)segment[segIndex].Id,
               (unsigned long)segment[segIndex].GpuAddress,
               (unsigned long)segment[segIndex].Bytes,
               (unsigned long)VERTS_PER_SEGMENT);
    }

    BuildClearRecord(clearRecord, (ULONG)surface.Handle,
                     (ULONG)depthSurface.Handle, width, height, useZ);
    for (rotation = 0; rotation < FRAME_ROTATIONS; ++rotation) {
        BuildFrameHeader(headers[rotation], (ULONG)surface.Handle,
                         useZ ? (ULONG)depthSurface.Handle : 0UL,
                         kMatrices[rotation], width, height, useZ);
        for (segIndex = 0; segIndex < SEGMENT_COUNT; ++segIndex) {
            draws[segIndex].OffsetBytes = 0;
            draws[segIndex].VertexCount = VERTS_PER_SEGMENT;
            batches[rotation][segIndex].Size =
                sizeof(batches[rotation][segIndex]);
            batches[rotation][segIndex].Version =
                RADEON3D_STATE_BATCH_VERSION;
            batches[rotation][segIndex].Generation = info.Generation;
            batches[rotation][segIndex].SegmentId =
                segment[segIndex].Id;
            batches[rotation][segIndex].Primitive =
                RADEON3D_EXEC_DRAW_QUADS;
            batches[rotation][segIndex].Header = headers[rotation];
            batches[rotation][segIndex].HeaderDwords = HEADER_DWORDS;
            batches[rotation][segIndex].Draws = &draws[segIndex];
            batches[rotation][segIndex].DrawCount = 1;
            batches[rotation][segIndex].Flags = RADEON3D_SUBMIT_FENCE;
        }
    }

    /* Validation frame: fence + wait so a rejected record shape fails
     * loudly here instead of silently dropping frames in the loop. */
    {
        ULONG validateFence = 0;
        BOOL ok;

        ok = Radeon3DExecute(device, clearRecord,
                             RADEON3D_EXEC_CLEAR_DWORDS,
                             RADEON3D_SUBMIT_FENCE, &validateFence);
        if (!ok || !validateFence ||
            !Radeon3DWaitFence(device, validateFence, 10000UL)) {
            result = Fail("validate_clear");
            goto out;
        }
        for (segIndex = 0; segIndex < SEGMENT_COUNT; ++segIndex) {
            validateFence = 0;
            ok = Radeon3DCommitStateBatch(device, &batches[0][segIndex],
                                          &validateFence);
            if (!ok || !validateFence ||
                !Radeon3DWaitFence(device, validateFence, 10000UL)) {
                info.Size = sizeof(info);
                if (Radeon3DGetInfo(device, &info))
                    printf("R3DREPLAY commit_fail_stage=%lu "
                           "fence=%08lx\n",
                           (unsigned long)info.CommitFailStage,
                           (unsigned long)validateFence);
                result = Fail("validate_batch");
                goto out;
            }
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
        (void)Radeon3DExecute(device, clearRecord,
                              RADEON3D_EXEC_CLEAR_DWORDS, 0UL, NULL);
        for (segIndex = 0; segIndex < SEGMENT_COUNT; ++segIndex)
            (void)Radeon3DCommitStateBatch(device,
                                           &batches[frame % FRAME_ROTATIONS][segIndex],
                                           NULL);
    }

    ReadEClock(&startClock);
    for (frame = 0; frame < frames; ++frame) {
        rotation = frame % FRAME_ROTATIONS;
        if (!noClear &&
            !Radeon3DExecute(device, clearRecord,
                             RADEON3D_EXEC_CLEAR_DWORDS, 0UL, NULL)) {
            ++failedSubmits;
            if (failedSubmits == 1UL && Radeon3DGetInfo(device, &info))
                printf("R3DREPLAY loopfail call=clear frame=%lu "
                       "stage=%lu\n",
                       (unsigned long)frame,
                       (unsigned long)info.CommitFailStage);
        }
        if (!clearOnly) {
            ULONG drain = pace && ((frame % pace) == pace - 1UL);
            for (segIndex = 0; segIndex < SEGMENT_COUNT; ++segIndex) {
                ULONG fence = 0;
                BOOL ok = Radeon3DCommitStateBatch(
                    device, &batches[rotation][segIndex],
                    (drain && segIndex == SEGMENT_COUNT - 1UL) ? &fence
                                                               : NULL);
                if (!ok) {
                    ++failedSubmits;
                    if (failedSubmits == 1UL &&
                        Radeon3DGetInfo(device, &info))
                        printf("R3DREPLAY loopfail call=batch frame=%lu "
                               "stage=%lu\n",
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
    printf("R3DREPLAY_SUMMARY version=3 mode=%s target=%s z=%s pace=%lu "
           "width=%lu height=%lu depth=%lu frames=%lu elapsed_ticks=%lu "
           "eclock_hz=%lu fps_x1000=%lu calls=%lu fail=%lu\n",
           clearOnly ? "clearonly" : (noClear ? "noclear" : "full"),
           offscreen ? "offscreen" : "screen",
           useZ ? "on" : "off",
           (unsigned long)pace,
           (unsigned long)width, (unsigned long)height,
           (unsigned long)depth, (unsigned long)frame,
           (unsigned long)elapsed,
           (unsigned long)eclockHz,
           (unsigned long)fpsX1000,
           (unsigned long)(frame * (clearOnly ? 1UL :
                                    (noClear ? SEGMENT_COUNT :
                                     SEGMENT_COUNT + 1UL))),
           (unsigned long)failedSubmits);
    printf("R3DREPLAY status=ok\n");

out:
    for (segIndex = 0; segIndex < SEGMENT_COUNT; ++segIndex) {
        if (device && segment[segIndex].CpuAddress)
            (void)Radeon3DFreeSegment(device, segment[segIndex].Id);
    }
    if (depthBitmap)
        FreeBitMap(depthBitmap);
    if (timerIO) {
        if (timerIO->tr_node.io_Device)
            CloseDevice((struct IORequest *)timerIO);
        DeleteIORequest((struct IORequest *)timerIO);
    }
    if (timerPort)
        DeleteMsgPort(timerPort);
    if (screen)
        p96CloseScreen(screen);
    if (bitmap && offscreen)
        FreeBitMap(bitmap);
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
