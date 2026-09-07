#!/usr/bin/env python3
"""Host checks for the actual private execute-buffer allocation paths."""

import pathlib
import subprocess
import tempfile
import unittest


SOURCE = pathlib.Path(__file__).resolve().parents[1] / "src/radeon3d_service.c"


def function(source, name):
    start = source.index("static ", source.index(name) - 32)
    opening = source.index("{", source.index(name, start))
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class ExecuteMemoryTests(unittest.TestCase):
    def test_allocations_and_failure_cleanup(self):
        source = SOURCE.read_text()
        implementation = "\n".join(function(source, name) for name in (
            "FreeExecuteEmitter", "AllocExecuteMemory", "EnsureExecuteBuffers",
            "EnsureStateBatchBuffer"))
        harness = r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t ULONG;
typedef int BOOL;
typedef void *APTR;
#define TRUE 1
#define FALSE 0
#define MEMF_PUBLIC 1U
#define MEMF_CHIP 2U
#define MEMF_FAST 4U
#define MEMF_LOCAL 256U
#define RADEON3D_MAX_BATCH_DWORDS 8192U
struct ExecBase { int unused; };
struct RadeonChipBase { struct ExecBase *ExecBase; };
struct Emitter {
    void (*Resolve)(void);
    void *ResolveUser;
    ULONG InterfaceVersion, FailStage;
};
struct Radeon3DDevice {
    ULONG *ExecuteGenerated, *ExecuteTrusted;
    struct Emitter *ExecuteEmitter;
    ULONG InterfaceVersion;
};
static union {
    max_align_t align;
    unsigned char bytes[32768];
} storage[8];
static ULONG flags[16], sizes[16], free_sizes[8];
static void *freed[8];
static unsigned calls, used, frees, local_fail, public_fail, fail_mask;
static ULONG memory_type;
static struct ExecBase exec;
static struct RadeonChipBase base = { &exec };
static void *Allocate(struct ExecBase *sys, ULONG bytes, ULONG attributes)
{
    assert(sys == &exec);
    assert(calls < 16 && bytes <= sizeof(storage[0]));
    sizes[calls] = bytes;
    flags[calls++] = attributes;
    if ((fail_mask & (1U << (calls - 1))) ||
        ((attributes & MEMF_LOCAL) ? local_fail : public_fail))
        return NULL;
    assert(used < 8);
    return &storage[used++];
}
#define AllocMem(bytes, flags) Allocate(SysBase, bytes, flags)
static void Release(struct ExecBase *sys, void *address, ULONG bytes)
{
    assert(sys == &exec);
    assert(frees < 8);
    freed[frees] = address;
    free_sizes[frees++] = bytes;
}
#define FreeMem(address, bytes) Release(SysBase, address, bytes)
static ULONG TypeOfMem(const void *address)
{
    assert(address);
    return memory_type;
}
static void Radeon3DEmitResolveSurface(void) {}
""" + implementation + r"""
static void Reset(struct Radeon3DDevice *device)
{
    memset(device, 0, sizeof(*device));
    calls = used = frees = local_fail = public_fail = fail_mask = 0;
    memory_type = MEMF_PUBLIC | MEMF_FAST | MEMF_LOCAL;
    device->InterfaceVersion = 17;
}
int main(void)
{
    struct Radeon3DDevice device;
    void *generated, *emitter;
    unsigned index;

    Reset(&device);
    assert(EnsureExecuteBuffers(&base, &device));
    assert(EnsureStateBatchBuffer(&base, &device));
    assert(calls == 3 && used == 3 && frees == 0);
    for (index = 0; index < calls; ++index)
        assert(flags[index] == MEMF_LOCAL);
    assert(sizes[0] == 32768 && sizes[1] == sizeof(struct Emitter));
    assert(sizes[2] == 32768);
    assert(device.ExecuteEmitter->Resolve == Radeon3DEmitResolveSurface);
    assert(device.ExecuteEmitter->ResolveUser == &device);
    assert(device.ExecuteEmitter->InterfaceVersion == 17);
    assert(device.ExecuteEmitter->FailStage == 0);
    generated = device.ExecuteGenerated;
    emitter = device.ExecuteEmitter;
    assert(EnsureExecuteBuffers(&base, &device));
    assert(EnsureStateBatchBuffer(&base, &device));
    assert(calls == 3 && device.ExecuteGenerated == generated);
    assert(device.ExecuteEmitter == emitter);

    Reset(&device);
    local_fail = 1;
    assert(EnsureExecuteBuffers(&base, &device));
    assert(EnsureStateBatchBuffer(&base, &device));
    assert(calls == 6 && used == 3 && frees == 0);
    for (index = 0; index < calls; index += 2) {
        assert(flags[index] == MEMF_LOCAL);
        assert(flags[index + 1] == MEMF_PUBLIC);
        assert(sizes[index] == sizes[index + 1]);
    }

    Reset(&device);
    local_fail = public_fail = 1;
    assert(!EnsureExecuteBuffers(&base, &device));
    assert(!EnsureStateBatchBuffer(&base, &device));
    assert(!device.ExecuteGenerated && !device.ExecuteEmitter);
    assert(!device.ExecuteTrusted && used == 0 && frees == 0);

    Reset(&device);
    device.ExecuteGenerated = (void *)&storage[0];
    generated = device.ExecuteGenerated;
    local_fail = public_fail = 1;
    assert(!EnsureExecuteBuffers(&base, &device));
    assert(!device.ExecuteGenerated && !device.ExecuteEmitter);
    assert(frees == 1 && freed[0] == generated && free_sizes[0] == 32768);

    Reset(&device);
    assert(EnsureExecuteBuffers(&base, &device));
    generated = device.ExecuteGenerated;
    emitter = device.ExecuteEmitter;
    local_fail = public_fail = 1;
    assert(!EnsureStateBatchBuffer(&base, &device));
    assert(device.ExecuteGenerated == generated && device.ExecuteEmitter == emitter);
    assert(!device.ExecuteTrusted && frees == 0);

    Reset(&device);
    fail_mask = 3; /* Generated fails in both pools, emitter succeeds. */
    assert(!EnsureExecuteBuffers(&base, &device));
    assert(calls == 3 && used == 1 && frees == 1);
    assert(!device.ExecuteGenerated && !device.ExecuteEmitter);
    assert(freed[0] == &storage[0] && free_sizes[0] == sizeof(struct Emitter));

    Reset(&device);
    fail_mask = 6; /* Generated succeeds, emitter fails in both pools. */
    assert(!EnsureExecuteBuffers(&base, &device));
    assert(calls == 3 && used == 1 && frees == 1);
    assert(!device.ExecuteGenerated && !device.ExecuteEmitter);
    assert(freed[0] == &storage[0] && free_sizes[0] == 32768);

    Reset(&device);
    fail_mask = 1; /* Only generated needs the public fallback. */
    assert(EnsureExecuteBuffers(&base, &device));
    assert(EnsureStateBatchBuffer(&base, &device));
    assert(calls == 4 && used == 3 && frees == 0);
    assert(flags[0] == MEMF_LOCAL);
    assert(flags[1] == MEMF_PUBLIC);
    assert(flags[2] == flags[0] && flags[3] == flags[0]);

    Reset(&device);
    memory_type = MEMF_PUBLIC | MEMF_CHIP | MEMF_LOCAL;
    assert(EnsureExecuteBuffers(&base, &device));
    assert(EnsureStateBatchBuffer(&base, &device));
    assert(calls == 6 && used == 6 && frees == 3);
    for (index = 0; index < 3; ++index) {
        assert(flags[index * 2] == MEMF_LOCAL);
        assert(flags[index * 2 + 1] == MEMF_PUBLIC);
        assert(freed[index] == &storage[index * 2]);
        assert(free_sizes[index] == sizes[index * 2]);
    }

    Reset(&device);
    memory_type = MEMF_PUBLIC | MEMF_CHIP | MEMF_LOCAL;
    public_fail = 1;
    assert(!AllocExecuteMemory(&exec, 64));
    assert(calls == 2 && used == 1 && frees == 1);
    assert(freed[0] == &storage[0] && free_sizes[0] == 64);
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix="r3d-exec-memory-") as directory:
            path = pathlib.Path(directory)
            (path / "check.c").write_text(harness)
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                str(path / "check.c"), "-o", str(path / "check"),
            ], check=True)
            subprocess.run([str(path / "check")], check=True)


if __name__ == "__main__":
    unittest.main()
