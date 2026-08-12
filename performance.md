# 2D Performance Work

This file tracks real-hardware benchmark results, accepted optimizations, and
the order in which unvalidated acceleration candidates should be tested.
P96Speed automation is documented separately in [`p96speed.md`](p96speed.md).

## Reference Configuration

- Amiga 4000, 68060 at 50 MHz
- Picasso96, `rtg.library` 43.538
- RV280 Radeon 9200 `1002:5964`
- P96Speed 1.2, 640x480x16, 13 seconds per test

The closed-driver reference is `Work:P96Speed_org.txt`. Version 0.12 produced a
validated focused solid-fill result of 10688 operations/second.

## Overlapping-Window Verification

The deterministic benchmark for this workload is documented in
[`p96overlap.md`](p96overlap.md). Its first two safe-driver runs measured a
stable one-to-19 `ClipRect` transition and an overlap ratio of 1.314x-1.336x.
Use that benchmark, rather than P96Speed's synthetic window rows, for paired
driver comparisons.

### Smart-refresh window-open fix

The shortened `p96windowmove` version 7 isolated a separate pathological path.
Three release Radeon9200 runs took 9.539-9.572 seconds in `overlap_open_us`,
while three Devil's Cut runs took 10.916-11.574 ms. DEBUG versions 9-11 added
bounded-wait, recovery, opcode histogram, and complete-copy phase timing:

- FIFO and idle waits had zero failures; no engine recovery occurred.
- Accepted complete-copy submissions succeeded immediately.
- The generic `BlitRectNoMaskCompleteDefault` interval accounted for about
  11.5 seconds in the instrumented run, with individual calls up to 713 ms.
- The slow smart-refresh workload used opcode `$6`, source XOR destination.

Radeon9200 now maps `$6` to hardware ROP3 `S XOR D`. Cross-surface operations
whose physical VRAM ranges overlap choose forward/reverse direction from their
absolute range order, as Devil's Cut does, instead of rejecting overlap to the
generic default. Existing surface, format, pitch, coordinate, and memory-bound
validation remains in force; other opcodes still fall back.

The final release card is CRC32 `B6239209`, 35,168 bytes. Three separate version
7 runs were byte-identical: whole time 177.773 ms and `overlap_open_us` 11.727
ms. That is about an 815x improvement in the pathological stage and puts it
inside Devil's Cut's measured 10.9-11.6 ms range. The full 8/16/32/8 correctness
sequence passed in DEBUG, including focused `$6` start/end/edge/source checks;
the deployed release repeated the full RGB565 pass. No crash was recorded.

Two Devil's Cut 0.666 runs produced the identical one-to-19 geometry and were
about 20.6 percent faster empty and 19.0 percent faster overlapped. Its overlap
multiplier was slightly worse, about 1.353x against Radeon9200's 1.325x mean.
The closed driver therefore does not select a layer path that avoids overlap
fragmentation; it executes this mixed fill/text workload more cheaply in both
states. Split fill-only and text-only measurements are required before choosing
the next callback optimization.

Version 2 split the same workload while Devil's Cut was active. Across two
runs, text alone took 187.7-189.7 ms empty and 234.2-238.2 ms overlapped, about
94 percent and 84-86 percent of the respective combined times. Full-window fill
was 3.38 ms empty and 8.50-8.62 ms overlapped; narrow fills were 8.79-9.00 ms
and 21.71-21.88 ms.

The exact same version 2 binary (`D6803586`) was then run twice under the safe
Radeon9200 card (`5E6FE3A3`). Text took 247.0-248.3 ms empty and 302.9-307.1 ms
overlapped, making its mean 31.3 and 29.3 percent slower than Devil's Cut. The
mean text gaps, 58.96 and 68.77 ms, explain essentially all of the empty
combined gap and about 91 percent of the overlap combined gap. Full-window fill
was only 5.2 and 11.9 percent slower; narrow fills were 22.9 and 25.1 percent
slower. Optimize `BlitTemplate` first, then reassess rectangle submission.

Static disassembly then corrected the assumed closed-driver path. Devil's Cut
`BlitTemplate` is `0x3360`-`0x3552`; its inner loops write every word directly
to `HOST_DATA0` and perform no FIFO-status reads. The checks beginning at
`0x3568` belong to `BlitPattern`.

A release prototype (`A1EAEE2E`) changed only Radeon9200's host-data submission
to match that behavior, retaining one bounded FIFO reservation for setup and
all existing validation, clipping, extraction, synchronization, recovery, and
fallback logic. Across two version 2 runs, mean isolated text fell from 247.66
to 219.23 ms empty (11.5 percent) and from 304.97 to 274.50 ms overlapped (10.0
percent). Mean combined redraw fell from 260.80 to 240.77 ms empty (7.7 percent)
and from 352.81 to 317.38 ms overlapped (10.0 percent). ClipRect geometry was
unchanged. The full 8/16/32/8 `p96screen` sequence passed with no `FAIL`, and no
crash was recorded. This closes 39-44 percent of the measured text gap to
Devil's Cut without using the broken VRAM-staging mode.

The first sustained real-window movement run made AmigaBridge unavailable while
`A1EAEE2E` was active, but the same symptom reproduced after restoring the safe
`5E6FE3A3` card and increasing the benchmark to `1024x768x16`. It therefore does
not establish a driver regression. The original 24-round-trip workload is an
excessive synchronous layers/refresh stress test at that resolution; the
routine comparison default was reduced to four round trips per trial. Keep the
safe card installed until the bounded workload is validated.

Driver switching also requires matching the Picasso96 mode database. A v3
Radeon9200 comparison attempt was aborted after changing `BOARDTYPE` alone:
`Radeon9200.card` loaded, but Workbench fell back to native PAL. Radeon9200 must
use `SETTINGSFILE=SYS:Devs/Picasso96Settings.9200`; do not compare results until
both monitor ToolTypes have been changed and the expected modes are available.

With the matching `.9200` settings restored, the exact version 3 moving-window
benchmark (`C0C2D18D`) completed under both drivers at `1024x768x16`, RGB format
4. Radeon9200 measured 291 us per empty move and 295 us per overlapping move;
Devil's Cut measured 302 and 305 us. Radeon9200 was therefore 3.7 percent faster
empty and 3.2 percent faster overlapped. The overlap multipliers were only
1.013x and 1.009x, so moving a smart-refresh window does not reproduce the large
text-heavy redraw gap from the fixed-window benchmark. Both drivers showed
occasional multi-millisecond outliers that can temporarily disconnect the
bridge, but their seven-trial medians were stable and neither run crashed.

Version 4 (`6D7417ED`) showed that the median-only interpretation was wrong for
the user's perceived workload. Radeon9200 took **127.555233 seconds** from mode
lookup through the final movement phase, while Devil's Cut took **0.182421
seconds**, making Devil's Cut about 699 times faster end to end. The complete
movement phases were only 20.471/26.998 ms for Radeon9200 and 19.564/22.764 ms
for Devil's Cut. Subtracting them leaves 127.508 seconds versus 0.140 seconds in
screen/window setup, initial rendering, and the transition between phases.
Therefore `MoveWindow()` throughput is not the visible problem; the
text-heavy population and refresh of real windows is. Version 3's statement
that the movement path did not reproduce the redraw gap is superseded.

On 2026-08-11, a cold-boot DEBUG build extended the public statistics block to
record `BlitRectNoMaskComplete` submissions and each fallback reason. The test
used the reference A4000/68060/RV280 machine with `CP=YES`, `DMASIZE=2M`, and
`HWSPRITE=YES`. It did not change copy-path behavior.

After boot, a 600x300 shell was opened at `(300,100)`, overlapping the existing
AmigaBridge window. The complete-copy counter delta was:

| Counter | Delta |
|---|---:|
| Calls | 1 |
| Hardware | 0 |
| Software default | 1 |
| Unequal pitch | 1 |
| Unsupported opcode | 0 |
| Overlap | 0 |
| Non-VRAM surface | 0 |
| Invalid surface | 0 |
| Acceleration unavailable | 0 |

An otherwise equivalent 280x250 shell was then opened in unoccupied screen
space at `(10,450)`. Every complete-copy counter remained unchanged. Thus the
slow path is specific to covering an existing window, and the controlled delta
identifies the equal-pitch restriction in `RadeonBlitRectNoMaskComplete`, not
the opcode, surface validation, overlap handling, or engine state, as the sole
reason that the layer backing-store copy used the CPU default.

The original release card (`CRC32 55CCD2AE`, 33,840 bytes) was restored after
the test and cold-booted without a recorded crash.

### Validated fix

The equal-pitch gate was removed after the diagnostic run. `SubmitCopy()`
already programs `SRC_PITCH_OFFSET` and `DST_PITCH_OFFSET` independently; all
other validation, disjointness, opcode, and fallback checks remain intact.

The focused cross-surface test was changed to allocate a half-screen-width
source bitmap. A cold-boot DEBUG build passed the complete 8/16/32/8 sequence:

| Format | Source pitch | Destination pitch | Complete4096 | Result |
|---|---:|---:|---:|---|
| CLUT8 | 320 | 640 | 29 ticks | PASS |
| RGB565PC | 640 | 1280 | 29 ticks | PASS |
| BGRA32 | 1280 | 2560 | 29 ticks | PASS |
| CLUT8 return | 320 | 640 | 29 ticks | PASS |

Every start/end/edge/source guard and final direct-color readback passed. After
the sequence, opening one shell over two existing windows changed the DEBUG
counters by two calls, two hardware submissions, two unequal-pitch operations,
and zero software fallbacks. Opcode, overlap, surface, and acceleration-state
rejection counters were unchanged, and no crash was recorded.

The final release artifact (`CRC32 281789DC`, 33,824 bytes) was then cold-booted
and ran the full RGB565 test. The unequal-pitch `640 -> 1280` copy guards and
direct-color readback passed, and 4096 complete copies took 11 ticks. No crash
was recorded.

## Whole-Callback Attribution (DEBUG version 6)

Removing the equal-pitch gate above fixed a real bug but did not make
overlapping windows feel fast, so the statistics block was extended to record
calls, hardware/software split, and EClock ticks for *every* 2D callback rather
than for `RectFill` alone. Versions 2-5 could only attribute time inside
`RectFill`, which is what P96Speed measures; interactive work is a different
mix and nothing had ever measured it. The release card is unchanged by the
addition (`CRC32 281789DC`, 33,824 bytes, byte-identical to the artifact
validated above).

Method: read the block, perform one action, read again, subtract. A third
read with no action in between measures the background rate, because the
AmigaBridge status window repaints continuously. Reference machine, 16bpp,
`CP=YES`, `DMASIZE=2M`, `HWSPRITE=YES`.

| Callback | open over a window | open in empty space | background |
|---|---:|---:|---:|
| FillRect | 3374 / 176 ms | 2405 / 117 ms | 2037 / 98 ms |
| BlitTemplate | 3488 / 534 ms | 2328 / 280 ms | 1779 / 206 ms |
| WaitBlitter | 13771 / 268 ms | 9521 / 175 ms | 7632 / 134 ms |
| BlitPattern | 10 / 2 ms | 13 / 2 ms | 0 |
| **Total** | **980 ms** | **574 ms** | **439 ms** |

Background-subtracted, one window opened over another costs about 1337 fills,
1709 template blits and 6139 drains; the same window opened in free space costs
about 368, 549 and 1889. **Overlapping roughly triples the number of drawing
operations**, which is layers.library splitting every render across the
ClipRects it created, not a driver decision. The driver's job is to make each
of those operations cheap.

Per-call gross cost in the instrumented build was 52 us for a fill and
120-153 us for a template blit. `BlitTemplate` is therefore the single most
expensive callback the driver installs and accounts for over half of all
driver time during interactive work. Its cost is almost entirely the host-data
upload: 103,192,974 uploaded longwords over 1,217,878 calls is 84.7 words per
call, and at the measured 1.33 us per MMIO write that is 112 us of the
measured cost. Each word is an individual non-burstable PCI write to one
register.

### Rejected: handing text back to the CPU

Because the earlier prototype note in this file recorded the software default
beating a host-data path 26 ticks to 52, the obvious inference was that
`BlitTemplate` should not be advertised at all. **A paired measurement refuted
this.** Two DEBUG cards differing only in whether `bi->BlitTemplate` is
installed, 4096 operations each at 16bpp:

| Benchmark | CPU default | host-data upload |
|---|---:|---:|
| `template4096` (8x8) | 30 | 29 |
| `template64-4096` (64x8) | 123 | **32** |
| `text1-4096` (`Text("X")`) | **33** | 47 |
| `text8-4096` (`Text("P96Speed")`) | 108 | **60** |

The upload path is 3.8x faster on a 64-pixel template and 1.8x faster on an
eight-character string. Only single-character `Text()` favours the CPU. The
callback stays installed; `HWTEXT=NO` exists to repeat this comparison without
a rebuild. The earlier 26-vs-52 note described a different, since-replaced
submission sequence and should not be generalised.

### Rejected: optimising the drain

`WaitBlitter` looked like the obvious next target because rtg.library calls it
after every operation. The counters say otherwise. `DrainReads / DrainCount`
is 1.0002 and `DrainWrites` is 0; half of all calls are no-ops because
`SynchronizeEngine()` returns immediately when `AccelPending` is clear, so a
**real drain costs two `RBBM_STATUS` reads, about 3 us**. The
`WaitFifo(bi, 64)` inside `WaitIdleAndFlush()` is not redundant either: it is
what lets `SubmitSolidRect()` write its seven registers with no reservation at
all, and both the Xorg radeon driver and Linux radeonfb use the same
fifo-then-idle sequence. Removing it would risk the reservation-free fill path
to win under one percent.

Ranked by measured PCI time, the drain is fourth:

| Source | Time | Share |
|---|---:|---:|
| Template host-data writes | 137.0 s | 75% |
| Template FIFO polling | 26.9 s | 15% |
| Fill submits | 10.8 s | 6% |
| Drain polling | 7.2 s | 4% |

### Superseded: burst FIFO reservation for template uploads

`SubmitTemplate()` reserved exactly the entries needed for one chunk, and a
typical row is six words, so it paid one `RBBM_STATUS` read per glyph row:
measured at **14.5 reads per template blit**, more than the drain and the fill
submits combined. Reserving `ACCEL_UPLOAD_BURST` (32) entries at a time and
spending them from a local counter amortises that read across four chunks. The
loop still never writes more than it has reserved, so the change is a strict
subset of the previous behaviour.

Steady-state Workbench measurement, same workload shape as the baseline:

| | before | after |
|---|---:|---:|
| Reads per template blit | 14.50 | **3.76** |
| Polling cost per blit | 21.6 us | **5.6 us** |
| Per blit, writes held constant | 134.0 us | **118.0 us** (11.9% faster) |

Fill submits are unchanged at 6.89 writes and zero reads. The full 8/16/32/8
`p96screen` sequence passed with no `FAIL` line, including `template-bg`,
`template-fg`, `template-edge`, `template-guard`, `template-j1bg` and
`template-j1fg`, and no crash was recorded. A 64-entry burst would save roughly
2 us more but waits for a completely empty FIFO on every reservation; 32 was
kept as the balance.

The later repeated-`HOST_DATA0` prototype above supersedes this compromise: it
removes mid-upload FIFO reads entirely, matching the closed drivers' proven
RV280 sequence, and passed the same pixel-regression coverage.

### Mono-source-from-memory: capability confirmed on RV280

Template host-data writes are 75 percent of all PCI time, and the disassembly
shows the inner loop is already four instructions per longword
(`bfextu`, `addq`, `move.l (a0)+`, `dbf`) with 86 percent of each iteration
spent stalled on the bus. There is nothing left to win in the instruction
stream; the only lever is issuing fewer or cheaper writes. Two facts were
established before building anything, because two earlier `BlitTemplate`
prototypes were rejected on this hardware.

**Aperture writes are cheaper than register writes.** Timing the same 2000
consecutive longwords both ways, repeated across three cold boots:

| | per longword |
|---|---:|
| MMIO register write (`MmioWriteTicks`) | 1.33 us |
| Framebuffer aperture write (`VramWriteTicks`) | **0.61 us** |

**The 2D engine can colour-expand a monochrome source read from VRAM.** A
DEBUG-only probe at `RadeonInitializeAcceleration()` stages a mono pattern in
unallocated VRAM, expands it with `DP_SRC_SOURCE_MEMORY` plus
`GMC_SRC_DATATYPE_MONO_FG_BG` into a second buffer pre-filled with `0x55`, and
reads the result back. `MonoFromMemory` reports `1` (supported): every
expanded byte was one of the two pens and the guard value was gone.

The probe also pins the ordering, which is the detail the earlier prototypes
foundered on:

| Source byte | Expanded bytes | Meaning |
|---|---|---|
| `0xAA` | `00 FF 00 FF` | not MSB-first |
| `0xC0` | `00 00 00 00 00 00 FF FF` | **LSB-first, normal pen polarity** |

`GMC_BYTE_PIX_ORDER` (bit 14) was measured to make **no difference** to a
memory source: both settings produced identical output. Amiga templates are
MSB-first, so staging must reverse the bits in each byte, which a 256-entry
table does in about four extra cycles per byte.

Expected result per template blit, at the measured 94.2 longwords:

| | per longword | per blit |
|---|---:|---:|
| Today, host-data stream | 1.33 us | 126 us |
| Staged: aperture write + bit reversal | ~0.93 us | **~85 us** |

That is roughly a third off the single largest cost in the driver, and about a
quarter off total driver PCI time. It is smaller than the naive 2.19x ratio
suggests because the bit reversal is not free.

The remaining work is a well-specified engineering task rather than a gamble:
a VRAM staging buffer, row packing with bit reversal and `XOffset` handling,
`MONO_FG_BG` for JAM2 and `MONO_FG_LA` for JAM1, an aperture read as the HDP
barrier before submitting, and the existing scissor and fallback boundary
retained unchanged.

### Staged expand implemented, and it wedges the engine

The submission path was written against the probe result: a 64 KiB staging
buffer hidden by lowering `bi->MemorySize` in `RadeonInitializeAcceleration()`,
rows packed through the existing `bfextu` extraction with a 256-entry byte
reversal, an aperture read as the HDP barrier, and a twelve-register expand
with `DP_SRC_SOURCE_MEMORY`. It is selected by `TEXTSTAGE`, falls back to the
host-data path whenever staging is refused, and both builds compile clean.

**On hardware it does not work.** A paired test, two release cards differing
only in the `TEXTSTAGE` default:

| Build | Result |
|---|---|
| `TEXTSTAGE=NO` (`5E6FE3A3`) | boots, 1024x768x8, full 16bpp suite passes |
| `TEXTSTAGE=YES` (`FF775D2B`) | Amiga reachable but unresponsive; every operation appears to hit the engine timeout |

An earlier DEBUG card with staging on came up with Workbench on a native
640x256 screen instead, so Picasso96 had failed outright. Note that the first
comparison of this was invalid: it changed DEBUG-versus-release *and* the
staging flag at once. Only the release-versus-release pair above is evidence.

The default is therefore `TEXTSTAGE=NO` and the driver ships the host-data
path. The capability itself is not in question - the probe expands a
memory-sourced mono bitmap correctly and repeatably - so the fault is in this
submission path, not in the hardware feature.

What has been ruled out: the code merely being linked in (the `TEXTSTAGE=NO`
card contains all of it and is healthy), and any crash (`last_crash` is empty
in every failing run).

The next step is to widen the probe rather than iterate on the live callback,
because the live path costs a boot cycle per attempt and can leave the machine
unusable. The probe already runs bounded and recoverable at init; extending it
to walk the parameters that separate the working case from the failing one -
multiple rows, a destination carrying `XBias`/`YBias`, a non-64 source pitch,
and RGB565PC/BGRA32 destinations - will identify which one wedges the engine
without risking the boot.

### What this leaves

Text is correctly accelerated and still costs about 130 us per call because
every glyph row crosses PCI as individual register writes. Removing that
requires not sending glyph bits through MMIO at all: cache the expanded font in
VRAM and colour-expand from memory with `DP_SRC_SOURCE_MEMORY` plus
`GMC_SRC_DATATYPE_MONO_FG_BG`, which is a materially different submission path
rather than another register-sequence adjustment. That, and the per-operation
full-FIFO drain in `WaitIdleAndFlush` (`WaitFifo(bi, 64)` after every single
operation), are the two remaining structural costs.

Also visible in the same run and not yet addressed: every `BlitPattern` call
took the software fallback, and every `DrawLine` call took the software
fallback.

## Current Gaps

| Operation | Closed driver | Validated driver | Remaining gap |
|---|---:|---:|---:|
| RectFill | 11881 | 10688 | 1.11x |
| RectFill Pattern | 11246 | 1387 | 8.1x |
| WritePixel | 134097 | 129429 | 1.04x |
| WriteChunkyPixels | 133 | 131 | 1.02x |
| WritePixelArray8 | 132 | 130 | 1.02x |
| WritePixelLine8 | 9034 | 8260 | 1.09x |
| DrawEllipse | 6543 | 4999 | 1.31x |
| DrawCircle | 7680 | 5465 | 1.41x |
| Draw | 19788 | 1276 | 15.5x |
| Draw Hor/Ver | 24543 | 4030 | 6.1x |
| ScrollRaster X | 1472 | 213 | 6.9x |
| ScrollRaster Y | 1555 | 215 | 7.2x |
| PutText | 8553 | 1018 | 8.4x |
| BlitBitMap | 13673 | 3416 | 4.0x |
| BlitBitMapRastPort | 7586 | 2597 | 2.9x |

CPU-written operations are already within about eight percent of the closed
driver. The remaining large gaps correspond to missing P96 callbacks or to
per-operation MMIO setup, not basic PCI framebuffer bandwidth.

## Version 0.12 RectFill Result

The benchmarked card with CRC32 `85C2D325` (30,696 bytes) produced a focused
RectFill sample of **10688 operations/second** at 640x480x16 for 13 seconds. It
differs from the final v0.12 artifact only in the embedded version. It is
preserved on the Amiga as
`LIBS:Picasso96/Radeon9200.card.direct-fast-10688`.

This candidate combines two independently measured changes:

| Build | RectFill op/s | Time/op | Change |
|---|---:|---:|---:|
| Previous release-like baseline | 5167 | 193.54 us | - |
| Direct volatile MMIO | ~6700 | ~149.25 us | +29.7% |
| Direct MMIO plus 32-bit surface validation | 10688 | 93.56 us | +106.9% |

Direct MMIO removes the `openpci.library` `pci_inl`/`pci_outl` call from every
register access. Acceleration submission uses inlined unchecked loads and
stores against the already-validated `MemoryIOBase`, matching the closed
driver's endian-swapped `move.l` sequences. The solid-fill path no longer
reserves FIFO entries before writing, and `WaitBlitter` now performs the same
FIFO-empty and GUI-idle checks as the closed driver without a semaphore or an
extra destination-cache/HDP sequence.

The second gain came from replacing `ValidateSurface`'s per-call 64-bit bounds
arithmetic with overflow-safe 32-bit subtraction checks. Hardware coordinates
are capped at 8191 and pitch at 16320, which bounds the intermediate products.
The normal 1 KiB-aligned surface path also avoids address-bias division.

The result is 2.07x the 5167 baseline and within 10.1 percent of the closed
driver's 11881 result. The complete CLUT8/RGB565PC/BGRA32 readback and guard
sequence passed, including fill, invert, template, pattern, masks, overlap, and
cross-surface copy tests, followed by a successful return to CLUT8.

## Accepted Changes

### Cross-surface source copies

`BlitRectNoMaskComplete` opcode `$C` is accelerated for disjoint on-board
surfaces with independently validated source and destination pitches. The
initial same-pitch implementation improved:

- BlitBitMap: about 242 to 3483-3936 operations/second
- BlitBitMapRastPort: about 237 to 2573-2804 operations/second

CLUT8, RGB565PC, and BGRA32 cross-surface tests pass with both equal and unequal
pitches.

### Coalesced FIFO reservations

Fill and copy submission reserve all required FIFO entries with one hardware
status read instead of three:

- Fill: one reservation for 7 writes
- Copy: one reservation for 10 writes

Five-run focused copy averages improved from 53.8 to 51.6 ticks (about four
percent). Copy ordering remains unchanged. The fill path's later seven-write
sequence is documented below.

### Direct-color solid fills

P96 direct-color pens are framebuffer-byte-order values. The Radeon brush
register expects the GPU's little-endian numeric pixel, so RGB565PC pens require
a 16-bit byte swap and BGRA32 pens require a 32-bit byte swap. With this
conversion enabled:

- RectFill improved from 102 to 2011 operations/second (19.7x).
- Draw Hor/Ver improved from 2939 to 4193 operations/second.
- OpenWindow improved from 25 to 46 operations/second.
- MoveWindow improved from 72 to 184 operations/second.
- SizeWindow improved from 35 to 49 operations/second.

Exact red/cyan edge tests and eight-color readback pass in RGB565PC and BGRA32,
followed by a successful return to CLUT8.

### 8x8 monochrome pattern fills

The hardware path accepts only:

- `JAM2`
- `Pattern.Size <= 3` (1, 2, 4, or 8 rows)
- identical high and low bytes in every 16-bit source row
- source pattern memory outside the framebuffer aperture
- CLUT8, RGB565PC, or BGRA32 destinations
- validated in-VRAM destination rectangles

Everything else synchronizes and uses `BlitPatternDefault`. P96Speed's 16x4
motif is representable by the RV280 8x8 monochrome brush:

```text
8888
2222
8888
2222
```

The 8/16/32/8 hardware sequence passes distinct foreground/background/guard
pens, horizontal and vertical phase, the far rectangle edge, all four outside
guard edges, complete row order for accepted heights 1, 2, 4, and 8, and CLUT8
partial write masks. A true 16-pixel pattern verifies that rejected input waits
for pending hardware before using `BlitPatternDefault`. P96Speed `RectFill
Pattern` improved from 37 to 1387 operations/second (37.5x), independently
proving that the hardware callback is active.

## Superseded Experiment

An early attempt to remove the destination-cache flush and `WAIT_UNTIL` pair
from every solid fill was correct under synthetic queue stress but produced
RectFill results of 1460 and 1493, below that build's validated 2011 result. It
was reverted at the time. After the MMIO backend, hardware cursor, and HDP
completion path were finalized, paired builds differing only by these two
writes improved from 4933.3 to 5113.3 operations/second and passed the complete
correctness sequence. The later controlled result supersedes the early one.

## Experimental CP Validation

The optional R200 command-processor backend was validated on hardware with the
release card CRC32 `0F6ABAAC` (29,444 bytes). The active monitor icon was
`DEVS:Monitors/Radeon.info`, CRC32 `7C5A7ED6` (545 bytes), with
`OUTPUT=VGA`, `DMASIZE=2M`, and `CP=YES`.

Live driver-state inspection after a cold boot confirmed that the private DMA
arena and CP state were both allocated. The CP ring had GPU address
`03E00000`, its initial write pointer was `00000020`, and its `Ready` flag was
one. This distinguishes the run from earlier measurements that silently used
the direct-MMIO fallback because `CP=YES` was absent.

The complete 8/16/32/8 sequence passed fill, pattern, mask, overlapping copy,
cross-surface complete-copy, guard, and direct-color readback tests. No crash
was recorded. Results are retained on the Amiga as:

| Result | CRC32 | Size | Fill256 | ScatterFill4096 | Copy256 |
|---|---:|---:|---:|---:|---:|
| `Work:p96screen_cp_active_8.txt` | `0EA404EC` | 4,677 | 19 | 40 | 20 |
| `Work:p96screen_cp_active_16.txt` | `25D1A6FE` | 4,618 | 20 | 36 | 26 |
| `Work:p96screen_cp_active_32.txt` | `F79C348B` | 4,619 | 32 | 37 | 41 |
| `Work:p96screen_cp_active_8return.txt` | `06879411` | 4,677 | 22 | 39 | 20 |

Five focused P96Speed RectFill samples at 640x480x16 and 13 seconds were 1972,
1984, 1954, 1928, and 1934 operations/second. Their mean is 1954.4 and median
is 1954. The 56 op/s range is 2.9 percent of the mean. The final saved report
is `Work:P96Speed_cp_active_rectfill.txt`, CRC32 `59BCCC53` (1,889 bytes).

The CP-active mean is 24.6 percent below the previous focused direct-MMIO
result of 2593 operations/second and 81.7 percent below the 10693 target. CP is
therefore hardware-correct but does not meet the RectFill performance goal
under rtg.library's per-operation `WaitBlitter` serialization. The controlled
CP-resident/direct-MMIO comparison is recorded below.

### CP-resident MMIO 2D and hardware cursor

The CP now remains initialized for future 3D work, but Picasso96 fill, pattern,
and copy callbacks submit exclusively through direct MMIO. `AccelPending` is a
backend value rather than a Boolean, so `WaitBlitter` drains the backend that
actually submitted the work instead of choosing from CP readiness.

The real RV280 hardware cursor removes rtg.library's software-sprite overlap
save/restore path. With `CP=YES`, `HWSPRITE=YES`, and MMIO 2D, three focused
RectFill samples were 3446, 3431, and 3395 operations/second. Their mean was
3424 and their 51 op/s spread was 1.5 percent. The final report is
`Work:P96Speed_hybrid_mmio_rectfill.txt`, CRC32 `B580AA45` (1889 bytes).

Making zero the explicit driver-owned `HOST_PATH_CNTL` baseline then removed
two redundant reads from every HDP invalidate while retaining a final readback
barrier. Three repeats were 3501, 3496, and 3466 operations/second: mean 3487.7,
median 3496, and a 1.0 percent spread. This is a 1.9 percent improvement over
the otherwise identical hybrid build and 78.5 percent over the earlier 1954.4
CP-active mean. The final report is
`Work:P96Speed_hostshadow_rectfill.txt`, CRC32 `02DF03DB` (1889 bytes).

DEBUG counters prove the intended split during these runs: `CpActive=1`, every
fill used the hardware callback, `FillSoftware=0`, and each fill issued exactly
nine MMIO writes before the final submission optimization. In the instrumented
build the whole `RadeonFillRect` call averaged
61.3 us net and drain contribution averaged 44.4 us per fill, for about 105.7
us inside the driver. At 3487.7 operations/second the complete operation takes
286.7 us, leaving roughly 181 us above the driver callbacks.

The DEBUG hooks themselves bracket nested regions with `ReadEClock`, so release
performance must be measured separately. The exact pre-optimization release
artifact produced 4938, 4946, and 4916 operations/second, mean 4933.3 or 202.70
us per operation. Removing the redundant pre-trigger `DSTCACHE_CTLSTAT` and
`WAIT_UNTIL` writes reduced fill submission from nine to seven writes. The
same-build repeats were 5136, 5088, and 5116 operations/second, mean 5113.3 or
195.57 us per operation. The change saves 7.14 us per fill and improves
throughput by 3.65 percent. The final report is
`Work:P96Speed_v010_fill7.txt`, CRC32 `8865D258` (1889 bytes).

After changing only the embedded version to 0.11, the exact release artifact
produced 5080, 5083, and 5037 operations/second: mean 5066.7, median 5080, and a
0.91 percent spread. This is 2.2 percent above the published v0.10 mean of 4960
and 159.3 percent above the original CP-active mean. The saved exact-artifact
report is `Work:P96Speed_v011_final.txt`, CRC32 `57CECA86` (1889 bytes).

The final HDP sequence passed the full 8/16/32/8 fill, pattern, mask, overlap,
complete-copy, guard, and direct-color readback suite without a crash:

| Result | CRC32 | Size | Fill256 | ScatterFill4096 | Copy256 |
|---|---:|---:|---:|---:|---:|
| `Work:p96screen_v011_8.txt` | `00F2C01B` | 4675 | 3 | 33 | 7 |
| `Work:p96screen_v011_16.txt` | `B94C0AAA` | 4616 | 5 | 27 | 7 |
| `Work:p96screen_v011_32.txt` | `2E80F380` | 4619 | 10 | 26 | 15 |
| `Work:p96screen_v011_8return.txt` | `1E10C436` | 4675 | 4 | 34 | 6 |

The v0.11 release mean remained 52.6 percent below the 10693 target. At that
stage, the cursor and MMIO changes had removed the identified driver-side
penalties, but the remaining gap appeared dominated by
rtg.library/graphics.library work outside the callbacks.
Moving cursor state from a file-static singleton into per-board `CardData` did
not regress the hot path; the final DEBUG build produced a 3629 op/s smoke
sample after the complete readback sequence.

## Hardware Validation Record

- Current release card: CRC32 `BC559E27`, 33992 bytes. Cold-booted on the
  reference machine, full 16bpp `p96screen` sequence with no `FAIL` line and no
  recorded crash. Contains the unequal-pitch cross-surface copy, hardware
  cursor and hardware text defaults, and the burst FIFO reservation.
- Previous release card: CRC32 `E7E2F009`, 31204 bytes
- Current release result: `Work:P96Speed_v011_final.txt`, CRC32 `57CECA86`,
  1889 bytes
- Release card: `Radeon9200.card.0.9-pattern`, CRC32 `B5ABB580`, 23088 bytes
- Previous rollback: `Radeon9200.card.0.8-fill`, CRC32 `D2A76396`, 21776 bytes
- Test utility: CRC32 `85036F1D`, 24392 bytes
- Complete P96Speed run: `Work:P96Speed_10pattern1.txt`, CRC32 `C69AA4CB`
- Accepted result after focused repeats: `Work:P96Speed_10pattern2.txt`, CRC32
  `70D67A67`

The complete run measured pattern fill at 1387 operations/second and solid fill
at 1427 operations/second. Repeating the individual tests produced 1387 and
2593 respectively. After a clean reboot, fully fenced `p96screen` complete-copy
timings were 41-45 ticks and fill timings were 18/21/41 ticks for CLUT8,
RGB565PC, and BGRA32, so there is no solid-fill or copy regression. Focused
timings taken immediately after P96Speed were temporarily much higher; reboot
before comparing these synthetic timings. Workbench returned normally after
every mode transition, and no crash was recorded.

## RectFill Performance Plan

### Where the time actually goes

RectFill is the largest remaining gap after the direct-colour and pattern work.
Measured at 640x480x16, 13 seconds, five focused runs on one boot:

| Run | 1 | 2 | 3 | 4 | 5 |
|---|---:|---:|---:|---:|---:|
| RectFill op/s | 1972 | 1984 | 1954 | 1928 | 1934 |

Mean 1954 op/s, spread 2.9 percent, saved as `Work:P96Speed_cp_active_rectfill.txt`.
That is 512 microseconds per operation against the closed driver's 11881 op/s,
or 84 microseconds. About 430 microseconds per operation is unexplained
overhead. The earlier 2593 figure in the gap table above has not been
reproducible on any subsequent boot and should be treated as an outlier, not a
regression baseline.

At that stage each RectFill issued roughly:

- `WaitFifo(9)` — at least one `RBBM_STATUS` read
- nine register writes to program the fill
- `RadeonWaitBlitter` to `WaitIdleAndFlush` — three poll loops over
  `RBBM_STATUS` and `DSTCACHE_CTLSTAT`, one flush write, then the HDP
  read-buffer invalidate at three reads and two writes

The working hypothesis is that PCI MMIO **reads** dominate. Reads are not
posted, so each one pays full bridge turnaround, while writes are posted and
comparatively free. Thirty to sixty reads per operation at a few microseconds
each accounts for the missing 430 microseconds. The competing hypothesis is that
the idle-and-flush handshake has an irreducible hardware latency. These are
distinguishable only by measurement, and everything below Phase 0 is contingent
on which one is true.

### Two null results to learn from

Both were changes made without the ability to measure what they targeted:

- The earliest CP-labelled runs did not have an active ring. It requires
  `data->DmaArena`, which requires `DMASIZE` in the active monitor icon. See
  [`tooltypes.md`](tooltypes.md). Those early results measured the direct-MMIO
  fallback; later `Radeon9200.Debug` statistics distinguish the backends.
- Removing the per-iteration `RadeonDelayUs(1)` from the `WaitFifo` and
  `WaitIdleAndFlush` poll loops produced no improvement. This is consistent with
  the drain being bounded by real GPU and bus time rather than by poll overhead:
  dropping the delay makes the loop spin more times at the same wall-clock cost,
  and burns PCI bandwidth doing it. The change is currently in the tree but is
  **unvalidated**; re-test it under Phase 0 instrumentation and revert it if it
  is still null.

### Phase 0 results

Implemented in `src/radeon_debug.{c,h}`: a DEBUG-only `PA_IGNORE` public port
named `Radeon9200.Debug` with a 124-byte stats block at port + 34, read from the
host by walking exec's PortList and decoded with
`tools/decode_debug_stats.py`. The release card is byte-identical with and
without the instrumentation (CRC32 `0F6ABAAC`), so it costs the shipping build
nothing.

First read, after a boot with the icon in place:

| Field | Value |
|---|---:|
| CpRequested / CpActive | 1 / **1** |
| DmaRequested / DmaReserved | 2097152 / 1 |
| BoardMemorySize | 65011712 |
| EClockRate | 709379 |

ToolType delivery works and **the CP is running for the first time**.
`BoardMemorySize` matches the `dmasize=2048k` row in [`dmasize.md`](dmasize.md)
exactly, which independently confirms the arena.

Measured costs, 2000 samples each:

| Operation | Cost |
|---|---:|
| MMIO read (`RBBM_STATUS`) | 2.73 us |
| MMIO write (`SCRATCH_REG1`) | 2.29 us |
| `ReadEClock` | 10.44 us (instrumentation only) |

**Posted writes are not cheap on this bridge.** A write costs 2.29 us against a
read's 2.73 us, only 16 percent less. The working hypothesis above — that
non-posted reads dominate and writes are comparatively free — is wrong.
Eliminating a write is worth almost as much as eliminating a read, which raises
the priority of the register shadowing in Phase 3.

Per-operation breakdown with the CP active, net of instrumentation:

| Stage | Time | Counted MMIO | Counted MMIO cost |
|---|---:|---|---:|
| Fill submit | 98.9 us | 2 reads, 1 write | 7.7 us |
| Drain (real, excluding no-ops) | 41.8 us | 4 reads, 2 writes | 15.5 us |

The fill submit is the problem, and it is not MMIO. About **91 us per fill is
unaccounted for inside `CpCommit()`**, because the two most expensive things it
does bypass `RadeonRead32`/`RadeonWrite32` and so are not counted:

1. the `pci_inl()` readback of the last ring dword (`src/radeon_cp.c:174`), a
   VRAM read back through the aperture, done purely as paranoia;
2. `host_to_pcicpy()` of 64 bytes, because `CP_RING_ALIGNMENT` pads every
   submission to 16 dwords when a fill needs 14.

For comparison, the final direct-MMIO submit path is seven writes plus one
`WaitFifo` read. **CP submission is substantially more expensive than MMIO
submission**, which explains why enabling the CP did not help. The ring is not
wrong in principle; it is being throttled by a readback and an oversized copy
on every single operation.

`DrainCount` is roughly twice `FillCount` because `SynchronizeEngine()` returns
immediately when `AccelPending` is false; the table above divides by the drains
that actually did work. The drain's 4 reads are one scratch poll plus the
three-read HDP invalidate, so shadowing `HOST_PATH_CNTL` alone saves 8.2 us per
operation.

#### Attribution during a real P96Speed run

The numbers above came from boot-time Workbench drawing. Sampling the counters
immediately before and after a focused 13-second P96Speed RectFill, on the
instrumented debug card with the CP active, gives the benchmark's own mix.
P96Speed reported **1992 op/s** and the driver counted 24840 fills over roughly
12.5 seconds, so every counted fill is benchmark work.

| Quantity | Per operation |
|---|---:|
| Elapsed (1992 op/s) | 502 us |
| `RadeonFillRect`, whole call | 101.1 us |
| `WaitBlitter` drain | 36.5 us |
| **Driver total, net** | **137.6 us** |
| Driver total, gross (adds ~41 us of `ReadEClock`) | ~179 us |

`FillSoftware` was **0**: every benchmark fill took the hardware path, so the
software-fallback theory is dead.

The important result is what is missing. The driver accounts for 36 percent of
elapsed time at most, which leaves **about 320 us per operation, roughly two
thirds, above our callbacks** in rtg.library and graphics.library. The closed
driver completes an entire RectFill in 84 us, which is far less than the time we
spend outside our own code. That overhead therefore cannot be intrinsic to
Picasso96 — something about how this driver presents itself is making rtg take a
slow path.

This caps every optimisation discussed above. Making `RadeonFillRect` and the
drain infinitely fast would move RectFill from 1992 to roughly 2900 op/s, still
four times short of 11881. **Micro-optimising the fill path cannot close this
gap.**

The first place to look is what `RadeonInstallCallbacks()` advertises
(`src/radeon_mode.c:851-856`): the driver clears `BIF_BLITTER`,
`BIF_HARDWARESPRITE`, `BIF_HASSPRITEBUFFER`, `BIF_VBLANKINTERRUPT`,
`BIF_INDISPLAYCHAIN` and others, and sets only `BIF_GRANTDIRECTACCESS` before
conditionally re-adding `BIF_BLITTER`. Capability flags and the set of
implemented callbacks determine which generic paths rtg emulates around every
operation. That investigation, previously Phase 4, is now the only work with
enough headroom to matter.

Revised priority based on this data:

1. **Find out why rtg.library spends ~320 us per operation above the driver.**
   Start with the capability flags and the set of installed callbacks. Nothing
   else can close a 6x gap.
2. Only then, inside the fill path: shadow `HOST_PATH_CNTL` (8.2 us per
   operation) and shadow the setup registers, which is worth more than
   previously assumed because writes cost nearly as much as reads.

Two items from the earlier draft are now retired:

- Dropping the `pci_inl()` ring readback is **not safe**. The ring lives in VRAM
  behind the aperture, so host writes traverse the card's HDP write path while
  the `CP_RB_WPTR` write goes down the register path. The aperture read is what
  drains HDP before the write pointer is bumped. It is a barrier, not paranoia.
- Shrinking `CP_RING_ALIGNMENT` buys nothing at 4 or 8, because a 14-dword
  payload rounds up to 16 either way. Only alignment 2 helps, worth about 4.6 us
  against an unquantified hardware risk.

More fundamentally, CP submission must write more dwords (8 for the fill plus 6
for the fence) than the MMIO path writes registers (7). At roughly 2.5 us per
PCI access in either direction, **the CP cannot beat direct MMIO for
per-operation-fenced work while the ring sits behind the aperture**. It would
need the ring in Fast RAM via PCI GART, which is a large project and is not
justified until the rtg overhead above is understood.

### Phase 0 — Observability, before any more optimisation

Three test rounds have been invalidated by not knowing what was running. Close
that first.

1. Report board `MemorySize` from `tools/p96screen.c` via the Picasso96 API. A
   2 MiB drop is direct proof that `DMASIZE`, and therefore ToolType delivery,
   is live — checkable without a reboot.
2. Publish CP status as a DEBUG-only public exec message port, created only when
   `RadeonCpInitialize()` succeeds. Use `PA_IGNORE` with no signal bit so it is
   safe to create from whatever task calls `InitCard()`, and remove it in
   `RadeonCpShutdown()`. It is then visible instantly over the bridge.
3. Time MMIO from inside the driver: N `RadeonRead32(RBBM_STATUS)` and N
   `RadeonWrite32` against `EClock`. This converts the read-cost hypothesis into
   a number and sets the budget for everything in Phase 1.
4. Instrument `RadeonFillRect` to accumulate read count, write count, and
   `EClock` ticks split between submit and drain, exposed through a memory
   region the bridge can read.

Exit criterion: a per-operation cost breakdown in microseconds. Do not start
Phase 1 without it.

### Phase 1 — Remove MMIO reads, if Phase 0 confirms they dominate

1. Software FIFO accounting, as the Xorg driver does. Track free slots in
   `RadeonBoardData` and only read `RBBM_STATUS` when the estimate is short.
   Saves the read in every fill submit.
2. Shadow `HOST_PATH_CNTL`. It is driver-owned and constant after init, so the
   HDP invalidate can drop from three reads and two writes to two writes.
3. Collapse the remaining drain polls. The `DC_BUSY` wait may already imply the
   `RBBM_ACTIVE` wait; confirm against the Xorg sequence before merging them.

### Phase 2 — Make completion observable without a PCI read

This is the structural fix and the strongest technical argument for finishing
the CP work. The drain has to observe GPU completion, and today it does so with
the most expensive operation available, repeated in a loop.

1. Enable scratch write-back. `RADEON_SCRATCH_ADDR` and `RADEON_SCRATCH_UMSK`
   make the GPU DMA its scratch registers into host memory. Point them at a Fast
   RAM buffer and the fence check becomes a normal CPU memory read of a few
   nanoseconds instead of a multi-microsecond PCI read. `CpConfigureRing()`
   currently sets both to 0, so write-back is disabled
   (`src/radeon_cp.c:195-197`).
2. Enable ring read-pointer write-back via `CP_RB_RPTR_ADDR`, removing the
   `CP_RB_RPTR` read in `CpReserve()`.
3. Drop the paranoid `pci_inl()` ring readback in `CpCommit()`
   (`src/radeon_cp.c:174`) and track the write pointer purely in software.

Note that this reframes the CP: its value on this platform is not batching,
which P96's per-operation `WaitBlitter` denies us anyway, but replacing an
expensive MMIO poll loop with a cheap Fast RAM poll. Keep the ring itself in
VRAM so the GPU fetches locally; only the write-back targets belong in Fast RAM.

### Phase 3 — Reduce setup writes

Lower value than Phase 2 because writes are posted, but cheap to do.

1. Use `DEFAULT_PITCH_OFFSET` and the default scissor registers for fills whose
   pitch and offset are unchanged, dropping `DST_PITCH_OFFSET` from the
   per-operation sequence.
2. Shadow `DP_GUI_MASTER_CNTL`, `DP_WRITE_MASK`, `DP_CNTL` and the brush colour,
   and skip writes when unchanged. P96Speed's RectFill loop holds pen and format
   constant, so most of the seven writes are redundant.

### Phase 4 — Question the per-operation fence

Confirm from the CardDevelop documentation whether `WaitBlitter` after every
`RectFill` is required by the P96 contract or is a consequence of the capability
flags this driver advertises in `BoardInfo`. If the fence can legitimately be
deferred to the point of CPU framebuffer access, that removes the drain from the
hot path entirely and dominates every other item here. Treat any change in this
area as correctness-critical. The earlier broad barrier-removal experiment
regressed, while the later paired retest accepted only the validated
seven-write sequence described above.

## Remaining Performance and Callback Plan

The release driver still reaches about 5067 RectFill operations/second against
the closed driver's 11881. Assembly versus C cannot explain that gap by itself:
the measured callback and drain costs leave substantial time in
graphics.library and rtg.library. Missing callbacks may improve the operations
they implement, but installing one is not evidence that rtg.library will change
its RectFill dispatch. Keep those two investigations separate and measure each
change against the same release benchmark.

1. **Compare BoardInfo presentation**: capture flags, format masks, sprite state,
   callback addresses, and relevant bitmap attributes for this driver and the
   closed driver. Change one advertised capability at a time; do not claim
   unsupported hardware merely to select a faster generic path.
2. **Measure the library-side interval**: retain a release-like timing build
   that brackets the caller-visible operation, `RadeonFillRect`, and
   `RadeonWaitBlitter`. This determines whether a callback or flag change
   actually removes work above the driver.
3. **InvertRect**: implemented with destination-only `ROP3_Dn`, the validated
   rectangle surface checks, and the existing fallback boundary. The real
   RV280 passes edge and guard tests in CLUT8/RGB565PC/BGRA32, CLUT8 partial
   masks, and a final CLUT8 state-restoration pass. P96Speed has no dedicated
   row for it.
4. **BlitTemplate**: two real-hardware prototypes were rejected. A 13x8
   host-data path delivered incorrect asserted bits and took 52 ticks per 4096
   operations versus the software default's 26. An 8x8 monochrome-brush path
   made JAM2 correct after LSB-first row packing, but JAM1 transparency failed
   and the paired benchmark took 39 ticks versus software's 16. The callback is
   therefore not advertised; future work requires a materially different
   submission path, not another small register-sequence adjustment.
5. **BlitPlanar2Chunky/Direct**: add only after defining a bounded upload path
   and focused correctness tests. Until then, preserve the software defaults.
6. **DrawLine**: use the Radeon line engine and the now-proven direct-color pen
   conversion. This targets Draw and may also improve circles and ellipses.
7. **Reduce hot-path overhead after measurement**: inspect generated 68k code,
   cache validated surface layout where ownership permits, account FIFO slots in
   software, and shadow unchanged setup registers. Do not attribute PCI latency
   to C call overhead without a paired measurement.
8. **Copy and pattern extensions**: add copy hazard tracking, broader minterms
   and pitches, and JAM1/complement/true 16-pixel patterns without weakening the
   validated fallback boundary.

`EnableSoftSprite` is also installed with the hardware-cursor callbacks. It
requests software rendering only when cursor allocation is absent or the
requested formats intersect `SoftSpriteFlags`; no mode-specific RV280 cursor
restriction is currently known within the driver's advertised mode limits. The
real machine completed the CLUT8/RGB565PC/BGRA32 screen cycle and full
acceleration tests after installing it.
