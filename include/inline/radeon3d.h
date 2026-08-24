#ifndef INLINE_RADEON3D_H
#define INLINE_RADEON3D_H

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif

#ifndef RADEON3D_BASE_NAME
#define RADEON3D_BASE_NAME Radeon9200Base
#endif

#define Radeon3DOpen(requestedVersion, info) \
    LP2(0x2a, struct Radeon3DDevice *, Radeon3DOpen, \
        ULONG, requestedVersion, d0, struct Radeon3DInfo *, info, a0, \
        , RADEON3D_BASE_NAME)

#define Radeon3DClose(device) \
    LP1NR(0x30, Radeon3DClose, \
          struct Radeon3DDevice *, device, a0, \
          , RADEON3D_BASE_NAME)

#define Radeon3DGetInfo(device, info) \
    LP2(0x36, BOOL, Radeon3DGetInfo, \
        struct Radeon3DDevice *, device, a0, \
        struct Radeon3DInfo *, info, a1, \
        , RADEON3D_BASE_NAME)

#define Radeon3DSubmit(device, commands, commandCount, flags, fenceOut) \
    LP5(0x42, BOOL, Radeon3DSubmit, \
        struct Radeon3DDevice *, device, a0, \
        const ULONG *, commands, a1, \
        ULONG, commandCount, d0, \
        ULONG, flags, d1, \
        ULONG *, fenceOut, a2, \
        , RADEON3D_BASE_NAME)

#define Radeon3DTestFence(device, fence) \
    LP2(0x48, BOOL, Radeon3DTestFence, \
        struct Radeon3DDevice *, device, a0, \
        ULONG, fence, d0, \
        , RADEON3D_BASE_NAME)

#define Radeon3DWaitFence(device, fence, timeoutMs) \
    LP3(0x4e, BOOL, Radeon3DWaitFence, \
        struct Radeon3DDevice *, device, a0, \
        ULONG, fence, d0, \
        ULONG, timeoutMs, d1, \
        , RADEON3D_BASE_NAME)

#define Radeon3DImportBitMap(device, bitmap, surface) \
    LP3(0x54, BOOL, Radeon3DImportBitMap, \
        struct Radeon3DDevice *, device, a0, \
        struct BitMap *, bitmap, a1, \
        struct Radeon3DSurface *, surface, a2, \
        , RADEON3D_BASE_NAME)

#define Radeon3DReleaseSurface(device, surface) \
    LP2NR(0x5a, Radeon3DReleaseSurface, \
        struct Radeon3DDevice *, device, a0, \
        struct Radeon3DSurface *, surface, a1, \
          , RADEON3D_BASE_NAME)

#define Radeon3DExecute(device, records, recordDwords, flags, fenceOut) \
    LP5(0x60, BOOL, Radeon3DExecute, \
        struct Radeon3DDevice *, device, a0, \
        const ULONG *, records, a1, \
        ULONG, recordDwords, d0, \
        ULONG, flags, d1, \
        ULONG *, fenceOut, a2, \
        , RADEON3D_BASE_NAME)

#define Radeon3DInvalidateForTest(device) \
    LP1(0x66, BOOL, Radeon3DInvalidateForTest, \
        struct Radeon3DDevice *, device, a0, \
        , RADEON3D_BASE_NAME)

#define Radeon3DAllocSegment(device, bytes, segment) \
    LP3(0x6c, BOOL, Radeon3DAllocSegment, \
        struct Radeon3DDevice *, device, a0, \
        ULONG, bytes, d0, \
        struct Radeon3DSegment *, segment, a1, \
        , RADEON3D_BASE_NAME)

#define Radeon3DFreeSegment(device, segmentId) \
    LP2(0x72, BOOL, Radeon3DFreeSegment, \
        struct Radeon3DDevice *, device, a0, \
        ULONG, segmentId, d0, \
        , RADEON3D_BASE_NAME)

#define Radeon3DCommitDraw(device, commit, fenceOut) \
    LP3(0x78, BOOL, Radeon3DCommitDraw, \
        struct Radeon3DDevice *, device, a0, \
        const struct Radeon3DCommit *, commit, a1, \
        ULONG *, fenceOut, a2, \
        , RADEON3D_BASE_NAME)

#endif
