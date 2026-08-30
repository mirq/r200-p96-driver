#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <devices/timer.h>

#include "radeon9200.h"
#include "radeon_debug.h"
#include "radeon_regs.h"
#include "radeon3d_emit.h"

#ifdef DEBUG
/*
 * Temporary diagnostics: dump the first Execute and the first CommitBatch
 * (record chain + generated CP stream) so the host can disassemble exactly
 * what the card was asked to do.
 */
static void DumpStreamFile(struct ExecBase *SysBase, const char *path,
                           const ULONG *records, ULONG recordDwords,
                           const ULONG *generated, ULONG generatedDwords)
{
    BPTR file;
    ULONG header[4];

    if (!SysBase)
        return;
    file = Open((STRPTR)path, MODE_NEWFILE);
    if (!file)
        return;
    header[0] = 0x52334455UL; /* 'R3DU' */
    header[1] = 1UL;
    header[2] = recordDwords;
    header[3] = generatedDwords;
    (void)Write(file, header, sizeof(header));
    if (recordDwords)
        (void)Write(file, (APTR)records, recordDwords * sizeof(ULONG));
    if (generatedDwords)
        (void)Write(file, (APTR)generated, generatedDwords * sizeof(ULONG));
    Close(file);
}

static LONG FirstExecuteDumpPending = 1;
static LONG FirstCommitDumpPending = 1;
#endif

#define RADEON3D_SESSION_MAGIC 0x52334453UL

struct Radeon3DEmitter;

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
    struct Radeon3DEmitter *ExecuteEmitter;
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
        base->StreamSegmentMask &= ~(1UL << index);
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
    if (interfaceVersion >= 13UL && base->StreamSegmentPool)
        info->Caps |= RADEON3D_CAP_STREAM_SEGMENTS;
    if (interfaceVersion >= 14UL)
        info->Caps |= RADEON3D_CAP_COMMIT_STATE_REUSE;
    if (interfaceVersion >= 15UL)
        info->Caps |= RADEON3D_CAP_COMMIT_STATE_BATCH;
    /* Ordered commits measured 52.4-53.0 fps against 55.5 with
     * drain-before-submit on reboot-isolated 800x600x32 windowed gears
     * (2026-08-30): the retired-tracking handshake costs more than the
     * intermediate drains it removes at this batch size. The machinery
     * stays; re-advertise the cap to re-enable it. */
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
    }
    if (requestedSize >= RADEON3D_INFO_V3_SIZE) {
        info->CommitFailStage = base->CommitFailStage;
        info->Size = RADEON3D_INFO_V3_SIZE;
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


static void FreeExecuteEmitter(struct RadeonChipBase *base,
                               struct Radeon3DDevice *device)
{
    struct ExecBase *SysBase = base->ExecBase;

    if (!device->ExecuteEmitter)
        return;
    FreeMem(device->ExecuteEmitter, sizeof(*device->ExecuteEmitter));
    device->ExecuteEmitter = NULL;
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

/* Execute buffers are allocated on first use. Both the record path and the
 * streaming commit path need them, so a client whose very first submission
 * is a commit must not find them missing. */
/* Emitter resolve hook: maps a record's raw surface handle to a value
 * descriptor through the session's surface table. This is the only
 * device-dependent piece of record processing. */
static struct Radeon3DEmitSurface *Radeon3DEmitResolveSurface(
    void *user, ULONG handleValue, struct Radeon3DEmitSurface *slot)
{
    struct Radeon3DDevice *device = user;
    struct Radeon3DSurfaceHandle *surface =
        FindSurfaceHandle(device, (APTR)handleValue);

    if (!surface)
        return NULL;
    slot->CpuAddress = surface->CpuAddress;
    slot->GpuAddress = surface->GpuAddress;
    slot->Pitch = surface->Pitch;
    slot->Width = surface->Width;
    slot->Height = surface->Height;
    slot->Format = surface->Format;
    return slot;
}

static BOOL EnsureExecuteBuffers(struct RadeonChipBase *base,
                                 struct Radeon3DDevice *device)
{
    struct ExecBase *SysBase = base->ExecBase;

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
    if (device->ExecuteTrusted && device->ExecuteGenerated &&
        device->ExecuteEmitter) {
        device->ExecuteEmitter->Resolve = Radeon3DEmitResolveSurface;
        device->ExecuteEmitter->ResolveUser = device;
        device->ExecuteEmitter->InterfaceVersion =
            device->InterfaceVersion;
        device->ExecuteEmitter->FailStage = 0;
        return TRUE;
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
    return FALSE;
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
    struct Radeon3DEmitter *emitter;
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
    if (!EnsureExecuteBuffers(base, device)) {
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
    emitter->State.TextureState = 0;
    emitter->State.Texture1State = 0;
    emitter->GuardClipEmitted = FALSE;
    emitter->MatrixValid = FALSE;
    emitter->TexGenMatrixValid[0] = FALSE;
    emitter->TexGenMatrixValid[1] = FALSE;
    emitter->ModelViewValid = FALSE;
    emitter->InvModelViewValid = FALSE;
    emitter->CommitVbuf = FALSE;
    emitter->CommitSegmentBytes = 0;
    emitter->CommitVbufAddress = 0;
    buildStart = RDEBUG_PHASE_BEGIN();
    {
        ULONG phase = ServiceExecMicros(base);
        result = Radeon3DEmitStream(emitter, trusted, recordDwords);
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
#ifdef DEBUG
    if (FirstExecuteDumpPending) {
        FirstExecuteDumpPending = 0;
        DumpStreamFile(base->ExecBase, "T:r3d_first_execute.bin",
                       records, recordDwords, generated, emitter->Count);
    }
#endif
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
    ULONG index;

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
    data = RadeonGetBoardData(bi);
    if (!data || !base->StreamSegmentPool) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    for (index = 0; index < RADEON3D_MAX_SEGMENTS; ++index) {
        if (!(base->StreamSegmentMask & (1UL << index))) {
            slot = &device->Segments[index];
            break;
        }
    }
    if (!slot) {
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    base->StreamSegmentMask |= 1UL << index;
    slot->CpuAddress = (APTR)((ULONG)base->StreamSegmentPool +
                             index * RADEON3D_MAX_SEGMENT_BYTES);
    slot->GpuAddress = base->StreamSegmentGpuBase +
                       index * RADEON3D_MAX_SEGMENT_BYTES;
    slot->Bytes = bytes;
    slot->Allocated = TRUE;
    segment->Version = RADEON3D_SEGMENT_VERSION;
    segment->Id = index;
    segment->CpuAddress = slot->CpuAddress;
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
    base->StreamSegmentMask &= ~(1UL << segmentId);
    slot->CpuAddress = NULL;
    slot->GpuAddress = 0;
    slot->Bytes = 0;
    slot->Allocated = FALSE;
    UnlockServiceBoard(base, bi, device);
    return TRUE;
}

/* Diagnostic: which check rejected the last streaming commit. Reported
 * through the info block so a client can print it after a failure. */
#define COMMIT_FAIL(base, stage) \
    do { (base)->CommitFailStage = (stage); } while (0)

static BOOL CommitRecords(
    struct Radeon3DDevice *device, struct BoardInfo *bi,
    const ULONG *records, ULONG recordDwords,
    const ULONG *vertexOffsets, ULONG recordCount,
    const struct Radeon3DSegmentSlot *slot, ULONG flags, ULONG *fenceOut)
{
    struct ExecBase *SysBase = bi->ExecBase;
    struct RadeonChipBase *base = device->Base;
    struct Radeon3DEmitter *emitter;
    ULONG *trusted;
    ULONG *generated;
    ULONG internalFence = 0;
    ULONG index;
    ULONG walk;
    ULONG phase;
    BOOL streamBuilt;
    BOOL result;

    if (fenceOut)
        *fenceOut = 0;
    if (!SysBase || !records || !recordDwords ||
        recordDwords > RADEON3D_MAX_BATCH_DWORDS ||
        !vertexOffsets || !recordCount || !slot ||
        (flags & ~RADEON3D_SUBMIT_FLAGS)) {
        COMMIT_FAIL(base, 1UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 1UL;
        return FALSE;
    }
    if (device->InterfaceVersion < 13UL ||
        !EnsureExecuteBuffers(base, device)) {
        COMMIT_FAIL(base, 2UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 2UL;
        return FALSE;
    }
    /* Draw records carry no inline vertices here, so each one consumes a
     * fetch offset; clears may ride along and consume none. Walk the chain
     * once up front to check the record count matches the offsets the
     * caller supplied, keeping the emitter's per-draw counter in sync. */
    walk = 0;
    index = 0;
    while (walk < recordDwords) {
        ULONG opcode;
        ULONG length;

        if (walk + 2UL > recordDwords)
            return FALSE;
        opcode = records[walk];
        length = records[walk + 1UL];
        if (length < 2UL || length > recordDwords - walk) {
            COMMIT_FAIL(base, 3UL);
            if (fenceOut)
                *fenceOut = 0x80000000UL | 3UL;
            return FALSE;
        }
        if ((opcode >= RADEON3D_EXEC_DRAW_TRIANGLES &&
             opcode <= RADEON3D_EXEC_DRAW_LINE_LOOP) ||
            (device->InterfaceVersion >= 14UL &&
             opcode >= RADEON3D_EXEC_REUSE_TRIANGLES &&
             opcode <= RADEON3D_EXEC_REUSE_LINE_LOOP))
            ++index;
        else if (opcode != RADEON3D_EXEC_CLEAR ||
                 length != RADEON3D_EXEC_CLEAR_DWORDS) {
            COMMIT_FAIL(base, 4UL);
            if (fenceOut)
                *fenceOut = 0x80000000UL | 4UL;
            return FALSE;
        }
        walk += length;
    }
    if (walk != recordDwords || index != recordCount) {
        COMMIT_FAIL(base, 5UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 5UL;
        return FALSE;
    }
    trusted = device->ExecuteTrusted;
    generated = device->ExecuteGenerated;
    emitter = device->ExecuteEmitter;
    base->CommitFailStage = 0;
    phase = ServiceExecMicros(base);
    ExecCopyRecords(trusted, records, recordDwords);
    base->ExecCopyMicros += ServiceExecMicros(base) - phase;
    emitter->Words = generated;
    emitter->Count = 0;
    emitter->StateValid = FALSE;
    emitter->State.TextureState = 0;
    emitter->State.Texture1State = 0;
    emitter->GuardClipEmitted = FALSE;
    emitter->MatrixValid = FALSE;
    emitter->TexGenMatrixValid[0] = FALSE;
    emitter->TexGenMatrixValid[1] = FALSE;
    emitter->ModelViewValid = FALSE;
    emitter->InvModelViewValid = FALSE;
    emitter->CommitVbuf = TRUE;
    emitter->CommitSegmentGpuBase = slot->GpuAddress;
    emitter->CommitSegmentBytes = slot->Bytes;
    emitter->CommitVertexOffsets = vertexOffsets;
    emitter->CommitDrawIndex = 0;
    phase = ServiceExecMicros(base);
    streamBuilt = Radeon3DEmitStream(emitter, trusted, recordDwords);
    result = streamBuilt;
    if (result)
        result = RadeonPrepare3D(bi);
    base->ExecBuildMicros += ServiceExecMicros(base) - phase;
    emitter->CommitVbuf = FALSE;
    emitter->CommitSegmentGpuBase = 0;
    emitter->CommitSegmentBytes = 0;
    emitter->CommitVertexOffsets = NULL;
    emitter->CommitDrawIndex = 0;
    emitter->CommitVbufAddress = 0;
    emitter->StateValid = FALSE;
    if (!result) {
        if (!streamBuilt && !base->CommitFailStage)
            COMMIT_FAIL(base, 6UL);
        else if (streamBuilt)
            COMMIT_FAIL(base, 7UL);
        /* Encode the emitter stage for clients whose info block predates
         * CommitFailStage: fenceOut keeps 0 for success only. */
        if (fenceOut)
            *fenceOut = 0x80000000UL | (6UL << 16) |
                        (base->CommitFailStage & 0xffffUL);
        (void)RadeonRecoverAcceleration(bi);
        return FALSE;
    }
    phase = ServiceExecMicros(base);
    result = RadeonCpSubmitStream(bi, generated, emitter->Count, TRUE,
                                  &internalFence);
    base->ExecSubmitMicros += ServiceExecMicros(base) - phase;
    if (result) {
        device->LastFence = internalFence;
        RadeonMark3DSubmitted(bi);
        if ((flags & RADEON3D_SUBMIT_FENCE) && fenceOut)
            *fenceOut = internalFence;
    } else {
        COMMIT_FAIL(base, 8UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | (8UL << 16) |
                        (emitter->Count & 0xffffUL);
        (void)RadeonRecoverAcceleration(bi);
    }
    base->ExecCalls++;
    base->ExecRecordDwords += recordDwords;
    base->ExecGeneratedDwords += emitter->Count;
    return result;
}

BOOL Radeon3DCommitDraw(
    __REGA0(struct Radeon3DDevice *device),
    __REGA1(const struct Radeon3DCommit *commit),
    __REGA2(ULONG *fenceOut),
    __REGA6(struct RadeonChipBase *base))
{
    struct BoardInfo *bi;
    struct Radeon3DSegmentSlot *slot;
    BOOL result;

    if (fenceOut)
        *fenceOut = 0;
    if (!base || !commit ||
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
    result = CommitRecords(device, bi, commit->Header, commit->HeaderDwords,
                           &commit->OffsetBytes, 1UL, slot, commit->Flags,
                           fenceOut);
    UnlockServiceBoard(base, bi, device);
    return result;
}

BOOL Radeon3DCommitBatch(
    __REGA0(struct Radeon3DDevice *device),
    __REGA1(const struct Radeon3DCommitBatch *commit),
    __REGA2(ULONG *fenceOut),
    __REGA6(struct RadeonChipBase *base))
{
    struct BoardInfo *bi;
    struct Radeon3DSegmentSlot *slot;
    ULONG index;
    BOOL result;

    if (base)
        COMMIT_FAIL(base, 40UL);
    if (fenceOut)
        *fenceOut = 0x80000000UL | 40UL;
    if (!base || !commit ||
        commit->Size < RADEON3D_COMMIT_BATCH_V1_SIZE ||
        commit->Version != RADEON3D_COMMIT_BATCH_VERSION ||
        !commit->Records || !commit->RecordDwords ||
        !commit->VertexOffsets || !commit->RecordCount ||
        commit->RecordDwords > RADEON3D_MAX_BATCH_DWORDS ||
        (commit->Flags & ~RADEON3D_SUBMIT_FLAGS)) {
        if (base)
            COMMIT_FAIL(base, 41UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 41UL;
        return FALSE;
    }
    bi = LockServiceBoard(base, &device);
    if (!bi) {
        COMMIT_FAIL(base, 42UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 42UL;
        return FALSE;
    }
    if (commit->SegmentId >= RADEON3D_MAX_SEGMENTS ||
        !device->Segments[commit->SegmentId].Allocated) {
        UnlockServiceBoard(base, bi, device);
        COMMIT_FAIL(base, 43UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 43UL;
        return FALSE;
    }
    slot = &device->Segments[commit->SegmentId];
    for (index = 0; index < commit->RecordCount; ++index) {
        if (commit->VertexOffsets[index] >= slot->Bytes) {
            UnlockServiceBoard(base, bi, device);
            COMMIT_FAIL(base, 44UL);
            if (fenceOut)
                *fenceOut = 0x80000000UL | 44UL;
            return FALSE;
        }
    }
    result = CommitRecords(device, bi, commit->Records, commit->RecordDwords,
                           commit->VertexOffsets, commit->RecordCount, slot,
                           commit->Flags, fenceOut);
#ifdef DEBUG
    if (FirstCommitDumpPending) {
        FirstCommitDumpPending = 0;
        DumpStreamFile(base->ExecBase, "T:r3d_first_commit.bin",
                       commit->Records, commit->RecordDwords, NULL, 0UL);
    }
#endif
    UnlockServiceBoard(base, bi, device);
    return result;
}

BOOL Radeon3DCommitStateBatch(
    __REGA0(struct Radeon3DDevice *device),
    __REGA1(const struct Radeon3DStateBatch *batch),
    __REGA2(ULONG *fenceOut),
    __REGA6(struct RadeonChipBase *base))
{
    struct Radeon3DStateBatch request;
    struct Radeon3DStateBatchDraw *draws;
    struct Radeon3DSegmentSlot *slot;
    struct Radeon3DEmitter *emitter;
    struct BoardInfo *bi;
    ULONG *trusted;
    ULONG *generated;
    ULONG primitiveType;
    ULONG internalFence = 0;
    ULONG stagedDwords;
    ULONG phase;
    ULONG index;
    BOOL streamBuilt;
    BOOL result;

    if (fenceOut)
        *fenceOut = 0;
    if (!base || !batch)
        return FALSE;
    request = *batch;
    if (request.Size < RADEON3D_STATE_BATCH_V1_SIZE ||
        request.Version != RADEON3D_STATE_BATCH_VERSION ||
        !request.Header ||
        request.HeaderDwords < RADEON3D_EXEC_DRAW_HW_TCL_HEADER_DWORDS ||
        request.HeaderDwords > RADEON3D_MAX_BATCH_DWORDS ||
        !request.Draws || !request.DrawCount ||
        request.DrawCount > RADEON3D_STATE_BATCH_MAX_DRAWS ||
        request.DrawCount >
            (RADEON3D_MAX_BATCH_DWORDS - request.HeaderDwords) /
                (RADEON3D_STATE_BATCH_DRAW_V1_SIZE / sizeof(ULONG)) ||
        !Radeon3DEmitPrimitiveType(request.Primitive, &primitiveType) ||
        (request.Flags & ~RADEON3D_SUBMIT_FLAGS)) {
        COMMIT_FAIL(base, 80UL);
        return FALSE;
    }
    stagedDwords = request.HeaderDwords +
                   request.DrawCount *
                       (RADEON3D_STATE_BATCH_DRAW_V1_SIZE / sizeof(ULONG));
    bi = LockServiceBoard(base, &device);
    if (!bi)
        return FALSE;
    if (device->InterfaceVersion < 15UL ||
        request.Generation != device->Generation ||
        request.Generation != base->ServiceGeneration ||
        request.SegmentId >= RADEON3D_MAX_SEGMENTS ||
        !device->Segments[request.SegmentId].Allocated ||
        !EnsureExecuteBuffers(base, device)) {
        COMMIT_FAIL(base, 81UL);
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    slot = &device->Segments[request.SegmentId];
    trusted = device->ExecuteTrusted;
    generated = device->ExecuteGenerated;
    emitter = device->ExecuteEmitter;

    base->CommitFailStage = 0;
    phase = ServiceExecMicros(base);
    ExecCopyRecords(trusted, request.Header, request.HeaderDwords);
    draws = (struct Radeon3DStateBatchDraw *)(trusted +
                                              request.HeaderDwords);
    for (index = 0; index < request.DrawCount; ++index)
        draws[index] = request.Draws[index];
    base->ExecCopyMicros += ServiceExecMicros(base) - phase;

    if (trusted[0] != request.Primitive ||
        trusted[1] != request.HeaderDwords) {
        COMMIT_FAIL(base, 82UL);
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    for (index = 0; index < request.DrawCount; ++index) {
        if ((draws[index].OffsetBytes & 3UL) ||
            draws[index].OffsetBytes >= slot->Bytes ||
            !draws[index].VertexCount ||
            slot->GpuAddress > ~0UL - draws[index].OffsetBytes) {
            COMMIT_FAIL(base, 83UL);
            UnlockServiceBoard(base, bi, device);
            return FALSE;
        }
    }

    emitter->Words = generated;
    emitter->Count = 0;
    emitter->StateValid = FALSE;
    emitter->State.TextureState = 0;
    emitter->State.Texture1State = 0;
    emitter->GuardClipEmitted = FALSE;
    emitter->MatrixValid = FALSE;
    emitter->TexGenMatrixValid[0] = FALSE;
    emitter->TexGenMatrixValid[1] = FALSE;
    emitter->ModelViewValid = FALSE;
    emitter->InvModelViewValid = FALSE;
    emitter->CommitVbuf = TRUE;
    emitter->CommitSegmentGpuBase = slot->GpuAddress;
    emitter->CommitSegmentBytes = slot->Bytes;
    emitter->CommitVertexOffsets = &draws[0].OffsetBytes;
    emitter->CommitDrawIndex = 0;

    /* The first descriptor drives the normal parser and emits the complete
     * state. Remaining descriptors draw directly from that validated state. */
    trusted[10] = draws[0].VertexCount;
    phase = ServiceExecMicros(base);
    streamBuilt = Radeon3DEmitDraw(emitter, trusted,
                                  request.HeaderDwords, primitiveType);
    for (index = 1; streamBuilt && index < request.DrawCount; ++index)
        streamBuilt = Radeon3DEmitVbufDraw(emitter, draws[index].OffsetBytes,
            draws[index].VertexCount, primitiveType);
    result = streamBuilt;
    if (result)
        result = RadeonPrepare3D(bi);
    base->ExecBuildMicros += ServiceExecMicros(base) - phase;
    emitter->CommitVbuf = FALSE;
    if (!result) {
        if (!streamBuilt && !base->CommitFailStage)
            COMMIT_FAIL(base, 84UL);
        else if (streamBuilt)
            COMMIT_FAIL(base, 85UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | (9UL << 16) |
                        (base->CommitFailStage & 0xffffUL);
        (void)RadeonRecoverAcceleration(bi);
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }

    phase = ServiceExecMicros(base);
    result = RadeonCpSubmitStream(bi, generated, emitter->Count, TRUE,
                                  &internalFence);
    base->ExecSubmitMicros += ServiceExecMicros(base) - phase;
    if (result) {
        device->LastFence = internalFence;
        RadeonMark3DSubmitted(bi);
        if ((request.Flags & RADEON3D_SUBMIT_FENCE) && fenceOut)
            *fenceOut = internalFence;
    } else {
        COMMIT_FAIL(base, 86UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | (10UL << 16) |
                        (emitter->Count & 0xffffUL);
        (void)RadeonRecoverAcceleration(bi);
    }
    base->ExecCalls++;
    base->ExecRecordDwords += stagedDwords;
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
