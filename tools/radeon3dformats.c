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

static ULONG SignedFloatBits(LONG value)
{
    ULONG bits = FloatBits((ULONG)(value < 0 ? -value : value));

    return value < 0 ? (0x80000000UL | bits) : bits;
}

static struct BitMap *AllocateSized(ULONG width, ULONG height, ULONG depth,
                                    ULONG format, ULONG formatMask)
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
    bitmap = p96AllocBitMap(width, height, depth,
                            BMF_CLEAR | BMF_DISPLAYABLE,
                            screen->RastPort.BitMap, format);
    p96CloseScreen(screen);
    return bitmap;
}

static struct BitMap *AllocateTarget(ULONG depth, ULONG format,
                                     ULONG formatMask)
{
    return AllocateSized(WIDTH, HEIGHT, depth, format, formatMask);
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
    /* B8G8R8A8 readback of the opaque red and green quads. The vertices
     * carry alpha 0xff and nothing masks it, so the alpha byte is 0xff,
     * matching the 32-bit format target check above. */
    if (first != 0x0000ffffUL || second != 0x00ff00ffUL)
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

/* Interface-11 lighting spike: one directional white light straight down
 * the eye axis lights a front-facing quad to full white regardless of its
 * object coordinates. Also proves the normals vertex layout is accepted. */
static BOOL DrawLitQuadMode(struct Radeon3DDevice *device,
                            struct Radeon3DSurface *surface,
                            ULONG extraOptions, ULONG lightMask,
                            ULONG *rejectStep)
{
    ULONG headerBase = RADEON3D_EXEC_DRAW_HW_TCL_HEADER_DWORDS +
                       (extraOptions & RADEON3D_DRAW_NORMALS ?
                        RADEON3D_EXEC_NORMAL_MATRICES_DWORDS : 0UL);
    ULONG headerDwords = headerBase;
    ULONG recordDwords;
    ULONG *records;
    ULONG fence = 0;
    ULONG one = FloatBits(1UL);
    ULONG *matrix;
    ULONG *light;
    ULONG index;
    BOOL ok;

    if (extraOptions & RADEON3D_DRAW_LIGHTING)
        headerDwords += RADEON3D_EXEC_LIGHT_STATE_DWORDS +
                        (lightMask ? RADEON3D_EXEC_LIGHT_BLOCK_DWORDS : 0UL);
    recordDwords = headerDwords +
        4UL * (extraOptions & RADEON3D_DRAW_NORMALS ?
               RADEON3D_EXEC_HW_TCL_NORMAL_VERTEX_DWORDS :
               RADEON3D_EXEC_HW_TCL_VERTEX_DWORDS);
    records = AllocMem(recordDwords * sizeof(ULONG), MEMF_CLEAR);
    if (!records)
        return FALSE;
    records[0] = RADEON3D_EXEC_DRAW_QUADS;
    records[1] = recordDwords;
    records[2] = (ULONG)surface->Handle;
    records[5] = RADEON3D_DRAW_FRAGMENT_STATE |
                 RADEON3D_DRAW_EXTENDED_VERTEX |
                 RADEON3D_DRAW_HW_TCL | extraOptions;
    records[8] = WIDTH;
    records[9] = HEIGHT;
    records[10] = 4;
    records[43] = (16UL << RADEON3D_TRANSFORM_POINT_SIZE_SHIFT);
    matrix = records + 21UL;
    matrix[0] = 0x3c000000UL; matrix[5] = 0x3c000000UL;
    matrix[10] = 0x3c000000UL; matrix[15] = one;
    records[37] = FloatBits(160UL);
    records[38] = FloatBits(160UL);
    records[39] = 0x80000000UL | FloatBits(120UL);
    records[40] = FloatBits(120UL);
    records[41] = one;
    records[42] = one;
    if (extraOptions & RADEON3D_DRAW_NORMALS) {
        matrix = records +
                 RADEON3D_EXEC_DRAW_HW_TCL_HEADER_DWORDS;
        matrix[0] = one; matrix[5] = one;
        matrix[10] = one; matrix[15] = one;
        matrix[16] = one; matrix[21] = one;
        matrix[26] = one; matrix[31] = one;
    }
    if (extraOptions & RADEON3D_DRAW_LIGHTING) {
        index = headerBase;
        /* Infinite viewer looking down +Z. */
        records[index + 6] = one;
        records[index + 8] = lightMask;
        /* Front material: white diffuse only (offsets are relative to the
         * lighting state base: glt 0..3, eye 4..7, control 8, material
         * 9..25). */
        records[index + 17] = one;
        records[index + 18] = one;
        records[index + 19] = one;
        records[index + 20] = one;
        light = records + index + RADEON3D_EXEC_LIGHT_STATE_DWORDS;
        light[4] = one; light[5] = one; light[6] = one; light[7] = one;
        light[14] = one;
        light[27] = 0xbf800000UL;
    }
    /* Object-space quad covering the full target: the model-view-projection
     * scale is 1/128 and the viewport is 320x240, so +-128 fills the clip
     * cube. Normals point straight at the viewer so N.L is 1 for the
     * directional light and the surface lights to full material diffuse. */
    {
        static const LONG corners[8] = {-128, -128, 128, -128,
                                        128, 128, -128, 128};
        BOOL withNormals = (extraOptions & RADEON3D_DRAW_NORMALS) != 0;
        ULONG stride = withNormals ?
            RADEON3D_EXEC_HW_TCL_NORMAL_VERTEX_DWORDS :
            RADEON3D_EXEC_HW_TCL_VERTEX_DWORDS;
        ULONG *vertex;

        for (index = 0; index < 4UL; ++index) {
            vertex = records + headerDwords + index * stride;
            vertex[0] = SignedFloatBits(corners[index * 2UL]);
            vertex[1] = SignedFloatBits(corners[index * 2UL + 1UL]);
            vertex[2] = 0;
            vertex[3] = one;
            if (withNormals) {
                vertex[6] = one;
                vertex[7] = 0xffffffffUL;
            } else {
                vertex[4] = 0xffffffffUL;
            }
        }
    }
    ok = Radeon3DExecute(device, records, recordDwords,
                         RADEON3D_SUBMIT_FENCE, &fence) && fence &&
         Radeon3DWaitFence(device, fence, 1000UL);
    FreeMem(records, recordDwords * sizeof(ULONG));
    return ok;
}

#define LIGHT_HEADER_DWORDS \
    (RADEON3D_EXEC_DRAW_HW_TCL_HEADER_DWORDS + \
     RADEON3D_EXEC_NORMAL_MATRICES_DWORDS + \
     RADEON3D_EXEC_LIGHT_STATE_DWORDS + \
     RADEON3D_EXEC_LIGHT_BLOCK_DWORDS)

static BOOL DrawLitQuad(struct Radeon3DDevice *device,
                        struct Radeon3DSurface *surface)
{
    /* Full lit quad used for colour readback and legacy rejection. */
    return DrawLitQuadMode(device, surface,
                           RADEON3D_DRAW_NORMALS |
                           RADEON3D_DRAW_LIGHTING, 1UL, NULL);
}

#define TEXGEN_TEXSIZE 64UL

/* Interface-10 texgen spike: an object-linear S plane maps object X across
 * the texture while T stays fixed, so a half-red/half-green texture must
 * appear as a left-red/right-green target with no supplied coordinates. */
static BOOL DrawTexGenQuad(struct Radeon3DDevice *device,
                           struct Radeon3DSurface *surface,
                           struct Radeon3DSurface *texture,
                           BOOL useTexGen)
{
    static const LONG corners[8] = {-128, -128, 128, -128,
                                    128, 128, -128, 128};
    ULONG headerDwords = useTexGen ?
        RADEON3D_EXEC_DRAW_TEXGEN_HEADER_DWORDS :
        RADEON3D_EXEC_DRAW_HW_TCL_HEADER_DWORDS;
    ULONG recordDwords = headerDwords +
                         4UL * RADEON3D_EXEC_HW_TCL_VERTEX_DWORDS;
    ULONG one = FloatBits(1UL);
    ULONG *records = AllocMem(recordDwords * sizeof(ULONG), MEMF_CLEAR);
    ULONG *matrix;
    ULONG *vertex;
    ULONG fence = 0;
    ULONG index;
    BOOL ok;

    if (!records)
        return FALSE;
    records[0] = RADEON3D_EXEC_DRAW_QUADS;
    records[1] = recordDwords;
    records[2] = (ULONG)surface->Handle;
    records[4] = (ULONG)texture->Handle;
    records[5] = RADEON3D_DRAW_TEXTURED | RADEON3D_DRAW_FRAGMENT_STATE |
                 RADEON3D_DRAW_EXTENDED_VERTEX | RADEON3D_DRAW_HW_TCL |
                 (useTexGen ? RADEON3D_DRAW_TEXGEN : 0UL);
    records[8] = WIDTH;
    records[9] = HEIGHT;
    records[10] = 4;
    records[12] = (TEXGEN_TEXSIZE - 1UL) |
                  ((TEXGEN_TEXSIZE - 1UL) << 16);
    records[43] = (16UL << RADEON3D_TRANSFORM_POINT_SIZE_SHIFT);
    matrix = records + 21UL;
    matrix[0] = 0x3c000000UL; matrix[5] = 0x3c000000UL;
    matrix[10] = 0x3c000000UL; matrix[15] = one;
    records[37] = FloatBits(160UL);
    records[38] = FloatBits(160UL);
    records[39] = 0x80000000UL | FloatBits(120UL);
    records[40] = FloatBits(120UL);
    records[41] = one;
    records[42] = one;
    if (useTexGen) {
        records[44] = RADEON3D_TEXGEN_MODE_OBJECT_LINEAR |
                      RADEON3D_TEXGEN_GEN_S | RADEON3D_TEXGEN_GEN_T;
        matrix = records + 46UL;
        matrix[0] = 0x3b800000UL;   /* S = objectX / 256 ... */
        matrix[12] = 0x3f000000UL;  /* ... + 0.5, so 0..1 across the quad */
        matrix[13] = 0x3f000000UL;  /* T is a constant 0.5 */
        matrix[15] = one;           /* Q is 1 */
    }
    for (index = 0; index < 4UL; ++index) {
        vertex = records + headerDwords +
                 index * RADEON3D_EXEC_HW_TCL_VERTEX_DWORDS;
        vertex[0] = SignedFloatBits(corners[index * 2UL]);
        vertex[1] = SignedFloatBits(corners[index * 2UL + 1UL]);
        vertex[3] = one;
        vertex[4] = 0xffffffffUL;
        if (!useTexGen) {
            vertex[5] = (index == 1UL || index == 2UL) ? one : 0UL;
            vertex[6] = 0x3f000000UL;
        }
    }
    ok = Radeon3DExecute(device, records, recordDwords,
                         RADEON3D_SUBMIT_FENCE, &fence) && fence &&
         Radeon3DWaitFence(device, fence, 1000UL);
    FreeMem(records, recordDwords * sizeof(ULONG));
    return ok;
}

static void FillHalfTexture(const struct Radeon3DSurface *texture)
{
    volatile UBYTE *row = (volatile UBYTE *)texture->CpuAddress;
    ULONG x, y;

    for (y = 0; y < TEXGEN_TEXSIZE; ++y) {
        volatile UBYTE *pixel = row + y * texture->Pitch;

        for (x = 0; x < TEXGEN_TEXSIZE; ++x, pixel += 4) {
            /* B8G8R8A8: left half opaque red, right half opaque green. */
            pixel[0] = 0x00;
            pixel[1] = (UBYTE)(x < TEXGEN_TEXSIZE / 2UL ? 0x00 : 0xff);
            pixel[2] = (UBYTE)(x < TEXGEN_TEXSIZE / 2UL ? 0xff : 0x00);
            pixel[3] = 0xff;
        }
    }
    (void)row[0];
}

static int TestTexGen(struct Radeon3DDevice *device)
{
    struct Radeon3DSurface surface;
    struct Radeon3DSurface texture;
    struct BitMap *bitmap = AllocateTarget(32UL, RGBFB_B8G8R8A8,
                                           RGBFF_B8G8R8A8);
    struct BitMap *texmap = AllocateSized(TEXGEN_TEXSIZE, TEXGEN_TEXSIZE,
                                          32UL, RGBFB_B8G8R8A8,
                                          RGBFF_B8G8R8A8);
    ULONG left, right;
    BOOL plain, generated;
    int result = 0;

    surface.Version = 0; surface.Handle = NULL;
    surface.Size = sizeof(surface);
    texture.Version = 0; texture.Handle = NULL;
    texture.Size = sizeof(texture);
    if (!bitmap || !texmap ||
        !Radeon3DImportBitMap(device, bitmap, &surface) ||
        !Radeon3DImportBitMap(device, texmap, &texture)) {
        result = 95;
        goto out;
    }
    FillHalfTexture(&texture);
    plain = DrawTexGenQuad(device, &surface, &texture, FALSE);
    generated = DrawTexGenQuad(device, &surface, &texture, TRUE);
    printf("R3DTEXGENPROBE textured=%s texgen=%s\n",
           plain ? "ok" : "REJECT", generated ? "ok" : "REJECT");
    if (!generated) {
        result = 96;
        goto out;
    }
    left = ReadSample32(&surface, WIDTH / 4UL, HEIGHT / 2UL);
    right = ReadSample32(&surface, WIDTH * 3UL / 4UL, HEIGHT / 2UL);
    printf("R3DTEXGEN left=%08lx right=%08lx\n", left, right);
    if ((left & 0x00ffff00UL) != 0x0000ff00UL ||
        (right & 0x00ffff00UL) != 0x00ff0000UL)
        result = 97;
out:
    if (texture.Handle) Radeon3DReleaseSurface(device, &texture);
    if (surface.Handle) Radeon3DReleaseSurface(device, &surface);
    if (texmap) p96FreeBitMap(texmap);
    if (bitmap) p96FreeBitMap(bitmap);
    return result;
}

/* Raw interface-12 probe. OpenGL sphere mapping reflects the unit eye vector
 * u about the eye-space normal, r = u - 2n(n.u), then takes
 * m = 2*sqrt(rx^2 + ry^2 + (rz+1)^2), s = rx/m + 0.5, t = ry/m + 0.5. A
 * normal parallel to u yields r = -u, not (0,0,1); the bisector n = u - r
 * is what reflects u onto a chosen r.
 *
 * Each vertex here carries the object-space normal that the submitted
 * inverse model-view maps onto that vertex's own bisector for r = (0,0,1),
 * so every vertex produces exactly (0.5,0.5) and the whole quad samples one
 * texel. The four normals are all different, so a wrong normal-matrix
 * orientation, a missing normal transform, or a non-sphere texgen input
 * moves the corners off that texel. The texture matrix then maps S to 0.75,
 * and readback is pixel-exact. */
static BOOL DrawSphereMapQuad(struct Radeon3DDevice *device,
                              struct Radeon3DSurface *surface,
                              struct Radeon3DSurface *texture)
{
    static const LONG corners[8] = {-128, -128, 128, -128,
                                    128, 128, -128, 128};
    /* Object-space normals, one per corner in the order above. With the
     * model-view below, corner p has eye position e = (px - 64, py, -64);
     * these are transpose(modelView) applied to normalize(normalize(e) -
     * (0,0,1)), so the inverse model-view delivers a unit eye-space normal
     * that reflects the eye vector exactly onto (0,0,1). Values are the
     * float32 encodings of, in order, (-0.503627,-0.335751,-1.299636),
     * (0.243259,-0.486519,-0.595862), (0.243259,0.486519,-0.595862) and
     * (-0.503627,0.335751,-1.299636). */
    static const ULONG normals[12] = {
        0xbf00edb6UL, 0xbeabe79dUL, 0xbfa65a7cUL,
        0x3e791903UL, 0xbef91903UL, 0xbf188a62UL,
        0x3e791903UL, 0x3ef91903UL, 0xbf188a62UL,
        0xbf00edb6UL, 0x3eabe79dUL, 0xbfa65a7cUL
    };
    const ULONG headerDwords = RADEON3D_EXEC_DRAW_TEXGEN_HEADER_DWORDS +
                               RADEON3D_EXEC_NORMAL_MATRICES_DWORDS;
    const ULONG recordDwords = headerDwords +
        4UL * RADEON3D_EXEC_HW_TCL_NORMAL_VERTEX_DWORDS;
    ULONG *records = AllocMem(recordDwords * sizeof(ULONG), MEMF_CLEAR);
    ULONG one = FloatBits(1UL);
    ULONG *matrix;
    ULONG *vertex;
    ULONG fence = 0;
    ULONG index;
    BOOL ok;

    if (!records)
        return FALSE;
    records[0] = RADEON3D_EXEC_DRAW_QUADS;
    records[1] = recordDwords;
    records[2] = (ULONG)surface->Handle;
    records[4] = (ULONG)texture->Handle;
    records[5] = RADEON3D_DRAW_TEXTURED | RADEON3D_DRAW_FRAGMENT_STATE |
                 RADEON3D_DRAW_EXTENDED_VERTEX | RADEON3D_DRAW_HW_TCL |
                 RADEON3D_DRAW_TEXGEN | RADEON3D_DRAW_NORMALS;
    records[8] = WIDTH;
    records[9] = HEIGHT;
    records[10] = 4;
    records[12] = (TEXGEN_TEXSIZE - 1UL) |
                  ((TEXGEN_TEXSIZE - 1UL) << 16);
    records[43] = (16UL << RADEON3D_TRANSFORM_POINT_SIZE_SHIFT);
    matrix = records + 21UL;
    matrix[0] = 0x3c000000UL; matrix[5] = 0x3c000000UL;
    matrix[10] = 0x3c000000UL; matrix[15] = one;
    records[37] = FloatBits(160UL);
    records[38] = FloatBits(160UL);
    records[39] = 0x80000000UL | FloatBits(120UL);
    records[40] = FloatBits(120UL);
    records[41] = one;
    records[42] = one;
    records[44] = RADEON3D_TEXGEN_MODE_SPHERE_MAP |
                  RADEON3D_TEXGEN_GEN_S | RADEON3D_TEXGEN_GEN_T;
    /* S' = 0.5*S + 0.5, making the center sphere coordinate 0.75. */
    matrix = records + 46UL;
    matrix[0] = 0x3f000000UL; matrix[5] = one;
    matrix[10] = one; matrix[12] = 0x3f000000UL; matrix[15] = one;
    /* x' = x + z. Its inverse has -1 in the same row. */
    matrix = records + RADEON3D_EXEC_DRAW_TEXGEN_HEADER_DWORDS;
    matrix[0] = one; matrix[5] = one; matrix[10] = one; matrix[15] = one;
    matrix[8] = one;
    matrix[16] = one; matrix[21] = one;
    matrix[26] = one; matrix[31] = one;
    matrix[24] = 0xbf800000UL;
    for (index = 0; index < 4UL; ++index) {
        vertex = records + headerDwords +
                 index * RADEON3D_EXEC_HW_TCL_NORMAL_VERTEX_DWORDS;
        vertex[0] = SignedFloatBits(corners[index * 2UL]);
        vertex[1] = SignedFloatBits(corners[index * 2UL + 1UL]);
        vertex[2] = SignedFloatBits(-64);
        vertex[3] = one;
        vertex[4] = normals[index * 3UL];
        vertex[5] = normals[index * 3UL + 1UL];
        vertex[6] = normals[index * 3UL + 2UL];
        vertex[7] = 0xffffffffUL;
    }
    ok = Radeon3DExecute(device, records, recordDwords,
                         RADEON3D_SUBMIT_FENCE, &fence) && fence &&
         Radeon3DWaitFence(device, fence, 1000UL);
    FreeMem(records, recordDwords * sizeof(ULONG));
    return ok;
}

static void FillSphereTexture(const struct Radeon3DSurface *texture)
{
    volatile UBYTE *row = (volatile UBYTE *)texture->CpuAddress;
    ULONG x, y;

    for (y = 0; y < TEXGEN_TEXSIZE; ++y) {
        volatile UBYTE *pixel = row + y * texture->Pitch;

        for (x = 0; x < TEXGEN_TEXSIZE; ++x, pixel += 4) {
            /* Every vertex generates (0.5,0.5); the texture matrix maps S
             * to 0.75, so the whole quad must sample texel (48,32). The
             * band tolerates 0.0625 of drift in each generated component
             * without accepting a wrong reflection. */
            BOOL center = x >= 46UL && x < 50UL &&
                          y >= 28UL && y < 36UL;

            pixel[0] = center ? 0x5a : 0x11;
            pixel[1] = center ? 0xc3 : 0x22;
            pixel[2] = center ? 0xe7 : 0x33;
            pixel[3] = 0xff;
        }
    }
    (void)row[0];
}

static int TestSphereMap(struct Radeon3DDevice *device)
{
    struct Radeon3DSurface surface;
    struct Radeon3DSurface texture;
    struct BitMap *bitmap = AllocateTarget(32UL, RGBFB_B8G8R8A8,
                                           RGBFF_B8G8R8A8);
    struct BitMap *texmap = AllocateSized(TEXGEN_TEXSIZE, TEXGEN_TEXSIZE,
                                          32UL, RGBFB_B8G8R8A8,
                                          RGBFF_B8G8R8A8);
    ULONG samples[5];
    static const ULONG samplePoints[10] = {
        32UL, 32UL, WIDTH - 33UL, 32UL,
        WIDTH - 33UL, HEIGHT - 33UL, 32UL, HEIGHT - 33UL,
        WIDTH / 2UL, HEIGHT / 2UL
    };
    ULONG index;
    int result = 0;

    surface.Version = 0; surface.Handle = NULL;
    surface.Size = sizeof(surface);
    texture.Version = 0; texture.Handle = NULL;
    texture.Size = sizeof(texture);
    if (!bitmap || !texmap ||
        !Radeon3DImportBitMap(device, bitmap, &surface) ||
        !Radeon3DImportBitMap(device, texmap, &texture)) {
        result = 100;
        goto out;
    }
    FillSphereTexture(&texture);
    if (!DrawSphereMapQuad(device, &surface, &texture)) {
        result = 101;
        goto out;
    }
    for (index = 0; index < 5UL; ++index) {
        samples[index] = ReadSample32(&surface,
                                      samplePoints[index * 2UL],
                                      samplePoints[index * 2UL + 1UL]);
        if (samples[index] != 0x5ac3e7ffUL)
            result = 102;
    }
    printf("R3DSPHEREMAP samples=%08lx,%08lx,%08lx,%08lx,%08lx "
           "expected=5ac3e7ff\n",
           samples[0], samples[1], samples[2], samples[3], samples[4]);
out:
    if (texture.Handle) Radeon3DReleaseSurface(device, &texture);
    if (surface.Handle) Radeon3DReleaseSurface(device, &surface);
    if (texmap) p96FreeBitMap(texmap);
    if (bitmap) p96FreeBitMap(bitmap);
    return result;
}

static int TestLegacySphereMapRejection(void)
{
    struct Radeon3DInfo info;
    struct Radeon3DDevice *device;
    struct Radeon3DSurface surface;
    struct Radeon3DSurface texture;
    struct BitMap *bitmap;
    struct BitMap *texmap;
    int result = 0;

    info.Size = sizeof(info);
    device = Radeon3DOpen(11UL, &info);
    if (!device || info.Version != 11UL ||
        (info.Caps & RADEON3D_CAP_HW_SPHERE_MAP))
        return 103;
    bitmap = AllocateTarget(32UL, RGBFB_B8G8R8A8, RGBFF_B8G8R8A8);
    texmap = AllocateSized(TEXGEN_TEXSIZE, TEXGEN_TEXSIZE, 32UL,
                           RGBFB_B8G8R8A8, RGBFF_B8G8R8A8);
    surface.Version = 0; surface.Handle = NULL;
    surface.Size = sizeof(surface);
    texture.Version = 0; texture.Handle = NULL;
    texture.Size = sizeof(texture);
    if (!bitmap || !texmap ||
        !Radeon3DImportBitMap(device, bitmap, &surface) ||
        !Radeon3DImportBitMap(device, texmap, &texture) ||
        DrawSphereMapQuad(device, &surface, &texture))
        result = 104;
    if (texture.Handle) Radeon3DReleaseSurface(device, &texture);
    if (surface.Handle) Radeon3DReleaseSurface(device, &surface);
    if (texmap) p96FreeBitMap(texmap);
    if (bitmap) p96FreeBitMap(bitmap);
    Radeon3DClose(device);
    return result;
}

static int TestLighting(struct Radeon3DDevice *device)
{
    struct Radeon3DSurface surface;
    struct BitMap *bitmap = AllocateTarget(32UL, RGBFB_B8G8R8A8,
                                           RGBFF_B8G8R8A8);
    ULONG sample;
    BOOL tcl, normals, light0, light1;
    int result = 0;

    surface.Version = 0;
    surface.Handle = NULL;
    surface.Size = sizeof(surface);
    if (!bitmap || !Radeon3DImportBitMap(device, bitmap, &surface)) {
        result = 90;
        goto out;
    }
    /* Step probe: isolate which option combination is rejected. */
    tcl = DrawLitQuadMode(device, &surface, 0UL, 0UL, NULL);
    normals = DrawLitQuadMode(device, &surface,
                              RADEON3D_DRAW_NORMALS, 0UL, NULL);
    light0 = DrawLitQuadMode(device, &surface,
                             RADEON3D_DRAW_NORMALS |
                             RADEON3D_DRAW_LIGHTING, 0UL, NULL);
    light1 = DrawLitQuadMode(device, &surface,
                             RADEON3D_DRAW_NORMALS |
                             RADEON3D_DRAW_LIGHTING, 1UL, NULL);
    printf("R3DLIGHTPROBE tcl=%s normals=%s light_nomask=%s "
           "light_one=%s\n",
           tcl ? "ok" : "REJECT",
           normals ? "ok" : "REJECT",
           light0 ? "ok" : "REJECT",
           light1 ? "ok" : "REJECT");
    if (!light1) {
        result = 91;
        goto out;
    }
    sample = ReadSample32(&surface, WIDTH / 2UL, HEIGHT / 2UL);
    printf("R3DLIGHT sample=%08lx\n", sample);
    if ((sample & 0x00ffffffUL) != 0x00ffffffUL)
        result = 92;
out:
    if (surface.Handle) Radeon3DReleaseSurface(device, &surface);
    if (bitmap) p96FreeBitMap(bitmap);
    return result;
}

static int TestLegacyLightingRejection(void)
{
    struct Radeon3DInfo info;
    struct Radeon3DDevice *device;
    struct Radeon3DSurface surface;
    struct BitMap *bitmap;
    int result = 0;

    info.Size = sizeof(info);
    device = Radeon3DOpen(10UL, &info);
    if (!device || info.Version != 10UL ||
        (info.Caps & RADEON3D_CAP_HW_NORMALS) ||
        (info.Caps & RADEON3D_CAP_HW_LIGHTING))
        return 93;
    bitmap = AllocateTarget(32UL, RGBFB_B8G8R8A8, RGBFF_B8G8R8A8);
    surface.Version = 0;
    surface.Handle = NULL;
    surface.Size = sizeof(surface);
    if (!bitmap || !Radeon3DImportBitMap(device, bitmap, &surface) ||
        DrawLitQuad(device, &surface))
        result = 94;
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
        /* The fourth B8G8R8A8 byte is unused by Picasso96; its content is
         * not part of the contract. */
        (depth == 32UL && (sample & 0xffffff00UL) != 0xcc663300UL))
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
    int formatResult = 0, texgenResult = 0, sphereResult = 0;
    int lightResult = 0;
    int quadResult = 0;
    int legacyResult = 0, legacyQuadResult = 0, legacyLightResult = 0;
    int legacySphereResult = 0;

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
        !(info.Caps & RADEON3D_CAP_NATIVE_QUAD_LISTS) ||
        !(info.Caps & RADEON3D_CAP_HW_SPHERE_MAP)) {
        result = 11;
        goto out;
    }
    result = TestFormat(device, 8UL, RGBFB_CLUT, RGBFF_CLUT,
                        RADEON3D_FORMAT_CLUT8);
    if (!result)
        result = TestFormat(device, 16UL, RGBFB_R5G6B5PC,
                            RGBFF_R5G6B5PC,
                            RADEON3D_FORMAT_R5G6B5PC);
    formatResult = result;
    result = 0;
    if (!result)
        result = TestFormat(device, 32UL, RGBFB_B8G8R8A8,
                            RGBFF_B8G8R8A8,
                            RADEON3D_FORMAT_B8G8R8A8);
    formatResult = formatResult ? formatResult : result;
    result = 0;
    if (!result)
        result = TestTexGen(device);
    texgenResult = result;
    result = 0;
    if (!result)
        result = TestSphereMap(device);
    sphereResult = result;
    result = 0;
    if (!result)
        result = TestLighting(device);
    lightResult = result;
    result = 0;
    if (!result)
        result = TestQuadList(device);
    quadResult = result;
    Radeon3DClose(device);
    device = NULL;
    legacyResult = TestLegacyRejection();
    legacyQuadResult = legacyResult ? 0 : TestLegacyQuadRejection();
    legacyLightResult = (legacyResult || legacyQuadResult) ? 0 :
                         TestLegacyLightingRejection();
    legacySphereResult = (legacyResult || legacyQuadResult ||
                          legacyLightResult) ? 0 :
                         TestLegacySphereMapRejection();
    result = formatResult ? formatResult :
             texgenResult ? texgenResult :
             sphereResult ? sphereResult :
             lightResult ? lightResult :
             quadResult ? quadResult :
             legacyResult ? legacyResult :
             legacyQuadResult ? legacyQuadResult :
             legacyLightResult ? legacyLightResult : legacySphereResult;
out:
    if (device) Radeon3DClose(device);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (P96Base) CloseLibrary(P96Base);
    if (Radeon9200Base) CloseLibrary(Radeon9200Base);
    printf("R3DFORMATS status=%s result=%d format8_16=%s texgen=%s "
           "sphere=%s light=%s quads=%s legacy_v5=%s legacy_v7_quad=%s "
           "legacy_v10_light=%s legacy_v11_sphere=%s\n",
           result ? "fail" : "ok", result,
           formatResult ? "fail" : "ok",
           texgenResult ? (texgenResult == 97 ? "color" : "fail") : "ok",
           sphereResult ? (sphereResult == 102 ? "pixel" : "fail") : "ok",
           lightResult ? (lightResult == 92 ? "color" : "fail") : "ok",
           quadResult ? "fail" : "ok",
           legacyResult ? "fail" : "rejected",
           legacyQuadResult ? "fail" : "rejected",
           legacyLightResult ? "fail" : "rejected",
           legacySphereResult ? "fail" : "rejected");
    return result;
}
