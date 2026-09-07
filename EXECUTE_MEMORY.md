# Private Execute Memory Placement

Follow-up: [CP_TEXTURE_FIXES.md](CP_TEXTURE_FIXES.md) records the subsequently
installed texture-matrix correction and fused CP copy. Its active pair and
rollback identities supersede this earlier stage's installation state; the
measurements and artifact identities below remain the historical memory-fix record.

2026-09-07. Driver base `13e7077d8a8e` plus the pre-existing lighting-output
selection in `radeon3d_emit.c`. Main MiniGL tree: `abac846`, dirty retirement
and bind-cache work. Only the native retirement change is installed; the new
PPC bind-cache DLL is not part of these measurements.

## Observed Placement

The physical 68060/MPC7410 WarpOS machine at `192.168.1.21:2345` has:

| Memory | Range | Attributes | Priority |
| --- | --- | --- | ---: |
| Amiga expansion RAM | 08000020..18000000 | 0505 | 40 |
| PPC RAM | 50410020..57E00000 | 2005 | 1 |
| Chip RAM | 00008020..00200000 | 0703 | -10 |

During the unchanged PPC gears run, read-only bridge inspection of the
verified driver found its private CPU working allocations in **PPC RAM**:

- Library base `08A04524`, ServiceDevices head at base + 98: `507DB0C0`.
- Device magic `R3DS`, generation 2, interface 17, owner base `08A04524`.
- ExecuteTrusted `51157B78`, ExecuteGenerated `5114FB78`, ExecuteEmitter
  `50E8F610` (device offsets 44, 48, 52 respectively).
- Code/jump targets were in Amiga expansion RAM (`08A1....`).

These are one live session's addresses, not constants to reuse after reboot.
The three working buffers used plain `MEMF_PUBLIC`. Unlike the streamed
vertices and texture surfaces, they do not need GPU or PPC access: generated
words are synchronously copied/swapped into the existing VRAM CP ring.

## Attribution

The unchanged fixed-run control used `DH2:gearsstut/precalc`, 640x480x32,
fullscreen BLIT, two buffers, no sync, host priority -1 and no live tracing.
It completed 600 frames / 2,031 executions, `error=0`, at **45.662 FPS**
(657 DateStamp ticks, 13.14 seconds), and exited normally.

`r3dinfo` before/after counter deltas:

| Phase | Whole Run (us) | Per Frame (ms) |
| --- | ---: | ---: |
| Header/descriptor copy | 545317 | 0.909 |
| Build + RadeonPrepare3D | 5422361 | 9.037 |
| CP submit | 1972758 | 3.288 |

The sample ring held 1,024 contiguous successful samples: 302 Execute clears
and 722 StateBatch submissions. These are elapsed phase intervals, **not
CPU-exclusive measurements**: they include memory latency, waits and any
descheduling. Raw sample ticks use EClock 709379 Hz, while cumulative counters
are already microseconds. Do not call the 13.234 ms total pure driver CPU cost.

CPU-only `tools/r3demitbench.c` isolates the existing emitter without opening
the Radeon service, programming MMIO, submitting a CP packet or drawing.
It resolves fake surfaces backed by an allocated fixture, emits a header
plus repeated VBUF descriptors, and hashes the final command stream after
the timed loop. All 12 cases succeeded and their final word counts/CRCs
matched when allocation placement alone changed:

| Textured Triangle Case | Repeats | PPC RAM Ticks | Local Fast Ticks | Ratio |
| --- | ---: | ---: | ---: | ---: |
| 1 draw | 200 | 212353 | 43886 | 4.84 |
| 5 draws | 200 | 237079 | 60499 | 3.92 |
| 20 draws | 200 | 354134 | 102339 | 3.46 |

The last case emits 419 dwords, CRC `A8A11546`, on both paths. These are
single-session diagnostic samples, **not three cold-boot benchmark medians**
or a prediction of gears FPS. The entire fixture moves (including header,
offset and fake texture), whereas the production patch moves only the three
working buffers. CRC checks cover the final repetition, not every repetition.

A separate `-m68060` emitter experiment also produced matching CRCs and a
smaller CPU-loop cost. Disassembly of the ordinary `-m68020-60` object shows
a 64-bit multiply for `% 3` validation. This is a separate optimization
candidate; **no CPU flags change in the memory-placement driver candidate**.

A priority-0 control completed 300 frames at 21.067 FPS, `error=0`; build
elapsed still averaged 8.42 ms/frame, so priority alone does not explain the
placement penalty. Two other successful diagnostic runs (900 frames with a
concurrent CPU probe; 1,200 frames with live read-only memory inspection) are
not clean benchmark samples and are not used for a speedup claim.

## Correction And Checks

V1 requested `MEMF_PUBLIC | MEMF_FAST | MEMF_LOCAL`. After installation and a
bridge cold reboot, bootstrap and 600 gears frames passed, but gears remained
45.523 FPS (659 ticks) and build elapsed remained about 9 ms/frame. V1 is an
ineffective experiment, not an accepted optimization.

The Sonnet allocator hook explains the failure: `NewAlloc` in
`/home/mirek/SonnetAmiga/sonnetlib/powerpc.s:3251-3340` redirects ANY, FAST or
PUBLIC requests from selected/tagged tasks by adding bit 13 (PPC memory).
`0x105` becomes `0x2105`, which no local/PPC region satisfies, so V1 takes its
old public/PPC fallback. A hardware CPU probe named with the hook's `_68K`
suffix reproduced this: `0x105` failed allocation; `0x100` succeeded at
`0892BF58`, type `0505`, with all 12 output CRCs intact. The suffix also affects
code placement, so those probe timings are not placement-only comparisons.

V2 `AllocExecuteMemory()` requests **only `MEMF_LOCAL`** (`0x100`), avoiding
that hook's redirection condition. It checks the result with `TypeOfMem` and
frees/rejects it if FAST is absent, rather than accidentally retaining Chip
RAM. If that preference fails, the old plain-public allocation still applies.
This changes only ExecuteGenerated, ExecuteEmitter and ExecuteTrusted.
Existing sizes, session ownership,
reuse and FreeMem cleanup are unchanged. No surface/vertex allocation, CP
packet, fence, posted-write readback, public ABI, or record validation changes.

`MEMF_LOCAL` formally means RESET-surviving memory, not portable CPU affinity.
On the observed target its bit excludes PPC RAM; checking the returned FAST
bit excludes chip RAM without triggering the hook. A preferred miss can invoke Exec reclamation before
fallback, as ordinary AllocMem does; no new low-memory behavior was measured
on hardware. The fallback prevents requiring a new memory type to render.

- `python3 -B tools/test_execute_memory.py -v`: passes with ASan/UBSan. It
  compiles the actual helper, both Ensure functions and FreeExecuteEmitter;
  checks local selection, fallback, reuse, total failure, both partial-success
  directions, mixed-pool allocations, state-buffer failure retention, and
  rejected-Chip cleanup with successful/failed public fallback.
- Driver uses unchanged GCC 6.5.0b/O2/`-m68020-60`/`-mregparm=4`/small-code
  flags. Changed service compilation and chip link pass; no undefined symbols.
  Startup remains at text offset zero and resident data follows it.
- The isolated pre-change chip rebuild exactly matches installed `07A7193D`.
  The isolated card rebuild is `C6D31392`, not the installed baseline, and has
  existing legacy card warnings. **It is not used in the candidate pair.**
- Independent review found no blocking production defect. The candidate pair
  retains the actual installed `18A453D6` card, downloaded with CRC verification.
- The diagnostic now snapshots the bounded memory list/names under Forbid and
  prints after Permit. This hardening does not change the measured emitter loop.
  `emitlocal2` was CRC-verified and run afterward: all 12 cases succeeded with
  the same final command CRCs, and the process exited normally.

CPU probe build (same driver emitter CPU/ABI flags; use separate outputs):

```sh
/opt/amiga/bin/m68k-amigaos-gcc -Iinclude -std=gnu99 -O2 -Wall -Wextra \
  -Werror -Wmissing-prototypes -Wstrict-prototypes -m68020-60 -mregparm=4 \
  -msmall-code -noixemul -ffreestanding -fno-builtin \
  -DEMIT_MEMORY_FLAGS=0x100UL tools/r3demitbench.c src/radeon3d_emit.c \
  -lamiga -o /tmp/opencode/emitlocal
```

`0x100` requests LOCAL alone; check the resulting type/address. The original
untagged local-placement comparison used `0x105`, which fails in tagged tasks.
The target-specific PPC comparison uses the Sonnet flag combination `0x2001`,
not an official WarpOS SDK selector. Normal tool builds omit the define and
use MEMF_PUBLIC. All timings and
printing are outside one another's measured intervals.

## Artifacts And Installation

| Artifact | Bytes | CRC32 |
| --- | ---: | --- |
| Original Radeon9200.chip / exact pre-change rebuild | 76588 | 07A7193D |
| Active Prometheus.card / candidate companion | 7796 | 18A453D6 |
| Active native minigl.library (retirement fix) | 126096 | 2F05868E |
| Unchanged PPC DLL | 258144 | 31878AB6 |
| Unchanged quiet host | 36536 | 98DE2066 |
| Unchanged precalc gears | 187136 | 1F3401F7 |
| Ineffective V1 `DH2:drvperf/local.chip` | 76736 | 209446AE |
| Hook-aware V2 `DH2:drvperf/local2.chip` | 76664 | 23ACBAEC |
| Companion `DH2:drvperf/active.card` | 7796 | 18A453D6 |
| Staging `DH2:drvperf/localpair.zip` | 48290 | 15F90CD0 |
| V2 `DH2:drvperf/localpair2.zip` | 48273 | 628FDF4A |
| Original CPU-only `emitbase` | 35576 | 84FC8CDF |
| CPU-only 060-code experiment `emit060` | 35260 | 2B67ED15 |
| PPC-memory CPU probe `emitppcmem` | 35592 | E4E3C26F |
| Local-memory CPU probe `emitlocal` | 35592 | 782E1168 |
| Snapshot-hardened local probe `emitlocal2` | 35684 | 11E38C89 |

V1 SHA256:
`d2efb1d1f18e90ca42d915b18b3b5e9dd85264cb84fa95e73a10033c3a25a191`.
Installed V2 SHA256:
`cadfae330286ef02c1fb512064b2c2dc14ea8eb1f8ac7dd3481e76c562bc338b`.
Candidate/companion ZIP transfer, extraction and destination CRCs verified.
V1 was installed and rendered successfully but did not improve timing. V2 is
now installed and hardware-validated below. All GL clients exited normally;
temporary presentation/priority overrides were cleared. The original placement
measurements reused one healthy boot. After V1's bridge cold reboot, InitPPC was
run once after verifying powerpc.library was absent. No physical power-cycle
claim is made for the bridge reboots.

Board: RV280 `5964`, 64 MiB installed VRAM, 58703872 bytes P96 VRAM, interface
17, service generation 2. `CPU` reports instruction cache/burst and data
cache/copyback enabled. Before installation, monitor `DEVS:Monitors/Prometheus.info` had
`BOARDTYPE=Prometheus`, `CP=YES`, `DMASize=1024K`, `Output=DVI`, but pointed to
generic `SYS:Devs/Picasso96Settings` (1406 bytes, `AB039BC2`), not the required
`.9200` file (1108 bytes, `2DA744EB`). They are different. Do not silently
replace this existing configuration. The user explicitly authorized preserving
current modes and installation: backups are `DH2:drvperf/mon-prev.info`
(`19EA6FF2`), `settings.prev` (`AB039BC2`), `set9200.prev` (`2DA744EB`). The
working generic file was copied byte-for-byte to the `.9200` profile and the
monitor ToolType updated through icon.library. Other ToolTypes are unchanged.
The resulting mode data remains `AB039BC2`; no resolution/refresh change was
introduced. ROM/revision were not re-read in this session.

The user subsequently authorized automatic bridge reboots on the healthy
machine. A hang/guru/grey-screen still requires operator power cycling.
Both driver files were installed together, with CRC checks before boot.
Each bridge cold reboot was followed by wait_bridge.sh polling, explicit
reconnection and exactly one InitPPC after the native bootstrap. No emulator
commands or interrupted GL clients were used. Final Workbench remains
1920x1080x8. The active pair is `23ACBAEC` / `18A453D6`; both exact `.previous`
recovery files hold the original `07A7193D` / `18A453D6` pair. A further original
copy is `DH2:drvperf/base.chip` / `base.card`.

## V2 Hardware Acceptance

Live read-only inspection during a separate 1,800-frame V2 diagnostic run:
library base `08A0452C`, device `507DD140`, ExecuteTrusted `09DEBE60`,
ExecuteGenerated `09DE3E60`, ExecuteEmitter `09D98098`. The device itself is
still in PPC RAM, but **all three changed working buffers are in local RAM**.
The run completed normally with 6,093 executions / 1,800 presents, `error=0`.
Its 56.461 FPS is not included in the clean reboot comparison.

Passed on V2, unchanged native library/DLL/host:

- Native bootstrap, windowed 640x480x16, 20 frames after every measured boot.
- Native phase5 `--stress`: image, subimage, mipmaps, wrap, environment, blend,
  alpha, deferred pixel ordering and residency (48 textures, 512x512).
- Native phase6 primitives (`D655ACB2`): fog, multitexture, scissor, points/lines.
- Native phase6 acceptance (`F0CF74FA`): deferred state/order, arrays, FastPath,
  window move/overlap/resize, batch budget and async; no execute/wait errors.
  Context-loss injection and fullscreen acceptance were not exercised.
- PPC texture update: 7,127 checks, zero failures, two contexts, 18 updates,
  16 frames; host confirms 50 executions / 16 presents and clean close/reopen.
- Final single-session normal fullscreen and precalc windowed gears checks,
  600 frames each: zero errors, 2,031 executions / 600 presents each. Reported
  FPS was 55.555 and 53.667 respectively; these are not three-run medians.
  Fullscreen used two buffers; windowed presentation reports buffers=0.

## Reboot-Isolated Comparison

Six independent **bridge cold reboots**, three per build, not six operator
power cycles. Same pre-test sequence each time: native bootstrap, r3dinfo,
InitPPC once, quiet host at priority -1, then unchanged precalc gears. Command:

```text
precalc -frames 600 -depth 32 -buffers 2 -nosync -envmap chrome.ppm
```

640x480x32 fullscreen, MINIGL_PRESENT=BLIT, 1 MiB shell stack, no live
profiling/tracing, complete run plus final drain timed. V2's first qualification
boot is B1; subsequent execution order was A1, A2, A3, B2, B3. Mode data,
matched card, native library, DLL and host were identical for all six.

| Build | Sample | DateStamp Ticks (50 Hz) | Reported FPS |
| --- | --- | ---: | ---: |
| Original 07A7193D | A1 | 674 | 44.510 |
| Original 07A7193D | A2 | 677 | 44.313 |
| Original 07A7193D | A3 | 677 | 44.313 |
| V2 23ACBAEC | B1 (`v2-gears.log`) | 551 | 54.446 |
| V2 23ACBAEC | B2 | 547 | 54.844 |
| V2 23ACBAEC | B3 | 549 | 54.644 |

Median: **44.313 -> 54.644 FPS, +23.3%** (calculated from ticks). Median frame
time: 22.567 -> 18.300 ms. All 3,600 frames completed with `error=0`; every
host confirmed 2,031 executions / 600 presents and normal shutdown.

Comparable A3/B3 counter deltas, milliseconds per frame:

| Phase | Original A3 | V2 B3 |
| --- | ---: | ---: |
| Copy | 0.878 | 0.708 |
| Build + prepare | 9.147 | 4.918 |
| CP submit | 3.272 | 2.590 |

These remain elapsed intervals, not exclusive CPU time. Input/output dword
counts were identical: 194,010 / 510,189 per 600-frame run. This establishes
a real private-working-memory penalty and a throughput improvement, **not
recovery of the recalled approximately 100 FPS or removal of all frame stalls**.
Other private session/surface structures remain in PPC memory; they are outside
this deliberately three-allocation correction. CPU-flag tuning, bind caching
and texture-update coalescing were not mixed into the comparison.

The six summaries and host logs were joined as `DH2:drvperf/ab-gears.log`
(708 bytes) and `ab-hosts.log` (2736 bytes), then CRC-verified on download into
`/tmp/opencode/driver-base/`. Final active chip/card/native/DLL/host/precalc
CRCs were rechecked. Temporary environment overrides are removed and no GL
client or host remains running.

Raw logs: `DH2:drvperf/{before,after,control,control-host,emitbase,emit060,
emitppcmem,emitlocal,emitlocal2,emit-loaded,load-gears,load-host,pre-pri0,post-pri0,
pri0-gears,pri0-host,mem-gears,mem-host}.log`. Host copies of the main sample
ring and placement probes are `/tmp/opencode/driver-after.log` and
`/tmp/opencode/driver-base/`. Build artifacts are isolated there. No changes
in unrelated main-tree bin/obj files or pre-existing driver edits were reverted.
Additional V1 logs use `new-*`; V2 qualification/regression logs use `v2-*`,
`v2mem-*`, `v2norm*`, `v2win*`; reboot comparison logs use `a1-*`, `a2-*`,
`a3-*`, `b2-*`, `b3-*`. Changes remain uncommitted in both repositories.
