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
| `DMASIZE=<n>[K\|M]` | none | Hides a 4096-byte-aligned tail of VRAM from Picasso96 for private driver use. Required by the CP ring. A malformed value is rejected non-fatally. See [`dmasize.md`](dmasize.md). |
| `CP=YES` | off | Initializes the R200 command processor for future clients. Picasso96 2D still uses direct MMIO. |
| `HWSPRITE=<YES\|NO>` | on | Controls the 64x64 ARGB RV280 hardware cursor. `NO` disables it; allocation failure falls back to the software cursor. |
| `HWTEXT=<YES\|NO>` | on | Controls the hardware `BlitTemplate` host-data upload used for text. `NO` leaves text to rtg.library's CPU default, which is faster only for single-character `Text()`. See [`performance.md`](performance.md). |
| `TEXTSTAGE=<YES\|NO>` | **off** | Experimental: stages the glyph bitmap in VRAM and colour-expands it from memory instead of streaming it through `HOST_DATA`. **Known broken** - it wedges the 2D engine on the reference machine and leaves the Amiga unresponsive. Do not enable outside diagnosis. |
| `OUTPUT=VGA` | VGA | Selects the VGA output path. |

`OUTPUT=VGA` is retained as a documented no-op for existing monitor icons; VGA
remains the only supported output.

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
