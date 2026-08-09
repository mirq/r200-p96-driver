# Monitor ToolTypes

Picasso96 passes the active monitor icon's ToolTypes array to `InitCard()` in
`a1`. On the validated machine the source is:

```text
DEVS:Monitors/Radeon
DEVS:Monitors/Radeon.info    <- ToolTypes live here
```

An icon beside `LIBS:Picasso96/Radeon9200.card` is not the active source on this
setup. Editing it does not configure this driver. If the monitor icon is
missing or its entries are inactive, `InitCard()` receives defaults without an
explicit ToolType error.

## Recognised ToolTypes

Parsed by `ParseOptions()` in `src/radeon9200.c:130`. Matching is a
case-insensitive prefix compare (`MatchOption()`, `src/radeon9200.c:78`), so an
entry wrapped in parentheses is inert and can be used as an in-icon comment.

| ToolType | Default | Effect |
|---|---|---|
| `DMASIZE=<n>[K\|M]` | none | Hides a 4096-byte-aligned tail of VRAM from Picasso96 for private driver use. Required by the CP ring. A malformed value is rejected non-fatally. See [`dmasize.md`](dmasize.md). |
| `CP=YES` | off | Initializes the R200 command processor for future clients. Picasso96 2D still uses direct MMIO. |
| `HWSPRITE=YES` | off | Enables the 64x64 ARGB RV280 hardware cursor, with software-cursor fallback on failure. |
| `OUTPUT=VGA` | VGA | Selects the VGA output path. |

`OUTPUT=` is the one dangerous entry. `ParseOptions()` defaults `VgaOutput` to
TRUE, but if the ToolType is *present* with any value other than exactly `VGA`,
`InitCard()` releases the board and returns FALSE — the card does not come up at
all. Prefer omitting it over risking a typo; the default is already correct.

## Editing the icon

Copy a valid Picasso96 monitor icon to `DEVS:Monitors/Radeon.info`, then edit it
with Workbench Information so its binary `DiskObject` structure is preserved.
The validated active entries are:

```text
BOARDTYPE=Radeon9200
OUTPUT=VGA
DMASIZE=2M
CP=YES
HWSPRITE=YES
```

Back up the old icon before editing. Changes take effect after a cold reboot,
because `InitCard()` normally runs once while Picasso96 loads the driver.

## Verifying delivery

A DEBUG build publishes `Radeon9200.Debug`. Its stats report the requested DMA
size, whether the arena was reserved, whether CP initialization succeeded, and
whether hardware-sprite callbacks were requested. Check those fields before
trusting a ToolType-dependent benchmark.
