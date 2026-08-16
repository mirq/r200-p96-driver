#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/radeon3d.h>
#include <stdio.h>
#include <stdlib.h>

#define CHIP_LIBRARY_NAME "Radeon9200.chip"
#define DEFAULT_ITERATIONS 10000UL

struct Library *Radeon9200Base;

int main(int argc, char **argv)
{
    struct Radeon3DInfo info;
    struct Radeon3DDevice *device;
    struct Radeon3DDevice *firstDevice = NULL;
    struct Radeon3DDevice *probeDevice;
    ULONG iterations = DEFAULT_ITERATIONS;
    ULONG batch;
    ULONG completed = 0;
    ULONG before;
    ULONG middle;
    ULONG after;
    LONG secondLoss;
    BOOL staleRejected = FALSE;

    if (argc > 1) {
        iterations = strtoul(argv[1], NULL, 0);
        if (!iterations) {
            printf("R3DSESSIONS status=bad_iterations\n");
            return 20;
        }
    }

    Radeon9200Base = OpenLibrary(
        (CONST_STRPTR)CHIP_LIBRARY_NAME, RADEON3D_LIBRARY_VERSION);
    if (!Radeon9200Base) {
        printf("R3DSESSIONS status=open_library_failed\n");
        return 20;
    }

    before = AvailMem(MEMF_PUBLIC);
    middle = before;
    for (batch = 0; batch < 2; ++batch) {
        ULONG index;

        for (index = 0; index < iterations; ++index) {
            info.Size = sizeof(info);
            device = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
            if (!device)
                break;
            if (!firstDevice)
                firstDevice = device;
            Radeon3DClose(device);
            ++completed;
        }
        if (index != iterations)
            break;
        if (!batch)
            middle = AvailMem(MEMF_PUBLIC);
    }
    after = AvailMem(MEMF_PUBLIC);
    secondLoss = (LONG)(middle - after);
    info.Size = sizeof(info);
    probeDevice = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
    if (probeDevice) {
        info.Size = sizeof(info);
        staleRejected = !Radeon3DGetInfo(firstDevice, &info);
        Radeon3DClose(probeDevice);
    }

    printf("R3DSESSIONS status=%s iterations=%lu public_before=%lu "
           "public_middle=%lu public_after=%lu first_loss=%ld "
           "second_loss=%ld stale_rejected=%lu\n",
           completed == iterations * 2UL && probeDevice &&
                   secondLoss <= 1024L && staleRejected
               ? "ok"
               : completed == iterations * 2UL && probeDevice
                     ? "leak_or_stale_handle"
                     : "open_failed",
           completed, before, middle, after, (long)(before - middle),
           (long)secondLoss, (unsigned long)staleRejected);

    CloseLibrary(Radeon9200Base);
    Radeon9200Base = NULL;
    return completed == iterations * 2UL && probeDevice &&
                   secondLoss <= 1024L && staleRejected
               ? 0
               : 10;
}
