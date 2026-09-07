/* Built by test_cp_copy.py with generated, unmodified CP function extracts.
 * All destinations are malloc RAM, never VRAM. No Radeon/P96/GPU API calls.
 * Host checks native representations of SWAPLONG; 68k checks actual LE bytes.
 * Native defaults to bounded smoke coverage; CP_SMOKE tests that schedule on host.
 */
#include <exec/types.h>
#include <hardware/byteswap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__m68k__)
#include <dos/dos.h>
#include <proto/exec.h>
#define CP_SMOKE
#endif
#ifdef CP_HOST_SANITIZE
#include <sanitizer/asan_interface.h>
#endif
#include "radeon_regs.h"
#include "cp_constants.h"

typedef char CpConstantsCheck[
    sizeof(ULONG) == 4 && CP_RING_SIZE == 1048576UL &&
    CP_RING_DWORDS == 262144UL && CP_RING_MASK == 262143UL &&
    CP_RING_ALIGNMENT == 16UL && CP_FENCE_DWORDS == 6UL &&
    CP_CACHE_FLUSH_ALL == 15UL && RADEON3D_MAX_BATCH_DWORDS == 8192UL &&
    RADEON3D_MAX_BATCH_DWORDS + CP_FENCE_DWORDS + CP_RING_ALIGNMENT <
        CP_RING_DWORDS ? 1 : -1];

#define GUARD 64UL
#define CANARY 0xa5
#define CHECK(condition) do { if (!(condition)) Fail(#condition, __LINE__); } while (0)

/* Only the state fields used by the extracted submission functions are needed.
 * The two fence fields ensure the commit does not change its caller's state. */
struct RadeonCpState {
    APTR RingMemory;
    ULONG WritePointer, PendingFence, NextFence;
    UBYTE Ready;
};

struct BoardInfo {
    struct RadeonCpState *State;
    ULONG Start, Padded, Next;
    unsigned Events;
    BOOL ReserveFail, WriteFail;
};

static BOOL CpReserve(struct BoardInfo *bi, struct RadeonCpState *state,
                      ULONG dwords);
static BOOL RadeonWrite32(struct BoardInfo *bi, ULONG reg, ULONG value);
static ULONG RadeonRead32(struct BoardInfo *bi, ULONG reg);

#include "cp_current.h"
#include "cp_old.h"

struct Buffer {
    UBYTE *Memory, *Data;
    ULONG Bytes, Prefix, Total;
};

static struct Buffer Source, Ring;
static UBYTE *Expected;
static struct RadeonCpState State;
static struct BoardInfo Board;
static ULONG CaseCount, CaseStart, CaseSequence, CopyCases, CommitCases;
static ULONG FullAudits;
static BOOL CaseFence;
static const char *Phase;

static void Fail(const char *condition, unsigned line)
{
    printf("CPCHECK FAIL phase=%s line=%u count=%lu start=%lu fence=%d "
           "sequence=%08lx check=%s\n", Phase, line, (unsigned long)CaseCount,
           (unsigned long)CaseStart, (int)CaseFence,
           (unsigned long)CaseSequence, condition);
    exit(20);
}

static void BetweenCases(void)
{
#if defined(__m68k__)
    /* Source protection is lifted before polling; no GPU work is in flight. */
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
        printf("CPCHECK STOP Ctrl-C copies=%lu commits=%lu phase=%s\n",
               (unsigned long)CopyCases, (unsigned long)CommitCases, Phase);
        exit(RETURN_WARN);
    }
    if ((CopyCases + CommitCases) % 512UL == 0)
        printf("CPCHECK progress copies=%lu commits=%lu phase=%s\n",
               (unsigned long)CopyCases, (unsigned long)CommitCases, Phase);
#endif
}

static struct Buffer AllocBuffer(ULONG bytes, ULONG offset)
{
    struct Buffer buffer;

    buffer.Bytes = bytes;
    buffer.Prefix = GUARD + offset * sizeof(ULONG);
    buffer.Total = bytes + 2UL * GUARD + 8UL * sizeof(ULONG);
    buffer.Memory = malloc(buffer.Total);
    CHECK(buffer.Memory != NULL);
    buffer.Data = buffer.Memory + buffer.Prefix;
    memset(buffer.Memory, CANARY, buffer.Total);
    return buffer;
}

static void CheckGuards(const struct Buffer *buffer)
{
    ULONG i;

    for (i = 0; i < buffer->Prefix; ++i)
        CHECK(buffer->Memory[i] == CANARY);
    for (i = buffer->Prefix + buffer->Bytes; i < buffer->Total; ++i)
        CHECK(buffer->Memory[i] == CANARY);
}

static void ProtectSource(struct Buffer *buffer, ULONG bytes, BOOL protect)
{
#ifdef CP_HOST_SANITIZE
    if (protect) {
        __asan_poison_memory_region(buffer->Memory, buffer->Prefix);
        __asan_poison_memory_region(buffer->Data + bytes,
                                   buffer->Total - buffer->Prefix - bytes);
    } else {
        __asan_unpoison_memory_region(buffer->Memory, buffer->Total);
    }
#else
    (void)buffer;
    (void)bytes;
    (void)protect;
#endif
}

static ULONG Pattern(ULONG i)
{
    static const ULONG values[8] = {
        0x01234567UL, 0x89abcdefUL, 0x00010203UL, 0x10203040UL,
        0x80000001UL, 0xff00aa55UL, 0x13579bdfUL, 0x2468ace0UL
    };

    return values[i % 8UL] ^ (ULONG)((i / 8UL) * 0x9e3779b9UL);
}

/* Deliberately independent of SWAPLONG and the MOVEM register ordering. */
static void ReferenceBytes(UBYTE *dst, ULONG value)
{
    unsigned b;
#if !defined(__m68k__)
    const ULONG endian = 1;
    const BOOL little = *(const UBYTE *)&endian;
#endif

    for (b = 0; b < 4; ++b) {
#if defined(__m68k__)
        dst[b] = (UBYTE)(value >> (8U * b));
#else
        dst[b] = (UBYTE)(value >> (8U * (little ? 3U - b : b)));
#endif
    }
}

static ULONG ReferenceDword(const ULONG *commands, ULONG count, ULONG index,
                            BOOL fence, ULONG sequence)
{
    /* Literal packet encodings make this independent of CpStreamDword. */
    static const ULONG tail[5] = {
        0x000005c5UL, 0x0000000fUL, 0x000005c8UL, 0x00070200UL, 0x00000578UL
    };

    if (index < count)
        return commands[index];
    index -= count;
    if (fence && index < 5UL)
        return tail[index];
    if (fence && index == 5UL)
        return sequence;
    return 0x80000000UL;
}

static void CheckBytes(const UBYTE *got, const UBYTE *expected, ULONG bytes)
{
    ULONG i;

    if (memcmp(got, expected, bytes) == 0)
        return;
    for (i = 0; i < bytes; ++i) {
        if (got[i] != expected[i]) {
            printf("CPCHECK byte=%lu address=%p got=%02x expected=%02x\n",
                   (unsigned long)i, (const void *)(got + i),
                   (unsigned)got[i], (unsigned)expected[i]);
            Fail("byte stream mismatch", __LINE__);
        }
    }
}

static void CheckSpan(ULONG start, ULONG words)
{
    ULONG first = CP_RING_DWORDS - start;

    if (first > words)
        first = words;
    CheckBytes(Ring.Data + 4UL * start, Expected + 4UL * start, 4UL * first);
    CheckBytes(Ring.Data, Expected, 4UL * (words - first));
}

static void AuditBuffers(BOOL fullRing)
{
    ULONG i;

    if (fullRing) {
        CheckBytes(Ring.Data, Expected, Ring.Bytes);
        ++FullAudits;
    }
    for (i = 0; i < RADEON3D_MAX_BATCH_DWORDS; ++i)
        CHECK(((ULONG *)Source.Data)[i] == Pattern(i));
}

static BOOL CpReserve(struct BoardInfo *bi, struct RadeonCpState *state,
                      ULONG dwords)
{
    CHECK(bi == &Board && state == &State);
    CHECK(bi->Events++ == 0 && state->WritePointer == bi->Start);
    CHECK(dwords == bi->Padded && dwords < CP_RING_DWORDS);
    return !bi->ReserveFail;
}

static BOOL RadeonWrite32(struct BoardInfo *bi, ULONG reg, ULONG value)
{
    CHECK(bi == &Board && bi->Events++ == 1 && !bi->ReserveFail);
    CHECK(reg == RADEON_CP_RB_WPTR && value == bi->Next);
    CHECK(bi->State->WritePointer == bi->Start);
    /* Every command/fence/padding byte must be visible at publication time. */
    CheckSpan(bi->Start, bi->Padded);
    return !bi->WriteFail;
}

static ULONG RadeonRead32(struct BoardInfo *bi, ULONG reg)
{
    CHECK(bi == &Board && bi->Events++ == 2 && !bi->WriteFail);
    CHECK(reg == RADEON_CP_RB_WPTR && bi->State->WritePointer == bi->Start);
    /* Existing code uses the read for ordering, not value validation. */
    return bi->Next ^ 0xdeadbeefUL;
}

static void TestCopy(ULONG count)
{
    typedef void (*CopyFunction)(volatile ULONG *, const ULONG *, ULONG);
    CopyFunction volatile functions[2] = {CpBurstCopySwapped, OldBurstCopySwapped};
    ULONG s, d, i;
    unsigned kernel;
    UBYTE *reference = malloc(count * sizeof(ULONG) + 1UL);

    CHECK(reference != NULL);
    CaseCount = count;
    for (i = 0; i < count; ++i)
        ReferenceBytes(reference + 4UL * i, Pattern(i));
    for (s = 0; s < 8; ++s) {
        struct Buffer src = AllocBuffer(count * sizeof(ULONG), s);
        ULONG *words = (ULONG *)src.Data;

        for (i = 0; i < count; ++i)
            words[i] = Pattern(i);
        for (d = 0; d < 8; ++d) {
            struct Buffer dst = AllocBuffer(count * sizeof(ULONG), d);

            for (kernel = 0; kernel < 2; ++kernel) {
                Phase = kernel ? "old-copy" : "fused-copy";
                memset(dst.Data, CANARY, dst.Bytes);
                ProtectSource(&src, src.Bytes, TRUE);
                functions[kernel]((volatile ULONG *)dst.Data, words, count);
                ProtectSource(&src, src.Bytes, FALSE);
                CheckBytes(dst.Data, reference, dst.Bytes);
                CheckGuards(&src);
                CheckGuards(&dst);
                for (i = 0; i < count; ++i)
                    CHECK(words[i] == Pattern(i));
                ++CopyCases;
                BetweenCases();
            }
            free(dst.Memory);
        }
        free(src.Memory);
    }
    free(reference);
}

enum Fault { NONE, ZERO, OVERSIZED, NULL_COMMANDS, NULL_STATE, NOT_READY,
             RESERVE_FAIL, WPTR_FAIL, MAX_COUNT };

static void TestCommit(ULONG count, ULONG start, BOOL fence, ULONG sequence,
                       enum Fault fault, BOOL fullCheck)
{
    ULONG *commands = (ULONG *)Source.Data;
    struct RadeonCpState *state = &State;
    struct RadeonCpState saved;
    ULONG i, bytes;
    BOOL valid, result;

    if (fault == ZERO)
        count = 0;
    if (fault == OVERSIZED)
        count = RADEON3D_MAX_BATCH_DWORDS + 1UL;
    if (fault == MAX_COUNT)
        count = 0xffffffffUL;
    if (fault == NULL_COMMANDS)
        commands = NULL;
    if (fault == NULL_STATE)
        state = NULL;
    CaseCount = count;
    CaseStart = start;
    CaseFence = fence;
    CaseSequence = sequence;
    State.Ready = fault != NOT_READY;
    State.WritePointer = start;
    saved = State;
    memset(&Board, 0, sizeof(Board));
    Board.State = &State;
    Board.Start = start;
    Board.ReserveFail = fault == RESERVE_FAIL;
    Board.WriteFail = fault == WPTR_FAIL;
    valid = state && State.Ready && commands && count &&
            count <= RADEON3D_MAX_BATCH_DWORDS;
    if (valid) {
        ULONG stream = count + (fence ? 6UL : 0UL);

        Board.Padded = ((stream + 15UL) / 16UL) * 16UL;
        Board.Next = (start + Board.Padded) % CP_RING_DWORDS;
        for (i = 0; i < Board.Padded + CP_RING_ALIGNMENT; ++i) {
            ULONG word = ReferenceDword(commands, count, i, fence, sequence);

            CHECK(CpStreamDword(commands, count, i, fence, sequence) == word);
            if (!Board.ReserveFail && i < Board.Padded)
                ReferenceBytes(Expected + 4UL * ((start + i) % CP_RING_DWORDS), word);
        }
    }
    bytes = valid ? count * sizeof(ULONG) : 0;
    ProtectSource(&Source, bytes, TRUE);
    result = CpCommitStream(&Board, state, commands, count, fence, sequence);
    ProtectSource(&Source, bytes, FALSE);
    CHECK(result == (valid && !Board.ReserveFail && !Board.WriteFail));
    CHECK(Board.Events == (!valid ? 0U : Board.ReserveFail ? 1U :
                           Board.WriteFail ? 2U : 3U));
    if (result)
        saved.WritePointer = Board.Next;
    CHECK(memcmp(&State, &saved, sizeof(State)) == 0);
    CheckGuards(&Source);
    CheckGuards(&Ring);
#ifdef CP_SMOKE
    fullCheck = (CommitCases + 1UL) % 128UL == 0;
#endif
    if (!fullCheck)
        CheckSpan((start + CP_RING_DWORDS - 8UL) % CP_RING_DWORDS,
                  Board.Padded + 16UL);
#ifdef CP_SMOKE
    if (fullCheck)
        AuditBuffers(TRUE);
#else
    AuditBuffers(fullCheck);
#endif
    ++CommitCases;
    BetweenCases();
}

int main(void)
{
    static const ULONG lengths[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
        78, 269, 419, 8192
    };
    static const ULONG sequences[] = {0, 1, 0x01234567UL, 0xffffffffUL};
#ifdef CP_SMOKE
    const ULONG sequenceCount = 128UL;
    const char *profile = "smoke";
#else
    const ULONG sequenceCount = 4096UL;
    const char *profile = "exhaustive";
#endif
    ULONG i, j, count, distance, sequence, wraps = 0;
    ULONG rollovers = 0;
    unsigned fence, fault;
    ULONG stackMarker = 0x01234567UL;
    const char *mode;

    Phase = "setup";
#if defined(__m68k__)
    mode = "m68k-asm-LE-bytes";
    CHECK(*(UBYTE *)&stackMarker == 0x01);
#else
    mode = "host-C-fallback-native-swapped-bytes";
#endif
    Source = AllocBuffer(RADEON3D_MAX_BATCH_DWORDS * sizeof(ULONG), 1);
    Ring = AllocBuffer(CP_RING_SIZE, 0);
    Expected = malloc(CP_RING_SIZE);
    CHECK(Expected != NULL);
    memset(Expected, CANARY, CP_RING_SIZE);
    memset(&State, 0, sizeof(State));
    State.RingMemory = Ring.Data;
    State.PendingFence = 0x98abcdefUL;
    State.NextFence = 0x76543210UL;
    for (i = 0; i < RADEON3D_MAX_BATCH_DWORDS; ++i)
        ((ULONG *)Source.Data)[i] = Pattern(i);
    printf("CPCHECK mode=%s profile=%s ring=%lu max=%lu source=%p "
           "destination=%p stack_local=%p\n",
           mode, profile, (unsigned long)CP_RING_DWORDS,
           (unsigned long)RADEON3D_MAX_BATCH_DWORDS, (void *)Source.Data,
           (void *)Ring.Data, (void *)&stackMarker);
#if defined(__m68k__)
    printf("CPCHECK code=%p TypeOfMem source=%08lx destination=%08lx stack=%08lx\n",
           (void *)CpBurstCopySwapped, (unsigned long)TypeOfMem(Source.Data),
           (unsigned long)TypeOfMem(Ring.Data), (unsigned long)TypeOfMem(&stackMarker));
#endif
    CpBurstCopySwapped((volatile ULONG *)Ring.Data, (ULONG *)Source.Data, 8);
    for (i = 0; i < 8; ++i)
        ReferenceBytes(Expected + 4UL * i, Pattern(i));
    CheckBytes(Ring.Data, Expected, 32);
    printf("CPCHECK first-burst-bytes=");
    for (i = 0; i < 32; ++i)
        printf("%02x", (unsigned)Ring.Data[i]);
    printf("\n");
    memset(Ring.Data, CANARY, Ring.Bytes);
    memset(Expected, CANARY, CP_RING_SIZE);
    TestCopy(0);
    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i)
        TestCopy(lengths[i]);
    CHECK(CopyCases == 2816UL);
    printf("CPCHECK copy cases=%lu passed (old/new, 8x8 source/dest offsets)\n",
           (unsigned long)CopyCases);

    Phase = "matrix";
    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        count = lengths[i];
        for (fence = 0; fence < 2; ++fence) {
            for (j = 0; j < 4; ++j)
                TestCommit(count, j * 7UL, fence, sequences[j], NONE, TRUE);
            /* Wrap inside either burst, scalar remainder, each fence word,
             * and padding, including an exactly ending command/stream. */
            for (distance = 1; distance <= 17; ++distance)
                TestCommit(count, CP_RING_DWORDS - distance, fence,
                           0x89abcdefUL, NONE, TRUE);
            for (distance = count; distance <= count + 21UL; ++distance)
                TestCommit(count, CP_RING_DWORDS - distance, fence,
                           0xfedcba98UL, NONE, TRUE);
        }
    }
    Phase = "failures";
    for (fence = 0; fence < 2; ++fence) {
        for (fault = ZERO; fault <= MAX_COUNT; ++fault) {
            TestCommit(17, 0, fence, 0x01234567UL, fault, TRUE);
            TestCommit(8192, CP_RING_DWORDS - 7UL, fence,
                       0x89abcdefUL, fault, TRUE);
        }
    }
    CHECK(CommitCases == 1838UL);
    printf("CPCHECK matrix/failures cases=%lu passed\n", (unsigned long)CommitCases);

    Phase = "sequence";
#ifdef CP_SMOKE
    State.WritePointer = 0;
#else
    State.WritePointer = CP_RING_DWORDS - CP_RING_ALIGNMENT;
#endif
    for (i = 0; i < sequenceCount; ++i) {
        ULONG before = State.WritePointer;

#ifdef CP_SMOKE
        /* 128 * 8208 dwords crosses four real rings; every sequence is fenced,
         * including both sides of the UINT32 rollover at submission 64. */
        count = RADEON3D_MAX_BATCH_DWORDS;
        sequence = (ULONG)(0xffffffc0UL + i);
        fence = TRUE;
#else
        count = lengths[i % (sizeof(lengths) / sizeof(lengths[0]))];
        sequence = (ULONG)(0xfffff800UL + i);
        fence = i & 1UL;
#endif
        if (i && !sequence)
            ++rollovers;
        TestCommit(count, before, fence, sequence, NONE, (i % 64UL) == 0);
        if (State.WritePointer < before)
            ++wraps;
    }
    AuditBuffers(TRUE);
    CHECK(CommitCases == 1838UL + sequenceCount);
    CHECK(wraps >= 3 && rollovers == 1);
#ifdef CP_SMOKE
    CHECK(wraps == 4 && State.WritePointer == 2048UL && FullAudits == 16UL);
#endif
    printf("CPCHECK PASS copies=%lu commits=%lu sequence=%lu wraps=%lu "
           "rollovers=%lu full_audits=%lu mode=%s profile=%s\n",
           (unsigned long)CopyCases, (unsigned long)CommitCases,
           (unsigned long)sequenceCount, (unsigned long)wraps,
           (unsigned long)rollovers, (unsigned long)FullAudits, mode, profile);
    free(Expected);
    free(Ring.Memory);
    free(Source.Memory);
    return 0;
}
