# CP Copy And Texture-Matrix Corrections

2026-09-07, physical 68060/MPC7410 WarpOS/RV280. Builds are based on driver
`13e7077d8a8e60625d8872d0b7a1f892906c5af1` plus the existing lighting-output and
validated local-execute-memory corrections. Main MiniGL tree remains `abac846`
with its previously recorded dirty work. The measurements preceded the source
checkpoint requested afterward. That checkpoint includes this work's source,
tests and evidence, but excludes the pre-existing lighting-output selection and
unrelated operational edits. Recorded driver binaries include that lighting
selection; the commit alone is not claimed to reproduce those artifact CRCs.

## Texture-Matrix Bug

`TextureOnlyDelta()` deliberately excludes the matrix blocks: they have their
own shadows/uploads. However, `EmitExecuteStateCached()` promoted a successful
texture-only capture and returned before that common matrix-upload code. Two
state-bearing records in one Execute/CommitBatch could therefore change both
texture/sampler and transform, but draw the second object using the old matrix.

The correction removes that premature return, keeping texture-only versus full
state emission mutually exclusive and promoting exactly once. All five matrix
caches remain independently checked: MVP, TEX0, TEX1, model-view and inverse
model-view. It skips redundant lighting uploads only on a successfully proven
texture-only delta. A simple fallthrough without that guard would add
43 + 39 * enabled-lights dwords to previously cheap lit texture switches.

No matrix arithmetic, orientation, record format or public ABI changed. The
existing transpose rules match the local Mesa classic R200 upload_matrix/
upload_matrix_t code: transpose MVP/TEX/MV, copy inverse-MV directly. The local
Mesa checkout identifies itself as 21.3.9; it was not mislabeled as 7.11.2.

This affects multiple ordinary state-bearing records in one submission, not
the later header-free VBUF descriptors in a homogeneous StateBatch. A changed
resolved texture descriptor, offset or legal sampler state can trigger it.
Content-serial bits remain rejected by the existing public validator and were
not used as a reproducer or enabled by this change.

### Regression Proof

`tools/test_tex_matrix.py` compiles the actual emitter and
`tools/tex_matrix_check.c` with fatal ASan/UBSan. Paired inline/CommitVbuf draws
cover all five matrix blocks and payload orientation, cross-unit changes,
unchanged matrices with 0/1/8 lights, lighting-change fallback, promotion,
subsequent unchanged/reuse draws, and output-capacity failure. Result:

- Corrected emitter: **1,188 checks, zero failures**.
- Checked early-return mutation: **139 failures**, reproducing the defect.
- Real 68k `texcheck`: **1,188 checks, zero failures**.

The host generates a header copy without seven 32-bit ABI-size assertions
because host pointers are 64-bit. Production headers are unchanged, and the
real 68k build retains all ABI checks. This is a logic/sanitizer test, not a
host proof of the Amiga ABI.

`tools/r3dtexmatrix.c` is a separate bounded public-service hardware probe.
It allocates three private offscreen 64x64 BGRA surfaces, writes two immutable
uniform textures once, then submits a clear and two HW-TCL rectangles in one
219-dword Execute. Every case waits for its fence before RGB readback.
Coordinates +/-0.25 with MVP X translations +/-0.5 and viewport scale/offset
32 yield rectangles at X=8..24 and X=40..56; samples at (16,32)/(48,32) are
interior. Alpha is not part of this oracle.

| Case | Original 23ACBAEC | Texture Fix 6B860EE0 | Combined 9768419A |
| --- | --- | --- | --- |
| Matrix only | PASS: red/red | PASS | PASS |
| Texture only | PASS: green/black | PASS | PASS |
| Texture + matrix | FAIL: green/black | PASS: red/green | PASS |
| Sampler + matrix | FAIL: red/black | PASS: red/red | PASS |

The controls passed on the original build, so the two failures were not guessed
geometry expectations or a generalized texture-visibility failure. No display
mode, raw CP command or client MMIO access is used by the probe.

## Fused CP Copy

The old `CpBurstCopySwapped()` swapped eight source dwords into a stack array,
then loaded that array back into D0-D7 for a MOVEM ring store. The new m68k
loop loads source words directly into D0-D7, swaps in registers, and performs
the same MOVEM destination store. The complete copy function shrinks from
212 to 160 bytes and eliminates the old 40-byte local stack frame.

Both assembly operands are address-register inputs; D0-D7, condition codes and
memory are clobbered explicitly. ROL.W/SWAP/MOVEM work on the unchanged
68020-60 target. The C fallback handles the same disjoint-buffer contract;
this private function is not a general overlapping-memory copy routine.

Preserved verbatim: scalar remainder, stream-count checks, ring reservation,
wrap splitting, fence/cache/idle tail, packet padding, final volatile ring
readback, WPTR write/readback and software pointer advancement. No fence,
texture posted-write drain, HDP invalidation, or TCL flush was removed.

### Copy Validation

`tools/test_cp_copy.py` extracts the real functions and verifies unchanged
non-copy paths against a pinned pre-change revision. It builds:

- Host exhaustive ASan/UBSan: **2,816 copy + 5,934 commit cases**, passed.
- Host native-smoke schedule: **2,816 copy + 1,966 commit cases**, passed.
- Actual 68k assembly CLI `cpcheck`: **2,816 copy + 1,966 commit cases**, passed
  on the physical machine, including four ring wraps and one sequence rollover.

Cases cover source/destination offsets, 1..17/78/269/419/8192-word lengths,
both fence modes, ring boundaries, zero/oversized/null/not-ready/reservation/
WPTR failures, guards, source immutability and publication ordering. The native
test uses RAM and mock MMIO, not the real CP ring; subsequent rendering tests
exercise the real ring. Native smoke keeps periodic full audits and normal
Ctrl-C handling rather than multi-gigabyte per-case scans.

Opcode auditing established that the CLI contains the entire **160-byte
production kernel byte-for-byte** and the frozen old 212-byte kernel. The
baseline object's rebuilt text is also identical. Host fallback tests alone
are not presented as proof of the m68k assembly or PCI posted-write ordering.

## Isolated Builds

Both builds retain GCC 6.5.0b, O2, `-m68020-60`, `-mregparm=4`, small-code,
freestanding/no-builtin flags and the original startup/library link order.
Chip links have no undefined symbols. Existing legacy card-source warnings
are not part of these chip changes; the actual validated card binary is kept.

- Matrix-only `6B860EE0` links the new emitter object with frozen
  `/tmp/opencode/driver-base/build/radeon_cp.o` (CRC `DC98CCFB`).
- Combined `9768419A` substitutes only `/tmp/opencode/cp-next/radeon_cp.o`
  (CRC `5F97BC46`) into that same link. Every other input object is identical,
  including the local-allocation fix in `service-local2.o`.
- This deliberately prevents the dirty CP source entering the texture-only
  baseline. The two fixes were not attributed as one performance variable.

Build/opcode metadata is in `/tmp/opencode/cp-next/manifest.json` and adjacent
logs; its NOT EXECUTED statement describes the build stage, superseded by the
physical `DH2:cptex/cp-cpu.log` result above. Re-run the CPU gates with:

```sh
python3 -B tools/test_tex_matrix.py --out /tmp/opencode/tex-next
python3 -B tools/test_cp_copy.py --out /tmp/opencode/cp-next \
  --baseline-object /tmp/opencode/driver-base/build/radeon_cp.o
python3 -B tools/test_execute_memory.py -v
```

The public hardware probe builds separately with the normal 68k CLI ABI:

```sh
/opt/amiga/bin/m68k-amigaos-gcc -std=gnu99 -O2 -Wall -Wextra -Werror \
  -m68020-60 -noixemul -Iinclude tools/r3dtexmatrix.c -lamiga \
  -o /tmp/opencode/texpair
```

## Reboot-Isolated Performance

Three independent bridge cold reboots per build, order A1/A2/A3/B1/B2/B3.
These are not operator power cycles. Same native bootstrap and r3dinfo setup,
InitPPC exactly once per boot, then unchanged quiet host and precalc client.
640x480x32 fullscreen, two buffers, BLIT, no sync, host priority -1, 1 MiB stack,
profiling/tracing off. Setup and log output are outside the interval; the final
drain is included. Command:

```text
precalc -frames 600 -depth 32 -buffers 2 -nosync -envmap chrome.ppm
```

| Driver | Sample | Ticks (50 Hz) | Reported FPS |
| --- | --- | ---: | ---: |
| Matrix fix + old CP, 6B860EE0 | A1 | 556 | 53.956 |
| Matrix fix + old CP, 6B860EE0 | A2 | 551 | 54.446 |
| Matrix fix + old CP, 6B860EE0 | A3 | 547 | 54.844 |
| Matrix fix + fused CP, 9768419A | B1 | 526 | 57.034 |
| Matrix fix + fused CP, 9768419A | B2 | 522 | 57.471 |
| Matrix fix + fused CP, 9768419A | B3 | 524 | 57.251 |

Median **54.446 -> 57.251 FPS, +5.2%** (5.153% calculated from ticks). All
3,600 frames completed with zero errors; every host reported exactly 2,031
executions and 600 presents, then exited normally.

A3/B3 driver-counter deltas, elapsed milliseconds per frame:

| Phase | Old CP | Fused CP |
| --- | ---: | ---: |
| Copy | 0.710 | 0.706 |
| Build + prepare | 4.938 | 4.931 |
| CP submit | 2.582 | 1.152 |

Submit elapsed falls **55.4%**; input/generated dword totals remain exactly
194,010 / 510,189 per run. These are elapsed phase measurements, not exclusive
CPU time, and phase savings need not add directly to wall-frame savings.

## Rendering Acceptance

Both the matrix-only and combined drivers passed native phase5 residency stress
(48 textures, 512x512), phase6 primitives (fog/multitexture/scissor/points/lines),
phase6 deferred/arrays/FastPath/window/resize/async acceptance, phase7 lighting,
and the PPC update test (**7,127 checks, zero failures, two contexts, 18 updates,
16 frames; host 50 executions/16 presents**). Context-loss injection and the
full fullscreen acceptance matrix were not exercised.

One matrix-only capture tried to read the host log before its final close and
hit `object is in use`; the script wrapper timed out after that Type failure.
The client had already passed all checks. Subsequent Status and log reads
confirmed normal host shutdown with 50 executions/16 presents. No GL client
was broken, no hardware recovery was needed; later scripts wait after client
exit before reading its host log.

Final combined-driver single-session normal fullscreen and precalc windowed
checks also passed, 600 frames each, zero errors and matching host counts:
57.692 and 55.045 FPS respectively. These are regression checks, not three-run
medians. Fullscreen used two buffers; windowed presentation reports buffers=0.

## Deployment And Evidence

| Artifact | Bytes | CRC32 |
| --- | ---: | --- |
| Prior local-memory baseline | 76664 | 23ACBAEC |
| Texture-only correction / rollback | 76760 | 6B860EE0 |
| Active combined Radeon9200.chip | 76708 | 9768419A |
| Unchanged matched Prometheus.card | 7796 | 18A453D6 |
| Unchanged native minigl.library | 126096 | 2F05868E |
| Unchanged PPC DLL | 258144 | 31878AB6 |
| Unchanged quiet host | 36536 | 98DE2066 |
| Unchanged precalc client | 187136 | 1F3401F7 |
| RGB hardware probe texpair | 12772 | 17941047 |
| Native emitter CPU probe texcheck | 38816 | E4D4790C |
| Native CP RAM probe cpcheck | 18240 | A08EAC5C |
| texprobes.zip | 28979 | 6BAA22B9 |
| matrixpair.zip | 48330 | 768F91FD |
| cp-next.zip | 14268 | D0AD64CD |
| cppair.zip | 48320 | A7A39057 |

Combined chip SHA256:
`68f84f107ce3ea2fd341d6cf00bfc15d4fb2f31b80a8c1ea5692bfa1c3d1914f`.
Matrix-only chip SHA256:
`35ab3debb2314f55befbcda83c857b7c8dd0e6a3ee5b514a63e9517b07f57d9f`.

Transfers, extracted test/driver artifacts, installed pairs and rollback pairs
were checked sequentially by CRC. **Active pair: 9768419A / 18A453D6. Both exact
`.previous` files hold 6B860EE0 / 18A453D6.** The earlier memory baseline remains
as `DH2:cptex/mem-base.chip` / `mem-base.card`; the older original pair remains
in `DH2:drvperf/base.chip` / `base.card`. Recovery still uses both components.

Target: `192.168.1.21:2345`, physical 68060 + MPC7410, Prometheus/RV280 `5964`,
64 MiB VRAM, 58703872 P96 bytes, 1-MiB CP ring, interface 17, EClock 709379 Hz.
Monitor/profile unchanged from EXECUTE_MEMORY.md: BOARDTYPE=Prometheus,
SETTINGSFILE=SYS:Devs/Picasso96Settings.9200 (mode data AB039BC2), CP=YES,
DMASize=1024K, Output=DVI; Workbench 1920x1080x8. ROM/revision not re-read.
Healthy bridge reboots use wait_bridge.sh plus explicit reconnect; hangs still
require an operator power cycle. No emulator commands were used.

All new target evidence is under `DH2:cptex`: `tex-before.log`, `tex-after.log`,
`tex-cpu.log`, `cp-cpu.log`, `mat-*`, `cp-*`, `a1-*` through `b3-*`,
`norm-*`, `win-*`. Combined `ab-gears.log` (708 bytes) and `ab-hosts.log`
(2736 bytes), CP CPU log and before/after RGB logs were CRC-verified on download
to `/tmp/opencode/cp-next/` and `/tmp/opencode/tex-next/`.

The corrected pair is left active, temporary environment overrides are removed,
and no GL client/host remains running. The validated changes are checkpointed
separately from unrelated worktree edits and undeployed PPC bind-cache work.

## Remaining Work

The largest measured driver phase is now build/prepare (~4.93 ms/frame), not
CP copying. Atom-level output reservation and remaining PPC-resident private
metadata are separate candidates. Texture-update allocation/coalescing still
needs its dedicated workload and ownership proof. No unsafe texture-visibility
skip, unrestricted client CP path, ABI extension or compiler-CPU change was
introduced here. Approximately 100 FPS and elimination of every frame stall
remain unproven.
