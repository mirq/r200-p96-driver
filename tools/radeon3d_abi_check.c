#include <stddef.h>

#include <proto/radeon3d.h>

struct Library *Radeon9200Base;

#define ABI_CHECK(name, expression) typedef char name[(expression) ? 1 : -1]

ABI_CHECK(Radeon3DInfoSize,
          sizeof(struct Radeon3DInfo) == RADEON3D_INFO_V4_SIZE);
ABI_CHECK(Radeon3DInfoSizeOffset,
          offsetof(struct Radeon3DInfo, Size) == 0);
ABI_CHECK(Radeon3DInfoVersionOffset,
          offsetof(struct Radeon3DInfo, Version) == 4);
ABI_CHECK(Radeon3DInfoGenerationOffset,
          offsetof(struct Radeon3DInfo, Generation) == 8);
ABI_CHECK(Radeon3DInfoDeviceIdOffset,
          offsetof(struct Radeon3DInfo, DeviceId) == 12);
ABI_CHECK(Radeon3DInfoCapsOffset,
          offsetof(struct Radeon3DInfo, Caps) == 16);
ABI_CHECK(Radeon3DInfoInstalledVramOffset,
          offsetof(struct Radeon3DInfo, InstalledVram) == 20);
ABI_CHECK(Radeon3DInfoPicasso96VramOffset,
          offsetof(struct Radeon3DInfo, Picasso96Vram) == 24);
ABI_CHECK(Radeon3DInfoMaxBatchOffset,
          offsetof(struct Radeon3DInfo, MaxBatchDwords) == 28);
ABI_CHECK(Radeon3DInfoV2Size,
          sizeof(struct Radeon3DInfo) == RADEON3D_INFO_V4_SIZE);
ABI_CHECK(Radeon3DInfoCommitFailOffset,
          offsetof(struct Radeon3DInfo, CommitFailStage) == 56);
ABI_CHECK(Radeon3DInfoSampleRingOffset,
          offsetof(struct Radeon3DInfo, SampleRing) == 60);
ABI_CHECK(Radeon3DInfoSampleEntriesOffset,
          offsetof(struct Radeon3DInfo, SampleRingEntries) == 64);
ABI_CHECK(Radeon3DInfoSampleSeqOffset,
          offsetof(struct Radeon3DInfo, SampleSeq) == 68);
ABI_CHECK(Radeon3DInfoEClockHzOffset,
          offsetof(struct Radeon3DInfo, EClockHz) == 72);
ABI_CHECK(Radeon3DInfoExecCallsOffset,
          offsetof(struct Radeon3DInfo, ExecCalls) == 32);
ABI_CHECK(Radeon3DInfoExecRecordOffset,
          offsetof(struct Radeon3DInfo, ExecRecordDwords) == 36);
ABI_CHECK(Radeon3DInfoExecGeneratedOffset,
          offsetof(struct Radeon3DInfo, ExecGeneratedDwords) == 40);
ABI_CHECK(Radeon3DInfoExecCopyOffset,
          offsetof(struct Radeon3DInfo, ExecCopyMicros) == 44);
ABI_CHECK(Radeon3DInfoExecBuildOffset,
          offsetof(struct Radeon3DInfo, ExecBuildMicros) == 48);
ABI_CHECK(Radeon3DInfoExecSubmitOffset,
          offsetof(struct Radeon3DInfo, ExecSubmitMicros) == 52);
ABI_CHECK(Radeon3DSurfaceSize,
          sizeof(struct Radeon3DSurface) == RADEON3D_SURFACE_V1_SIZE);
ABI_CHECK(Radeon3DSurfaceCpuOffset,
          offsetof(struct Radeon3DSurface, CpuAddress) == 12);
ABI_CHECK(Radeon3DSurfaceHandleOffset,
          offsetof(struct Radeon3DSurface, Handle) == 36);
ABI_CHECK(Radeon3DDynamicTriangleCapability,
          RADEON3D_CAP_IMMD_TRI_LIST == (1UL << 6));
ABI_CHECK(Radeon3DDynamicTriangleLimit,
          RADEON3D_IMMD_MAX_VERTICES == 255UL);
ABI_CHECK(Radeon3DTestInvalidateCapability,
          RADEON3D_CAP_TEST_INVALIDATE == (1UL << 11));
ABI_CHECK(Radeon3DInterfaceVersion,
            RADEON3D_IFACE_VERSION == 20UL);
ABI_CHECK(Radeon3DStreamSegmentCapability,
           RADEON3D_CAP_STREAM_SEGMENTS == (1UL << 21));
ABI_CHECK(Radeon3DAuxSurfaceCapability,
           RADEON3D_CAP_AUX_SURFACES == (1UL << 25));
ABI_CHECK(Radeon3DSegmentStructSize,
          sizeof(struct Radeon3DSegment) == RADEON3D_SEGMENT_V1_SIZE);
ABI_CHECK(Radeon3DMaxSegments,
          RADEON3D_MAX_SEGMENTS == 12UL);
/* The semantic record ABI (RADEON3D_EXEC_* opcodes and sizes, the
 * commit/state-batch structures, the DRAW_/TEX_/TEXGEN_/FRAGMENT_/
 * VERTEX_/TRANSFORM_/BLEND_/LIGHT_/MATERIAL_ option constants and the
 * capability bits 7-20/22-24) was removed with the record path; the
 * assertions that pinned its layout went with it. */
ABI_CHECK(Radeon3DIndirectDispatchCapability,
           RADEON3D_CAP_INDIRECT_DISPATCH == (1UL << 26));
ABI_CHECK(Radeon3DIndirectRenderCapability,
           RADEON3D_CAP_INDIRECT_RENDER == (1UL << 27));
ABI_CHECK(Radeon3DMultiFenceCapability,
           RADEON3D_CAP_MULTI_FENCE == (1UL << 28));
ABI_CHECK(Radeon3DFenceCoalesceCapability,
           RADEON3D_CAP_FENCE_COALESCE == (1UL << 29));
ABI_CHECK(Radeon3DPpcRingCapability,
           RADEON3D_CAP_PPC_RING == (1UL << 30));
ABI_CHECK(Radeon3DLeaseStructSize,
          sizeof(struct Radeon3DLease) == RADEON3D_LEASE_V1_SIZE);
ABI_CHECK(Radeon3DLeaseVersion,
          RADEON3D_LEASE_VERSION == 1UL);
ABI_CHECK(Radeon3DIndirectNoFenceFlag,
           RADEON3D_INDIRECT_NO_FENCE == (1UL << 1));
ABI_CHECK(Radeon3DIndirectFlagsMask,
           RADEON3D_INDIRECT_FLAGS == RADEON3D_INDIRECT_NO_FENCE);
ABI_CHECK(Radeon3DIndirectSize,
          sizeof(struct Radeon3DIndirect) == RADEON3D_INDIRECT_V1_SIZE);
ABI_CHECK(Radeon3DIndirectSegmentOffset,
          offsetof(struct Radeon3DIndirect, SegmentId) == 8);
ABI_CHECK(Radeon3DIndirectOffsetOffset,
          offsetof(struct Radeon3DIndirect, ByteOffset) == 12);
ABI_CHECK(Radeon3DIndirectCountOffset,
          offsetof(struct Radeon3DIndirect, DwordCount) == 16);
ABI_CHECK(Radeon3DIndirectFlagsOffset,
          offsetof(struct Radeon3DIndirect, Flags) == 20);

void Radeon3DAbiCalls(void)
{
    struct Radeon3DInfo info;
    struct Radeon3DDevice *device;

    info.Size = sizeof(info);
    device = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
    if (device) {
        ULONG command = 0x80000000UL;
        ULONG fence;
        struct Radeon3DSurface surface;

        (void)Radeon3DGetInfo(device, &info);
        (void)Radeon3DSubmit(device, &command, 1,
                             RADEON3D_SUBMIT_FENCE, &fence);
        (void)Radeon3DTestFence(device, fence);
        (void)Radeon3DWaitFence(device, fence, 1000);
        surface.Size = sizeof(surface);
        (void)Radeon3DImportBitMap(device, NULL, &surface);
        Radeon3DReleaseSurface(device, &surface);
        {
            struct Radeon3DIndirect indirect;

            indirect.Size = sizeof(indirect);
            (void)Radeon3DDispatchIndirect(device, &indirect, &fence);
        }
        (void)Radeon3DSubmitFence(device, &fence);
        {
            struct Radeon3DLease lease;

            lease.Size = sizeof(lease);
            (void)Radeon3DAcquireLease(device, &lease);
            (void)Radeon3DHeartbeatLease(device);
            (void)Radeon3DReleaseLease(device, 0UL);
        }
        (void)Radeon3DInvalidateForTest(device);
        Radeon3DClose(device);
    }
}
