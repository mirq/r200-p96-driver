#ifndef INLINE_RADEON3D_PROTOS_H
#define INLINE_RADEON3D_PROTOS_H

#include <exec/types.h>
#include <radeon3d.h>

struct Radeon3DDevice *__Radeon3DOpen(
    __reg("a6") void *base,
    __reg("d0") ULONG requestedVersion,
    __reg("a0") struct Radeon3DInfo *info) = "\tjsr\t-42(a6)";
void __Radeon3DClose(
    __reg("a6") void *base,
    __reg("a0") struct Radeon3DDevice *device) = "\tjsr\t-48(a6)";
BOOL __Radeon3DGetInfo(
    __reg("a6") void *base,
    __reg("a0") struct Radeon3DDevice *device,
    __reg("a1") struct Radeon3DInfo *info) = "\tjsr\t-54(a6)";
BOOL __Radeon3DSubmit(
    __reg("a6") void *base,
    __reg("a0") struct Radeon3DDevice *device,
    __reg("a1") const ULONG *commands,
    __reg("d0") ULONG commandCount,
    __reg("d1") ULONG flags,
    __reg("a2") ULONG *fenceOut) = "\tjsr\t-66(a6)";
BOOL __Radeon3DTestFence(
    __reg("a6") void *base,
    __reg("a0") struct Radeon3DDevice *device,
    __reg("d0") ULONG fence) = "\tjsr\t-72(a6)";
BOOL __Radeon3DWaitFence(
    __reg("a6") void *base,
    __reg("a0") struct Radeon3DDevice *device,
    __reg("d0") ULONG fence,
    __reg("d1") ULONG timeoutMs) = "\tjsr\t-78(a6)";
BOOL __Radeon3DImportBitMap(
    __reg("a6") void *base,
    __reg("a0") struct Radeon3DDevice *device,
    __reg("a1") struct BitMap *bitmap,
    __reg("a2") struct Radeon3DSurface *surface) = "\tjsr\t-84(a6)";
void __Radeon3DReleaseSurface(
    __reg("a6") void *base,
    __reg("a0") struct Radeon3DDevice *device,
    __reg("a1") struct Radeon3DSurface *surface) = "\tjsr\t-90(a6)";
BOOL __Radeon3DExecute(
    __reg("a6") void *base,
    __reg("a0") struct Radeon3DDevice *device,
    __reg("a1") const ULONG *records,
    __reg("d0") ULONG recordDwords,
    __reg("d1") ULONG flags,
    __reg("a2") ULONG *fenceOut) = "\tjsr\t-96(a6)";
BOOL __Radeon3DInvalidateForTest(
    __reg("a6") void *base,
    __reg("a0") struct Radeon3DDevice *device) = "\tjsr\t-102(a6)";
BOOL __Radeon3DAllocSegment(
    __reg("a6") void *base,
    __reg("a0") struct Radeon3DDevice *device,
    __reg("d0") ULONG bytes,
    __reg("a1") struct Radeon3DSegment *segment) = "\tjsr\t-108(a6)";
BOOL __Radeon3DFreeSegment(
    __reg("a6") void *base,
    __reg("a0") struct Radeon3DDevice *device,
    __reg("d0") ULONG segmentId) = "\tjsr\t-114(a6)";
BOOL __Radeon3DCommitDraw(
    __reg("a6") void *base,
    __reg("a0") struct Radeon3DDevice *device,
    __reg("a1") const struct Radeon3DCommit *commit,
    __reg("a2") ULONG *fenceOut) = "\tjsr\t-120(a6)";
#ifndef RADEON3D_BASE_NAME
#define RADEON3D_BASE_NAME Radeon9200Base
#endif

#define Radeon3DOpen(requestedVersion, info) \
    __Radeon3DOpen(RADEON3D_BASE_NAME, (requestedVersion), (info))
#define Radeon3DClose(device) \
    __Radeon3DClose(RADEON3D_BASE_NAME, (device))
#define Radeon3DGetInfo(device, info) \
    __Radeon3DGetInfo(RADEON3D_BASE_NAME, (device), (info))
#define Radeon3DSubmit(device, commands, commandCount, flags, fenceOut) \
    __Radeon3DSubmit(RADEON3D_BASE_NAME, (device), (commands), \
                     (commandCount), (flags), (fenceOut))
#define Radeon3DTestFence(device, fence) \
    __Radeon3DTestFence(RADEON3D_BASE_NAME, (device), (fence))
#define Radeon3DWaitFence(device, fence, timeoutMs) \
    __Radeon3DWaitFence(RADEON3D_BASE_NAME, (device), (fence), (timeoutMs))
#define Radeon3DImportBitMap(device, bitmap, surface) \
    __Radeon3DImportBitMap(RADEON3D_BASE_NAME, (device), (bitmap), (surface))
#define Radeon3DReleaseSurface(device, surface) \
    __Radeon3DReleaseSurface(RADEON3D_BASE_NAME, (device), (surface))
#define Radeon3DExecute(device, records, recordDwords, flags, fenceOut) \
    __Radeon3DExecute(RADEON3D_BASE_NAME, (device), (records), \
                       (recordDwords), (flags), (fenceOut))
#define Radeon3DInvalidateForTest(device) \
    __Radeon3DInvalidateForTest(RADEON3D_BASE_NAME, (device))
#define Radeon3DAllocSegment(device, bytes, segment) \
    __Radeon3DAllocSegment(RADEON3D_BASE_NAME, (device), (bytes), (segment))
#define Radeon3DFreeSegment(device, segmentId) \
    __Radeon3DFreeSegment(RADEON3D_BASE_NAME, (device), (segmentId))
#define Radeon3DCommitDraw(device, commit, fenceOut) \
    __Radeon3DCommitDraw(RADEON3D_BASE_NAME, (device), (commit), (fenceOut))

#endif
