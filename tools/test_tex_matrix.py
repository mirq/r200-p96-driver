#!/usr/bin/env python3
"""Compile real paired-record emission, including the old early-return mutant."""
import argparse
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=pathlib.Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="r3d-matrix-") as temporary:
        path = args.out or pathlib.Path(temporary)
        path.mkdir(parents=True, exist_ok=True)
        (path / "exec").mkdir(exist_ok=True)
        (path / "exec/types.h").write_text(
            "#include <stdint.h>\n"
            "typedef uint32_t ULONG; typedef int32_t LONG; typedef void *APTR;\n"
            "typedef uint16_t UWORD; typedef int16_t WORD; typedef int16_t BOOL;\n"
            "typedef uint8_t UBYTE; typedef int8_t BYTE;\n"
            "#define TRUE 1\n#define FALSE 0\n")
        # Host pointers are 64-bit; ABI sizes are checked by the real 68k build.
        # Keep every declaration/constant, omitting only seven ABI size asserts.
        abi, removed = re.subn(
            r"typedef char Radeon3D\w+SizeCheck\[[^\]]*\];", "",
            (ROOT / "include/radeon3d.h").read_text())
        if removed != 7:
            raise SystemExit("review changed Radeon3D ABI size checks")
        (path / "radeon3d.h").write_text(abi)
        (path / "radeon3d_emit.h").write_text(
            (ROOT / "include/radeon3d_emit.h").read_text().replace(
                '"../src/radeon_regs.h"', '"radeon_regs.h"'))
        current = (ROOT / "src/radeon3d_emit.c").read_text()
        boundary = "        } else if (!EmitExecuteState(emitter, state))"
        if current.count(boundary) != 1:
            raise SystemExit("review changed texture-only return mutation")
        mutant = current.replace(boundary,
            "            PromoteExecuteState(emitter);\n"
            "            return TRUE;\n" + boundary, 1)
        for name, source in (("fixed", current), ("old-return", mutant)):
            cfile = path / (name + ".c")
            cfile.write_text(source)
            output = path / name
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-parentheses", "-Wno-sign-compare", "-fno-pie", "-no-pie",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + str(path),
                "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src"),
                str(cfile), str(ROOT / "tools/tex_matrix_check.c"), "-o", str(output),
            ], check=True)
            result = subprocess.run([str(output)], capture_output=True, text=True)
            summary = result.stdout.strip().splitlines()
            print(name, summary[-1] if summary else "no summary")
            if result.stderr:
                print(result.stderr)
            expected = 0 if name == "fixed" else 5
            if result.returncode != expected:
                print(result.stdout)
                raise SystemExit("unexpected " + name + " result " + str(result.returncode))
        print("TEX_MATRIX_HOST pass; old early return rejected")


if __name__ == "__main__":
    main()
