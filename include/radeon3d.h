#ifndef RADEON3D_H
#define RADEON3D_H

#include <exec/types.h>

#define RADEON3D_LIBRARY_VERSION 3UL
#define RADEON3D_IFACE_VERSION   7UL

#define RADEON3D_CAP_CP_READY     (1UL << 0)
#define RADEON3D_CAP_SINGLE_BOARD (1UL << 1)
#define RADEON3D_CAP_OWNER_PINNED  (1UL << 2)
#define RADEON3D_CAP_PACKET2_SUBMIT (1UL << 3)
#define RADEON3D_CAP_FENCES         (1UL << 4)
#define RADEON3D_CAP_BITMAP_IMPORT  (1UL << 5)
#define RADEON3D_CAP_IMMD_TRI_LIST  (1UL << 6)
#define RADEON3D_CAP_PHASE2_EXECUTE (1UL << 7)
#define RADEON3D_CAP_PHASE4_DEPTH_FUNCS (1UL << 8)
#define RADEON3D_CAP_PHASE5_TEXTURE_STATE (1UL << 9)
#define RADEON3D_CAP_PHASE6_FOG_MULTITEX   (1UL << 10)
/* Diagnostic builds only; the ABI vector remains present in release builds. */
#define RADEON3D_CAP_TEST_INVALIDATE        (1UL << 11)
#define RADEON3D_CAP_COLOR_TARGET_FORMATS   (1UL << 12)
#define RADEON3D_CAP_NATIVE_TRI_PRIMITIVES  (1UL << 13)

#define RADEON3D_MAX_BATCH_DWORDS 8192UL
#define RADEON3D_IMMD_MAX_VERTICES 255UL
#define RADEON3D_IMMD_VERTEX_DWORDS 3UL

#define RADEON3D_SUBMIT_FENCE (1UL << 0)
#define RADEON3D_SUBMIT_FLAGS  RADEON3D_SUBMIT_FENCE

#define RADEON3D_EXEC_CLEAR          1UL
#define RADEON3D_EXEC_DRAW_TRIANGLES 2UL
#define RADEON3D_EXEC_DRAW_TRI_STRIP 3UL
#define RADEON3D_EXEC_DRAW_TRI_FAN   4UL

#define RADEON3D_CLEAR_COLOR (1UL << 0)
#define RADEON3D_CLEAR_DEPTH (1UL << 1)
#define RADEON3D_CLEAR_MASK  (RADEON3D_CLEAR_COLOR | RADEON3D_CLEAR_DEPTH)

#define RADEON3D_DRAW_TEXTURED       (1UL << 0)
#define RADEON3D_DRAW_BILINEAR       (1UL << 1)
#define RADEON3D_DRAW_ALPHA_BLEND    (1UL << 2)
#define RADEON3D_DRAW_DEPTH_LESS     (1UL << 3)
#define RADEON3D_DRAW_DEPTH_WRITE    (1UL << 4)
#define RADEON3D_DRAW_DEPTH_FUNC_SHIFT 5UL
#define RADEON3D_DRAW_DEPTH_FUNC_MASK  (7UL << RADEON3D_DRAW_DEPTH_FUNC_SHIFT)
#define RADEON3D_DEPTH_LESS      0UL
#define RADEON3D_DEPTH_LEQUAL    1UL
#define RADEON3D_DEPTH_EQUAL     2UL
#define RADEON3D_DEPTH_GEQUAL    3UL
#define RADEON3D_DEPTH_GREATER   4UL
#define RADEON3D_DEPTH_NOTEQUAL  5UL
#define RADEON3D_DEPTH_NEVER     6UL
#define RADEON3D_DEPTH_ALWAYS    7UL
#define RADEON3D_DRAW_DEPTH_FUNC(value) \
    ((value) << RADEON3D_DRAW_DEPTH_FUNC_SHIFT)
#define RADEON3D_DRAW_STATE_V4       (1UL << 8)
#define RADEON3D_DRAW_STATE_V5       (1UL << 9)
#define RADEON3D_DRAW_OPTIONS_V3     0x000000ffUL
#define RADEON3D_DRAW_OPTIONS_V4     0x000001ffUL
#define RADEON3D_DRAW_OPTIONS        0x000003ffUL

#define RADEON3D_TEX_MODULATE        (1UL << 0)
#define RADEON3D_TEX_REPEAT_S        (1UL << 1)
#define RADEON3D_TEX_REPEAT_T        (1UL << 2)
#define RADEON3D_TEX_MAG_LINEAR      (1UL << 3)
#define RADEON3D_TEX_MIN_SHIFT       4UL
#define RADEON3D_TEX_MIN_MASK        (7UL << RADEON3D_TEX_MIN_SHIFT)
#define RADEON3D_TEX_LEVELS_SHIFT    8UL
#define RADEON3D_TEX_LEVELS_MASK     (15UL << RADEON3D_TEX_LEVELS_SHIFT)
#define RADEON3D_TEX_STATE_MASK      0x00000f7fUL

#define RADEON3D_TEX_MIN_NEAREST                 0UL
#define RADEON3D_TEX_MIN_LINEAR                  1UL
#define RADEON3D_TEX_MIN_NEAREST_MIPMAP_NEAREST 2UL
#define RADEON3D_TEX_MIN_LINEAR_MIPMAP_NEAREST  3UL
#define RADEON3D_TEX_MIN_NEAREST_MIPMAP_LINEAR  4UL
#define RADEON3D_TEX_MIN_LINEAR_MIPMAP_LINEAR   5UL

#define RADEON3D_FRAGMENT_ALPHA_TEST       (1UL << 0)
#define RADEON3D_FRAGMENT_ALPHA_FUNC_SHIFT 1UL
#define RADEON3D_FRAGMENT_ALPHA_FUNC_MASK  (7UL << 1)
#define RADEON3D_FRAGMENT_ALPHA_REF_SHIFT  8UL
#define RADEON3D_FRAGMENT_ALPHA_REF_MASK   (255UL << 8)
#define RADEON3D_FRAGMENT_BLEND            (1UL << 16)
#define RADEON3D_FRAGMENT_SRC_SHIFT        17UL
#define RADEON3D_FRAGMENT_SRC_MASK         (15UL << 17)
#define RADEON3D_FRAGMENT_DST_SHIFT        21UL
#define RADEON3D_FRAGMENT_DST_MASK         (15UL << 21)
#define RADEON3D_FRAGMENT_STATE_MASK       0x01ffffffUL

#define RADEON3D_PHASE6_FOG                 (1UL << 0)
#define RADEON3D_PHASE6_TEXTURE1            (1UL << 1)
#define RADEON3D_PHASE6_STATE_MASK           0x00000003UL

#define RADEON3D_BLEND_ZERO                0UL
#define RADEON3D_BLEND_ONE                 1UL
#define RADEON3D_BLEND_SRC_COLOR           2UL
#define RADEON3D_BLEND_ONE_MINUS_SRC_COLOR 3UL
#define RADEON3D_BLEND_DST_COLOR           4UL
#define RADEON3D_BLEND_ONE_MINUS_DST_COLOR 5UL
#define RADEON3D_BLEND_SRC_ALPHA           6UL
#define RADEON3D_BLEND_ONE_MINUS_SRC_ALPHA 7UL
#define RADEON3D_BLEND_DST_ALPHA           8UL
#define RADEON3D_BLEND_ONE_MINUS_DST_ALPHA 9UL
#define RADEON3D_BLEND_SRC_ALPHA_SATURATE  10UL

#define RADEON3D_EXEC_CLEAR_DWORDS       11UL
#define RADEON3D_EXEC_DRAW_HEADER_DWORDS 11UL
#define RADEON3D_EXEC_DRAW_V4_HEADER_DWORDS 15UL
#define RADEON3D_EXEC_DRAW_V5_HEADER_DWORDS 21UL
#define RADEON3D_EXEC_VERTEX_DWORDS       6UL
#define RADEON3D_EXEC_V5_VERTEX_DWORDS    9UL

struct Radeon3DDevice;
struct BitMap;

/* Keep the chip-library reference open until after Radeon3DClose returns. */

/*
 * Public structures use only fixed-width AmigaOS types and the normal m68k
 * two-byte ABI alignment. Callers set Size before opening or querying.
 */
struct Radeon3DInfo {
    ULONG Size;
    ULONG Version;
    ULONG Generation;
    ULONG DeviceId;
    ULONG Caps;
    ULONG InstalledVram;
    ULONG Picasso96Vram;
    ULONG MaxBatchDwords;
};

#define RADEON3D_INFO_V1_SIZE 32UL

/* requestedVersion is the newest interface version understood by the caller. */

/*
 * Submission accepts PACKET2 no-ops and the bounded immediate triangle-list
 * stream documented in RADEON3D_SUBMISSION.md. General register and draw
 * packet submission remains unavailable.
 */

/*
 * Interface-v2 execution records are host-endian and self-contained. Handles
 * are the opaque values returned in Radeon3DSurface.Handle. See
 * RADEON3D_SUBMISSION.md for exact record layouts and restrictions.
 */

typedef char Radeon3DInfoV1SizeCheck[
    sizeof(struct Radeon3DInfo) == RADEON3D_INFO_V1_SIZE ? 1 : -1];

#define RADEON3D_SURFACE_VERSION 1UL

#define RADEON3D_FORMAT_R5G6B5PC  1UL
#define RADEON3D_FORMAT_B8G8R8A8  2UL
#define RADEON3D_FORMAT_CLUT8      3UL

struct Radeon3DSurface {
    ULONG Size;
    ULONG Version;
    ULONG Generation;
    APTR CpuAddress;
    ULONG GpuAddress;
    ULONG Pitch;
    ULONG Width;
    ULONG Height;
    ULONG Format;
    APTR Handle;
};

#define RADEON3D_SURFACE_V1_SIZE 40UL

typedef char Radeon3DSurfaceV1SizeCheck[
    sizeof(struct Radeon3DSurface) == RADEON3D_SURFACE_V1_SIZE ? 1 : -1];

/* The caller owns an imported BitMap until Radeon3DReleaseSurface returns. */

#endif
