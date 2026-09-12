#!/usr/bin/env python3
"""ASan/UBSan host checks of production indirect dispatch, never hardware.

Run with python3 tools/test_indirect_dispatch.py. Extract the C functions,
request/session structures and constants without rewriting their bodies; mock
only the OS, board, transition, MMIO, submission and timing boundaries. ULONG
stays 32-bit even on a 64-bit host, including the GPU-address overflow check.
"""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def block(source, pattern):
    match = re.search(pattern, source, re.M)
    if not match:
        raise ValueError("Cannot extract " + pattern)
    opening = source.index("{", match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def define(source, name):
    match = re.search(r"^#define " + re.escape(name) + r"(?=[\s(])", source, re.M)
    if not match:
        raise ValueError("Cannot extract " + name)
    end = source.index("\n", match.start())
    while source[end - 1] == "\\":
        end = source.index("\n", end + 1)
    return source[match.start():end]


class IndirectDispatchTests(unittest.TestCase):
    def test_dispatch_transitions_and_capabilities(self):
        source = (ROOT / "src/radeon3d_service.c").read_text()
        public = (ROOT / "include/radeon3d.h").read_text()
        internal = (ROOT / "src/radeon9200.h").read_text()
        registers = (ROOT / "src/radeon_regs.h").read_text()
        constants = "\n".join(define(public, name) for name in (
            *re.findall(r"^#define (RADEON3D_CAP_\w+)", public, re.M),
            "RADEON3D_IFACE_VERSION", "RADEON3D_MAX_SEGMENTS",
            "RADEON3D_MAX_BATCH_DWORDS", "RADEON3D_INDIRECT_VERSION",
            "RADEON3D_INDIRECT_V1_SIZE", "RADEON3D_INFO_V1_SIZE",
            "RADEON3D_INFO_V2_SIZE", "RADEON3D_INFO_V3_SIZE",
            "RADEON3D_INFO_V4_SIZE", "RADEON3D_SAMPLE_RING_SIZE",
            "RADEON3D_SAMPLE_DISPATCH",
        ))
        constants += "\n" + "\n".join(define(registers, name) for name in (
            "RADEON_CP_PACKET0", "RADEON_CP_IB_BASE", "RADEON_CP_CSQ_MODE",
            "CP_CSQ_CACHE_PARTITION", "RADEON_CP_CSQ_STAT",
            "RADEON_DP_DATATYPE", "RADEON_HOST_BIG_ENDIAN_EN",
        ))
        constants += "\n" + define(internal, "RADEON3D_SERVICE_READY")
        constants += "\n" + define(source, "RADEON3D_SESSION_MAGIC")
        constants += "\n" + define(source, "COMMIT_FAIL")
        structures = "\n".join(block(text, r"^struct " + name + r" \{") + ";"
                               for text, name in (
            (public, "Radeon3DIndirect"), (public, "Radeon3DInfo"),
            (source, "Radeon3DSegmentSlot"), (source, "Radeon3DDevice"),
        ))
        usable = block(source, r"^static BOOL IsUsableDevice\(")
        implementation = "\n".join(block(source, pattern) for pattern in (
            r"^static void FillInfo\(", r"^BOOL Radeon3DDispatchIndirect\(",
        ))
        harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t ULONG;
typedef uint8_t UBYTE;
typedef int BOOL;
typedef void *APTR;
#define TRUE 1
#define FALSE 0
#define __REGA0(x) x
#define __REGA1(x) x
#define __REGA2(x) x
#define __REGA6(x) x
struct ExecBase { int unused; };
struct SignalSemaphore { int unused; };
struct MinNode { struct MinNode *mln_Succ, *mln_Pred; };
struct MinList { struct MinNode *mlh_Head, *mlh_Tail, *mlh_TailPred; };
struct BoardInfo {
    struct SignalSemaphore BoardLock;
    ULONG MemorySize;
    BOOL Ready;
};
struct RadeonBoardData { ULONG DeviceId, InstalledVram; };
struct RadeonChipBase {
    struct ExecBase *ExecBase;
    struct BoardInfo *BoardInfo;
    struct SignalSemaphore ServiceLock;
    ULONG ServiceGeneration, ServiceState;
    APTR StreamSegmentPool, AuxSurfacePool, ExecSampleRing;
    ULONG ExecCalls, ExecRecordDwords, ExecGeneratedDwords;
    ULONG ExecCopyMicros, ExecBuildMicros, ExecSubmitMicros;
    ULONG CommitFailStage, ExecSampleSeq, ExecClockHz;
};
""" + constants + "\n" + structures + r"""
_Static_assert(sizeof(ULONG) == 4, "Amiga ULONG width");
_Static_assert(sizeof(struct Radeon3DIndirect) == RADEON3D_INDIRECT_V1_SIZE,
               "indirect request layout");
_Static_assert(RADEON3D_IFACE_VERSION == 18, "interface remains 18");
_Static_assert(RADEON3D_CAP_INDIRECT_DISPATCH == (1UL << 26), "dispatch bit");
_Static_assert(RADEON3D_CAP_INDIRECT_RENDER == (1UL << 27), "render bit");
static struct ExecBase exec;
static struct BoardInfo board, other_board;
static struct RadeonBoardData data;
static struct RadeonChipBase chip;
static struct Radeon3DDevice session, handle;
static struct Radeon3DIndirect request, expected_request;
static struct RadeonChipBase *base_arg;
static struct Radeon3DDevice *device_arg;
static const struct Radeon3DIndirect *request_arg;
static int pool, segment_memory;
static BOOL active, lock_ok, prepare_ok, submit_ok, recover_ok, output_fence;
static BOOL board_locked, service_locked, prepared, restore_setup;
static BOOL mutate_request;
static ULONG submit_fence, csq_mode, datatype, ticks;
static unsigned submits, marks, recoveries, samples, cases, event_count;
static char events[128];
enum Mutation {
    NONE, GENERATION, BOTH_GENERATIONS, MAGIC, DEVICE_BASE, DEVICE_GENERATION,
    INACTIVE, SERVICE_STATE, BOARD, CP_UNREADY, POOL, LEASE_RELEASED,
    LEASE_CPU, LEASE_GPU, LEASE_BYTES, MUTATION_END
};
static enum Mutation mutation;

static void Event(char event)
{
    assert(event_count + 1 < sizeof(events));
    events[event_count++] = event;
    events[event_count] = 0;
}
static BOOL RadeonCpIsReady(struct BoardInfo *bi)
{
    if (prepared)
        assert(board_locked && service_locked);
    return bi && bi->Ready;
}
""" + usable + r"""
static struct RadeonBoardData *RadeonGetBoardData(struct BoardInfo *bi)
{
    return bi ? &data : NULL;
}
static struct BoardInfo *LockServiceBoard(struct RadeonChipBase *base,
                                         struct Radeon3DDevice **device)
{
    assert(base == &chip && !board_locked && !service_locked);
    Event('L');
    if (!base->ExecBase || *device != &handle || !active || !lock_ok ||
        !IsUsableDevice(base, &session))
        return NULL;
    *device = &session;
    board_locked = TRUE;
    return base->BoardInfo;
}
static void UnlockServiceBoard(struct RadeonChipBase *base,
                                struct BoardInfo *bi,
                                struct Radeon3DDevice *device)
{
    assert(base == &chip && bi == &board && device == &session);
    assert(board_locked && !service_locked);
    Event('U');
    board_locked = FALSE;
}
static void Obtain(struct ExecBase *sys, struct SignalSemaphore *lock)
{
    assert(sys == &exec && lock == &chip.ServiceLock);
    assert(board_locked && !service_locked);
    Event('[');
    service_locked = TRUE;
}
static void Release(struct ExecBase *sys, struct SignalSemaphore *lock)
{
    assert(sys == &exec && lock == &chip.ServiceLock);
    assert(board_locked && service_locked);
    Event(']');
    service_locked = FALSE;
}
#define ObtainSemaphore(lock) Obtain(SysBase, lock)
#define ReleaseSemaphore(lock) Release(SysBase, lock)
static struct Radeon3DDevice *FindActiveDevice(struct RadeonChipBase *base,
                                              struct Radeon3DDevice *device)
{
    assert(base == &chip && device == &session);
    assert(board_locked && service_locked && prepared);
    Event('V');
    return active ? device : NULL;
}
static BOOL RadeonPrepare3D(struct BoardInfo *bi)
{
    struct Radeon3DSegmentSlot *slot = &session.Segments[expected_request.SegmentId];
    assert(bi == &board && board_locked && !service_locked && !prepared);
    Event('P');
    prepared = TRUE;
    /* Model the engine restore that would undo setup done before Prepare. */
    if (restore_setup) {
        csq_mode = 0;
        datatype = RADEON_HOST_BIG_ENDIAN_EN | 0x1234UL;
    }
    switch (mutation) {
    case NONE: break;
    case GENERATION: ++chip.ServiceGeneration; break;
    case BOTH_GENERATIONS: ++chip.ServiceGeneration; ++session.Generation; break;
    case MAGIC: session.Magic = 0; break;
    case DEVICE_BASE: session.Base = NULL; break;
    case DEVICE_GENERATION: ++session.Generation; break;
    case INACTIVE: active = FALSE; break;
    case SERVICE_STATE: chip.ServiceState = 0; break;
    case BOARD: chip.BoardInfo = &other_board; break;
    case CP_UNREADY: board.Ready = FALSE; break;
    case POOL: chip.StreamSegmentPool = NULL; break;
    case LEASE_RELEASED: slot->Allocated = FALSE; break;
    case LEASE_CPU: slot->CpuAddress = NULL; break;
    case LEASE_GPU: slot->GpuAddress += 16; break;
    case LEASE_BYTES: slot->Bytes += 16; break;
    default: assert(0);
    }
    if (mutate_request) {
        request.SegmentId = UINT32_MAX;
        request.ByteOffset = 0xfffffff0UL;
        request.DwordCount = UINT32_MAX;
    }
    return prepare_ok;
}
static ULONG RadeonRead32(struct BoardInfo *bi, ULONG reg)
{
    assert(bi == &board && board_locked && !service_locked && prepared);
    switch (reg) {
    case RADEON_CP_CSQ_MODE: Event('c'); return csq_mode;
    case RADEON_DP_DATATYPE: Event('d'); return datatype;
    case RADEON_CP_CSQ_STAT: Event('q'); return 0x5a000000UL;
    default: assert(0); return 0;
    }
}
static BOOL RadeonWrite32(struct BoardInfo *bi, ULONG reg, ULONG value)
{
    assert(bi == &board && board_locked && !service_locked && prepared);
    switch (reg) {
    case RADEON_CP_CSQ_MODE:
        assert(value == CP_CSQ_CACHE_PARTITION);
        Event('C'); csq_mode = value; break;
    case RADEON_DP_DATATYPE:
        assert(value == (datatype & ~RADEON_HOST_BIG_ENDIAN_EN));
        Event('D'); datatype = value; break;
    default: assert(0);
    }
    return TRUE;
}
static ULONG ServiceExecTicks(struct RadeonChipBase *base)
{
    assert(base == &chip && board_locked && !service_locked && prepared);
    Event('t');
    return ++ticks;
}
static BOOL RadeonCpSubmitStream(struct BoardInfo *bi, const ULONG *commands,
                                 ULONG count, BOOL fence_needed, ULONG *fence)
{
    const struct Radeon3DSegmentSlot *slot =
        &session.Segments[expected_request.SegmentId];

    assert(bi == &board && board_locked && !service_locked && prepared);
    assert(csq_mode == CP_CSQ_CACHE_PARTITION);
    assert(!(datatype & RADEON_HOST_BIG_ENDIAN_EN));
    assert(count == 3 && fence_needed && *fence == 0 && submits == 0);
    assert(commands[0] == RADEON_CP_PACKET0(RADEON_CP_IB_BASE, 1));
    assert(commands[1] == slot->GpuAddress + expected_request.ByteOffset);
    assert(commands[2] == expected_request.DwordCount && session.LastFence == 23);
    assert(!(commands[2] & 1UL));
    assert(expected_request.ByteOffset < slot->Bytes);
    assert(commands[2] <= (slot->Bytes - expected_request.ByteOffset) / sizeof(ULONG));
    Event('S');
    ++submits;
    *fence = submit_fence;
    return submit_ok;
}
static void RadeonMark3DSubmitted(struct BoardInfo *bi)
{
    assert(bi == &board && board_locked && !service_locked);
    assert(submits == 1 && submit_ok && submit_fence && !marks && !recoveries);
    assert(session.LastFence == submit_fence);
    Event('M');
    ++marks;
}
static BOOL RadeonRecoverAcceleration(struct BoardInfo *bi)
{
    assert(bi == &board && board_locked && !service_locked);
    assert(submits == 1 && !marks && !recoveries && session.LastFence == 23);
    Event('R');
    ++recoveries;
    ++chip.ServiceGeneration;
    board.Ready = recover_ok;
    return recover_ok;
}
static void RecordExecSample(struct RadeonChipBase *base, ULONG type,
                              ULONG records, ULONG generated, ULONG copy,
                              ULONG build, ULONG submit, BOOL ok)
{
    assert(base == &chip && board_locked && !service_locked);
    assert(type == RADEON3D_SAMPLE_DISPATCH && records == expected_request.DwordCount);
    assert(generated == 3 && !copy && !build && submit == 1 && !samples);
    assert(ok == (submit_ok && submit_fence != 0));
    Event('N');
    ++samples;
}
""" + implementation + r"""
static void Reset(void)
{
    memset(&chip, 0, sizeof(chip));
    memset(&session, 0, sizeof(session));
    chip.ExecBase = &exec;
    chip.BoardInfo = &board;
    chip.ServiceGeneration = 9;
    chip.ServiceState = RADEON3D_SERVICE_READY;
    chip.StreamSegmentPool = &pool;
    session.Magic = RADEON3D_SESSION_MAGIC;
    session.Generation = chip.ServiceGeneration;
    session.InterfaceVersion = 18;
    session.Base = &chip;
    session.Handle = &handle;
    session.LastFence = 23;
    session.Segments[0] = (struct Radeon3DSegmentSlot){
        &segment_memory, 0x01000000UL, 256, TRUE
    };
    request = (struct Radeon3DIndirect){
        RADEON3D_INDIRECT_V1_SIZE, RADEON3D_INDIRECT_VERSION, 0, 16, 4, 0
    };
    base_arg = &chip; device_arg = &handle; request_arg = &request;
    active = lock_ok = prepare_ok = submit_ok = recover_ok = output_fence = TRUE;
    board.Ready = other_board.Ready = TRUE;
    board_locked = service_locked = prepared = FALSE;
    mutate_request = FALSE;
    restore_setup = TRUE;
    mutation = NONE;
    csq_mode = CP_CSQ_CACHE_PARTITION;
    datatype = 0x1234UL;
    submit_fence = 0x12345UL;
    submits = marks = recoveries = samples = event_count = ticks = 0;
    events[0] = 0;
}
static void Check(ULONG stage, const char *expected)
{
    ULONG fence = 0xdeadbeefUL;
    BOOL ok;

    expected_request = request;
    ok = Radeon3DDispatchIndirect(device_arg, request_arg,
                                  output_fence ? &fence : NULL, base_arg);
    if (ok != (stage == 0) || strcmp(events, expected))
        fprintf(stderr, "case %u: stage %u, result %d, events %s expected %s\n",
                cases + 1, stage, ok, events, expected);
    assert(ok == (stage == 0) && !strcmp(events, expected));
    assert(!board_locked && !service_locked);
    assert(session.LastFence == (ok ? submit_fence : 23));
    assert(marks == (unsigned)ok);
    assert(recoveries == (stage == 106));
    assert(submits == (unsigned)(ok || stage == 106) && samples == submits);
    if (output_fence)
        assert(fence == (ok ? submit_fence : (0x80000000UL | stage)));
    if (base_arg) {
        if (submits)
            assert(chip.CommitFailStage == (0xc510005aUL |
                                           (restore_setup ? 0x100UL : 0)));
        else
            assert(chip.CommitFailStage == stage);
    }
    ++cases;
}
int main(void)
{
    unsigned version, pool_live, cp_ready, index;
    ULONG *short_request;
    struct Radeon3DInfo info;
    const ULONG indirect_caps = RADEON3D_CAP_INDIRECT_DISPATCH |
                                RADEON3D_CAP_INDIRECT_RENDER;
    const char *success = "LP[V]cCdDqtStMNU";
    const char *failure = "LP[V]cCdDqtStRNU";

    for (version = 16; version <= 19; ++version)
        for (pool_live = 0; pool_live <= 1; ++pool_live)
            for (cp_ready = 0; cp_ready <= 1; ++cp_ready) {
                Reset();
                chip.StreamSegmentPool = pool_live ? &pool : NULL;
                board.Ready = cp_ready;
                memset(&info, 0, sizeof(info));
                info.Size = RADEON3D_INFO_V4_SIZE;
                FillInfo(&chip, &info, version);
                assert((info.Caps & indirect_caps) ==
                       (version >= 18 && pool_live ? indirect_caps : 0));
                assert(!!(info.Caps & RADEON3D_CAP_CP_READY) == cp_ready);
                assert(info.Version == version && info.Generation == 9);
                ++cases;
            }

    /* Rejected requests must not prepare, touch MMIO or submit. */
    Reset(); base_arg = NULL; Check(101, "");
    Reset(); request_arg = NULL; Check(101, "");
    Reset(); request.Size--; Check(101, "");
    Reset();
    short_request = malloc(sizeof(*short_request));
    assert(short_request);
    *short_request = sizeof(*short_request);
    request_arg = (const struct Radeon3DIndirect *)short_request;
    Check(101, ""); /* Reject before copying beyond the readable Size field. */
    free(short_request);
    Reset(); request.Version++; Check(101, "");
    Reset(); request.Flags = 1; Check(101, "");
    Reset(); request.DwordCount = 0; Check(101, "");
    Reset(); request.DwordCount = 1; Check(101, "");
    Reset(); request.DwordCount = 3; Check(101, "");
    Reset(); request.ByteOffset = 0;
    request.DwordCount = RADEON3D_MAX_BATCH_DWORDS - 1; /* 8191, within lease. */
    session.Segments[0].Bytes = RADEON3D_MAX_BATCH_DWORDS * sizeof(ULONG);
    Check(101, "");
    Reset(); request.DwordCount = RADEON3D_MAX_BATCH_DWORDS + 2; Check(101, "");
    Reset(); request.ByteOffset = 1; Check(101, "");
    Reset(); request.ByteOffset = UINT32_MAX; Check(101, "");
    Reset(); device_arg = NULL; Check(102, "L");
    Reset(); chip.ExecBase = NULL; Check(102, "L");
    Reset(); lock_ok = FALSE; Check(102, "L");
    Reset(); session.Generation++; Check(102, "L");
    Reset(); board.Ready = FALSE; Check(102, "L");
    Reset(); session.InterfaceVersion = 17; Check(103, "LU");
    Reset(); chip.StreamSegmentPool = NULL; Check(103, "LU");
    Reset(); request.SegmentId = RADEON3D_MAX_SEGMENTS; Check(103, "LU");
    Reset(); request.SegmentId = UINT32_MAX; Check(103, "LU");
    Reset(); session.Segments[0].Allocated = FALSE; Check(103, "LU");
    Reset(); session.Segments[0].Bytes = 0; Check(104, "LU");
    Reset(); request.ByteOffset = 256; Check(104, "LU");
    Reset(); request.ByteOffset = 0xfffffff0UL; Check(104, "LU");
    Reset(); request.DwordCount = 62; Check(104, "LU");
    Reset(); session.Segments[0].Bytes = 17; Check(104, "LU");
    Reset(); session.Segments[0].GpuAddress = 0xfffffff0UL; Check(105, "LU");

    Reset(); prepare_ok = FALSE; Check(107, "LPU");
    Reset(); prepare_ok = FALSE; mutation = GENERATION; Check(107, "LPU");
    for (mutation = GENERATION; mutation < MUTATION_END; ) {
        enum Mutation current = mutation;
        Reset();
        mutation = current;
        Check(108, "LP[V]U");
        mutation = current + 1;
    }

    for (index = 0; index < 4; ++index) {
        Reset();
        submit_ok = FALSE;
        submit_fence = (index & 1) ? 0x12345UL : 0;
        recover_ok = (index & 2) != 0;
        Check(106, failure);
    }
    Reset(); submit_fence = 0; Check(106, failure);
    Reset(); submit_fence = 0; recover_ok = FALSE; Check(106, failure);

    Reset(); Check(0, success); /* ID 0 is a valid live lease. */
    Reset(); output_fence = FALSE; Check(0, success);
    Reset(); restore_setup = FALSE; Check(0, "LP[V]cdqtStMNU");
    Reset(); request.ByteOffset = 0; Check(0, success);
    Reset(); request.ByteOffset = 240; Check(0, success); /* Exact lease end. */
    Reset(); request.DwordCount = 2; Check(0, success);
    Reset(); request.Size += 4; Check(0, success);
    Reset(); request.ByteOffset = 0; request.DwordCount = RADEON3D_MAX_BATCH_DWORDS;
    session.Segments[0].Bytes = request.DwordCount * sizeof(ULONG);
    Check(0, success);
    Reset(); request.SegmentId = RADEON3D_MAX_SEGMENTS - 1;
    session.Segments[request.SegmentId] = session.Segments[0];
    session.Segments[0].Allocated = FALSE;
    Check(0, success);

    /* Prepare must not let caller edits widen or redirect the validated IB,
     * including the recorded dword count on both successful and failed submits. */
    Reset(); mutate_request = TRUE; Check(0, success);
    Reset(); request.ByteOffset = 240; mutate_request = TRUE; Check(0, success);
    Reset(); mutate_request = TRUE; submit_ok = FALSE; Check(106, failure);
    printf("indirect dispatch: %u cases passed (production functions, no hardware)\n",
           cases);
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="r3d-indirect-", dir="/tmp/opencode") as directory:
            path = Path(directory)
            (path / "check.c").write_text(harness)
            subprocess.run([
                "cc", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                str(path / "check.c"), "-o", str(path / "check"),
            ], check=True)
            subprocess.run([str(path / "check")], check=True, env=dict(
                os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
            ))


if __name__ == "__main__":
    unittest.main()
