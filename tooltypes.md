# Monitor ToolTypes

Picasso96 passes the active monitor icon's ToolTypes array to `InitCard()` in
`a1`. On the validated machine the source is:

```text
DEVS:Monitors/Radeon
DEVS:Monitors/Radeon.info    <- ToolTypes live here
```

An icon beside `LIBS:Picasso96/Radeon9200.chip` is not the active source on this
setup. Editing it does not configure this driver. If the monitor icon is
missing or its entries are inactive, `InitCard()` receives defaults without an
explicit ToolType error.

## Recognised ToolTypes

Parsed by `Prometheus.card` before it loads `Radeon9200.chip`. Matching is
case-insensitive, and an entry wrapped in parentheses remains inert.

| ToolType | Default | Effect |
|---|---|---|
| `DMASIZE=<n>[K\|M]` | none | Reserves a 4096-byte-aligned shared Prometheus DMA arena at the high end of VRAM. A valid positive arena is required for Radeon initialization; invalid, zero, unavailable, or oversized requests fail initialization. |
| `CP=YES` | off | Initializes the R200 command processor used by active Radeon3D clients. Picasso96 2D still uses direct MMIO. |
| `HWSPRITE=<YES\|NO>` | on | Controls the 64x64 ARGB RV280 hardware cursor. `NO` disables it; allocation failure falls back to the software cursor. |
| `HWTEXT=<YES\|NO>` | on | Controls the hardware `BlitTemplate` host-data upload used for text. `NO` leaves text to rtg.library's CPU default. Historical comparisons are in [`performance.md`](performance.md). |
| `TEXTSTAGE=<YES\|NO>` | **off** | Experimental: stages the glyph bitmap in VRAM and colour-expands it from memory instead of streaming it through `HOST_DATA`. **Known broken** - it wedges the 2D engine on the reference machine and leaves the Amiga unresponsive. Do not enable outside diagnosis. |
| `OUTPUT=VGA` | VGA | Selects the VGA output path. |
| `OUTPUT=DVI` | VGA | Internal-TMDS path for the validated RV280 COMBIOS profile. It uses CRTC0, FP shadow timings, and the board's DFP/TMDS PLL table. |

`OUTPUT=VGA` remains the safe default for unvalidated boards. `OUTPUT=DVI` is
restricted to the validated internal-TMDS COMBIOS profile and falls back to the
VGA path when the ROM does not describe a supported connector or TMDS PLL
table. Retain a serial recovery path and use a conservative 640x480@60 mode for
first boot on a new board.

`DMASIZE` accepts positive decimal bytes with an optional case-insensitive `K`
or `M` suffix. The final occurrence wins, rounds up to 4096 bytes, and must
leave at least 4 MiB for Picasso96. It is published for peer PCI bus masters;
Radeon CP and cursor allocations are separate private reservations below it.
If a peer created the fixed early arena first, Radeon adopts and excludes the
existing arena when it satisfies the requested size.

## RV280 DVI bring-up record

`PrmScan` on the reference board reports ATI RV280 `1002:5964`, revision 1,
with a 128 KiB option ROM. Its legacy COMBIOS revision 8 directory contains a
connector-info table at ROM offset `0x0511` and DFP/TMDS table revision 4 at
`0x057c`; the external-TMDS table is absent. This is the internal RV280 TMDS
route. The DFP table must supply the transmitter PLL values for digital modes;
do not substitute generic VGA PLL settings.

Physical DVI output was validated on this board after a cold reboot: the
1920x1080 Picasso96 Workbench is stable and visually correct. The programmable
pixel-clock ladder was also validated through Picasso96Mode; 250 kHz clock
steps are advertised from the BIOS-derived minimum to 164.75 MHz, keeping the
actual single-link TMDS clock at or below the 165 MHz limit. Horizontal sync
widths are rounded to the nearest 8-pixel CRTC character clock, allowing the
44-pixel CEA-861 1080p60 hsync to be programmed as 48 pixels.

## Editing the icon

Copy a valid Picasso96 monitor icon to `DEVS:Monitors/Radeon.info`, then edit it
with Workbench Information so its binary `DiskObject` structure is preserved.
The validated active entries are:

```text
BOARDTYPE=Prometheus
SETTINGSFILE=SYS:Devs/Picasso96Settings.9200
OUTPUT=VGA
DMASIZE=2M
CP=YES
HWSPRITE=YES
```

`BOARDTYPE` and `SETTINGSFILE` must be switched together. Selecting
`Prometheus` while leaving the other driver's settings file active can load
`Radeon9200.chip` successfully but leave Workbench on its native fallback
screen because the expected Radeon9200 modes are unavailable.

Back up the old icon before editing. Changes take effect after a cold reboot,
because `InitCard()` normally runs once while Picasso96 loads the driver.
`HWSPRITE=YES` remains in the validated list to make the desired state explicit,
but it may be omitted because hardware cursor support is enabled by default.

## Verifying delivery

A DEBUG build publishes `Radeon9200.Debug`. Its stats report the requested DMA
size, whether the arena was reserved, whether CP initialization succeeded, and
whether hardware-sprite callbacks were requested. Check those fields before
trusting a ToolType-dependent benchmark.
