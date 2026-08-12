# Radeon9200.card 0.14

Version 0.14 is an early-development text-rendering performance update. It
optimizes the hardware JAM1/JAM2 `BlitTemplate` upload path used by Picasso96
`Text()` while retaining the direct-MMIO acceleration introduced in 0.12.

## Highlights

- Extracts complete monochrome template words with one 68020 `bfextu`.
- Uses an endian-correct raw host-data store with Radeon MSB-first consumption,
  removing software bit reversal from the upload loop.
- Advances source and row pointers directly instead of recalculating template
  offsets for every uploaded word.
- Caches stable template engine state and reserves available FIFO capacity for
  the complete upload when possible.
- Retains bounded extraction for partial final words and software fallback for
  unsupported CLUT write masks.
- Enables the hardware cursor by default; `HWSPRITE=NO` remains available for
  an explicit software-cursor fallback.
- Accelerates disjoint cross-surface source copies with independent source and
  destination pitches, removing the layer backing-store software fallback for
  overlapping windows.
- Accelerates cross-surface source-XOR-destination (opcode `$6`) and selects
  reverse copy direction for physically overlapping surfaces. This removes the
  generic P96 backing-store path that made a smart-refresh window take about
  9.6 seconds to open over another window; the validated release takes 11.7 ms.
- Adds DEBUG version 4 template workload counters and focused wide-template and
  `Text()` benchmarks to `p96screen`.
- Extends the DEBUG block to version 5 with cross-surface copy path, unequal
  pitch, and fallback-reason counters.
- Extends the DEBUG block to version 6 with calls, hardware/software split, and
  timing for every 2D callback, so interactive work can be attributed instead
  of only `RectFill`.
- Adds `HWTEXT` to select between the hardware `BlitTemplate` upload and
  rtg.library's CPU default. The hardware path remains the default: it is 1.8x
  faster on eight-character `Text()` and 3.8x faster on a 64-pixel template.
- Reserves template host-data FIFO entries in bursts instead of once per glyph
  row, cutting FIFO polling from 14.5 to 3.76 reads per template blit and the
  whole blit by 11.9 percent.
- Adds an experimental VRAM-staged `BlitTemplate` expand behind `TEXTSTAGE`.
  It is **off by default and known broken**: it wedges the 2D engine on the
  reference machine. The shipping text path is unchanged.
- Adds DEBUG version 7/8 probes that measure framebuffer-aperture write cost
  against MMIO register write cost, and confirm on real hardware that the 2D
  engine can colour-expand a monochrome source read from VRAM. Both are
  DEBUG-only; the release card is byte-identical without them.

## Text Performance

P96Speed 1.2 Text performance increased from approximately 4400 to 6300
operations/second on the development system, a gain of about 43 percent. The
focused `p96screen` `Text("P96Speed")` benchmark improved from 40 to 30 ticks.

## Development Status

This remains an early-development driver. The release candidate was tested on
the target Amiga before tagging; no additional release-only test pass was run.

The library reports `$VER: Radeon9200.card 0.14 (12.8.2026)`.
