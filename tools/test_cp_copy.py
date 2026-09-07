#!/usr/bin/env python3
"""CPU-only CP tests. Run with python3 tools/test_cp_copy.py.

Builds/runs the actual extracted C fallback with ASan/UBSan, both exhaustive
coverage and the bounded native smoke schedule, then cross-builds cpcheck
with the real 68k assembly and SDK byteswap header. Native keeps all boundary
and failure cases but uses periodic full audits and 128 large sequential commits.
Only radeon_cp.o is built, never a chip/driver candidate. cpcheck uses RAM and
mock reserve/MMIO calls; running it on an Amiga is a separate operator step.
Generated sources, disassembly, logs and the two-file ZIP stay in --out.
"""

import argparse
import binascii
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
BASELINE = "13e7077d8a8e60625d8872d0b7a1f892906c5af1"
CROSS = "/opt/amiga/bin/m68k-amigaos-"
# Keep these identical to the release driver's Makefile CFLAGS.
CFLAGS = shlex.split(
    "-std=gnu99 -O2 -Wall -Wextra -Werror -Wmissing-prototypes "
    "-Wstrict-prototypes -m68020-60 -mregparm=4 -msmall-code "
    "-noixemul -ffreestanding -fno-builtin"
)
CPPFLAGS = ["-I" + str(ROOT / path) for path in (
    "include", "Prometheus/PromLib", "Picasso96Develop/PrivateInclude",
    "OpenPci2.1-SDK290208/Include",
)]


def function(source, name):
    match = re.search(r"^static [^;{}]*\b" + name + r"\s*\(", source, re.M)
    if not match:
        raise RuntimeError("Cannot extract " + name)
    opening = source.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def define(source, name):
    return re.search(r"^#define " + name + r"\s+[^\n]+$", source, re.M)[0]


def kernel_bytes(disassembly, text):
    symbols = [(int(match[1], 16), match[2]) for match in re.finditer(
        r"^([0-9a-f]+) [0-9a-f]+ (_[^:\n]+):$", disassembly, re.M)]
    start = next(offset for offset, name in symbols if name == "_CpBurstCopySwapped")
    end = min(offset for offset, _ in symbols if offset > start)
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path("/tmp/opencode/cp-next"))
    parser.add_argument("--baseline-ref", default=BASELINE)
    parser.add_argument("--baseline-object", type=Path,
                        help="Frozen accepted radeon_cp.o, compared read-only")
    parser.add_argument("--host-only", action="store_true")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    log = []

    def run(command, env=None):
        command = [str(arg) for arg in command]
        print("+ " + shlex.join(command), flush=True)
        result = subprocess.run(command, cwd=ROOT, text=True, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log.append("+ " + shlex.join(command) + "\n" + result.stdout)
        (out / "test.log").write_text("\n".join(log))
        if result.returncode:
            print(result.stdout, end="")
            result.check_returncode()
        return result.stdout

    source = (ROOT / "src/radeon_cp.c").read_text()
    baseline = run(["git", "show", args.baseline_ref + ":src/radeon_cp.c"])
    for name in ("CpStreamDword", "CpCommitStream", "CpReserve"):
        if function(source, name) != function(baseline, name):
            raise RuntimeError(name + " changed outside the copy optimization")
    copy = function(source, "CpBurstCopySwapped")
    old_copy = function(baseline, "CpBurstCopySwapped")
    if copy[copy.index("    while (words--)"):] != old_copy[old_copy.index(
            "    while (words--)"):]:
        raise RuntimeError("Scalar remainder changed")

    constants = "\n".join(define(source, name) for name in (
        "CP_RING_SIZE", "CP_RING_DWORDS", "CP_RING_MASK", "CP_RING_ALIGNMENT",
        "CP_FENCE_DWORDS", "CP_CACHE_FLUSH_ALL",
    ))
    constants += "\n" + define((ROOT / "include/radeon3d.h").read_text(),
                               "RADEON3D_MAX_BATCH_DWORDS")
    (out / "cp_constants.h").write_text(constants + "\n")
    (out / "cp_current.h").write_text("\n\n".join(function(source, name) for name in (
        "CpStreamDword", "CpBurstCopySwapped", "CpCommitStream")) + "\n")
    # The former staging helper exists only in this generated test include.
    start = baseline.rindex("#if defined(__GNUC__)", 0,
                            baseline.index("static void CpBurstStore8"))
    end = baseline.index(old_copy) + len(old_copy)
    old = baseline[start:end].replace("CpBurstStore8", "OldBurstStore8")
    old = old.replace("CpBurstCopySwapped", "OldBurstCopySwapped")
    (out / "cp_old.h").write_text(old + "\n")
    fake = out / "fake/exec"
    fake.mkdir(parents=True, exist_ok=True)
    (fake / "types.h").write_text("""#ifndef EXEC_TYPES_H
#define EXEC_TYPES_H
#include <stdint.h>
typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef uint8_t UBYTE;
typedef int16_t BOOL;
typedef void *APTR;
#define TRUE 1
#define FALSE 0
#endif
""")
    harness = ROOT / "tools/cp_copy_check.c"
    test_includes = ["-I" + str(out), "-I" + str(ROOT / "src")]
    endian = "__LITTLE_ENDIAN__" if sys.byteorder == "little" else "__BIG_ENDIAN__"
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    hosts = []
    for name, flags in (("cpcheck-host", []), ("cpcheck-smoke", ["-DCP_SMOKE"])):
        host = out / name
        print(run([os.environ.get("CC", "cc"), "-std=gnu99", "-O2", "-Wall",
                   "-Wextra", "-Werror", "-Wmissing-prototypes", "-Wstrict-prototypes",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                   "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                   "-DCP_HOST_SANITIZE", "-D" + endian, "-I" + str(out / "fake"),
                   *flags, *test_includes, *CPPFLAGS, harness, "-o", host]), end="")
        print(run([host], env=env), end="")
        hosts.append(host)
    if args.host_only:
        return

    version = run([CROSS + "gcc", "--version"])
    if "6.5.0" not in version:
        raise RuntimeError("The comparison requires the established GCC 6.5 toolchain")
    print(version.splitlines()[0])
    obj = out / "radeon_cp.o"
    print(run([CROSS + "gcc", *CPPFLAGS, *CFLAGS, "-MMD", "-MP", "-c",
               ROOT / "src/radeon_cp.c", "-o", obj]), end="")
    cli = out / "cpcheck"
    print(run([CROSS + "gcc", *CFLAGS, *test_includes, *CPPFLAGS, harness,
               "-lamiga", "-o", cli]), end="")

    # Audit actual opcodes, not just the asm string. MOVEM (An),D0-D7;
    # eight ROL.W #8 / SWAP / ROL.W #8 triplets; MOVEM D0-D7,(An).
    swaps = b"".join(word.to_bytes(2, "big") for reg in range(8)
                     for word in (0xE158 + reg, 0x4840 + reg, 0xE158 + reg))
    fused = re.compile(b"\x4c[\xd0-\xd6]\x00\xff" + re.escape(swaps) +
                       b"\x48[\xd0-\xd6]\x00\xff")
    artifacts = [obj, cli, *hosts]
    for artifact in (obj, cli):
        disassembly = run([CROSS + "objdump", "-dr", artifact])
        (out / (artifact.name + ".dis")).write_text(disassembly)
        raw = out / (artifact.name + ".text.bin")
        run([CROSS + "objcopy", "-O", "binary", "-j", ".text", artifact, raw])
        matches = list(fused.finditer(raw.read_bytes()))
        if not matches:
            raise RuntimeError("Fused MOVEM/swap opcodes absent in " + artifact.name)
        for match in matches:
            print("OPCODES %s text+0x%x bytes=%s" % (
                artifact.name, match.start(), match[0].hex()))
    cli_text = (out / "cpcheck.text.bin").read_bytes()
    kernel = kernel_bytes((out / "radeon_cp.o.dis").read_text(),
                          (out / "radeon_cp.o.text.bin").read_bytes())
    if cli_text.count(kernel) != 1:
        raise RuntimeError("CLI does not contain the exact production copy function")
    print("KERNEL current object/CLI: all %d bytes identical" % len(kernel))
    kernels = {"current_bytes": len(kernel), "current_cli_identical": True}

    if args.baseline_object:
        frozen = args.baseline_object.resolve()
        base_source = out / "baseline_cp.c"
        base_source.write_text(baseline)
        base_obj = out / "baseline_cp.o"
        print(run([CROSS + "gcc", *CPPFLAGS, *CFLAGS,
                   "-I" + str(ROOT / "src"), "-c", base_source, "-o", base_obj]),
              end="")
        for artifact, name in ((frozen, "frozen"), (base_obj, "baseline")):
            run([CROSS + "objcopy", "-O", "binary", "-j", ".text", artifact,
                 out / (name + ".text.bin")])
            (out / (name + ".dis")).write_text(run([CROSS + "objdump", "-dr", artifact]))
        if (out / "frozen.text.bin").read_bytes() != (out / "baseline.text.bin").read_bytes():
            raise RuntimeError("Rebuilt baseline .text differs from frozen accepted object")
        print("BASELINE frozen/rebuilt .text byte-identical")
        old_kernel = kernel_bytes((out / "frozen.dis").read_text(),
                                  (out / "frozen.text.bin").read_bytes())
        if cli_text.count(old_kernel) != 1:
            raise RuntimeError("CLI does not contain the exact frozen baseline copy function")
        print("KERNEL frozen baseline/CLI: all %d bytes identical" % len(old_kernel))
        kernels.update(baseline_bytes=len(old_kernel), baseline_cli_identical=True)
        artifacts.extend((frozen, base_obj))

    archive = out / "cp-next.zip"
    print(run(["zip", "-j", "-FS", archive, cli, obj]), end="")
    print(run(["unzip", "-v", archive]), end="")
    artifacts.append(archive)
    manifest = {
        "head": run(["git", "rev-parse", "HEAD"]).strip(),
        "dirty": run(["git", "status", "--short"]),
        "baseline_ref": args.baseline_ref,
        "compiler": version.splitlines()[0],
        "cflags": CFLAGS, "cppflags": CPPFLAGS,
        "cp_source_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "host": "ASan/UBSan C fallback passed: exhaustive and native smoke schedule",
        "m68k": "cross-built and opcode-audited; NOT EXECUTED",
        "kernels": kernels,
        "artifacts": [],
    }
    for artifact in artifacts:
        data = artifact.read_bytes()
        entry = {"path": str(artifact), "bytes": len(data),
                 "crc32": "%08X" % binascii.crc32(data),
                 "sha256": hashlib.sha256(data).hexdigest()}
        manifest["artifacts"].append(entry)
        print("ARTIFACT {path} bytes={bytes} crc32={crc32} sha256={sha256}".format(**entry))
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("68k cpcheck is CPU/RAM-only, cross-built but not run; no driver linked/deployed.")


if __name__ == "__main__":
    main()
