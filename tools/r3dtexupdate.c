#include <devices/timer.h>
#include <dos/dos.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <proto/radeon3d.h>
#include <stdio.h>
#include <string.h>

#define SIDE 64UL
#define ROUNDS 16UL
#define REUSE_ROUNDS 8UL
#define REUSE_TRIES 8UL
#define WAIT_MS 1000UL
#define BUDGET_SECONDS 30UL
#define RECORD_WORDS (RADEON3D_EXEC_DRAW_FRAGMENT_HEADER_DWORDS + \
                      6UL * RADEON3D_EXEC_VERTEX_DWORDS)

struct Library *Radeon9200Base;
struct Device *TimerBase;
static ULONG ClockHz;
static unsigned long long StartTicks;
static ULONG Checks, PixelFailures, Reuses;

static unsigned long long Ticks(void)
{
    struct EClockVal value;
    ReadEClock(&value);
    return ((unsigned long long)value.ev_hi << 32) | value.ev_lo;
}

static BOOL WithinBudget(void)
{
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
        puts("R3DTEXUPDATE abort=ctrl-c");
        return FALSE;
    }
    if (Ticks() - StartTicks >=
        (unsigned long long)ClockHz * BUDGET_SECONDS) {
        puts("R3DTEXUPDATE abort=deadline");
        return FALSE;
    }
    return TRUE;
}

static BOOL AllocAux(struct Radeon3DDevice *device,
                     struct Radeon3DSurface *surface, ULONG format,
                     ULONG generation)
{
    ULONG bytes = format == RADEON3D_FORMAT_R5G6B5PC ? 2UL : 4UL;
    BOOL valid;
    memset(surface, 0, sizeof(*surface));
    surface->Size = sizeof(*surface);
    if (!Radeon3DAllocSurface(device, SIDE, SIDE, format, surface)) {
        printf("R3DTEXUPDATE allocation=failed format=%lu\n",
               (unsigned long)format);
        return FALSE;
    }
    printf("R3DTEXUPDATE allocation format=%lu handle=%08lx cpu=%08lx "
           "gpu=%08lx pitch=%lu width=%lu height=%lu\n", (unsigned long)format,
           (unsigned long)surface->Handle, (unsigned long)surface->CpuAddress,
           (unsigned long)surface->GpuAddress, (unsigned long)surface->Pitch,
           (unsigned long)surface->Width, (unsigned long)surface->Height);
    /* Exact pitch makes the single-level POT sampler layout unambiguous. */
    valid = surface->Version == RADEON3D_SURFACE_VERSION &&
           surface->Generation == generation && surface->Handle &&
           surface->CpuAddress && surface->Format == format &&
           surface->Pitch == SIDE * bytes && surface->Width == SIDE &&
           surface->Height == SIDE && !(surface->GpuAddress & 31UL) &&
           surface->GpuAddress <= 0xffffffffUL - surface->Pitch * SIDE &&
           (ULONG)surface->CpuAddress <=
               0xffffffffUL - surface->Pitch * SIDE;
    if (!valid) puts("R3DTEXUPDATE allocation=invalid-descriptor-or-layout");
    return valid;
}

static BOOL Distinct(const struct Radeon3DSurface *a,
                     const struct Radeon3DSurface *b)
{
    ULONG ac = (ULONG)a->CpuAddress, bc = (ULONG)b->CpuAddress;
    ULONG ab = a->Pitch * a->Height, bb = b->Pitch * b->Height;
    BOOL distinct = a->Handle != b->Handle &&
           (ac + ab <= bc || bc + bb <= ac) &&
           (a->GpuAddress + ab <= b->GpuAddress ||
            b->GpuAddress + bb <= a->GpuAddress);
    if (!distinct) puts("R3DTEXUPDATE allocation=alias-or-overlap");
    return distinct;
}

/* Saturated primaries/white expand exactly from 565 to 8888. Quadrants
 * change on every write; interior readback avoids raster/filter edges. */
static ULONG Color(ULONG x, ULONG y, ULONG phase)
{
    static const ULONG colors[4] = {
        0x0000ffffUL, 0x00ff00ffUL, 0xff0000ffUL, 0xffffffffUL
    };
    return colors[((x >= SIDE / 2UL) +
                   2UL * (y >= SIDE / 2UL) + phase) & 3UL];
}

static void WriteTexture(const struct Radeon3DSurface *surface, ULONG phase)
{
    ULONG x, y, color, packed;
    ULONG bytes = surface->Format == RADEON3D_FORMAT_R5G6B5PC ? 2UL : 4UL;
    for (y = 0; y < SIDE; ++y) {
        volatile UBYTE *pixel = (volatile UBYTE *)surface->CpuAddress +
                                y * surface->Pitch;
        for (x = 0; x < SIDE; ++x, pixel += bytes) {
            color = Color(x, y, phase);
            if (bytes == 2UL) {
                packed = (((color >> 8) & 255UL) >> 3) << 11 |
                         (((color >> 16) & 255UL) >> 2) << 5 |
                         ((color >> 24) >> 3);
                pixel[0] = (UBYTE)packed;
                pixel[1] = (UBYTE)(packed >> 8);
            } else {
                pixel[0] = (UBYTE)(color >> 24);
                pixel[1] = (UBYTE)(color >> 16);
                pixel[2] = (UBYTE)(color >> 8);
                pixel[3] = (UBYTE)color;
            }
        }
    }
    /* No client cache/MMIO workaround or extra aperture read: Execute owns
     * the documented final-byte drain before GPU texture fetches. */
}

/* 0: verified, 5: pixel mismatch (safe to continue), 20: stop/IO failure. */
static int Sample(struct Radeon3DDevice *device,
                  const struct Radeon3DSurface *target,
                  const struct Radeon3DSurface *texture,
                  const char *stage, ULONG round, ULONG phase, BOOL write)
{
    static const ULONG corners[12] = {0, 0, 1, 0, 0, 1,
                                      1, 0, 1, 1, 0, 1};
    ULONG records[RECORD_WORDS] = {0};
    ULONG i, x, y, fence = 0, bad = 0, firstX = 0, firstY = 0;
    ULONG firstGot = 0, firstWant = 0, got, want;
    unsigned long long t0, t1, t2, t3, t4, writeNanos;
    BOOL submitted, waited;
    if (!WithinBudget()) return 20;
    records[0] = RADEON3D_EXEC_DRAW_TRIANGLES;
    records[1] = RECORD_WORDS;
    records[2] = (ULONG)target->Handle;
    records[4] = (ULONG)texture->Handle;
    records[5] = RADEON3D_DRAW_TEXTURED | RADEON3D_DRAW_FRAGMENT_STATE;
    records[8] = SIDE;
    records[9] = SIDE;
    records[10] = 6UL;
    records[12] = (SIDE - 1UL) | ((SIDE - 1UL) << 16);
    /* Nearest, clamp, replace, one level. Content bits are NOT accepted by
     * the current validator even though the public header names them. */
    for (i = 0; i < 6UL; ++i) {
        ULONG *v = records + RADEON3D_EXEC_DRAW_FRAGMENT_HEADER_DWORDS +
                   i * RADEON3D_EXEC_VERTEX_DWORDS;
        v[0] = corners[2UL * i] ? 0x42800000UL : 0UL; /* 64.0 */
        v[1] = corners[2UL * i + 1UL] ? 0x42800000UL : 0UL;
        v[3] = corners[2UL * i] ? 0x3f800000UL : 0UL;
        v[4] = corners[2UL * i + 1UL] ? 0x3f800000UL : 0UL;
        v[5] = 0xffffffffUL;
    }
    t0 = Ticks();
    if (write) WriteTexture(texture, phase);
    t1 = Ticks();
    submitted = Radeon3DExecute(device, records, RECORD_WORDS,
                                RADEON3D_SUBMIT_FENCE, &fence);
    t2 = Ticks();
    waited = submitted && fence && Radeon3DWaitFence(device, fence, WAIT_MS);
    t3 = Ticks();
    if (!waited) {
        printf("R3DTEXUPDATE stage=%s round=%lu format=%lu submit=%ld "
               "fence=%08lx wait=failed\n", stage, (unsigned long)round,
               (unsigned long)texture->Format, (long)submitted,
               (unsigned long)fence);
        return 20;
    }
    /* Every subsequent write/release is after this completed use fence.
     * Read only returned target memory, never registers or private packets. */
    for (y = 4UL; y < SIDE - 4UL; ++y) {
        if (y >= 28UL && y < 36UL) continue;
        for (x = 4UL; x < SIDE - 4UL; ++x) {
            volatile UBYTE *p;
            if (x >= 28UL && x < 36UL) continue;
            p = (volatile UBYTE *)target->CpuAddress + y * target->Pitch +
                x * 4UL;
            got = ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
                  ((ULONG)p[2] << 8) | p[3];
            want = Color(x, y, phase);
            if (got != want) {
                if (!bad) {
                    firstX = x; firstY = y; firstGot = got; firstWant = want;
                }
                ++bad;
            }
        }
    }
    t4 = Ticks();
    ++Checks;
    if (bad) ++PixelFailures;
    writeNanos = (t1 - t0) * 1000000000ULL / ClockHz;
    printf("R3DTEXUPDATE stage=%s round=%lu format=%lu phase=%lu write=%ld "
           "bad=%lu ticks_write=%lu ticks_execute=%lu ticks_wait=%lu "
           "ticks_verify=%lu write_us=%lu.%03lu\n", stage, (unsigned long)round,
           (unsigned long)texture->Format, (unsigned long)phase, (long)write,
           (unsigned long)bad, (unsigned long)(t1-t0), (unsigned long)(t2-t1),
           (unsigned long)(t3-t2), (unsigned long)(t4-t3),
           (unsigned long)(writeNanos / 1000ULL),
           (unsigned long)(writeNanos % 1000ULL));
    if (bad)
        printf("R3DTEXUPDATE pixel x=%lu y=%lu got=%08lx expected=%08lx\n",
               (unsigned long)firstX, (unsigned long)firstY,
               (unsigned long)firstGot, (unsigned long)firstWant);
    return bad ? 5 : 0;
}

static int TestFormat(struct Radeon3DDevice *device, ULONG format,
                      ULONG generation)
{
    struct Radeon3DSurface target = {0}, a = {0}, b = {0};
    ULONG i, attempt, oldGpu, oldCpu;
    int result = 20;
    if (!AllocAux(device, &target, RADEON3D_FORMAT_B8G8R8A8, generation) ||
        !AllocAux(device, &a, format, generation) ||
        !Distinct(&target, &a)) goto out;
    for (i = 0; i < ROUNDS; ++i)
        if (Sample(device, &target, &a, "in-place", i, i, TRUE) == 20)
            goto out;
    if (!AllocAux(device, &b, format, generation) ||
        !Distinct(&a, &b) || !Distinct(&target, &b)) goto out;
    for (i = 0; i < ROUNDS; ++i) {
        if (Sample(device, &target, &a, "live-A", i, i, TRUE) == 20 ||
            Sample(device, &target, &b, "live-B", i, i + 1UL, TRUE) == 20 ||
            Sample(device, &target, &a, "live-A-preserved", i, i, FALSE) == 20)
            goto out;
    }
    /* Keep B live, and leave A as the most recently sampled address. Each
     * replacement starts with different pixels at the exact old GPU address. */
    for (i = 0; i < REUSE_ROUNDS; ++i) {
        /* Color repeats every four phases: offset by 17, not 16. Use the
         * same phase for the upload and both replacement readbacks. */
        const ULONG phase = ROUNDS + i + 1UL;
        oldGpu = a.GpuAddress;
        oldCpu = (ULONG)a.CpuAddress;
        Radeon3DReleaseSurface(device, &a);
        for (attempt = 0; attempt < REUSE_TRIES; ++attempt) {
            if (!WithinBudget() || !AllocAux(device, &a, format, generation) ||
                !Distinct(&a, &b) || !Distinct(&a, &target)) goto out;
            if (a.GpuAddress == oldGpu) break;
            Radeon3DReleaseSurface(device, &a);
        }
        if (attempt == REUSE_TRIES) {
            printf("R3DTEXUPDATE reuse=NOT-OBSERVED format=%lu old_gpu=%08lx\n",
                   (unsigned long)format, (unsigned long)oldGpu);
            result = 10;
            goto out;
        }
        ++Reuses;
        printf("R3DTEXUPDATE reuse=observed round=%lu old_gpu=%08lx "
               "new_gpu=%08lx old_cpu=%08lx new_cpu=%08lx\n",
               (unsigned long)i, (unsigned long)oldGpu,
               (unsigned long)a.GpuAddress, (unsigned long)oldCpu,
               (unsigned long)a.CpuAddress);
        if (Sample(device, &target, &a, "reallocated", i,
                   phase, TRUE) == 20 ||
            Sample(device, &target, &b, "reuse-B-preserved", i,
                   ROUNDS, FALSE) == 20 ||
            Sample(device, &target, &a, "reuse-A-preserved", i,
                   phase, FALSE) == 20) goto out;
    }
    result = 0;
out:
    if (b.Handle) Radeon3DReleaseSurface(device, &b);
    if (a.Handle) Radeon3DReleaseSurface(device, &a);
    if (target.Handle) Radeon3DReleaseSurface(device, &target);
    return result;
}

int main(void)
{
    struct MsgPort *port = NULL;
    struct timerequest *timer = NULL;
    struct Radeon3DDevice *device = NULL;
    struct Radeon3DInfo info = {0};
    struct EClockVal clock;
    ULONG i, overhead = 0xffffffffUL;
    const ULONG caps = RADEON3D_CAP_CP_READY | RADEON3D_CAP_FENCES |
        RADEON3D_CAP_PHASE2_EXECUTE | RADEON3D_CAP_TEXTURE_STATE |
        RADEON3D_CAP_COLOR_TARGET_FORMATS | RADEON3D_CAP_AUX_SURFACES;
    int result = 20;
    port = CreateMsgPort();
    if (!port) {
        puts("R3DTEXUPDATE setup=message-port-failed");
        goto out;
    }
    timer = (struct timerequest *)CreateIORequest(port, sizeof(*timer));
    if (!timer || OpenDevice((CONST_STRPTR)"timer.device", UNIT_ECLOCK,
                            (struct IORequest *)timer, 0)) {
        puts("R3DTEXUPDATE setup=timer-failed");
        goto out;
    }
    TimerBase = timer->tr_node.io_Device;
    ClockHz = ReadEClock(&clock); /* Return value is Hz, NOT ev_lo. */
    if (!ClockHz) {
        puts("R3DTEXUPDATE setup=zero-eclock-rate");
        goto out;
    }
    for (i = 0; i < 32UL; ++i) {
        unsigned long long start = Ticks();
        ULONG elapsed = (ULONG)(Ticks() - start);
        if (elapsed < overhead) overhead = elapsed;
    }
    Radeon9200Base = OpenLibrary((CONST_STRPTR)"Radeon9200.chip",
                                 RADEON3D_LIBRARY_VERSION);
    if (!Radeon9200Base) {
        puts("R3DTEXUPDATE setup=chip-library-failed");
        goto out;
    }
    info.Size = sizeof(info);
    device = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);
    if (!device || info.Version != RADEON3D_IFACE_VERSION ||
        (info.Caps & caps) != caps) {
        puts("R3DTEXUPDATE setup=service-or-required-capability-failed");
        goto out;
    }
    printf("R3DTEXUPDATE iface=%lu generation=%lu device=%08lx caps=%08lx "
           "eclock_hz=%lu service_eclock_hz=%lu clock_pair_min_ticks=%lu "
           "side=%lu rounds=%lu reuse_rounds=%lu wait_ms=%lu budget_s=%lu\n",
           (unsigned long)info.Version, (unsigned long)info.Generation,
           (unsigned long)info.DeviceId, (unsigned long)info.Caps,
           (unsigned long)ClockHz, (unsigned long)info.EClockHz,
           (unsigned long)overhead, SIDE, ROUNDS, REUSE_ROUNDS, WAIT_MS,
           BUDGET_SECONDS);
    StartTicks = Ticks();
    result = TestFormat(device, RADEON3D_FORMAT_R5G6B5PC, info.Generation);
    if (!result)
        result = TestFormat(device, RADEON3D_FORMAT_B8G8R8A8, info.Generation);
    if (!result && PixelFailures) result = 5;
out:
    if (device) Radeon3DClose(device);
    if (Radeon9200Base) CloseLibrary(Radeon9200Base);
    if (TimerBase) CloseDevice((struct IORequest *)timer);
    if (timer) DeleteIORequest((struct IORequest *)timer);
    if (port) DeleteMsgPort(port);
    printf("R3DTEXUPDATE status=%s result=%d checks=%lu pixel_failures=%lu "
           "address_reuses=%lu\n", result ? "NOT-PASS" : "PASS", result,
           (unsigned long)Checks, (unsigned long)PixelFailures,
           (unsigned long)Reuses);
    return result;
}
