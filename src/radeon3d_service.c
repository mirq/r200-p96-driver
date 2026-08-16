#include <exec/memory.h>
#include <proto/exec.h>

#include "radeon9200.h"
#include "radeon_regs.h"

#define RADEON3D_SESSION_MAGIC 0x52334453UL
#define RADEON3D_EXEC_SUPPRESS_COLOR_WRITE 0x80000000UL

struct Radeon3DDevice {
    struct MinNode Node;
    ULONG Magic;
    ULONG Generation;
    ULONG InterfaceVersion;
    struct RadeonChipBase *Base;
    struct Library *OwnerBase;
    struct Radeon3DDevice *Handle;
    ULONG ActiveCalls;
    ULONG LastFence;
    UBYTE Closing;
    UBYTE CleanupDone;
    UBYTE CleanupReady;
    ULONG *ExecuteTrusted;
    ULONG *ExecuteGenerated;
    struct MinList Surfaces;
};

struct Radeon3DSurfaceHandle {
    struct MinNode Node;
    struct Radeon3DDevice *Device;
    struct BitMap *BitMap;
    APTR CpuAddress;
    ULONG GpuAddress;
    ULONG Pitch;
    ULONG Width;
    ULONG Height;
    ULONG Format;
};

static struct Radeon3DDevice *FindDevice(
    struct RadeonChipBase *base, struct Radeon3DDevice *candidate)
{
    struct MinNode *node;

    for (node = base->ServiceDevices.mlh_Head; node->mln_Succ;
         node = node->mln_Succ) {
        struct Radeon3DDevice *device = (struct Radeon3DDevice *)node;

        if (device->Handle == candidate)
            return device;
    }
    return NULL;
}

static struct Radeon3DDevice *FindActiveDevice(
    struct RadeonChipBase *base, struct Radeon3DDevice *candidate)
{
    struct MinNode *node;

    for (node = base->ServiceDevices.mlh_Head; node->mln_Succ;
         node = node->mln_Succ) {
        if ((struct Radeon3DDevice *)node == candidate)
            return candidate;
    }
    return NULL;
}

static void AddServiceDevice(struct RadeonChipBase *base,
                             struct Radeon3DDevice *device)
{
    struct MinNode *tail = (struct MinNode *)&base->ServiceDevices.mlh_Tail;
    struct MinNode *previous = base->ServiceDevices.mlh_TailPred;

    device->Node.mln_Succ = tail;
    device->Node.mln_Pred = previous;
    previous->mln_Succ = &device->Node;
    base->ServiceDevices.mlh_TailPred = &device->Node;
}

static void AddRetiredDevice(struct RadeonChipBase *base,
                             struct Radeon3DDevice *device)
{
    struct MinNode *tail =
        (struct MinNode *)&base->RetiredServiceDevices.mlh_Tail;
    struct MinNode *previous = base->RetiredServiceDevices.mlh_TailPred;

    device->Node.mln_Succ = tail;
    device->Node.mln_Pred = previous;
    previous->mln_Succ = &device->Node;
    base->RetiredServiceDevices.mlh_TailPred = &device->Node;
}

static void RemoveServiceDevice(struct Radeon3DDevice *device)
{
    device->Node.mln_Pred->mln_Succ = device->Node.mln_Succ;
    device->Node.mln_Succ->mln_Pred = device->Node.mln_Pred;
    device->Node.mln_Succ = NULL;
    device->Node.mln_Pred = NULL;
}

static void ReapRetiredDevicesLocked(struct RadeonChipBase *base)
{
    struct ExecBase *SysBase = base->ExecBase;
    struct MinNode *node = base->RetiredServiceDevices.mlh_Head;

    while (node->mln_Succ) {
        struct Radeon3DDevice *device = (struct Radeon3DDevice *)node;

        node = node->mln_Succ;
        if (device->CleanupReady) {
            RemoveServiceDevice(device);
            FreeMem(device, sizeof(*device));
        }
    }
}

static void AddSurfaceHandle(struct Radeon3DDevice *device,
                             struct Radeon3DSurfaceHandle *surface)
{
    struct MinNode *tail = (struct MinNode *)&device->Surfaces.mlh_Tail;
    struct MinNode *previous = device->Surfaces.mlh_TailPred;

    surface->Node.mln_Succ = tail;
    surface->Node.mln_Pred = previous;
    previous->mln_Succ = &surface->Node;
    device->Surfaces.mlh_TailPred = &surface->Node;
}

static struct Radeon3DSurfaceHandle *FindSurfaceHandle(
    struct Radeon3DDevice *device, APTR candidate)
{
    struct MinNode *node;

    for (node = device->Surfaces.mlh_Head; node->mln_Succ;
         node = node->mln_Succ) {
        if ((APTR)node == candidate)
            return (struct Radeon3DSurfaceHandle *)node;
    }
    return NULL;
}

static void RemoveSurfaceHandle(struct Radeon3DSurfaceHandle *surface)
{
    surface->Node.mln_Pred->mln_Succ = surface->Node.mln_Succ;
    surface->Node.mln_Succ->mln_Pred = surface->Node.mln_Pred;
    surface->Node.mln_Succ = NULL;
    surface->Node.mln_Pred = NULL;
}

static void FreeDeviceSurfaces(struct RadeonChipBase *base,
                               struct Radeon3DDevice *device)
{
    struct ExecBase *SysBase = base->ExecBase;
    struct Radeon3DSurfaceHandle *surface;

    while (device->Surfaces.mlh_Head->mln_Succ) {
        surface = (struct Radeon3DSurfaceHandle *)device->Surfaces.mlh_Head;
        RemoveSurfaceHandle(surface);
        FreeMem(surface, sizeof(*surface));
    }
    if (device->ExecuteTrusted) {
        FreeMem(device->ExecuteTrusted,
                RADEON3D_MAX_BATCH_DWORDS * sizeof(ULONG));
        device->ExecuteTrusted = NULL;
    }
    if (device->ExecuteGenerated) {
        FreeMem(device->ExecuteGenerated,
                RADEON3D_MAX_BATCH_DWORDS * sizeof(ULONG));
        device->ExecuteGenerated = NULL;
    }
}

static void FillInfo(struct RadeonChipBase *base, struct Radeon3DInfo *info,
                     ULONG interfaceVersion)
{
    struct BoardInfo *bi = base->BoardInfo;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    info->Size = RADEON3D_INFO_V1_SIZE;
    info->Version = interfaceVersion;
    info->Generation = base->ServiceGeneration;
    info->DeviceId = data ? data->DeviceId : 0;
    info->Caps = RADEON3D_CAP_SINGLE_BOARD |
                 RADEON3D_CAP_OWNER_PINNED |
                 RADEON3D_CAP_PACKET2_SUBMIT |
                 RADEON3D_CAP_FENCES |
                 RADEON3D_CAP_BITMAP_IMPORT |
                 RADEON3D_CAP_IMMD_TRI_LIST;
    if (interfaceVersion >= 2UL)
        info->Caps |= RADEON3D_CAP_PHASE2_EXECUTE;
    if (interfaceVersion >= 3UL)
        info->Caps |= RADEON3D_CAP_PHASE4_DEPTH_FUNCS;
    if (interfaceVersion >= 4UL)
        info->Caps |= RADEON3D_CAP_PHASE5_TEXTURE_STATE;
    if (interfaceVersion >= 5UL)
        info->Caps |= RADEON3D_CAP_PHASE6_FOG_MULTITEX |
                      RADEON3D_CAP_TEST_INVALIDATE;
    if (interfaceVersion >= 6UL)
        info->Caps |= RADEON3D_CAP_COLOR_TARGET_FORMATS;
    if (interfaceVersion >= 7UL)
        info->Caps |= RADEON3D_CAP_NATIVE_TRI_PRIMITIVES;
    if (RadeonCpIsReady(bi))
        info->Caps |= RADEON3D_CAP_CP_READY;
    info->InstalledVram = data ? data->InstalledVram : 0;
    info->Picasso96Vram = bi ? bi->MemorySize : 0;
    info->MaxBatchDwords = RADEON3D_MAX_BATCH_DWORDS;
}

static BOOL IsUsableDevice(struct RadeonChipBase *base,
                           struct Radeon3DDevice *device)
{
    return device && device->Magic == RADEON3D_SESSION_MAGIC &&
           device->Base == base &&
           device->Generation == base->ServiceGeneration &&
           base->ServiceState == RADEON3D_SERVICE_READY &&
           base->BoardInfo && RadeonCpIsReady(base->BoardInfo);
}

static void CloseOwnerPin(struct RadeonChipBase *base,
                          struct BoardInfo *bi,
                          struct Library *ownerPin)
{
    struct ExecBase *SysBase = base->ExecBase;
    BOOL ownerExpunge = ownerPin && ownerPin->lib_OpenCnt == 1 &&
                        (ownerPin->lib_Flags & LIBF_DELEXP);

    if (ownerExpunge)
        (void)RadeonReleaseBoard(base, bi, FALSE);
    if (ownerPin)
        CloseLibrary(ownerPin);
}

static void FinishDeviceCleanup(struct RadeonChipBase *base,
                                struct BoardInfo *bi,
                                struct Radeon3DDevice *device,
                                BOOL cleanup,
                                struct Library *ownerPin)
{
    struct ExecBase *SysBase = base->ExecBase;

    if (cleanup) {
        FreeDeviceSurfaces(base, device);
        ObtainSemaphore(&base->ServiceLock);
        device->CleanupReady = TRUE;
        if (base->ServiceSessions)
            --base->ServiceSessions;
        ReleaseSemaphore(&base->ServiceLock);
    }

    /* Dropping the owner pin can release the board, so it must be last. */
    CloseOwnerPin(base, bi, ownerPin);
}

static struct BoardInfo *LockServiceBoard(
    struct RadeonChipBase *base, struct Radeon3DDevice **devicePtr)
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct Radeon3DDevice *device;
    struct BoardInfo *bi = NULL;
    struct Library *ownerPin = NULL;
    BOOL cleanup = FALSE;

    if (!SysBase || !devicePtr || !*devicePtr)
        return NULL;
    ObtainSemaphore(&base->ServiceLock);
    device = FindDevice(base, *devicePtr);
    if (IsUsableDevice(base, device)) {
        ++device->ActiveCalls;
        *devicePtr = device;
        bi = base->BoardInfo;
    }
    ReleaseSemaphore(&base->ServiceLock);
    if (!bi)
        return NULL;

    ObtainSemaphore(&bi->BoardLock);
    ObtainSemaphore(&base->ServiceLock);
    if (!FindActiveDevice(base, device) ||
        !IsUsableDevice(base, device) || base->BoardInfo != bi) {
        if (device->ActiveCalls)
            --device->ActiveCalls;
        if (device->Closing && !device->ActiveCalls) {
            ownerPin = device->OwnerBase;
            device->OwnerBase = NULL;
            if (!device->CleanupDone) {
                device->CleanupDone = TRUE;
                cleanup = TRUE;
            }
        }
        ReleaseSemaphore(&base->ServiceLock);
        ReleaseSemaphore(&bi->BoardLock);
        FinishDeviceCleanup(base, bi, device, cleanup, ownerPin);
        return NULL;
    }
    ReleaseSemaphore(&base->ServiceLock);
    return bi;
}

static void UnlockServiceBoard(struct RadeonChipBase *base,
                               struct BoardInfo *bi,
                               struct Radeon3DDevice *device)
{
    struct ExecBase *SysBase = base->ExecBase;
    struct Library *ownerPin = NULL;
    BOOL cleanup = FALSE;

    ReleaseSemaphore(&bi->BoardLock);
    ObtainSemaphore(&base->ServiceLock);
    if (device->ActiveCalls)
        --device->ActiveCalls;
    if (device->Closing && !device->ActiveCalls) {
        ownerPin = device->OwnerBase;
        device->OwnerBase = NULL;
        if (!device->CleanupDone) {
            device->CleanupDone = TRUE;
            cleanup = TRUE;
        }
    }
    ReleaseSemaphore(&base->ServiceLock);
    FinishDeviceCleanup(base, bi, device, cleanup, ownerPin);
}

static BOOL MatchRegister(const ULONG *commands, ULONG commandCount,
                          ULONG *index, ULONG reg, ULONG value)
{
    if (*index > commandCount - 2UL ||
        commands[*index] != RADEON_CP_PACKET0(reg, 0) ||
        commands[*index + 1UL] != value)
        return FALSE;
    *index += 2UL;
    return TRUE;
}

static struct Radeon3DSurfaceHandle *FindTriangleTarget(
    struct Radeon3DDevice *device, ULONG gpuAddress)
{
    struct MinNode *node;

    for (node = device->Surfaces.mlh_Head; node->mln_Succ;
         node = node->mln_Succ) {
        struct Radeon3DSurfaceHandle *surface =
            (struct Radeon3DSurfaceHandle *)node;

        if (surface->GpuAddress == gpuAddress &&
            !(surface->GpuAddress & ~R200_COLOROFFSET_MASK) &&
            surface->Width <= 65536UL && surface->Height <= 65536UL &&
            surface->Format == RADEON3D_FORMAT_R5G6B5PC &&
            (surface->Pitch / 2UL) <= R200_COLORPITCH_MASK &&
            !((surface->Pitch / 2UL) & ~R200_COLORPITCH_MASK))
            return surface;
    }
    return NULL;
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

static BOOL ValidScreenCoordinate(ULONG bits, ULONG limit)
{
    return !(bits & 0x80000000UL) &&
           (bits & 0x7f800000UL) != 0x7f800000UL &&
           bits <= UnsignedFloatBits(limit);
}

static BOOL ValidateTriangleBatch(struct Radeon3DDevice *device,
                                  const ULONG *commands,
                                  ULONG commandCount)
{
    struct Radeon3DSurfaceHandle *target;
    ULONG seControl = R200_BFACE_SOLID | R200_FFACE_SOLID |
                      R200_DIFFUSE_SHADE_GOURAUD |
                      R200_VTX_PIX_CENTER_OGL |
                      R200_ROUND_MODE_ROUND |
                      R200_ROUND_PREC_4TH_PIX;
    ULONG index = 0;
    ULONG vertex;
    ULONG vertexCount;
    ULONG vertexDwords;
    ULONG widthHeight;

    if (commandCount < 46UL)
        return FALSE;
    vertexDwords = commandCount - 46UL;
    if (vertexDwords % RADEON3D_IMMD_VERTEX_DWORDS)
        return FALSE;
    vertexCount = vertexDwords / RADEON3D_IMMD_VERTEX_DWORDS;
    if (vertexCount < 3UL || vertexCount > RADEON3D_IMMD_MAX_VERTICES ||
        vertexCount % 3UL)
        return FALSE;

    if (!MatchRegister(commands, commandCount, &index,
                       R200_SE_VAP_CNTL_STATUS, 0) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_SE_VAP_CNTL,
                       R200_VAP_FORCE_W_TO_ONE |
                           (9UL << R200_VAP_VF_MAX_VTX_NUM_SHIFT)) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_SE_VTX_STATE_CNTL, 0) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_SE_VTE_CNTL, 0) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_SE_VTX_FMT_0,
                       R200_VTX_PK_RGBA << R200_VTX_COLOR_0_SHIFT) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_SE_VTX_FMT_1, 0) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_SE_CNTL, seControl) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_PP_CNTL, R200_TEX_BLEND_0_ENABLE) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_PP_TXCBLEND_0,
                       R200_TXC_ARG_C_DIFFUSE_COLOR) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_PP_TXCBLEND2_0,
                       R200_TXC_CLAMP_0_1 | R200_TXC_OUTPUT_REG_R0) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_PP_TXABLEND_0,
                       R200_TXA_ARG_C_DIFFUSE_ALPHA) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_PP_TXABLEND2_0,
                       R200_TXA_CLAMP_0_1 | R200_TXA_OUTPUT_REG_R0) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_PP_CNTL_X, 0) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_RE_AUX_SCISSOR_CNTL, 0) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_RE_CNTL, 0) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_RE_TOP_LEFT, 0))
        return FALSE;
    if (index > commandCount - 2UL ||
        commands[index] != RADEON_CP_PACKET0(R200_RE_WIDTH_HEIGHT, 0))
        return FALSE;
    widthHeight = commands[index + 1UL];
    index += 2UL;
    if (
        !MatchRegister(commands, commandCount, &index,
                       R200_RB3D_PLANEMASK, 0xffffffffUL) ||
        !MatchRegister(commands, commandCount, &index,
                       R200_RB3D_BLENDCNTL,
                       R200_SRC_BLEND_GL_ONE | R200_DST_BLEND_GL_ZERO) ||
        !MatchRegister(commands, commandCount, &index,
                       RADEON_RB3D_CNTL, R200_COLOR_FORMAT_RGB565))
        return FALSE;

    if (index > commandCount - 4UL ||
        commands[index] != RADEON_CP_PACKET0(R200_RB3D_COLOROFFSET, 0))
        return FALSE;
    target = FindTriangleTarget(device, commands[index + 1UL]);
    if (!target || widthHeight !=
            (((target->Height - 1UL) << 16) | (target->Width - 1UL)))
        return FALSE;
    index += 2UL;
    if (!MatchRegister(commands, commandCount, &index,
                       R200_RB3D_COLORPITCH, target->Pitch / 2UL) ||
        index > commandCount - 11UL ||
        commands[index++] !=
            RADEON_CP_PACKET3(R200_CP_CMD_3D_DRAW_IMMD_2, vertexDwords) ||
        commands[index++] !=
            ((vertexCount << 16) | R200_CP_VC_CNTL_PRIM_WALK_RING |
              R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST))
        return FALSE;
    for (vertex = 0; vertex < vertexCount; ++vertex) {
        if (!ValidScreenCoordinate(commands[index], target->Width) ||
            !ValidScreenCoordinate(commands[index + 1UL], target->Height))
            return FALSE;
        index += RADEON3D_IMMD_VERTEX_DWORDS;
    }
    return index == commandCount;
}

static BOOL ValidateBatch(struct Radeon3DDevice *device,
                          const ULONG *commands, ULONG commandCount)
{
    ULONG index;

    if (!commands || !commandCount ||
        commandCount > RADEON3D_MAX_BATCH_DWORDS)
        return FALSE;
    for (index = 0; index < commandCount; ++index) {
        if (commands[index] != 0x80000000UL)
            return ValidateTriangleBatch(device, commands, commandCount);
    }
    return TRUE;
}

struct Radeon3DExecuteEmitter {
    ULONG *Words;
    ULONG Count;
};

struct Radeon3DExecuteState {
    struct Radeon3DSurfaceHandle *Color;
    struct Radeon3DSurfaceHandle *Depth;
    struct Radeon3DSurfaceHandle *Texture;
    struct Radeon3DSurfaceHandle *Texture1;
    ULONG Options;
    ULONG Left;
    ULONG Top;
    ULONG Right;
    ULONG Bottom;
    BOOL ClearDepth;
    BOOL StateV4;
    BOOL StateV5;
    ULONG TextureOffset;
    ULONG TextureWidth;
    ULONG TextureHeight;
    ULONG TextureState;
    ULONG FragmentState;
    ULONG TextureBytes;
    ULONG Texture1Offset;
    ULONG Texture1Width;
    ULONG Texture1Height;
    ULONG Texture1State;
    ULONG Texture1Bytes;
    ULONG Phase6State;
    ULONG FogColor;
};

static BOOL ExecuteEmitWord(struct Radeon3DExecuteEmitter *emitter,
                            ULONG value)
{
    if (emitter->Count >= RADEON3D_MAX_BATCH_DWORDS)
        return FALSE;
    emitter->Words[emitter->Count++] = value;
    return TRUE;
}

static BOOL ExecuteEmitRegister(struct Radeon3DExecuteEmitter *emitter,
                                ULONG reg, ULONG value)
{
    return ExecuteEmitWord(emitter, RADEON_CP_PACKET0(reg, 0)) &&
           ExecuteEmitWord(emitter, value);
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

static struct Radeon3DSurfaceHandle *ExecuteSurface(
    struct Radeon3DDevice *device, ULONG handleValue)
{
    return FindSurfaceHandle(device, (APTR)handleValue);
}

static ULONG SurfaceBytesPerPixel(const struct Radeon3DSurfaceHandle *surface)
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

static ULONG ColorTargetControl(const struct Radeon3DSurfaceHandle *surface)
{
    if (surface->Format == RADEON3D_FORMAT_CLUT8)
        return R200_COLOR_FORMAT_RGB332 | R200_DITHER_ENABLE;
    if (surface->Format == RADEON3D_FORMAT_B8G8R8A8)
        return R200_COLOR_FORMAT_ARGB8888;
    return R200_COLOR_FORMAT_RGB565;
}

static BOOL ValidColorTarget(struct Radeon3DDevice *device,
                             struct Radeon3DSurfaceHandle *surface)
{
    ULONG bytesPerPixel = SurfaceBytesPerPixel(surface);

    return surface && bytesPerPixel &&
           (surface->Format == RADEON3D_FORMAT_R5G6B5PC ||
            device->InterfaceVersion >= 6UL) &&
           !(surface->GpuAddress & ~R200_COLOROFFSET_MASK) &&
           surface->Width && surface->Width <= 65536UL &&
           surface->Height && surface->Height <= 65536UL &&
           !(surface->Pitch % bytesPerPixel) &&
           !((surface->Pitch / bytesPerPixel) & ~R200_COLORPITCH_MASK);
}

static BOOL ExecuteSurfacesOverlap(struct Radeon3DSurfaceHandle *first,
                                   struct Radeon3DSurfaceHandle *second)
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

static BOOL ValidDepthTarget(struct Radeon3DSurfaceHandle *surface,
                             struct Radeon3DSurfaceHandle *color)
{
    return surface && !ExecuteSurfacesOverlap(surface, color) &&
           surface->Format == RADEON3D_FORMAT_R5G6B5PC &&
           surface->Width == color->Width &&
           surface->Height == color->Height &&
           !(surface->GpuAddress & 0x0fUL) &&
           !(surface->Pitch & 1UL) &&
           !((surface->Pitch / 2UL) & ~R200_DEPTHPITCH_MASK);
}

static BOOL ValidTextureTarget(struct Radeon3DSurfaceHandle *surface,
                               struct Radeon3DSurfaceHandle *color,
                               struct Radeon3DSurfaceHandle *depth)
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

static BOOL ValidTextureTargetV4(struct Radeon3DSurfaceHandle *surface,
                                 struct Radeon3DSurfaceHandle *color,
                                 struct Radeon3DSurfaceHandle *depth,
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

static BOOL EmitExecuteTexture(struct Radeon3DExecuteEmitter *emitter,
                               struct Radeon3DSurfaceHandle *texture,
                               ULONG unit, ULONG options, BOOL stateV4,
                               ULONG textureOffset, ULONG textureWidth,
                               ULONG textureHeight, ULONG textureState,
                               ULONG textureBytes)
{
    ULONG filterReg = unit ? R200_PP_TXFILTER_1 : R200_PP_TXFILTER_0;
    ULONG formatReg = unit ? R200_PP_TXFORMAT_1 : R200_PP_TXFORMAT_0;
    ULONG formatXReg = unit ? R200_PP_TXFORMAT_X_1 : R200_PP_TXFORMAT_X_0;
    ULONG sizeReg = unit ? R200_PP_TXSIZE_1 : R200_PP_TXSIZE_0;
    ULONG pitchReg = unit ? R200_PP_TXPITCH_1 : R200_PP_TXPITCH_0;
    ULONG multiReg = unit ? R200_PP_TXMULTI_CTL_1 : R200_PP_TXMULTI_CTL_0;
    ULONG offsetReg = unit ? R200_PP_TXOFFSET_1 : R200_PP_TXOFFSET_0;
    ULONG textureFormat = R200_TXFORMAT_NON_POWER2;
    ULONG filter = R200_CLAMP_S_CLAMP_LAST | R200_CLAMP_T_CLAMP_LAST;
    ULONG textureSize = (texture->Width - 1UL) |
                        ((texture->Height - 1UL) << 16);
    ULONG texturePitch = texture->Pitch - 32UL;
    ULONG textureAddress = texture->GpuAddress;
    volatile UBYTE *textureEnd;

    if (stateV4) {
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
        if (!(textureState & RADEON3D_TEX_REPEAT_S))
            filter |= R200_CLAMP_S_CLAMP_LAST;
        if (!(textureState & RADEON3D_TEX_REPEAT_T))
            filter |= R200_CLAMP_T_CLAMP_LAST;
        textureAddress += textureOffset;
        textureSize = (textureWidth - 1UL) |
                      ((textureHeight - 1UL) << 16);
        if (levels > 1UL) {
            for (scan = textureWidth; scan > 1UL; scan >>= 1) ++logWidth;
            for (scan = textureHeight; scan > 1UL; scan >>= 1) ++logHeight;
            textureFormat = (logWidth << R200_TXFORMAT_WIDTH_SHIFT) |
                            (logHeight << R200_TXFORMAT_HEIGHT_SHIFT);
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
           ExecuteEmitRegister(emitter, formatXReg, 0) &&
           ExecuteEmitRegister(emitter, sizeReg, textureSize) &&
           ExecuteEmitRegister(emitter, pitchReg, texturePitch) &&
           ExecuteEmitRegister(emitter, multiReg, 0) &&
           ExecuteEmitRegister(emitter, offsetReg, textureAddress);
}

static BOOL ValidExecuteScissor(struct Radeon3DSurfaceHandle *target,
                                ULONG left, ULONG top,
                                ULONG right, ULONG bottom)
{
    return left < right && top < bottom &&
           right <= target->Width && bottom <= target->Height;
}

static BOOL EmitExecuteState(struct Radeon3DExecuteEmitter *emitter,
                             const struct Radeon3DExecuteState *state)
{
    struct Radeon3DSurfaceHandle *color = state->Color;
    struct Radeon3DSurfaceHandle *depth = state->Depth;
    struct Radeon3DSurfaceHandle *texture = state->Texture;
    struct Radeon3DSurfaceHandle *texture1 = state->Texture1;
    ULONG options = state->Options;
    ULONG left = state->Left;
    ULONG top = state->Top;
    ULONG right = state->Right;
    ULONG bottom = state->Bottom;
    BOOL clearDepth = state->ClearDepth;
    BOOL stateV4 = state->StateV4;
    BOOL stateV5 = state->StateV5;
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
    ULONG phase6State = state->Phase6State;
    ULONG fogColor = state->FogColor;
    ULONG seControl = R200_BFACE_SOLID | R200_FFACE_SOLID |
                      R200_DIFFUSE_SHADE_GOURAUD |
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
    BOOL textured1 = stateV5 &&
                     (phase6State & RADEON3D_PHASE6_TEXTURE1) != 0;
    BOOL fog = stateV5 && (phase6State & RADEON3D_PHASE6_FOG) != 0;
    BOOL useDepth = depth != NULL;
    ULONG depthFunc = (options & RADEON3D_DRAW_DEPTH_FUNC_MASK) >>
                      RADEON3D_DRAW_DEPTH_FUNC_SHIFT;

    if (useDepth)
        format0 |= R200_VTX_Z0;
    if (textured) {
        format0 &= ~(3UL << R200_VTX_COLOR_0_SHIFT);
        if (stateV4)
            format0 |= R200_VTX_PK_RGBA << R200_VTX_COLOR_0_SHIFT;
        format1 = 2UL << R200_VTX_TEX0_COMP_CNT_SHIFT;
        ppControl |= R200_TEX_0_ENABLE;
    }
    if (textured1) {
        format1 |= 2UL << R200_VTX_TEX1_COMP_CNT_SHIFT;
        ppControl |= R200_TEX_1_ENABLE | R200_TEX_BLEND_1_ENABLE;
    }
    if (fog) {
        format0 |= R200_VTX_DISCRETE_FOG;
        ppControl |= R200_FOG_ENABLE;
        seControl |= R200_FOG_SHADE_GOURAUD |
                     R200_DISC_FOG_SHADE_GOURAUD;
    }
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
    if (stateV4 && (fragmentState & RADEON3D_FRAGMENT_ALPHA_TEST)) {
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
    if (stateV4 && (fragmentState & RADEON3D_FRAGMENT_BLEND)) {
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

    if (!ExecuteEmitRegister(emitter, R200_SE_VAP_CNTL_STATUS, 0) ||
        !ExecuteEmitRegister(emitter, R200_SE_VAP_CNTL,
                             R200_VAP_FORCE_W_TO_ONE |
                                 (9UL << R200_VAP_VF_MAX_VTX_NUM_SHIFT)) ||
        !ExecuteEmitRegister(emitter, R200_SE_VTX_STATE_CNTL, 0) ||
        !ExecuteEmitRegister(emitter, R200_SE_VTE_CNTL, 0) ||
        !ExecuteEmitRegister(emitter, R200_SE_VTX_FMT_0, format0) ||
        !ExecuteEmitRegister(emitter, R200_SE_VTX_FMT_1, format1) ||
        !ExecuteEmitRegister(emitter, R200_SE_CNTL, seControl) ||
        !ExecuteEmitRegister(emitter, R200_PP_MISC, ppMisc) ||
        !ExecuteEmitRegister(emitter, R200_PP_CNTL, ppControl) ||
        !ExecuteEmitRegister(emitter, R200_PP_TXCBLEND_0,
                              textured && stateV4 &&
                                      (textureState & RADEON3D_TEX_MODULATE)
                                  ? R200_TXC_ARG_A_R0_COLOR |
                                        R200_TXC_ARG_B_DIFFUSE_COLOR
                              : textured ? R200_TXC_ARG_C_R0_COLOR
                                       : R200_TXC_ARG_C_DIFFUSE_COLOR) ||
        !ExecuteEmitRegister(emitter, R200_PP_TXCBLEND2_0,
                             R200_TXC_CLAMP_0_1 |
                                 R200_TXC_OUTPUT_REG_R0) ||
        !ExecuteEmitRegister(emitter, R200_PP_TXABLEND_0,
                              textured && stateV4 &&
                                      (textureState & RADEON3D_TEX_MODULATE)
                                  ? R200_TXA_ARG_A_R0_ALPHA |
                                        R200_TXA_ARG_B_DIFFUSE_ALPHA
                              : textured ? R200_TXA_ARG_C_R0_ALPHA
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
                                (texture1State & RADEON3D_TEX_MODULATE)
                                    ? R200_TXA_ARG_A_R1_ALPHA |
                                          R200_TXA_ARG_B_R0_ALPHA
                                    : R200_TXA_ARG_C_R1_ALPHA) ||
          !ExecuteEmitRegister(emitter, R200_PP_TXABLEND2_1,
                               R200_TXA_CLAMP_0_1 |
                                   R200_TXA_OUTPUT_REG_R0))) ||
        !ExecuteEmitRegister(emitter, R200_PP_CNTL_X, 0))
        return FALSE;

    if (textured &&
        !EmitExecuteTexture(emitter, texture, 0, options, stateV4,
                            textureOffset, textureWidth, textureHeight,
                            textureState, textureBytes))
        return FALSE;
    if (textured1 &&
        !EmitExecuteTexture(emitter, texture1, 1, options, TRUE,
                            texture1Offset, texture1Width, texture1Height,
                            texture1State, texture1Bytes))
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

    return ExecuteEmitRegister(emitter, R200_RE_AUX_SCISSOR_CNTL, 0) &&
           ExecuteEmitRegister(emitter, R200_RE_CNTL,
                               R200_SCISSOR_ENABLE) &&
           ExecuteEmitRegister(emitter, R200_RE_TOP_LEFT,
                               left | (top << 16)) &&
           ExecuteEmitRegister(emitter, R200_RE_WIDTH_HEIGHT,
                               (right - 1UL) | ((bottom - 1UL) << 16)) &&
            ExecuteEmitRegister(emitter, R200_RB3D_PLANEMASK,
                                (options & RADEON3D_EXEC_SUPPRESS_COLOR_WRITE)
                                    ? 0UL : 0xffffffffUL) &&
           ExecuteEmitRegister(emitter, R200_RB3D_BLENDCNTL,
                               blendControl) &&
           ExecuteEmitRegister(emitter, RADEON_RB3D_CNTL, rbControl) &&
           ExecuteEmitRegister(emitter, R200_RB3D_COLOROFFSET,
                               color->GpuAddress) &&
           ExecuteEmitRegister(emitter, R200_RB3D_COLORPITCH,
                               color->Pitch / SurfaceBytesPerPixel(color));
}

static BOOL EmitExecuteVertices(struct Radeon3DExecuteEmitter *emitter,
                                 const ULONG *vertices, ULONG vertexCount,
                                 BOOL useDepth, BOOL textured, BOOL stateV4,
                                 BOOL stateV5, ULONG phase6State,
                                 ULONG primitiveType)
{
    ULONG dwordsPerVertex = 3UL + (useDepth ? 1UL : 0UL) +
                              (textured ? (stateV4 ? 2UL : 1UL) : 0UL) +
                              ((stateV5 &&
                                (phase6State & RADEON3D_PHASE6_FOG))
                                   ? 1UL : 0UL) +
                             ((stateV5 &&
                               (phase6State & RADEON3D_PHASE6_TEXTURE1))
                                  ? 2UL : 0UL);
    ULONG vertexDwords = vertexCount * dwordsPerVertex;
    ULONG vertex;

    if (!ExecuteEmitWord(emitter,
                         RADEON_CP_PACKET3(R200_CP_CMD_3D_DRAW_IMMD_2,
                                           vertexDwords)) ||
        !ExecuteEmitWord(emitter,
                          (vertexCount << 16) |
                              R200_CP_VC_CNTL_PRIM_WALK_RING |
                              primitiveType))
        return FALSE;
    for (vertex = 0; vertex < vertexCount; ++vertex) {
        const ULONG *input = vertices + vertex *
            (stateV5 ? RADEON3D_EXEC_V5_VERTEX_DWORDS
                     : RADEON3D_EXEC_VERTEX_DWORDS);

        if (!ExecuteEmitWord(emitter, input[0]) ||
            !ExecuteEmitWord(emitter, input[1]))
            return FALSE;
        if (useDepth && !ExecuteEmitWord(emitter, input[2]))
            return FALSE;
        if (stateV5 && (phase6State & RADEON3D_PHASE6_FOG) &&
            !ExecuteEmitWord(emitter, input[8]))
            return FALSE;
        if (stateV5 && !ExecuteEmitWord(emitter, input[5]))
            return FALSE;
        if (textured) {
            if (stateV4 && !stateV5 && !ExecuteEmitWord(emitter, input[5]))
                return FALSE;
            if (!ExecuteEmitWord(emitter, input[3]) ||
                !ExecuteEmitWord(emitter, input[4]))
                return FALSE;
        } else if (!stateV5 && !ExecuteEmitWord(emitter, input[5]))
            return FALSE;
        if (stateV5 && (phase6State & RADEON3D_PHASE6_TEXTURE1) &&
            (!ExecuteEmitWord(emitter, input[6]) ||
             !ExecuteEmitWord(emitter, input[7])))
            return FALSE;
    }
    return TRUE;
}

static BOOL EmitExecuteClear(struct Radeon3DDevice *device,
                             struct Radeon3DExecuteEmitter *emitter,
                             const ULONG *record, ULONG length)
{
    struct Radeon3DSurfaceHandle *color;
    struct Radeon3DSurfaceHandle *depth;
    struct Radeon3DExecuteState state = {0};
    ULONG clearMask;
    ULONG vertices[6UL * RADEON3D_EXEC_VERTEX_DWORDS];
    ULONG vertex;
    static const UBYTE corners[12] = {0, 0, 1, 0, 1, 1,
                                      0, 0, 1, 1, 0, 1};

    if (length != RADEON3D_EXEC_CLEAR_DWORDS)
        return FALSE;
    color = ExecuteSurface(device, record[2]);
    depth = ExecuteSurface(device, record[3]);
    clearMask = record[4];
    if (!clearMask || (clearMask & ~RADEON3D_CLEAR_MASK) ||
        !ValidColorTarget(device, color) ||
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
    state.Color = color;
    state.Depth = depth;
    state.Options = (clearMask & RADEON3D_CLEAR_COLOR)
                        ? 0UL : RADEON3D_EXEC_SUPPRESS_COLOR_WRITE;
    state.Left = record[7];
    state.Top = record[8];
    state.Right = record[9];
    state.Bottom = record[10];
    state.ClearDepth = (clearMask & RADEON3D_CLEAR_DEPTH) != 0;
    return EmitExecuteState(emitter, &state) &&
           EmitExecuteVertices(emitter, vertices, 6UL, depth != NULL,
                                 FALSE, FALSE, FALSE, 0,
                                 R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST);
}

static BOOL EmitExecuteDraw(struct Radeon3DDevice *device,
                             struct Radeon3DExecuteEmitter *emitter,
                             const ULONG *record, ULONG length,
                             ULONG primitiveType)
{
    struct Radeon3DSurfaceHandle *color;
    struct Radeon3DSurfaceHandle *depth;
    struct Radeon3DSurfaceHandle *texture;
    struct Radeon3DSurfaceHandle *texture1 = NULL;
    struct Radeon3DExecuteState state;
    ULONG options;
    ULONG vertexCount;
    const ULONG *vertices;
    ULONG vertex;
    ULONG headerDwords;
    ULONG textureOffset = 0, textureWidth = 0, textureHeight = 0;
    ULONG textureState = 0, fragmentState = 0, textureBytes = 0;
    ULONG texture1Offset = 0, texture1Width = 0, texture1Height = 0;
    ULONG texture1State = 0, texture1Bytes = 0, phase6State = 0;
    ULONG fogColor = 0;
    ULONG levels = 1, minFilter = 0, sourceBlend = 0, destinationBlend = 0;
    ULONG levels1 = 1, minFilter1 = 0;
    ULONG vertexStride;
    BOOL textured;
    BOOL textured1;
    BOOL fog;
    BOOL useDepth;
    BOOL stateV4;
    BOOL stateV5;

    if (length < RADEON3D_EXEC_DRAW_HEADER_DWORDS)
        return FALSE;
    color = ExecuteSurface(device, record[2]);
    depth = ExecuteSurface(device, record[3]);
    texture = ExecuteSurface(device, record[4]);
    options = record[5];
    stateV4 = (options & RADEON3D_DRAW_STATE_V4) != 0;
    stateV5 = (options & RADEON3D_DRAW_STATE_V5) != 0;
    headerDwords = stateV5 ? RADEON3D_EXEC_DRAW_V5_HEADER_DWORDS
                           : stateV4 ? RADEON3D_EXEC_DRAW_V4_HEADER_DWORDS
                                     : RADEON3D_EXEC_DRAW_HEADER_DWORDS;
    vertexStride = stateV5 ? RADEON3D_EXEC_V5_VERTEX_DWORDS
                           : RADEON3D_EXEC_VERTEX_DWORDS;
    if (length < headerDwords)
        return FALSE;
    vertexCount = record[10];
    vertices = record + headerDwords;
    textured = (options & RADEON3D_DRAW_TEXTURED) != 0;
    useDepth = (options & (RADEON3D_DRAW_DEPTH_LESS |
                           RADEON3D_DRAW_DEPTH_WRITE)) != 0;
    if (stateV4) {
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
    if (stateV5) {
        texture1 = ExecuteSurface(device, record[15]);
        texture1Offset = record[16];
        texture1Width = (record[17] & 0xffffUL) + 1UL;
        texture1Height = (record[17] >> 16) + 1UL;
        texture1State = record[18];
        phase6State = record[19];
        fogColor = record[20];
        levels1 = ((texture1State & RADEON3D_TEX_LEVELS_MASK) >>
                   RADEON3D_TEX_LEVELS_SHIFT) + 1UL;
        minFilter1 = (texture1State & RADEON3D_TEX_MIN_MASK) >>
                     RADEON3D_TEX_MIN_SHIFT;
    }
    textured1 = stateV5 &&
                (phase6State & RADEON3D_PHASE6_TEXTURE1) != 0;
    fog = stateV5 && (phase6State & RADEON3D_PHASE6_FOG) != 0;
    if ((options & ~RADEON3D_DRAW_OPTIONS) ||
        (stateV5 && (!stateV4 || device->InterfaceVersion < 5UL)) ||
        (!stateV5 && (options & ~RADEON3D_DRAW_OPTIONS_V4)) ||
        (stateV4 && device->InterfaceVersion < 4UL) ||
        (!stateV4 && options & ~RADEON3D_DRAW_OPTIONS_V3) ||
        (device->InterfaceVersion < 3UL &&
         (options & RADEON3D_DRAW_DEPTH_FUNC_MASK)) ||
        (stateV4 && (options & (RADEON3D_DRAW_BILINEAR |
                                RADEON3D_DRAW_ALPHA_BLEND))) ||
        (stateV4 && ((textureState & ~RADEON3D_TEX_STATE_MASK) ||
                     minFilter > RADEON3D_TEX_MIN_LINEAR_MIPMAP_LINEAR ||
                     (fragmentState & ~RADEON3D_FRAGMENT_STATE_MASK) ||
                     sourceBlend > RADEON3D_BLEND_SRC_ALPHA_SATURATE ||
                     destinationBlend > RADEON3D_BLEND_ONE_MINUS_DST_ALPHA)) ||
        (stateV5 && ((phase6State & ~RADEON3D_PHASE6_STATE_MASK) ||
                     (texture1State & ~RADEON3D_TEX_STATE_MASK) ||
                     minFilter1 >
                         RADEON3D_TEX_MIN_LINEAR_MIPMAP_LINEAR ||
                     (fogColor & 0xff000000UL))) ||
        (options & RADEON3D_DRAW_BILINEAR && !textured) ||
        (options & RADEON3D_DRAW_ALPHA_BLEND && !textured) ||
        (options & RADEON3D_DRAW_DEPTH_FUNC_MASK &&
         !(options & RADEON3D_DRAW_DEPTH_LESS)) ||
        (options & RADEON3D_DRAW_DEPTH_WRITE &&
         !(options & RADEON3D_DRAW_DEPTH_LESS)) ||
        vertexCount < 3UL || vertexCount > RADEON3D_IMMD_MAX_VERTICES ||
        (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST &&
         vertexCount % 3UL) ||
        length != headerDwords + vertexCount * vertexStride ||
        !ValidColorTarget(device, color) ||
        (useDepth ? !ValidDepthTarget(depth, color) : record[3] != 0) ||
        (textured ? (stateV4
                         ? !ValidTextureTargetV4(texture, color, depth,
                                                 textureOffset, textureWidth,
                                                 textureHeight, levels,
                                                 &textureBytes) ||
                               (minFilter >=
                                    RADEON3D_TEX_MIN_NEAREST_MIPMAP_NEAREST &&
                                levels == 1UL)
                         : !ValidTextureTarget(texture, color, depth))
                    : record[4] != 0 || (stateV4 &&
                          (textureOffset || record[12] || textureState))) ||
        (textured1
             ? !ValidTextureTargetV4(texture1, color, depth,
                                     texture1Offset, texture1Width,
                                     texture1Height, levels1,
                                     &texture1Bytes) ||
                    (texture1 != texture &&
                     ExecuteSurfacesOverlap(texture1, texture)) ||
                   (minFilter1 >=
                        RADEON3D_TEX_MIN_NEAREST_MIPMAP_NEAREST &&
                    levels1 == 1UL)
             : stateV5 && (record[15] || texture1Offset || record[17] ||
                           texture1State)) ||
        (!fog && stateV5 && fogColor) ||
        !ValidExecuteScissor(color, record[6], record[7],
                             record[8], record[9]))
        return FALSE;
    if (textured && !stateV4)
        textureBytes = (texture->Height - 1UL) * texture->Pitch +
                       texture->Width *
                           (texture->Format == RADEON3D_FORMAT_B8G8R8A8
                                ? 4UL : 2UL);
    for (vertex = 0; vertex < vertexCount; ++vertex) {
        const ULONG *input = vertices + vertex * vertexStride;

        if (!ValidScreenCoordinate(input[0], color->Width) ||
            !ValidScreenCoordinate(input[1], color->Height) ||
            !ValidUnitFloat(input[2]) ||
            (stateV4 && textured
                  ? (!ValidTextureCoordinate(input[3]) ||
                     !ValidTextureCoordinate(input[4]))
                  : (!ValidUnitFloat(input[3]) || !ValidUnitFloat(input[4]))) ||
            (!textured && (input[3] || input[4])) ||
            (stateV5 &&
             (textured1
                  ? (!ValidTextureCoordinate(input[6]) ||
                     !ValidTextureCoordinate(input[7]))
                  : (input[6] || input[7]))) ||
            (stateV5 &&
             (fog ? !ValidUnitFloat(input[8]) : input[8] != 0)))
            return FALSE;
    }
    state.Color = color;
    state.Depth = depth;
    state.Texture = texture;
    state.Texture1 = texture1;
    state.Options = options;
    state.Left = record[6];
    state.Top = record[7];
    state.Right = record[8];
    state.Bottom = record[9];
    state.ClearDepth = FALSE;
    state.StateV4 = stateV4;
    state.StateV5 = stateV5;
    state.TextureOffset = textureOffset;
    state.TextureWidth = textureWidth;
    state.TextureHeight = textureHeight;
    state.TextureState = textureState;
    state.FragmentState = fragmentState;
    state.TextureBytes = textureBytes;
    state.Texture1Offset = texture1Offset;
    state.Texture1Width = texture1Width;
    state.Texture1Height = texture1Height;
    state.Texture1State = texture1State;
    state.Texture1Bytes = texture1Bytes;
    state.Phase6State = phase6State;
    state.FogColor = fogColor;
    return EmitExecuteState(emitter, &state) &&
           EmitExecuteVertices(emitter, vertices, vertexCount, useDepth,
                                 textured, stateV4, stateV5, phase6State,
                                 primitiveType);
}

static BOOL BuildExecuteStream(struct Radeon3DDevice *device,
                               const ULONG *records, ULONG recordDwords,
                               struct Radeon3DExecuteEmitter *emitter)
{
    ULONG index = 0;

    if (device->InterfaceVersion < 2UL)
        return FALSE;
    while (index < recordDwords) {
        ULONG length;

        if (recordDwords - index < 2UL)
            return FALSE;
        length = records[index + 1UL];
        if (length < 2UL || length > recordDwords - index)
            return FALSE;
        if (records[index] == RADEON3D_EXEC_CLEAR) {
            if (!EmitExecuteClear(device, emitter, records + index, length))
                return FALSE;
        } else if (records[index] == RADEON3D_EXEC_DRAW_TRIANGLES) {
            if (!EmitExecuteDraw(device, emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST))
                return FALSE;
        } else if (device->InterfaceVersion >= 7UL &&
                   records[index] == RADEON3D_EXEC_DRAW_TRI_STRIP) {
            if (!EmitExecuteDraw(device, emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_TRI_STRIP))
                return FALSE;
        } else if (device->InterfaceVersion >= 7UL &&
                   records[index] == RADEON3D_EXEC_DRAW_TRI_FAN) {
            if (!EmitExecuteDraw(device, emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_TRI_FAN))
                return FALSE;
        } else
            return FALSE;
        index += length;
    }
    return index == recordDwords && emitter->Count;
}

void Radeon3DAdvanceGeneration(struct RadeonChipBase *base)
{
    if (!base)
        return;
    ++base->ServiceGeneration;
    if (!base->ServiceGeneration)
        ++base->ServiceGeneration;
}

BOOL Radeon3DInvalidateForTest(
    __REGA0(struct Radeon3DDevice *device),
    __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct BoardInfo *bi;
    BOOL recovered;

    if (!SysBase || !device)
        return FALSE;
    bi = LockServiceBoard(base, &device);
    if (!bi)
        return FALSE;
    recovered = RadeonRecoverAcceleration(bi);
    UnlockServiceBoard(base, bi, device);
    return recovered;
}

struct Radeon3DDevice *Radeon3DOpen(
    __REGD0(ULONG requestedVersion),
    __REGA0(struct Radeon3DInfo *info),
    __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct Radeon3DDevice *device;
    struct RadeonBoardData *data;
    struct Library *ownerBase;

    if (!SysBase || !info || info->Size < RADEON3D_INFO_V1_SIZE ||
        !requestedVersion)
        return NULL;

    ownerBase = OpenLibrary((CONST_STRPTR)"rtg.library", 0);
    if (!ownerBase)
        return NULL;
    device = AllocMem(sizeof(*device), MEMF_PUBLIC | MEMF_CLEAR);
    if (!device) {
        CloseLibrary(ownerBase);
        return NULL;
    }
    device->Surfaces.mlh_Head =
        (struct MinNode *)&device->Surfaces.mlh_Tail;
    device->Surfaces.mlh_Tail = NULL;
    device->Surfaces.mlh_TailPred =
        (struct MinNode *)&device->Surfaces.mlh_Head;

    ObtainSemaphore(&base->ServiceLock);
    ReapRetiredDevicesLocked(base);
    data = RadeonGetBoardData(base->BoardInfo);
    if (base->ServiceState != RADEON3D_SERVICE_READY ||
        !base->BoardInfo || !data ||
        !data->Initialized ||
        !RadeonCpIsReady(base->BoardInfo)) {
        ReleaseSemaphore(&base->ServiceLock);
        FreeDeviceSurfaces(base, device);
        FreeMem(device, sizeof(*device));
        CloseLibrary(ownerBase);
        return NULL;
    }

    device->Magic = RADEON3D_SESSION_MAGIC;
    device->Generation = base->ServiceGeneration;
    device->InterfaceVersion = requestedVersion < RADEON3D_IFACE_VERSION
                                   ? requestedVersion
                                   : RADEON3D_IFACE_VERSION;
    device->Base = base;
    device->OwnerBase = ownerBase;
    device->Handle = (struct Radeon3DDevice *)base->ServiceNextHandle++;
    if (base->ServiceNextHandle < 0x80000000UL)
        base->ServiceNextHandle = 0x80000000UL;
    AddServiceDevice(base, device);
    ++base->ServiceSessions;
    FillInfo(base, info, device->InterfaceVersion);
    ReleaseSemaphore(&base->ServiceLock);
    return device->Handle;
}

void Radeon3DClose(
    __REGA0(struct Radeon3DDevice *device),
    __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct Radeon3DDevice *active;
    struct Radeon3DDevice *candidate = device;
    struct Library *ownerBase;
    BOOL cleanup = FALSE;
    struct BoardInfo *bi;

    if (!SysBase || !device)
        return;

    bi = LockServiceBoard(base, &device);
    if (bi) {
        if (device->LastFence &&
            !RadeonCpWaitFence(bi, device->LastFence, 1000UL))
            (void)RadeonRecoverAcceleration(bi);
        device->LastFence = 0;
        UnlockServiceBoard(base, bi, device);
    }

    ObtainSemaphore(&base->ServiceLock);
    active = FindDevice(base, candidate);
    if (!active) {
        ReleaseSemaphore(&base->ServiceLock);
        return;
    }
    RemoveServiceDevice(active);
    active->Closing = TRUE;
    ownerBase = NULL;
    if (!active->ActiveCalls) {
        ownerBase = active->OwnerBase;
        active->OwnerBase = NULL;
        active->CleanupDone = TRUE;
        cleanup = TRUE;
    }
    active->Magic = 0;
    active->Base = NULL;
    AddRetiredDevice(base, active);
    ReleaseSemaphore(&base->ServiceLock);

    FinishDeviceCleanup(base, NULL, active, cleanup, ownerBase);
}

BOOL Radeon3DGetInfo(
    __REGA0(struct Radeon3DDevice *device),
    __REGA1(struct Radeon3DInfo *info),
    __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct Radeon3DDevice *active;
    BOOL valid;

    if (!SysBase || !device || !info ||
        info->Size < RADEON3D_INFO_V1_SIZE)
        return FALSE;

    ObtainSemaphore(&base->ServiceLock);
    active = FindDevice(base, device);
    valid = active && active->Magic == RADEON3D_SESSION_MAGIC &&
            active->Base == base &&
            base->ServiceState == RADEON3D_SERVICE_READY &&
            base->BoardInfo && RadeonCpIsReady(base->BoardInfo) &&
            active->Generation == base->ServiceGeneration;
    if (valid)
        FillInfo(base, info, active->InterfaceVersion);
    ReleaseSemaphore(&base->ServiceLock);
    return valid;
}

BOOL Radeon3DSubmit(
    __REGA0(struct Radeon3DDevice *device),
    __REGA1(const ULONG *commands),
    __REGD0(ULONG commandCount),
    __REGD1(ULONG flags),
    __REGA2(ULONG *fenceOut),
    __REGA6(struct RadeonChipBase *base))
{
    struct BoardInfo *bi;
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    ULONG *trusted;
    ULONG internalFence = 0;
    ULONG index;
    BOOL submitAttempted = FALSE;
    BOOL result;

    if (fenceOut)
        *fenceOut = 0;
    if (!SysBase || !commands || !commandCount ||
        commandCount > RADEON3D_MAX_BATCH_DWORDS ||
        (flags & ~RADEON3D_SUBMIT_FLAGS))
        return FALSE;
    trusted = AllocMem(commandCount * sizeof(*trusted), MEMF_PUBLIC);
    if (!trusted)
        return FALSE;
    for (index = 0; index < commandCount; ++index)
        trusted[index] = commands[index];
    bi = LockServiceBoard(base, &device);
    if (!bi) {
        FreeMem(trusted, commandCount * sizeof(*trusted));
        return FALSE;
    }
    result = ValidateBatch(device, trusted, commandCount);
    if (result)
        result = RadeonPrepare3D(bi);
    if (result) {
        submitAttempted = TRUE;
        result = RadeonCpSubmitStream(bi, trusted, commandCount, TRUE,
                                      &internalFence);
    }
    if (result && internalFence) {
        device->LastFence = internalFence;
        RadeonMark3DSubmitted(bi);
        if ((flags & RADEON3D_SUBMIT_FENCE) && fenceOut)
            *fenceOut = internalFence;
    } else if (submitAttempted) {
        (void)RadeonRecoverAcceleration(bi);
    }
    UnlockServiceBoard(base, bi, device);
    FreeMem(trusted, commandCount * sizeof(*trusted));
    return result;
}

BOOL Radeon3DExecute(
    __REGA0(struct Radeon3DDevice *device),
    __REGA1(const ULONG *records),
    __REGD0(ULONG recordDwords),
    __REGD1(ULONG flags),
    __REGA2(ULONG *fenceOut),
    __REGA6(struct RadeonChipBase *base))
{
    struct BoardInfo *bi;
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct Radeon3DExecuteEmitter emitter;
    ULONG *trusted;
    ULONG *generated;
    ULONG internalFence = 0;
    ULONG index;
    BOOL submitAttempted = FALSE;
    BOOL result = FALSE;

    if (fenceOut)
        *fenceOut = 0;
    if (!SysBase || !records || !recordDwords ||
        recordDwords > RADEON3D_MAX_BATCH_DWORDS ||
        (flags & ~RADEON3D_SUBMIT_FLAGS))
        return FALSE;
    bi = LockServiceBoard(base, &device);
    if (!bi)
        return FALSE;
    if (device->InterfaceVersion < 2UL) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    if (!device->ExecuteTrusted)
        device->ExecuteTrusted = AllocMem(
            RADEON3D_MAX_BATCH_DWORDS * sizeof(ULONG), MEMF_PUBLIC);
    if (!device->ExecuteGenerated)
        device->ExecuteGenerated = AllocMem(
            RADEON3D_MAX_BATCH_DWORDS * sizeof(ULONG), MEMF_PUBLIC);
    if (!device->ExecuteTrusted || !device->ExecuteGenerated) {
        if (device->ExecuteTrusted) {
            FreeMem(device->ExecuteTrusted,
                    RADEON3D_MAX_BATCH_DWORDS * sizeof(ULONG));
            device->ExecuteTrusted = NULL;
        }
        if (device->ExecuteGenerated) {
            FreeMem(device->ExecuteGenerated,
                    RADEON3D_MAX_BATCH_DWORDS * sizeof(ULONG));
            device->ExecuteGenerated = NULL;
        }
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    trusted = device->ExecuteTrusted;
    generated = device->ExecuteGenerated;
    if (!trusted || !generated) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    for (index = 0; index < recordDwords; ++index)
        trusted[index] = records[index];
    emitter.Words = generated;
    emitter.Count = 0;
    result = BuildExecuteStream(device, trusted, recordDwords, &emitter);
    if (result)
        result = RadeonPrepare3D(bi);
    if (result) {
        submitAttempted = TRUE;
        result = RadeonCpSubmitStream(bi, generated, emitter.Count, TRUE,
                                      &internalFence);
    }
    if (result && internalFence) {
        device->LastFence = internalFence;
        RadeonMark3DSubmitted(bi);
        if ((flags & RADEON3D_SUBMIT_FENCE) && fenceOut)
            *fenceOut = internalFence;
    } else if (submitAttempted) {
        (void)RadeonRecoverAcceleration(bi);
    }
    UnlockServiceBoard(base, bi, device);
    return result;
}

static BOOL IsSessionFence(struct RadeonChipBase *base,
                           struct Radeon3DDevice *device, ULONG fence)
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct Radeon3DDevice *active;
    BOOL valid;

    if (!SysBase || !device || !fence)
        return FALSE;
    ObtainSemaphore(&base->ServiceLock);
    active = FindDevice(base, device);
    valid = active && active->Magic == RADEON3D_SESSION_MAGIC &&
            active->Base == base && active->LastFence == fence;
    ReleaseSemaphore(&base->ServiceLock);
    return valid;
}

BOOL Radeon3DTestFence(
    __REGA0(struct Radeon3DDevice *device),
    __REGD0(ULONG fence),
    __REGA6(struct RadeonChipBase *base))
{
    struct BoardInfo *bi;
    BOOL result;

    if (!IsSessionFence(base, device, fence))
        return FALSE;
    bi = LockServiceBoard(base, &device);
    if (!bi)
        return FALSE;
    result = RadeonCpTestFence(bi, fence);
    UnlockServiceBoard(base, bi, device);
    return result;
}

BOOL Radeon3DWaitFence(
    __REGA0(struct Radeon3DDevice *device),
    __REGD0(ULONG fence),
    __REGD1(ULONG timeoutMs),
    __REGA6(struct RadeonChipBase *base))
{
    struct BoardInfo *bi;
    BOOL result;

    if (!IsSessionFence(base, device, fence))
        return FALSE;
    bi = LockServiceBoard(base, &device);
    if (!bi)
        return FALSE;
    result = RadeonCpWaitFence(bi, fence, timeoutMs);
    UnlockServiceBoard(base, bi, device);
    return result;
}

static ULONG PublicSurfaceFormat(RGBFTYPE format, ULONG bytesPerPixel)
{
    if (format == RGBFB_CLUT && bytesPerPixel == 1)
        return RADEON3D_FORMAT_CLUT8;
    if (format == RGBFB_R5G6B5PC && bytesPerPixel == 2)
        return RADEON3D_FORMAT_R5G6B5PC;
    if (format == RGBFB_B8G8R8A8 && bytesPerPixel == 4)
        return RADEON3D_FORMAT_B8G8R8A8;
    return 0;
}

BOOL Radeon3DImportBitMap(
    __REGA0(struct Radeon3DDevice *device),
    __REGA1(struct BitMap *bitmap),
    __REGA2(struct Radeon3DSurface *surface),
    __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct BoardInfo *bi;
    struct RadeonBoardData *data;
    struct Radeon3DSurfaceHandle *handle = NULL;
    ULONG memoryBase;
    ULONG memoryAddress;
    ULONG memoryLimit;
    ULONG offset;
    ULONG pitch;
    ULONG width;
    ULONG height;
    ULONG bytesPerPixel;
    ULONG rowBytes;
    ULONG format;
    ULONG publicFormat;
    BOOL added = FALSE;

    if (!SysBase || !bitmap || !surface ||
        surface->Size < RADEON3D_SURFACE_V1_SIZE)
        return FALSE;
    surface->Version = 0;
    surface->Handle = NULL;
    bi = LockServiceBoard(base, &device);
    if (!bi)
        return FALSE;
    data = RadeonGetBoardData(bi);
    if (!data || !bi->GetBitMapAttr)
        goto out;

    memoryAddress = bi->GetBitMapAttr(bi, bitmap, GBMA_MEMORY);
    pitch = bi->GetBitMapAttr(bi, bitmap, GBMA_BYTESPERROW);
    bytesPerPixel = bi->GetBitMapAttr(bi, bitmap, GBMA_BYTESPERPIXEL);
    format = bi->GetBitMapAttr(bi, bitmap, GBMA_RGBFORMAT);
    width = bi->GetBitMapAttr(bi, bitmap, GBMA_WIDTH);
    height = bi->GetBitMapAttr(bi, bitmap, GBMA_HEIGHT);
    publicFormat = PublicSurfaceFormat((RGBFTYPE)format, bytesPerPixel);
    memoryBase = (ULONG)bi->MemoryBase;
    memoryLimit = bi->MemorySize;
    if (data->InstalledVram < memoryLimit)
        memoryLimit = data->InstalledVram;
    if (bi->MemorySpaceSize < memoryLimit)
        memoryLimit = bi->MemorySpaceSize;

    if (!memoryAddress || memoryAddress < memoryBase ||
        memoryAddress - memoryBase >= memoryLimit ||
        !pitch || (pitch & 63UL) || !width || !height ||
        !publicFormat || width > ~0UL / bytesPerPixel)
        goto out;
    offset = memoryAddress - memoryBase;
    rowBytes = width * bytesPerPixel;
    if (rowBytes > pitch || rowBytes > memoryLimit - offset ||
        height - 1UL > (memoryLimit - offset - rowBytes) / pitch ||
        data->FramebufferGpuBase > ~0UL - offset)
        goto out;

    handle = AllocMem(sizeof(*handle), MEMF_PUBLIC | MEMF_CLEAR);
    if (!handle)
        goto out;
    handle->Device = device;
    handle->BitMap = bitmap;
    handle->CpuAddress = (APTR)memoryAddress;
    handle->GpuAddress = data->FramebufferGpuBase + offset;
    handle->Pitch = pitch;
    handle->Width = width;
    handle->Height = height;
    handle->Format = publicFormat;

    ObtainSemaphore(&base->ServiceLock);
    if (FindActiveDevice(base, device) &&
        IsUsableDevice(base, device) && !device->Closing) {
        AddSurfaceHandle(device, handle);
        added = TRUE;
    }
    ReleaseSemaphore(&base->ServiceLock);
    if (!added)
        goto out;

    surface->Version = RADEON3D_SURFACE_VERSION;
    surface->Generation = device->Generation;
    surface->CpuAddress = handle->CpuAddress;
    surface->GpuAddress = handle->GpuAddress;
    surface->Pitch = handle->Pitch;
    surface->Width = handle->Width;
    surface->Height = handle->Height;
    surface->Format = handle->Format;
    surface->Handle = handle;

out:
    if (!added && handle)
        FreeMem(handle, sizeof(*handle));
    UnlockServiceBoard(base, bi, device);
    return added;
}

void Radeon3DReleaseSurface(
    __REGA0(struct Radeon3DDevice *device),
    __REGA1(struct Radeon3DSurface *surface),
    __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct BoardInfo *bi;
    struct Radeon3DDevice *active;
    struct Radeon3DDevice *candidate = device;
    struct Radeon3DSurfaceHandle *handle = NULL;

    if (!SysBase || !surface ||
        surface->Size < RADEON3D_SURFACE_V1_SIZE || !surface->Handle)
        return;
    bi = LockServiceBoard(base, &device);
    if (bi) {
        if (device->LastFence &&
            !RadeonCpWaitFence(bi, device->LastFence, 1000UL))
            (void)RadeonRecoverAcceleration(bi);
        device->LastFence = 0;
    }
    ObtainSemaphore(&base->ServiceLock);
    active = FindDevice(base, candidate);
    if (active) {
        handle = FindSurfaceHandle(active, surface->Handle);
        if (handle && handle->Device == active)
            RemoveSurfaceHandle(handle);
        else
            handle = NULL;
    }
    ReleaseSemaphore(&base->ServiceLock);
    if (bi)
        UnlockServiceBoard(base, bi, device);
    if (handle)
        FreeMem(handle, sizeof(*handle));
    surface->Version = 0;
    surface->Generation = 0;
    surface->CpuAddress = NULL;
    surface->GpuAddress = 0;
    surface->Pitch = 0;
    surface->Width = 0;
    surface->Height = 0;
    surface->Format = 0;
    surface->Handle = NULL;
}

BOOL Radeon3DDetachOwner(
    __REGA0(struct BoardInfo *bi),
    __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    if (!SysBase || !bi)
        return FALSE;
    return RadeonReleaseBoard(base, bi, FALSE);
}

void Radeon3DInvalidateService(struct BoardInfo *bi)
{
    struct RadeonChipBase *base;
    struct ExecBase *SysBase;

    if (!bi || !bi->ChipBase)
        return;
    base = (struct RadeonChipBase *)bi->ChipBase;
    if (base->ServiceState != RADEON3D_SERVICE_READY)
        return;
    SysBase = base->ExecBase;
    if (!SysBase)
        return;
    ObtainSemaphore(&base->ServiceLock);
    if (base->BoardInfo == bi &&
        base->ServiceState == RADEON3D_SERVICE_READY) {
        base->ServiceState = RADEON3D_SERVICE_ATTACHED;
        Radeon3DAdvanceGeneration(base);
    }
    ReleaseSemaphore(&base->ServiceLock);
}

BOOL Radeon3DRearmService(struct BoardInfo *bi)
{
    struct RadeonChipBase *base;
    struct ExecBase *SysBase;
    BOOL ready = FALSE;

    if (!bi || !bi->ChipBase || !RadeonCpIsReady(bi))
        return FALSE;
    base = (struct RadeonChipBase *)bi->ChipBase;
    SysBase = base->ExecBase;
    if (!SysBase)
        return FALSE;
    ObtainSemaphore(&base->ServiceLock);
    if (base->BoardInfo == bi &&
        (base->ServiceState == RADEON3D_SERVICE_ATTACHED ||
         base->ServiceState == RADEON3D_SERVICE_READY)) {
        base->ServiceState = RADEON3D_SERVICE_READY;
        ready = TRUE;
    }
    ReleaseSemaphore(&base->ServiceLock);
    return ready;
}

void Radeon3DFreeRetiredDevices(struct RadeonChipBase *base)
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct MinNode *node;

    if (!SysBase)
        return;
    while ((node = base->RetiredServiceDevices.mlh_Head)->mln_Succ) {
        struct Radeon3DDevice *device = (struct Radeon3DDevice *)node;

        RemoveServiceDevice(device);
        FreeMem(device, sizeof(*device));
    }
}
