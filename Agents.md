# Radeon9200.card Development Notes

## Goal

Build a small, pure-C Picasso96 card driver for desktop RV280 Radeon 9200
boards on classic Amiga PCI bridges. The first hardware target is a
Prometheus or FireBird bridge and VGA output through CRTC0 and the primary
DAC.

## Hard Constraints

- Target the OpenPCI 2.1 API in `OpenPci2.1-SDK290208` only.
- Do not use APIs, headers, assumptions, or compatibility shims from the
  newer openpci.library. It is a different implementation and API.
- Use Picasso96 CardDevelop 3.6 structures and callback ABI.
- Keep the driver in C. Small 68k entry stubs are acceptable only if an ABI
  cannot be expressed safely in C.
- Initial PCI IDs are ATI `1002:5960`, `1002:5961`, and `1002:5964`.
- Initial display output is VGA only. DVI, TMDS, CRTC1, TV output, overlays,
  interrupts, and hardware cursors are later milestones.
- Do not inspect or copy code from
  `/home/mirek/warp3d-r9200/Prometheus/PrometheusCard`; it is not a valid
  reference for this driver.
- Test on real hardware. Do not add mocked or simulated hardware tests.

## OpenPCI 2.1 Rules

- Open `openpci.library` with `MIN_OPENPCI_VERSION`.
- Enumerate with `pci_find_device()` and pass the previous returned device as
  the continuation cursor.
- Treat every `struct pci_dev` returned by OpenPCI as read-only.
- Claim a device with `pci_obtain_card()` before changing configuration or
  touching registers, and pair it with `pci_release_card()`.
- Preserve and restore the original PCI command word on failure or unload.
- Use BAR0 for the framebuffer aperture and BAR2 for RV280 MMIO only after
  validating their type, address, and size.
- Access PCI memory and registers only through `pci_in*()`, `pci_out*()`, and
  the OpenPCI copy functions. Never use direct volatile pointer accesses.
- Radeon registers are little-endian. Use `SWAPWORD()` and `SWAPLONG()` at the
  MMIO boundary; do not swap PCI configuration values or addresses.
- Do not disable or alter unclaimed Radeon boards.

## Picasso96 Rules

- Use the complete supplied `struct BoardInfo`; never replace it with a local
  shortened definition or clear the whole structure.
- `FindCard` receives `BoardInfo` in A0. `InitCard` receives `BoardInfo` in A0
  and the NULL-terminated ToolTypes array in A1.
- Keep per-board state in `BoardInfo.CardData`. The card library base may hold
  only library-lifetime bookkeeping.
- Preserve existing `BoardInfo.Flags` bits and callbacks unless this driver
  intentionally implements and replaces them.
- Callbacks are entered without the card library base guaranteed in A6.
- Do not return success from `InitCard` until all callbacks required by the
  advertised formats and flags are installed and safe.

## DMA Policy

Version 0.5 implements `DMASIZE` as a page-aligned private region at the high
end of VRAM and excludes it from Picasso96 framebuffer allocation. OpenPCI 2.1
defines `pci_allocdma_mem(size, MEM_PCI)` for graphics-board memory, but the
local SDK has no driver-side API for publishing this Radeon-specific range. Do
not use newer openpci memory-provider calls or describe the private arena as an
OpenPCI provider. Publication and real inter-card transfers remain unresolved.

## Acceleration Policy

Version 0.6 uses the RV280 direct-MMIO 2D engine without a CP ring or the DMA
arena. Every FIFO, idle, and cache poll is bounded. One timeout recovery reset
is allowed; successful recovery permanently routes the session to software,
and failed recovery blocks further VRAM rendering through the wrappers.

The validated hardware subset is CLUT8 `FillRect` (including partial masks),
same-`RenderInfo` `BlitRect` in CLUT8/RGB565PC/BGRA32, and cross-surface
`BlitRectNoMaskComplete` for opcode `$C` (source copy) between disjoint,
same-pitch surfaces. Surface bases are rebased to a 1 KiB GPU address with
checked X/Y bias so ordinary 16-byte P96 allocations remain usable. RGB565PC
and BGRA32 fills must remain software until hardware brush-color endian
handling is separately proven. Template, pattern, line, invert, planar,
host-data, and any cross-surface operations with overlap, unequal pitch, or
non-copy opcodes remain software fallbacks.

## Source Priorities

1. Local Picasso96 CardDevelop 3.6 headers and examples for ABI and ownership.
2. Local OpenPCI 2.1 headers, autodocs, and examples for all PCI access.
3. Mesa R200 sources for PCI IDs, formats, and register definitions.
4. NetBSD `radeonfb` and `radeonfb_bios` for portable ROM, PLL, reset, and
   modesetting algorithms.

Do not transplant code without retaining applicable notices and adapting its
bus access to OpenPCI 2.1.

## Milestones

1. Compilable resident card library with safe discovery, ownership, BAR
   validation, cleanup, and endian-safe register helpers.
2. Bounded ROM loading and legacy Radeon BIOS parsing.
3. Cold RV280 reset and memory-controller initialization.
4. Conservative CRTC0 VGA modesetting, starting at 640x480 at 60 Hz.
5. Minimal Picasso96 callbacks and verified CLUT8 framebuffer operation.
6. Verified 16-bit and 32-bit formats, panning, palette, and DPMS.
7. Private `DMASIZE` reservation, followed separately by proven OpenPCI 2.1
   inter-card DMA behavior if a provider path is deliberately added.
8. Completed first 2D subset: CLUT8 fills and overlap-safe same-surface copies.
   Additional operations remain separate milestones.

## Build And Verification

- Build with `make` using `/opt/amiga/bin/m68k-amigaos-gcc`.
- Keep `src/startup.c` and `src/library.c` first in link order so `_start` is
  the first code and the resident data remains in the first code hunk.
- Treat compiler warnings as defects.
- A successful build is not a hardware validation. Record each real-hardware
  test, bridge type, CPU, board ID, cold/warm boot state, and observed output.
