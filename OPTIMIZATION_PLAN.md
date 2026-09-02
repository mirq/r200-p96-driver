# Critical-Path Performance Plan

Goal: make the driver's critical paths meaningfully faster, starting from
measured attribution rather than intuition. This plan follows the
measurement discipline in [`performance.md`](performance.md): no number is
claimed without an artifact-identified run on the reference hardware.

## Status

| # | Step | Status |
|---|------|--------|
| 1 | Release-safe frame-time attribution + Python visualizer | **done in code, hardware runs pending** |
| 1a | Depth-attached replay mode (`-z`, `-pace`) + 16 vs 32 bpp depth matrix | **done, hardware measured** |
| 2 | Merge `CommitRecords` structural walk into the emitter | pending |
| 3 | Emitter atom-level output reservation | pending |
| 4 | 2D per-`RenderInfo` surface-layout cache | pending |
| 5 | Single classified state comparison (replace `SameExecuteState` + `TextureOnlyDelta`) | pending |
| 6 | Fused 68060 swap+`movem` CP ring kernel | pending |
| 7 | Baseline per `performance.md` metadata rules | blocking for all claims |

## Step 1 - Frame-time attribution (this change)

First hardware result (2026-08-31, smoke run, not the formal baseline):
`Radeon9200.chip` 75504 B, CRC32 `C3838A36` (commit bfe2285 + this
instrumentation), 68060/PAL/Exec 40.10, EClock 709379 Hz. Workload
`gears_bench_r200_gcc` windowed 800x600x32, 300 frames at 9.36 FPS
(`MINIGL_GEARS_SUMMARY`), capture `Work:r3d-samplering-1.txt` (1024
samples, all `ok=1`, no recovery events), chart
`gears-frames-samplering.png`.

Second hardware result, current PPC stack (`serfast` = profiled Altivec
gears + `mg_host`, initppc first): 300 frames at 57.73 FPS vs the
uninstrumented reference 57.849 FPS -- instrumentation cost is below run
noise. Ring attribution (`Work:r3d-serfast-1.txt`,
`serfast-frames-sampling.png`): frame period 17.1 ms, driver CPU 2.64
ms/frame median (build 50.6%, submit 35.3%, copy 14.2%), non-driver
share 83%. Ring FPS estimate 58.5 matches the summary. Two outlier
frames carried multi-ms copy/submit stalls (ring back-pressure), 2 of
300. Caveat: for the older async host shape the ring's wall-clock phase
times are inflated by host-task descheduling while the PPC renders
(ec_altivec run measured ~19 ms/frame "driver" time that is not real
driver CPU); the synchronous 68k client (2.23 ms/frame) and serfast
(2.64 ms/frame) are the trustworthy anchors.

Attribution: the driver costs ~2.2-2.6 ms CPU per frame across all three
client generations, while the clients took the frame from 107 ms (68k)
to 35.8 ms (async Altivec) to 17.1 ms (S1 kernels + vseg=4 + drain-paced
host). The driver is now ~15% of the serfast frame, so plan steps 2-3
cap at roughly a 8-10% FPS gain; 2D text workloads must be attributed
separately via the DEBUG block.

## Pre-serialized replay benchmark (step 1 addendum)

`tools/r3dreplay.c` (68k, `make r3dreplay`, deployed as `Work:r3dreplay`)
proves the ceiling with zero per-frame client work: five pre-serialized
frames (one clear Execute + two TCL state batches, rotating only the
ModelProjection) replay round-robin from static vertex segments into a
visible 800x600 screen; the timed loop issues only three prebuilt API
calls per frame, no fences.

Result on the instrumented driver: **57.75 FPS at 800x600x32 --
identical to the full serfast MiniGL stack (57.73)**, and **109.8 FPS at
800x600x16** (exactly 2x with half the bytes). Effective fill bandwidth
comes out ~370-410 MB/s in both depths, i.e. the 32-bit frame is VRAM
fill-bandwidth bound. Driver CPU for the replay shape is only ~1.05
ms/frame (103 record dwords/frame) with the GPU-bound loop showing
multi-ms submit stalls (ring back-pressure) as expected.

Conclusions: (1) the current 57.8 FPS stack is GPU-fill-bound at
800x600x32 -- client and driver are not the bottleneck there; (2) the
m68k driver + pipeline demonstrably sustain 110 FPS when the fill load
halves, so the headroom at 32-bit is bounded by pixels written per
frame (clear strategy, overdraw, target depth), not by m68k CPU work;
(3) the replay tool is the reference ceiling probe for future driver
changes: same inputs, zero client variance.

### Ceiling decomposition (r3dreplay modes, 800x600, all fail=0)

| Configuration | FPS | ms/frame |
|---|---:|---:|
| clear only, 32bpp | 476 | 2.10 |
| clear only, 16bpp | 903 | 1.11 |
| geometry only (6480 small tris), 32bpp | 63.4 | 15.78 |
| geometry only, 16bpp | 119.3 | 8.38 |
| full, 32bpp, visible screen | 57.75 | 17.31 |
| full, 32bpp, offscreen target | 54.6 | 18.31 |
| geometry only, 32bpp, offscreen | 70.2 | 14.25 |

Reading: the R200 clear (two huge triangles) runs at ~900 MB/s -- native
engine rate, and proof that GPU-to-VRAM fills never traverse the
Prometheus bridge (whose CPU aperture is ~6 MB/s and which carries only
~1.4 KB/frame of ring commands, ~0.25 ms). The 6480-small-triangle
geometry runs at ~300-336 MB/s and scales exactly with bytes per pixel:
the ceiling is per-triangle rasterizer setup + memory bandwidth of the
RV280, not the Prometheus board and not the m68k. Scanout is free
(offscreen == screen within noise). Offscreen mode must keep the screen
open for the whole run: closing it lets the deferred board release
advance the service generation and silently invalidate the session
mid-run (the tool now counts failed submits to catch this).

Problem: the per-phase Execute timers used one `TR_GETSYSTIME` DoIO per
phase boundary, so they were made DEBUG-only and release builds reported
zeros. There was no way to see what takes most of a frame in a release
driver.

### Depth-attached replay (phase 0 addendum, 2026-09-01)

`r3dreplay` gained `-z`: a 16-bit RGB565 depth surface (width padded to
64-pixel tiles, allocated against a depth-matched 16-bit seed-screen
friend -- a 32-bit screen friend makes p96AllocBitMap hand out a 32-bit
bitmap regardless of the rgbFormat argument), a color+depth clear
record, and `RADEON3D_DRAW_DEPTH_LESS | RADEON3D_DRAW_DEPTH_WRITE` on
every draw. This is the gears shape: the real MiniGL stack always
clears and depth-tests Z16 at both color depths, so the depth traffic
must be in the ceiling comparison. `-pace N` (default 64) waits on the
last batch's fence every N frames.

Why pacing is required: the tool never drained, so a long run outpaces
the GPU until the CP ring space wait (`CP_TIMEOUT_POLLS`) expires; the
service then treats the submit failure as CP death and runs full
recovery, which invalidates the session (`Radeon3DSubmit`,
`src/radeon3d_service.c`, the `RadeonRecoverAcceleration` call after a
failed `RadeonCpSubmitStream`). Every later submit then fails for the
rest of the run. Three of nine unpaced 2000-frame runs hit this
(fail=1188..5712, FPS inflated and meaningless). A real client paces
per frame and does not hit it, but a real client *can* ring-fill during
a hitch and lose its session -- flagged as a robustness follow-up: a
ring-full timeout is not a CP hang and recovery there is aggressive.

Results (800x600, 2000 frames, `-pace 64`, fail=0, three runs each,
median; RV280, driver interface 16, generation 5, warm session, no
cold-boot protocol -- not a formal `performance.md` baseline):

| Configuration | Median FPS | ms/frame |
|---|---:|---:|
| full, 32bpp, color-only clear | 53.6 | 18.7 |
| full, 32bpp, +Z16 clear/test/write | 62.8 | 15.9 |
| full, 16bpp, color-only clear | 104.4 | 9.6 |
| full, 16bpp, +Z16 clear/test/write | 100.3 | 10.0 |

Readings: (1) the depth-enabled 16 vs 32 ratio is **1.60x**
(100.3/62.8), exactly the 4-to-6 bytes-per-pixel bandwidth prediction
-- the old "2x at 16-bit" replay claim compared color-only cells and
was never a valid prediction for a depth-tested workload. (2) Depth
*enabled* is faster than color-only at both depths: early-Z culling on
the self-occluding torus removes more overdraw than the Z traffic
costs, so depth on/off is not a pure bandwidth A/B. (3) The gears-shaped
16-bit ceiling at 800x600 is ~100 FPS; the measured serfast stack
(57.8 at 32bpp) is client/present/pipeline-limited, and 16-bit headroom
on the real stack is bounded by that 1.60x only if the rest of the
pipeline also scales -- which the per-frame `ClipBlit` (windowed) and
flip pacing (fullscreen) do not.

Follow-ups opened by this run: (a) recovery-on-submit-failure is too
aggressive for ring back-pressure (above); (b) fullscreen flips
invalidate the full semantic state serial per frame
(minigl_ppc `library/r200_surface.c`, `BumpSemanticStateSerial` after
`ChangeScreenBuffer`) -- phase 2 target; (c) the +Z-vs-no-Z result
suggests overdraw/culling strategy is worth measuring before chasing
more bandwidth.


Delivered:

- `timer.device` opens once on the first session open in **every** build.
  Phase boundaries use `ReadEClock()` -- one direct device call, no message
  round trip (the DEBUG microbenchmark puts it well under a microsecond).
  Cumulative `ExecCopy/Build/SubmitMicros` in the info block are live again
  in release; conversion uses one 64-bit multiply/divide per phase.
- New per-submission sample ring (`RADEON3D_SAMPLE_RING_SIZE` = 1024
  entries, one `Radeon3DSample` each: Seq, wall tick, type, result, dwords
  in/out, copy/build/submit ticks) written under the service lock at the
  end of `Radeon3DExecute`, `CommitRecords` (draw + batch),
  `Radeon3DCommitStateBatch`, and `Radeon3DSubmit`. Entry Seq is stored
  last with a compiler barrier, so readers skip torn entries.
- Info block V4 tail (`RADEON3D_INFO_V4_SIZE` = 76): `SampleRing`,
  `SampleRingEntries`, `SampleSeq`, `EClockHz`, size-gated like V1-V3.
- `radeon3dinfo` dumps `R3DCLOCK hz=...` and one `R3DSAMPLE ...` line per
  captured submission.
- `tools/perfplot.py`: parses capture files, groups submissions into
  frames (`--frame-gap-ms`, default 2), reports per-type and per-phase
  totals, median/p95 driver CPU per frame, frame period and estimated FPS,
  worst frames, an ASCII waterfall, and (if matplotlib is installed) a
  stacked phase chart + period-vs-driver plot + histogram via `--out`.
  `--debug dump.txt` decodes a 2D DEBUG block (reusing
  `decode_debug_stats.py`) for per-callback attribution.

Hardware procedure (once, cold boot, per `performance.md` metadata rules):

1. Deploy release driver, record artifact hashes.
2. Run the workload (e.g. gears), then `Work:p96overlap >...` style capture:
   `Work:radeon3dinfo >Work:r3d-capture.txt` right after the workload.
3. `python3 tools/perfplot.py r3d-capture.txt --out frames.png`.
4. For 2D attribution, build `DEBUG=1`, capture the `Radeon9200.Debug`
   block with `amiga_inspect_memory`, and run
   `python3 tools/perfplot.py --debug dump.txt`.

Known limits: EClock resolution is ~1.4 us on the reference 68060, so
per-call numbers for tiny batches are quantized -- trust accumulated
totals and frame-level sums. The ring holds the last 1024 submissions;
`radeon3dinfo` reads it without the service lock and skips torn entries.

## Step 2 - Merge CommitRecords structural walk into the emitter

`CommitRecords()` walks every record up front
(`src/radeon3d_service.c`, the `walk`/`index` loop) to count draws and
verify the offset table, then `Radeon3DEmitStream()` parses the same chain
again. Have the emitter count draws as it consumes, guard
`CommitDrawIndex < recordCount`, validate each offset once against the
segment, and compare the final draw count with `recordCount` before
submission. Removes one complete cached-memory pass; GPU bounds checks are
unchanged.

## Step 3 - Emitter atom-level output reservation

Every `ExecuteEmitWord()` bounds-checks and post-increments `Count`
(`src/radeon3d_emit.c`); `ExecuteEmitRegister()` therefore pays two checks
per register pair and matrix/vector blocks repeat it per dword. Reserve
capacity once per atom (state block, matrix, vertex batch), write through
a local output pointer, commit `Count` once. This replaces the hottest
branch chain in the build phase and is likely worth more than hand-written
assembly for the emitter.

## Step 4 - 2D surface-layout cache

`ValidateSurface()` recomputes format datatype, aperture limits, pitch
alignment, GPU address alignment, and biases (division/modulo) for every
fill/copy/template callback (`src/radeon_accel.c`). Layout is invariant
per (board, memory, pitch, format, MemorySize, MemorySpaceSize, installed
VRAM, FramebufferGpuBase) -- exactly what the line cache
(`ValidateLineSurface()`) already keys on. Add a `RenderInfo`-keyed layout
cache and keep only per-rectangle coordinate/end checks in the hot path;
`ValidateSameSurfaceRectangle()` shows the pattern. `RadeonBlitRectNoMaskComplete()`
also validates two identical-layout surfaces independently -- reuse the
same-surface path when memory/pitch/format match. Highest-value item for
small fills, glyphs, and fragmented `ClipRect` workloads (the measured
p96overlap text gap).

## Step 5 - Single classified state comparison

`EmitExecuteStateCached()` calls `SameExecuteState()` and, on mismatch,
`TextureOnlyDelta()`, which re-compares most of the same fields including
all enabled light blocks. Replace with one comparison returning
`equal | texture-only | changed`, or maintain a change mask / fingerprint
per section while capturing. Cuts per-record build cost for multi-draw
streams with lighting.

## Step 6 - Fused 68060 CP ring kernel

`CpBurstCopySwapped()` stages eight swapped longwords to the stack, then
`movem` loads them back and stores to the VRAM aperture
(`src/radeon_cp.c`). A 68060 assembly kernel can load source dwords into
d0-d7, apply `rol.w #8; swap; rol.w #8` in registers, and issue one
`movem.l d0-d7,(a0)`. The prior `movem` work bought 13.9% of submit
(56.6 -> 53.0 ms/frame overall); expect a few percent, not 2x. The
final-dword aperture readback before the WPTR update must stay.

## Step 7 - Baseline

Run the full `performance.md` version-3.0 procedure (cold boot, 8/16/32/8
`p96screen` gate, P96Speed, `p96overlap`, `p96windowmove`, plus the 3D
acceptance suites) before and after each optimization step lands, with the
metadata this file and `performance.md` require. No claim without it.

## Checks that must survive every step

- `ValidateSurface()` bounds/pitch/overflow checks (offboard memory,
  offset overflow, hardware limits, VRAM end).
- Texture/depth/overlap/float/segment/scissor validation in
  `Radeon3DEmitDraw()`; the single-load discipline for caller-owned
  records.
- CP final-dword readback before `WPTR` update (`src/radeon_cp.c`).
- FIFO/idle/fence timeouts and recovery paths.
- Lock order `BoardLock` before `ServiceLock`, and session
  generation/owner validation.
