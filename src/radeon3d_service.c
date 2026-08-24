#include <exec/memory.h>
#include <proto/exec.h>
#include <devices/timer.h>

#include "radeon9200.h"
#include "radeon_debug.h"
#include "radeon_regs.h"

#define RADEON3D_SESSION_MAGIC 0x52334453UL
#define RADEON3D_EXEC_SUPPRESS_COLOR_WRITE 0x80000000UL

struct Radeon3DExecuteEmitter;

struct Radeon3DSegmentSlot {
    APTR CpuAddress;
    ULONG GpuAddress;
    ULONG Bytes;
    BOOL Allocated;
};

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
    struct Radeon3DExecuteEmitter *ExecuteEmitter;
    struct Radeon3DSegmentSlot Segments[RADEON3D_MAX_SEGMENTS];
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

/*
 * Execute-phase attribution clock. Independent of radeon_debug.c's shared
 * TimerBase so release builds (where the debug timer never opens) still
 * measure; UNIT_VBLANK TR_GETSYSTIME costs one short DoIO per phase
 * boundary, which is noise against the multi-millisecond phases being
 * attributed. Unsigned delta arithmetic absorbs the 2^32 microsecond
 * timeval wrap because real intervals are orders of magnitude shorter.
 */
static void EnsureExecTimer(struct RadeonChipBase *base)
{
    struct ExecBase *SysBase = base->ExecBase;
    struct MsgPort *port;
    struct timerequest *io;

    if (!SysBase || base->ExecTimerIO || base->ExecTimerFailed)
        return;
    port = CreateMsgPort();
    io = port ? (struct timerequest *)CreateIORequest(
                    port, sizeof(struct timerequest))
              : NULL;
    if (!port || !io ||
        OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                   (struct IORequest *)io, 0) != 0) {
        if (io)
            DeleteIORequest((struct IORequest *)io);
        if (port)
            DeleteMsgPort(port);
        base->ExecTimerFailed = TRUE;
        return;
    }
    base->ExecTimerPort = (APTR)port;
    base->ExecTimerIO = (APTR)io;
}

static ULONG ServiceExecMicros(struct RadeonChipBase *base)
{
    struct ExecBase *SysBase = base->ExecBase;
    struct timerequest *io = (struct timerequest *)base->ExecTimerIO;

    if (!SysBase || !io)
        return 0;
    io->tr_node.io_Command = TR_GETSYSTIME;
    DoIO((struct IORequest *)io);
    return io->tr_time.tv_secs * 1000000UL + io->tr_time.tv_micro;
}

/* Trusted-record copy: both sides are cached memory, so the cost is pure
 * instruction count. Eight values held in registers per iteration give the
 * scheduler independent loads/stores instead of a dependent per-dword
 * chain; no intermediate staging pass. */
static void ExecCopyRecords(ULONG *dst, const ULONG *src, ULONG words)
{
    while (words >= 8UL) {
        ULONG a = src[0], b = src[1], c = src[2], d = src[3];
        ULONG e = src[4], f = src[5], g = src[6], h = src[7];

        dst[0] = a;
        dst[1] = b;
        dst[2] = c;
        dst[3] = d;
        dst[4] = e;
        dst[5] = f;
        dst[6] = g;
        dst[7] = h;
        src += 8;
        dst += 8;
        words -= 8;
    }
    while (words--)
        *(dst++) = *(src++);
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

/* Defined with the emitter, whose size is not known here. */
static void FreeExecuteEmitter(struct RadeonChipBase *base,
                               struct Radeon3DDevice *device);

static void FreeDeviceSegments(struct RadeonChipBase *base,
                               struct BoardInfo *bi,
                               struct Radeon3DDevice *device)
{
    struct BoardInfo *board = bi ? bi : base->BoardInfo;
    ULONG index;

    if (!board)
        return;
    for (index = 0; index < RADEON3D_MAX_SEGMENTS; ++index) {
        struct Radeon3DSegmentSlot *slot = &device->Segments[index];

        if (!slot->Allocated)
            continue;
        (void)RadeonFreePrivateVram(board, slot->CpuAddress, slot->Bytes);
        slot->CpuAddress = NULL;
        slot->GpuAddress = 0;
        slot->Bytes = 0;
        slot->Allocated = FALSE;
    }
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
    FreeExecuteEmitter(base, device);
}

static void FillInfo(struct RadeonChipBase *base, struct Radeon3DInfo *info,
                     ULONG interfaceVersion)
{
    struct BoardInfo *bi = base->BoardInfo;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    ULONG requestedSize = info->Size;

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
        info->Caps |= RADEON3D_CAP_DEPTH_FUNCS;
    if (interfaceVersion >= 4UL)
        info->Caps |= RADEON3D_CAP_TEXTURE_STATE;
    if (interfaceVersion >= 5UL)
        info->Caps |= RADEON3D_CAP_FOG_MULTITEX;
#ifdef DEBUG
    if (interfaceVersion >= 5UL)
        info->Caps |= RADEON3D_CAP_TEST_INVALIDATE;
#endif
    if (interfaceVersion >= 6UL)
        info->Caps |= RADEON3D_CAP_COLOR_TARGET_FORMATS;
    if (interfaceVersion >= 7UL)
        info->Caps |= RADEON3D_CAP_NATIVE_TRI_PRIMITIVES;
    if (interfaceVersion >= 8UL)
        info->Caps |= RADEON3D_CAP_NATIVE_QUAD_LISTS;
    if (interfaceVersion >= 9UL)
        info->Caps |= RADEON3D_CAP_HW_TRANSFORM_CLIP;
    if (interfaceVersion >= 10UL)
        info->Caps |= RADEON3D_CAP_HW_TEXGEN;
    if (interfaceVersion >= 11UL)
        info->Caps |= RADEON3D_CAP_HW_NORMALS |
                      RADEON3D_CAP_HW_LIGHTING;
    if (interfaceVersion >= 12UL)
        info->Caps |= RADEON3D_CAP_HW_SPHERE_MAP;
    if (interfaceVersion >= 12UL)
        info->Caps |= RADEON3D_CAP_COMPACT_TCL_VERTEX;
    if (interfaceVersion >= 13UL)
        info->Caps |= RADEON3D_CAP_STREAM_SEGMENTS;
    if (RadeonCpIsReady(bi))
        info->Caps |= RADEON3D_CAP_CP_READY;
    info->InstalledVram = data ? data->InstalledVram : 0;
    info->Picasso96Vram = bi ? bi->MemorySize : 0;
    info->MaxBatchDwords = RADEON3D_MAX_BATCH_DWORDS;
    if (requestedSize >= RADEON3D_INFO_V2_SIZE) {
        /* Size-gated V2 tail: older callers pass a V1-sized buffer and
         * must not see anything beyond it touched. */
        info->ExecCalls = base->ExecCalls;
        info->ExecRecordDwords = base->ExecRecordDwords;
        info->ExecGeneratedDwords = base->ExecGeneratedDwords;
        info->ExecCopyMicros = base->ExecCopyMicros;
        info->ExecBuildMicros = base->ExecBuildMicros;
        info->ExecSubmitMicros = base->ExecSubmitMicros;
        info->Size = RADEON3D_INFO_V2_SIZE;
    } else {
        info->Size = RADEON3D_INFO_V1_SIZE;
    }
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
        FreeDeviceSegments(base, bi, device);
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

static BOOL ValidPositiveFloat(ULONG bits)
{
    return bits != 0 && !(bits & 0x80000000UL) &&
           (bits & 0x7f800000UL) != 0x7f800000UL;
}

static BOOL ValidFloat(ULONG bits)
{
    return (bits & 0x7f800000UL) != 0x7f800000UL;
}

static ULONG UnsignedHalfFloatBits(ULONG value)
{
    ULONG bits=UnsignedFloatBits(value);
    return bits ? bits-(1UL<<23) : 0;
}

static BOOL ValidateTriangleBatch(struct Radeon3DDevice *device,
                                  const ULONG *commands,
                                  ULONG commandCount)
{
    struct Radeon3DSurfaceHandle *target;
    /* This is the published wire contract for the immediate triangle list,
     * not an internal choice: it must keep matching RADEON3D_SUBMISSION.md
     * exactly. Alpha shading is deliberately absent because the stream only
     * targets RGB565, which has no alpha channel to interpolate. */
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
    BOOL FragmentStatePresent;
    BOOL ExtendedVertex;
    BOOL HardwareTcl;
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
    ULONG VertexState;
    ULONG FogColor;
    ULONG ModelProjection[16];
    ULONG Viewport[6];
    ULONG TransformFlags;
    BOOL TexGen;
    ULONG TexGenState[2];
    ULONG TexGenMatrix[2][16];
    BOOL NormalVertex;
    BOOL Lighting;
    ULONG LightControl;
    ULONG ModelView[16];
    ULONG InvModelView[16];
    ULONG GlobalAmbient[4];
    ULONG EyeVector[4];
    ULONG Material[17];
    ULONG Lights[8][31];
};

struct Radeon3DExecuteEmitter {
    ULONG *Words;
    ULONG Count;
    struct Radeon3DExecuteState State;
    BOOL StateValid;
    /* The MVP matrix and guard-clip scalars are tracked separately from the
     * register block so a matrix-only change between records re-emits ~21
     * dwords instead of the whole TCL state. */
    BOOL GuardClipEmitted;
    BOOL MatrixValid;
    ULONG Matrix[16];
    BOOL TexGenMatrixValid[2];
    ULONG TexGenMatrix[2][16];
    BOOL ModelViewValid;
    ULONG ModelView[16];
    BOOL InvModelViewValid;
    ULONG InvModelView[16];
    /* Commit path: vertices live in a streaming segment at this card
     * address; a VBUF_2 fetch packet replaces the inline vertex stream. */
    BOOL CommitVbuf;
    ULONG CommitVbufAddress;
    /* Per-record scratch for the clear and draw builders. Lighting made
     * Radeon3DExecuteState too large to keep on a client's stack, and the
     * emitter itself is session-owned, so the builders borrow this instead
     * of declaring their own. The two builders never nest. */
    struct Radeon3DExecuteState Scratch;
};

static void FreeExecuteEmitter(struct RadeonChipBase *base,
                               struct Radeon3DDevice *device)
{
    struct ExecBase *SysBase = base->ExecBase;

    if (!device->ExecuteEmitter)
        return;
    FreeMem(device->ExecuteEmitter, sizeof(*device->ExecuteEmitter));
    device->ExecuteEmitter = NULL;
}

static void ClearExecuteState(struct Radeon3DExecuteState *state)
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

static BOOL ExecuteEmitMatrix(struct Radeon3DExecuteEmitter *emitter,
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
static BOOL ExecuteEmitVectorBlock(struct Radeon3DExecuteEmitter *emitter,
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

static BOOL ExecuteEmitScalarBlock(struct Radeon3DExecuteEmitter *emitter,
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

static BOOL ExecuteEmitGuardClipState(struct Radeon3DExecuteEmitter *emitter)
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

static BOOL ValidTextureTargetWithState(struct Radeon3DSurfaceHandle *surface,
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

static BOOL UsesPowerOfTwoTexturePath(ULONG width, ULONG height, ULONG state)
{
    (void)state;
    return IsPowerOfTwo(width) && IsPowerOfTwo(height);
}

static BOOL EmitExecuteTexture(struct Radeon3DExecuteEmitter *emitter,
                               struct Radeon3DSurfaceHandle *texture,
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
        outputFormat0 |= R200_VTX_DISCRETE_FOG;
        outputSelect |= R200_OUTPUT_DISCRETE_FOG;
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
        if (state->TransformFlags & RADEON3D_TRANSFORM_FLAT_SHADE)
            seControl &= ~R200_DIFFUSE_SHADE_GOURAUD;
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

static BOOL SameExecuteState(const struct Radeon3DExecuteState *a,
                             const struct Radeon3DExecuteState *b)
{
    ULONG index;

    if (!(a->Color == b->Color && a->Depth == b->Depth &&
           a->Texture == b->Texture && a->Texture1 == b->Texture1 &&
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

static BOOL EmitExecuteStateCached(struct Radeon3DExecuteEmitter *emitter,
                                   const struct Radeon3DExecuteState *state)
{
    ULONG index;

    if (!emitter->StateValid || !SameExecuteState(&emitter->State, state)) {
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

static BOOL EmitExecuteVertices(struct Radeon3DExecuteEmitter *emitter,
                                  const ULONG *vertices, ULONG vertexCount,
                                  BOOL useDepth, BOOL textured,
                                  BOOL fragmentStatePresent,
                                  BOOL extendedVertex, BOOL hardwareTcl,
                                  BOOL normalVertex, BOOL compactVertex,
                                  ULONG vertexState,
                                  ULONG primitiveType,
                                  const struct Radeon3DSurfaceHandle *color)
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
        /* Vertex data is fetched from the segment by the hardware. The
         * LOAD_VBPNTR packet points the VAP at the segment (one array,
         * components and stride both the vertex dword count), then the
         * VBUF_2 packet fires the primitive with WALK_LIST. Hardware TCL
         * is mandatory: SE_VTX_FMT describes the vertex layout. */
        if (!hardwareTcl)
            return FALSE;
        return ExecuteEmitWord(
                   emitter,
                   RADEON_CP_PACKET3(R200_CP_CMD_3D_LOAD_VBPNTR, 2UL)) &&
               ExecuteEmitWord(emitter, 1UL) &&
               ExecuteEmitWord(emitter,
                               dwordsPerVertex |
                                   (dwordsPerVertex << 8)) &&
               ExecuteEmitWord(emitter, emitter->CommitVbufAddress) &&
               ExecuteEmitWord(
                   emitter,
                   RADEON_CP_PACKET3(R200_CP_CMD_3D_DRAW_VBUF_2, 0UL)) &&
               ExecuteEmitWord(
                   emitter,
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

static BOOL EmitExecuteClear(struct Radeon3DDevice *device,
                             struct Radeon3DExecuteEmitter *emitter,
                             const ULONG *record, ULONG length)
{
    struct Radeon3DSurfaceHandle *color;
    struct Radeon3DSurfaceHandle *depth;
    struct Radeon3DExecuteState *state = &emitter->Scratch;
    ULONG clearMask;
    ULONG vertices[6UL * RADEON3D_EXEC_VERTEX_DWORDS];
    ULONG vertex;
    static const UBYTE corners[12] = {0, 0, 1, 0, 1, 1,
                                      0, 0, 1, 1, 0, 1};

    ClearExecuteState(state);
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
    state->Color = color;
    state->Depth = depth;
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

static BOOL EmitExecuteDraw(struct Radeon3DDevice *device,
                             struct Radeon3DExecuteEmitter *emitter,
                             const ULONG *record, ULONG length,
                             ULONG primitiveType)
{
    struct Radeon3DSurfaceHandle *color;
    struct Radeon3DSurfaceHandle *depth;
    struct Radeon3DSurfaceHandle *texture;
    struct Radeon3DSurfaceHandle *texture1 = NULL;
    struct Radeon3DExecuteState *state = &emitter->Scratch;
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
    color = ExecuteSurface(device, record[2]);
    depth = ExecuteSurface(device, record[3]);
    texture = ExecuteSurface(device, record[4]);
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
    if (length < headerDwords)
        return FALSE;
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
        texture1 = ExecuteSurface(device, record[15]);
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
         (hardwareTcl && (device->InterfaceVersion < 9UL ||
                         !extendedVertex || !fragmentStatePresent ||
                         perspective ||
                         (record[43] & ~RADEON3D_TRANSFORM_STATE_MASK) ||
                         !(record[43] &
                            RADEON3D_TRANSFORM_POINT_SIZE_MASK))) ||
         (texGen &&
          (device->InterfaceVersion < 10UL || !hardwareTcl ||
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
             (device->InterfaceVersion < 12UL || !normalVertex ||
              (record[44] & RADEON3D_TEXGEN_COMPONENTS) !=
                  (RADEON3D_TEXGEN_GEN_S | RADEON3D_TEXGEN_GEN_T))) ||
            (texGenMode1 == RADEON3D_TEXGEN_MODE_SPHERE_MAP &&
             (device->InterfaceVersion < 12UL || !normalVertex ||
              (record[45] & RADEON3D_TEXGEN_COMPONENTS) !=
                  (RADEON3D_TEXGEN_GEN_S | RADEON3D_TEXGEN_GEN_T))))) ||
          ((normalVertex || lighting) &&
           (device->InterfaceVersion < 11UL || !hardwareTcl ||
            !extendedVertex || !fragmentStatePresent)) ||
          (lighting &&
           (record[lightControlIndex] &
              RADEON3D_LIGHT_CONTROL_RESERVED)) ||
         (!hardwareTcl && (options & ~RADEON3D_DRAW_OPTIONS_PRE_TCL)) ||
        (extendedVertex &&
         (!fragmentStatePresent || device->InterfaceVersion < 5UL)) ||
        (!extendedVertex &&
         (options & ~RADEON3D_DRAW_OPTIONS_FRAGMENT)) ||
        (fragmentStatePresent && device->InterfaceVersion < 4UL) ||
        (!fragmentStatePresent &&
         options & ~RADEON3D_DRAW_OPTIONS_BASIC) ||
        (device->InterfaceVersion < 3UL &&
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
                       device->InterfaceVersion < 8UL) ||
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
        (primitiveType != R200_CP_VC_CNTL_PRIM_TYPE_QUADS &&
         vertexCount > RADEON3D_IMMD_MAX_VERTICES) ||
        (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_TRI_LIST &&
           vertexCount % 3UL) ||
        (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_LINE_LIST &&
         vertexCount % 2UL) ||
        (primitiveType == R200_CP_VC_CNTL_PRIM_TYPE_QUADS &&
         (vertexCount < 4UL ||
          vertexCount > (extendedVertex
                              ? RADEON3D_IMMD_MAX_EXTENDED_QUAD_VERTICES
                                 : RADEON3D_IMMD_MAX_QUAD_VERTICES) ||
          vertexCount % 4UL)) ||
        (emitter->CommitVbuf
             ? length != headerDwords
             : length != headerDwords + vertexCount * vertexStride) ||
        !ValidColorTarget(device, color) ||
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
                             record[8], record[9]))
        return FALSE;
    if (textured && !fragmentStatePresent)
        textureBytes = (texture->Height - 1UL) * texture->Pitch +
                       texture->Width *
                           (texture->Format == RADEON3D_FORMAT_B8G8R8A8
                                ? 4UL : 2UL);
    state->Color = color;
    state->Depth = depth;
    state->Texture = texture;
    state->Texture1 = texture1;
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
    return EmitExecuteStateCached(emitter, state) &&
           EmitExecuteVertices(emitter, vertices, vertexCount, useDepth,
                                textured, fragmentStatePresent,
                                extendedVertex, hardwareTcl,
                                normalVertex, compactVertex,
                                vertexState,
                                primitiveType, color);
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
        } else if (device->InterfaceVersion >= 8UL &&
                   records[index] == RADEON3D_EXEC_DRAW_QUADS) {
            if (!EmitExecuteDraw(device, emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_QUADS))
                return FALSE;
        } else if (device->InterfaceVersion >= 9UL &&
                   records[index] == RADEON3D_EXEC_DRAW_POINTS) {
            if (!EmitExecuteDraw(device, emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_POINT_LIST))
                return FALSE;
        } else if (device->InterfaceVersion >= 9UL &&
                   records[index] == RADEON3D_EXEC_DRAW_LINES) {
            if (!EmitExecuteDraw(device, emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_LINE_LIST))
                return FALSE;
        } else if (device->InterfaceVersion >= 9UL &&
                   records[index] == RADEON3D_EXEC_DRAW_LINE_STRIP) {
            if (!EmitExecuteDraw(device, emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_LINE_STRIP))
                return FALSE;
        } else if (device->InterfaceVersion >= 9UL &&
                   records[index] == RADEON3D_EXEC_DRAW_LINE_LOOP) {
            if (!EmitExecuteDraw(device, emitter, records + index, length,
                                 R200_CP_VC_CNTL_PRIM_TYPE_LINE_LOOP))
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
#ifndef DEBUG
    (void)device;
    (void)base;
    return FALSE;
#else
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
#endif
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
    EnsureExecTimer(base);
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
    struct Radeon3DExecuteEmitter *emitter;
    ULONG *trusted;
    ULONG *generated;
    ULONG internalFence = 0;
    ULONG copyStart, buildStart, submitStart;
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
    /* The emitter is session-owned rather than automatic: with lighting
     * state it is far too large for a client's stack. */
    if (!device->ExecuteEmitter)
        device->ExecuteEmitter = AllocMem(
            sizeof(*device->ExecuteEmitter), MEMF_PUBLIC);
    if (!device->ExecuteTrusted || !device->ExecuteGenerated ||
        !device->ExecuteEmitter) {
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
        FreeExecuteEmitter(base, device);
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    trusted = device->ExecuteTrusted;
    generated = device->ExecuteGenerated;
    emitter = device->ExecuteEmitter;
    if (!trusted || !generated || !emitter) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    copyStart = RDEBUG_PHASE_BEGIN();
    {
        ULONG phase = ServiceExecMicros(base);
        ExecCopyRecords(trusted, records, recordDwords);
        base->ExecCopyMicros += ServiceExecMicros(base) - phase;
    }
    RDEBUG_EXECUTE_PHASE(RADEON_DEBUG_EXEC_COPY, copyStart);
    emitter->Words = generated;
    emitter->Count = 0;
    emitter->StateValid = FALSE;
    emitter->GuardClipEmitted = FALSE;
    emitter->MatrixValid = FALSE;
    emitter->TexGenMatrixValid[0] = FALSE;
    emitter->TexGenMatrixValid[1] = FALSE;
    emitter->ModelViewValid = FALSE;
    emitter->InvModelViewValid = FALSE;
    emitter->CommitVbuf = FALSE;
    emitter->CommitVbufAddress = 0;
    buildStart = RDEBUG_PHASE_BEGIN();
    {
        ULONG phase = ServiceExecMicros(base);
        result = BuildExecuteStream(device, trusted, recordDwords, emitter);
        if (result)
            result = RadeonPrepare3D(bi);
        base->ExecBuildMicros += ServiceExecMicros(base) - phase;
    }
    if (!result) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    RDEBUG_EXECUTE_PHASE(RADEON_DEBUG_EXEC_BUILD, buildStart);
    submitStart = RDEBUG_PHASE_BEGIN();
    {
        ULONG phase = ServiceExecMicros(base);
        result = RadeonCpSubmitStream(bi, generated, emitter->Count, TRUE,
                                      &internalFence);
        base->ExecSubmitMicros += ServiceExecMicros(base) - phase;
    }
    if (result) {
        device->LastFence = internalFence;
        RadeonMark3DSubmitted(bi);
        if ((flags & RADEON3D_SUBMIT_FENCE) && fenceOut)
            *fenceOut = internalFence;
    } else {
        (void)RadeonRecoverAcceleration(bi);
    }
    RDEBUG_EXECUTE_PHASE(RADEON_DEBUG_EXEC_SUBMIT, submitStart);
    RDEBUG_EXECUTE_SAMPLE(recordDwords, emitter->Count);
    base->ExecCalls++;
    base->ExecRecordDwords += recordDwords;
    base->ExecGeneratedDwords += emitter->Count;
    UnlockServiceBoard(base, bi, device);
    return result;
}

BOOL Radeon3DAllocSegment(
    __REGA0(struct Radeon3DDevice *device),
    __REGD0(ULONG bytes),
    __REGA1(struct Radeon3DSegment *segment),
    __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct BoardInfo *bi;
    struct RadeonBoardData *data;
    struct Radeon3DSegmentSlot *slot = NULL;
    ULONG memoryBase;
    ULONG offset;
    ULONG index;
    APTR memory;

    if (!SysBase || !segment ||
        segment->Size < RADEON3D_SEGMENT_V1_SIZE ||
        !bytes || bytes > RADEON3D_MAX_SEGMENT_BYTES || (bytes & 3UL))
        return FALSE;
    segment->Version = 0;
    segment->Id = 0;
    segment->CpuAddress = NULL;
    segment->GpuAddress = 0;
    segment->Bytes = 0;
    bi = LockServiceBoard(base, &device);
    if (!bi)
        return FALSE;
    if (device->InterfaceVersion < 13UL) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    for (index = 0; index < RADEON3D_MAX_SEGMENTS; ++index) {
        if (!device->Segments[index].Allocated) {
            slot = &device->Segments[index];
            break;
        }
    }
    if (!slot) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    memory = RadeonAllocatePrivateVram(bi, bytes);
    if (!memory) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    data = RadeonGetBoardData(bi);
    memoryBase = (ULONG)bi->MemoryBase;
    offset = (ULONG)memory - memoryBase;
    if (!data || offset > data->InstalledVram ||
        bytes > data->InstalledVram - offset ||
        offset > bi->MemorySpaceSize ||
        bytes > bi->MemorySpaceSize - offset ||
        data->FramebufferGpuBase > ~0UL - offset) {
        (void)RadeonFreePrivateVram(bi, memory, bytes);
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    slot->CpuAddress = memory;
    slot->GpuAddress = data->FramebufferGpuBase + offset;
    slot->Bytes = bytes;
    slot->Allocated = TRUE;
    segment->Version = RADEON3D_SEGMENT_VERSION;
    segment->Id = index;
    segment->CpuAddress = memory;
    segment->GpuAddress = slot->GpuAddress;
    segment->Bytes = bytes;
    UnlockServiceBoard(base, bi, device);
    return TRUE;
}

BOOL Radeon3DFreeSegment(
    __REGA0(struct Radeon3DDevice *device),
    __REGD0(ULONG segmentId),
    __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct BoardInfo *bi;
    struct Radeon3DSegmentSlot *slot;

    if (!SysBase || segmentId >= RADEON3D_MAX_SEGMENTS)
        return FALSE;
    bi = LockServiceBoard(base, &device);
    if (!bi)
        return FALSE;
    if (device->InterfaceVersion < 13UL) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    slot = &device->Segments[segmentId];
    if (!slot->Allocated) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    (void)RadeonFreePrivateVram(bi, slot->CpuAddress, slot->Bytes);
    slot->CpuAddress = NULL;
    slot->GpuAddress = 0;
    slot->Bytes = 0;
    slot->Allocated = FALSE;
    UnlockServiceBoard(base, bi, device);
    return TRUE;
}

BOOL Radeon3DCommitDraw(
    __REGA0(struct Radeon3DDevice *device),
    __REGA1(const struct Radeon3DCommit *commit),
    __REGA2(ULONG *fenceOut),
    __REGA6(struct RadeonChipBase *base))
{
    struct BoardInfo *bi;
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct Radeon3DExecuteEmitter *emitter;
    struct Radeon3DSegmentSlot *slot;
    ULONG *trusted;
    ULONG *generated;
    ULONG internalFence = 0;
    BOOL result = FALSE;

    if (fenceOut)
        *fenceOut = 0;
    if (!SysBase || !commit ||
        commit->Size < RADEON3D_COMMIT_V1_SIZE ||
        commit->Version != RADEON3D_COMMIT_VERSION ||
        !commit->Header || !commit->HeaderDwords ||
        commit->HeaderDwords > RADEON3D_MAX_BATCH_DWORDS ||
        (commit->OffsetBytes & 3UL) ||
        (commit->Flags & ~RADEON3D_SUBMIT_FLAGS))
        return FALSE;
    bi = LockServiceBoard(base, &device);
    if (!bi)
        return FALSE;
    if (device->InterfaceVersion < 13UL || !device->ExecuteTrusted ||
        !device->ExecuteGenerated || !device->ExecuteEmitter) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    if (commit->SegmentId >= RADEON3D_MAX_SEGMENTS ||
        !device->Segments[commit->SegmentId].Allocated ||
        commit->OffsetBytes >=
            device->Segments[commit->SegmentId].Bytes) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    slot = &device->Segments[commit->SegmentId];
    if (slot->GpuAddress > ~0UL - commit->OffsetBytes) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    trusted = device->ExecuteTrusted;
    generated = device->ExecuteGenerated;
    emitter = device->ExecuteEmitter;
    ExecCopyRecords(trusted, commit->Header, commit->HeaderDwords);
    emitter->Words = generated;
    emitter->Count = 0;
    emitter->StateValid = FALSE;
    emitter->GuardClipEmitted = FALSE;
    emitter->MatrixValid = FALSE;
    emitter->TexGenMatrixValid[0] = FALSE;
    emitter->TexGenMatrixValid[1] = FALSE;
    emitter->ModelViewValid = FALSE;
    emitter->InvModelViewValid = FALSE;
    emitter->CommitVbuf = TRUE;
    emitter->CommitVbufAddress = slot->GpuAddress + commit->OffsetBytes;
    result = BuildExecuteStream(device, trusted, commit->HeaderDwords,
                                emitter);
    emitter->CommitVbuf = FALSE;
    if (result)
        result = RadeonPrepare3D(bi);
    if (result) {
        result = RadeonCpSubmitStream(bi, generated, emitter->Count, TRUE,
                                      &internalFence);
    }
    if (result) {
        device->LastFence = internalFence;
        RadeonMark3DSubmitted(bi);
        if ((commit->Flags & RADEON3D_SUBMIT_FENCE) && fenceOut)
            *fenceOut = internalFence;
    } else {
        (void)RadeonRecoverAcceleration(bi);
    }
    base->ExecCalls++;
    base->ExecRecordDwords += commit->HeaderDwords;
    base->ExecGeneratedDwords += emitter->Count;
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
