# Radeon9200.chip Development Notes

## Target and safety

The reference target is a physical 68060 Amiga, not an emulator. Do not use
emulator reset, pause, state, or lifecycle commands. The primary AmigaBridge
endpoint is `192.168.1.21:2345`; the old `192.168.100.*` endpoints are no
longer accessible. A cold reboot takes about 100 seconds before the bridge is
available.

Run AmigaBridge transfers and filesystem copies sequentially. Concurrent copy
requests can use or report the wrong destination. Keep a native display or
serial recovery path available for hardware tests.

Switch both monitor ToolTypes before a cold reboot:

```text
BOARDTYPE=Prometheus
SETTINGSFILE=SYS:Devs/Picasso96Settings.9200
```

Changing only `BOARDTYPE` can leave Workbench on the native fallback screen.

## Architecture

- `Prometheus.card` owns PCI enumeration, board claiming, configuration,
  address translation, interrupt registration, and the shared DMA arena.
- `Radeon9200.chip` owns RV280 initialization, display, cursor, 2D callbacks,
  and the Radeon3D service. It must not enumerate independently.
- Prometheus passes its handoff in `BoardInfo.CardData`; Radeon per-board state
  belongs in `BoardInfo.ChipData`. Library bases contain only library-lifetime
  state.
- `InitChip` publishes chip metadata. `InitRadeonFeatures` validates the
  Prometheus handoff and installs only callbacks that are safe for the
  advertised formats.
- BAR0 is the framebuffer aperture and BAR2 is RV280 MMIO. Validate ownership,
  type, address, and size before using either.
- Radeon registers are little-endian. Use `SWAPWORD()`/`SWAPLONG()` at mapped
  MMIO boundaries; do not swap PCI configuration values or addresses.
- Mapped, endian-correct volatile aperture access is required on the hot path;
  per-register PCI-library calls are too expensive on the target bridge.
- Do not disable or alter an unclaimed Radeon board.

Use the complete supplied Picasso96 CardDevelop 3.6 `BoardInfo`; never replace
or clear it wholesale. Preserve existing flags and callbacks unless the chip
deliberately replaces them. Callbacks are entered without the chip library base
guaranteed in A6.

## Supported hardware policy

Initial PCI IDs are ATI `1002:5960`, `1002:5961`, and `1002:5964`. CRTC0 with
the primary VGA DAC is the conservative path. Internal TMDS/DVI is supported
only for a validated COMBIOS connector and transmitter table. CRTC1, external
TMDS, TV output, overlays, and interrupts remain out of scope.

The 64x64 ARGB cursor and CP ring use private VRAM reservations. `DMASIZE` is a
separate page-aligned high-VRAM arena excluded from Picasso96 and Radeon
graphics, then published through Prometheus's existing DMA vectors for peer bus
masters. A valid positive arena is required for Radeon initialization.

## Acceleration and 3D

Picasso96 2D uses bounded direct MMIO even when the CP is active. The hardware
subset includes `FillRect`, `InvertRect`, `BlitRect`, `DrawLine`, supported
JAM1/JAM2 templates and patterns, and complete copies for every four-bit P96
minterm. All surfaces and arithmetic must be validated before MMIO. Unsupported
or unsafe work drains as required and uses the saved P96 software callback.

FIFO and idle waits are bounded. Recovery invalidates 3D sessions, resets the
engine, reloads and self-tests the CP, and invalidates cached 2D state before
acceleration resumes. Failed recovery must not permit unsafe VRAM rendering.

The Radeon3D API is an active bounded service, not a future raw-register path.
Do not expose unrestricted packets or client-controlled register writes. Follow
[`RADEON3D_SUBMISSION.md`](RADEON3D_SUBMISSION.md) and the authoritative MiniGL
roadmap at `/home/mirek/r200_minigl/MINIGL_R200_PLAN.md` before changing the ABI.
Results are tracked in [`R200_3D_PROGRESS.md`](R200_3D_PROGRESS.md).

Check any planned fixed-function behaviour against the Mesa implementation
before writing it, and again before trusting a probe's expected values. Mesa
7.11.2 is the last release carrying the classic r200 driver: its
`src/mesa/drivers/dri/r200` gives the register programming and upload
orientation, and its software TNL (`src/mesa/tnl/t_vb_texgen.c`,
`t_vb_light.c`) gives the exact arithmetic the hardware path must reproduce,
including which inputs arrive normalized. A hand-derived expectation is a
common source of false probe failures. The interface-12 sphere-map probe first
asserted that a normal parallel to the eye vector produces (0.5,0.5); the
reflection `f = u - 2n(n.u)` in fact gives `-u`, so its corner samples failed
against correct hardware while the interpolated centre passed.

## Sources, build, and validation

Prefer, in order: local Picasso96 CardDevelop headers/examples for ABI;
Prometheus headers/source for card ownership; Mesa R200 sources for formats and
registers; and NetBSD `radeonfb` sources for portable ROM and modesetting
algorithms. Do not inspect or copy
`/home/mirek/warp3d-r9200/Prometheus/PrometheusCard`. Retain applicable notices
when adapting third-party material.

Build with `make` using `/opt/amiga/bin/m68k-amigaos-gcc`. Keep
`src/startup.c` and `src/library.c` first in link order so `_start` and resident
data remain in the first code hunk. Treat warnings as defects.

A successful build is not hardware validation. Record commit, dirty state,
artifact hashes, CPU, bridge, board ID/revision, VRAM/ROM, ToolTypes, mode,
cold/warm state, and exact tests for every physical run.
