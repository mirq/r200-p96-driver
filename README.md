# Radeon9200.card

Experimental pure-C Picasso96 card driver for desktop RV280 Radeon 9200 PCI
boards.

## Current Status

The current milestone builds a resident card library, produces VGA output,
and opens Picasso96-managed screens. It implements:

- Picasso96 CardDevelop 3.6 library and callback ABI.
- OpenPCI 2.1 discovery and ownership for ATI `1002:5960`, `1002:5961`, and
  `1002:5964` display devices.
- BAR0 framebuffer and BAR2 MMIO validation, PCI command preservation, and
  endian-safe OpenPCI register access.
- Bounded PCI option-ROM loading and selection of a checksummed x86 image.
- Legacy COMBIOS parsing and cold-card ASIC, PLL, SDRAM, and memory-controller
  initialization.
- CRTC0 and primary-DAC setup for an initial `640x480@60` CLUT8 VGA screen.
- A startup test pattern with black, red, green, blue, cyan, magenta, yellow,
  and white vertical bars.
- CLUT8, little-endian RGB565 (`RGBFB_R5G6B5PC`), and four-byte
  BGRA/XRGB8888 (`RGBFB_B8G8R8A8`) scanout.
- Picasso96 callbacks for modesetting, format-aware pitch and panning,
  indexed and identity-LUT palette updates, display blanking, vertical sync,
  and DPMS.
- Strict `DMASIZE` ToolType parsing and a transactional, 4 KiB-aligned private
  arena reserved from the high end of usable VRAM.
- Bounded direct-MMIO 2D engine initialization, synchronization, reset, and
  timeout fallback.
- Hardware rectangle fills in all three formats, including CLUT8 partial write
  masks, overlap-safe same-surface rectangle copies, and cross-surface
  source-copy (`opcode $C`) rectangle copies between disjoint, same-pitch
  Picasso96 surfaces.
- Hardware JAM2 monochrome patterns with 1, 2, 4, or 8 rows when each 16-bit
  source row repeats the same eight horizontal pixels.

`InitCard` returns success only after hardware initialization and startup
screen setup have completed. Version 0.9 is validated from a cold boot on a
real 68060 Amiga with a Prometheus/FireBird bridge, an RV280 `1002:5964` 128 MiB
COMBIOS board, and `rtg.library` 43.538. The tested configuration provides a
64 MiB linear aperture and boots a `1024x768x8` Workbench on the Radeon. With
`DMASIZE=2048k`, Picasso96 receives 62 MiB and the private high 2 MiB remains
outside its allocator.

Picasso96 API tests repeatedly open and close `640x480` screens in all three
advertised formats. Their measured pitches are 640, 1280, and 2560 bytes.
Tests cover rectangle edges, direct-color fills, right/down overlapping copies,
cross-surface disjoint copies, unaligned P96 surface addresses, and CLUT8
partial masks. P96 pixel readback returns the expected RGB565-quantized colors
and exact 32-bit `00RRGGBB` colors. No crash is recorded after an 8/16/32/8
transition sequence. Pattern tests cover phase, rectangle edges, overdraw
guards, complete row ordering for all four accepted heights, CLUT8 partial
masks, and synchronized fallback for unsupported 16-pixel patterns.

On the validated `640x480x8` surface, 256 full-screen fills dropped from 627
DOS ticks through the software path to 17-23 ticks in hardware. The matching
256 half-screen copies dropped from 718 ticks to 16-22 ticks. These runs are
approximately 27-37 times faster for fills and 33-45 times faster for copies.
Cross-surface `BlitBitMap` and `BlitBitMapRastPort` improved from 242/237
ops/s to 3483/2573 ops/s (roughly 14x and 11x) in P96Speed 640x480x16 tests.
The validated monochrome pattern path improved `RectFill Pattern` from 37 to
1387 ops/s (37.5x). A repeated solid-fill sample reached 2593 ops/s.

## Build

```sh
make clean && make
```

The default cross-compiler prefix is `/opt/amiga/bin/m68k-amigaos-`. The
release artifact is `Radeon9200.card` in the project root.

For serial/debug logging through `KPrintF`:

```sh
make clean && make DEBUG=1
```

Always clean when switching `DEBUG`, because both configurations use the same
object and output paths.

Build the P96 screen test utility with:

```sh
make tools
```

The result is `build/p96screen`. Its AmigaDOS syntax is:

```text
p96screen [8|16|32] [seconds] [test]
```

It selects the `Radeon9200` board, opens a `640x480` screen, draws eight color
bars, reads direct-color pixels back through Picasso96, and closes the screen.
The optional `test` argument also runs fill, pattern, overlap-copy, and
CLUT8-mask readback checks, plus fill/copy timing in the selected format.

See [`p96speed.md`](p96speed.md) for the verified MCP procedure that selects
the 640x480x16 mode, runs all 21 P96Speed tests, waits for completion, and saves
the ASCII result without manual input.

See [`performance.md`](performance.md) for benchmark comparisons, accepted and
rejected optimizations, hardware-validation records, and ordered follow-up
work.

## Picasso96 Configuration

The monitor icon must use `BOARDTYPE=Radeon9200`. The corresponding entry in
`DEVS:Picasso96Settings` must use board type `BT_Radeon` and board name
`Radeon9200`. Load `DEVS:Monitors/Radeon` before `IPrefs` if the system startup
does not run `LoadMonDrvs`.

Back up an existing settings file before editing it. The validated target has
`640x480` modes for depths 8, 16, and 32; the 32-bit mode uses the standard
25.175 MHz `640x480@60` timing.

## DMASIZE

`DMASIZE` accepts decimal bytes with an optional `K` or `M` suffix, without
whitespace or trailing text. The key and suffix are case-insensitive. Examples:

```text
DMASIZE=0
DMASIZE=64K
DMASIZE=2M
```

The final `DMASIZE` occurrence wins. Invalid, overflowing, or oversized values
disable the reservation without preventing display initialization. Positive
requests round up to 4096 bytes and must leave at least 4 MiB for Picasso96.

Version 0.5 keeps the arena private to the Radeon driver. Its page-map
allocator is initialized and self-tested before `MemorySize` is reduced. No
public allocation vectors are exposed yet.

## First Hardware Test

Use a recoverable setup with a working native display or serial console and a
way to disable the driver on the next boot. Connect a VGA monitor that accepts
`640x480@60`. `OUTPUT=VGA` is the only accepted output ToolType; omitting it
also selects VGA.

Record the bridge model, CPU, exact PCI device ID, VRAM size, ROM type, and
whether the card was cold or already posted. A successful first test should
show eight equal-width vertical color bars. A debug build reports posted/cold
state, COMBIOS revision, PLL parameters, usable VRAM, and the failing
initialization stage.

## Limits

- Legacy COMBIOS only; ATOM BIOS cold initialization is rejected.
- CRTC0 and the primary analog DAC only.
- The only direct-color layouts are `RGBFB_R5G6B5PC` and
  `RGBFB_B8G8R8A8`. There is no 15-bit, packed 24-bit, native-endian alias, or
  alternate channel ordering yet.
- No DVI, TMDS, TV output, CRTC1, interrupts, cursor, or overlay.
- Pattern acceleration is limited to JAM2, heights up to eight rows, and
  16-pixel source rows whose two bytes are identical. Other patterns use P96
  software.
- Template, line, invert, planar conversion, host-data, and cross-surface
  operations with overlap, different pitch, or non-copy opcodes still use P96
  software.
- Linear scanout is limited to the lower 64 MiB aperture.
- Horizontal panning is quantized to 8 pixels in CLUT8, 4 pixels in RGB565,
  and 2 pixels in BGRA32 because CRTC offsets have eight-byte granularity.
- Border/overscan programming is not implemented; modes are borderless.
- One Radeon board instance per library load.
- Failure and unload restore PCI command ownership state and leave output
  blank, but do not restore every pre-existing Radeon register.

The private `DMASIZE` reservation is not an OpenPCI 2.1 `MEM_PCI` provider.
OpenPCI publication, address translation for a cooperating client, and real
inter-card DMA transfers remain separate, unimplemented work. Newer openpci
memory-provider APIs are deliberately out of scope.
