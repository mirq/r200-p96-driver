/* Paired-record emitter tests: CPU memory only, no Radeon service or GPU. */
#include <stdio.h>
#include <string.h>
#include "radeon3d_emit.h"

static struct Radeon3DEmitter Emitter;
static ULONG Words[RADEON3D_MAX_BATCH_DWORDS];
static ULONG Record[512];
static ULONG Offsets[8];
static UBYTE Pixels[64 * 64 * 4];
static unsigned Checks, Failures;

#define CHECK(test) do { ++Checks; if (!(test)) { \
    ++Failures; printf("TEX_MATRIX_FAIL line=%d\n", __LINE__); } } while (0)

static struct Radeon3DEmitSurface *Resolve(void *user, ULONG handle,
                                         struct Radeon3DEmitSurface *surface)
{
    (void)user;
    if (!handle || handle > 4UL)
        return NULL;
    surface->CpuAddress = Pixels;
    surface->GpuAddress = handle * 0x00200000UL;
    surface->Pitch = 256;
    surface->Width = surface->Height = 64;
    surface->Format = RADEON3D_FORMAT_B8G8R8A8;
    return surface;
}

static void Identity(ULONG *matrix)
{
    unsigned index;
    for (index = 0; index < 16; ++index)
        matrix[index] = index % 5 ? 0 : 0x3f800000UL;
}

static ULONG Setup(BOOL commit, BOOL texgen, BOOL normals, int lights)
{
    ULONG header = texgen ? 78UL : 44UL;
    ULONG stride = normals ? 13UL : 10UL;
    ULONG vertex, index;

    memset(&Emitter, 0, sizeof(Emitter));
    memset(Record, 0, sizeof(Record));
    Emitter.Words = Words;
    Emitter.InterfaceVersion = RADEON3D_IFACE_VERSION;
    Emitter.Resolve = Resolve;
    Emitter.CommitVbuf = commit;
    Emitter.CommitSegmentGpuBase = 0x01000000UL;
    Emitter.CommitSegmentBytes = 65536;
    Emitter.CommitVertexOffsets = Offsets;
    Radeon3DEmitResetCaptures(&Emitter);
    Record[0] = RADEON3D_EXEC_DRAW_TRIANGLES;
    Record[2] = 1;
    Record[4] = 2;
    Record[5] = RADEON3D_DRAW_HW_TCL | RADEON3D_DRAW_EXTENDED_VERTEX |
                RADEON3D_DRAW_FRAGMENT_STATE | RADEON3D_DRAW_TEXTURED;
    Record[8] = Record[9] = 64;
    Record[10] = 3;
    Record[12] = 63UL | (63UL << 16);
    Identity(Record + 21);
    Record[37] = Record[38] = Record[39] = Record[40] = UnsignedFloatBits(32);
    Record[41] = Record[42] = 0x3f000000UL;
    Record[43] = 16UL << RADEON3D_TRANSFORM_POINT_SIZE_SHIFT;
    if (texgen) {
        Record[5] |= RADEON3D_DRAW_TEXGEN;
        Record[15] = 3;
        Record[17] = Record[12];
        Record[19] = RADEON3D_VERTEX_TEXTURE1;
        Record[44] = Record[45] = RADEON3D_TEXGEN_MODE_OBJECT_LINEAR |
                                  RADEON3D_TEXGEN_GEN_S | RADEON3D_TEXGEN_GEN_T;
        Identity(Record + 46);
        Identity(Record + 62);
    }
    if (normals) {
        Record[5] |= RADEON3D_DRAW_NORMALS;
        Identity(Record + header);
        Identity(Record + header + 16);
        header += 32;
    }
    if (lights >= 0) {
        Record[5] |= RADEON3D_DRAW_LIGHTING;
        Record[header + 8] = (1UL << lights) - 1UL;
        header += RADEON3D_EXEC_LIGHT_STATE_DWORDS +
                  (ULONG)lights * RADEON3D_EXEC_LIGHT_BLOCK_DWORDS;
    }
    for (vertex = 0; vertex < 3; ++vertex) {
        ULONG *input = Record + header + vertex * stride;
        input[3] = 0x3f800000UL;
        input[normals ? 7 : 4] = 0xffffffffUL;
    }
    for (index = 0; index < 8; ++index)
        Offsets[index] = index * 512UL;
    Record[1] = commit ? header : header + 3UL * stride;
    return header;
}

static BOOL Draw(void)
{
    return Radeon3DEmitDraw(&Emitter, Record, Record[1],
                            R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST);
}

static ULONG DrawWords(BOOL commit, BOOL texgen, BOOL normals)
{
    ULONG arrays = 3UL + (normals ? 1UL : 0UL) + (texgen ? 1UL : 0UL);
    if (commit)
        return 4UL + (arrays / 2UL) * 3UL + (arrays & 1UL) * 2UL;
    return 2UL + 3UL * (7UL + (normals ? 3UL : 0UL) + (texgen ? 2UL : 0UL));
}

static unsigned MatrixUploads(ULONG first, ULONG address, const ULONG *matrix,
                              BOOL transpose)
{
    ULONG index, row, column;
    unsigned found = 0;
    for (index = first; index + 21UL <= Emitter.Count; ++index) {
        if (Words[index] != RADEON_CP_PACKET0(R200_SE_TCL_STATE_FLUSH, 0) ||
            Words[index + 2] != RADEON_CP_PACKET0(R200_SE_TCL_VECTOR_INDX_REG, 0) ||
            Words[index + 3] != ((1UL << R200_VEC_INDX_OCTWORD_STRIDE_SHIFT) |
                                  address) ||
            Words[index + 4] != RADEON_CP_PACKET0_ONE(R200_SE_TCL_VECTOR_DATA_REG, 15))
            continue;
        ++found;
        for (row = 0; row < 4; ++row)
            for (column = 0; column < 4; ++column)
                CHECK(Words[index + 5UL + row * 4UL + column] ==
                      matrix[transpose ? column * 4UL + row : row * 4UL + column]);
    }
    return found;
}

static void Pair(BOOL commit, BOOL texgen, BOOL normals, int lights,
                 ULONG changes, BOOL switchUnit1)
{
    ULONG first, matrixBase = texgen ? 78UL : 44UL;
    ULONG expected = 14UL + DrawWords(commit, texgen, normals);
    struct Radeon3DEmitState *next;
    Setup(commit, texgen, normals, lights);
    CHECK(Draw());
    first = Emitter.Count;
    next = Emitter.Build;
    Record[switchUnit1 ? 15 : 4] = 4;
    /* Off-diagonal entries distinguish transpose from direct uploads. */
    if (changes & 1UL) Record[22] = 0x3e800000UL;
    if (changes & 2UL) Record[47] = 0x3f000000UL;
    if (changes & 4UL) Record[63] = 0x3f400000UL;
    if (changes & 8UL) Record[matrixBase + 1] = 0x3e000000UL;
    if (changes & 16UL) Record[matrixBase + 17] = 0x3e400000UL;
    CHECK(Draw());
    CHECK(Emitter.Live == next);
    CHECK(Emitter.Live != Emitter.Build);
    CHECK(Emitter.StateValid);
    CHECK(MatrixUploads(first, R200_VS_MATRIX_2_MVP, Record + 21, TRUE) ==
          ((changes & 1UL) ? 1U : 0U));
    CHECK(Emitter.Matrix[1] == Record[22]);
    if (texgen) {
        CHECK(MatrixUploads(first, R200_VS_MATRIX_3_TEX0, Record + 46, TRUE) ==
              ((changes & 2UL) ? 1U : 0U));
        CHECK(MatrixUploads(first, R200_VS_MATRIX_4_TEX1, Record + 62, TRUE) ==
              ((changes & 4UL) ? 1U : 0U));
        CHECK(Emitter.TexGenMatrix[0][1] == Record[47]);
        CHECK(Emitter.TexGenMatrix[1][1] == Record[63]);
    }
    if (normals) {
        CHECK(MatrixUploads(first, R200_VS_MATRIX_0_MV, Record + matrixBase, TRUE) ==
              ((changes & 8UL) ? 1U : 0U));
        CHECK(MatrixUploads(first, R200_VS_MATRIX_1_INV_MV,
                            Record + matrixBase + 16, FALSE) ==
              ((changes & 16UL) ? 1U : 0U));
        CHECK(Emitter.ModelView[1] == Record[matrixBase + 1]);
        CHECK(Emitter.InvModelView[1] == Record[matrixBase + 17]);
    }
    while (changes) {
        expected += (changes & 1UL) ? 21UL : 0;
        changes >>= 1;
    }
    CHECK(Emitter.Count - first == expected);
    first = Emitter.Count;
    CHECK(Draw());
    CHECK(MatrixUploads(first, R200_VS_MATRIX_2_MVP, Record + 21, TRUE) == 0);
    CHECK(Emitter.Count - first == DrawWords(commit, texgen, normals) +
          (lights >= 0 ? 43UL + 39UL * (ULONG)lights : 0));
    if (commit) {
        first = Emitter.Count;
        CHECK(Radeon3DEmitVbufDraw(&Emitter, 2048, 3,
                                   R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST));
        CHECK(Emitter.Count - first == DrawWords(TRUE, texgen, normals));
    }
}

int main(void)
{
    BOOL commit;
    ULONG first;
    for (commit = FALSE; commit <= TRUE; ++commit) {
        Pair(commit, FALSE, FALSE, -1, 0, FALSE);
        Pair(commit, FALSE, FALSE, -1, 1, FALSE);
        Pair(commit, TRUE, FALSE, -1, 2, FALSE);
        Pair(commit, TRUE, FALSE, -1, 4, FALSE);
        Pair(commit, TRUE, FALSE, -1, 2, TRUE);
        Pair(commit, TRUE, FALSE, -1, 7, TRUE);
        Pair(commit, FALSE, TRUE, -1, 8, FALSE);
        Pair(commit, FALSE, TRUE, -1, 16, FALSE);
        Pair(commit, FALSE, TRUE, -1, 25, FALSE);
        Pair(commit, TRUE, TRUE, -1, 31, FALSE);
        Pair(commit, TRUE, TRUE, 0, 0, FALSE);
        Pair(commit, TRUE, TRUE, 1, 0, FALSE);
        Pair(commit, TRUE, TRUE, 8, 0, FALSE);
        Pair(commit, TRUE, TRUE, 1, 31, TRUE);

        Setup(commit, FALSE, TRUE, 1);
        CHECK(Draw());
        first = Emitter.Count;
        Record[4] = 4;
        Record[76] = 0x3f000000UL; /* Lighting change must use full state. */
        CHECK(Draw());
        CHECK(Emitter.Count - first > 14UL + DrawWords(commit, FALSE, TRUE));

        Setup(commit, FALSE, FALSE, -1);
        CHECK(Draw());
        Record[4] = 4;
        Record[22] = 0x3f000000UL;
        Emitter.Count = RADEON3D_MAX_BATCH_DWORDS - 14UL - 10UL;
        CHECK(!Draw());
        CHECK(Emitter.Count <= RADEON3D_MAX_BATCH_DWORDS);
    }
    printf("TEX_MATRIX_CHECK checks=%u failures=%u\n", Checks, Failures);
    return Failures ? 5 : 0;
}
