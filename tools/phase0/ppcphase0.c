/*
 * Phase-0 PPC probe (WarpOS). docs/09-ppc-direct-ring-design.md, section 7.
 *
 * Usage (from an AmigaDOS shell, after phase0host prints the address):
 *
 *     Work:ppcphase0 <controlSegmentAddressHex> [--no-mmio-write]
 *
 * The control address is the 68k-mapped CpuAddress of the control-block
 * streaming segment. The MiniGL WarpOS VRAM probe already proved that the
 * PPC aliases the 68k address map for Radeon VRAM; this probe extends the
 * evidence to BAR2 MMIO and measures the costs a direct ring writer would
 * pay:
 *
 *   1. MMIO read cost over RBBM_STATUS / CP_RB_RPTR / SCRATCH_REG0.
 *   2. MMIO write cost over SCRATCH_REG1, posted and committed, gated by
 *      the control block's P0_FLAG_MMIO_WRITE_OK and --no-mmio-write.
 *   3. Aperture store bandwidth over the arena segment: native big-endian
 *      stores and stwbrx byte-reversed stores (the ring-dword shape).
 *   4. Aperture read cost.
 *   5. stwbrx byte-order verification; the 68k host cross-checks the same
 *      dword, which doubles as the cross-CPU ordering proof.
 *   6. Results published into the control block, acknowledged last with a
 *      readback.
 *
 * Exit codes: 0 ok, 10 validation or stage failure, 20 usage.
 */

#include <exec/types.h>
#include <stdio.h>
#include <string.h>

#include "phase0_regs.h"

struct PPCBase;
extern struct PPCBase *PowerPCBase;

struct timeval {
    ULONG tv_secs;
    ULONG tv_micro;
};

VOID __GetSysTimePPC(void *, struct timeval *) =
    "\tlwz\tr0,-682(r3)\n\tmtlr\tr0\n\tblrl";
#define GetSysTimePPC(value) __GetSysTimePPC(PowerPCBase, (value))

extern ULONG P0MmioRead32(volatile void *address);
extern void P0MmioWrite32(volatile void *address, ULONG value);
extern ULONG P0MmioWriteCommit(volatile void *address, ULONG value);
extern void P0WriteBarrier(void);
extern void P0FlushRange(void *address, ULONG bytes);
extern void P0FillBurstNative(volatile ULONG *destination, ULONG value,
                              ULONG dwords);
extern void P0FillBurstBr(volatile ULONG *destination, ULONG value,
                          ULONG dwords);
extern ULONG P0SumBurst(volatile ULONG *source, ULONG dwords);

static ULONG ElapsedMicros(const struct timeval *start,
                           const struct timeval *end)
{
    ULONG seconds = end->tv_secs - start->tv_secs;
    LONG micros = (LONG)end->tv_micro - (LONG)start->tv_micro;

    if (micros < 0) {
        --seconds;
        micros += 1000000L;
    }
    return seconds * 1000000UL + (ULONG)micros;
}

static ULONG NanosPerOp(ULONG micros, ULONG iterations)
{
    if (!iterations)
        return 0;
    return (micros * 1000UL) / iterations;
}

static ULONG KiloBytesPerSecond(ULONG bytes, ULONG passes, ULONG micros)
{
    unsigned long long total = (unsigned long long)bytes * passes;

    if (!micros)
        return 0;
    return (ULONG)(total * 1000ULL /
                   ((unsigned long long)micros * 1024ULL));
}

/* Hex or decimal parse without stdlib beyond the proven WarpOS runtime. */
static ULONG ParseAddress(const char *text)
{
    ULONG value = 0;
    ULONG index = 0;

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        index = 2;
    for (; text[index]; ++index) {
        char digit = text[index];

        if (digit >= '0' && digit <= '9')
            value = (value << 4) | (ULONG)(digit - '0');
        else if (digit >= 'a' && digit <= 'f')
            value = (value << 4) | (ULONG)(digit - 'a' + 10);
        else if (digit >= 'A' && digit <= 'F')
            value = (value << 4) | (ULONG)(digit - 'A' + 10);
        else
            return 0;
    }
    return value;
}

int main(int argc, char **argv)
{
    volatile ULONG *block = NULL;
    volatile UBYTE *mmioBase = NULL;
    volatile void *rbbm;
    volatile void *rptr;
    volatile void *scratch0;
    volatile void *scratch1;
    volatile ULONG *arena = NULL;
    struct timeval start;
    struct timeval end;
    ULONG controlAddress = 0;
    ULONG mmioReadMicros;
    ULONG postedMicros;
    ULONG commitMicros;
    ULONG nativeMicros;
    ULONG brMicros;
    ULONG aperReadMicros;
    ULONG readSum = 0;
    ULONG commitReadback = 0;
    ULONG selfRead = 0;
    ULONG stwbrxOk = 0;
    ULONG scratchValue = 0;
    ULONG stage = P0_STAGE_ARGS;
    ULONG allowMmioWrite = 0;
    ULONG index;
    int result = 20;

    mmioReadMicros = 0;
    postedMicros = 0;
    commitMicros = 0;
    nativeMicros = 0;
    brMicros = 0;
    aperReadMicros = 0;

    if (argc < 2 || argc > 3)
        goto publish;
    controlAddress = ParseAddress(argv[1]);
    if (!controlAddress || (controlAddress & 3UL))
        goto publish;
    allowMmioWrite = !(argc == 3 && !strcmp(argv[2], "--no-mmio-write"));

    block = (volatile ULONG *)controlAddress;
    stage = P0_STAGE_BLOCK_MAGIC;
    if (block[P0_I_MAGIC] != P0_MAGIC)
        goto publish;
    stage = P0_STAGE_VERSION;
    if (block[P0_I_VERSION] != P0_VERSION)
        goto publish;
    stage = P0_STAGE_HOST_DONE;
    if (block[P0_I_HOST_DONE] != P0_HOST_DONE)
        goto publish;
    if (!(block[P0_I_FLAGS] & P0_FLAG_MMIO_WRITE_OK))
        allowMmioWrite = 0;
    stage = P0_STAGE_BAR2_RANGE;
    if (!block[P0_I_BAR2_CPU] || block[P0_I_BAR2_BYTES] < 0x1600UL)
        goto publish;
    stage = P0_STAGE_ARENA_RANGE;
    if (!block[P0_I_ARENA_CPU] ||
        block[P0_I_ARENA_CPU] < block[P0_I_BAR0_CPU] ||
        block[P0_I_ARENA_CPU] + block[P0_I_ARENA_BYTES] >
            block[P0_I_BAR0_CPU] + block[P0_I_BAR0_BYTES])
        goto publish;

    mmioBase = (volatile UBYTE *)(ULONG)block[P0_I_BAR2_CPU];
    arena = (volatile ULONG *)block[P0_I_ARENA_CPU];
    rbbm = mmioBase + P0_RBBM_STATUS;
    rptr = mmioBase + P0_CP_RB_RPTR;
    scratch0 = mmioBase + P0_SCRATCH_REG0;
    scratch1 = mmioBase + P0_SCRATCH_REG1;

    /* 1. MMIO read cost: three always-safe registers, interleaved so the
     * loop is not a single-register pipeline artifact. */
    stage = P0_STAGE_MMIO_READ;
    {
        ULONG value = 0;

        GetSysTimePPC(&start);
        for (index = 0; index < P0_MMIO_ITERATIONS; ++index) {
            value ^= P0MmioRead32(rbbm);
            value ^= P0MmioRead32(rptr);
            value ^= P0MmioRead32(scratch0);
        }
        GetSysTimePPC(&end);
        mmioReadMicros = ElapsedMicros(&start, &end);
        readSum ^= value;
    }

    /* 2. MMIO write cost over SCRATCH_REG1: posted first, then committed
     * (store + readback of the same register, the driver's publication
     * shape). */
    stage = P0_STAGE_MMIO_WRITE;
    if (allowMmioWrite) {
        GetSysTimePPC(&start);
        for (index = 0; index < P0_MMIO_ITERATIONS; ++index)
            P0MmioWrite32(scratch1, 0UL);
        P0WriteBarrier();
        GetSysTimePPC(&end);
        postedMicros = ElapsedMicros(&start, &end);

        GetSysTimePPC(&start);
        for (index = 0; index < P0_MMIO_COMMIT_ITERS; ++index)
            commitReadback ^= P0MmioWriteCommit(scratch1,
                                                P0_APER_PATTERN ^ index);
        GetSysTimePPC(&end);
        commitMicros = ElapsedMicros(&start, &end);
    }

    /* 3. Aperture store bandwidth: one burst per pass, native big-endian
     * and stwbrx byte-reversed, barrier between passes. */
    stage = P0_STAGE_APER_STORE;
    {
        ULONG dwords = P0_APER_BURST_BYTES / sizeof(ULONG);
        ULONG pass;

        for (index = 0; index < dwords; ++index)
            arena[index] = P0_APER_PATTERN ^ index;
        P0WriteBarrier();

        GetSysTimePPC(&start);
        for (pass = 0; pass < P0_APER_PASSES; ++pass) {
            P0FillBurstNative(arena, P0_APER_PATTERN ^ pass, dwords);
            P0WriteBarrier();
        }
        GetSysTimePPC(&end);
        nativeMicros = ElapsedMicros(&start, &end);

        GetSysTimePPC(&start);
        for (pass = 0; pass < P0_APER_PASSES; ++pass) {
            P0FillBurstBr(arena, P0_APER_PATTERN ^ (pass + 1UL), dwords);
            P0WriteBarrier();
        }
        GetSysTimePPC(&end);
        brMicros = ElapsedMicros(&start, &end);
    }

    /* 4. Aperture read cost. */
    stage = P0_STAGE_APER_READ;
    GetSysTimePPC(&start);
    for (index = 0; index < 8UL; ++index)
        readSum ^= P0SumBurst(arena, P0_APER_BURST_BYTES / sizeof(ULONG));
    GetSysTimePPC(&end);
    aperReadMicros = ElapsedMicros(&start, &end);

    /* 5. stwbrx byte-order verification against the little-endian dword
     * 0x5a5aa5a5, whose bytes on the bus are A5 A5 5A 5A. */
    stage = P0_STAGE_STWBRX;
    {
        volatile UBYTE *bytes = (volatile UBYTE *)arena;

        P0FillBurstBr(arena, P0_APER_PATTERN, 1UL);
        P0WriteBarrier();
        if (bytes[0] == 0xA5 && bytes[1] == 0xA5 && bytes[2] == 0x5A &&
            bytes[3] == 0x5A)
            stwbrxOk = 1;
        selfRead = P0MmioRead32((volatile void *)arena);
        scratchValue = P0MmioRead32(scratch1);
    }

    stage = 0;
    result = 0;

publish:
    if (block) {
        if (stage) {
            block[P0_I_PPC_STATUS] = 1UL;
            block[P0_I_PPC_STAGE] = stage;
            P0WriteBarrier();
            (void)block[P0_I_PPC_STAGE];
        } else {
            block[P0_I_PPC_STATUS] = 0UL;
            block[P0_I_PPC_STAGE] = 0UL;
            block[P0_I_PPC_MMIO_READ_NS] =
                NanosPerOp(mmioReadMicros, P0_MMIO_ITERATIONS * 3UL);
            block[P0_I_PPC_MMIO_WRITE_NS] =
                NanosPerOp(postedMicros, P0_MMIO_ITERATIONS);
            block[P0_I_PPC_MMIO_COMMIT_NS] =
                NanosPerOp(commitMicros, P0_MMIO_COMMIT_ITERS);
            block[P0_I_PPC_APER_NATIVE_KBPS] =
                KiloBytesPerSecond(P0_APER_BURST_BYTES, P0_APER_PASSES,
                                   nativeMicros);
            block[P0_I_PPC_APER_BR_KBPS] =
                KiloBytesPerSecond(P0_APER_BURST_BYTES, P0_APER_PASSES,
                                   brMicros);
            block[P0_I_PPC_APER_READ_NS] =
                NanosPerOp(aperReadMicros,
                           8UL * (P0_APER_BURST_BYTES / sizeof(ULONG)));
            block[P0_I_PPC_SELFREAD] = selfRead;
            block[P0_I_PPC_STWBRX_OK] = stwbrxOk;
            block[P0_I_PPC_MMIO_SCRATCH] = scratchValue;
            P0WriteBarrier();
            block[P0_I_PPC_ACK] = P0_PPC_ACK;
            P0WriteBarrier();
            (void)block[P0_I_PPC_ACK];
        }
    }

    printf("PPCPHASE0 status=%s stage=%lu "
           "mmio_read_ns=%lu mmio_write_ns=%lu mmio_commit_ns=%lu "
           "aper_native_kbps=%lu aper_br_kbps=%lu aper_read_ns=%lu "
           "selfread=%08lx stwbrx_ok=%lu scratch=%08lx\n",
           result ? "fail" : "ok",
           (unsigned long)stage,
           (unsigned long)(block ? block[P0_I_PPC_MMIO_READ_NS] : 0UL),
           (unsigned long)(block ? block[P0_I_PPC_MMIO_WRITE_NS] : 0UL),
           (unsigned long)(block ? block[P0_I_PPC_MMIO_COMMIT_NS] : 0UL),
           (unsigned long)(block ? block[P0_I_PPC_APER_NATIVE_KBPS] : 0UL),
           (unsigned long)(block ? block[P0_I_PPC_APER_BR_KBPS] : 0UL),
           (unsigned long)(block ? block[P0_I_PPC_APER_READ_NS] : 0UL),
           (unsigned long)selfRead,
           (unsigned long)stwbrxOk,
           (unsigned long)scratchValue);
    if (result)
        printf("PPCPHASE0 hint=usage: ppcphase0 <hexAddress> "
               "[--no-mmio-write]\n");
    return result;
}
