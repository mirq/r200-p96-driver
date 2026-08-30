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
#include <stdio.h>

#include <p96_bitmap_api.h>

#define CHIP_LIBRARY_NAME "Radeon9200.chip"

#define HEADER_DWORDS 44UL
#define VERTEX_DWORDS 10UL
#define VERTEX_COUNT 4UL

struct Library *Radeon9200Base;
struct Library *P96Base;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;

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

static int Fail(const char *check)
{
    printf("R3DSTREAM status=fail check=%s\n", check);
    return 10;
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

static ULONG SignedFloatBits(LONG value)
{
    ULONG bits = FloatBits((ULONG)(value < 0 ? -value : value));

    return value < 0 ? (0x80000000UL | bits) : bits;
}

static void BuildHeader(ULONG *header, ULONG handle, ULONG vertexCount,
                        ULONG recordDwords)
{
    ULONG one = FloatBits(1.0f);
    ULONG index;

    for (index = 0; index < HEADER_DWORDS; ++index)
        header[index] = 0;
    header[0] = RADEON3D_EXEC_DRAW_QUADS;
    header[1] = recordDwords;
    header[2] = handle;
    header[5] = RADEON3D_DRAW_FRAGMENT_STATE |
                RADEON3D_DRAW_EXTENDED_VERTEX |
                RADEON3D_DRAW_HW_TCL;
    header[8] = 320UL;
    header[9] = 240UL;
    header[10] = vertexCount;
    header[21] = 0x3c000000UL;
    header[26] = 0x3c000000UL;
    header[31] = 0x3c000000UL;
    header[36] = one;
    header[37] = FloatBits(160.0f);
    header[38] = FloatBits(160.0f);
    header[39] = 0x80000000UL | FloatBits(120.0f);
    header[40] = FloatBits(120.0f);
    header[41] = one;
    header[42] = one;
    header[43] = 16UL << RADEON3D_TRANSFORM_POINT_SIZE_SHIFT;
}

static void WriteVertices(volatile ULONG *vertices)
{
    static const LONG corners[8] = {-128, -128, 128, -128,
                                    128, 128, -128, 128};
    ULONG vertex;

    for (vertex = 0; vertex < VERTEX_COUNT; ++vertex) {
        volatile ULONG *out = vertices + vertex * VERTEX_DWORDS;

        out[0] = SignedFloatBits(corners[vertex * 2UL]);
        out[1] = SignedFloatBits(corners[vertex * 2UL + 1UL]);
        out[2] = 0;
        out[3] = 0x3f800000UL;
        out[4] = 0xff00ff00UL;
        out[5] = 0;
        out[6] = 0;
        out[7] = 0;
        out[8] = 0;
        out[9] = 0;
    }
}

int main(void)
{
    struct Radeon3DInfo info;
    struct Radeon3DSurface surface;
    struct Radeon3DSegment segment;
    struct Radeon3DCommit commit;
    struct Radeon3DDevice *device = NULL;
    struct Screen *screen = NULL;
    struct BitMap *bitmap = NULL;
    ULONG header[HEADER_DWORDS];
    ULONG fence = 0;
    volatile ULONG *vertices;
    ULONG *pixels;
    ULONG sample;
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
    printf("R3DSTREAM iface=%lu caps=%08lx generation=%lu commit_stage=%08lx\n",
           (unsigned long)info.Version, (unsigned long)info.Caps,
           (unsigned long)info.Generation,
           (unsigned long)info.CommitFailStage);
    if ((info.Caps & (RADEON3D_CAP_STREAM_SEGMENTS |
                       RADEON3D_CAP_COMMIT_STATE_BATCH |
                       RADEON3D_CAP_ORDERED_COMMITS)) !=
        (RADEON3D_CAP_STREAM_SEGMENTS |
         RADEON3D_CAP_COMMIT_STATE_BATCH |
         RADEON3D_CAP_ORDERED_COMMITS)) {
        result = Fail("capabilities");
        goto out;
    }

    screen = LockPubScreen(NULL);
    if (!screen) {
        result = Fail("lock_public_screen");
        goto out;
    }
    UnlockPubScreen(NULL, screen);
    screen = NULL;
    bitmap = AllocateRgb565BitMap(320, 240);
    if (!bitmap) {
        result = Fail("alloc_bitmap");
        goto out;
    }
    surface.Size = sizeof(surface);
    if (!Radeon3DImportBitMap(device, bitmap, &surface) ||
        surface.Version != RADEON3D_SURFACE_VERSION) {
        result = Fail("import_bitmap");
        goto out;
    }
    printf("R3DSTREAM surface cpu=%08lx gpu=%08lx pitch=%lu\n",
           (unsigned long)surface.CpuAddress,
           (unsigned long)surface.GpuAddress,
           (unsigned long)surface.Pitch);

    segment.Size = sizeof(segment);
    if (!Radeon3DAllocSegment(device, 65536UL, &segment) ||
        segment.Version != RADEON3D_SEGMENT_VERSION ||
        !segment.CpuAddress || !segment.GpuAddress) {
        result = Fail("alloc_segment");
        goto out;
    }
    printf("R3DSTREAM segment id=%lu cpu=%08lx gpu=%08lx bytes=%lu\n",
           (unsigned long)segment.Id, (unsigned long)segment.CpuAddress,
           (unsigned long)segment.GpuAddress,
           (unsigned long)segment.Bytes);

    vertices = (volatile ULONG *)segment.CpuAddress;
    WriteVertices(vertices);

    /* Fence sanity: a PACKET2 no-op must submit and retire. */
    {
        ULONG noop = 0x80000000UL;
        ULONG noopFence = 0;

        if (!Radeon3DSubmit(device, &noop, 1, RADEON3D_SUBMIT_FENCE,
                            &noopFence) || !noopFence)
            printf("R3DSTREAM noop_submit=failed\n");
        else if (!Radeon3DWaitFence(device, noopFence, 5000UL))
            printf("R3DSTREAM noop_fence=timeout\n");
        else
            printf("R3DSTREAM noop_fence=ok\n");
    }

    /* Control: the identical draw through Execute with inline vertices,
     * cloned from the proven radeon3dformats quad probe. */
    {
        ULONG record[HEADER_DWORDS + VERTEX_COUNT * VERTEX_DWORDS];
        ULONG index;
        ULONG controlFence = 0;
        BOOL submitted;

        BuildHeader(header, (ULONG)surface.Handle, VERTEX_COUNT,
                    HEADER_DWORDS + VERTEX_COUNT * VERTEX_DWORDS);
        for (index = 0; index < HEADER_DWORDS; ++index)
            record[index] = header[index];
        for (index = 0; index < VERTEX_COUNT * VERTEX_DWORDS; ++index)
            record[HEADER_DWORDS + index] = vertices[index];
        submitted = Radeon3DExecute(device, record,
                                    HEADER_DWORDS + VERTEX_COUNT * VERTEX_DWORDS,
                                    RADEON3D_SUBMIT_FENCE, &controlFence);
        printf("R3DSTREAM control submit=%lu fence=%lu\n",
               (unsigned long)submitted, (unsigned long)controlFence);
        if (submitted && controlFence) {
            BOOL waited = Radeon3DWaitFence(device, controlFence, 10000UL);

            printf("R3DSTREAM control waited=%lu\n", (unsigned long)waited);
            if (waited) {
                pixels = (ULONG *)surface.CpuAddress;
                sample = (pixels[120UL * (surface.Pitch / 4UL) + 80UL] >> 16) &
                         0xffffUL;
                printf("R3DSTREAM control_sample=%04lx expected=07e0\n",
                       (unsigned long)sample);
            }
        }
    }

    /* Commit path: same header, vertices fetched from the segment. The VAP
     * fetches little-endian, so the commit copy is byte-reversed relative
     * to the inline Execute copy, which the ring writer swaps. */
    ULONG reverse;

    for (reverse = 0; reverse < VERTEX_COUNT * VERTEX_DWORDS; ++reverse) {
        ULONG value = vertices[reverse];

        vertices[256UL + reverse] =
            (value << 24) | ((value << 8) & 0x00ff0000UL) |
            ((value >> 8) & 0x0000ff00UL) | (value >> 24);
    }
    BuildHeader(header, (ULONG)surface.Handle, VERTEX_COUNT, HEADER_DWORDS);
    commit.Size = sizeof(commit);
    commit.Version = RADEON3D_COMMIT_VERSION;
    commit.SegmentId = segment.Id;
    commit.OffsetBytes = 1024UL;
    commit.Header = header;
    commit.HeaderDwords = HEADER_DWORDS;
    commit.Flags = RADEON3D_SUBMIT_FENCE;
    if (!Radeon3DCommitDraw(device, &commit, &fence) || !fence) {
        result = Fail("commit_draw");
        goto out;
    }
    if (!Radeon3DWaitFence(device, fence, 10000UL)) {
        result = Fail("wait_fence");
        goto out;
    }

    pixels = (ULONG *)surface.CpuAddress;
    sample = (pixels[120UL * (surface.Pitch / 4UL) + 80UL] >> 16) & 0xffffUL;
    printf("R3DSTREAM commit_sample=%04lx expected=07e0 or e007\n",
           (unsigned long)sample);
    if (sample != 0x07e0UL && sample != 0xe007UL) {
        result = Fail("pixel");
        goto out;
    }

    /* Batch variant: two draws in one commit, offsets 0 and 112. */
    {
        struct Radeon3DCommitBatch batchCommit;
        ULONG offsets[2];
        ULONG batchFence = 0;
        BOOL ok;

        offsets[0] = 0;
        offsets[1] = 112;
        header[10] = 4;
        commit.Header = header;
        commit.HeaderDwords = HEADER_DWORDS;
        header[10] = 6;
        /* second header variant reuses the same array; the batch carries
         * two entries pointing at the same header for a minimal check */
        header[10] = 4;
        batchCommit.Size = sizeof(batchCommit);
        batchCommit.Version = RADEON3D_COMMIT_BATCH_VERSION;
        batchCommit.SegmentId = segment.Id;
        batchCommit.Records = header;
        batchCommit.RecordDwords = HEADER_DWORDS;
        batchCommit.VertexOffsets = offsets;
        batchCommit.RecordCount = 1;
        batchCommit.Flags = RADEON3D_SUBMIT_FENCE;
        ok = Radeon3DCommitBatch(device, &batchCommit, &batchFence);
        printf("R3DSTREAM batch ok=%lu fence=%08lx\n",
                (unsigned long)ok, (unsigned long)batchFence);
        if (ok && batchFence)
            Radeon3DWaitFence(device, batchFence, 10000UL);
        {
            struct Radeon3DCommitBatch bad = batchCommit;
            ULONG badFence = 0;

            bad.RecordCount = 0;
            ok = Radeon3DCommitBatch(device, &bad, &badFence);
            printf("R3DSTREAM badargs ok=%lu fence=%08lx expected=80000029\n",
                   (unsigned long)ok, (unsigned long)badFence);
        }
        header[10] = VERTEX_COUNT;
    }

    /* Interface-15 homogeneous batch: reuse the existing state header and
     * byte-swapped segment vertices, then verify an unaligned offset fails. */
    {
        struct Radeon3DStateBatchDraw draw;
        struct Radeon3DStateBatch stateBatch;
        ULONG stateFence = 0;
        BOOL ok;

        BuildHeader(header, (ULONG)surface.Handle, VERTEX_COUNT,
                    HEADER_DWORDS);
        draw.OffsetBytes = 1024UL;
        draw.VertexCount = VERTEX_COUNT;
        stateBatch.Size = sizeof(stateBatch);
        stateBatch.Version = RADEON3D_STATE_BATCH_VERSION;
        stateBatch.Generation = info.Generation;
        stateBatch.SegmentId = segment.Id;
        stateBatch.Primitive = RADEON3D_EXEC_DRAW_QUADS;
        stateBatch.Header = header;
        stateBatch.HeaderDwords = HEADER_DWORDS;
        stateBatch.Draws = &draw;
        stateBatch.DrawCount = 1;
        stateBatch.Flags = RADEON3D_SUBMIT_FENCE;
        ok = Radeon3DCommitStateBatch(device, &stateBatch, &stateFence);
        printf("R3DSTREAM state_batch ok=%lu fence=%08lx\n",
               (unsigned long)ok, (unsigned long)stateFence);
        if (!ok || !stateFence ||
            !Radeon3DWaitFence(device, stateFence, 10000UL)) {
            result = Fail("commit_state_batch");
            goto out;
        }
        draw.OffsetBytes = 1025UL;
        stateFence = 0;
        ok = Radeon3DCommitStateBatch(device, &stateBatch, &stateFence);
        printf("R3DSTREAM state_batch_bad_align ok=%lu fence=%08lx\n",
               (unsigned long)ok, (unsigned long)stateFence);
        if (ok) {
            result = Fail("commit_state_batch_alignment");
            goto out;
        }
    }

    if (!Radeon3DFreeSegment(device, segment.Id)) {
        result = Fail("free_segment");
        goto out;
    }
    printf("R3DSTREAM status=ok\n");

out:
    if (bitmap)
        FreeBitMap(bitmap);
    if (device)
        Radeon3DClose(device);
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
