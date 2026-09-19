#!/usr/bin/env python3
"""Run actual emitter serial validation under ASan/UBSan, plus old-mask mutant."""
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
    with tempfile.TemporaryDirectory(prefix="r3d-serial-") as temporary:
        path = args.out or pathlib.Path(temporary)
        path.mkdir(parents=True, exist_ok=True)
        (path / "exec").mkdir(exist_ok=True)
        (path / "exec/types.h").write_text(
            "#include <stdint.h>\n"
            "typedef uint32_t ULONG; typedef int32_t LONG; typedef void *APTR;\n"
            "typedef uint16_t UWORD; typedef int16_t WORD; typedef int16_t BOOL;\n"
            "typedef uint8_t UBYTE; typedef int8_t BYTE;\n"
            "#define TRUE 1\n#define FALSE 0\n")
        abi, removed = re.subn(
            r"typedef char Radeon3D\w+SizeCheck\[[^\]]*\];", "",
            (ROOT / "include/radeon3d.h").read_text())
        if removed != 8:  # includes the interface-18 indirect request
            raise SystemExit("review changed Radeon3D ABI size checks")
        (path / "radeon3d.h").write_text(abi)
        (path / "radeon3d_emit.h").write_text(
            (ROOT / "include/radeon3d_emit.h").read_text().replace(
                '"../src/radeon_regs.h"', '"radeon_regs.h"'))
        source = (ROOT / "src/radeon3d_emit.c").read_text()
        mask = "~(RADEON3D_TEX_STATE_MASK | RADEON3D_TEX_CONTENT_MASK)"
        if source.count(mask) != 2:
            raise SystemExit("review changed texture-state validators")
        mutant = source.replace(mask, "~RADEON3D_TEX_STATE_MASK")
        for name, text, expected in (("fixed", source, 0), ("old-mask", mutant, 5)):
            cfile, output = path / (name + ".c"), path / name
            cfile.write_text(text)
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-parentheses", "-Wno-sign-compare", "-fno-pie", "-no-pie",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + str(path), "-I" + str(ROOT / "include"),
                "-I" + str(ROOT / "src"), str(cfile),
                str(ROOT / "tools/tex_serial_check.c"), "-o", str(output),
            ], check=True)
            result = subprocess.run([str(output)], capture_output=True, text=True)
            print(name, result.stdout.strip().splitlines()[-1:])
            if result.stderr or result.returncode != expected:
                raise SystemExit(result.stdout + result.stderr +
                                 "unexpected result: " + str(result.returncode))
        print("TEX_SERIAL_HOST pass; old mask rejected")


if __name__ == "__main__":
    main()
