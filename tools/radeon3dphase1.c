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
#define PACKET2           0x80000000UL
#define WRAP_SUBMITS      70UL
#define TRIANGLE_VERTICES 6UL
#define TRIANGLE_DWORDS   (46UL + TRIANGLE_VERTICES * 3UL)

#define PACKET0(reg)      ((ULONG)(reg) >> 2)

static void EmitRegister(ULONG *commands, ULONG *count,
                         ULONG reg, ULONG value)
{
    commands[(*count)++] = PACKET0(reg);
    commands[(*count)++] = value;
}

static ULONG UnsignedFloatBits(ULONG value)
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

static ULONG BuildTriangles(ULONG *commands, ULONG colorAddress,
                            ULONG colorPitch, ULONG width, ULONG height,
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
    EmitRegister(commands, &count, 0x1c40UL, colorAddress);
    EmitRegister(commands, &count, 0x1c48UL, colorPitch / 2UL);

    commands[count++] = 0xc0003500UL | ((vertexCount * 3UL) << 16);
    commands[count++] = (vertexCount << 16) | 0x00000034UL;
    for (index = 0; index < vertexCount * 3UL; ++index)
        commands[count++] = vertices[index];
    return count;
}

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

struct Library *Radeon9200Base;
struct Library *P96Base;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;

static int Fail(const char *check, ULONG iteration)
{
    printf("R3DPHASE1 status=fail check=%s iteration=%lu\n",
           check, iteration);
    return 10;
}

int main(void)
{
    struct Radeon3DInfo info;
    struct Radeon3DSurface surface;
    struct Radeon3DSurface retainedSurface;
    struct Radeon3DSurface invalidSurface;
    struct Radeon3DDevice *device = NULL;
    struct Screen *screen = NULL;
    struct BitMap *bitmap = NULL;
    struct BitMap *planar = NULL;
    ULONG *commands = NULL;
    ULONG fence = 0;
    ULONG previousFence = 0;
    ULONG surfaceCpu = 0;
    ULONG surfaceGpu = 0;
    ULONG surfacePitch = 0;
    ULONG surfaceFormat = 0;
    ULONG triangleBytes = 0;
    ULONG triangleDwords;
    ULONG triangleSample = 0;
    ULONG transitionSample = 0;
    static const ULONG vertices[TRIANGLE_VERTICES * 3UL] = {
        0x42000000UL, 0x42000000UL, 0xff0000ffUL,
        0x43900000UL, 0x42000000UL, 0xff00ff00UL,
        0x43200000UL, 0x43500000UL, 0xffff0000UL,
        0x42800000UL, 0x43000000UL, 0xffffffffUL,
        0x43800000UL, 0x43000000UL, 0xffffffffUL,
        0x43200000UL, 0x43600000UL, 0xffffffffUL
    };
    ULONG index;
    int result = 0;

    Radeon9200Base = OpenLibrary(
        (CONST_STRPTR)CHIP_LIBRARY_NAME, RADEON3D_LIBRARY_VERSION);
    P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    GfxBase = (struct GfxBase *)OpenLibrary(
        (CONST_STRPTR)"graphics.library", 39);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary(
        (CONST_STRPTR)"intuition.library", 39);
    if (!Radeon9200Base || !P96Base || !GfxBase || !IntuitionBase) {
        result = Fail("open_library", 0);
        goto out;
    }

    info.Size = sizeof(info);
    device = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
    if (!device) {
        result = Fail("open_service", 0);
        goto out;
    }
    if ((info.Caps & (RADEON3D_CAP_CP_READY |
                       RADEON3D_CAP_PACKET2_SUBMIT |
                       RADEON3D_CAP_FENCES |
                       RADEON3D_CAP_BITMAP_IMPORT |
                       RADEON3D_CAP_IMMD_TRI_LIST)) !=
            (RADEON3D_CAP_CP_READY |
             RADEON3D_CAP_PACKET2_SUBMIT |
             RADEON3D_CAP_FENCES |
             RADEON3D_CAP_BITMAP_IMPORT |
             RADEON3D_CAP_IMMD_TRI_LIST) ||
        info.MaxBatchDwords != RADEON3D_MAX_BATCH_DWORDS) {
        result = Fail("capabilities", 0);
        goto out;
    }

    screen = LockPubScreen(NULL);
    if (!screen) {
        result = Fail("lock_public_screen", 0);
        goto out;
    }
    UnlockPubScreen(NULL, screen);
    screen = NULL;
    bitmap = AllocateRgb565BitMap(320, 240);
    if (!bitmap) {
        result = Fail("alloc_bitmap", 0);
        goto out;
    }
    printf("R3DPHASE1 bitmap memory=%08lx pitch=%lu bpp=%lu format=%lu "
           "width=%lu height=%lu p96=%lu onboard=%lu\n",
           p96GetBitMapAttr(bitmap, P96_BITMAP_MEMORY),
           p96GetBitMapAttr(bitmap, P96_BITMAP_BYTES_PER_ROW),
           p96GetBitMapAttr(bitmap, P96_BITMAP_BYTES_PER_PIXEL),
           p96GetBitMapAttr(bitmap, P96_BITMAP_RGB_FORMAT),
           p96GetBitMapAttr(bitmap, P96_BITMAP_WIDTH),
           p96GetBitMapAttr(bitmap, P96_BITMAP_HEIGHT),
           p96GetBitMapAttr(bitmap, P96_BITMAP_IS_P96),
           p96GetBitMapAttr(bitmap, P96_BITMAP_IS_ON_BOARD));
    surface.Size = sizeof(surface);
    if (!Radeon3DImportBitMap(device, bitmap, &surface) ||
        surface.Version != RADEON3D_SURFACE_VERSION ||
        surface.Generation != info.Generation ||
        !surface.CpuAddress || !surface.GpuAddress || !surface.Handle ||
        surface.Width != 320 || surface.Height != 240 ||
        (surface.Format != RADEON3D_FORMAT_R5G6B5PC &&
         surface.Format != RADEON3D_FORMAT_B8G8R8A8)) {
        result = Fail("import_bitmap", 0);
        goto out;
    }
    surfaceCpu = (ULONG)surface.CpuAddress;
    surfaceGpu = surface.GpuAddress;
    surfacePitch = surface.Pitch;
    surfaceFormat = surface.Format;
    Radeon3DReleaseSurface(device, &surface);
    if (surface.Handle || surface.Version) {
        result = Fail("release_surface", 0);
        goto out;
    }
    retainedSurface.Size = sizeof(retainedSurface);
    if (!Radeon3DImportBitMap(device, bitmap, &retainedSurface)) {
        result = Fail("reimport_bitmap", 0);
        goto out;
    }

    planar = AllocBitMap(16, 16, 1, BMF_CLEAR, NULL);
    if (planar) {
        invalidSurface.Size = sizeof(invalidSurface);
        if (Radeon3DImportBitMap(device, planar, &invalidSurface)) {
            result = Fail("planar_imported", 0);
            goto out;
        }
    }

    commands = AllocMem(
        RADEON3D_MAX_BATCH_DWORDS * sizeof(*commands), MEMF_PUBLIC);
    if (!commands) {
        result = Fail("alloc_commands", 0);
        goto out;
    }
    for (index = 0; index < RADEON3D_MAX_BATCH_DWORDS; ++index)
        commands[index] = PACKET2;

    commands[RADEON3D_MAX_BATCH_DWORDS / 2UL] = 0;
    fence = 0xdeadbeefUL;
    if (Radeon3DSubmit(device, commands, RADEON3D_MAX_BATCH_DWORDS,
                       RADEON3D_SUBMIT_FENCE, &fence) || fence != 0) {
        result = Fail("malformed_accepted", 0);
        goto out;
    }
    commands[RADEON3D_MAX_BATCH_DWORDS / 2UL] = PACKET2;
    if (Radeon3DSubmit(device, commands,
                       RADEON3D_MAX_BATCH_DWORDS + 1UL, 0, NULL) ||
        Radeon3DSubmit(device, commands, 1, ~RADEON3D_SUBMIT_FLAGS, NULL)) {
        result = Fail("bounds_accepted", 0);
        goto out;
    }

    if (retainedSurface.Format != RADEON3D_FORMAT_R5G6B5PC ||
        retainedSurface.Width != 320UL || retainedSurface.Height != 240UL ||
        retainedSurface.Pitch != 640UL) {
        result = Fail("triangle_surface", 0);
        goto out;
    }
    {
        volatile UBYTE *pixels = (volatile UBYTE *)retainedSurface.CpuAddress;
        ULONG size = retainedSurface.Pitch * retainedSurface.Height;

        for (index = 0; index < size; ++index)
            pixels[index] = 0;
        (void)pixels[0];
    }
    triangleDwords = BuildTriangles(commands, retainedSurface.GpuAddress,
                                    retainedSurface.Pitch,
                                    retainedSurface.Width,
                                    retainedSurface.Height, vertices,
                                    TRIANGLE_VERTICES);
    if (triangleDwords != TRIANGLE_DWORDS) {
        result = Fail("triangle_build", triangleDwords);
        goto out;
    }
    commands[45] ^= 1UL << 16;
    if (Radeon3DSubmit(device, commands, triangleDwords, 0, NULL)) {
        result = Fail("vertex_count_accepted", 0);
        goto out;
    }
    commands[45] ^= 1UL << 16;
    commands[44] ^= 1UL << 16;
    if (Radeon3DSubmit(device, commands, triangleDwords, 0, NULL)) {
        result = Fail("packet_count_accepted", 0);
        goto out;
    }
    commands[44] ^= 1UL << 16;
    commands[46] = 0x7fc00000UL;
    if (Radeon3DSubmit(device, commands, triangleDwords, 0, NULL)) {
        result = Fail("nan_coordinate_accepted", 0);
        goto out;
    }
    commands[46] = vertices[0];
    commands[46] = 0x7f800000UL;
    if (Radeon3DSubmit(device, commands, triangleDwords, 0, NULL)) {
        result = Fail("infinite_coordinate_accepted", 0);
        goto out;
    }
    commands[46] = vertices[0];
    commands[46] = 0xbf800000UL;
    if (Radeon3DSubmit(device, commands, triangleDwords, 0, NULL)) {
        result = Fail("negative_coordinate_accepted", 0);
        goto out;
    }
    commands[46] = vertices[0];
    commands[46] = UnsignedFloatBits(retainedSurface.Width + 1UL);
    if (Radeon3DSubmit(device, commands, triangleDwords, 0, NULL)) {
        result = Fail("outside_coordinate_accepted", 0);
        goto out;
    }
    commands[46] = vertices[0];
    if (Radeon3DSubmit(device, commands, triangleDwords - 3UL, 0, NULL)) {
        result = Fail("partial_triangle_accepted", 0);
        goto out;
    }
    commands[triangleDwords] = PACKET2;
    if (Radeon3DSubmit(device, commands, triangleDwords + 1UL, 0, NULL)) {
        result = Fail("trailing_dword_accepted", 0);
        goto out;
    }
    fence = 0;
    if (!Radeon3DSubmit(device, commands, triangleDwords,
                        RADEON3D_SUBMIT_FENCE, &fence)) {
        result = Fail("triangle_submit", 1);
        goto out;
    }
    if (!fence) {
        result = Fail("triangle_submit", 2);
        goto out;
    }
    if (!Radeon3DWaitFence(device, fence, 1000UL)) {
        result = Fail("triangle_submit", 3);
        goto out;
    }
    {
        volatile UBYTE *pixels = (volatile UBYTE *)retainedSurface.CpuAddress;
        ULONG size = retainedSurface.Pitch * retainedSurface.Height;
        ULONG center = 120UL * retainedSurface.Pitch + 160UL * 2UL;

        for (index = 0; index < size; ++index) {
            if (pixels[index])
                ++triangleBytes;
        }
        triangleSample = ((ULONG)pixels[center] << 8) |
                         pixels[center + 1UL];
    }
    if (triangleBytes < 1000UL || !triangleSample) {
        printf("R3DPHASE1 triangle_bytes=%lu triangle_sample=%04lx\n",
               triangleBytes, triangleSample);
        result = Fail("triangle_readback", 0);
        goto out;
    }

    BltBitMap(bitmap, 150, 110, bitmap, 4, 4, 16, 16,
              0xc0, 0xff, NULL);
    WaitBlit();
    {
        volatile UBYTE *pixels = (volatile UBYTE *)retainedSurface.CpuAddress;
        ULONG destination = 8UL * retainedSurface.Pitch + 8UL * 2UL;

        transitionSample = ((ULONG)pixels[destination] << 8) |
                           pixels[destination + 1UL];
    }
    if (!transitionSample) {
        result = Fail("triangle_to_2d", 0);
        goto out;
    }

    for (index = 0; index < RADEON3D_MAX_BATCH_DWORDS; ++index)
        commands[index] = PACKET2;

    for (index = 0; index < WRAP_SUBMITS; ++index) {
        fence = 0;
        if (!Radeon3DSubmit(device, commands,
                            RADEON3D_MAX_BATCH_DWORDS,
                            RADEON3D_SUBMIT_FENCE, &fence) ||
            !fence || fence == previousFence) {
            result = Fail("submit", index);
            goto out;
        }
        if (Radeon3DTestFence(device, fence + 1UL)) {
            result = Fail("future_fence", index);
            goto out;
        }
        if (!Radeon3DWaitFence(device, fence, 1000) ||
            !Radeon3DTestFence(device, fence)) {
            result = Fail("fence", index);
            goto out;
        }
        previousFence = fence;
    }

    fence = 0xdeadbeefUL;
    if (!Radeon3DSubmit(device, commands, 1, 0, &fence) || fence != 0) {
        result = Fail("unfenced_submit", WRAP_SUBMITS);
        goto out;
    }
    Radeon3DReleaseSurface(device, &retainedSurface);
    if (retainedSurface.Handle || retainedSurface.Version) {
        result = Fail("retained_release", WRAP_SUBMITS);
        goto out;
    }
    triangleDwords = BuildTriangles(commands, surfaceGpu, surfacePitch,
                                    320UL, 240UL, vertices,
                                    TRIANGLE_VERTICES);
    if (Radeon3DSubmit(device, commands, triangleDwords, 0, NULL)) {
        result = Fail("released_target_accepted", WRAP_SUBMITS);
        goto out;
    }

    printf("R3DPHASE1 status=ok submits=%lu dwords=%lu last_fence=%lu "
           "generation=%lu caps=%08lx surface_cpu=%08lx "
            "surface_gpu=%08lx surface_pitch=%lu surface_format=%lu "
            "triangle_bytes=%lu triangle_sample=%04lx transition=%04lx\n",
           WRAP_SUBMITS, WRAP_SUBMITS * RADEON3D_MAX_BATCH_DWORDS,
           previousFence, info.Generation, info.Caps, surfaceCpu,
            surfaceGpu, surfacePitch, surfaceFormat, triangleBytes,
            triangleSample, transitionSample);

out:
    if (screen)
        UnlockPubScreen(NULL, screen);
    if (commands)
        FreeMem(commands,
                RADEON3D_MAX_BATCH_DWORDS * sizeof(*commands));
    if (device)
        Radeon3DClose(device);
    if (planar)
        FreeBitMap(planar);
    if (bitmap)
        p96FreeBitMap(bitmap);
    if (Radeon9200Base)
        CloseLibrary(Radeon9200Base);
    if (P96Base)
        CloseLibrary(P96Base);
    if (IntuitionBase)
        CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase)
        CloseLibrary((struct Library *)GfxBase);
    Radeon9200Base = NULL;
    IntuitionBase = NULL;
    GfxBase = NULL;
    return result;
}
