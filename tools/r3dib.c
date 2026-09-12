/* R3DIB - Radeon3D trusted indirect dispatch gate (interface 18).
 *
 * Tier-1 hardware gate for CP_IB_BASE/CP_IB_BUFSZ execution on the
 * physical card. Run this before any client-side indirect-buffer work.
 *
 * Sequence:
 *  1. Open an interface-18 session and require CAP_INDIRECT_DISPATCH,
 *     CAP_STREAM_SEGMENTS and CAP_CP_READY.
 *  2. Submit a plain PACKET2 no-op batch with a fence and retire it. If
 *     this fails the CP is not healthy; stop without touching IBs.
 *  3. Allocate one segment, fill it with PACKET2 no-ops, dispatch the
 *     first 64 dwords as an indirect buffer and retire the returned
 *     fence. The fence sits in ring order after the indirect packet, so
 *     a retired fence proves the CP consumed the buffer.
 *  4. Dispatch the maximum-size (8192-dword) indirect buffer from a
 *     4 KiB offset and retire its fence.
 *  5. Negative checks: each rejected dispatch must fail with the
 *     documented fence error encoding (0x80000000 | stage).
 *  6. An interface-17 session must be rejected.
 *
 * On a fence timeout this tool prints status=fence_timeout and exits
 * without retrying. A timeout after step 3 means the IB registers or
 * fetch path are wrong; treat the machine as suspect and follow the
 * physical-device recovery protocol before drawing conclusions.
 */

#include <exec/libraries.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/radeon3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHIP_LIBRARY_NAME "Radeon9200.chip"
#define IB_SEGMENT_BYTES (64UL * 1024UL)
#define IB_FIRST_OFFSET 0UL
#define IB_FIRST_DWORDS 8UL
#define IB_SECOND_OFFSET 4096UL
#define IB_SECOND_DWORDS 64UL
#define IB_THIRD_OFFSET 8192UL
#define IB_THIRD_DWORDS RADEON3D_MAX_BATCH_DWORDS
/* A working indirect buffer retires its fence in ~100 microseconds; five
 * seconds is a 40000x margin. Longer waits busy-poll MMIO at task priority
 * and starve the rest of the machine when the fence does not retire. */
#define FENCE_WAIT_MS 5000UL
/* CP packet type 2 (no-op); identical value to the driver's internal
 * RADEON_CP_PACKET2, restated here because tools see only the public ABI. */
#define IB_PACKET2 0x80000000UL

/* Indirect buffers are fetched by the CP with the same convention as the
 * ring: the CP's internal instruction stream is little-endian and this
 * driver does not set RADEON_BUF_SWAP_32BIT in CP_RB_CNTL (it pre-swaps
 * ring dwords in software instead). A producer must therefore store each
 * dword byte-reversed so the CP reads the value it was given. On PPC the
 * equivalent store is stwbrx. A native-endian IB decodes as garbage
 * packets and stalls the CP (proven on hardware 2026-09-08).
 *
 * Content note (hardware-proven 2026-09-08): an indirect buffer must not
 * consist solely of PACKET2 no-ops -- the boot probe passes the same
 * 64-dword IB from the same segment when it begins with one real packet.
 * Real emitters begin with state packets anyway; the gate tool mirrors
 * that shape. */
static ULONG CpNativeDword(ULONG value)
{
    return ((value & 0x000000ffUL) << 24) |
           ((value & 0x0000ff00UL) << 8) |
           ((value & 0x00ff0000UL) >> 8) |
           ((value & 0xff000000UL) >> 24);
}

struct Library *Radeon9200Base;

static BOOL ExpectReject(struct Radeon3DDevice *device,
                         const struct Radeon3DIndirect *indirect,
                         ULONG stage, const char *name)
{
    ULONG fence = 0;
    BOOL result = Radeon3DDispatchIndirect(device, indirect, &fence);

    if (result) {
        printf("R3DIB negative=%s status=unexpected_pass\n", name);
        return FALSE;
    }
    if (fence != (0x80000000UL | stage)) {
        printf("R3DIB negative=%s status=wrong_stage fence=%08lx "
               "want=%08lx\n", name,
               (unsigned long)fence,
               (unsigned long)(0x80000000UL | stage));
        return FALSE;
    }
    printf("R3DIB negative=%s stage=%lu ok\n", name,
           (unsigned long)stage);
    return TRUE;
}

int main(void)
{
    struct Radeon3DInfo info;
    struct Radeon3DDevice *device;
    struct Radeon3DDevice *legacy;
    struct Radeon3DSegment segment;
    struct Radeon3DIndirect indirect;
    volatile ULONG *ib;
    /* The service API takes host-endian dwords and swaps them into the
     * ring itself. Only raw segment contents (the IB body) are CP-native. */
    ULONG command = IB_PACKET2;
    ULONG fence;
    ULONG index;
    ULONG ok = 1;

    Radeon9200Base = OpenLibrary(
        (CONST_STRPTR)CHIP_LIBRARY_NAME, RADEON3D_LIBRARY_VERSION);
    if (!Radeon9200Base) {
        printf("R3DIB status=open_library_failed\n");
        return 20;
    }

    info.Size = sizeof(info);
    device = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
    if (!device) {
        printf("R3DIB status=open_session_failed\n");
        CloseLibrary(Radeon9200Base);
        return 20;
    }
    printf("R3DIB iface=%lu caps=%08lx\n",
           (unsigned long)info.Version, (unsigned long)info.Caps);
    if (!(info.Caps & RADEON3D_CAP_INDIRECT_DISPATCH) ||
        !(info.Caps & RADEON3D_CAP_STREAM_SEGMENTS) ||
        !(info.Caps & RADEON3D_CAP_CP_READY)) {
        printf("R3DIB status=caps_missing\n");
        Radeon3DClose(device);
        CloseLibrary(Radeon9200Base);
        return 20;
    }

    /* Step 2: baseline CP health without any indirect buffer. */
    if (!Radeon3DSubmit(device, &command, 1, RADEON3D_SUBMIT_FENCE,
                        &fence) ||
        !Radeon3DWaitFence(device, fence, FENCE_WAIT_MS)) {
        printf("R3DIB status=baseline_submit_failed\n");
        Radeon3DClose(device);
        CloseLibrary(Radeon9200Base);
        return 20;
    }
    printf("R3DIB baseline=ok\n");

    /* Step 3: segment lease, PACKET2 fill, first dispatch. */
    memset(&segment, 0, sizeof(segment));
    segment.Size = sizeof(segment);
    if (!Radeon3DAllocSegment(device, IB_SEGMENT_BYTES, &segment)) {
        printf("R3DIB status=alloc_segment_failed\n");
        Radeon3DClose(device);
        CloseLibrary(Radeon9200Base);
        return 20;
    }
    printf("R3DIB segment=%lu cpu=%08lx gpu=%08lx bytes=%lu\n",
           (unsigned long)segment.Id,
           (unsigned long)segment.CpuAddress,
           (unsigned long)segment.GpuAddress,
           (unsigned long)segment.Bytes);
    ib = (volatile ULONG *)segment.CpuAddress;
    /* Test 1 mirrors the proven boot probe exactly: 8 dwords, one real
     * packet (a scratch-register write) then no-op padding, minimal fill.
     * PACKET0(RADEON_SCRATCH_REG1=0x15e4, 0) = (0x15e4 >> 2) = 0x579. */
    ib[0] = CpNativeDword(0x00000579UL);
    ib[1] = CpNativeDword(0x2a1d5c1dUL);
    for (index = 2; index < IB_FIRST_DWORDS; ++index)
        ib[index] = CpNativeDword(0x80000000UL);
    (void)ib[IB_FIRST_DWORDS - 1UL];

    memset(&indirect, 0, sizeof(indirect));
    indirect.Size = sizeof(indirect);
    indirect.Version = RADEON3D_INDIRECT_VERSION;
    indirect.SegmentId = segment.Id;
    indirect.ByteOffset = IB_FIRST_OFFSET;
    indirect.DwordCount = IB_FIRST_DWORDS;
    if (!Radeon3DDispatchIndirect(device, &indirect, &fence)) {
        struct Radeon3DInfo failure;

        memset(&failure, 0, sizeof(failure));
        failure.Size = sizeof(failure);
        (void)Radeon3DGetInfo(device, &failure);
        printf("R3DIB status=first_dispatch_failed stage=%08lx\n",
               (unsigned long)failure.CommitFailStage);
        ok = 0;
    } else if (!Radeon3DWaitFence(device, fence, FENCE_WAIT_MS)) {
        struct Radeon3DInfo trail;

        memset(&trail, 0, sizeof(trail));
        trail.Size = sizeof(trail);
        (void)Radeon3DGetInfo(device, &trail);
        printf("R3DIB status=fence_timeout step=first trail=%08lx\n",
               (unsigned long)trail.CommitFailStage);
        ok = 0;
    } else {
        printf("R3DIB first=ok dwords=%lu\n",
               (unsigned long)indirect.DwordCount);
    }

    /* Test 2: 64 dwords at a 4 KiB offset, after extending the fill. */
    if (ok) {
        for (index = IB_FIRST_DWORDS; index < 4160UL; ++index)
            ib[index] = CpNativeDword(0x80000000UL);
        (void)ib[4159UL];
        indirect.ByteOffset = IB_SECOND_OFFSET;
        indirect.DwordCount = IB_SECOND_DWORDS;
        if (!Radeon3DDispatchIndirect(device, &indirect, &fence)) {
            printf("R3DIB status=second_dispatch_failed\n");
            ok = 0;
        } else if (!Radeon3DWaitFence(device, fence, FENCE_WAIT_MS)) {
            struct Radeon3DInfo trail;

            memset(&trail, 0, sizeof(trail));
            trail.Size = sizeof(trail);
            (void)Radeon3DGetInfo(device, &trail);
            printf("R3DIB status=fence_timeout step=second "
                   "trail=%08lx\n",
                   (unsigned long)trail.CommitFailStage);
            ok = 0;
        } else {
            printf("R3DIB second=ok dwords=%lu\n",
                   (unsigned long)indirect.DwordCount);
        }
    }

    /* Test 3: size ladder at a fixed offset, coarse steps. A working IB
     * retires in ~100us; failures cost the 5s wait budget each. */
    if (ok) {
        static const ULONG ladder[4] = { 256UL, 1024UL, 4096UL, 8192UL };
        ULONG step;

        for (index = 4160UL; index < IB_THIRD_OFFSET + 8192UL; ++index)
            ib[index] = CpNativeDword(0x80000000UL);
        (void)ib[IB_THIRD_OFFSET + 8191UL];
        for (step = 0; step < 4UL; ++step) {
            indirect.ByteOffset = IB_THIRD_OFFSET;
            indirect.DwordCount = ladder[step];
            if (!Radeon3DDispatchIndirect(device, &indirect, &fence)) {
                printf("R3DIB status=ladder_dispatch_failed size=%lu\n",
                       (unsigned long)ladder[step]);
                ok = 0;
                break;
            } else if (!Radeon3DWaitFence(device, fence, FENCE_WAIT_MS)) {
                struct Radeon3DInfo trail;

                memset(&trail, 0, sizeof(trail));
                trail.Size = sizeof(trail);
                (void)Radeon3DGetInfo(device, &trail);
                printf("R3DIB status=fence_timeout size=%lu trail=%08lx\n",
                       (unsigned long)ladder[step],
                       (unsigned long)trail.CommitFailStage);
                ok = 0;
                break;
            } else {
                printf("R3DIB ladder=ok size=%lu\n",
                       (unsigned long)ladder[step]);
            }
        }
    }

    /* Step 5: rejection matrix. Each entry pins one service stage. */
    if (ok) {
        struct Radeon3DIndirect bad;

        bad = indirect;
        bad.ByteOffset = 4UL;
        ok = ExpectReject(device, &bad, 101UL, "unaligned");
        bad = indirect;
        bad.Version = 0UL;
        ok &= ExpectReject(device, &bad, 101UL, "version");
        bad = indirect;
        bad.Flags = 1UL;
        ok &= ExpectReject(device, &bad, 101UL, "flags");
        bad = indirect;
        bad.DwordCount = RADEON3D_MAX_BATCH_DWORDS + 1UL;
        ok &= ExpectReject(device, &bad, 101UL, "count");
        bad = indirect;
        bad.SegmentId = RADEON3D_MAX_SEGMENTS;
        ok &= ExpectReject(device, &bad, 103UL, "segment");
        bad = indirect;
        bad.ByteOffset = IB_SEGMENT_BYTES - 4096UL;
        bad.DwordCount = IB_THIRD_DWORDS;
        ok &= ExpectReject(device, &bad, 104UL, "range");
    }

    /* Step 6: an interface-17 session gets no indirect dispatch. */
    if (ok) {
        info.Size = sizeof(info);
        legacy = Radeon3DOpen(17UL, &info);
        if (!legacy) {
            printf("R3DIB status=legacy_open_failed\n");
            ok = 0;
        } else {
            ULONG legacyFence = 0;
            BOOL accepted = Radeon3DDispatchIndirect(
                legacy, &indirect, &legacyFence);

            printf("R3DIB legacy accepted=%d fence=%08lx\n",
                   accepted ? 1 : 0, (unsigned long)legacyFence);
            ok &= !accepted;
            Radeon3DClose(legacy);
        }
    }

    (void)Radeon3DFreeSegment(device, segment.Id);
    Radeon3DClose(device);
    CloseLibrary(Radeon9200Base);
    printf("R3DIB status=%s\n", ok ? "pass" : "fail");
    return ok ? 0 : 20;
}
