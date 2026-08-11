# Radeon9200.card 0.13

Version 0.13 is an early-development text-rendering performance update. It
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
- Adds DEBUG version 4 template workload counters and focused wide-template and
  `Text()` benchmarks to `p96screen`.

## Text Performance

P96Speed 1.2 Text performance increased from approximately 4400 to 6300
operations/second on the development system, a gain of about 43 percent. The
focused `p96screen` `Text("P96Speed")` benchmark improved from 40 to 30 ticks.

## Development Status

This remains an early-development driver. The release candidate was tested on
the target Amiga before tagging; no additional release-only test pass was run.

The library reports `$VER: Radeon9200.card 0.13 (11.8.2026)`.
