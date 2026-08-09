# 2D Performance Work

This file tracks real-hardware benchmark results, accepted optimizations, and
the order in which unvalidated acceleration candidates should be tested.
P96Speed automation is documented separately in [`p96speed.md`](p96speed.md).

## Reference Configuration

- Amiga 4000, 68060 at 50 MHz
- Picasso96, `rtg.library` 43.538
- RV280 Radeon 9200 `1002:5964`
- P96Speed 1.2, 640x480x16, 13 seconds per test

The closed-driver reference is `Work:P96Speed_org.txt`. The current validated
result is `Work:P96Speed_10pattern2.txt`.

## Current Gaps

| Operation | Closed driver | Validated driver | Remaining gap |
|---|---:|---:|---:|
| RectFill | 11881 | 2593 | 4.6x |
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

## Accepted Changes

### Cross-surface source copies

`BlitRectNoMaskComplete` opcode `$C` is accelerated for disjoint, same-pitch
on-board surfaces. It improved:

- BlitBitMap: about 242 to 3483-3936 operations/second
- BlitBitMapRastPort: about 237 to 2573-2804 operations/second

All CLUT8, RGB565PC, and BGRA32 cross-surface tests pass.

### Coalesced FIFO reservations

Fill and copy submission reserve all required FIFO entries with one hardware
status read instead of three:

- Fill: one reservation for 9 writes
- Copy: one reservation for 10 writes

Five-run focused copy averages improved from 53.8 to 51.6 ticks (about four
percent). All cache flush and `WAIT_UNTIL` ordering remains unchanged.

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

## Rejected Experiment

Removing the destination-cache flush and `WAIT_UNTIL` pair from every solid
fill was correct under synthetic queue stress but did not improve the real
workload. Two P96Speed samples produced RectFill results of 1460 and 1493,
below the validated 2011 result. The barrier removal has therefore been
reverted.

Do not reintroduce it without dependency/cache accounting and a repeatable
P96Speed improvement.

## Hardware Validation Record

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

## Planned Follow-up Work

1. **DrawLine**: use the Radeon line engine and the now-proven direct-color pen
   conversion. This targets Draw and may also improve circles and ellipses.
2. **BlitTemplate**: implement bounded monochrome host-data expansion for text.
   This targets PutText and CON output, but requires special care because P96's
   `BIF_SYSTEM2SCREENBLITS` warning applies to classic bridges.
3. **Copy hazard tracking**: batch copies only when pending destinations cannot
   alias later sources. This targets scrolling and the remaining bitmap gap.
4. **BlitPattern extension**: consider JAM1, complement, inverse-video, and true
   16-pixel patterns without weakening the validated narrow fallback boundary.
5. **Broader minterms and pitches**: extend `BlitRectNoMaskComplete` to all 16
   ROPs and independently valid source/destination pitches.
6. **InvertRect**: add destination-only `ROP3_Dn`; low risk, but no dedicated
   P96Speed row makes it lower priority.
