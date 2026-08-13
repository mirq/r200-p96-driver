# Radeon9200.chip Development Notes

The reference test target is a physical Amiga, not an emulator. Never use
emulator lifecycle, reset, pause, or state-management commands for it.
The AmigaBridge endpoint is `192.168.100.47:2345`.
The alternate network card uses `192.168.100.50:2345`.
A physical Amiga reboot takes approximately 100 seconds before the bridge is
available again.

Switching card drivers requires changing both active monitor ToolTypes before
the cold reboot. `BOARDTYPE=Prometheus` must be paired with
`SETTINGSFILE=SYS:Devs/Picasso96Settings.9200`; changing only `BOARDTYPE`
loads the card but leaves Workbench on the native fallback screen. Restore the
matching Radeon settings-file ToolType when switching back to `Radeon.card`.

## Goal

Build a small, pure-C Picasso96 chip driver for desktop RV280 Radeon 9200
boards behind Prometheus.card. The first hardware target is a
Prometheus or FireBird bridge and VGA output through CRTC0 and the primary
DAC.

## Hard Constraints

- Target `prometheus.library` version 2 or newer for PCI ownership,
  configuration, address translation, interrupts, and DMA allocation.
- Use Picasso96 CardDevelop 3.6 structures and callback ABI.
- Keep the driver in C. Small 68k entry stubs are acceptable only if an ABI
  cannot be expressed safely in C.
- Initial PCI IDs are ATI `1002:5960`, `1002:5961`, and `1002:5964`.
- Initial display output is VGA only. DVI, TMDS, CRTC1, TV output, overlays,
  and interrupts are later milestones. The current hardware-cursor subset is a
  64x64 ARGB RV280 cursor backed by the Prometheus DMA arena.
- Do not inspect or copy code from
  `/home/mirek/warp3d-r9200/Prometheus/PrometheusCard`; it is not a valid
  reference for this driver.
- Test on real hardware. Do not add mocked or simulated hardware tests.

## Prometheus Rules

- `Prometheus.card` owns enumeration, board claiming, interrupt registration,
  and the shared DMA arena. `Radeon9200.chip` must not enumerate independently.
- Use BAR0 for the framebuffer aperture and BAR2 for RV280 MMIO only after
  validating their type, address, and size.
- Use Prometheus for discovery, ownership, configuration space, and bridge setup.
  After BAR type, size, and ownership validation, mapped Radeon framebuffer and
  MMIO apertures may use endian-correct volatile CPU accesses. This is required
  for the validated hot path; per-register `pci_in*()`/`pci_out*()` library
  calls are prohibitively expensive on the target bridge.
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

`DMASIZE` is a page-aligned region at the high end of VRAM, excluded from
Picasso96 and all Radeon graphics operations, then published through the
existing Prometheus DMA vectors for peer PCI bus masters such as RTL8139.
Radeon-private resources must be reserved separately below this shared region.

## Acceleration Policy

The CP ring may remain initialized for future 3D work, but all current
Picasso96 2D operations use direct MMIO. Explicit FIFO and idle polls are
bounded; solid fills follow the validated hardware-backpressure path and do not
pre-poll the FIFO. One timeout recovery reset is allowed; successful recovery
permanently routes the session to software, and failed recovery blocks further
VRAM rendering through the wrappers.

The validated hardware subset is `FillRect` and destination-only `InvertRect`
in CLUT8/RGB565PC/BGRA32 (including CLUT8 partial masks), same-`RenderInfo`
`BlitRect` in all three formats, and cross-surface
`BlitRectNoMaskComplete` for opcode `$C` (source copy) and `$6` (source XOR
destination) between on-board surfaces with independently validated pitches.
Overlapping surfaces select direction from their absolute VRAM ranges, matching
the smart-refresh backing-store workload. Surface bases are rebased to a 1 KiB
GPU address with checked X/Y bias so ordinary 16-byte P96 allocations remain
usable.

The validated subset also includes JAM1/JAM2 `BlitTemplate` and JAM2
`BlitPattern` for 1/2/4/8-row patterns
whose 16-bit rows contain identical bytes. Hardware activity is established by
the P96Speed pattern increase from 37 to 1387 operations/second; 8/16/32/8
readback tests pass every accepted height, phase, edge, and overdraw guard.
CLUT8 partial-mask patterns and synchronized software fallback for a rejected
16-pixel pattern also pass.
Planar, unsupported patterns, and complete-copy opcodes other than `$6` and
`$C` remain software fallbacks.

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
8. Completed first 2D subset: fills and overlap-safe same-surface copies in all
   advertised formats. Additional operations remain separate milestones.
9. CP initialization retained for future 3D while Picasso96 2D uses MMIO, plus
   a private-VRAM RV280 hardware cursor with per-board state.

## Build And Verification

- Build with `make` using `/opt/amiga/bin/m68k-amigaos-gcc`.
- Keep `src/startup.c` and `src/library.c` first in link order so `_start` is
  the first code and the resident data remains in the first code hunk.
- Treat compiler warnings as defects.
- A successful build is not a hardware validation. Record each real-hardware
  test, bridge type, CPU, board ID, cold/warm boot state, and observed output.
