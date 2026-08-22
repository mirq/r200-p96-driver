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
           RADEON3D_CAP_DEPTH_FUNCS == (1UL << 8));
ABI_CHECK(Radeon3DBasicOptions,
          RADEON3D_DRAW_OPTIONS_BASIC == 0x000000ffUL);
ABI_CHECK(Radeon3DPhase5TextureCapability,
            RADEON3D_CAP_TEXTURE_STATE == (1UL << 9));
ABI_CHECK(Radeon3DPhase6Capability,
            RADEON3D_CAP_FOG_MULTITEX == (1UL << 10));
ABI_CHECK(Radeon3DTestInvalidateCapability,
          RADEON3D_CAP_TEST_INVALIDATE == (1UL << 11));
ABI_CHECK(Radeon3DColorTargetCapability,
          RADEON3D_CAP_COLOR_TARGET_FORMATS == (1UL << 12));
ABI_CHECK(Radeon3DNativePrimitiveCapability,
           RADEON3D_CAP_NATIVE_TRI_PRIMITIVES == (1UL << 13));
ABI_CHECK(Radeon3DNativeQuadCapability,
           RADEON3D_CAP_NATIVE_QUAD_LISTS == (1UL << 14));
ABI_CHECK(Radeon3DTransformClipCapability,
          RADEON3D_CAP_HW_TRANSFORM_CLIP == (1UL << 15));
ABI_CHECK(Radeon3DInterfaceVersion,
           RADEON3D_IFACE_VERSION == 9UL);
ABI_CHECK(Radeon3DTriangleStripOpcode,
          RADEON3D_EXEC_DRAW_TRI_STRIP == 3UL);
ABI_CHECK(Radeon3DTriangleFanOpcode,
           RADEON3D_EXEC_DRAW_TRI_FAN == 4UL);
ABI_CHECK(Radeon3DQuadOpcode,
           RADEON3D_EXEC_DRAW_QUADS == 5UL);
ABI_CHECK(Radeon3DPointOpcode,
          RADEON3D_EXEC_DRAW_POINTS == 6UL);
ABI_CHECK(Radeon3DLineOpcode,
          RADEON3D_EXEC_DRAW_LINES == 7UL);
ABI_CHECK(Radeon3DLineStripOpcode,
          RADEON3D_EXEC_DRAW_LINE_STRIP == 8UL);
ABI_CHECK(Radeon3DLineLoopOpcode,
          RADEON3D_EXEC_DRAW_LINE_LOOP == 9UL);
ABI_CHECK(Radeon3DQuadVertexLimit,
          RADEON3D_IMMD_MAX_QUAD_VERTICES == 252UL);
ABI_CHECK(Radeon3DQuadExtendedVertexLimit,
           RADEON3D_IMMD_MAX_EXTENDED_QUAD_VERTICES == 252UL);
ABI_CHECK(Radeon3DFragmentOptions,
          RADEON3D_DRAW_OPTIONS_FRAGMENT == 0x000001ffUL);
ABI_CHECK(Radeon3DFragmentStateOption,
          RADEON3D_DRAW_FRAGMENT_STATE == (1UL << 8));
ABI_CHECK(Radeon3DExtendedVertexOption,
          RADEON3D_DRAW_EXTENDED_VERTEX == (1UL << 9));
ABI_CHECK(Radeon3DPreTclOptions,
          RADEON3D_DRAW_OPTIONS_PRE_TCL == 0x000003ffUL);
ABI_CHECK(Radeon3DHardwareTclOption,
          RADEON3D_DRAW_HW_TCL == (1UL << 10));
ABI_CHECK(Radeon3DHardwareTclOptions,
          RADEON3D_DRAW_OPTIONS == 0x000007ffUL);
ABI_CHECK(Radeon3DVertexStateMask,
          RADEON3D_VERTEX_STATE_MASK == 0x00000007UL);
ABI_CHECK(Radeon3DTransformStateMask,
          RADEON3D_TRANSFORM_STATE_MASK == 0x000fff3fUL);
ABI_CHECK(Radeon3DExecuteClearSize,
           RADEON3D_EXEC_CLEAR_DWORDS == 11UL);
ABI_CHECK(Radeon3DExecuteDrawSize,
          RADEON3D_EXEC_DRAW_HEADER_DWORDS == 11UL);
ABI_CHECK(Radeon3DExecuteDrawFragmentSize,
          RADEON3D_EXEC_DRAW_FRAGMENT_HEADER_DWORDS == 15UL);
ABI_CHECK(Radeon3DExecuteDrawExtendedSize,
          RADEON3D_EXEC_DRAW_EXTENDED_HEADER_DWORDS == 21UL);
ABI_CHECK(Radeon3DExecuteDrawHardwareTclSize,
          RADEON3D_EXEC_DRAW_HW_TCL_HEADER_DWORDS == 44UL);
ABI_CHECK(Radeon3DExecuteVertexSize,
          RADEON3D_EXEC_VERTEX_DWORDS == 6UL);
ABI_CHECK(Radeon3DExecuteExtendedVertexSize,
          RADEON3D_EXEC_EXTENDED_VERTEX_DWORDS == 9UL);
ABI_CHECK(Radeon3DExecuteHardwareTclVertexSize,
          RADEON3D_EXEC_HW_TCL_VERTEX_DWORDS == 10UL);

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
