# Radeon3D bounded submission contract

`Radeon3DSubmit()` is not an unrestricted command-processor escape hatch. An
interface-v1 client may submit either a batch consisting entirely of R200
`PACKET2` no-ops or the immediate triangle-list stream described below. All
dwords are host-endian; the service performs the PCI byte swapping.

Clients must require `RADEON3D_CAP_IMMD_TRI_LIST` before using this stream.

## Immediate triangle list

The stream contains 22 single-register `PACKET0` writes followed by one
`3D_DRAW_IMMD_2` packet. The register order and values are fixed except for the
four target-derived values noted below:

```text
SE_VAP_CNTL_STATUS   0
SE_VAP_CNTL          FORCE_W_TO_ONE | (9 << VF_MAX_VTX_NUM_SHIFT)
SE_VTX_STATE_CNTL    0
SE_VTE_CNTL          0
SE_VTX_FMT_0         packed RGBA color 0
SE_VTX_FMT_1         0
SE_CNTL              solid front/back, Gouraud, OGL pixel center, 1/4 rounding
PP_CNTL              texture blend stage 0 enabled
PP_TXCBLEND_0        diffuse color
PP_TXCBLEND2_0       clamp 0..1, output R0
PP_TXABLEND_0        diffuse alpha
PP_TXABLEND2_0       clamp 0..1, output R0
PP_CNTL_X            0
RE_AUX_SCISSOR_CNTL  0
RE_CNTL              0
RE_TOP_LEFT          0
RE_WIDTH_HEIGHT      ((surface height - 1) << 16) | (surface width - 1)
RB3D_PLANEMASK       0xffffffff
RB3D_BLENDCNTL       source ONE, destination ZERO
RB3D_CNTL            RGB565
RB3D_COLOROFFSET     imported surface GPU address
RB3D_COLORPITCH      imported surface pitch / 2
```

The target must be a live bitmap imported by the submitting session, begin at
the supplied 16-byte-aligned GPU address, use `RADEON3D_FORMAT_R5G6B5PC`, fit
the R200 width, height and color-pitch fields, and retain the dimensions and
pitch encoded in the stream.

The final packet is:

```text
PACKET3(3D_DRAW_IMMD_2, vertexCount * 3)
(vertexCount << 16) | PRIM_WALK_RING | TRI_LIST
vertex 0: IEEE-754 X, IEEE-754 Y, packed RGBA
...
```

`vertexCount` must be a positive multiple of three from 3 through
`RADEON3D_IMMD_MAX_VERTICES`. X and Y must be non-negative finite IEEE-754
single-precision values no greater than the imported surface width and height.
Packed colors are unrestricted. Extra packets, missing dwords, alternate
registers, alternate state values and unsupported targets are rejected before
the ring write pointer changes.

## Interface-v2 semantic execution

Clients that negotiate interface 2 and receive
`RADEON3D_CAP_PHASE2_EXECUTE` may call `Radeon3DExecute()`. This entry point
does not accept hardware packets or register values. It copies and validates a
host-endian record stream, resolves opaque imported-surface handles, generates
a complete trusted R200 stream, and submits it with the same service-owned
fence and recovery rules as `Radeon3DSubmit()`.

Every record starts with an opcode and its exact dword count, including the
two-dword header. A call may contain multiple GPU records. Parsing must end
exactly at the supplied dword count, and the generated stream may not exceed
`RADEON3D_MAX_BATCH_DWORDS`.

### Clear record

`RADEON3D_EXEC_CLEAR` contains exactly 11 dwords:

```text
opcode, 11
color target handle
depth target handle or zero
RADEON3D_CLEAR_COLOR/DEPTH mask
packed clear RGBA
IEEE-754 depth in [0,1]
left, top, right-exclusive, bottom-exclusive
```

The color target is a live session-owned imported RGB565 surface. Interface-v6
clients with `RADEON3D_CAP_COLOR_TARGET_FORMATS` may also use imported CLUT8
surfaces as RGB332 targets and imported B8G8R8A8 surfaces as ARGB8888 targets.
The service derives color format and pixel pitch from the imported surface;
RGB332 output uses hardware dithering. A depth clear requires a distinct,
non-overlapping imported RGB565 surface with the same dimensions and a
representable D16 pitch. A color-only clear requires a zero depth handle. The
scissor must be nonempty and contained by the target. The service emits two
triangles with fixed replace blending; depth clear uses D16, `ALWAYS`, and
depth writes.

### Triangle record

`RADEON3D_EXEC_DRAW_TRIANGLES` contains an 11-dword header followed by six
dwords per vertex:

```text
opcode, 11 + vertexCount * 6
color target handle
depth target handle or zero
texture handle or zero
options
left, top, right-exclusive, bottom-exclusive
vertexCount

per vertex: X, Y, Z, S, T IEEE-754 bits, packed RGBA
```

`vertexCount` is 3 through 255 and divisible by three. X/Y are finite,
non-negative, and within the color target. Z/S/T are finite values in `[0,1]`.
Untextured records require zero S/T. Unknown option bits are rejected.

The only options are:

```text
RADEON3D_DRAW_TEXTURED
RADEON3D_DRAW_BILINEAR
RADEON3D_DRAW_ALPHA_BLEND
RADEON3D_DRAW_DEPTH_LESS
RADEON3D_DRAW_DEPTH_WRITE
```

Interface-v3 sessions may also encode a trusted depth comparison in bits 5-7
with `RADEON3D_DRAW_DEPTH_FUNC()`. The values are `LESS`, `LEQUAL`, `EQUAL`,
`GEQUAL`, `GREATER`, `NOTEQUAL`, `NEVER`, and `ALWAYS`. Zero encodes `LESS`, so
existing interface-v2 records retain their exact behavior. Interface-v2
sessions must leave the comparison bits zero.

Interface-v4 sessions with `RADEON3D_CAP_PHASE5_TEXTURE_STATE` may set
`RADEON3D_DRAW_STATE_V4`. Such a draw uses a 15-dword header: the original 11
dwords followed by texture byte offset, packed texture width/height,
`textureState`, and `fragmentState`. The record retains the same six public
dwords per vertex. The service validates all state enums and backing ranges,
then emits diffuse color before S/T in the trusted textured hardware stream.

`textureState` selects replace/modulate, repeat/clamp independently for S/T,
nearest/linear magnification, all six minification filters, and one through 12
packed POT mip levels. Mip trees use consecutive 32-byte-aligned rows and must
fit completely inside the imported backing surface. `fragmentState` selects an
eight-way alpha test plus reference and validated source/destination blend
factors. Untextured v4 records may use fragment state; clients requiring no
Phase 5 state should continue emitting the original header.

Interface-v5 sessions with `RADEON3D_CAP_PHASE6_FOG_MULTITEX` may set both
`RADEON3D_DRAW_STATE_V4` and `RADEON3D_DRAW_STATE_V5`. Setting v5 without v4 is
invalid. A v5 draw uses a 21-dword header and nine dwords per public vertex:

```text
opcode, 21 + vertexCount * 9
color target handle
depth target handle or zero
texture 0 handle or zero
options
left, top, right-exclusive, bottom-exclusive
vertexCount
texture 0 byte offset
texture 0 packed (width - 1) | ((height - 1) << 16)
texture0State
fragmentState
texture 1 handle or zero
texture 1 byte offset
texture 1 packed (width - 1) | ((height - 1) << 16)
texture1State
phase6State
fog color in low 24-bit RGB

per vertex: X, Y, Z, S0, T0, packed RGBA, S1, T1, fogAmount
```

`phase6State` contains only `RADEON3D_PHASE6_FOG` and
`RADEON3D_PHASE6_TEXTURE1`. Inactive texture-1 header fields and S1/T1 must be
zero. An inactive fog color and every inactive fog amount must be zero. Active
S1/T1 must be finite texture coordinates; active fog amounts must be finite in
`[0,1]`, where zero preserves the fragment and one selects the fog color. The
fog color high byte is always zero.

Texture 1 uses the same validated sampler, environment, mip-level and backing
rules as texture 0, and its complete imported backing must not overlap color or
depth. Distinct texture surfaces must not partially overlap; the same imported
surface may be sampled by both units. The service routes unit 1 to STQ1 and emits stage 1 as
replace or modulation of the stage-0 R0 result. Fog uses discrete per-vertex
fog and the supplied packed fog color. The trusted immediate hardware order is
X/Y, optional Z, optional fog, diffuse color, optional ST0, optional ST1.

Bilinear and alpha blending require texturing. Alpha blending is fixed to
source alpha / one-minus-source-alpha. Depth writes require `DEPTH_LESS` and a
distinct same-size D16 surface. Textures are non-overlapping imported RGB565 or
B8G8R8A8 surfaces, at most 2048x2048, with 32-byte-aligned address and
representable NPOT pitch. Filtering is nearest unless `BILINEAR` is set;
coordinates clamp to the final texel.

Color, depth, and texture roles are checked by complete VRAM address range, not
only by handle identity. The client retains every imported bitmap until
`Radeon3DReleaseSurface()` returns. CPU-written texture data must be completed
before execution; the service drains the final texture byte before submitting
texture fetches.

Interface-v1 sessions cannot call `Radeon3DExecute()` and do not receive its
capability. Fence tokens are accepted only by the session that most recently
received them.

Interface-v5 diagnostic clients may use `Radeon3DInvalidateForTest()` when
`RADEON3D_CAP_TEST_INVALIDATE` is present. It exercises the production recovery
sequence: invalidate existing sessions, reset the engine, reload and test the
CP, then re-arm the service. Existing sessions remain stale after a successful
recovery, so clients must reopen and reimport their resources.
