#include <exec/libraries.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/radeon3d.h>
#include <stdio.h>

#define CHIP_LIBRARY_NAME "Radeon9200.chip"

struct Library *Radeon9200Base;

/* Dump the release-safe timing ring: one R3DSAMPLE line per captured
 * submission, oldest first. Ticks are raw EClock values; tools/perfplot.py
 * converts them with the R3DCLOCK rate. */
static void DumpSamples(const struct Radeon3DInfo *info)
{
    const struct Radeon3DSample *samples;
    ULONG entries = info->SampleRingEntries;
    ULONG seq = info->SampleSeq;
    ULONG first;
    ULONG seqNo;

    if (info->Size < RADEON3D_INFO_V4_SIZE || !info->SampleRing)
        return;
    printf("R3DCLOCK hz=%lu entries=%lu seq=%lu\n",
           info->EClockHz, entries, seq);
    if (!info->EClockHz || !entries || !seq)
        return;
    samples = (const struct Radeon3DSample *)info->SampleRing;
    first = seq > entries ? seq - entries + 1UL : 1UL;
    for (seqNo = first; seqNo <= seq; ++seqNo) {
        const volatile struct Radeon3DSample *sample =
            &samples[(seqNo - 1UL) & (entries - 1UL)];
        struct Radeon3DSample copy;

        if (sample->Seq != seqNo)
            continue;
        copy = *sample;
        if (sample->Seq != seqNo || copy.Seq != seqNo)
            continue;
        printf("R3DSAMPLE seq=%lu wall=%lu type=%lu ok=%lu in=%lu "
               "out=%lu copy=%lu build=%lu submit=%lu\n",
               copy.Seq, copy.WallTicks, copy.Type,
               copy.Result, copy.RecordDwords,
               copy.GeneratedDwords, copy.CopyTicks,
               copy.BuildTicks, copy.SubmitTicks);
    }
}

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
    } else {
        if (info.Size >= 56UL && info.ExecCalls) {
            /* V2 tail: cumulative Execute phase attribution in microseconds. */
            printf("R3DEXEC calls=%lu record_dwords=%lu generated_dwords=%lu "
                   "copy_us=%lu build_us=%lu submit_us=%lu\n",
                   info.ExecCalls, info.ExecRecordDwords,
                   info.ExecGeneratedDwords, info.ExecCopyMicros,
                   info.ExecBuildMicros, info.ExecSubmitMicros);
        }
        DumpSamples(&info);
    }

    Radeon3DClose(device);
    CloseLibrary(Radeon9200Base);
    Radeon9200Base = NULL;
    return result;
}
