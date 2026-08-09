# Radeon9200.card 0.9

Version 0.9 extends the validated direct-MMIO RV280 2D subset with direct-color
solid fills and narrow monochrome pattern acceleration. It also reduces FIFO
status reads for fills and copies while retaining bounded waits, recovery, and
software fallbacks.

## Highlights

- Accelerates solid `FillRect` in CLUT8, RGB565PC, and BGRA32, including CLUT8
  partial write masks.
- Accelerates JAM2 `BlitPattern` for 1, 2, 4, or 8 rows when each 16-bit source
  row contains two identical bytes and therefore repeats every eight pixels.
- Preserves software fallback for true 16-pixel patterns, unsupported draw
  modes, system-incompatible sources, and invalid destinations.
- Coalesces MMIO FIFO reservations to one status poll for each fill or copy.
- Adds phase, edge, overdraw, row-order, partial-mask, and fallback tests to
  `p96screen`.

## Benchmarks

P96Speed 1.2 was run on an Amiga 4000 with a 50 MHz 68060 and RV280
`1002:5964`, using 640x480x16 and 13 seconds per test.

| Operation | v0.7 | v0.9 | Closed driver |
|---|---:|---:|---:|
| RectFill | 102 | 2593 | 11881 |
| RectFill Pattern | 35 | 1387 | 11246 |
| WritePixel | 128772 | 129429 | 134097 |
| Draw Hor/Ver | 2939 | 4030 | 24543 |
| PutText | 1018 | 1018 | 8553 |
| BlitBitMap | 3660 | 3416 | 13673 |
| BlitBitMapRastPort | 2673 | 2597 | 7586 |

Pattern fill improved from the immediate 37 op/s software baseline to 1387
op/s, a 37.5x increase. The complete v0.9 run initially measured solid fill at
1427 op/s; an individual repeat measured 2593 op/s. Other v0.9 rows in the
table come from the complete run. The saved accepted result is
`Work:P96Speed_10pattern2.txt`, CRC32 `70D67A67`.

After a clean reboot, fully fenced `p96screen` measurements were:

- 4096 cross-surface copies: 41-45 DOS ticks.
- 256 full-screen fills: 18/21/41 ticks in CLUT8/RGB565PC/BGRA32.
- All 8/16/32/8 pattern, fill, overlap-copy, cross-surface-copy, mask, and
  fallback checks passed.

## Hardware Validation

- Tested card: RV280 Radeon 9200 `1002:5964`, 128 MiB COMBIOS board.
- CPU/bridge: 68060 at 50 MHz, Prometheus/FireBird environment.
- Active and rollback release binary: 23088 bytes, CRC32 `B5ABB580`.
- Previous direct-fill rollback: CRC32 `D2A76396`.
- No crash was recorded after cold boot, 8/16/32/8 transitions, P96Speed, and
  post-benchmark smoke tests.

The library reports `$VER: Radeon9200.card 0.9 (9.8.2026)`. Pattern support is
deliberately narrow; line, template/text, invert, planar, and broader pattern
or cross-surface operations continue to use Picasso96 software fallbacks.
