#include <exec/libraries.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/radeon3d.h>
#include <stdio.h>

#define CHIP_LIBRARY_NAME "Radeon9200.chip"

struct Library *Radeon9200Base;

int main(void)
{
    struct Radeon3DInfo info;
    struct Radeon3DDevice *device;
    int result = 0;

    Radeon9200Base = OpenLibrary(
        (CONST_STRPTR)CHIP_LIBRARY_NAME, RADEON3D_LIBRARY_VERSION);
    if (!Radeon9200Base) {
        printf("R3DINFO status=open_library_failed min_version=%lu\n",
               RADEON3D_LIBRARY_VERSION);
        return 20;
    }

    info.Size = sizeof(info);
    device = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
    if (!device) {
        printf("R3DINFO status=open_service_failed library_version=%u\n",
               (unsigned int)Radeon9200Base->lib_Version);
        CloseLibrary(Radeon9200Base);
        Radeon9200Base = NULL;
        return 10;
    }

    printf("R3DINFO status=ok library_version=%u interface_version=%lu "
           "generation=%lu device=%04lx caps=%08lx installed_vram=%lu "
           "p96_vram=%lu max_batch_dwords=%lu\n",
           (unsigned int)Radeon9200Base->lib_Version,
           info.Version, info.Generation, info.DeviceId, info.Caps,
           info.InstalledVram, info.Picasso96Vram, info.MaxBatchDwords);

    info.Size = sizeof(info);
    if (!Radeon3DGetInfo(device, &info)) {
        printf("R3DINFO status=get_info_failed\n");
        result = 10;
    }

    Radeon3DClose(device);
    CloseLibrary(Radeon9200Base);
    Radeon9200Base = NULL;
    return result;
}
