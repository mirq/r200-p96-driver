# Radeon9200.card 0.12

Version 0.12 is a performance and callback-completeness update. It replaces
per-register OpenPCI library calls with validated direct MMIO, removes costly
generic arithmetic from the common surface path, and raises focused RectFill
performance from roughly 5.1k to 10.7k operations/second.

## Highlights

- Uses endian-correct volatile accesses after BAR ownership, type, and size
  validation instead of calling `pci_inl()`/`pci_outl()` for every register.
- Inlines unchecked MMIO access in acceleration code, matching the closed
  driver's direct `move.l` command sequences.
- Removes the solid-fill FIFO pre-poll and uses the RV280's validated bus
  backpressure behavior.
- Matches the closed driver's `WaitBlitter` behavior: wait for 64 free FIFO
  entries, then wait for GUI idle, without a semaphore or redundant cache/HDP
  operations.
- Replaces per-fill 64-bit surface bounds arithmetic with overflow-safe 32-bit
  checks; normal 1 KiB-aligned surfaces also avoid address-bias division.
- Adds hardware destination inversion through `ROP3_Dn`.
- Adds hardware JAM1/JAM2 monochrome `BlitTemplate` through host-data upload.
- Installs `EnableSoftSprite` alongside the existing RV280 hardware cursor.
- Extends `p96screen` with invert and template correctness and timing tests.

## RectFill Performance

P96Speed 1.2 was run on an Amiga 4000 with a 50 MHz 68060 and RV280
`1002:5964`, using 640x480x16 and 13 seconds per focused test.

| Build | RectFill op/s | Time/op | Change |
|---|---:|---:|---:|
| v0.11 validated mean | 5066.7 | 197.37 us | baseline |
| Pre-v0.12 comparison | 5167 | 193.54 us | - |
| Direct volatile MMIO | ~6700 | ~149.25 us | +29.7% vs 5167 |
| **v0.12** | **10688** | **93.56 us** | **+106.9% vs 5167** |
| Closed driver reference | 11881 | 84.17 us | v0.12 is 10.1% lower |

Direct volatile MMIO removed about 44 microseconds per operation. Replacing the
generic 64-bit validator removed another 56 microseconds and was the change that
crossed the 10k target. The v0.12 result is 2.11x the v0.11 validated mean and
within 10.1 percent of the closed driver.

## Hardware Validation

- Tested card: RV280 Radeon 9200 `1002:5964`, 128 MiB COMBIOS board.
- CPU/bridge: 68060 at 50 MHz, Prometheus/FireBird environment.
- Active ToolTypes: `DMASIZE=2M`, `CP=YES`, and `HWSPRITE=YES`.
- Final release binary: 30,696 bytes, CRC32 `FB3D08BE`, SHA-256
  `3b3f99815afe272526c3247b6389ea56f04b6c8214828403af014be3294562e0`.
- Benchmarked binary before the version-only rebuild: 30,696 bytes, CRC32
  `85C2D325`; executable code is otherwise identical.
- Preserved on the Amiga as
  `LIBS:Picasso96/Radeon9200.card.direct-fast-10688`.
- The CLUT8/RGB565PC/BGRA32/CLUT8 sequence passed fill, inversion, JAM1/JAM2
  template, pattern, mask, overlap-copy, cross-surface copy, guard, and
  direct-color readback tests.
- No crash was recorded during cold boots, P96Speed, validation screen cycles,
  or return to Workbench.

The library reports `$VER: Radeon9200.card 0.12 (10.8.2026)`.
