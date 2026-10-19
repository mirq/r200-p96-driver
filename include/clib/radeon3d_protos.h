#ifndef CLIB_RADEON3D_PROTOS_H
#define CLIB_RADEON3D_PROTOS_H

#include <exec/types.h>
#include <radeon3d.h>

struct Radeon3DDevice *Radeon3DOpen(
    ULONG requestedVersion, struct Radeon3DInfo *info);
void Radeon3DClose(struct Radeon3DDevice *device);
BOOL Radeon3DGetInfo(struct Radeon3DDevice *device,
                     struct Radeon3DInfo *info);
BOOL Radeon3DSubmit(struct Radeon3DDevice *device,
                    const ULONG *commands, ULONG commandCount,
                    ULONG flags, ULONG *fenceOut);
BOOL Radeon3DTestFence(struct Radeon3DDevice *device, ULONG fence);
BOOL Radeon3DWaitFence(struct Radeon3DDevice *device, ULONG fence,
                       ULONG timeoutMs);
BOOL Radeon3DImportBitMap(struct Radeon3DDevice *device,
                          struct BitMap *bitmap,
                          struct Radeon3DSurface *surface);
void Radeon3DReleaseSurface(struct Radeon3DDevice *device,
                             struct Radeon3DSurface *surface);
BOOL Radeon3DInvalidateForTest(struct Radeon3DDevice *device);
BOOL Radeon3DAllocSegment(struct Radeon3DDevice *device, ULONG bytes,
                          struct Radeon3DSegment *segment);
BOOL Radeon3DFreeSegment(struct Radeon3DDevice *device, ULONG segmentId);
BOOL Radeon3DAllocSurface(struct Radeon3DDevice *device, ULONG width,
                          ULONG height, ULONG format,
                          struct Radeon3DSurface *surface);
BOOL Radeon3DDispatchIndirect(struct Radeon3DDevice *device,
                              const struct Radeon3DIndirect *indirect,
                              ULONG *fenceOut);
BOOL Radeon3DSubmitFence(struct Radeon3DDevice *device, ULONG *fenceOut);
BOOL Radeon3DAcquireLease(struct Radeon3DDevice *device,
                          struct Radeon3DLease *lease);
BOOL Radeon3DReleaseLease(struct Radeon3DDevice *device, ULONG lastFence);
BOOL Radeon3DHeartbeatLease(struct Radeon3DDevice *device);

#endif
