/*
 * Dual-target R200 command emitter, extracted from radeon3d_service.c.
 * Generates the CP dword stream for Execute/CommitDraw/CommitBatch and
 * CommitStateBatch. Byte-for-byte identical output to the pre-extraction
 * in-service emitter; see MINIGLPPC_130FPS_PLAN.md for the verification
 * protocol. No ExecBase, no locking, no I/O.
 */

#include "radeon_regs.h"

#include "radeon3d_emit.h"

/* Resolve a record's raw handle dword through the caller's table into one
 * of the emitter's rotating descriptor slots. Comparison is by value, so
 * slot identity is irrelevant. */
static struct Radeon3DEmitSurface *ExecuteSurface(
    struct Radeon3DEmitter *emitter, ULONG handle)
{
    struct Radeon3DEmitSurface *slot;

    if (!emitter->Resolve)
        return NULL;
    slot = &emitter->Surfaces[emitter->ResolveSlot++ & 3UL];
    return emitter->Resolve(emitter->ResolveUser, handle, slot);
}

static BOOL SurfaceSame(const struct Radeon3DEmitSurface *x, BOOL xv,
                        const struct Radeon3DEmitSurface *y, BOOL yv)
{
    return xv == yv && (!xv ||
        (x->CpuAddress == y->CpuAddress &&
         x->GpuAddress == y->GpuAddress && x->Pitch == y->Pitch &&
         x->Width == y->Width && x->Height == y->Height &&
         x->Format == y->Format));
}

/* TRUE when two captures differ only in texture-unit fields; reports which
 * units changed so the caller can re-emit just those atoms instead of the
 * full register block. Residency re-imports after in-place texel updates
 * change only the surface handle, and a full re-emit per update made
 * Quake's per-frame lightmap uploads dominate the command stream. */
static BOOL TextureOnlyDelta(const struct Radeon3DEmitState *a,
                             const struct Radeon3DEmitState *b,
                             BOOL *unit0, BOOL *unit1)
{
    ULONG index;
    ULONG light;

    *unit0 = !SurfaceSame(&a->Texture, a->TextureValid,
                          &b->Texture, b->TextureValid) ||
             a->TextureOffset != b->TextureOffset ||
             a->TextureWidth != b->TextureWidth ||
             a->TextureHeight != b->TextureHeight ||
             a->TextureState != b->TextureState ||
             a->TextureBytes != b->TextureBytes;
    *unit1 = !SurfaceSame(&a->Texture1, a->Texture1Valid,
                          &b->Texture1, b->Texture1Valid) ||
             a->Texture1Offset != b->Texture1Offset ||
             a->Texture1Width != b->Texture1Width ||
             a->Texture1Height != b->Texture1Height ||
             a->Texture1State != b->Texture1State ||
             a->Texture1Bytes != b->Texture1Bytes;
    if (!*unit0 && !*unit1)
        return FALSE;
    if (!(SurfaceSame(&a->Color, a->ColorValid, &b->Color, b->ColorValid) &&
          SurfaceSame(&a->Depth, a->DepthValid, &b->Depth, b->DepthValid) &&
           a->Options == b->Options && a->Left == b->Left &&
           a->Top == b->Top && a->Right == b->Right &&
           a->Bottom == b->Bottom && a->ClearDepth == b->ClearDepth &&
           a->FragmentStatePresent == b->FragmentStatePresent &&
           a->ExtendedVertex == b->ExtendedVertex &&
           a->HardwareTcl == b->HardwareTcl &&
           a->FragmentState == b->FragmentState &&
            a->VertexState == b->VertexState &&
            a->FogColor == b->FogColor &&
            a->TransformFlags == b->TransformFlags &&
            a->TexGen == b->TexGen &&
            a->TexGenState[0] == b->TexGenState[0] &&
            a->TexGenState[1] == b->TexGenState[1] &&
            a->NormalVertex == b->NormalVertex &&
            a->Lighting == b->Lighting &&
            a->LightControl == b->LightControl))
        return FALSE;
    if (a->Lighting) {
        for (index = 0; index < 4UL; ++index)
            if (a->GlobalAmbient[index] != b->GlobalAmbient[index] ||
                a->EyeVector[index] != b->EyeVector[index])
                return FALSE;
        for (index = 0; index < RADEON3D_MATERIAL_DWORDS; ++index)
            if (a->Material[index] != b->Material[index])
                return FALSE;
        for (light = 0; light < 8UL; ++light) {
            if (!(a->LightControl &
                  (RADEON3D_LIGHT_CONTROL_ENABLED_MASK << light)))
                continue;
            for (index = 0; index < RADEON3D_EXEC_LIGHT_BLOCK_DWORDS;
                 ++index)
                if (a->Lights[light][index] != b->Lights[light][index])
                    return FALSE;
        }
    }
    for (index = 0; index < 6UL; ++index)
        if (a->Viewport[index] != b->Viewport[index])
            return FALSE;
    return TRUE;
}

static BOOL SameExecuteState(const struct Radeon3DEmitState *a,
                             const struct Radeon3DEmitState *b)
{
    ULONG index;

    if (!(SurfaceSame(&a->Color, a->ColorValid, &b->Color, b->ColorValid) &&
          SurfaceSame(&a->Depth, a->DepthValid, &b->Depth, b->DepthValid) &&
          SurfaceSame(&a->Texture, a->TextureValid,
                      &b->Texture, b->TextureValid) &&
          SurfaceSame(&a->Texture1, a->Texture1Valid,
                      &b->Texture1, b->Texture1Valid) &&
           a->Options == b->Options && a->Left == b->Left &&
           a->Top == b->Top && a->Right == b->Right &&
           a->Bottom == b->Bottom && a->ClearDepth == b->ClearDepth &&
           a->FragmentStatePresent == b->FragmentStatePresent &&
           a->ExtendedVertex == b->ExtendedVertex &&
           a->HardwareTcl == b->HardwareTcl &&
           a->TextureOffset == b->TextureOffset &&
           a->TextureWidth == b->TextureWidth &&
           a->TextureHeight == b->TextureHeight &&
           a->TextureState == b->TextureState &&
           a->FragmentState == b->FragmentState &&
           a->TextureBytes == b->TextureBytes &&
           a->Texture1Offset == b->Texture1Offset &&
           a->Texture1Width == b->Texture1Width &&
           a->Texture1Height == b->Texture1Height &&
           a->Texture1State == b->Texture1State &&
           a->Texture1Bytes == b->Texture1Bytes &&
            a->VertexState == b->VertexState &&
            a->FogColor == b->FogColor &&
            a->TransformFlags == b->TransformFlags &&
            a->TexGen == b->TexGen &&
            a->TexGenState[0] == b->TexGenState[0] &&
            a->TexGenState[1] == b->TexGenState[1] &&
            a->NormalVertex == b->NormalVertex &&
            a->Lighting == b->Lighting &&
            a->LightControl == b->LightControl))
        return FALSE;
    if (a->Lighting) {
        ULONG light;

        for (index = 0; index < 4UL; ++index)
            if (a->GlobalAmbient[index] != b->GlobalAmbient[index] ||
                a->EyeVector[index] != b->EyeVector[index])
                return FALSE;
        for (index = 0; index < RADEON3D_MATERIAL_DWORDS; ++index)
            if (a->Material[index] != b->Material[index])
                return FALSE;
        for (light = 0; light < 8UL; ++light) {
            if (!(a->LightControl &
                  (RADEON3D_LIGHT_CONTROL_ENABLED_MASK << light)))
                continue;
            for (index = 0; index < RADEON3D_EXEC_LIGHT_BLOCK_DWORDS;
                 ++index)
                if (a->Lights[light][index] != b->Lights[light][index])
                    return FALSE;
        }
    }
    /* ModelProjection is deliberately excluded: the matrix upload is cached
     * separately from this register block by EmitExecuteStateCached(). */
    for (index = 0; index < 6UL; ++index)
        if (a->Viewport[index] != b->Viewport[index])
            return FALSE;
    return TRUE;
}

static void ClearExecuteState(struct Radeon3DEmitState *state)
{
    ULONG *words = (ULONG *)state;
    ULONG count = sizeof(*state) / sizeof(*words);
    UBYTE *tail;

    while (count >= 8UL) {
        words[0] = 0;
        words[1] = 0;
        words[2] = 0;
        words[3] = 0;
        words[4] = 0;
        words[5] = 0;
        words[6] = 0;
        words[7] = 0;
        words += 8;
        count -= 8UL;
    }
    while (count--)
        *words++ = 0;

    tail = (UBYTE *)words;
    count = sizeof(*state) % sizeof(*words);
    while (count--)
        *tail++ = 0;
}

static BOOL ExecuteEmitWord(struct Radeon3DEmitter *emitter,
                            ULONG value)
{
    if (emitter->Count >= RADEON3D_MAX_BATCH_DWORDS)
        return FALSE;
    emitter->Words[emitter->Count++] = value;
    return TRUE;
}

static BOOL ExecuteEmitRegister(struct Radeon3DEmitter *emitter,
                                ULONG reg, ULONG value)
{
    return ExecuteEmitWord(emitter, RADEON_CP_PACKET0(reg, 0)) &&
           ExecuteEmitWord(emitter, value);
}

static BOOL ExecuteEmitMatrix(struct Radeon3DEmitter *emitter,
                               const ULONG *matrix, ULONG vectorAddress,
                               BOOL transpose)
{
    ULONG row, column;

    if (!ExecuteEmitRegister(emitter, R200_SE_TCL_STATE_FLUSH, 0) ||
        !ExecuteEmitRegister(emitter, R200_SE_TCL_VECTOR_INDX_REG,
                              (1UL << R200_VEC_INDX_OCTWORD_STRIDE_SHIFT) |
                                  vectorAddress) ||
        !ExecuteEmitWord(emitter,
                         RADEON_CP_PACKET0_ONE(R200_SE_TCL_VECTOR_DATA_REG,
                                               15UL)))
        return FALSE;
    for (row = 0; row < 4UL; ++row)
        for (column = 0; column < 4UL; ++column)
            if (!ExecuteEmitWord(emitter,
                                 matrix[transpose ? column * 4UL + row :
                                                    row * 4UL + column]))
                return FALSE;
    return TRUE;
}

/* Vector-memory blocks are strided: each dword lands stride octword slots
 * after its predecessor, matching Mesa's cmdvec() encoding. */
static BOOL ExecuteEmitVectorBlock(struct Radeon3DEmitter *emitter,
                                   ULONG address, ULONG stride,
                                   const ULONG *data, ULONG count)
{
    ULONG index;

    if (!ExecuteEmitRegister(emitter, R200_SE_TCL_STATE_FLUSH, 0) ||
        !ExecuteEmitRegister(emitter, R200_SE_TCL_VECTOR_INDX_REG,
                              (stride <<
                               R200_VEC_INDX_OCTWORD_STRIDE_SHIFT) |
                                  address) ||
        !ExecuteEmitWord(emitter,
                         RADEON_CP_PACKET0_ONE(R200_SE_TCL_VECTOR_DATA_REG,
                                               count - 1UL)))
        return FALSE;
    for (index = 0; index < count; ++index)
        if (!ExecuteEmitWord(emitter, data[index]))
            return FALSE;
    return TRUE;
}

static BOOL ExecuteEmitScalarBlock(struct Radeon3DEmitter *emitter,
                                   ULONG address, ULONG stride,
                                   const ULONG *data, ULONG count)
{
    ULONG index;

    if (!ExecuteEmitRegister(emitter, R200_SE_TCL_SCALAR_INDX_REG,
                             address | (stride <<
                                        R200_SCAL_INDX_DWORD_STRIDE_SHIFT)) ||
        !ExecuteEmitWord(emitter,
                         RADEON_CP_PACKET0_ONE(R200_SE_TCL_SCALAR_DATA_REG,
                                               count - 1UL)))
        return FALSE;
    for (index = 0; index < count; ++index)
        if (!ExecuteEmitWord(emitter, data[index]))
            return FALSE;
    return TRUE;
}

static BOOL ExecuteEmitGuardClipState(struct Radeon3DEmitter *emitter)
{
    ULONG index;

    if (!ExecuteEmitRegister(emitter, R200_SE_TCL_SCALAR_INDX_REG,
                             R200_SS_VERT_GUARD_CLIP_ADJ_ADDR |
                                 (1UL <<
                                  R200_SCAL_INDX_DWORD_STRIDE_SHIFT)) ||
        !ExecuteEmitWord(emitter,
                         RADEON_CP_PACKET0_ONE(R200_SE_TCL_SCALAR_DATA_REG,
                                               3UL)))
        return FALSE;
    for (index = 0; index < 4UL; ++index)
        if (!ExecuteEmitWord(emitter, 0x3f800000UL))
            return FALSE;
    return TRUE;
}

static BOOL ValidUnitFloat(ULONG bits)
{
    return !(bits & 0x80000000UL) && bits <= 0x3f800000UL;
}

static BOOL ValidTextureCoordinate(ULONG bits)
{
    return (bits & 0x7f800000UL) != 0x7f800000UL &&
           (bits & 0x7fffffffUL) <= 0x47000000UL;
}
static ULONG SurfaceBytesPerPixel(const struct Radeon3DEmitSurface *surface)
{
    if (!surface)
        return 0;
    if (surface->Format == RADEON3D_FORMAT_CLUT8)
        return 1UL;
    if (surface->Format == RADEON3D_FORMAT_R5G6B5PC)
        return 2UL;
    if (surface->Format == RADEON3D_FORMAT_B8G8R8A8)
        return 4UL;
    return 0;
}

static ULONG ColorTargetControl(const struct Radeon3DEmitSurface *surface)
{
    if (surface->Format == RADEON3D_FORMAT_CLUT8)
        return R200_COLOR_FORMAT_RGB332 | R200_DITHER_ENABLE;
    if (surface->Format == RADEON3D_FORMAT_B8G8R8A8)
        return R200_COLOR_FORMAT_ARGB8888;
    return R200_COLOR_FORMAT_RGB565;
}

static BOOL ValidColorTarget(const struct Radeon3DEmitter *emitter,
                             const struct Radeon3DEmitSurface *surface)
{
    ULONG bytesPerPixel = SurfaceBytesPerPixel(surface);

    return surface && bytesPerPixel &&
           (surface->Format == RADEON3D_FORMAT_R5G6B5PC ||
            emitter->InterfaceVersion >= 6UL) &&
           !(surface->GpuAddress & ~R200_COLOROFFSET_MASK) &&
           surface->Width && surface->Width <= 65536UL &&
           surface->Height && surface->Height <= 65536UL &&
           !(surface->Pitch % bytesPerPixel) &&
           !((surface->Pitch / bytesPerPixel) & ~R200_COLORPITCH_MASK);
}

static BOOL ExecuteSurfacesOverlap(struct Radeon3DEmitSurface *first,
                                   struct Radeon3DEmitSurface *second)
{
    ULONG firstBytesPerPixel;
    ULONG secondBytesPerPixel;
    ULONG firstBytes;
    ULONG secondBytes;

    if (!first || !second)
        return FALSE;
    firstBytesPerPixel = SurfaceBytesPerPixel(first);
    secondBytesPerPixel = SurfaceBytesPerPixel(second);
    if (!firstBytesPerPixel || !secondBytesPerPixel)
        return TRUE;
    firstBytes = (first->Height - 1UL) * first->Pitch +
                 first->Width * firstBytesPerPixel;
    secondBytes = (second->Height - 1UL) * second->Pitch +
                  second->Width * secondBytesPerPixel;
    if (first->GpuAddress <= second->GpuAddress)
        return second->GpuAddress - first->GpuAddress < firstBytes;
    return first->GpuAddress - second->GpuAddress < secondBytes;
}

static BOOL ValidDepthTarget(struct Radeon3DEmitSurface *surface,
                             struct Radeon3DEmitSurface *color)
{
    /* Width may exceed the color surface: minigl.library pads the depth
     * storage so its pitch is a whole number of 64-pixel tile pairs (the
     * R200 drops whole 32-pixel tile columns at widths such as 800, whose
     * 1600-byte pitch is 12.5 tile pairs).  Rendering and clearing are
     * bounded by RE_WIDTH_HEIGHT, so the extra columns are never touched.
     * 64 pixels of 16-bit depth is 128 bytes; the same alignment the Linux
     * radeon DDX applies to every tiled surface. */
    return surface && !ExecuteSurfacesOverlap(surface, color) &&
           surface->Format == RADEON3D_FORMAT_R5G6B5PC &&
           surface->Width >= color->Width &&
           surface->Height == color->Height &&
           !(surface->GpuAddress & 0x0fUL) &&
           !(surface->Pitch & 127UL) &&
           !((surface->Pitch / 2UL) & ~R200_DEPTHPITCH_MASK);
}

static BOOL ValidTextureTarget(struct Radeon3DEmitSurface *surface,
                               struct Radeon3DEmitSurface *color,
                               struct Radeon3DEmitSurface *depth)
{
    return surface && !ExecuteSurfacesOverlap(surface, color) &&
           !ExecuteSurfacesOverlap(surface, depth) &&
           (surface->Format == RADEON3D_FORMAT_R5G6B5PC ||
            surface->Format == RADEON3D_FORMAT_B8G8R8A8) &&
           surface->Width && surface->Width <= 2048UL &&
           surface->Height && surface->Height <= 2048UL &&
           !(surface->GpuAddress & ~R200_TXO_OFFSET_MASK) &&
           surface->Pitch >= 32UL && !(surface->Pitch & 31UL) &&
           !((surface->Pitch - 32UL) & ~R200_TXPITCH_MASK);
}

static BOOL IsPowerOfTwo(ULONG value)
{
    return value && !(value & (value - 1UL));
}

/* Diagnostic companion to ValidTextureTargetWithState(): reports which of
 * its checks rejects a texture, for the commit-failure stage report. */
static ULONG TextureRejectReason(struct Radeon3DEmitSurface *surface,
                                 struct Radeon3DEmitSurface *color,
                                 struct Radeon3DEmitSurface *depth,
                                 ULONG offset, ULONG width, ULONG height,
                                 ULONG levels)
{
    if (!surface || !color)
        return 80UL;
    if (ExecuteSurfacesOverlap(surface, color))
        return 81UL;
    if (depth && ExecuteSurfacesOverlap(surface, depth))
        return 82UL;
    if (surface->Format != RADEON3D_FORMAT_R5G6B5PC &&
        surface->Format != RADEON3D_FORMAT_B8G8R8A8)
        return 83UL;
    if (!width || width > 2048UL || !height || height > 2048UL)
        return 84UL;
    if (!levels || levels > 12UL)
        return 85UL;
    if (offset > 0xffffffffUL - surface->GpuAddress ||
        ((surface->GpuAddress + offset) & ~R200_TXO_OFFSET_MASK))
        return 86UL;
    if (levels > 1UL && (!IsPowerOfTwo(width) || !IsPowerOfTwo(height)))
        return 87UL;
    return 88UL;   /* allocation/mip-layout bounds */
}

static BOOL ValidTextureTargetWithState(struct Radeon3DEmitSurface *surface,
                                 struct Radeon3DEmitSurface *color,
                                 struct Radeon3DEmitSurface *depth,
                                 ULONG offset, ULONG width, ULONG height,
                                 ULONG levels, ULONG *usedBytes)
{
    ULONG bytesPerPixel, allocationBytes, needed = 0;
    ULONG levelWidth = width, levelHeight = height, level;

    if (!surface || !color || ExecuteSurfacesOverlap(surface, color) ||
        (depth && ExecuteSurfacesOverlap(surface, depth)) ||
        (surface->Format != RADEON3D_FORMAT_R5G6B5PC &&
         surface->Format != RADEON3D_FORMAT_B8G8R8A8) ||
        !width || width > 2048UL || !height || height > 2048UL ||
         !levels || levels > 12UL ||
        offset > 0xffffffffUL - surface->GpuAddress ||
        (surface->GpuAddress + offset) & ~R200_TXO_OFFSET_MASK ||
        surface->Height > 0xffffffffUL / surface->Pitch)
        return FALSE;
    bytesPerPixel = surface->Format == RADEON3D_FORMAT_B8G8R8A8 ? 4UL : 2UL;
    allocationBytes = (surface->Height - 1UL) * surface->Pitch +
                      surface->Width * bytesPerPixel;
    if (offset >= allocationBytes)
        return FALSE;
    if (levels == 1UL) {
        if (width > surface->Pitch / bytesPerPixel ||
            width * bytesPerPixel > allocationBytes - offset ||
            height - 1UL > (allocationBytes - offset - width * bytesPerPixel) /
                               surface->Pitch)
            return FALSE;
        needed = (height - 1UL) * surface->Pitch + width * bytesPerPixel;
    } else {
        if (!IsPowerOfTwo(width) || !IsPowerOfTwo(height))
            return FALSE;
        for (level = 0; level < levels; ++level) {
            ULONG rowBytes = levelWidth * bytesPerPixel;
            ULONG stride = (rowBytes + 31UL) & ~31UL;
            ULONG levelBytes;
            if (levelHeight > 0xffffffffUL / stride)
                return FALSE;
            levelBytes = stride * levelHeight;
            if (needed > allocationBytes - offset ||
                levelBytes > allocationBytes - offset - needed)
                return FALSE;
            needed += levelBytes;
            if (levelWidth > 1UL) levelWidth >>= 1;
            if (levelHeight > 1UL) levelHeight >>= 1;
        }
    }
    *usedBytes = needed;
    return needed && needed <= allocationBytes - offset;
}

static BOOL UsesPowerOfTwoTexturePath(ULONG width, ULONG height, ULONG state)
{
    (void)state;
    return IsPowerOfTwo(width) && IsPowerOfTwo(height);
}
static BOOL EmitExecuteTexture(struct Radeon3DEmitter *emitter,
                               const struct Radeon3DEmitSurface *texture,
                               ULONG unit, ULONG options,
                               BOOL fragmentStatePresent,
                               ULONG textureOffset, ULONG textureWidth,
                               ULONG textureHeight, ULONG textureState,
                                ULONG textureBytes, BOOL perspective,
                                BOOL projected)
{
    ULONG filterReg = unit ? R200_PP_TXFILTER_1 : R200_PP_TXFILTER_0;
    ULONG formatReg = unit ? R200_PP_TXFORMAT_1 : R200_PP_TXFORMAT_0;
    ULONG formatXReg = unit ? R200_PP_TXFORMAT_X_1 : R200_PP_TXFORMAT_X_0;
    ULONG sizeReg = unit ? R200_PP_TXSIZE_1 : R200_PP_TXSIZE_0;
    ULONG pitchReg = unit ? R200_PP_TXPITCH_1 : R200_PP_TXPITCH_0;
    ULONG multiReg = unit ? R200_PP_TXMULTI_CTL_1 : R200_PP_TXMULTI_CTL_0;
    ULONG offsetReg = unit ? R200_PP_TXOFFSET_1 : R200_PP_TXOFFSET_0;
    ULONG textureFormat = R200_TXFORMAT_NON_POWER2;
    ULONG filter = 0;
    ULONG textureSize = (texture->Width - 1UL) |
                        ((texture->Height - 1UL) << 16);
    ULONG texturePitch = texture->Pitch - 32UL;
    ULONG textureAddress = texture->GpuAddress;
    volatile UBYTE *textureEnd;

    (void)perspective;

    if (fragmentStatePresent) {
        static const ULONG minFilters[6] = {
            0, R200_MIN_FILTER_LINEAR,
            R200_MIN_FILTER_NEAREST_MIP_NEAREST,
            R200_MIN_FILTER_LINEAR_MIP_NEAREST,
            R200_MIN_FILTER_NEAREST_MIP_LINEAR,
            R200_MIN_FILTER_LINEAR_MIP_LINEAR
        };
        ULONG minFilter = (textureState & RADEON3D_TEX_MIN_MASK) >>
                          RADEON3D_TEX_MIN_SHIFT;
        ULONG levels = ((textureState & RADEON3D_TEX_LEVELS_MASK) >>
                        RADEON3D_TEX_LEVELS_SHIFT) + 1UL;
        ULONG logWidth = 0, logHeight = 0, scan;

        filter = minFilters[minFilter];
        if (textureState & RADEON3D_TEX_MAG_LINEAR)
            filter |= R200_MAG_FILTER_LINEAR;
        /* Mesa maps GL_REPEAT to the zero-valued CLAMP_*_WRAP field.
         * WRAPEN_* is the separate D3D/cylindrical interpolation facility. */
        if (!(textureState & RADEON3D_TEX_REPEAT_S))
            filter |= R200_CLAMP_S_CLAMP_LAST;
        if (!(textureState & RADEON3D_TEX_REPEAT_T))
            filter |= R200_CLAMP_T_CLAMP_LAST;
        textureAddress += textureOffset;
        textureSize = (textureWidth - 1UL) |
                      ((textureHeight - 1UL) << 16);
        /* Power-of-two layout is independent of whether the current min
         * filter actually samples mip levels. The NPOT path always clamps on
         * R200, so treating a POT texture with GL_LINEAR as NPOT breaks
         * GL_REPEAT (notably Quake world textures). */
        if (UsesPowerOfTwoTexturePath(textureWidth, textureHeight,
                                     textureState)) {
            for (scan = textureWidth; scan > 1UL; scan >>= 1) ++logWidth;
            for (scan = textureHeight; scan > 1UL; scan >>= 1) ++logHeight;
            textureFormat = (logWidth << R200_TXFORMAT_WIDTH_SHIFT) |
                            (logHeight << R200_TXFORMAT_HEIGHT_SHIFT);
            if (levels > 1UL)
                filter |= (levels - 1UL) << R200_MAX_MIP_LEVEL_SHIFT;
            textureSize = 0;
            texturePitch = 0;
        }
    }
    if (unit)
        textureFormat |= R200_TXFORMAT_ST_ROUTE_STQ1;
    textureEnd = (volatile UBYTE *)texture->CpuAddress + textureOffset +
                 textureBytes - 1UL;
    /* Drain posted CPU texture writes before the CP reads this allocation. */
    (void)*textureEnd;
    if (texture->Format == RADEON3D_FORMAT_R5G6B5PC)
        textureFormat |= R200_TXFORMAT_RGB565;
    else
        textureFormat |= R200_TXFORMAT_ARGB8888 | R200_TXFORMAT_ALPHA_IN_MAP;
    if (options & RADEON3D_DRAW_BILINEAR)
        filter |= R200_MAG_FILTER_LINEAR | R200_MIN_FILTER_LINEAR;
    return ExecuteEmitRegister(emitter, filterReg, filter) &&
           ExecuteEmitRegister(emitter, formatReg, textureFormat) &&
            ExecuteEmitRegister(emitter, formatXReg,
                                projected ? R200_TEXCOORD_PROJ : 0) &&
           ExecuteEmitRegister(emitter, sizeReg, textureSize) &&
           ExecuteEmitRegister(emitter, pitchReg, texturePitch) &&
           ExecuteEmitRegister(emitter, multiReg, 0) &&
           ExecuteEmitRegister(emitter, offsetReg, textureAddress);
}
static BOOL ValidExecuteScissor(struct Radeon3DEmitSurface *target,
                                ULONG left, ULONG top,
                                ULONG right, ULONG bottom)
{
    return left < right && top < bottom &&
           right <= target->Width && bottom <= target->Height;
}
static BOOL EmitExecuteState(struct Radeon3DEmitter *emitter,
                             const struct Radeon3DEmitState *state)
{
    const struct Radeon3DEmitSurface *color =
        state->ColorValid ? &state->Color : NULL;
    const struct Radeon3DEmitSurface *depth =
        state->DepthValid ? &state->Depth : NULL;
    const struct Radeon3DEmitSurface *texture =
        state->TextureValid ? &state->Texture : NULL;
    const struct Radeon3DEmitSurface *texture1 =
        state->Texture1Valid ? &state->Texture1 : NULL;
    ULONG options = state->Options;
    ULONG left = state->Left;
    ULONG top = state->Top;
    ULONG right = state->Right;
    ULONG bottom = state->Bottom;
    BOOL clearDepth = state->ClearDepth;
    BOOL fragmentStatePresent = state->FragmentStatePresent;
    BOOL extendedVertex = state->ExtendedVertex;
    BOOL hardwareTcl = state->HardwareTcl;
    ULONG textureOffset = state->TextureOffset;
    ULONG textureWidth = state->TextureWidth;
    ULONG textureHeight = state->TextureHeight;
    ULONG textureState = state->TextureState;
    ULONG fragmentState = state->FragmentState;
    ULONG textureBytes = state->TextureBytes;
    ULONG texture1Offset = state->Texture1Offset;
    ULONG texture1Width = state->Texture1Width;
    ULONG texture1Height = state->Texture1Height;
    ULONG texture1State = state->Texture1State;
    ULONG texture1Bytes = state->Texture1Bytes;
    ULONG vertexState = state->VertexState;
    ULONG fogColor = state->FogColor;
    ULONG seControl = R200_BFACE_SOLID | R200_FFACE_SOLID |
                      R200_FLAT_SHADE_VTX_LAST |
                      R200_DIFFUSE_SHADE_GOURAUD |
                      R200_ALPHA_SHADE_GOURAUD |
                      R200_VTX_PIX_CENTER_OGL |
                      R200_ROUND_MODE_ROUND |
                      R200_ROUND_PREC_4TH_PIX;
    ULONG format0 = R200_VTX_PK_RGBA << R200_VTX_COLOR_0_SHIFT;
    ULONG format1 = 0;
    ULONG ppControl = R200_TEX_BLEND_0_ENABLE;
    ULONG rbControl = ColorTargetControl(color);
    ULONG blendControl = R200_SRC_BLEND_GL_ONE | R200_DST_BLEND_GL_ZERO;
    ULONG ppMisc = R200_ALPHA_TEST_ALWAYS;
    ULONG zControl = R200_DEPTH_FORMAT_16BIT_INT_Z |
                     R200_STENCIL_TEST_ALWAYS;
    BOOL textured = (options & RADEON3D_DRAW_TEXTURED) != 0;
    BOOL textured1 = extendedVertex &&
                      (vertexState & RADEON3D_VERTEX_TEXTURE1) != 0;
    BOOL texGen = state->TexGen;
    BOOL texGen0 = texGen && state->TexGenState[0] !=
                   RADEON3D_TEXGEN_MODE_OFF;
    BOOL texGen1 = texGen && state->TexGenState[1] !=
                    RADEON3D_TEXGEN_MODE_OFF;
    BOOL sphereMap0 = texGen0 &&
        (state->TexGenState[0] & RADEON3D_TEXGEN_MODE_MASK) ==
            RADEON3D_TEXGEN_MODE_SPHERE_MAP;
    BOOL sphereMap1 = texGen1 &&
        (state->TexGenState[1] & RADEON3D_TEXGEN_MODE_MASK) ==
            RADEON3D_TEXGEN_MODE_SPHERE_MAP;
    BOOL normalVertex = state->NormalVertex;
    BOOL lighting = state->Lighting;
    BOOL fog = extendedVertex &&
               (vertexState & RADEON3D_VERTEX_FOG) != 0;
    BOOL perspective = hardwareTcl || (extendedVertex &&
                       (vertexState & RADEON3D_VERTEX_CLIP_COORDINATES) != 0);
    /* The semantic ABI always carries normalized S/T. ST_DENORMALIZED is for
     * rectangle/texel-space coordinates and must not follow POT layout. */
    BOOL denormalized = FALSE;
    BOOL useDepth = depth != NULL;
    ULONG depthFunc = (options & RADEON3D_DRAW_DEPTH_FUNC_MASK) >>
                      RADEON3D_DRAW_DEPTH_FUNC_SHIFT;
    ULONG outputFormat0 = R200_VTX_Z0 | R200_VTX_W0 |
                          (R200_VTX_FP_RGBA << R200_VTX_COLOR_0_SHIFT);
    ULONG outputFormat1 = 0;
    ULONG outputSelect = R200_OUTPUT_XYZW;
    ULONG tclControl = R200_UCP_IN_CLIP_SPACE;
    ULONG texProcControl0 = 0;
    ULONG texProcControl1 = 0x00543210UL;
    ULONG texProcControl2 = 0x00ffffffUL;
    ULONG perLightCtl[4] = {0, 0, 0, 0};
    ULONG lightModelCtl0 = R200_SPECULAR_LIGHTS |
                           R200_DIFFUSE_SPECULAR_COMBINE |
                           R200_LOCAL_LIGHT_VEC_GL;

    /* Texels rewritten in place stay invisible until the host read path is
     * invalidated: the texture cache keeps serving the old lines for the
     * surface address, and no texture-register rewrite evicts them. The
     * client marks in-place updates by changing the content serial carried
     * in the texture state; flush the HDP read buffer before the atom so
     * the next sampling draw re-reads VRAM. */
    if (((textureState ^ emitter->State.TextureState) |
         (texture1State ^ emitter->State.Texture1State)) &
        RADEON3D_TEX_CONTENT_MASK &&
        (!ExecuteEmitRegister(emitter, RADEON_HOST_PATH_CNTL,
                              RADEON_HDP_READ_BUFFER_INVALIDATE) ||
         !ExecuteEmitRegister(emitter, RADEON_HOST_PATH_CNTL, 0UL)))
        return FALSE;
    if (normalVertex)
        format0 |= R200_VTX_N0;
    if (sphereMap0 || sphereMap1)
        lightModelCtl0 |= R200_LOCAL_VIEWER | R200_NORMALIZE_NORMALS;
    if (lighting) {
        ULONG light;

        lightModelCtl0 |= R200_LIGHTING_ENABLE | R200_NORMALIZE_NORMALS;
        if (state->LightControl & RADEON3D_LIGHT_LOCAL_VIEWER)
            lightModelCtl0 |= R200_LOCAL_VIEWER;
        for (light = 0; light < 8UL; ++light) {
            ULONG shift = (light & 1UL) ? R200_LIGHT_1_SHIFT : 0UL;
            const ULONG *block = state->Lights[light];

            if (!(state->LightControl &
                  (RADEON3D_LIGHT_CONTROL_ENABLED_MASK << light)))
                continue;
            perLightCtl[light >> 1] |= (R200_LIGHT_ENABLE |
                                        R200_LIGHT_ENABLE_AMBIENT |
                                        R200_LIGHT_ENABLE_SPECULAR) <<
                                       shift;
            /* position W is a raw float dword; zero means directional */
            if (block[15])
                perLightCtl[light >> 1] |= R200_LIGHT_IS_LOCAL << shift;
            if (state->LightControl &
                (1UL << (RADEON3D_LIGHT_SPOT_SHIFT + light)))
                perLightCtl[light >> 1] |= R200_LIGHT_IS_SPOT << shift;
            if (state->LightControl &
                (1UL << (RADEON3D_LIGHT_ATTEN_SHIFT + light))) {
                perLightCtl[light >> 1] |=
                    R200_LIGHT_ENABLE_RANGE_ATTEN << shift;
                if (!block[20] && !block[21])
                    perLightCtl[light >> 1] |=
                        R200_LIGHT_CONSTANT_RANGE_ATTEN << shift;
            }
        }
    }

    if (useDepth || perspective)
        format0 |= R200_VTX_Z0;
    if (perspective)
        format0 |= R200_VTX_W0;
    if (textured) {
        format0 &= ~(3UL << R200_VTX_COLOR_0_SHIFT);
        if (fragmentStatePresent)
            format0 |= R200_VTX_PK_RGBA << R200_VTX_COLOR_0_SHIFT;
        format1 = 2UL << R200_VTX_TEX0_COMP_CNT_SHIFT;
        outputFormat1 |= 2UL << R200_VTX_TEX0_COMP_CNT_SHIFT;
        if (hardwareTcl && texGen0)
            outputSelect |= R200_OUTPUT_TEX_0;
        if (texGen0) {
            outputFormat1 &= ~(7UL << R200_VTX_TEX0_COMP_CNT_SHIFT);
            outputFormat1 |= 4UL << R200_VTX_TEX0_COMP_CNT_SHIFT;
            texProcControl0 |= R200_TEXGEN_TEXMAT_0_ENABLE |
                               R200_TEXMAT_0_ENABLE;
            texProcControl1 &= ~(R200_TEXGEN_INPUT_MASK << 0);
            texProcControl1 |= (sphereMap0 ? R200_TEXGEN_INPUT_SPHERE :
                                             R200_TEXGEN_INPUT_OBJ) << 0;
            texProcControl2 &= ~((state->TexGenState[0] &
                                  RADEON3D_TEXGEN_COMPONENTS) >> 4);
        }
        ppControl |= R200_TEX_0_ENABLE;
    }
    if (textured1) {
        format1 |= 2UL << R200_VTX_TEX1_COMP_CNT_SHIFT;
        outputFormat1 |= 2UL << R200_VTX_TEX1_COMP_CNT_SHIFT;
        if (hardwareTcl && texGen1)
            outputSelect |= R200_OUTPUT_TEX_1;
        if (texGen1) {
            outputFormat1 &= ~(7UL << R200_VTX_TEX1_COMP_CNT_SHIFT);
            outputFormat1 |= 4UL << R200_VTX_TEX1_COMP_CNT_SHIFT;
            texProcControl0 |= R200_TEXGEN_TEXMAT_1_ENABLE |
                               R200_TEXMAT_1_ENABLE;
            texProcControl1 &= ~(R200_TEXGEN_INPUT_MASK << 4);
            texProcControl1 |= (sphereMap1 ? R200_TEXGEN_INPUT_SPHERE :
                                             R200_TEXGEN_INPUT_OBJ) << 4;
            texProcControl2 &= ~((state->TexGenState[1] &
                                  RADEON3D_TEXGEN_COMPONENTS) >> 0);
        }
        ppControl |= R200_TEX_1_ENABLE | R200_TEX_BLEND_1_ENABLE;
    }
    if (fog) {
        format0 |= R200_VTX_DISCRETE_FOG;
        ppControl |= R200_FOG_ENABLE;
        seControl |= R200_FOG_SHADE_GOURAUD |
                      R200_DISC_FOG_SHADE_GOURAUD;
        /* Mesa's fixed-function R200 path always exports secondary colour
         * while fog is active.  RV280's fog interpolator requires that
         * companion output even when VTX_FOG selects the discrete scalar. */
        outputFormat0 |= R200_VTX_DISCRETE_FOG |
                         (R200_VTX_FP_RGBA << R200_VTX_COLOR_1_SHIFT);
        /* OUTPUT_DISCRETE_FOG is a programmable-vertex-output selector.
         * Fixed TCL exports fog from VTX_FMT; selecting it here reads an
         * unwritten result and collapses the blend factor to zero. */
        outputSelect |= R200_OUTPUT_COLOR_1;
    }
    /* Hardware TCL keeps the base PK_RGBA colour format: the record carries
     * the same packed ARGB dword as the non-TCL paths, and the TCL unit
     * accepts packed colour input. */
    if (useDepth) {
        rbControl |= R200_Z_ENABLE;
        if (clearDepth)
            zControl |= R200_Z_TEST_ALWAYS;
        else {
            static const ULONG depthTests[8] = {
                R200_Z_TEST_LESS, R200_Z_TEST_LEQUAL, R200_Z_TEST_EQUAL,
                R200_Z_TEST_GEQUAL, R200_Z_TEST_GREATER,
                R200_Z_TEST_NOTEQUAL, R200_Z_TEST_NEVER,
                R200_Z_TEST_ALWAYS
            };
            zControl |= depthTests[depthFunc];
        }
        if (clearDepth || (options & RADEON3D_DRAW_DEPTH_WRITE))
            zControl |= R200_Z_WRITE_ENABLE;
    }
    if (fragmentStatePresent &&
        (fragmentState & RADEON3D_FRAGMENT_ALPHA_TEST)) {
        static const ULONG alphaTests[8] = {
            R200_ALPHA_TEST_LESS, R200_ALPHA_TEST_LEQUAL,
            R200_ALPHA_TEST_EQUAL, R200_ALPHA_TEST_GEQUAL,
            R200_ALPHA_TEST_GREATER, R200_ALPHA_TEST_NOTEQUAL,
            R200_ALPHA_TEST_NEVER, R200_ALPHA_TEST_ALWAYS
        };
        ULONG alphaFunction = (fragmentState &
                               RADEON3D_FRAGMENT_ALPHA_FUNC_MASK) >>
                              RADEON3D_FRAGMENT_ALPHA_FUNC_SHIFT;
        ppControl |= R200_ALPHA_TEST_ENABLE;
        ppMisc = alphaTests[alphaFunction] |
                 ((fragmentState & RADEON3D_FRAGMENT_ALPHA_REF_MASK) >>
                  RADEON3D_FRAGMENT_ALPHA_REF_SHIFT);
    }
    if (fragmentStatePresent &&
        (fragmentState & RADEON3D_FRAGMENT_BLEND)) {
        static const ULONG sourceBlend[11] = {
            R200_SRC_BLEND_GL_ZERO, R200_SRC_BLEND_GL_ONE,
            R200_SRC_BLEND_GL_SRC_COLOR,
            R200_SRC_BLEND_GL_ONE_MINUS_SRC_COLOR,
            R200_SRC_BLEND_GL_DST_COLOR,
            R200_SRC_BLEND_GL_ONE_MINUS_DST_COLOR,
            R200_SRC_BLEND_GL_SRC_ALPHA,
            (39UL << 16), R200_SRC_BLEND_GL_DST_ALPHA,
            R200_SRC_BLEND_GL_ONE_MINUS_DST_ALPHA,
            R200_SRC_BLEND_GL_SRC_ALPHA_SATURATE
        };
        static const ULONG destinationBlend[10] = {
            R200_DST_BLEND_GL_ZERO, R200_DST_BLEND_GL_ONE,
            R200_DST_BLEND_GL_SRC_COLOR,
            R200_DST_BLEND_GL_ONE_MINUS_SRC_COLOR,
            R200_DST_BLEND_GL_DST_COLOR,
            R200_DST_BLEND_GL_ONE_MINUS_DST_COLOR,
            R200_DST_BLEND_GL_SRC_ALPHA,
            R200_DST_BLEND_GL_ONE_MINUS_SRC_ALPHA,
            R200_DST_BLEND_GL_DST_ALPHA,
            R200_DST_BLEND_GL_ONE_MINUS_DST_ALPHA
        };
        ULONG source = (fragmentState & RADEON3D_FRAGMENT_SRC_MASK) >>
                       RADEON3D_FRAGMENT_SRC_SHIFT;
        ULONG destination = (fragmentState & RADEON3D_FRAGMENT_DST_MASK) >>
                            RADEON3D_FRAGMENT_DST_SHIFT;
        rbControl |= R200_ALPHA_BLEND_ENABLE;
        blendControl = sourceBlend[source] | destinationBlend[destination];
    } else if (options & RADEON3D_DRAW_ALPHA_BLEND) {
        rbControl |= R200_ALPHA_BLEND_ENABLE;
        blendControl = R200_SRC_BLEND_GL_SRC_ALPHA |
                       R200_DST_BLEND_GL_ONE_MINUS_SRC_ALPHA;
    }

    if (hardwareTcl) {
        ULONG pointSize = (state->TransformFlags &
                           RADEON3D_TRANSFORM_POINT_SIZE_MASK) >>
                          RADEON3D_TRANSFORM_POINT_SIZE_SHIFT;
        if (state->TransformFlags & RADEON3D_TRANSFORM_FLAT_SHADE) {
            /* GL flat shade is SE_CNTL field value 1; clearing GOURAUD alone
             * selects 0 = SOLID, which samples the unprogrammed solid-colour
             * path and draws nothing visible. */
            seControl &= ~R200_DIFFUSE_SHADE_MASK;
            seControl |= R200_DIFFUSE_SHADE_FLAT;
        }
        if (state->TransformFlags & RADEON3D_TRANSFORM_POLYGON_LINE) {
            seControl &= ~(R200_BFACE_SOLID | R200_FFACE_SOLID);
            seControl |= R200_BFACE_LINE | R200_FFACE_LINE;
        } else if (state->TransformFlags &
                   RADEON3D_TRANSFORM_POLYGON_POINT) {
            seControl &= ~(R200_BFACE_SOLID | R200_FFACE_SOLID);
            seControl |= R200_BFACE_POINT | R200_FFACE_POINT;
        }
        if (state->TransformFlags & RADEON3D_TRANSFORM_FRONT_CCW) {
            seControl |= R200_FFACE_CULL_CCW;
            tclControl |= R200_CULL_FRONT_IS_CCW;
        }
        if (state->TransformFlags & RADEON3D_TRANSFORM_CULL_FRONT)
            tclControl |= R200_CULL_FRONT;
        if (state->TransformFlags & RADEON3D_TRANSFORM_CULL_BACK)
            tclControl |= R200_CULL_BACK;
        if (!ExecuteEmitRegister(emitter, R200_RE_POINTSIZE,
                                 pointSize | (1024UL << 16)) ||
            !ExecuteEmitRegister(emitter, R200_SE_LINE_WIDTH, 16UL))
            return FALSE;
        /* RE_POINTSIZE is programmed above for completeness, but the hardware
         * ignores it for point primitives under TCL: measured on RV280, every
         * point rasterises one pixel whatever this register, the point-sprite
         * vport scale, or SE_TCL_POINT_SPRITE_CNTL contain. minigl.library
         * sizes points itself instead (EmitSizedPoints), so nothing further is
         * emitted here. See GLITCH_HUNT_FINDINGS.md. */
    }
    if (perspective &&
        (!ExecuteEmitRegister(emitter,R200_SE_VPORT_XSCALE,
                              hardwareTcl ? state->Viewport[0] :
                                  UnsignedHalfFloatBits(color->Width)) ||
         !ExecuteEmitRegister(emitter,R200_SE_VPORT_XOFFSET,
                              hardwareTcl ? state->Viewport[1] :
                                  UnsignedHalfFloatBits(color->Width)) ||
         !ExecuteEmitRegister(emitter,R200_SE_VPORT_YSCALE,
                              hardwareTcl ? state->Viewport[2] :
                                  (UnsignedHalfFloatBits(color->Height) |
                                   0x80000000UL)) ||
         !ExecuteEmitRegister(emitter,R200_SE_VPORT_YOFFSET,
                              hardwareTcl ? state->Viewport[3] :
                                  UnsignedHalfFloatBits(color->Height)) ||
         !ExecuteEmitRegister(emitter,R200_SE_VPORT_ZSCALE,
                              hardwareTcl ? state->Viewport[4] : 0x3f000000UL) ||
         !ExecuteEmitRegister(emitter,R200_SE_VPORT_ZOFFSET,
                              hardwareTcl ? state->Viewport[5] : 0x3f000000UL)))
        return FALSE;
    if (!ExecuteEmitRegister(emitter, R200_SE_VAP_CNTL_STATUS, 0) ||
        !ExecuteEmitRegister(emitter, R200_SE_VAP_CNTL,
                               (hardwareTcl ? R200_VAP_TCL_ENABLE :
                                perspective ? 0UL : R200_VAP_FORCE_W_TO_ONE) |
                                  (9UL << R200_VAP_VF_MAX_VTX_NUM_SHIFT)) ||
        !ExecuteEmitRegister(emitter, R200_SE_VTX_STATE_CNTL,
                              R200_VSC_UPDATE_USER_COLOR_0_ENABLE) ||
         !ExecuteEmitRegister(emitter, R200_SE_VTE_CNTL,
                               (denormalized ? R200_VTX_ST_DENORMALIZED : 0) |
                                   (perspective ? R200_VPORT_X_SCALE_ENA |
                                      R200_VPORT_X_OFFSET_ENA |
                                      R200_VPORT_Y_SCALE_ENA |
                                      R200_VPORT_Y_OFFSET_ENA |
                                      R200_VPORT_Z_SCALE_ENA |
                                      R200_VPORT_Z_OFFSET_ENA |
                                      R200_VTX_W0_FMT : 0)) ||
        !ExecuteEmitRegister(emitter, R200_SE_VTX_FMT_0, format0) ||
        !ExecuteEmitRegister(emitter, R200_SE_VTX_FMT_1, format1) ||
        (hardwareTcl &&
         (!ExecuteEmitRegister(emitter,R200_SE_TCL_OUTPUT_VTX_FMT_0,
                               outputFormat0) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_OUTPUT_VTX_FMT_1,
                               outputFormat1) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_OUTPUT_VTX_COMP_SEL,
                               outputSelect) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_INPUT_VTX_VECTOR_ADDR_0,
                               0x00000000UL) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_INPUT_VTX_VECTOR_ADDR_1,
                               0x00000302UL) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_INPUT_VTX_VECTOR_ADDR_2,
                               0x09080706UL) ||
           !ExecuteEmitRegister(emitter,R200_SE_TCL_INPUT_VTX_VECTOR_ADDR_3,
                                0x00000b0aUL) ||
           !ExecuteEmitRegister(emitter,R200_SE_TCL_MATRIX_SEL_2,2UL) ||
           !ExecuteEmitRegister(emitter,R200_SE_TCL_MATRIX_SEL_3,
                                3UL << R200_TEXMAT_0_SHIFT |
                                4UL << R200_TEXMAT_1_SHIFT) ||
          (normalVertex &&
           (!ExecuteEmitRegister(emitter,R200_SE_TCL_MATRIX_SEL_0,
                                 0UL) ||
            !ExecuteEmitRegister(emitter,R200_SE_TCL_MATRIX_SEL_1,
                                 1UL))) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_LIGHT_MODEL_CTL_0,
                               lightModelCtl0) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_LIGHT_MODEL_CTL_1,
                               0xffff1111UL) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_PER_LIGHT_CTL_0,
                               perLightCtl[0]) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_PER_LIGHT_CTL_1,
                               perLightCtl[1]) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_PER_LIGHT_CTL_2,
                               perLightCtl[2]) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_PER_LIGHT_CTL_3,
                               perLightCtl[3]) ||
           !ExecuteEmitRegister(emitter,R200_SE_TCL_TEX_PROC_CTL_2,
                                texProcControl2) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_TEX_PROC_CTL_3,
                               0x00543210UL) ||
           !ExecuteEmitRegister(emitter,R200_SE_TCL_TEX_PROC_CTL_0,
                                texProcControl0) ||
           !ExecuteEmitRegister(emitter,R200_SE_TCL_TEX_PROC_CTL_1,
                                texProcControl1) ||
          !ExecuteEmitRegister(emitter,R200_SE_TC_TEX_CYL_WRAP_CTL,0) ||
          !ExecuteEmitRegister(emitter,R200_SE_TCL_UCP_VERT_BLEND_CTL,
                               tclControl))) ||
        !ExecuteEmitRegister(emitter, R200_SE_CNTL, seControl) ||
        !ExecuteEmitRegister(emitter, R200_PP_MISC, ppMisc) ||
        !ExecuteEmitRegister(emitter, R200_PP_CNTL, ppControl) ||
        !ExecuteEmitRegister(emitter, R200_PP_TXCBLEND_0,
                              textured && fragmentStatePresent &&
                                      (textureState & RADEON3D_TEX_MODULATE)
                                  ? R200_TXC_ARG_A_R0_COLOR |
                                        R200_TXC_ARG_B_DIFFUSE_COLOR
                              : textured ? R200_TXC_ARG_C_R0_COLOR
                                       : R200_TXC_ARG_C_DIFFUSE_COLOR) ||
        !ExecuteEmitRegister(emitter, R200_PP_TXCBLEND2_0,
                             R200_TXC_CLAMP_0_1 |
                                 R200_TXC_OUTPUT_REG_R0) ||
        !ExecuteEmitRegister(emitter, R200_PP_TXABLEND_0,
                               textured ? R200_TXA_ARG_C_R0_ALPHA
                                        : R200_TXA_ARG_C_DIFFUSE_ALPHA) ||
        !ExecuteEmitRegister(emitter, R200_PP_TXABLEND2_0,
                              R200_TXA_CLAMP_0_1 |
                                  R200_TXA_OUTPUT_REG_R0) ||
        (textured1 &&
         (!ExecuteEmitRegister(emitter, R200_PP_TXCBLEND_1,
                                (texture1State & RADEON3D_TEX_MODULATE)
                                    ? R200_TXC_ARG_A_R1_COLOR |
                                          R200_TXC_ARG_B_R0_COLOR
                                    : R200_TXC_ARG_C_R1_COLOR) ||
          !ExecuteEmitRegister(emitter, R200_PP_TXCBLEND2_1,
                               R200_TXC_CLAMP_0_1 |
                                   R200_TXC_OUTPUT_REG_R0) ||
          !ExecuteEmitRegister(emitter, R200_PP_TXABLEND_1,
                                 R200_TXA_ARG_C_R1_ALPHA) ||
          !ExecuteEmitRegister(emitter, R200_PP_TXABLEND2_1,
                               R200_TXA_CLAMP_0_1 |
                                   R200_TXA_OUTPUT_REG_R0))) ||
        !ExecuteEmitRegister(emitter, R200_PP_CNTL_X, 0))
        return FALSE;

    if (textured &&
        !EmitExecuteTexture(emitter, texture, 0, options,
                             fragmentStatePresent,
                             textureOffset, textureWidth, textureHeight,
                              textureState, textureBytes, perspective,
                              texGen0 && (state->TexGenState[0] &
                                          RADEON3D_TEXGEN_GEN_Q)))
        return FALSE;
    if (textured1 &&
        !EmitExecuteTexture(emitter, texture1, 1, options, TRUE,
                             texture1Offset, texture1Width, texture1Height,
                              texture1State, texture1Bytes, perspective,
                              texGen1 && (state->TexGenState[1] &
                                          RADEON3D_TEXGEN_GEN_Q)))
        return FALSE;
    if (fog &&
        !ExecuteEmitRegister(emitter, R200_PP_FOG_COLOR,
                             fogColor | R200_FOG_USE_VTX_FOG))
        return FALSE;
    if (useDepth &&
        (!ExecuteEmitRegister(emitter, R200_RB3D_DEPTHOFFSET,
                              depth->GpuAddress) ||
         !ExecuteEmitRegister(emitter, R200_RB3D_DEPTHPITCH,
                              depth->Pitch / 2UL) ||
         !ExecuteEmitRegister(emitter, R200_RB3D_ZSTENCILCNTL, zControl)))
        return FALSE;

    if (!ExecuteEmitRegister(emitter, R200_RE_AUX_SCISSOR_CNTL, 0) ||
        !ExecuteEmitRegister(emitter,R200_RE_CNTL,R200_SCISSOR_ENABLE |
                                (perspective ? R200_PERSPECTIVE_ENABLE : 0)) ||
        !ExecuteEmitRegister(emitter, R200_RE_TOP_LEFT,
                                left | (top << 16)) ||
        !ExecuteEmitRegister(emitter, R200_RE_WIDTH_HEIGHT,
                                (right - 1UL) | ((bottom - 1UL) << 16)) ||
        !ExecuteEmitRegister(emitter, R200_RB3D_PLANEMASK,
                                 (options & RADEON3D_EXEC_SUPPRESS_COLOR_WRITE)
                                     ? 0UL : 0xffffffffUL) ||
        !ExecuteEmitRegister(emitter, R200_RB3D_BLENDCNTL,
                                blendControl) ||
        !ExecuteEmitRegister(emitter, RADEON_RB3D_CNTL, rbControl) ||
        !ExecuteEmitRegister(emitter, R200_RB3D_COLOROFFSET,
                                color->GpuAddress) ||
        !ExecuteEmitRegister(emitter, R200_RB3D_COLORPITCH,
                             color->Pitch / SurfaceBytesPerPixel(color)))
        return FALSE;
    /* The guard-clip scalars and the MVP upload are emitted separately by
     * EmitExecuteStateCached(), which caches them across records. */
    return TRUE;
}
static BOOL EmitExecuteStateCached(struct Radeon3DEmitter *emitter,
                                   const struct Radeon3DEmitState *state)
{
    ULONG index;

    if (!emitter->StateValid || !SameExecuteState(&emitter->State, state)) {
        BOOL unit0 = FALSE, unit1 = FALSE;

        if (emitter->StateValid &&
            TextureOnlyDelta(&emitter->State, state, &unit0, &unit1)) {
            BOOL perspective = state->HardwareTcl ||
                (state->ExtendedVertex &&
                 (state->VertexState &
                  RADEON3D_VERTEX_CLIP_COORDINATES));
            BOOL texGen0 = state->TexGen &&
                state->TexGenState[0] != RADEON3D_TEXGEN_MODE_OFF;
            BOOL texGen1 = state->TexGen &&
                state->TexGenState[1] != RADEON3D_TEXGEN_MODE_OFF;

            if (unit0 && state->TextureValid &&
                !EmitExecuteTexture(emitter, state->TextureValid ? &state->Texture : NULL, 0,
                                    state->Options,
                                    state->FragmentStatePresent,
                                    state->TextureOffset,
                                    state->TextureWidth,
                                    state->TextureHeight,
                                    state->TextureState,
                                    state->TextureBytes, perspective,
                                    texGen0 &&
                                    (state->TexGenState[0] &
                                     RADEON3D_TEXGEN_GEN_Q)))
                return FALSE;
            if (unit1 && state->Texture1Valid &&
                !EmitExecuteTexture(emitter, state->Texture1Valid ? &state->Texture1 : NULL, 1,
                                    state->Options, TRUE,
                                    state->Texture1Offset,
                                    state->Texture1Width,
                                    state->Texture1Height,
                                    state->Texture1State,
                                    state->Texture1Bytes, perspective,
                                    texGen1 &&
                                    (state->TexGenState[1] &
                                     RADEON3D_TEXGEN_GEN_Q)))
                return FALSE;
            emitter->State = *state;
            return TRUE;
        }
        if (!EmitExecuteState(emitter, state))
            return FALSE;
        emitter->State = *state;
        emitter->StateValid = TRUE;
    }
    if (state->HardwareTcl) {
        if (!emitter->GuardClipEmitted) {
            if (!ExecuteEmitGuardClipState(emitter))
                return FALSE;
            emitter->GuardClipEmitted = TRUE;
        }
        if (!emitter->MatrixValid) {
            emitter->MatrixValid = TRUE;
            for (index = 0; index < 16UL; ++index)
                emitter->Matrix[index] = state->ModelProjection[index];
            if (!ExecuteEmitMatrix(emitter, state->ModelProjection,
                                   R200_VS_MATRIX_2_MVP, TRUE))
                return FALSE;
        } else {
            for (index = 0; index < 16UL; ++index)
                if (emitter->Matrix[index] != state->ModelProjection[index]) {
                    for (; index < 16UL; ++index)
                        emitter->Matrix[index] =
                            state->ModelProjection[index];
                    if (!ExecuteEmitMatrix(emitter, state->ModelProjection,
                                           R200_VS_MATRIX_2_MVP, TRUE))
                        return FALSE;
                    break;
                }
        }
        for (index = 0; index < 2UL; ++index) {
            ULONG component;
            ULONG vectorAddress = index ? R200_VS_MATRIX_4_TEX1 :
                                         R200_VS_MATRIX_3_TEX0;

            if (state->TexGenState[index] == RADEON3D_TEXGEN_MODE_OFF)
                continue;
            if (!emitter->TexGenMatrixValid[index]) {
                emitter->TexGenMatrixValid[index] = TRUE;
                for (component = 0; component < 16UL; ++component)
                    emitter->TexGenMatrix[index][component] =
                        state->TexGenMatrix[index][component];
                if (!ExecuteEmitMatrix(emitter, state->TexGenMatrix[index],
                                       vectorAddress, TRUE))
                    return FALSE;
                continue;
            }
            for (component = 0; component < 16UL; ++component)
                if (emitter->TexGenMatrix[index][component] !=
                    state->TexGenMatrix[index][component]) {
                    for (; component < 16UL; ++component)
                        emitter->TexGenMatrix[index][component] =
                            state->TexGenMatrix[index][component];
                    if (!ExecuteEmitMatrix(emitter,
                                           state->TexGenMatrix[index],
                                           vectorAddress, TRUE))
                        return FALSE;
                    break;
                }
        }
        if (state->NormalVertex) {
            const ULONG *sources[2];
            BOOL *valid[2];
            ULONG *shadow[2];
            static const ULONG addresses[2] = {R200_VS_MATRIX_0_MV,
                                               R200_VS_MATRIX_1_INV_MV};
            ULONG which;

            sources[0] = state->ModelView;
            sources[1] = state->InvModelView;
            valid[0] = &emitter->ModelViewValid;
            valid[1] = &emitter->InvModelViewValid;
            shadow[0] = emitter->ModelView;
            shadow[1] = emitter->InvModelView;
            for (which = 0; which < 2UL; ++which) {
                BOOL changed = !*valid[which];

                for (index = 0; index < 16UL; ++index)
                    if (shadow[which][index] != sources[which][index]) {
                        changed = TRUE;
                        break;
                    }
                if (!changed)
                    continue;
                *valid[which] = TRUE;
                for (index = 0; index < 16UL; ++index)
                    shadow[which][index] = sources[which][index];
                if (!ExecuteEmitMatrix(emitter, sources[which],
                                       addresses[which], which == 0UL))
                    return FALSE;
            }
        }
        if (state->Lighting) {
            ULONG light;

            if (!ExecuteEmitVectorBlock(emitter, R200_VS_GLOBAL_AMBIENT_ADDR,
                                        1UL, state->GlobalAmbient, 4UL) ||
                !ExecuteEmitVectorBlock(emitter, R200_VS_EYE_VECTOR_ADDR,
                                        1UL, state->EyeVector, 4UL))
                return FALSE;
            /* Mesa encodes the material-shininess scalar page as
             * SS_MAT_0_SHININESS - 0x100 via its SCALARS2 command. */
            if (!ExecuteEmitVectorBlock(emitter, R200_VS_MAT_0_EMISS,
                                        1UL, state->Material, 16UL) ||
                !ExecuteEmitScalarBlock(emitter,
                                        R200_SS_MAT_0_SHININESS - 0x100UL,
                                        1UL, state->Material + 16UL, 1UL))
                return FALSE;
            for (light = 0; light < 8UL; ++light) {
                if (!(state->LightControl &
                      (RADEON3D_LIGHT_CONTROL_ENABLED_MASK << light)))
                    continue;
                if (!ExecuteEmitVectorBlock(
                        emitter, R200_VS_LIGHT_AMBIENT_ADDR + light,
                        R200_LIGHT_VECTOR_STRIDE, state->Lights[light],
                        RADEON3D_LIGHT_BLOCK_VECTOR_DWORDS) ||
                    !ExecuteEmitScalarBlock(
                        emitter, R200_SS_LIGHT_DCD_ADDR + light,
                        R200_LIGHT_VECTOR_STRIDE,
                        state->Lights[light] +
                            RADEON3D_LIGHT_BLOCK_VECTOR_DWORDS,
                        RADEON3D_LIGHT_BLOCK_SCALAR_DWORDS))
                    return FALSE;
            }
        }
    }
    return TRUE;
}
static BOOL EmitExecuteVertices(struct Radeon3DEmitter *emitter,
                                  const ULONG *vertices, ULONG vertexCount,
                                  BOOL useDepth, BOOL textured,
                                  BOOL fragmentStatePresent,
                                  BOOL extendedVertex, BOOL hardwareTcl,
                                  BOOL normalVertex, BOOL compactVertex,
                                  ULONG vertexState,
                                  ULONG primitiveType,
                                  const struct Radeon3DEmitSurface *color)
{
    BOOL perspective=hardwareTcl || (extendedVertex &&
                     (vertexState & RADEON3D_VERTEX_CLIP_COORDINATES));
    ULONG tclStride = hardwareTcl ?
        (compactVertex ? (normalVertex ? 10UL : 7UL) :
         normalVertex ? RADEON3D_EXEC_HW_TCL_NORMAL_VERTEX_DWORDS :
                        RADEON3D_EXEC_HW_TCL_VERTEX_DWORDS) : 0UL;
    ULONG dwordsPerVertex = hardwareTcl ?
        5UL + (normalVertex ? 3UL : 0UL) +
              (textured ? 2UL : 0UL) +
              ((vertexState & RADEON3D_VERTEX_TEXTURE1) ? 2UL : 0UL) +
              ((vertexState & RADEON3D_VERTEX_FOG) ? 1UL : 0UL) :
        3UL + ((useDepth || perspective) ? 1UL : 0UL) +
                               (perspective ? 1UL : 0UL) +
                               (textured ? (fragmentStatePresent ? 2UL : 1UL) : 0UL) +
                              ((extendedVertex &&
                                (vertexState & RADEON3D_VERTEX_FOG))
                                   ? 1UL : 0UL) +
                             ((extendedVertex &&
                               (vertexState & RADEON3D_VERTEX_TEXTURE1))
                                   ? 2UL : 0UL);
    ULONG vertexDwords = vertexCount * dwordsPerVertex;
    ULONG vertex;

    if (emitter->CommitVbuf) {
        UBYTE components[6];
        UBYTE offsets[6];
        ULONG arrays = 0;
        ULONG array;
        ULONG payload;
        ULONG colorOffset = normalVertex ? 7UL : 4UL;

        /* Vertex data is fetched from the segment by the hardware. The
         * VAP needs one array descriptor per active attribute. The arrays
         * are interleaved in one segment record, so they share a stride but
         * start at the position/normal/fog/color/texture dword offsets.
         * VBUF_2 then fires the primitive with WALK_LIST. Hardware TCL
         * is mandatory: SE_VTX_FMT describes the vertex layout. */
        if (!hardwareTcl)
            return FALSE;
        components[arrays] = 4;
        offsets[arrays++] = 0;
        if (normalVertex) {
            components[arrays] = 3;
            offsets[arrays++] = 4;
        }
        if (vertexState & RADEON3D_VERTEX_FOG) {
            components[arrays] = 1;
            offsets[arrays++] = normalVertex ? 12 : 9;
        }
        if (!textured || fragmentStatePresent) {
            components[arrays] = 1;
            offsets[arrays++] = (UBYTE)colorOffset;
        }
        if (textured) {
            components[arrays] = 2;
            offsets[arrays++] = (UBYTE)(normalVertex ? 8UL : 5UL);
        }
        if (vertexState & RADEON3D_VERTEX_TEXTURE1) {
            components[arrays] = 2;
            offsets[arrays++] = (UBYTE)(normalVertex ? 10UL : 7UL);
        }
        payload = 1UL + (arrays >> 1) * 3UL + (arrays & 1UL) * 2UL;
        if (!ExecuteEmitWord(
                emitter,
                RADEON_CP_PACKET3(R200_CP_CMD_3D_LOAD_VBPNTR,
                                  payload - 1UL)) ||
            !ExecuteEmitWord(emitter, arrays))
            return FALSE;
        for (array = 0; array + 1UL < arrays; array += 2UL) {
            if (!ExecuteEmitWord(
                    emitter,
                    components[array] | (tclStride << 8) |
                        ((ULONG)components[array + 1UL] << 16) |
                        (tclStride << 24)) ||
                !ExecuteEmitWord(emitter,
                                 emitter->CommitVbufAddress +
                                     (ULONG)offsets[array] * 4UL) ||
                !ExecuteEmitWord(emitter,
                                 emitter->CommitVbufAddress +
                                     (ULONG)offsets[array + 1UL] * 4UL))
                return FALSE;
        }
        if (array < arrays &&
            (!ExecuteEmitWord(emitter,
                              components[array] | (tclStride << 8)) ||
             !ExecuteEmitWord(emitter,
                              emitter->CommitVbufAddress +
                                  (ULONG)offsets[array] * 4UL)))
            return FALSE;
        return ExecuteEmitWord(
                   emitter,
                   RADEON_CP_PACKET3(R200_CP_CMD_3D_DRAW_VBUF_2, 0UL)) &&
               ExecuteEmitWord(emitter,
                               (vertexCount << 16) |
                                   R200_CP_VC_CNTL_PRIM_WALK_LIST |
                                   R200_VF_TCL_OUTPUT_VTX_ENABLE |
                                   primitiveType);
    }

    if (hardwareTcl &&
        !(vertexState & (RADEON3D_VERTEX_TEXTURE1 |
                         RADEON3D_VERTEX_FOG))) {
        /* The common TCL vertex shapes emit a contiguous prefix of their
         * record: 0..9 textured with normals, 0..7 unlit with normals,
         * 0..6 textured without normals. Validate every dword once, then
         * block-copy the prefix instead of paying a bounds-checked call
         * per emitted dword. Compact records omit only the unit-1/fog
         * tail; inactive unit-0 coordinates remain in their ABI stride. */
        ULONG emitted = normalVertex ? (textured ? 10UL : 8UL) :
                        textured ? 7UL : 5UL;

        if (emitted) {
            ULONG stride = tclStride;
            /* Generated components sit before the unset-feature zeros:
             * unit 1 starts at dword 10 (normals) or 7 (plain), and unit 0
             * itself is zero when texturing is off. */
            ULONG firstZero = normalVertex ?
                                  (textured ? 10UL : 8UL) :
                                  textured ? 7UL : 5UL;

            if (emitter->Count + 2UL + vertexDwords >
                    RADEON3D_MAX_BATCH_DWORDS)
                return FALSE;
            if (!ExecuteEmitWord(emitter,
                                 RADEON_CP_PACKET3(R200_CP_CMD_3D_DRAW_IMMD_2,
                                                   vertexDwords)) ||
                !ExecuteEmitWord(emitter,
                                  (vertexCount << 16) |
                                      R200_CP_VC_CNTL_PRIM_WALK_RING |
                                      R200_VF_TCL_OUTPUT_VTX_ENABLE |
                                      primitiveType))
                return FALSE;
            for (vertex = 0; vertex < vertexCount; ++vertex) {
                const ULONG *input = vertices + vertex * stride;
                ULONG *output = emitter->Words + emitter->Count;
                ULONG index;

                if (!ValidFloat(input[0]) || !ValidFloat(input[1]) ||
                    !ValidFloat(input[2]) || !ValidFloat(input[3]) ||
                    (normalVertex && (!ValidFloat(input[4]) ||
                                      !ValidFloat(input[5]) ||
                                      !ValidFloat(input[6]))) ||
                    (textured
                         ? (!ValidTextureCoordinate(
                                input[normalVertex ? 8UL : 5UL]) ||
                            !ValidTextureCoordinate(
                                input[normalVertex ? 9UL : 6UL]))
                         : (input[normalVertex ? 8UL : 5UL] ||
                            input[normalVertex ? 9UL : 6UL])))
                    return FALSE;
                /* Full-stride records must carry zero dwords for unset
                 * features; compact records simply end at the prefix. */
                if (!compactVertex) {
                    if (input[firstZero] || input[firstZero + 1UL])
                        return FALSE;
                    for (index = firstZero + 2UL; index < stride; ++index)
                        if (input[index])
                            return FALSE;
                }
                for (index = 0; index < emitted; ++index)
                    output[index] = input[index];
                emitter->Count += emitted;
            }
            return TRUE;
        }
    }
    if (!ExecuteEmitWord(emitter,
                         RADEON_CP_PACKET3(R200_CP_CMD_3D_DRAW_IMMD_2,
                                           vertexDwords)) ||
        !ExecuteEmitWord(emitter,
                          (vertexCount << 16) |
                              R200_CP_VC_CNTL_PRIM_WALK_RING |
                              (hardwareTcl ? R200_VF_TCL_OUTPUT_VTX_ENABLE : 0) |
                              primitiveType))
        return FALSE;
    for (vertex = 0; vertex < vertexCount; ++vertex) {
        const ULONG *input = vertices + vertex *
            (hardwareTcl ? tclStride :
             extendedVertex ? RADEON3D_EXEC_EXTENDED_VERTEX_DWORDS
                       : RADEON3D_EXEC_VERTEX_DWORDS);

        if (hardwareTcl) {
            /* The packed ARGB colour dword is shared with the non-TCL paths;
             * it needs no float validation. With normals the triple sits
             * between W and colour and every later dword shifts by three. */
            ULONG colorIndex = normalVertex ? 7UL : 4UL;
            ULONG st0Index = normalVertex ? 8UL : 5UL;
            ULONG st1Index = normalVertex ? 10UL : 7UL;
            ULONG fogIndex = normalVertex ? 12UL : 9UL;

            if (!ValidFloat(input[0]) || !ValidFloat(input[1]) ||
                !ValidFloat(input[2]) || !ValidFloat(input[3]) ||
                (normalVertex && (!ValidFloat(input[4]) ||
                                  !ValidFloat(input[5]) ||
                                  !ValidFloat(input[6]))) ||
                (textured && (!ValidTextureCoordinate(input[st0Index]) ||
                              !ValidTextureCoordinate(input[st0Index +
                                                            1UL]))) ||
                (!textured && (input[st0Index] ||
                               input[st0Index + 1UL])) ||
                (vertexState & RADEON3D_VERTEX_TEXTURE1
                     ? (!ValidTextureCoordinate(input[st1Index]) ||
                        !ValidTextureCoordinate(input[st1Index + 1UL]))
                      : (input[st1Index] || input[st1Index + 1UL])) ||
                (vertexState & RADEON3D_VERTEX_FOG
                     ? !ValidUnitFloat(input[fogIndex])
                      : input[fogIndex]))
                return FALSE;
            if (!ExecuteEmitWord(emitter,input[0]) ||
                !ExecuteEmitWord(emitter,input[1]) ||
                !ExecuteEmitWord(emitter,input[2]) ||
                !ExecuteEmitWord(emitter,input[3]))
                return FALSE;
            if (normalVertex &&
                (!ExecuteEmitWord(emitter,input[4]) ||
                 !ExecuteEmitWord(emitter,input[5]) ||
                 !ExecuteEmitWord(emitter,input[6])))
                return FALSE;
            if (!ExecuteEmitWord(emitter,input[colorIndex]))
                return FALSE;
            if (textured &&
                (!ExecuteEmitWord(emitter,input[st0Index]) ||
                 !ExecuteEmitWord(emitter,input[st0Index + 1UL])))
                return FALSE;
            if ((vertexState & RADEON3D_VERTEX_TEXTURE1) &&
                (!ExecuteEmitWord(emitter,input[st1Index]) ||
                 !ExecuteEmitWord(emitter,input[st1Index + 1UL])))
                return FALSE;
            if ((vertexState & RADEON3D_VERTEX_FOG) &&
                !ExecuteEmitWord(emitter,input[fogIndex]))
                return FALSE;
            continue;
        }

        if (color &&
            ((perspective ? (!ValidFloat(input[0]) ||
                              !ValidFloat(input[1]) ||
                              !ValidFloat(input[2]))
                          : (!ValidScreenCoordinate(input[0],color->Width) ||
                             !ValidScreenCoordinate(input[1],color->Height) ||
                             !ValidUnitFloat(input[2]))) ||
             (fragmentStatePresent && textured
                  ? (!ValidTextureCoordinate(input[3]) ||
                     !ValidTextureCoordinate(input[4]))
                  : (!ValidUnitFloat(input[3]) ||
                     !ValidUnitFloat(input[4]))) ||
             (!textured && (input[3] || input[4])) ||
             (extendedVertex &&
              (vertexState & RADEON3D_VERTEX_TEXTURE1
                   ? (!ValidTextureCoordinate(input[6]) ||
                      !ValidTextureCoordinate(input[7]))
                   : (input[6] || input[7]))) ||
             (extendedVertex &&
               (vertexState & RADEON3D_VERTEX_FOG
                    ? !ValidUnitFloat(input[8])
                     : (vertexState & RADEON3D_VERTEX_CLIP_COORDINATES)
                           ? !ValidPositiveFloat(input[8])
                          : input[8] != 0))))
            return FALSE;
        if (!ExecuteEmitWord(emitter, input[0]) ||
            !ExecuteEmitWord(emitter, input[1]))
            return FALSE;
        if ((useDepth || perspective) && !ExecuteEmitWord(emitter,input[2]))
            return FALSE;
        if (perspective && !ExecuteEmitWord(emitter,input[8]))
            return FALSE;
        if (extendedVertex && (vertexState & RADEON3D_VERTEX_FOG) &&
            !ExecuteEmitWord(emitter, input[8]))
            return FALSE;
        if (extendedVertex && !ExecuteEmitWord(emitter, input[5]))
            return FALSE;
        if (textured) {
            if (fragmentStatePresent && !extendedVertex &&
                !ExecuteEmitWord(emitter, input[5]))
                return FALSE;
            if (!ExecuteEmitWord(emitter, input[3]) ||
                !ExecuteEmitWord(emitter, input[4]))
                return FALSE;
        } else if (!extendedVertex && !ExecuteEmitWord(emitter, input[5]))
            return FALSE;
        if (extendedVertex && (vertexState & RADEON3D_VERTEX_TEXTURE1) &&
            (!ExecuteEmitWord(emitter, input[6]) ||
              !ExecuteEmitWord(emitter, input[7])))
            return FALSE;
    }
    return TRUE;
}
BOOL Radeon3DEmitClear(                             struct Radeon3DEmitter *emitter,
                             const ULONG *record, ULONG length)
{
    struct Radeon3DEmitSurface *color;
    struct Radeon3DEmitSurface *depth;
    struct Radeon3DEmitState *state = &emitter->Scratch;
    ULONG clearMask;
    ULONG vertices[6UL * RADEON3D_EXEC_VERTEX_DWORDS];
    ULONG vertex;
    static const UBYTE corners[12] = {0, 0, 1, 0, 1, 1,
                                      0, 0, 1, 1, 0, 1};

    ClearExecuteState(state);
    if (length != RADEON3D_EXEC_CLEAR_DWORDS)
        return FALSE;
    color = ExecuteSurface(emitter, record[2]);
    depth = ExecuteSurface(emitter, record[3]);
    clearMask = record[4];
    if (!clearMask || (clearMask & ~RADEON3D_CLEAR_MASK) ||
        !ValidColorTarget(emitter, color) ||
        ((clearMask & RADEON3D_CLEAR_DEPTH) &&
         (!ValidDepthTarget(depth, color) || !ValidUnitFloat(record[6]))) ||
        (!(clearMask & RADEON3D_CLEAR_DEPTH) && record[3]) ||
        !ValidExecuteScissor(color, record[7], record[8],
                             record[9], record[10]))
        return FALSE;
    for (vertex = 0; vertex < 6UL; ++vertex) {
        ULONG *output = vertices + vertex * RADEON3D_EXEC_VERTEX_DWORDS;

        output[0] = UnsignedFloatBits(corners[vertex * 2UL]
                                         ? record[9]
                                         : record[7]);
        output[1] = UnsignedFloatBits(corners[vertex * 2UL + 1UL]
                                         ? record[10]
                                         : record[8]);
        output[2] = record[6];
        output[3] = 0;
        output[4] = 0;
        output[5] = record[5];
    }
    if (color) {
        state->Color = *color;
        state->ColorValid = TRUE;
    } else {
        state->ColorValid = FALSE;
    }
    if (depth) {
        state->Depth = *depth;
        state->DepthValid = TRUE;
    } else {
        state->DepthValid = FALSE;
    }
    state->Options = (clearMask & RADEON3D_CLEAR_COLOR)
                        ? 0UL : RADEON3D_EXEC_SUPPRESS_COLOR_WRITE;
    state->Left = record[7];
    state->Top = record[8];
    state->Right = record[9];
    state->Bottom = record[10];
    state->ClearDepth = (clearMask & RADEON3D_CLEAR_DEPTH) != 0;
    return EmitExecuteStateCached(emitter, state) &&
           EmitExecuteVertices(emitter, vertices, 6UL, depth != NULL,
                                FALSE, FALSE, FALSE, FALSE, FALSE, FALSE,
                                0,
                                R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST, NULL);
}
BOOL Radeon3DEmitDraw(                             struct Radeon3DEmitter *emitter,
                             const ULONG *record, ULONG length,
                             ULONG primitiveType)
{
    struct Radeon3DEmitSurface *color;
    struct Radeon3DEmitSurface *depth;
    struct Radeon3DEmitSurface *texture;
    struct Radeon3DEmitSurface *texture1 = NULL;
    struct Radeon3DEmitState *state = &emitter->Scratch;
    ULONG options;
    ULONG vertexCount;
    const ULONG *vertices;
    ULONG headerDwords;
    ULONG textureOffset = 0, textureWidth = 0, textureHeight = 0;
    ULONG textureState = 0, fragmentState = 0, textureBytes = 0;
    ULONG texture1Offset = 0, texture1Width = 0, texture1Height = 0;
    ULONG texture1State = 0, texture1Bytes = 0, vertexState = 0;
    ULONG fogColor = 0;
    ULONG levels = 1, minFilter = 0, sourceBlend = 0, destinationBlend = 0;
    ULONG levels1 = 1, minFilter1 = 0;
    ULONG vertexStride;
    ULONG commitOffset = 0;
    BOOL compactVertex;
    BOOL textured;
    BOOL textured1;
    BOOL fog;
    BOOL perspective;
    BOOL useDepth;
    BOOL fragmentStatePresent;
    BOOL extendedVertex;
    BOOL hardwareTcl;
    BOOL texGen;
    BOOL normalVertex;
    BOOL lighting;
    ULONG matrixBase;
    ULONG lightControlIndex;
    ULONG lightBlockBase;
    ULONG enabledLights;
    ULONG texGenMode0;
    ULONG texGenMode1;

    ClearExecuteState(state);
    if (length < RADEON3D_EXEC_DRAW_HEADER_DWORDS)
        return FALSE;
    color = ExecuteSurface(emitter, record[2]);
    depth = ExecuteSurface(emitter, record[3]);
    texture = ExecuteSurface(emitter, record[4]);
    options = record[5];
    fragmentStatePresent =
        (options & RADEON3D_DRAW_FRAGMENT_STATE) != 0;
    extendedVertex = (options & RADEON3D_DRAW_EXTENDED_VERTEX) != 0;
    hardwareTcl = (options & RADEON3D_DRAW_HW_TCL) != 0;
    texGen = (options & RADEON3D_DRAW_TEXGEN) != 0;
    normalVertex = (options & RADEON3D_DRAW_NORMALS) != 0;
    lighting = (options & RADEON3D_DRAW_LIGHTING) != 0;
    compactVertex = (options & RADEON3D_DRAW_COMPACT_VERTEX) != 0;
    if (lighting)
        normalVertex = TRUE;
    headerDwords = texGen ? RADEON3D_EXEC_DRAW_TEXGEN_HEADER_DWORDS :
                   hardwareTcl ? RADEON3D_EXEC_DRAW_HW_TCL_HEADER_DWORDS :
                    extendedVertex ? RADEON3D_EXEC_DRAW_EXTENDED_HEADER_DWORDS
                           : fragmentStatePresent
                               ? RADEON3D_EXEC_DRAW_FRAGMENT_HEADER_DWORDS
                                     : RADEON3D_EXEC_DRAW_HEADER_DWORDS;
    matrixBase = headerDwords;
    if (hardwareTcl && normalVertex) {
        /* Model-view then inverse model-view, 16 dwords each. */
        matrixBase = headerDwords;
        headerDwords += RADEON3D_EXEC_NORMAL_MATRICES_DWORDS;
    }
    lightControlIndex = 0;
    lightBlockBase = 0;
    enabledLights = 0;
    if (lighting && hardwareTcl) {
        lightControlIndex = headerDwords + 8UL;
        lightBlockBase = headerDwords +
                         RADEON3D_EXEC_LIGHT_STATE_DWORDS;
        headerDwords += RADEON3D_EXEC_LIGHT_STATE_DWORDS;
    }
    vertexStride = hardwareTcl ?
        (compactVertex ? (normalVertex ? 10UL : 7UL) :
         normalVertex ? RADEON3D_EXEC_HW_TCL_NORMAL_VERTEX_DWORDS :
                        RADEON3D_EXEC_HW_TCL_VERTEX_DWORDS) :
        extendedVertex ? RADEON3D_EXEC_EXTENDED_VERTEX_DWORDS
                       : RADEON3D_EXEC_VERTEX_DWORDS;
    if (emitter->CommitVbuf) {
        commitOffset =
            emitter->CommitVertexOffsets[emitter->CommitDrawIndex++];
        emitter->CommitVbufAddress = emitter->CommitSegmentGpuBase +
                                     commitOffset;
    }
    if (length < headerDwords) {
        if (emitter->CommitVbuf)
            emitter->FailStage = 20UL;
        return FALSE;
    }
    if (lighting && hardwareTcl) {
        ULONG scan;

        enabledLights = record[lightControlIndex] &
                        RADEON3D_LIGHT_CONTROL_ENABLED_MASK;
        scan = enabledLights;
        while (scan) {
            headerDwords += (scan & 1UL) ?
                RADEON3D_EXEC_LIGHT_BLOCK_DWORDS : 0UL;
            scan >>= 1;
        }
    }
    if (length < headerDwords)
        return FALSE;
    vertexCount = record[10];
    vertices = record + headerDwords;
    textured = (options & RADEON3D_DRAW_TEXTURED) != 0;
    useDepth = (options & (RADEON3D_DRAW_DEPTH_LESS |
                           RADEON3D_DRAW_DEPTH_WRITE)) != 0;
    if (fragmentStatePresent) {
        textureOffset = record[11];
        textureWidth = (record[12] & 0xffffUL) + 1UL;
        textureHeight = (record[12] >> 16) + 1UL;
        textureState = record[13];
        fragmentState = record[14];
        levels = ((textureState & RADEON3D_TEX_LEVELS_MASK) >>
                  RADEON3D_TEX_LEVELS_SHIFT) + 1UL;
        minFilter = (textureState & RADEON3D_TEX_MIN_MASK) >>
                    RADEON3D_TEX_MIN_SHIFT;
        sourceBlend = (fragmentState & RADEON3D_FRAGMENT_SRC_MASK) >>
                      RADEON3D_FRAGMENT_SRC_SHIFT;
        destinationBlend = (fragmentState & RADEON3D_FRAGMENT_DST_MASK) >>
                           RADEON3D_FRAGMENT_DST_SHIFT;
    }
    if (extendedVertex) {
        texture1 = ExecuteSurface(emitter, record[15]);
        texture1Offset = record[16];
        texture1Width = (record[17] & 0xffffUL) + 1UL;
        texture1Height = (record[17] >> 16) + 1UL;
        texture1State = record[18];
        vertexState = record[19];
        fogColor = record[20];
        levels1 = ((texture1State & RADEON3D_TEX_LEVELS_MASK) >>
                   RADEON3D_TEX_LEVELS_SHIFT) + 1UL;
        minFilter1 = (texture1State & RADEON3D_TEX_MIN_MASK) >>
                     RADEON3D_TEX_MIN_SHIFT;
    }
    textured1 = extendedVertex &&
                (vertexState & RADEON3D_VERTEX_TEXTURE1) != 0;
    fog = extendedVertex &&
          (vertexState & RADEON3D_VERTEX_FOG) != 0;
    perspective = extendedVertex &&
                  (vertexState & RADEON3D_VERTEX_CLIP_COORDINATES) != 0;
    texGenMode0 = texGen ? record[44] & RADEON3D_TEXGEN_MODE_MASK :
                           RADEON3D_TEXGEN_MODE_OFF;
    texGenMode1 = texGen ? record[45] & RADEON3D_TEXGEN_MODE_MASK :
                           RADEON3D_TEXGEN_MODE_OFF;
    if ((options & ~RADEON3D_DRAW_OPTIONS) ||
         (hardwareTcl && (emitter->InterfaceVersion < 9UL ||
                         !extendedVertex || !fragmentStatePresent ||
                         perspective ||
                         (record[43] & ~RADEON3D_TRANSFORM_STATE_MASK) ||
                         !(record[43] &
                            RADEON3D_TRANSFORM_POINT_SIZE_MASK))) ||
         (texGen &&
          (emitter->InterfaceVersion < 10UL || !hardwareTcl ||
           (record[44] & ~RADEON3D_TEXGEN_STATE_MASK) ||
           (record[45] & ~RADEON3D_TEXGEN_STATE_MASK) ||
            texGenMode0 > RADEON3D_TEXGEN_MODE_SPHERE_MAP ||
            texGenMode1 > RADEON3D_TEXGEN_MODE_SPHERE_MAP ||
            (!texGenMode0 &&
             (record[44] & RADEON3D_TEXGEN_COMPONENTS)) ||
            (!texGenMode1 &&
             (record[45] & RADEON3D_TEXGEN_COMPONENTS)) ||
            (texGenMode0 &&
             ((record[44] & (RADEON3D_TEXGEN_GEN_S |
                             RADEON3D_TEXGEN_GEN_T)) !=
                  (RADEON3D_TEXGEN_GEN_S | RADEON3D_TEXGEN_GEN_T) ||
              !textured)) ||
            (texGenMode1 &&
             ((record[45] & (RADEON3D_TEXGEN_GEN_S |
                             RADEON3D_TEXGEN_GEN_T)) !=
                  (RADEON3D_TEXGEN_GEN_S | RADEON3D_TEXGEN_GEN_T) ||
              !textured1)) ||
            (texGenMode0 == RADEON3D_TEXGEN_MODE_SPHERE_MAP &&
             (emitter->InterfaceVersion < 12UL || !normalVertex ||
              (record[44] & RADEON3D_TEXGEN_COMPONENTS) !=
                  (RADEON3D_TEXGEN_GEN_S | RADEON3D_TEXGEN_GEN_T))) ||
            (texGenMode1 == RADEON3D_TEXGEN_MODE_SPHERE_MAP &&
             (emitter->InterfaceVersion < 12UL || !normalVertex ||
              (record[45] & RADEON3D_TEXGEN_COMPONENTS) !=
                  (RADEON3D_TEXGEN_GEN_S | RADEON3D_TEXGEN_GEN_T))))) ||
          ((normalVertex || lighting) &&
           (emitter->InterfaceVersion < 11UL || !hardwareTcl ||
            !extendedVertex || !fragmentStatePresent)) ||
          (lighting &&
           (record[lightControlIndex] &
              RADEON3D_LIGHT_CONTROL_RESERVED)) ||
         (!hardwareTcl && (options & ~RADEON3D_DRAW_OPTIONS_PRE_TCL)) ||
        (extendedVertex &&
         (!fragmentStatePresent || emitter->InterfaceVersion < 5UL)) ||
        (!extendedVertex &&
         (options & ~RADEON3D_DRAW_OPTIONS_FRAGMENT)) ||
        (fragmentStatePresent && emitter->InterfaceVersion < 4UL) ||
        (!fragmentStatePresent &&
         options & ~RADEON3D_DRAW_OPTIONS_BASIC) ||
        (emitter->InterfaceVersion < 3UL &&
         (options & RADEON3D_DRAW_DEPTH_FUNC_MASK)) ||
        (fragmentStatePresent &&
         (options & (RADEON3D_DRAW_BILINEAR |
                                RADEON3D_DRAW_ALPHA_BLEND))) ||
        (fragmentStatePresent &&
         ((textureState & ~RADEON3D_TEX_STATE_MASK) ||
                     minFilter > RADEON3D_TEX_MIN_LINEAR_MIPMAP_LINEAR ||
                     (fragmentState & ~RADEON3D_FRAGMENT_STATE_MASK) ||
                     sourceBlend > RADEON3D_BLEND_SRC_ALPHA_SATURATE ||
                     destinationBlend > RADEON3D_BLEND_ONE_MINUS_DST_ALPHA)) ||
        (extendedVertex &&
         ((vertexState & ~RADEON3D_VERTEX_STATE_MASK) ||
          ((vertexState & RADEON3D_VERTEX_CLIP_COORDINATES) &&
                       emitter->InterfaceVersion < 8UL) ||
                      (fog && perspective) ||
                     (texture1State & ~RADEON3D_TEX_STATE_MASK) ||
                     minFilter1 >
                         RADEON3D_TEX_MIN_LINEAR_MIPMAP_LINEAR ||
                     (fogColor & 0xff000000UL))) ||
         (options & RADEON3D_DRAW_COMPACT_VERTEX &&
          (!hardwareTcl ||
           (vertexState & (RADEON3D_VERTEX_TEXTURE1 |
                           RADEON3D_VERTEX_FOG)))) ||
         (options & RADEON3D_DRAW_BILINEAR && !textured) ||
        (options & RADEON3D_DRAW_ALPHA_BLEND && !textured) ||
        (options & RADEON3D_DRAW_DEPTH_FUNC_MASK &&
         !(options & RADEON3D_DRAW_DEPTH_LESS)) ||
        (options & RADEON3D_DRAW_DEPTH_WRITE &&
         !(options & RADEON3D_DRAW_DEPTH_LESS)) ||
        (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_POINT_LIST
             ? !vertexCount
             : (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_LINE_LIST ||
                primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_LINE_STRIP ||
                primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_LINE_LOOP)
                   ? vertexCount < 2UL
                   : vertexCount < 3UL) ||
         (!emitter->CommitVbuf &&
          primitiveType != R200_CP_VC_CNTL_PRIM_TYPE_QUADS &&
          vertexCount > RADEON3D_IMMD_MAX_VERTICES) ||
        (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST &&
           vertexCount % 3UL) ||
        (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_LINE_LIST &&
         vertexCount % 2UL) ||
         (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_QUADS &&
          (vertexCount < 4UL ||
           (!emitter->CommitVbuf &&
            vertexCount > (extendedVertex
                                ? RADEON3D_IMMD_MAX_EXTENDED_QUAD_VERTICES
                                   : RADEON3D_IMMD_MAX_QUAD_VERTICES)) ||
           vertexCount % 4UL)) ||
        (emitter->CommitVbuf &&
         (!hardwareTcl || commitOffset >= emitter->CommitSegmentBytes ||
          !vertexStride ||
          vertexCount > (emitter->CommitSegmentBytes - commitOffset) /
                            (vertexStride * 4UL))) ||
        (emitter->CommitVbuf
             ? length != headerDwords
             : length != headerDwords + vertexCount * vertexStride) ||
        !ValidColorTarget(emitter, color) ||
        (useDepth ? !ValidDepthTarget(depth, color) : record[3] != 0) ||
        (textured ? (fragmentStatePresent
                          ? !ValidTextureTargetWithState(texture, color, depth,
                                                 textureOffset, textureWidth,
                                                 textureHeight, levels,
                                                 &textureBytes) ||
                               (minFilter >=
                                    RADEON3D_TEX_MIN_NEAREST_MIPMAP_NEAREST &&
                                levels == 1UL)
                         : !ValidTextureTarget(texture, color, depth))
                    : record[4] != 0 || (fragmentStatePresent &&
                          (textureOffset || record[12] || textureState))) ||
        (textured1
             ? !ValidTextureTargetWithState(texture1, color, depth,
                                     texture1Offset, texture1Width,
                                     texture1Height, levels1,
                                     &texture1Bytes) ||
                    (texture1 != texture &&
                     ExecuteSurfacesOverlap(texture1, texture)) ||
                   (minFilter1 >=
                        RADEON3D_TEX_MIN_NEAREST_MIPMAP_NEAREST &&
                    levels1 == 1UL)
             : extendedVertex && (record[15] || texture1Offset || record[17] ||
                           texture1State)) ||
        (!fog && extendedVertex && fogColor) ||
        !ValidExecuteScissor(color, record[6], record[7],
                              record[8], record[9])) {
        if (emitter->CommitVbuf) {
            /* Re-evaluate the terms individually so the reported stage says
             * which one rejected the draw; the combined predicate above is
             * too coarse to attribute a streaming failure. */
            ULONG detail = 22UL;

            if (!ValidColorTarget(emitter, color))
                detail = 60UL;
            else if (useDepth && !ValidDepthTarget(depth, color))
                detail = 61UL;
            else if (!useDepth && record[3])
                detail = 62UL;
            else if (textured && fragmentStatePresent &&
                     !ValidTextureTargetWithState(texture, color, depth,
                                                  textureOffset, textureWidth,
                                                  textureHeight, levels,
                                                  &textureBytes))
                detail = TextureRejectReason(texture, color,
                                             useDepth ? depth : NULL,
                                             textureOffset, textureWidth,
                                             textureHeight, levels);
            else if (textured && !fragmentStatePresent &&
                     !ValidTextureTarget(texture, color, depth))
                detail = 64UL;
            else if (textured1 &&
                     !ValidTextureTargetWithState(texture1, color, depth,
                                                  texture1Offset, texture1Width,
                                                  texture1Height, levels1,
                                                  &texture1Bytes))
                detail = 20UL + TextureRejectReason(texture1, color,
                                                    useDepth ? depth : NULL,
                                                    texture1Offset,
                                                    texture1Width,
                                                    texture1Height, levels1);
            else if (!ValidExecuteScissor(color, record[6], record[7],
                                          record[8], record[9]))
                detail = 66UL;
            else if (!hardwareTcl)
                detail = 67UL;
            else if (commitOffset >= emitter->CommitSegmentBytes)
                detail = 68UL;
            else if (!vertexStride)
                detail = 69UL;
            else if (vertexCount > (emitter->CommitSegmentBytes - commitOffset) /
                                       (vertexStride * 4UL))
                detail = 70UL;
            else if (length != headerDwords)
                detail = 71UL;
            emitter->FailStage = detail;
        }
        return FALSE;
    }
    if (textured && !fragmentStatePresent)
        textureBytes = (texture->Height - 1UL) * texture->Pitch +
                       texture->Width *
                           (texture->Format == RADEON3D_FORMAT_B8G8R8A8
                                ? 4UL : 2UL);
    if (color) {
        state->Color = *color;
        state->ColorValid = TRUE;
    } else {
        state->ColorValid = FALSE;
    }
    if (depth) {
        state->Depth = *depth;
        state->DepthValid = TRUE;
    } else {
        state->DepthValid = FALSE;
    }
    if (texture) {
        state->Texture = *texture;
        state->TextureValid = TRUE;
    } else {
        state->TextureValid = FALSE;
    }
    if (texture1) {
        state->Texture1 = *texture1;
        state->Texture1Valid = TRUE;
    } else {
        state->Texture1Valid = FALSE;
    }
    state->Options = options;
    state->Left = record[6];
    state->Top = record[7];
    state->Right = record[8];
    state->Bottom = record[9];
    state->ClearDepth = FALSE;
    state->FragmentStatePresent = fragmentStatePresent;
    state->ExtendedVertex = extendedVertex;
    state->HardwareTcl = hardwareTcl;
    state->TextureOffset = textureOffset;
    state->TextureWidth = textureWidth;
    state->TextureHeight = textureHeight;
    state->TextureState = textureState;
    state->FragmentState = fragmentState;
    state->TextureBytes = textureBytes;
    state->Texture1Offset = texture1Offset;
    state->Texture1Width = texture1Width;
    state->Texture1Height = texture1Height;
    state->Texture1State = texture1State;
    state->Texture1Bytes = texture1Bytes;
    state->VertexState = vertexState;
    state->FogColor = fogColor;
    state->TexGen = texGen;
    if (hardwareTcl) {
        ULONG index;
        for (index = 0; index < 16UL; ++index) {
            if (!ValidFloat(record[21UL + index]))
                return FALSE;
            state->ModelProjection[index] = record[21UL + index];
        }
        for (index = 0; index < 6UL; ++index) {
            if (!ValidFloat(record[37UL + index]))
                return FALSE;
            state->Viewport[index] = record[37UL + index];
        }
        state->TransformFlags = record[43];
    }
    if (texGen) {
        ULONG matrix, index;

        state->TexGenState[0] = record[44];
        state->TexGenState[1] = record[45];
        for (matrix = 0; matrix < 2UL; ++matrix)
            for (index = 0; index < 16UL; ++index) {
                ULONG value = record[46UL + matrix * 16UL + index];

                if (!ValidFloat(value) ||
                    (!(state->TexGenState[matrix] &
                       RADEON3D_TEXGEN_MODE_MASK) && value))
                    return FALSE;
                state->TexGenMatrix[matrix][index] = value;
            }
    }
    state->NormalVertex = normalVertex;
    state->Lighting = lighting;
    if (normalVertex) {
        ULONG index;

        for (index = 0; index < 16UL; ++index) {
            if (!ValidFloat(record[matrixBase + index]) ||
                !ValidFloat(record[matrixBase + 16UL + index]))
                return FALSE;
            state->ModelView[index] = record[matrixBase + index];
            state->InvModelView[index] =
                record[matrixBase + 16UL + index];
        }
    }
    if (lighting) {
        ULONG light, index, block = lightBlockBase;

        for (index = 0; index < 4UL; ++index) {
            ULONG globalAmbient = record[lightControlIndex - 8UL + index];
            ULONG eyeVector = record[lightControlIndex - 4UL + index];

            if (!ValidFloat(globalAmbient) || !ValidFloat(eyeVector))
                return FALSE;
            state->GlobalAmbient[index] = globalAmbient;
            state->EyeVector[index] = eyeVector;
        }
        state->LightControl = record[lightControlIndex];
        for (index = 0; index < RADEON3D_MATERIAL_DWORDS; ++index) {
            ULONG value = record[lightControlIndex + 1UL + index];

            if (!ValidFloat(value))
                return FALSE;
            state->Material[index] = value;
        }
        for (light = 0; light < 8UL; ++light) {
            if (!(enabledLights & (1UL << light)))
                continue;
            for (index = 0; index < RADEON3D_EXEC_LIGHT_BLOCK_DWORDS;
                 ++index) {
                ULONG value = record[block + index];

                if (!ValidFloat(value))
                    return FALSE;
                state->Lights[light][index] = value;
            }
            block += RADEON3D_EXEC_LIGHT_BLOCK_DWORDS;
        }
    }
    if (!EmitExecuteStateCached(emitter, state)) {
        if (emitter->CommitVbuf)
            emitter->FailStage = 23UL;
        return FALSE;
    }
    return EmitExecuteVertices(emitter, vertices, vertexCount, useDepth,
                               textured, fragmentStatePresent,
                               extendedVertex, hardwareTcl,
                               normalVertex, compactVertex,
                               vertexState,
                               primitiveType, color);
}
BOOL Radeon3DEmitVbufDraw(                                struct Radeon3DEmitter *emitter,
                                ULONG commitOffset, ULONG vertexCount,
                                ULONG primitiveType)
{
    const struct Radeon3DEmitState *state = &emitter->State;
    ULONG vertexStride;
    ULONG options;
    BOOL textured;
    BOOL useDepth;

    if (!emitter->CommitVbuf || !emitter->StateValid || !state->HardwareTcl)
        return FALSE;
    options = state->Options;
    vertexStride = (options & RADEON3D_DRAW_COMPACT_VERTEX)
        ? (state->NormalVertex ? 10UL : 7UL)
        : (state->NormalVertex ? RADEON3D_EXEC_HW_TCL_NORMAL_VERTEX_DWORDS
                               : RADEON3D_EXEC_HW_TCL_VERTEX_DWORDS);
    if ((primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_POINT_LIST
             ? !vertexCount
             : (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_LINE_LIST ||
                primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_LINE_STRIP ||
                primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_LINE_LOOP)
                   ? vertexCount < 2UL
                   : vertexCount < 3UL) ||
        (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST &&
         vertexCount % 3UL) ||
        (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_LINE_LIST &&
         vertexCount % 2UL) ||
        (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_QUADS &&
         (vertexCount < 4UL || vertexCount % 4UL)) ||
        commitOffset >= emitter->CommitSegmentBytes || !vertexStride ||
        vertexCount > (emitter->CommitSegmentBytes - commitOffset) /
                          (vertexStride * 4UL)) {
        emitter->FailStage = 72UL;
        return FALSE;
    }
    emitter->CommitVbufAddress = emitter->CommitSegmentGpuBase + commitOffset;
    textured = (options & RADEON3D_DRAW_TEXTURED) != 0;
    useDepth = (options & (RADEON3D_DRAW_DEPTH_LESS |
                           RADEON3D_DRAW_DEPTH_WRITE)) != 0;
    return EmitExecuteVertices(
        emitter, NULL, vertexCount, useDepth, textured,
        state->FragmentStatePresent, state->ExtendedVertex,
        state->HardwareTcl, state->NormalVertex,
        (options & RADEON3D_DRAW_COMPACT_VERTEX) != 0,
        state->VertexState, primitiveType,
        state->ColorValid ? &state->Color : NULL);
}

BOOL Radeon3DEmitReuseDraw(                                 struct Radeon3DEmitter *emitter,
                                 const ULONG *record, ULONG length,
                                 ULONG primitiveType)
{
    ULONG commitOffset;

    if (length != RADEON3D_EXEC_REUSE_DWORDS)
        return FALSE;
    commitOffset =
        emitter->CommitVertexOffsets[emitter->CommitDrawIndex++];
    return Radeon3DEmitVbufDraw(emitter, commitOffset, record[2],
                               primitiveType);
}
BOOL Radeon3DEmitPrimitiveType(ULONG opcode, ULONG *primitiveType)
{
    static const UBYTE primitives[] = {
        R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST,
        R200_CP_VC_CNTL_PRIM_TYPE_TRI_STRIP,
        R200_CP_VC_CNTL_PRIM_TYPE_TRI_FAN,
        R200_CP_VC_CNTL_PRIM_TYPE_QUADS,
        R200_CP_VC_CNTL_PRIM_TYPE_POINT_LIST,
        R200_CP_VC_CNTL_PRIM_TYPE_LINE_LIST,
        R200_CP_VC_CNTL_PRIM_TYPE_LINE_STRIP,
        R200_CP_VC_CNTL_PRIM_TYPE_LINE_LOOP
    };

    if (!primitiveType || opcode < RADEON3D_EXEC_DRAW_TRIANGLES ||
        opcode > RADEON3D_EXEC_DRAW_LINE_LOOP)
        return FALSE;
    *primitiveType = primitives[opcode - RADEON3D_EXEC_DRAW_TRIANGLES];
    return TRUE;
}
BOOL Radeon3DEmitStream(struct Radeon3DEmitter *emitter,
                        const ULONG *records, ULONG recordDwords)
{
    ULONG index = 0;

    if (emitter->InterfaceVersion < 2UL)
        return FALSE;
    while (index < recordDwords) {
        ULONG length;

        if (recordDwords - index < 2UL)
            return FALSE;
        length = records[index + 1UL];
        if (length < 2UL || length > recordDwords - index)
            return FALSE;
        if (records[index] == RADEON3D_EXEC_CLEAR) {
            BOOL commitVbuf = emitter->CommitVbuf;
            BOOL emitted;

            /* Clear builds its own projected rectangle; only draw records
             * consume client vertices from the streaming segment. */
            emitter->CommitVbuf = FALSE;
            emitted = Radeon3DEmitClear(emitter, records + index,
                                       length);
            emitter->CommitVbuf = commitVbuf;
            if (!emitted)
                return FALSE;
        } else if (records[index] == RADEON3D_EXEC_DRAW_TRIANGLES) {
            if (!Radeon3DEmitDraw(emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST))
                return FALSE;
        } else if (emitter->InterfaceVersion >= 7UL &&
                   records[index] == RADEON3D_EXEC_DRAW_TRI_STRIP) {
            if (!Radeon3DEmitDraw(emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_TRI_STRIP))
                return FALSE;
        } else if (emitter->InterfaceVersion >= 7UL &&
                   records[index] == RADEON3D_EXEC_DRAW_TRI_FAN) {
            if (!Radeon3DEmitDraw(emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_TRI_FAN))
                return FALSE;
        } else if (emitter->InterfaceVersion >= 8UL &&
                   records[index] == RADEON3D_EXEC_DRAW_QUADS) {
            if (!Radeon3DEmitDraw(emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_QUADS))
                return FALSE;
        } else if (emitter->InterfaceVersion >= 9UL &&
                   records[index] == RADEON3D_EXEC_DRAW_POINTS) {
            if (!Radeon3DEmitDraw(emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_POINT_LIST))
                return FALSE;
        } else if (emitter->InterfaceVersion >= 9UL &&
                   records[index] == RADEON3D_EXEC_DRAW_LINES) {
            if (!Radeon3DEmitDraw(emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_LINE_LIST))
                return FALSE;
        } else if (emitter->InterfaceVersion >= 9UL &&
                   records[index] == RADEON3D_EXEC_DRAW_LINE_STRIP) {
            if (!Radeon3DEmitDraw(emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_LINE_STRIP))
                return FALSE;
        } else if (emitter->InterfaceVersion >= 9UL &&
                   records[index] == RADEON3D_EXEC_DRAW_LINE_LOOP) {
            if (!Radeon3DEmitDraw(emitter, records + index, length,
                                  R200_CP_VC_CNTL_PRIM_TYPE_LINE_LOOP))
                return FALSE;
        } else if (emitter->InterfaceVersion >= 14UL && emitter->CommitVbuf &&
                   records[index] >= RADEON3D_EXEC_REUSE_TRIANGLES &&
                   records[index] <= RADEON3D_EXEC_REUSE_LINE_LOOP) {
            ULONG primitiveType;

            if (!Radeon3DEmitPrimitiveType(
                    records[index] - RADEON3D_EXEC_REUSE_TRIANGLES +
                        RADEON3D_EXEC_DRAW_TRIANGLES,
                    &primitiveType))
                return FALSE;
            if (!Radeon3DEmitReuseDraw(
                    emitter,  records + index, length,
                    primitiveType))
                return FALSE;
        } else
            return FALSE;
        index += length;
    }
    return index == recordDwords && emitter->Count;
}
