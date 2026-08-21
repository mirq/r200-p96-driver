#include <exec/libraries.h>
#include <exec/types.h>
#include <graphics/gfx.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/radeon3d.h>
#include <stdio.h>

#include <p96_bitmap_api.h>

#define CHIP_LIBRARY_NAME "Radeon9200.chip"
#define WIDTH 320UL
#define HEIGHT 240UL

struct Library *Radeon9200Base;
struct Library *P96Base;
struct IntuitionBase *IntuitionBase;

static ULONG FloatBits(ULONG value)
{
    ULONG top = 0, scan = value;
    if (!value) return 0;
    while (scan >>= 1) ++top;
    return ((127UL + top) << 23) |
           ((value << (23UL - top)) & 0x007fffffUL);
}

static struct BitMap *AllocateTarget(ULONG depth, ULONG format,
                                     ULONG formatMask)
{
    struct TagItem modeTags[] = {
        {P96_BESTMODE_FORMATS_ALLOWED, 0},
        {P96_BESTMODE_NOMINAL_WIDTH, 640UL},
        {P96_BESTMODE_NOMINAL_HEIGHT, 480UL},
        {P96_BESTMODE_DEPTH, 0}, {TAG_DONE, 0}
    };
    struct TagItem screenTags[] = {
        {P96_SCREEN_DISPLAY_ID, 0}, {P96_SCREEN_DEPTH, 0},
        {P96_SCREEN_RGB_FORMAT, 0}, {P96_SCREEN_SHOW_TITLE, FALSE},
        {P96_SCREEN_BEHIND, TRUE}, {P96_SCREEN_QUIET, TRUE},
        {TAG_DONE, 0}
    };
    struct Screen *screen;
    struct BitMap *bitmap;
    ULONG mode;

    modeTags[0].ti_Data = formatMask;
    modeTags[3].ti_Data = depth;
    screenTags[1].ti_Data = depth;
    screenTags[2].ti_Data = format;
    mode = p96BestModeIDTagList(modeTags);
    if (mode == 0xffffffffUL) return NULL;
    screenTags[0].ti_Data = mode;
    screen = p96OpenScreenTagList(screenTags);
    if (!screen) return NULL;
    bitmap = p96AllocBitMap(WIDTH, HEIGHT, depth,
                            BMF_CLEAR | BMF_DISPLAYABLE,
                            screen->RastPort.BitMap, format);
    p96CloseScreen(screen);
    return bitmap;
}

static BOOL DrawTriangle(struct Radeon3DDevice *device,
                         struct Radeon3DSurface *surface)
{
    ULONG records[RADEON3D_EXEC_DRAW_HEADER_DWORDS +
                  3UL * RADEON3D_EXEC_VERTEX_DWORDS];
    ULONG *vertex;
    ULONG index;
    ULONG fence = 0;
    static const ULONG positions[6] = {32, 32, 288, 32, 160, 208};

    for (index = 0; index < sizeof(records) / sizeof(records[0]); ++index)
        records[index] = 0;
    records[0] = RADEON3D_EXEC_DRAW_TRIANGLES;
    records[1] = sizeof(records) / sizeof(records[0]);
    records[2] = (ULONG)surface->Handle;
    records[6] = 0;
    records[7] = 0;
    records[8] = WIDTH;
    records[9] = HEIGHT;
    records[10] = 3;
    for (index = 0; index < 3UL; ++index) {
        vertex = records + RADEON3D_EXEC_DRAW_HEADER_DWORDS +
                 index * RADEON3D_EXEC_VERTEX_DWORDS;
        vertex[0] = FloatBits(positions[index * 2UL]);
        vertex[1] = FloatBits(positions[index * 2UL + 1UL]);
        vertex[2] = 0;
        vertex[3] = 0;
        vertex[4] = 0;
        vertex[5] = 0xff3366ccUL;
    }
    return Radeon3DExecute(device, records,
                           sizeof(records) / sizeof(records[0]),
                           RADEON3D_SUBMIT_FENCE, &fence) && fence &&
           Radeon3DWaitFence(device, fence, 1000UL);
}

static BOOL DrawQuadList(struct Radeon3DDevice *device,
                         struct Radeon3DSurface *surface)
{
    ULONG records[RADEON3D_EXEC_DRAW_HEADER_DWORDS +
                  8UL * RADEON3D_EXEC_VERTEX_DWORDS];
    ULONG *vertex;
    ULONG index;
    ULONG fence = 0;
    static const ULONG positions[16] = {
        20, 20, 120, 20, 120, 100, 20, 100,
        180, 120, 300, 120, 300, 220, 180, 220
    };

    for (index = 0; index < sizeof(records) / sizeof(records[0]); ++index)
        records[index] = 0;
    records[0] = RADEON3D_EXEC_DRAW_QUADS;
    records[1] = sizeof(records) / sizeof(records[0]);
    records[2] = (ULONG)surface->Handle;
    records[8] = WIDTH;
    records[9] = HEIGHT;
    records[10] = 8;
    for (index = 0; index < 8UL; ++index) {
        vertex = records + RADEON3D_EXEC_DRAW_HEADER_DWORDS +
                 index * RADEON3D_EXEC_VERTEX_DWORDS;
        vertex[0] = FloatBits(positions[index * 2UL]);
        vertex[1] = FloatBits(positions[index * 2UL + 1UL]);
        vertex[5] = index < 4UL ? 0xffff0000UL : 0xff00ff00UL;
    }
    return Radeon3DExecute(device, records,
                           sizeof(records) / sizeof(records[0]),
                           RADEON3D_SUBMIT_FENCE, &fence) && fence &&
           Radeon3DWaitFence(device, fence, 1000UL);
}

static ULONG ReadSample32(const struct Radeon3DSurface *surface,
                          ULONG x, ULONG y)
{
    volatile UBYTE *pixel = (volatile UBYTE *)surface->CpuAddress +
                            y * surface->Pitch + x * 4UL;
    return ((ULONG)pixel[0] << 24) | ((ULONG)pixel[1] << 16) |
           ((ULONG)pixel[2] << 8) | (ULONG)pixel[3];
}

static int TestQuadList(struct Radeon3DDevice *device)
{
    struct Radeon3DSurface surface;
    struct BitMap *bitmap = AllocateTarget(32UL, RGBFB_B8G8R8A8,
                                           RGBFF_B8G8R8A8);
    ULONG first, second;
    int result = 0;

    surface.Version = 0;
    surface.Handle = NULL;
    surface.Size = sizeof(surface);
    if (!bitmap || !Radeon3DImportBitMap(device, bitmap, &surface)) {
        result = 80;
        goto out;
    }
    if (!DrawQuadList(device, &surface)) {
        result = 81;
        goto out;
    }
    first = ReadSample32(&surface, 70UL, 60UL);
    second = ReadSample32(&surface, 240UL, 170UL);
    printf("R3DQUADS count=2 first=%08lx second=%08lx\n", first, second);
    if (first != 0x0000ff00UL || second != 0x00ff0000UL)
        result = 82;
out:
    if (surface.Handle) Radeon3DReleaseSurface(device, &surface);
    if (bitmap) p96FreeBitMap(bitmap);
    return result;
}

static int TestLegacyQuadRejection(void)
{
    struct Radeon3DInfo info;
    struct Radeon3DDevice *device;
    struct Radeon3DSurface surface;
    struct BitMap *bitmap;
    int result = 0;

    info.Size = sizeof(info);
    device = Radeon3DOpen(7UL, &info);
    if (!device || info.Version != 7UL ||
        (info.Caps & RADEON3D_CAP_NATIVE_QUAD_LISTS))
        return 83;
    bitmap = AllocateTarget(32UL, RGBFB_B8G8R8A8, RGBFF_B8G8R8A8);
    surface.Version = 0;
    surface.Handle = NULL;
    surface.Size = sizeof(surface);
    if (!bitmap || !Radeon3DImportBitMap(device, bitmap, &surface) ||
        DrawQuadList(device, &surface))
        result = 84;
    if (surface.Handle) Radeon3DReleaseSurface(device, &surface);
    if (bitmap) p96FreeBitMap(bitmap);
    Radeon3DClose(device);
    return result;
}

static int TestFormat(struct Radeon3DDevice *device, ULONG depth,
                      ULONG p96Format, ULONG formatMask,
                      ULONG expectedFormat)
{
    struct Radeon3DSurface surface;
    struct BitMap *bitmap = AllocateTarget(depth, p96Format, formatMask);
    volatile UBYTE *pixels;
    ULONG offset, sample = 0, index;
    int result = 0;

    surface.Version = 0;
    surface.Handle = NULL;
    if (!bitmap) return 20 + (int)depth;
    surface.Size = sizeof(surface);
    if (!Radeon3DImportBitMap(device, bitmap, &surface) ||
        surface.Format != expectedFormat) {
        result = 30 + (int)depth;
        goto out;
    }
    if (!DrawTriangle(device, &surface)) {
        result = 40 + (int)depth;
        goto out;
    }
    pixels = (volatile UBYTE *)surface.CpuAddress;
    offset = 120UL * surface.Pitch + 160UL * (depth >> 3);
    for (index = 0; index < (depth >> 3); ++index)
        sample = (sample << 8) | pixels[offset + index];
    printf("R3DFORMAT depth=%lu format=%lu pitch=%lu sample=%08lx\n",
           depth, surface.Format, surface.Pitch, sample);
    if ((depth == 8UL &&
         ((((sample >> 5) & 7UL) < 1UL ||
           ((sample >> 5) & 7UL) > 2UL) ||
          (((sample >> 2) & 7UL) < 2UL ||
           ((sample >> 2) & 7UL) > 3UL) ||
          ((sample & 3UL) < 2UL))) ||
        (depth == 16UL && sample != 0x3933UL) ||
        (depth == 32UL && sample != 0xcc663300UL))
        result = 50 + (int)depth;
out:
    if (surface.Handle) Radeon3DReleaseSurface(device, &surface);
    p96FreeBitMap(bitmap);
    return result;
}

static int TestLegacyRejection(void)
{
    struct Radeon3DInfo info;
    struct Radeon3DDevice *device;
    struct Radeon3DSurface surface;
    struct BitMap *bitmap;
    int result = 0;

    info.Size = sizeof(info);
    device = Radeon3DOpen(5UL, &info);
    if (!device || info.Version != 5UL ||
        (info.Caps & RADEON3D_CAP_COLOR_TARGET_FORMATS))
        return 70;
    bitmap = AllocateTarget(32UL, RGBFB_B8G8R8A8, RGBFF_B8G8R8A8);
    surface.Version = 0;
    surface.Handle = NULL;
    surface.Size = sizeof(surface);
    if (!bitmap || !Radeon3DImportBitMap(device, bitmap, &surface) ||
        DrawTriangle(device, &surface))
        result = 71;
    if (surface.Handle) Radeon3DReleaseSurface(device, &surface);
    if (bitmap) p96FreeBitMap(bitmap);
    Radeon3DClose(device);
    return result;
}

int main(void)
{
    struct Radeon3DInfo info;
    struct Radeon3DDevice *device = NULL;
    int result = 0;

    Radeon9200Base = OpenLibrary(CHIP_LIBRARY_NAME,
                                 RADEON3D_LIBRARY_VERSION);
    P96Base = OpenLibrary("Picasso96API.library", 2);
    IntuitionBase = (struct IntuitionBase *)
        OpenLibrary("intuition.library", 39);
    if (!Radeon9200Base || !P96Base || !IntuitionBase) {
        result = 10;
        goto out;
    }
    info.Size = sizeof(info);
    device = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
    if (!device || info.Version != RADEON3D_IFACE_VERSION ||
        !(info.Caps & RADEON3D_CAP_COLOR_TARGET_FORMATS) ||
        !(info.Caps & RADEON3D_CAP_NATIVE_QUAD_LISTS)) {
        result = 11;
        goto out;
    }
    result = TestFormat(device, 8UL, RGBFB_CLUT, RGBFF_CLUT,
                        RADEON3D_FORMAT_CLUT8);
    if (!result)
        result = TestFormat(device, 16UL, RGBFB_R5G6B5PC,
                            RGBFF_R5G6B5PC,
                            RADEON3D_FORMAT_R5G6B5PC);
    if (!result)
        result = TestFormat(device, 32UL, RGBFB_B8G8R8A8,
                            RGBFF_B8G8R8A8,
                            RADEON3D_FORMAT_B8G8R8A8);
    if (!result)
        result = TestQuadList(device);
    Radeon3DClose(device);
    device = NULL;
    if (!result)
        result = TestLegacyRejection();
    if (!result)
        result = TestLegacyQuadRejection();
out:
    if (device) Radeon3DClose(device);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (P96Base) CloseLibrary(P96Base);
    if (Radeon9200Base) CloseLibrary(Radeon9200Base);
    printf("R3DFORMATS status=%s result=%d legacy_v5=%s legacy_v7_quad=%s\n",
           result ? "fail" : "ok", result,
           result ? "unknown" : "rejected",
           result ? "unknown" : "rejected");
    return result;
}
