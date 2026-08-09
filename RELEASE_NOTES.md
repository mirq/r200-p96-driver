# Radeon9200.card 0.10

Version 0.10 keeps the R200 command processor initialized for future 3D work
while routing Picasso96 2D operations through the faster direct-MMIO path. It
also adds an opt-in RV280 hardware cursor, reduces completion-path MMIO reads,
and adds enough DEBUG observability to prove which backend is active.

## Highlights

- Initializes and validates the R200 CP ring and microcode from private VRAM
  reserved with `DMASIZE`, while retaining bounded fallback and recovery.
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

| Build | RectFill op/s | Change from v0.9 |
|---|---:|---:|
| v0.9 published focused result | 2593 | baseline |
| v0.10 release, sample 1 | 4960 | +91.3% |
| v0.10 release, sample 2 | 4956 | +91.1% |
| v0.10 release, sample 3 | 4964 | +91.4% |
| **v0.10 mean** | **4960** | **+91.3%** |
| Closed driver reference | 11881 | v0.10 is 58.3% lower |

The v0.10 samples have an eight op/s range, only 0.16 percent of the mean.
Compared with the original five-run CP-active mean of 1954.4 op/s, the final
release is 153.8 percent faster. The saved final result is
`Work:P96Speed_release_mmio_rectfill.txt`, CRC32 `E374B4AB`.

Controlled DEBUG measurements explain the gain:

- Suppressing the software cursor raised RectFill from 1992 to 3153 op/s,
  showing that generic sprite overlap handling dominated the original path.
- Real hardware cursor plus MMIO 2D averaged 3424 op/s under instrumentation.
- Shadowing `HOST_PATH_CNTL` raised that instrumented mean to 3487.7 op/s and
  reduced measured driver time by about 6 microseconds per fill.
- Removing DEBUG timing hooks for the release raised the stable mean to
  4960 op/s; nested `ReadEClock` calls materially affected the instrumented
  absolute score.

Fully fenced release `p96screen` fill timings improved from v0.9's 18/21/41
ticks to 4/6/12 ticks in the final CLUT8/RGB565PC/BGRA32 sequence, reductions
of approximately 78%, 71%, and 71%. The full 8/16/32/8 sequence passed fill, pattern, masks,
overlap-copy, cross-surface copy, guard, and direct-color readback tests.

## Hardware Validation

- Tested card: RV280 Radeon 9200 `1002:5964`, 128 MiB COMBIOS board.
- CPU/bridge: 68060 at 50 MHz, Prometheus/FireBird environment.
- Active ToolTypes: `DMASIZE=2M`, `CP=YES`, and `HWSPRITE=YES`.
- Release binary: 31260 bytes, CRC32 `2AC0183B`, SHA-256
  `0790823a6ab0347f43f67ab5c2ba9b2da2d0c6b5e6cfa1d2ecf108b5aa410b60`.
- Final 8/16/32/8 test records: CRC32 `CDE47391`, `0506FCC6`, `2F66E5E5`,
  and `BC93CD8B`.
- No crash was recorded after repeated cold boots, 8/16/32/8 transitions,
  focused P96Speed runs, and post-benchmark Workbench use.

The library reports `$VER: Radeon9200.card 0.10 (10.8.2026)`. Hardware cursor
support remains opt-in; bridge screenshots cannot capture the overlay, so
physical cursor appearance should be confirmed on the attached display.
Line, template/text, invert, planar, broader pattern, and unsupported
cross-surface operations continue to use Picasso96 software fallbacks.
