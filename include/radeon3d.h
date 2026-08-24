#ifndef RADEON3D_H
#define RADEON3D_H

#include <exec/types.h>

#define RADEON3D_LIBRARY_VERSION 3UL
#define RADEON3D_IFACE_VERSION   13UL

#define RADEON3D_CAP_CP_READY     (1UL << 0)
#define RADEON3D_CAP_SINGLE_BOARD (1UL << 1)
#define RADEON3D_CAP_OWNER_PINNED  (1UL << 2)
#define RADEON3D_CAP_PACKET2_SUBMIT (1UL << 3)
#define RADEON3D_CAP_FENCES         (1UL << 4)
#define RADEON3D_CAP_BITMAP_IMPORT  (1UL << 5)
#define RADEON3D_CAP_IMMD_TRI_LIST  (1UL << 6)
#define RADEON3D_CAP_PHASE2_EXECUTE (1UL << 7)
#define RADEON3D_CAP_DEPTH_FUNCS        (1UL << 8)
#define RADEON3D_CAP_TEXTURE_STATE      (1UL << 9)
#define RADEON3D_CAP_FOG_MULTITEX       (1UL << 10)
/* Diagnostic builds only; the ABI vector remains present in release builds. */
#define RADEON3D_CAP_TEST_INVALIDATE        (1UL << 11)
#define RADEON3D_CAP_COLOR_TARGET_FORMATS   (1UL << 12)
#define RADEON3D_CAP_NATIVE_TRI_PRIMITIVES  (1UL << 13)
#define RADEON3D_CAP_NATIVE_QUAD_LISTS      (1UL << 14)
#define RADEON3D_CAP_HW_TRANSFORM_CLIP      (1UL << 15)
#define RADEON3D_CAP_HW_TEXGEN              (1UL << 16)
#define RADEON3D_CAP_HW_NORMALS             (1UL << 17)
#define RADEON3D_CAP_HW_LIGHTING            (1UL << 18)
#define RADEON3D_CAP_HW_SPHERE_MAP          (1UL << 19)
#define RADEON3D_CAP_COMPACT_TCL_VERTEX     (1UL << 20)
#define RADEON3D_CAP_STREAM_SEGMENTS        (1UL << 21)

#define RADEON3D_MAX_BATCH_DWORDS 8192UL
#define RADEON3D_IMMD_MAX_VERTICES 255UL
#define RADEON3D_IMMD_MAX_QUAD_VERTICES 252UL
#define RADEON3D_IMMD_MAX_EXTENDED_QUAD_VERTICES 252UL
#define RADEON3D_IMMD_VERTEX_DWORDS 3UL

/* Streaming segments (interface 13). A segment is service-allocated private
 * VRAM that the context-owning task writes through its CpuAddress; draw
 * commits reference vertex data at GpuAddress + OffsetBytes instead of
 * carrying vertices inline in the Execute record. Segments are freed
 * automatically when the device closes. */
#define RADEON3D_MAX_SEGMENTS      8UL
#define RADEON3D_MAX_SEGMENT_BYTES (256UL * 1024UL)

#define RADEON3D_SEGMENT_VERSION 1UL

struct Radeon3DSegment {
    ULONG Size;
    ULONG Version;
    ULONG Id;
    APTR CpuAddress;
    ULONG GpuAddress;
    ULONG Bytes;
};

#define RADEON3D_SEGMENT_V1_SIZE 24UL

typedef char Radeon3DSegmentV1SizeCheck[
    sizeof(struct Radeon3DSegment) == RADEON3D_SEGMENT_V1_SIZE ? 1 : -1];

/* Draw commit: the header is an ordinary HW-TCL draw-record header whose
 * record[10] vertex count is the real count, but only HeaderDwords dwords
 * are supplied — the vertices are fetched from the segment by the hardware.
 * OffsetBytes must be 4-byte aligned and leave room for the full vertex
 * array inside the segment. Flags accept RADEON3D_SUBMIT_FENCE. */
#define RADEON3D_COMMIT_VERSION 1UL

struct Radeon3DCommit {
    ULONG Size;
    ULONG Version;
    ULONG SegmentId;
    ULONG OffsetBytes;
    const ULONG *Header;
    ULONG HeaderDwords;
    ULONG Flags;
};

#define RADEON3D_COMMIT_V1_SIZE 28UL

typedef char Radeon3DCommitV1SizeCheck[
    sizeof(struct Radeon3DCommit) == RADEON3D_COMMIT_V1_SIZE ? 1 : -1];

/* Batched commit: Records is a chain of header-only draw records linked by
 * their dword[1] length fields, each carrying its real vertex count in
 * dword[10]. VertexOffsets holds one byte offset into the segment per
 * record, in record order. Every record must be a hardware-TCL draw;
 * clears and non-TCL records are rejected. One ring submit, one fence. */
#define RADEON3D_COMMIT_BATCH_VERSION 1UL

struct Radeon3DCommitBatch {
    ULONG Size;
    ULONG Version;
    ULONG SegmentId;
    const ULONG *Records;
    ULONG RecordDwords;
    const ULONG *VertexOffsets;
    ULONG RecordCount;
    ULONG Flags;
};

#define RADEON3D_COMMIT_BATCH_V1_SIZE 32UL

typedef char Radeon3DCommitBatchV1SizeCheck[
    sizeof(struct Radeon3DCommitBatch) == RADEON3D_COMMIT_BATCH_V1_SIZE
        ? 1 : -1];

#define RADEON3D_SUBMIT_FENCE (1UL << 0)
#define RADEON3D_SUBMIT_FLAGS  RADEON3D_SUBMIT_FENCE

#define RADEON3D_EXEC_CLEAR          1UL
#define RADEON3D_EXEC_DRAW_TRIANGLES 2UL
#define RADEON3D_EXEC_DRAW_TRI_STRIP 3UL
#define RADEON3D_EXEC_DRAW_TRI_FAN   4UL
#define RADEON3D_EXEC_DRAW_QUADS     5UL
#define RADEON3D_EXEC_DRAW_POINTS    6UL
#define RADEON3D_EXEC_DRAW_LINES     7UL
#define RADEON3D_EXEC_DRAW_LINE_STRIP 8UL
#define RADEON3D_EXEC_DRAW_LINE_LOOP 9UL

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
#define RADEON3D_DRAW_FRAGMENT_STATE (1UL << 8)
#define RADEON3D_DRAW_EXTENDED_VERTEX (1UL << 9)
#define RADEON3D_DRAW_HW_TCL         (1UL << 10)
#define RADEON3D_DRAW_TEXGEN         (1UL << 11)
#define RADEON3D_DRAW_NORMALS        (1UL << 12)
#define RADEON3D_DRAW_LIGHTING       (1UL << 13)
/* Hardware-TCL records only: vertices omit the unit-1 texture and fog
 * dwords entirely instead of carrying zeros (stride 10 with normals,
 * 7 without). Requires RADEON3D_CAP_COMPACT_TCL_VERTEX and is rejected
 * when RADEON3D_VERTEX_TEXTURE1 or RADEON3D_VERTEX_FOG is set. */
#define RADEON3D_DRAW_COMPACT_VERTEX (1UL << 14)
#define RADEON3D_DRAW_OPTIONS_BASIC  0x000000ffUL
#define RADEON3D_DRAW_OPTIONS_FRAGMENT 0x000001ffUL
#define RADEON3D_DRAW_OPTIONS_PRE_TCL 0x000003ffUL
#define RADEON3D_DRAW_OPTIONS        0x00007fffUL

#define RADEON3D_TEX_MODULATE        (1UL << 0)
#define RADEON3D_TEX_REPEAT_S        (1UL << 1)
#define RADEON3D_TEX_REPEAT_T        (1UL << 2)
#define RADEON3D_TEX_MAG_LINEAR      (1UL << 3)
#define RADEON3D_TEX_MIN_SHIFT       4UL
#define RADEON3D_TEX_MIN_MASK        (7UL << RADEON3D_TEX_MIN_SHIFT)
#define RADEON3D_TEX_LEVELS_SHIFT    8UL
#define RADEON3D_TEX_LEVELS_MASK     (15UL << RADEON3D_TEX_LEVELS_SHIFT)
#define RADEON3D_TEX_STATE_MASK      0x00000f7fUL
/* Bits 16-31 carry the client's texture content serial. It changes whenever
 * an in-place texel update reaches the surface, forcing the service to
 * re-emit the texture unit state (including PP_TXOFFSET) so the GPU's
 * texture cache re-fetches the rewritten lines. */
#define RADEON3D_TEX_CONTENT_SHIFT   16UL
#define RADEON3D_TEX_CONTENT_MASK    (0xffffUL << RADEON3D_TEX_CONTENT_SHIFT)

#define RADEON3D_TEX_MIN_NEAREST                 0UL
#define RADEON3D_TEX_MIN_LINEAR                  1UL
#define RADEON3D_TEX_MIN_NEAREST_MIPMAP_NEAREST 2UL
#define RADEON3D_TEX_MIN_LINEAR_MIPMAP_NEAREST  3UL
/* Semantic values: the service maps these last two to raw R200 filter
 * encodings 6 and 7, as in Mesa's r200SetTexFilter(). */
#define RADEON3D_TEX_MIN_NEAREST_MIPMAP_LINEAR  4UL
#define RADEON3D_TEX_MIN_LINEAR_MIPMAP_LINEAR   5UL

/* Texgen is available only with RADEON3D_DRAW_TEXGEN and
 * RADEON3D_DRAW_HW_TCL. Sphere map additionally requires
 * RADEON3D_DRAW_NORMALS and generates exactly S/T. A set component bit
 * requests generated output; clear components retain supplied coordinates. */
#define RADEON3D_TEXGEN_MODE_MASK          0x0000000fUL
#define RADEON3D_TEXGEN_MODE_OFF           0UL
#define RADEON3D_TEXGEN_MODE_OBJECT_LINEAR 1UL
#define RADEON3D_TEXGEN_MODE_SPHERE_MAP    2UL
#define RADEON3D_TEXGEN_GEN_S               (1UL << 4)
#define RADEON3D_TEXGEN_GEN_T               (1UL << 5)
#define RADEON3D_TEXGEN_GEN_R               (1UL << 6)
#define RADEON3D_TEXGEN_GEN_Q               (1UL << 7)
#define RADEON3D_TEXGEN_COMPONENTS          0x000000f0UL
#define RADEON3D_TEXGEN_STATE_MASK          0x000000ffUL

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

#define RADEON3D_VERTEX_FOG                  (1UL << 0)
#define RADEON3D_VERTEX_TEXTURE1             (1UL << 1)
/* Extended vertex dword 8 is positive finite clip W. Perspective is
 * independent of texture unit 1 and mutually exclusive with fog. S/T are
 * homogeneous clip x/y/z/w and raw S/T, following Mesa hardware projection. */
#define RADEON3D_VERTEX_CLIP_COORDINATES     (1UL << 2)
#define RADEON3D_VERTEX_STATE_MASK            0x00000007UL

/* Hardware transform/clip state. The front-face convention is OpenGL's
 * pre-viewport convention; Radeon9200.chip owns the hardware Y inversion. */
#define RADEON3D_TRANSFORM_CULL_FRONT        (1UL << 0)
#define RADEON3D_TRANSFORM_CULL_BACK         (1UL << 1)
#define RADEON3D_TRANSFORM_FRONT_CCW         (1UL << 2)
#define RADEON3D_TRANSFORM_FLAT_SHADE        (1UL << 3)
#define RADEON3D_TRANSFORM_POLYGON_LINE      (1UL << 4)
#define RADEON3D_TRANSFORM_POLYGON_POINT     (1UL << 5)
#define RADEON3D_TRANSFORM_POINT_SIZE_SHIFT  8UL
#define RADEON3D_TRANSFORM_POINT_SIZE_MASK   (0x0fffUL << 8)
#define RADEON3D_TRANSFORM_STATE_MASK        0x000fff3fUL

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
#define RADEON3D_EXEC_DRAW_FRAGMENT_HEADER_DWORDS 15UL
#define RADEON3D_EXEC_DRAW_EXTENDED_HEADER_DWORDS 21UL
#define RADEON3D_EXEC_DRAW_HW_TCL_HEADER_DWORDS 44UL
#define RADEON3D_EXEC_DRAW_TEXGEN_HEADER_DWORDS 78UL
#define RADEON3D_EXEC_NORMAL_MATRICES_DWORDS 32UL
#define RADEON3D_EXEC_LIGHT_STATE_DWORDS 26UL
#define RADEON3D_EXEC_LIGHT_BLOCK_DWORDS 31UL
#define RADEON3D_EXEC_MAX_LIGHT_BLOCKS 8UL
#define RADEON3D_EXEC_VERTEX_DWORDS       6UL
#define RADEON3D_EXEC_EXTENDED_VERTEX_DWORDS 9UL
#define RADEON3D_EXEC_HW_TCL_VERTEX_DWORDS 10UL
#define RADEON3D_EXEC_HW_TCL_NORMAL_VERTEX_DWORDS 13UL

/* Lighting and normal state (interface 11). RADEON3D_DRAW_LIGHTING implies
 * RADEON3D_DRAW_NORMALS and hardware TCL. Light positions and directions are
 * supplied in eye space, as OpenGL derives them at specification time.
 *
 * lightControl bits:
 *   0..7   enabled-light mask, one bit per GL_LIGHT0..GL_LIGHT7; the header
 *          carries one dense 31-dword block per set bit in ascending order
 *   8      local viewer specular model
 *   16..23 light N is a spotlight (cutoff below 180 degrees)
 *   24..31 light N enables range attenuation */
#define RADEON3D_LIGHT_CONTROL_ENABLED_MASK 0x000000ffUL
#define RADEON3D_LIGHT_LOCAL_VIEWER         (1UL << 8)
#define RADEON3D_LIGHT_SPOT_SHIFT           16UL
#define RADEON3D_LIGHT_ATTEN_SHIFT          24UL
#define RADEON3D_LIGHT_CONTROL_RESERVED     0x0000fe00UL

/* Light block: 24 vector dwords then 7 scalar dwords, all IEEE floats.
 * Vector: ambient RGBA, diffuse RGBA, specular RGBA, eye-space position XYZW,
 * negated normalized spot direction XYZW, attenuation quadratic/linear/
 * constant/reserved. Scalar: spot DCD, DCM, exponent, cos(cutoff), specular
 * threshold, squared range cutoff, 1/constant-attenuation (or +inf). */
#define RADEON3D_LIGHT_BLOCK_VECTOR_DWORDS 24UL
#define RADEON3D_LIGHT_BLOCK_SCALAR_DWORDS 7UL

/* Material block: emissive, ambient, diffuse, specular RGBA then shininess. */
#define RADEON3D_MATERIAL_DWORDS 17UL

/* Basic/fragment: x,y,z,s0,t0,packedColor.
 * Extended: x,y,z,s0,t0,packedColor,s1,t1,fog-or-w.
 * HW TCL: object x,y,z,w,packedARGBColor,s0,t0,s1,t1,fog.
 * packedARGBColor is (alpha<<24)|(red<<16)|(green<<8)|blue, the same packing
 * and byte order the non-TCL paths use.
 * HW TCL header dwords 21..36 are an OpenGL column-major MVP matrix; dwords
 * 37..42 are viewport X/Y/Z scale and offset pairs; dword 43 is TCL state.
 * A HW-TCL texgen header adds unit 0/1 state at dwords 44/45 followed by
 * OpenGL column-major matrices at dwords 46..61/62..77. Object-linear mode
 * uses plane matrices; sphere-map mode uses the OpenGL texture matrix.
 * A normals header adds the model-view and inverse model-view matrices
 * immediately after the TCL (and optional texgen) block. A lighting header
 * then adds global ambient, eye vector, light control, and front material,
 * followed by one dense block per enabled light. Vertices gain a normal XYZ
 * triple after W. */

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
    /* V2: cumulative Radeon3DExecute phase attribution, present in release
     * builds too. Microsecond totals across all sessions of this service;
     * read them after a workload to attribute Execute time between the
     * trusted-record copy, the CP-stream builder, and the ring submit. */
    ULONG ExecCalls;
    ULONG ExecRecordDwords;
    ULONG ExecGeneratedDwords;
    ULONG ExecCopyMicros;
    ULONG ExecBuildMicros;
    ULONG ExecSubmitMicros;
    /* V3: which check rejected the most recent streaming commit, zero if
     * none has failed. Stage numbers are internal to the service and only
     * meaningful when read right after a failed commit. */
    ULONG CommitFailStage;
};

#define RADEON3D_INFO_V1_SIZE 32UL
#define RADEON3D_INFO_V2_SIZE 56UL
#define RADEON3D_INFO_V3_SIZE 60UL

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

typedef char Radeon3DInfoV3SizeCheck[
    sizeof(struct Radeon3DInfo) == RADEON3D_INFO_V3_SIZE ? 1 : -1];
#ifdef __GNUC__
typedef char Radeon3DInfoV1PrefixCheck[
    __builtin_offsetof(struct Radeon3DInfo, ExecCalls) ==
        RADEON3D_INFO_V1_SIZE ? 1 : -1];
#endif

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
