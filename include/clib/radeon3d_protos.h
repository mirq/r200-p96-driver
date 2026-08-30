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
BOOL Radeon3DExecute(struct Radeon3DDevice *device,
                     const ULONG *records, ULONG recordDwords,
                      ULONG flags, ULONG *fenceOut);
BOOL Radeon3DInvalidateForTest(struct Radeon3DDevice *device);
BOOL Radeon3DAllocSegment(struct Radeon3DDevice *device, ULONG bytes,
                          struct Radeon3DSegment *segment);
BOOL Radeon3DFreeSegment(struct Radeon3DDevice *device, ULONG segmentId);
BOOL Radeon3DCommitDraw(struct Radeon3DDevice *device,
                        const struct Radeon3DCommit *commit,
                        ULONG *fenceOut);
BOOL Radeon3DCommitBatch(struct Radeon3DDevice *device,
                         const struct Radeon3DCommitBatch *commit,
                         ULONG *fenceOut);
BOOL Radeon3DCommitStateBatch(struct Radeon3DDevice *device,
                              const struct Radeon3DStateBatch *batch,
                              ULONG *fenceOut);

#endif
