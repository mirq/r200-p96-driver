#include <stddef.h>

#include <proto/radeon3d.h>

struct Library *Radeon9200Base;

#define ABI_CHECK(name, expression) typedef char name[(expression) ? 1 : -1]

ABI_CHECK(Radeon3DInfoSize,
          sizeof(struct Radeon3DInfo) == RADEON3D_INFO_V1_SIZE);
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
ABI_CHECK(Radeon3DPhase2Capability,
          RADEON3D_CAP_PHASE2_EXECUTE == (1UL << 7));
ABI_CHECK(Radeon3DPhase4DepthCapability,
          RADEON3D_CAP_PHASE4_DEPTH_FUNCS == (1UL << 8));
ABI_CHECK(Radeon3DDepthOptions,
          RADEON3D_DRAW_OPTIONS_V3 == 0x000000ffUL);
ABI_CHECK(Radeon3DPhase5TextureCapability,
           RADEON3D_CAP_PHASE5_TEXTURE_STATE == (1UL << 9));
ABI_CHECK(Radeon3DPhase6Capability,
           RADEON3D_CAP_PHASE6_FOG_MULTITEX == (1UL << 10));
ABI_CHECK(Radeon3DTestInvalidateCapability,
          RADEON3D_CAP_TEST_INVALIDATE == (1UL << 11));
ABI_CHECK(Radeon3DColorTargetCapability,
          RADEON3D_CAP_COLOR_TARGET_FORMATS == (1UL << 12));
ABI_CHECK(Radeon3DInterfaceVersion,
          RADEON3D_IFACE_VERSION == 6UL);
ABI_CHECK(Radeon3DV4Options,
          RADEON3D_DRAW_OPTIONS_V4 == 0x000001ffUL);
ABI_CHECK(Radeon3DV4StateOption,
          RADEON3D_DRAW_STATE_V4 == (1UL << 8));
ABI_CHECK(Radeon3DV5StateOption,
          RADEON3D_DRAW_STATE_V5 == (1UL << 9));
ABI_CHECK(Radeon3DV5Options,
          RADEON3D_DRAW_OPTIONS == 0x000003ffUL);
ABI_CHECK(Radeon3DPhase6StateMask,
          RADEON3D_PHASE6_STATE_MASK == 0x00000003UL);
ABI_CHECK(Radeon3DExecuteClearSize,
           RADEON3D_EXEC_CLEAR_DWORDS == 11UL);
ABI_CHECK(Radeon3DExecuteDrawSize,
          RADEON3D_EXEC_DRAW_HEADER_DWORDS == 11UL);
ABI_CHECK(Radeon3DExecuteDrawV4Size,
          RADEON3D_EXEC_DRAW_V4_HEADER_DWORDS == 15UL);
ABI_CHECK(Radeon3DExecuteDrawV5Size,
          RADEON3D_EXEC_DRAW_V5_HEADER_DWORDS == 21UL);
ABI_CHECK(Radeon3DExecuteVertexSize,
          RADEON3D_EXEC_VERTEX_DWORDS == 6UL);
ABI_CHECK(Radeon3DExecuteV5VertexSize,
          RADEON3D_EXEC_V5_VERTEX_DWORDS == 9UL);

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
        (void)Radeon3DExecute(device, &command, 1, 0, &fence);
        (void)Radeon3DInvalidateForTest(device);
        Radeon3DClose(device);
    }
}
