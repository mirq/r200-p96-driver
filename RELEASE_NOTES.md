# Radeon9200.card 0.11

Version 0.11 is a focused performance update to 0.10. It removes two redundant
pre-trigger synchronization writes from each direct-MMIO solid fill while
retaining the post-operation `WaitBlitter` completion fence.

## Highlights

- Reduces solid-fill submission from nine to seven MMIO writes, improving the
  focused RectFill mean by 3.65 percent in an otherwise identical build.
- Retains the R200 CP ring and microcode in private VRAM reserved with
  `DMASIZE`, with bounded fallback and recovery.
- Uses direct MMIO for Picasso96 fill, pattern, same-surface copy, and
  cross-surface copy even when the CP is active.
- Tracks the backend with pending work so `WaitBlitter` drains MMIO or CP
  correctly instead of selecting solely from CP readiness.
- Adds an opt-in 64x64 ARGB RV280 hardware-cursor surface with normal and
  Picasso96 BIGSPRITE pointer rendering.
- Removes rtg.library's software-cursor overlap save/restore overhead when
  `HWSPRITE=YES` is active.
- Establishes a driver-owned `HOST_PATH_CNTL` baseline and removes two
  redundant PCI reads from each HDP invalidate while retaining a final readback
  barrier.
- Publishes DEBUG-only CP, DMA, MMIO, fill, drain, fallback, and cursor callback
  counters through the `Radeon9200.Debug` public port.
- Corrects ToolType documentation: settings come from
  `DEVS:Monitors/Radeon.info`, not an icon beside the card binary.

## RectFill Performance

P96Speed 1.2 was run on an Amiga 4000 with a 50 MHz 68060 and RV280
`1002:5964`, using 640x480x16 and 13 seconds per focused test.

| Build | RectFill op/s | Change from v0.10 |
|---|---:|---:|
| v0.9 published focused result | 2593 | -47.7% |
| v0.10 published mean | 4960 | baseline |
| v0.11 sample 1 | 5080 | +2.4% |
| v0.11 sample 2 | 5083 | +2.5% |
| v0.11 sample 3 | 5037 | +1.6% |
| **v0.11 mean** | **5066.7** | **+2.2%** |
| Closed driver reference | 11881 | v0.11 is 57.4% lower |

The exact v0.11 artifact samples have a 46 op/s range, 0.91 percent of the mean.
Compared with the original five-run CP-active mean of 1954.4 op/s, the final
result is 159.3 percent faster. It remains 52.6 percent below the 10693 target.
The saved final result is `Work:P96Speed_v011_final.txt`, CRC32 `57CECA86`.

Controlled DEBUG measurements explain the gain:

- Suppressing the software cursor raised RectFill from 1992 to 3153 op/s,
  showing that generic sprite overlap handling dominated the original path.
- Real hardware cursor plus MMIO 2D averaged 3424 op/s under instrumentation.
- Shadowing `HOST_PATH_CNTL` raised that instrumented mean to 3487.7 op/s and
  reduced measured driver time by about 6 microseconds per fill.
- Removing DEBUG timing hooks raised the release path to approximately
  4.93-4.96k op/s; nested `ReadEClock` calls materially affected the
  instrumented absolute score.
- Removing two redundant pre-trigger flush/idle writes reduced fill submission
  from nine to seven MMIO writes. Paired builds differing only by that change
  improved from 4933.3 to 5113.3 op/s, saving 7.14 microseconds per operation
  and adding 3.65 percent.

Fully fenced release `p96screen` fill timings improved from v0.9's 18/21/41
ticks to 3/5/10 ticks in the final CLUT8/RGB565PC/BGRA32 sequence, reductions
of approximately 83%, 76%, and 76%. The full 8/16/32/8 sequence passed fill,
pattern, masks, overlap-copy, cross-surface copy, guard, and direct-color
readback tests.

## Hardware Validation

- Tested card: RV280 Radeon 9200 `1002:5964`, 128 MiB COMBIOS board.
- CPU/bridge: 68060 at 50 MHz, Prometheus/FireBird environment.
- Active ToolTypes: `DMASIZE=2M`, `CP=YES`, and `HWSPRITE=YES`.
- Release binary: 31204 bytes, CRC32 `E7E2F009`, SHA-256
  `ef5c9f2893af13d13411de8f1157896a6c22ae4a89379c23e122b991d0460594`.
- Final 8/16/32/8 test records: CRC32 `00F2C01B`, `B94C0AAA`, `2E80F380`,
  and `1E10C436`.
- Binary release assets include the AMD microcode license and inherited-code
  redistribution notices.
- No crash was recorded after repeated cold boots, 8/16/32/8 transitions,
  focused P96Speed runs, and post-benchmark Workbench use.

The library reports `$VER: Radeon9200.card 0.11 (10.8.2026)`. Hardware cursor
support remains opt-in; bridge screenshots cannot capture the overlay, so
physical cursor appearance should be confirmed on the attached display.
Line, template/text, invert, planar, broader pattern, and unsupported
cross-surface operations continue to use Picasso96 software fallbacks.
