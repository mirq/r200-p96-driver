# Overlapping-Window Benchmark

`p96overlap` measures the specific performance problem that synthetic drawing
tests miss: layers.library splitting one render across visible and obscured
`ClipRect`s.

The tool opens a private `640x480x16` Picasso96 screen and a 592x432
smart-refresh target window. It measures the same fill, text, and narrow-fill
workload in two states:

- the target has one unobscured `ClipRect`;
- three fixed borderless windows divide it into visible and backing-store
  regions.

Window creation is outside the timed interval. Each trial starts and ends with
`WaitBlit()`, uses EClock, and the reported result is the median of seven
trials after two warmups. The target layer is locked while its `ClipRect`s are
counted.

## Build And Run

```sh
make tools
```

Deploy `build/p96overlap` and run:

```text
Work:p96overlap >Work:p96overlap-result.txt
```

The optional argument changes the render iterations per trial. The default is
12; use the default for driver comparisons.

## Current Baseline

Safe Radeon9200 card `5E6FE3A3`, `1024x768x8` Workbench, benchmark screen
`640x480x16`, 12 iterations:

| Run | Empty median | Overlap median | Ratio | Penalty |
|---|---:|---:|---:|---:|
| 1 | 256.802 ms | 337.529 ms | 1.314x | 80.726 ms |
| 2 | 258.767 ms | 345.964 ms | 1.336x | 87.197 ms |

Both runs reported one visible empty `ClipRect`, then 19 overlapping
`ClipRect`s: 10 visible and 9 obscured/backing-store regions. Empty medians
differed by 0.8 percent and overlap medians by 2.5 percent.

## Devil's Cut Comparison

Devil's Cut `Radeon.card 0.666`, CRC32 `1F479254`, was cold-booted with the
same `1024x768x8` Workbench mode. Version 1 of the benchmark initially rejected
its board name, `RV280-PrmPCI`; the selector was extended to accept that exact
name without changing the workload or timed path. The revised executable has
CRC32 `66A1614D`.

| Driver/run | Empty median | Overlap median | Ratio | Penalty |
|---|---:|---:|---:|---:|
| Radeon9200 1 | 256.802 ms | 337.529 ms | 1.314x | 80.726 ms |
| Radeon9200 2 | 258.767 ms | 345.964 ms | 1.336x | 87.197 ms |
| Devil's Cut 1 | 203.511 ms | 276.183 ms | 1.357x | 72.672 ms |
| Devil's Cut 2 | 205.805 ms | 277.789 ms | 1.349x | 71.984 ms |

The layer geometry matched exactly: one visible empty `ClipRect`, then 19
overlapping rectangles split into 10 visible and 9 obscured. Devil's Cut was
about 20.6 percent faster in the empty state and 19.0 percent faster in the
overlapped state, but its mean overlap multiplier was slightly worse (about
1.353x versus 1.325x). It therefore does not avoid layers.library's
fragmentation. Its advantage is a broadly cheaper rendering workload, not a
different overlap dispatch.

The Radeon9200 numbers were produced by the previous executable CRC32
`560250BA`. The only timed-path-independent changes in `66A1614D` are accepting
the closed driver's board name and saturating a negative displayed delta at
zero. Re-run Radeon9200 with `66A1614D` before treating small differences as a
release decision; the approximately 20 percent gap is much larger than the
observed variance or those changes.

### Version 2 attribution

Version 2 retains the combined workload and geometry, and additionally times
its three components independently. Executable CRC32 `D6803586`:

| Devil's Cut run | Empty fill | Overlap fill | Empty text | Overlap text | Empty bars | Overlap bars |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 3.376 ms | 8.623 ms | 189.716 ms | 234.185 ms | 8.789 ms | 21.881 ms |
| 2 | 3.380 ms | 8.496 ms | 187.674 ms | 238.206 ms | 8.999 ms | 21.709 ms |
| Radeon9200 1 | 3.536 ms | 9.563 ms | 247.023 ms | 302.870 ms | 10.888 ms | 28.014 ms |
| Radeon9200 2 | 3.553 ms | 9.574 ms | 248.296 ms | 307.062 ms | 10.892 ms | 26.514 ms |

Text consumes about 94 percent of Devil's Cut's empty combined workload and
84-86 percent of its overlapped workload. Radeon9200's mean isolated text time
is 31.3 percent slower empty and 29.3 percent slower overlapped. Its full fills
are 5.2 and 11.9 percent slower, while narrow fills are 22.9 and 25.1 percent
slower. The text gap is 58.96 ms empty and 68.77 ms overlapped, accounting for
essentially all of the 58.94 ms empty combined gap and about 91 percent of the
75.82 ms overlap combined gap. `BlitTemplate` is therefore the primary next
optimization target; rectangle submission is secondary.

### Repeated `HOST_DATA0` prototype

Static analysis corrected the closed-driver comparison: Devil's Cut's
`BlitTemplate` ends at `0x3552`; the checks previously attributed to it at
`0x3568` are the following `BlitPattern`. Its template loop writes every word
directly to `HOST_DATA0` without polling `RBBM_STATUS` during the upload.

Radeon9200 prototype `A1EAEE2E` adopts only that submission behavior. It keeps
the existing validation, clipping, cached setup, bounded setup reservation,
tail extraction, engine synchronization, and fallback handling. Two runs with
the same benchmark binary:

| Prototype run | Combined empty | Combined overlap | Text empty | Text overlap |
|---|---:|---:|---:|---:|
| 1 | 241.836 ms | 323.178 ms | 218.042 ms | 281.199 ms |
| 2 | 239.695 ms | 311.580 ms | 220.416 ms | 267.799 ms |

Against the safe Radeon9200 means, isolated text improved by 11.5 percent empty
and 10.0 percent overlapped. Combined redraw improved by 7.7 and 10.0 percent.
Geometry remained one empty `ClipRect` and 19 overlapping `ClipRect`s split
10 visible/9 obscured. The full 8/16/32/8 `p96screen` sequence passed without a
`FAIL`, including JAM1/JAM2 template guards, and no crash was recorded.

The prototype closes about 39 percent of the empty text gap and 44 percent of
the overlap text gap to Devil's Cut. Remaining mean text gaps are 30.5 ms empty
and 38.3 ms overlapped, so host-data write cost and setup outside FIFO polling
remain relevant.

## Paired Driver Procedure

1. Cold-boot the selected card with the same monitor ToolTypes and Workbench
   mode.
2. Do not leave animation, status, or benchmark windows running on the private
   benchmark screen.
3. Run the unmodified benchmark twice and retain both output files.
4. Cold-boot the comparison card and repeat with the same executable.
5. Compare empty time, overlap time, ratio, and `ClipRect` counts separately.

Matching `ClipRect` counts but different elapsed times attribute the difference
below layers.library, in P96 dispatch or the driver callbacks. Different counts
mean the two configurations are not exercising the same layer geometry and the
timings are not directly comparable.
