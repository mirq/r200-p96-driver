# Moving-Window Benchmark

`p96windowmove` measures Intuition and layers.library work caused by moving a
real AmigaOS window over another window. It complements `p96overlap`, which
keeps its window geometry fixed and times application drawing.

The reference target is a physical Amiga with an RV280 card, not an emulator.
Do not use emulator lifecycle or reset controls during deployment or recovery.

For paired driver runs, switch both monitor ToolTypes before cold rebooting.
Radeon9200 requires `BOARDTYPE=Radeon9200` together with
`SETTINGSFILE=SYS:Devs/Picasso96Settings.9200`; the closed `Radeon.card` must
use its matching settings file. Changing only `BOARDTYPE` can leave Workbench
on a native fallback screen and invalidate mode availability.

The tool opens a private `1024x768x16` Picasso96 high-color screen and uses normal bordered,
smart-refresh Intuition windows with title, drag, depth, and close gadgets. A
320x220 mover travels 400 pixels right and left along a fixed path. The same
movement is measured in two states:

- `empty`: the mover is the only window on the screen;
- `overlap`: the mover crosses a populated 784x548 target window.

Window creation and initial fill/text/bar drawing are outside the timed
interval. Every `MoveWindow()` is followed by `WaitBlit()`. Each reported value
is the median of seven trials after two warmups. The default trial performs four
round trips, or eight individual moves. Across warmups and measured trials that
is 72 moves per state and 144 moves total.

The earlier 24-round-trip default was excessive at this resolution: sustained
smart-refresh work could monopolize the physical machine long enough for
AmigaBridge to become unreachable. Use larger values only as an explicit stress
test, not for routine driver comparison.

## Build And Run

```sh
make tools
```

Deploy `build/p96windowmove` and run:

```text
Work:p96windowmove >Work:p96windowmove-result.txt
```

The optional argument changes the number of round trips per trial and is capped
at four. Keep the default for driver comparisons. Compare `us_per_move`, the
overlap ratio, and the absolute overlap penalty using the exact same executable
and display mode.
The result includes Picasso96's numeric `RGBFormat`; paired runs should use the
same format where both drivers advertise it.

Version 4 also reports aggregate timing so stalls cannot be hidden by the
median:

- `TOTAL whole_us` covers mode lookup, screen/window setup, drawing, warmups,
  and both measured movement phases, ending immediately after the last move.
- `TOTAL empty_us` and `overlap_us` include two warmups plus seven trials.
- `empty_measured_us` and `overlap_measured_us` sum the seven reported trials.
- `AVERAGE *_loop_us` is the average round-trip cycle across warmups and trials.
- `AVERAGE *_trial_us` is the average of the seven measured trials.

Version 5 adds `STAGES` timing for mode lookup, private-screen open, each
window open, each initial `DrawWindow()`, and closing the empty-phase mover.
Together with the movement totals, these fields attribute the whole benchmark
without relying on subtraction.

Version 6 caps initial text population at three rows per window. Earlier
versions filled nearly every 13-pixel row, causing Radeon9200's target setup to
take roughly two minutes before the already-capped movement test began. Fill,
narrow bars, smart refresh, normal borders, window sizes, and movement geometry
are unchanged. The output records `text_rows=3` so results cannot be confused
with the heavier versions.

Version 7 reduces repetition to three measured trials with no warmups and one
round trip per trial. Each run still measures both the empty-screen and overlap
cases and reports their median, but performs 6 moves per case instead of the 72
moves used by versions 5 and 6. Combined with the version 6 text-row cap, this
keeps the slow Radeon9200 benchmark near the 15-second target.

## Version 7 Complete-Copy Fix

Three safe Radeon9200 runs reproduced `overlap_open_us` at 9.539-9.572 seconds;
three Devil's Cut runs took 10.916-11.574 ms. DEBUG versions 9-11 ruled out FIFO
timeouts, idle timeouts, and engine recovery. Direct phase timing showed that
the generic `BlitRectNoMaskCompleteDefault` path consumed essentially the whole
delay. Its workload used complete-copy opcode `$6` (source XOR destination),
which Radeon9200 did not accelerate, plus physically overlapping surfaces.

The validated fix maps opcode `$6` to Radeon ROP3 `S XOR D` and chooses copy
direction from the absolute source/destination VRAM ranges when they overlap.
Opcode `$C` source copy remains supported, and all other complete-copy opcodes
still fall back. Release card CRC32 `B6239209`, 35,168 bytes, produced three
separate byte-identical runs:

| Metric | Before | Fixed release | Devil's Cut |
|---|---:|---:|---:|
| Whole benchmark | 9.693-9.732 s | **177.773 ms** | 124.193-125.113 ms |
| Open mover over target | 9.539-9.572 s | **11.727 ms** | 10.916-11.574 ms |

The overlap-open stage improved about 815x and is now within the measured
Devil's Cut range. The release build passed the RGB565 full suite; the same
source passed DEBUG CLUT8/RGB565/BGRA32/CLUT8-return tests including XOR
start/end pixels, untouched edge, unchanged source, and all existing guards.

## Version 3 Median Result

Version 3 executable CRC32 `C0C2D18D`, `1024x768x16`, RGB format `4`, four
round trips per trial:

| Driver | Empty median | Overlap median | Empty per move | Overlap per move | Ratio |
|---|---:|---:|---:|---:|---:|
| Radeon9200 `5E6FE3A3` | 2.334 ms | 2.366 ms | 291 us | 295 us | 1.013x |
| Devil's Cut `1F479254` | 2.423 ms | 2.445 ms | 302 us | 305 us | 1.009x |

Radeon9200 was 3.7 percent faster empty and 3.2 percent faster while crossing
the target. The overlap penalty itself was small for both drivers: 32 us per
eight-move trial for Radeon9200 and 22 us for Devil's Cut. This movement path
does not reproduce the large fixed-window text-redraw gap measured by
`p96overlap`.

This conclusion is superseded by version 4's whole-benchmark timing below. The
version 3 median covered only `MoveWindow()` calls and hid almost all elapsed
time in setup and initial window rendering.

Both runs contained occasional long trials despite stable medians. Radeon9200's
largest measured empty/overlap trials were 4,242/18,639 ticks; Devil's Cut's
were 1,868/11,437 ticks. AmigaBridge can disconnect while one of these
synchronous Intuition/layers operations is in progress, then return after the
benchmark finishes. Use medians and do not interpret a temporary bridge loss as
a crash; neither paired run recorded one.

Saved output files:

- `Work:p96windowmove-1024-radeon9200-v3-4cycles.txt`, CRC32 `DD85EC57`
- `Work:p96windowmove-1024-devilscut-v3-4cycles.txt`, CRC32 `EC71CAD6`

## Version 4 Whole-Time Result

Version 4 executable CRC32 `6D7417ED`, same `1024x768x16` RGB format 4 and
four-cycle workload:

| Metric | Radeon9200 `5E6FE3A3` | Devil's Cut `1F479254` |
|---|---:|---:|
| Whole benchmark | **127.555233 s** | **0.182421 s** |
| Empty phase, warmups + trials | 20.471 ms | 19.564 ms |
| Overlap phase, warmups + trials | 26.998 ms | 22.764 ms |
| Seven empty trials | 18.323 ms | 17.415 ms |
| Seven overlap trials | 16.656 ms | 17.880 ms |
| Average empty round trip | 568 us | 543 us |
| Average overlap round trip | 749 us | 632 us |
| Time outside movement phases | **127.507764 s** | **0.140093 s** |

Devil's Cut completes the whole benchmark about **699 times faster**. Movement
submission itself is close: Radeon9200 is 4.6 percent slower for the aggregate
empty phase and 18.6 percent slower for the aggregate overlap phase. Almost the
entire whole-process difference occurs before the overlap movement timing,
during mode lookup, private-screen/window creation, initial fill/text/bar
drawing, and the transition from the empty mover to the populated target.

This is consistent with `p96overlap`: Radeon9200's expensive text-rendering and
refresh path dominates real window setup, while timing only subsequent window
coordinates misses it. Further benchmark attribution should split whole time
at screen open, each window open, each `DrawWindow()`, and window close.

Saved output files:

- `Work:p96windowmove-1024-radeon9200-v4.txt`, CRC32 `304643F5`
- `Work:p96windowmove-1024-devilscut-v4.txt`, CRC32 `36CEACBB`
