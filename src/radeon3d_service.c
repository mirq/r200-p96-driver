#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <devices/timer.h>

#include "radeon9200.h"
#include "radeon_debug.h"
#include "radeon_regs.h"
#include "radeon3d_emit.h"

#define RADEON3D_SESSION_MAGIC 0x52334453UL

/* Diagnostic: which check rejected the most recent submission. Reported
 * through the info block so a client can print it after a failure. Defined
 * before every submission path that uses it. */
#define COMMIT_FAIL(base, stage) \
    do { (base)->CommitFailStage = (stage); } while (0)


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
    /* Non-zero PoolBytes marks a Radeon3DAllocSurface() surface: the VRAM
     * belongs to the aux pool and goes back to it on release. An imported
     * BitMap leaves both fields zero. */
    ULONG PoolOffset;
    ULONG PoolBytes;
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
 * Submission-phase attribution clock, live in every build. Opens timer.device
 * once on the first session open and brackets each phase with ReadEClock():
 * one direct device call per boundary, no message round trip, which the
 * DEBUG microbenchmark measures at well under a microsecond -- the old
 * per-phase TR_GETSYSTIME DoIOs were the reason these totals were
 * DEBUG-only. Cumulative info totals stay microseconds (64-bit conversion,
 * one divide per phase boundary); the per-submission sample ring keeps raw
 * ticks and lets the reader convert with EClockHz. Unsigned delta
 * arithmetic absorbs the 2^32 tick wrap because real intervals are orders
 * of magnitude shorter. EClock resolution is about 1.4 us on the reference
 * 68060 -- accumulate many samples before trusting per-call numbers.
 */
static void EnsureExecTimer(struct RadeonChipBase *base)
{
    struct ExecBase *SysBase = base->ExecBase;
    struct MsgPort *port;
    struct timerequest *io;
    struct Device *TimerBase;
    struct EClockVal value;

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
    TimerBase = io->tr_node.io_Device;
    base->ExecClockHz = (ULONG)ReadEClock(&value);
    if (!base->ExecSampleRing)
        base->ExecSampleRing = AllocMem(
            RADEON3D_SAMPLE_RING_SIZE * sizeof(struct Radeon3DSample),
            MEMF_PUBLIC);
}

/* Raw EClock low word; callers take unsigned deltas. */
static ULONG ServiceExecTicks(struct RadeonChipBase *base)
{
    struct Device *TimerBase;
    struct EClockVal value;
    struct timerequest *io = (struct timerequest *)base->ExecTimerIO;

    if (!io)
        return 0UL;
    TimerBase = io->tr_node.io_Device;
    ReadEClock(&value);
    return value.ev_lo;
}

/* Lease-expiry clock for the 2D paths (radeon_accel.c has no timer). */
ULONG Radeon3DNow(struct RadeonChipBase *base)
{
    return ServiceExecTicks(base);
}

static ULONG ExecTicksToMicros(const struct RadeonChipBase *base,
                               ULONG ticks)
{
    ULONG hz = base->ExecClockHz;

    if (!hz)
        return 0UL;
    return (ULONG)((unsigned long long)ticks * 1000000ULL / hz);
}

/* One sample per submission, written with the service lock held. Clear Seq
 * before reusing a slot and publish the new Seq last so lock-free readers can
 * reject an entry while its payload is changing. */
static void RecordExecSample(struct RadeonChipBase *base, ULONG type,
                             ULONG recordDwords, ULONG generatedDwords,
                             ULONG copyTicks, ULONG buildTicks,
                             ULONG submitTicks, BOOL ok)
{
    struct Device *TimerBase;
    struct EClockVal value;
    struct timerequest *io = (struct timerequest *)base->ExecTimerIO;
    struct Radeon3DSample *samples = base->ExecSampleRing;
    struct Radeon3DSample *sample;
    ULONG seq;

    if (!samples || !io)
        return;
    TimerBase = io->tr_node.io_Device;
    ReadEClock(&value);
    seq = base->ExecSampleSeq + 1UL;
    sample = &samples[(seq - 1UL) & (RADEON3D_SAMPLE_RING_SIZE - 1UL)];
    sample->Seq = 0UL;
    __asm__ __volatile__ ("" ::: "memory");
    sample->WallTicks = value.ev_lo;
    sample->Type = type;
    sample->Result = ok ? 1UL : 0UL;
    sample->RecordDwords = recordDwords;
    sample->GeneratedDwords = generatedDwords;
    sample->CopyTicks = copyTicks;
    sample->BuildTicks = buildTicks;
    sample->SubmitTicks = submitTicks;
    __asm__ __volatile__ ("" ::: "memory");
    sample->Seq = seq;
    base->ExecSampleSeq = seq;
}

void Radeon3DFreeExecTimer(struct RadeonChipBase *base)
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct timerequest *io;

    if (!SysBase)
        return;
    io = (struct timerequest *)base->ExecTimerIO;
    if (io) {
        CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io);
        base->ExecTimerIO = NULL;
    }
    if (base->ExecTimerPort) {
        DeleteMsgPort((struct MsgPort *)base->ExecTimerPort);
        base->ExecTimerPort = NULL;
    }
    if (base->ExecSampleRing) {
        FreeMem(base->ExecSampleRing,
                RADEON3D_SAMPLE_RING_SIZE * sizeof(struct Radeon3DSample));
        base->ExecSampleRing = NULL;
    }
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

/* Sub-allocator for the aux surface pool. First fit with the block table
 * scanned repeatedly instead of kept sorted: at most RADEON3D_MAX_AUX_SURFACES
 * blocks are live and nothing here runs per frame. Every offset and size is
 * AUX_POOL_ALIGNMENT-aligned, which also satisfies the 16-byte GpuAddress
 * alignment the depth unit requires. Callers hold ServiceLock. */
#define AUX_POOL_ALIGNMENT 4096UL
#define AUX_POOL_ALIGN_MASK (AUX_POOL_ALIGNMENT - 1UL)

static BOOL AuxPoolAlloc(struct RadeonChipBase *base, ULONG bytes,
                         ULONG *offsetOut, ULONG *bytesOut)
{
    ULONG candidate = 0;
    ULONG index;
    BOOL moved = TRUE;

    if (!base->AuxSurfacePool || !bytes ||
        bytes > base->AuxSurfaceBytes - AUX_POOL_ALIGN_MASK)
        return FALSE;
    bytes = (bytes + AUX_POOL_ALIGN_MASK) & ~AUX_POOL_ALIGN_MASK;
    /* Push the candidate past every block it overlaps until a pass finds no
     * overlap; each push strictly increases it, so this terminates. */
    while (moved) {
        moved = FALSE;
        if (candidate > base->AuxSurfaceBytes ||
            bytes > base->AuxSurfaceBytes - candidate)
            return FALSE;
        for (index = 0; index < RADEON3D_MAX_AUX_SURFACES; ++index) {
            struct Radeon3DAuxBlock *block = &base->AuxSurfaceBlocks[index];

            if (!block->Bytes)
                continue;
            if (candidate < block->Offset + block->Bytes &&
                block->Offset < candidate + bytes) {
                candidate = block->Offset + block->Bytes;
                moved = TRUE;
            }
        }
    }
    for (index = 0; index < RADEON3D_MAX_AUX_SURFACES; ++index) {
        struct Radeon3DAuxBlock *block = &base->AuxSurfaceBlocks[index];

        if (block->Bytes)
            continue;
        block->Offset = candidate;
        block->Bytes = bytes;
        *offsetOut = candidate;
        *bytesOut = bytes;
        return TRUE;
    }
    return FALSE;
}

static void AuxPoolFree(struct RadeonChipBase *base, ULONG offset,
                        ULONG bytes)
{
    ULONG index;

    for (index = 0; index < RADEON3D_MAX_AUX_SURFACES; ++index) {
        struct Radeon3DAuxBlock *block = &base->AuxSurfaceBlocks[index];

        if (block->Bytes != bytes || block->Offset != offset)
            continue;
        block->Offset = 0;
        block->Bytes = 0;
        return;
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
        if (surface->PoolBytes)
            AuxPoolFree(base, surface->PoolOffset, surface->PoolBytes);
        FreeMem(surface, sizeof(*surface));
    }
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
    /* Bits 7-12 and 14-20 were the semantic record capabilities; they are
     * no longer advertised (see the retired-bits note in radeon3d.h). */
#ifdef DEBUG
    if (interfaceVersion >= 5UL)
        info->Caps |= RADEON3D_CAP_TEST_INVALIDATE;
#endif
    if (interfaceVersion >= 13UL && base->StreamSegmentPool)
        info->Caps |= RADEON3D_CAP_STREAM_SEGMENTS;
    if (interfaceVersion >= 17UL && base->AuxSurfacePool)
        info->Caps |= RADEON3D_CAP_AUX_SURFACES;
    if (interfaceVersion >= 18UL && base->StreamSegmentPool)
        info->Caps |= RADEON3D_CAP_INDIRECT_DISPATCH |
                      RADEON3D_CAP_INDIRECT_RENDER;
    /* Ranged fence validation ships in the same service build as interface
     * 18; a driver from before this bit does not set it. */
    if (interfaceVersion >= 18UL)
        info->Caps |= RADEON3D_CAP_MULTI_FENCE;
    /* Interface 19: fence-less indirect dispatch + Radeon3DSubmitFence. */
    if (interfaceVersion >= 19UL)
        info->Caps |= RADEON3D_CAP_FENCE_COALESCE;
    /* Interface 20: PPC engine lease. Gated by the fallback flag, the
     * streaming pool and a working EClock (the lease expiry needs it). */
    if (interfaceVersion >= 20UL && base->PpcRingAllowed &&
        base->StreamSegmentPool && base->ExecClockHz)
        info->Caps |= RADEON3D_CAP_PPC_RING;
    if (RadeonCpIsReady(bi))
        info->Caps |= RADEON3D_CAP_CP_READY;
    info->InstalledVram = data ? data->InstalledVram : 0;
    info->Picasso96Vram = bi ? bi->MemorySize : 0;
    info->MaxBatchDwords = RADEON3D_MAX_BATCH_DWORDS;
    /* Size-gated tails: a caller passing a smaller buffer must not see
     * anything beyond it touched, and Size reports back the largest tier
     * actually filled. Each tier only raises Size, so a caller sitting
     * between two tiers keeps the lower one -- V2 used to be clobbered
     * back to V1 by an else on the V3 test. */
    if (requestedSize >= RADEON3D_INFO_V2_SIZE) {
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
    }
    if (requestedSize >= RADEON3D_INFO_V4_SIZE) {
        info->SampleRing = base->ExecSampleRing;
        info->SampleRingEntries = base->ExecSampleRing
                                      ? RADEON3D_SAMPLE_RING_SIZE
                                      : 0UL;
        info->SampleSeq = base->ExecSampleSeq;
        info->EClockHz = base->ExecClockHz;
        info->Size = RADEON3D_INFO_V4_SIZE;
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
     * not an internal choice: it must keep matching
     * docs/02-service-abi-reference.md exactly. Alpha shading is deliberately
     * absent because the stream only targets RGB565, which has no alpha
     * channel to interpolate. */
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
    /* Skip leading PACKET2 fill dwords, then the remainder must pass the
     * immediate-triangle-list exact match. The fill skip is not optional:
     * ValidateTriangleBatch anchors on a PACKET0 header at index 0, so a
     * fill-prefixed mixed batch must be rejected here rather than waved
     * through to the CP. An all-fill batch is the idle submission. */
    for (index = 0; index < commandCount; ++index) {
        if (commands[index] != 0x80000000UL)
            return ValidateTriangleBatch(device, commands, commandCount);
    }
    return TRUE;
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
        /* A session closing while it still holds the lease ends the lease:
         * its owner is going away, so a bounded full idle drain covers the
         * PPC's untracked work and the next 2D operation restores the
         * baseline. */
        if (base->LeaseActive && base->LeaseDevice == device) {
            base->LeaseActive = FALSE;
            base->LeaseDevice = NULL;
            base->LeaseGrantTicks = 0;
            (void)RadeonCpWaitIdle(bi);
            RadeonCpAdoptFence(bi, 0UL);
            RadeonMark3DSubmitted(bi);
        }
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
    ULONG copyTicks = 0;
    ULONG buildTicks = 0;
    ULONG submitTicks = 0;
    ULONG phase;
    ULONG generatedDwords = 0;
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
    phase = ServiceExecTicks(base);
    for (index = 0; index < commandCount; ++index)
        trusted[index] = commands[index];
    copyTicks = ServiceExecTicks(base) - phase;
    bi = LockServiceBoard(base, &device);
    if (!bi) {
        FreeMem(trusted, commandCount * sizeof(*trusted));
        return FALSE;
    }
    /* A live PPC lease owns the ring: every 68k submission is refused
     * (stage 122), and callers fall back to their non-submitting paths. */
    if (base->LeaseActive) {
        COMMIT_FAIL(base, 122UL);
        UnlockServiceBoard(base, bi, device);
        FreeMem(trusted, commandCount * sizeof(*trusted));
        return FALSE;
    }
    phase = ServiceExecTicks(base);
    result = ValidateBatch(device, trusted, commandCount);
    if (result)
        result = RadeonPrepare3D(bi);
    buildTicks = ServiceExecTicks(base) - phase;
    base->ExecBuildMicros += ExecTicksToMicros(base, buildTicks);
    if (result) {
        submitAttempted = TRUE;
        phase = ServiceExecTicks(base);
        result = RadeonCpSubmitStream(bi, trusted, commandCount, TRUE,
                                      &internalFence);
        submitTicks = ServiceExecTicks(base) - phase;
        base->ExecSubmitMicros += ExecTicksToMicros(base, submitTicks);
    }
    if (result && internalFence) {
        device->LastFence = internalFence;
        RadeonMark3DSubmitted(bi);
        if ((flags & RADEON3D_SUBMIT_FENCE) && fenceOut)
            *fenceOut = internalFence;
    } else if (submitAttempted) {
        (void)RadeonRecoverAcceleration(bi);
    }
    base->ExecCalls++;
    base->ExecRecordDwords += commandCount;
    if (submitAttempted)
        generatedDwords = commandCount;
    base->ExecGeneratedDwords += generatedDwords;
    RecordExecSample(base, RADEON3D_SAMPLE_SUBMIT, commandCount,
                     generatedDwords, copyTicks, buildTicks, submitTicks,
                     result);
    UnlockServiceBoard(base, bi, device);
    FreeMem(trusted, commandCount * sizeof(*trusted));
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

static ULONG AuxFormatBytesPerPixel(ULONG format)
{
    switch (format) {
    case RADEON3D_FORMAT_CLUT8:
        return 1UL;
    case RADEON3D_FORMAT_R5G6B5PC:
        return 2UL;
    case RADEON3D_FORMAT_B8G8R8A8:
        return 4UL;
    }
    return 0;
}

/* Allocate a render surface out of the private VRAM pool (interface 17).
 * Picasso96 cannot see this memory: it was carved off bi->MemorySize before
 * the board went live, so unlike an imported BitMap the result can never
 * collide with a p96 bitmap or an AllocScreenBuffer() buffer. Pitch is padded
 * to AUX_SURFACE_PITCH_ALIGNMENT so tiled depth is legal at widths whose
 * natural pitch is not a whole number of tile pairs (800 pixels of 16-bit
 * depth is 1600 bytes, 12.5 pairs), and Width reports the padded pixel count.
 * Release with Radeon3DReleaseSurface(). */
#define AUX_SURFACE_PITCH_ALIGNMENT 128UL
#define AUX_SURFACE_PITCH_MASK (AUX_SURFACE_PITCH_ALIGNMENT - 1UL)

BOOL Radeon3DAllocSurface(
    __REGA0(struct Radeon3DDevice *device),
    __REGD0(ULONG width),
    __REGD1(ULONG height),
    __REGD2(ULONG format),
    __REGA1(struct Radeon3DSurface *surface),
    __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct BoardInfo *bi;
    struct Radeon3DSurfaceHandle *handle = NULL;
    ULONG bytesPerPixel;
    ULONG pitch;
    ULONG offset = 0;
    ULONG poolBytes = 0;
    BOOL added = FALSE;

    if (!SysBase || !surface || surface->Size < RADEON3D_SURFACE_V1_SIZE)
        return FALSE;
    bytesPerPixel = AuxFormatBytesPerPixel(format);
    if (!bytesPerPixel || !width || !height ||
        width > RADEON3D_AUX_MAX_DIMENSION ||
        height > RADEON3D_AUX_MAX_DIMENSION)
        return FALSE;
    surface->Version = 0;
    surface->Handle = NULL;
    bi = LockServiceBoard(base, &device);
    if (!bi)
        return FALSE;
    if (device->InterfaceVersion < 17UL || !base->AuxSurfacePool)
        goto out;
    pitch = (width * bytesPerPixel + AUX_SURFACE_PITCH_MASK) &
            ~AUX_SURFACE_PITCH_MASK;
    handle = AllocMem(sizeof(*handle), MEMF_PUBLIC | MEMF_CLEAR);
    if (!handle)
        goto out;

    ObtainSemaphore(&base->ServiceLock);
    if (FindActiveDevice(base, device) &&
        IsUsableDevice(base, device) && !device->Closing &&
        AuxPoolAlloc(base, pitch * height, &offset, &poolBytes)) {
        handle->Device = device;
        handle->BitMap = NULL;
        handle->CpuAddress = (APTR)((ULONG)base->AuxSurfacePool + offset);
        handle->GpuAddress = base->AuxSurfaceGpuBase + offset;
        handle->Pitch = pitch;
        handle->Width = pitch / bytesPerPixel;
        handle->Height = height;
        handle->Format = format;
        handle->PoolOffset = offset;
        handle->PoolBytes = poolBytes;
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

BOOL Radeon3DDispatchIndirect(
    __REGA0(struct Radeon3DDevice *device),
    __REGA1(const struct Radeon3DIndirect *indirect),
    __REGA2(ULONG *fenceOut),
    __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct BoardInfo *bi;
    struct Radeon3DSegmentSlot *slot;
    struct Radeon3DSegmentSlot lease;
    struct Radeon3DIndirect request;
    ULONG commands[3];
    ULONG generation;
    ULONG phase, submitTicks;
    ULONG fence = 0;
    BOOL addFence;
    BOOL result;

    if (base)
        COMMIT_FAIL(base, 100UL);
    if (fenceOut)
        *fenceOut = 0x80000000UL | 100UL;
    if (!base || !indirect ||
        indirect->Size < RADEON3D_INDIRECT_V1_SIZE) {
        if (base)
            COMMIT_FAIL(base, 101UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 101UL;
        return FALSE;
    }
    /* Validate and consume one fixed-size snapshot, even if the caller
     * changes its descriptor while Prepare synchronizes the engine. */
    request = *indirect;
    if (request.Size < RADEON3D_INDIRECT_V1_SIZE ||
        request.Version != RADEON3D_INDIRECT_VERSION ||
        (request.Flags & ~RADEON3D_INDIRECT_FLAGS) || !request.DwordCount ||
        (request.DwordCount & 1UL) ||
        request.DwordCount > RADEON3D_MAX_BATCH_DWORDS ||
        (request.ByteOffset & 15UL)) {
        COMMIT_FAIL(base, 101UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 101UL;
        return FALSE;
    }
    bi = LockServiceBoard(base, &device);
    if (!bi) {
        COMMIT_FAIL(base, 102UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 102UL;
        return FALSE;
    }
    if (device->InterfaceVersion < 18UL ||
        ((request.Flags & RADEON3D_INDIRECT_NO_FENCE) &&
         device->InterfaceVersion < 19UL) ||
        !base->StreamSegmentPool ||
        request.SegmentId >= RADEON3D_MAX_SEGMENTS ||
        !device->Segments[request.SegmentId].Allocated) {
        UnlockServiceBoard(base, bi, device);
        COMMIT_FAIL(base, 103UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 103UL;
        return FALSE;
    }
    /* A live PPC lease owns the ring: the indirect kick would race it. */
    if (base->LeaseActive) {
        UnlockServiceBoard(base, bi, device);
        COMMIT_FAIL(base, 122UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 122UL;
        return FALSE;
    }
    slot = &device->Segments[request.SegmentId];
    if (request.ByteOffset >= slot->Bytes ||
        request.DwordCount >
            (slot->Bytes - request.ByteOffset) / sizeof(ULONG)) {
        UnlockServiceBoard(base, bi, device);
        COMMIT_FAIL(base, 104UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 104UL;
        return FALSE;
    }
    if (slot->GpuAddress > ~(ULONG)0 - request.ByteOffset) {
        UnlockServiceBoard(base, bi, device);
        COMMIT_FAIL(base, 105UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 105UL;
        return FALSE;
    }
    generation = base->ServiceGeneration;
    lease = *slot;
    if (!RadeonPrepare3D(bi)) {
        COMMIT_FAIL(base, 107UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 107UL;
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    /* Prepare may recover successfully but invalidate this session. Keep
     * BoardLock held and recheck before any indirect setup or submission. */
    ObtainSemaphore(&base->ServiceLock);
    result = FindActiveDevice(base, device) && IsUsableDevice(base, device) &&
             base->ServiceGeneration == generation && base->BoardInfo == bi &&
             base->StreamSegmentPool && slot->Allocated &&
             slot->CpuAddress == lease.CpuAddress &&
             slot->GpuAddress == lease.GpuAddress && slot->Bytes == lease.Bytes;
    ReleaseSemaphore(&base->ServiceLock);
    if (!result) {
        COMMIT_FAIL(base, 108UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 108UL;
        UnlockServiceBoard(base, bi, device);
        return FALSE;
    }
    /* PACKET0(count=1) writes IB_BASE then IB_BUFSZ consecutively; the CP
     * fetches DwordCount dwords from the address and returns to the ring.
     * The retiring fence tail appended by RadeonCpSubmitStream executes
     * only after the indirect buffer, so a retired fence proves fetch. */
    commands[0] = RADEON_CP_PACKET0(RADEON_CP_IB_BASE, 1);
    commands[1] = slot->GpuAddress + request.ByteOffset;
    commands[2] = request.DwordCount;
    /* The guard below writes the engine baseline over MMIO. While an earlier
     * fence-less (interface-19) submission may still be fetching its
     * indirect buffer, that write can corrupt the in-flight fetch, so it is
     * withheld until the ring is quiescent. It is then still guaranteed to
     * run before the next fetch: only a 2D transition can clobber the
     * baseline, and every 2D transition drains first (RadeonCpWaitDrained),
     * which clears PendingUnfenced. */
    if (!RadeonCpUnfencedPending(bi))
    {
        ULONG csqMode = RadeonRead32(bi, RADEON_CP_CSQ_MODE);
        ULONG dpDatatype;

        if (csqMode != CP_CSQ_CACHE_PARTITION) {
            /* A clobbered partition starves the indirect fetch and stalls
             * the CP mid-buffer; restore before every dispatch. */
            (void)RadeonWrite32(bi, RADEON_CP_CSQ_MODE,
                                CP_CSQ_CACHE_PARTITION);
            csqMode = CP_CSQ_CACHE_PARTITION;
        }
        /* The indirect fetch follows DP_DATATYPE's host-endian bit while
         * the ring fetch does not: RestoreEngineState() sets
         * HOST_BIG_ENDIAN_EN on every Prepare3D transition, and with it
         * set the producer's CP-native (pre-swapped) stream decodes as
         * garbage and the CP stalls mid-buffer (2026-09-08 matrix). Clear
         * it for the dispatch; 2D host-data paths program their own swap
         * in RBBM_GUICNTL per operation and the next Prepare3D transition
         * restores the 2D baseline. */
        dpDatatype = RadeonRead32(bi, RADEON_DP_DATATYPE);
        if (dpDatatype & RADEON_HOST_BIG_ENDIAN_EN)
            (void)RadeonWrite32(bi, RADEON_DP_DATATYPE,
                                dpDatatype & ~RADEON_HOST_BIG_ENDIAN_EN);
        /* Diagnostic trail: mode byte + endian bit + write pointer of the
         * indirect queue, readable via GetInfo after a failed fence. */
        base->CommitFailStage = 0xc5000000UL | ((csqMode & 0xffUL) << 16) |
                                (((dpDatatype >> 29) & 1UL) << 8) |
                                ((RadeonRead32(bi, RADEON_CP_CSQ_STAT) >>
                                  24) & 0xffUL);
    }
    phase = ServiceExecTicks(base);
    /* Interface 19: NO_FENCE suppresses the per-dispatch drain tail. The
     * dispatch is still ring-ordered; the caller closes the run with
     * Radeon3DSubmitFence() (or a later fenced submission) before relying
     * on retirement. LastFence tracks only fenced submissions, so a
     * subsequent release that waits on it is not misled: every release
     * path runs a full drain first, and the closing fence is a submission
     * at least as new as any unfenced dispatch. */
    addFence = (request.Flags & RADEON3D_INDIRECT_NO_FENCE) == 0;
    result = RadeonCpSubmitStream(bi, commands, 3UL, addFence, &fence);
    submitTicks = ServiceExecTicks(base) - phase;
    if (result && addFence && fence) {
        device->LastFence = fence;
        RadeonMark3DSubmitted(bi);
    } else if (result && !addFence) {
        /* Unfenced dispatch still handed the engine to the CP; a fenced
         * submission that returned no fence stays a failure below. */
        fence = 0;
        RadeonMark3DSubmitted(bi);
    } else {
        result = FALSE;
        (void)RadeonRecoverAcceleration(bi);
    }
    RecordExecSample(base, RADEON3D_SAMPLE_DISPATCH, request.DwordCount,
                     3UL, 0UL, 0UL, submitTicks, result);
    base->ExecCalls++;
    base->ExecRecordDwords += request.DwordCount;
    if (result)
        base->ExecGeneratedDwords += 3UL;
    if (result)
        base->ExecSubmitMicros += ExecTicksToMicros(base, submitTicks);
    UnlockServiceBoard(base, bi, device);
    if (fenceOut)
        *fenceOut = result ? fence : (0x80000000UL | 106UL);
    return result;
}

/* Interface 19: close a run of RADEON3D_INDIRECT_NO_FENCE dispatches.
 * Two CP-native no-ops ride the ring; the standard fence tail appended by
 * RadeonCpSubmitStream performs the cache flush and the full idle wait, so
 * the fence retires only when every earlier submission (fenced or not) has
 * completely drained. That keeps one fence meaning for every consumer:
 * "everything up to this serial is done". */
BOOL Radeon3DSubmitFence(
    __REGA0(struct Radeon3DDevice *device),
    __REGA1(ULONG *fenceOut),
    __REGA6(struct RadeonChipBase *base))
{
    struct BoardInfo *bi;
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    ULONG commands[2];
    ULONG generation;
    ULONG phase, submitTicks = 0;
    ULONG fence = 0;
    BOOL result;

    if (base)
        COMMIT_FAIL(base, 110UL);
    if (fenceOut)
        *fenceOut = 0;
    if (!SysBase) {
        if (base)
            COMMIT_FAIL(base, 111UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 111UL;
        return FALSE;
    }
    bi = LockServiceBoard(base, &device);
    if (!bi) {
        COMMIT_FAIL(base, 112UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 112UL;
        return FALSE;
    }
    if (device->InterfaceVersion < 19UL) {
        UnlockServiceBoard(base, bi, device);
        COMMIT_FAIL(base, 113UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 113UL;
        return FALSE;
    }
    if (base->LeaseActive) {
        UnlockServiceBoard(base, bi, device);
        COMMIT_FAIL(base, 122UL);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 122UL;
        return FALSE;
    }
    generation = base->ServiceGeneration;
    if (!RadeonPrepare3D(bi)) {
        COMMIT_FAIL(base, 114UL);
        UnlockServiceBoard(base, bi, device);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 114UL;
        return FALSE;
    }
    /* Prepare may recover successfully but invalidate this session. Keep
     * BoardLock held and recheck before submitting. */
    ObtainSemaphore(&base->ServiceLock);
    result = FindActiveDevice(base, device) && IsUsableDevice(base, device) &&
             base->ServiceGeneration == generation && base->BoardInfo == bi;
    ReleaseSemaphore(&base->ServiceLock);
    if (!result) {
        COMMIT_FAIL(base, 115UL);
        UnlockServiceBoard(base, bi, device);
        if (fenceOut)
            *fenceOut = 0x80000000UL | 115UL;
        return FALSE;
    }
    /* A real register write rather than bare PACKET2 no-ops: this is the
     * stream shape the ring has executed for every other service call, so
     * the fence-only kick cannot introduce an untried packet form. */
    commands[0] = RADEON_CP_PACKET0(RADEON_SCRATCH_REG1, 0);
    commands[1] = 0UL;
    if (!RadeonCpUnfencedPending(bi))
    {
        /* Same guard as the indirect path, withheld for the same reason:
         * MMIO baseline writes must not race an in-flight IB fetch. */
        ULONG csqMode = RadeonRead32(bi, RADEON_CP_CSQ_MODE);
        ULONG dpDatatype;

        if (csqMode != CP_CSQ_CACHE_PARTITION) {
            (void)RadeonWrite32(bi, RADEON_CP_CSQ_MODE,
                                CP_CSQ_CACHE_PARTITION);
            csqMode = CP_CSQ_CACHE_PARTITION;
        }
        dpDatatype = RadeonRead32(bi, RADEON_DP_DATATYPE);
        if (dpDatatype & RADEON_HOST_BIG_ENDIAN_EN)
            (void)RadeonWrite32(bi, RADEON_DP_DATATYPE,
                                dpDatatype & ~RADEON_HOST_BIG_ENDIAN_EN);
        base->CommitFailStage = 0xc5000000UL | ((csqMode & 0xffUL) << 16) |
                                (((dpDatatype >> 29) & 1UL) << 8) |
                                ((RadeonRead32(bi, RADEON_CP_CSQ_STAT) >>
                                  24) & 0xffUL);
    }
    phase = ServiceExecTicks(base);
    result = RadeonCpSubmitStream(bi, commands, 2UL, TRUE, &fence);
    submitTicks = ServiceExecTicks(base) - phase;
    if (result && fence) {
        device->LastFence = fence;
        RadeonMark3DSubmitted(bi);
    } else {
        result = FALSE;
        (void)RadeonRecoverAcceleration(bi);
    }
    RecordExecSample(base, RADEON3D_SAMPLE_DISPATCH, 2UL, 2UL,
                     0UL, 0UL, submitTicks, result);
    if (result) {
        base->ExecCalls++;
        base->ExecRecordDwords += 2UL;
        base->ExecGeneratedDwords += 2UL;
        base->ExecSubmitMicros += ExecTicksToMicros(base, submitTicks);
    }
    UnlockServiceBoard(base, bi, device);
    if (fenceOut)
        *fenceOut = result ? fence : (0x80000000UL | 116UL);
    return result;
}

/*
 * Interface-20 PPC engine lease. While a lease is live the PPC is the only
 * ring writer: every service submission path rejects with stage 122, 2D
 * falls back to software (LeaseBlocks in radeon_accel.c), and the lease
 * dies with its session, an explicit release, or the expiry bound. All
 * descriptors are 68k-mapped addresses the PPC aliases 1:1 (Phase-0
 * verified). Fail-closed: any refusal leaves the existing bounded paths
 * untouched.
 */
BOOL Radeon3DAcquireLease(
    __REGA0(struct Radeon3DDevice *device),
    __REGA1(struct Radeon3DLease *lease),
    __REGA6(struct RadeonChipBase *base))
{
    struct BoardInfo *bi;
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    APTR ringCpu = NULL;
    ULONG ringGpu = 0;
    ULONG ringDwords = 0;
    ULONG ringMask = 0;
    ULONG stage = 120UL;
    BOOL granted = FALSE;

    if (!SysBase || !device || !lease ||
        lease->Size < RADEON3D_LEASE_V1_SIZE)
        return FALSE;
    lease->Version = 0UL;
    bi = LockServiceBoard(base, &device);
    if (!bi)
        return FALSE;
    if (device->InterfaceVersion < 20UL)
        stage = 121UL;
    else if (!base->PpcRingAllowed || !base->StreamSegmentPool ||
             !base->ExecClockHz || !RadeonCpIsReady(bi))
        stage = 120UL;
    else if (base->LeaseActive)
        stage = 122UL;
    else if (!RadeonPrepare3D(bi))
        stage = 125UL;
    else if (!RadeonCpGetRingInfo(bi, &ringCpu, &ringGpu, &ringDwords,
                                  &ringMask))
        stage = 125UL;
    else {
        struct RadeonBoardData *data = RadeonGetBoardData(bi);

        /* The CP fetch guards go in before the first lease dword, exactly
         * as the indirect dispatch does. */
        RadeonLeaseGrantGuards(bi);
        base->LeaseActive = TRUE;
        base->LeaseDevice = device;
        base->LeaseGrantTicks = ServiceExecTicks(base);
        /* Park 2D in FALLBACK for the lease: the callbacks then take the
         * software path without entering their failure/recovery paths
         * (which would abort the CP the lease holder is using). */
        if (data)
            data->AccelState = RADEON_ACCEL_FALLBACK;
        lease->Version = RADEON3D_LEASE_VERSION;
        lease->Generation = base->ServiceGeneration;
        lease->Flags = RADEON3D_LEASE_FLAGS;
        lease->RingCpuAddress = ringCpu;
        lease->RingGpuAddress = ringGpu;
        lease->RingDwords = ringDwords;
        lease->RingMask = ringMask;
        lease->Bar2Base = (ULONG)bi->MemoryIOBase;
        lease->Bar0Base = (ULONG)bi->MemoryBase;
        lease->NextFence = RadeonCpCurrentFence(bi);
        lease->HeartbeatMs = 1000UL;
        lease->MaxBatchDwords = RADEON3D_MAX_BATCH_DWORDS;
        lease->Size = RADEON3D_LEASE_V1_SIZE;
        granted = TRUE;
        stage = 0UL;
    }
    if (!granted) {
        COMMIT_FAIL(base, stage);
        lease->Generation = 0UL;
        lease->RingCpuAddress = NULL;
        lease->RingGpuAddress = 0UL;
        lease->NextFence = 0UL;
    }
    UnlockServiceBoard(base, bi, device);
    return granted;
}

BOOL Radeon3DReleaseLease(
    __REGA0(struct Radeon3DDevice *device),
    __REGD0(ULONG lastFence),
    __REGA6(struct RadeonChipBase *base))
{
    struct BoardInfo *bi;
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    ULONG stage = 123UL;
    BOOL result = TRUE;

    if (!SysBase || !device)
        return FALSE;
    bi = LockServiceBoard(base, &device);
    if (!bi)
        return FALSE;
    if (device->InterfaceVersion < 20UL)
        stage = 121UL;
    else if (!base->LeaseActive)
        stage = 123UL;
    else if (base->LeaseDevice != device)
        stage = 124UL;
    else {
        struct RadeonBoardData *data = RadeonGetBoardData(bi);

        if (lastFence &&
            !RadeonCpWaitFence(bi, lastFence, 2000UL)) {
            /* The lease's last fence never retired: treat it like any CP
             * fault, then re-arm anyway so the desktop keeps working. */
            (void)RadeonRecoverAcceleration(bi);
            result = FALSE;
        }
        RadeonCpAdoptFence(bi, lastFence);
        base->LeaseActive = FALSE;
        base->LeaseDevice = NULL;
        base->LeaseGrantTicks = 0;
        /* The PPC's ring work is not tracked by the 68k bookkeeping, so
         * the next Picasso96 operation must restore the 2D baseline. */
        RadeonMark3DSubmitted(bi);
        /* Restore 2D acceleration only when the engine is healthy; a
         * failed fence wait already recovered (or fell back) itself. */
        if (data && result && data->AccelState == RADEON_ACCEL_FALLBACK)
            data->AccelState = RADEON_ACCEL_READY;
        stage = 0UL;
    }
    if (stage)
        COMMIT_FAIL(base, stage);
    UnlockServiceBoard(base, bi, device);
    return result;
}

/* Interface 20: renew the calling session's lease-expiry deadline.
 * Metadata-only (ServiceLock, no BoardLock, no MMIO) so a per-frame
 * heartbeat is cheap; a holder that misses the service bound is reclaimed
 * by the 2D path with full recovery. Fail-closed (121/123/124). */
BOOL Radeon3DHeartbeatLease(
    __REGA0(struct Radeon3DDevice *device),
    __REGA6(struct RadeonChipBase *base))
{
    struct Radeon3DDevice *active;
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    ULONG stage = 123UL;
    BOOL ok = FALSE;

    if (!SysBase || !device)
        return FALSE;
    ObtainSemaphore(&base->ServiceLock);
    active = FindDevice(base, device);
    if (active && active->Magic == RADEON3D_SESSION_MAGIC &&
        active->Base == base &&
        active->Generation == base->ServiceGeneration &&
        base->ServiceState == RADEON3D_SERVICE_READY &&
        base->BoardInfo && RadeonCpIsReady(base->BoardInfo) &&
        base->LeaseActive && base->LeaseDevice == active) {
        base->LeaseGrantTicks = ServiceExecTicks(base);
        ok = TRUE;
        stage = 0UL;
    } else if (active && active->Base == base &&
               base->LeaseActive && base->LeaseDevice != active) {
        stage = 124UL;
    } else if (!active || active->Base != base) {
        stage = 121UL;
    }
    if (stage)
        COMMIT_FAIL(base, stage);
    ReleaseSemaphore(&base->ServiceLock);
    return ok;
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
    /* Accept any fence this session has submitted, not only device->LastFence:
     * a consumer may keep several submissions in flight and retire them in
     * order (RADEON3D_CAP_MULTI_FENCE). The CP scratch counter is monotonic
     * per session and CpFenceReached handles wrap; this range check only
     * bounds the value so a stale handle cannot test an arbitrary one. */
    valid = active && active->Magic == RADEON3D_SESSION_MAGIC &&
            active->Base == base && active->LastFence &&
            (LONG)(active->LastFence - fence) >= 0;
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
        if (handle && handle->Device == active) {
            RemoveSurfaceHandle(handle);
            if (handle->PoolBytes)
                AuxPoolFree(base, handle->PoolOffset, handle->PoolBytes);
        } else {
            handle = NULL;
        }
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
