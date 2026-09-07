/* Public semantic-service regression: two texture/matrix states in one batch. */
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/radeon3d.h>
#include <stdio.h>
#include <string.h>

#define SIDE 64UL
#define DRAW_WORDS (RADEON3D_EXEC_DRAW_HW_TCL_HEADER_DWORDS + \
                    6UL * RADEON3D_EXEC_HW_TCL_VERTEX_DWORDS)
struct Library *Radeon9200Base;
static ULONG Records[RADEON3D_EXEC_CLEAR_DWORDS + 2UL * DRAW_WORDS];

static BOOL AllocSurface(struct Radeon3DDevice *device,
                     struct Radeon3DSurface *surface, ULONG generation)
{
    surface->Size = sizeof(*surface);
    if (!Radeon3DAllocSurface(device, SIDE, SIDE,
                             RADEON3D_FORMAT_B8G8R8A8, surface))
        return FALSE;
    return surface->Handle && surface->CpuAddress &&
           surface->Generation == generation && surface->Width == SIDE &&
           surface->Height == SIDE && surface->Pitch == SIDE * 4UL &&
           surface->Format == RADEON3D_FORMAT_B8G8R8A8;
}

static void FillTexture(const struct Radeon3DSurface *surface, BOOL green)
{
    ULONG x, y;
    for (y = 0; y < SIDE; ++y) {
        volatile UBYTE *pixel = (volatile UBYTE *)surface->CpuAddress +
                                y * surface->Pitch;
        for (x = 0; x < SIDE; ++x, pixel += 4) {
            pixel[0] = 0;
            pixel[1] = green ? 255 : 0;
            pixel[2] = green ? 0 : 255;
            pixel[3] = 255;
        }
    }
}

static void DrawRecord(ULONG *record, const struct Radeon3DSurface *target,
                       const struct Radeon3DSurface *texture,
                       BOOL right, BOOL linear)
{
    static const UBYTE corners[12] = {0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1};
    ULONG index;
    memset(record, 0, DRAW_WORDS * sizeof(*record));
    record[0] = RADEON3D_EXEC_DRAW_TRIANGLES;
    record[1] = DRAW_WORDS;
    record[2] = (ULONG)target->Handle;
    record[4] = (ULONG)texture->Handle;
    record[5] = RADEON3D_DRAW_TEXTURED | RADEON3D_DRAW_FRAGMENT_STATE |
                RADEON3D_DRAW_EXTENDED_VERTEX | RADEON3D_DRAW_HW_TCL;
    record[8] = record[9] = SIDE;
    record[10] = 6;
    record[12] = (SIDE - 1UL) | ((SIDE - 1UL) << 16);
    record[13] = linear ? RADEON3D_TEX_MAG_LINEAR : 0;
    for (index = 0; index < 16; index += 5)
        record[21 + index] = 0x3f800000UL;
    record[33] = right ? 0x3f000000UL : 0xbf000000UL; /* MVP translate +/-0.5 */
    record[37] = record[38] = record[39] = record[40] = 0x42000000UL; /* 32 */
    record[41] = record[42] = 0x3f000000UL;
    record[43] = 16UL << RADEON3D_TRANSFORM_POINT_SIZE_SHIFT;
    for (index = 0; index < 6; ++index) {
        ULONG *vertex = record + RADEON3D_EXEC_DRAW_HW_TCL_HEADER_DWORDS +
                       index * RADEON3D_EXEC_HW_TCL_VERTEX_DWORDS;
        vertex[0] = corners[index * 2] ? 0x3e800000UL : 0xbe800000UL;
        vertex[1] = corners[index * 2 + 1] ? 0x3e800000UL : 0xbe800000UL;
        vertex[3] = 0x3f800000UL;
        vertex[4] = 0xffffffffUL;
        vertex[5] = vertex[6] = 0x3f000000UL;
    }
}

/* RGB is the oracle; P96 color targets need not preserve alpha. */
static ULONG Pixel(const struct Radeon3DSurface *surface, ULONG x)
{
    volatile UBYTE *pixel = (volatile UBYTE *)surface->CpuAddress +
                            32UL * surface->Pitch + x * 4UL;
    return ((ULONG)pixel[2] << 16) | ((ULONG)pixel[1] << 8) | pixel[0];
}

int main(void)
{
    struct Radeon3DDevice *device = NULL;
    struct Radeon3DInfo info = {0};
    struct Radeon3DSurface target = {0}, red = {0}, green = {0};
    ULONG test, fence = 0, failures = 0;
    int result = 20;
    static const char *names[] = {"matrix-only", "texture-only",
                                  "texture-and-matrix", "sampler-and-matrix"};

    Radeon9200Base = OpenLibrary((CONST_STRPTR)"Radeon9200.chip",
                                 RADEON3D_LIBRARY_VERSION);
    if (!Radeon9200Base)
        goto out;
    info.Size = sizeof(info);
    device = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
    if (!device || !(info.Caps & RADEON3D_CAP_AUX_SURFACES) ||
        !(info.Caps & RADEON3D_CAP_HW_TRANSFORM_CLIP) ||
        !AllocSurface(device, &target, info.Generation) ||
        !AllocSurface(device, &red, info.Generation) ||
        !AllocSurface(device, &green, info.Generation))
        goto out;
    FillTexture(&red, FALSE);
    FillTexture(&green, TRUE);
    for (test = 0; test < 4; ++test) {
        ULONG left, right, wantLeft, wantRight;
        BOOL textureChange = test == 1 || test == 2;
        BOOL matrixChange = test != 1;
        memset(Records, 0, sizeof(Records));
        Records[0] = RADEON3D_EXEC_CLEAR;
        Records[1] = RADEON3D_EXEC_CLEAR_DWORDS;
        Records[2] = (ULONG)target.Handle;
        Records[4] = RADEON3D_CLEAR_COLOR;
        Records[9] = Records[10] = SIDE;
        DrawRecord(Records + RADEON3D_EXEC_CLEAR_DWORDS, &target, &red,
                   FALSE, FALSE);
        DrawRecord(Records + RADEON3D_EXEC_CLEAR_DWORDS + DRAW_WORDS, &target,
                   textureChange ? &green : &red, matrixChange, test == 3);
        if (!Radeon3DExecute(device, Records, sizeof(Records) / sizeof(Records[0]),
                              RADEON3D_SUBMIT_FENCE, &fence) || !fence ||
            !Radeon3DWaitFence(device, fence, 1000)) {
            printf("R3DTEXMATRIX case=%s submit-or-wait=failed\n", names[test]);
            goto out;
        }
        left = Pixel(&target, 16);
        right = Pixel(&target, 48);
        wantLeft = matrixChange ? 0xff0000UL : 0x00ff00UL;
        wantRight = matrixChange ? (textureChange ? 0x00ff00UL : 0xff0000UL) : 0;
        if (left != wantLeft || right != wantRight)
            ++failures;
        printf("R3DTEXMATRIX case=%s left=%06lx right=%06lx want=%06lx,%06lx %s\n",
               names[test], (unsigned long)left, (unsigned long)right,
               (unsigned long)wantLeft, (unsigned long)wantRight,
               left == wantLeft && right == wantRight ? "PASS" : "FAIL");
    }
    result = failures ? 5 : 0;
out:
    if (device) {
        if (green.Handle) Radeon3DReleaseSurface(device, &green);
        if (red.Handle) Radeon3DReleaseSurface(device, &red);
        if (target.Handle) Radeon3DReleaseSurface(device, &target);
        Radeon3DClose(device);
    }
    if (Radeon9200Base)
        CloseLibrary(Radeon9200Base);
    printf("R3DTEXMATRIX result=%d failures=%lu\n", result, (unsigned long)failures);
    return result;
}
