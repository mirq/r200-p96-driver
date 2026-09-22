# 05 - Build, deploy and run

## 1. Toolchain

This is a cross-compile-only project. The host cannot run the outputs.

| Item | Value |
|---|---|
| Compiler prefix | `/opt/amiga/bin/m68k-amigaos-` (`CROSS ?= /opt/amiga/bin/m68k-amigaos-`) |
| Pinned version | GCC 6.5.0b (`stefanreinauer/amiga-gcc:gcc-v6.5.0b-20251124` in CI) |
| Language/flags | `-std=gnu99 -O2 -Wall -Wextra -Werror -Wmissing-prototypes -Wstrict-prototypes -m68020-60 -mregparm=4 -msmall-code -noixemul -ffreestanding -fno-builtin` |
| Link | `-ramiga-lib -nostartfiles -nodefaultlibs -lamiga -lgcc` |
| vbcc | `/home/mirek/vbcc` for the probes that are built with it; always `-O=1` (the installed 0.9h m68k backend miscompiles explicit `__reg()` library calls at `-O=2`) |

Do not substitute a Docker toolchain or a different GCC major version for
performance comparisons: changed code generation invalidates binary and
hardware baselines. Warnings are treated as defects for the chip and tools.

`src/startup.c` and `src/library.c` must stay first in link order so `_start`
and the resident data remain in the first code hunk.

## 2. Build commands

```sh
make clean && make          # release chip + card
make tools                  # p96screen, p96overlap, p96windowmove, rtgpresent
make abi-check              # GCC + vbcc ABI layout fixtures
make r3d-tools              # radeon3dinfo, radeon3dsessions, radeon3dphase1, radeon3dformats
make vramstream             # segment write throughput probe
make r3dstream              # streaming commit probe (vbcc)
make r3dreplay              # pre-serialized ceiling benchmark (vbcc)
make r3dtexupdate           # texture visibility gate
make r3dib                  # trusted indirect dispatch gate
```

A normal build produces the matched `Radeon9200.chip` and `Prometheus.card`
files in the repository root (the build tree is `build/`, the card is built
under `build/prometheus-card/`). `make clean` removes all four build
directories and the root artifacts.

## 3. Build matrix

| Command | Chip output | Card output | Build dir | Extra defines |
|---|---|---|---|---|
| `make` | `Radeon9200.chip` | `Prometheus.card` | `build/` | - |
| `make DEBUG=1` | `Radeon9200-debug.chip` | `Prometheus-debug.card` | `build-debug/` | `-DDEBUG`, links `-ldebug` |
| `make FASTWAIT=1` | `Radeon9200-fastwait.chip` | `Prometheus.card` | `build-fastwait/` | `-DRADEON_FAST_WAIT` |
| `make DEBUG=1 FASTWAIT=1` | `Radeon9200-debug-fastwait.chip` | `Prometheus-debug.card` | `build-debug-fastwait/` | both |
| `make DEBUG=1 PROBES=1` | `Radeon9200-debug.chip` | `Prometheus-debug.card` | `build-debug/` | `-DDEBUG -DRADEON_BOOT_PROBES=1` |

`FASTWAIT=1` omits the destination-cache flush after FIFO-empty/engine-idle
completion. It is a controlled experiment, not a release configuration.

`PROBES=1` compiles the boot-time engine experiments (MMIO/VRAM sampling, CP
no-op batches, CP function matrix, indirect-buffer matrix, fallback probe,
first-stream dumps). They run during `LoadMonDrvs` and were implicated in two
unrecoverable boot hangs on the physical machine. Default is off. **Never
install a probes-on debug chip as the active driver.** See
[`08-troubleshooting.md`](08-troubleshooting.md#7-debug-chip-boot-hangs).

### 3.1 DEBUG builds

- The resident name must stay `Radeon9200.chip` in every build, including
  DEBUG, because `minigl.library`'s `MGLInit()` opens the chip by that exact
  internal name and refuses to initialize if it fails. A debug chip installed
  under its own file name breaks every MiniGL client.
- Debug builds are installed **as** `Radeon9200.chip` with the standard card,
  and are identified by CRC, not by filename.
- The card build embeds `__DATE__ ", " __TIME__`, so its CRC changes per
  rebuild; verify a deployed card by the push CRC check, not a stable value.
- `RLOG()` (KPrintF) is a no-op in all builds; the chip is silent even in
  DEBUG. Diagnostics come from the passive `Radeon9200.Debug` port.

### 3.2 CI

`.github/workflows/ci-build.yml` builds release, debug and tools in the pinned
container and archives `Radeon9200.chip`, `Prometheus.card`,
`Radeon9200-debug.chip`, `p96screen` and the license/notice files.
`release.yml` and `coverity.yml` cover release packaging and static analysis.
The `Prometheus` directory is a git submodule (`mirq/Prometheus`, branch
`main`); check it out recursively.

## 4. Installation

Install `Radeon9200.chip` and `Prometheus.card` as a **matched pair**:

```text
LIBS:Picasso96/Radeon9200.chip
LIBS:Picasso96/Prometheus.card
```

The active monitor icon is `DEVS:Monitors/Radeon.info`; an icon beside the chip
library is **not** the active source on the validated setup. Preserve the
binary `DiskObject` by editing the icon with Workbench **Information**.

The validated ToolTypes are:

```text
BOARDTYPE=Prometheus
SETTINGSFILE=SYS:Devs/Picasso96Settings.9200
OUTPUT=VGA
DMASIZE=2M
CP=YES
HWSPRITE=YES
```

`BOARDTYPE` and `SETTINGSFILE` must be switched together. Changing only
`BOARDTYPE` can load the chip successfully but leave Workbench on the native
fallback screen because the expected modes are unavailable.

Keep the last known-good matched pair:

```text
LIBS:Picasso96/Radeon9200.chip.previous
LIBS:Picasso96/Prometheus.card.previous
```

Never maintain or restore only one `.previous` component. Before installing an
experimental pair, copy both active files to those exact names.

### 4.1 ToolTypes

Parsed by `Prometheus.card` before it loads `Radeon9200.chip`; matching is
case-insensitive and an entry wrapped in parentheses stays inert.

| ToolType | Default | Effect |
|---|---|---|
| `DMASIZE=<n>[K\|M]` | none | Reserves a 4096-byte-aligned shared Prometheus DMA arena at the high end of VRAM. **Required** for Radeon initialization; invalid/zero/unavailable/oversized fails init. Rounds up to 4096 bytes and must leave at least 4 MiB for Picasso96. |
| `CP=YES` | off | Initializes the R200 command processor used by Radeon3D. Picasso96 2D still uses direct MMIO. Without it, Radeon3D sessions cannot open. |
| `HWSPRITE=<YES\|NO>` | on | 64x64 ARGB RV280 hardware cursor; `NO` disables it, allocation failure falls back to software. |
| `HWTEXT=<YES\|NO>` | on | Hardware `BlitTemplate` host-data upload for text; `NO` leaves text to rtg.library's CPU default. |
| `TEXTSTAGE=<YES\|NO>` | **off** | Experimental VRAM glyph staging. **Known broken**: wedges the 2D engine on the reference machine. Do not enable outside diagnosis. |
| `OUTPUT=VGA` | VGA | Primary VGA DAC path. |
| `OUTPUT=DVI` | VGA | Internal-TMDS path for a validated RV280 COMBIOS profile; falls back to VGA when the ROM does not describe a supported connector/TMDS PLL table. |

`OUTPUT=VGA` is the safe default for an unvalidated board. Retain a serial
recovery path and use a conservative 640x480@60 mode for first boot.

`DMASIZE` accepts positive decimal bytes with an optional case-insensitive `K`
or `M` suffix; the final occurrence wins. It is published for peer PCI bus
masters; CP, cursor, segment and aux allocations are separate private
reservations.

### 4.2 DVI bring-up record

`PrmScan` on the reference board reports ATI RV280 `1002:5964` rev 1, 128 KiB
option ROM, legacy COMBIOS rev 8, connector-info table at ROM `0x0511`,
DFP/TMDS table rev 4 at `0x057c`, no external-TMDS table. The DFP table must
supply transmitter PLL values; do not substitute generic VGA PLL settings.
1920x1080 Workbench over DVI was validated stable and visually correct after a
cold reboot, and the programmable pixel-clock ladder (250 kHz steps from the
BIOS minimum to 164.75 MHz) was validated through Picasso96Mode. Horizontal
sync widths round to the nearest 8-pixel CRTC character clock (CEA-861 1080p60
44-pixel hsync programs as 48).

## 5. Recovery layers

Two layers exist in `S:startup-sequence`:

1. **Automatic.** The sequence writes `S:driver-boot-failed` just before
   `LoadMonDrvs` and clears it at boot end **only when `C:RTGPresent` reports
   an RTG screen**. If a boot stalls (marker survives) or the driver fails and
   the machine falls back to the native PAL display (`RTGPresent` returns WARN
   and writes `RAM:driver-fallback`), the next boot restores both `.previous`
   files to their active names and leaves a proof note in
   `RAM:driver-autorecovered`. Successful RTG boots never delete `RAM:` notes
   from earlier boots; check both note files when investigating an automatic
   restore.
2. **Manual.** Holding the right mouse button makes `C:TestRMB` return WARN,
   and the block copies both `.previous` files only when both recovery files
   exist. Do not hold the right mouse button for a normal experimental-driver
   boot.

`C:RTGPresent` locks the default public screen and treats mode ids with the RTG
flag (`0x80000000`) or a screen wider than 900 pixels as RTG; otherwise it
returns 5 (WARN). The deployment path is `C:RTGPresent` (AmigaDOS `C:`), not
the local `build/` path.

If a run also installs a new `LIBS:minigl.library`, execute `Avail Flush`
before launching clients or AmigaOS may retain and reopen the old resident
library despite a matching disk CRC.

## 6. Deployment rules for the physical machine

The reference target is a **physical 68060 Amiga**, not an emulator. Do not
use emulator reset, pause, state or lifecycle commands.

- Connect to the AmigaBridge endpoint explicitly (the current reference is
  `192.168.1.21:2345`; localhost defaults fail). A cold reboot normally takes
  80-100 s; poll with the wait script rather than sleeping a fixed interval,
  then reconnect explicitly.
- Run bridge transfers and filesystem copies **sequentially**. Concurrent copy
  requests can use or report the wrong destination.
- Use deployment names of at most 16 characters and verify the deployed CRC32
  before running. Longer names can alias after Amiga filesystem truncation and
  silently overwrite another test variant.
- `InitPPC` must run once per boot before launching any WarpOS/PPC client.
  Without it the launch hangs silently: the process ignores `Break`, holds its
  redirect log open, and every later PPC load fails with `Unknown command`.
  Check `Status` for leftover processes before diagnosing anything else after
  a boot, and recover from a wedged launch with a reboot, not another `InitPPC`.
- After a grey screen, guru or hard GPU hang, require an operator cold power
  cycle: a warm reboot does not reliably reset the Radeon and can leave
  windowed presentation broken while simpler tests still pass. Prefer each
  client's normal quit path. Do not break a running fullscreen GL client and
  immediately launch another; in-flight CP work can wedge the machine. After
  an interrupted GL client, run a small bootstrap/service probe before a
  heavier workload.
- Keep a native display or serial recovery path available for hardware tests.

Switch both monitor ToolTypes before a cold reboot (`BOARDTYPE=Prometheus`,
`SETTINGSFILE=SYS:Devs/Picasso96Settings.9200`).

## 7. First-boot checklist for a new board

1. `OUTPUT=VGA`, conservative 640x480 mode, valid positive `DMASIZE`.
2. Install the matched pair with both `.previous` files in place.
3. Cold boot; confirm an RTG Workbench and that `C:RTGPresent` reports OK.
4. Run `radeon3dinfo` and record library/interface version, generation, device
   id, caps, VRAM and `max_batch_dwords`.
5. Run `p96screen 8`, `16`, `32`, `8` and reject the build on any failure.
6. Only then try `OUTPUT=DVI` (if the ROM profile is supported) or enable
   `CP=YES` for 3D clients.

## 8. Version identification

Record for every run: git commit, dirty state, artifact sizes and hashes,
compile-time configuration, CPU/bridge/board/VRAM/ROM, ToolTypes, mode and
cold/warm state. The chip's `lib_IdString` is
`Radeon9200.chip 3.0 (2.9.2026)`; the Radeon3D interface number comes from
`Radeon3DInfo.Version`, not from the library version.
