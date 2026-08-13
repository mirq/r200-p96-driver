# Prometheus Radeon DMA Integration Plan

## Architecture

`Prometheus.card` remains the Picasso96 card driver and the single owner of
Prometheus PCI discovery, board claiming, interrupt registration, and the
public DMA allocator reached through `prometheus.library`.

`Radeon9200.chip` is the RV280 chip driver. It receives mapped BARs and a small
handoff record in `BoardInfo.CardData`, stores its private state in
`BoardInfo.ChipData`, initializes the Radeon hardware, and installs Picasso96
callbacks. It does not enumerate or claim the PCI board through OpenPCI.

Initialization is deliberately split:

1. `Prometheus.card` finds an ATI `1002:5960`, `1002:5961`, or `1002:5964`
   board and validates framebuffer BAR0 and MMIO BAR2.
2. It opens `Picasso96/Radeon9200.chip` and calls `InitChip()`.
3. The chip probes installed VRAM and returns the final usable memory span.
4. `Prometheus.card` validates, aligns, and reserves `DMASIZE` from that span.
5. It calls the Radeon feature vector. CP, cursor, and acceleration resources
   use separate private VRAM below the already-published Prometheus DMA pool.

This order prevents the chip from allocating private resources before the card
driver knows the final reservation and prevents Picasso96 or Radeon graphics
operations from using the reserved high-VRAM tail.

## DMA Contract

The existing public API and LVOs remain unchanged:

| API | Card LVO | Contract |
| --- | ---: | --- |
| `Prm_AllocDMABuffer(size)` | `AllocDMAMem`, `-42` | Returns a CPU pointer in the reserved PCI aperture |
| `Prm_FreeDMABuffer(ptr, size)` | `FreeDMAMem`, `-48` | Requires the original pointer and exact requested size |
| `Prm_GetPhysicalAddress(ptr)` | library `-108` | Converts the CPU pointer to the PCI bus address |

The allocator uses 4096-byte pages. Each request is overflow-checked and
rounded up to pages without adding an unnecessary extra page. Allocation uses
best-fit contiguous runs. Free validates range, page alignment, run start,
continuation metadata, and exact original size. Every semaphore path releases
the lock, including invalid and duplicate frees.

`DMASIZE` accepts decimal bytes with one optional `K` or `M` suffix. Parsing is
strict and overflow-checked. The final occurrence wins. Invalid, zero,
unavailable, or oversized requests leave DMA disabled without preventing
display initialization. Reservations are page-aligned and must leave at least
4 MiB for Picasso96.

## Implementation Phases

1. Convert the Radeon resident, target, and function table from `.card` to
   `.chip`.
2. Move Radeon private data from `CardData` to `ChipData` and keep the compile
   time 64-byte ABI check.
3. Replace OpenPCI discovery/configuration with the Prometheus board handoff
   and `prometheus.library` config/address services.
4. Add Radeon enumeration and two-stage chip initialization to
   `Prometheus.card`.
5. Replace the legacy DMA chunk allocator with the checked page-run allocator.
6. Make `DMASIZE` parsing and high-tail reservation strict and transactional.
7. Preserve the public Prometheus DMA ABI and verify generated LVO ordering.
8. Build both components with the m68k cross compiler and treat Radeon warnings
   as errors.

## Host Verification

1. Run `make clean && make` in the root repository.
2. Confirm the output is `Radeon9200.chip` and the resident contains
   `Radeon9200.chip 1.0`.
3. Inspect symbols and disassembly for `InitChip` at chip LVO `-30` and
   `InitRadeonFeatures` at LVO `-36`.
4. Compile all modified `Prometheus.card` sources with the m68k compiler.
5. Run `git diff --check` in both repositories.
6. Confirm `Prometheus.card` still exports allocate/free at `-42` and `-48`.

## Real-Hardware Validation

Use a cold boot on each supported Prometheus/FireStorm target. Do not infer DMA
correctness from allocation alone.

1. Install `Prometheus.card` and `Picasso96/Radeon9200.chip`; select
   `BOARDTYPE=Prometheus` with the matching Picasso96 settings file.
2. Boot with no `DMASIZE`; verify Radeon VGA startup, Workbench, 8/16/32-bit
   modes, and the existing P96 acceleration tests.
3. Boot with `DMASIZE=2M`; verify Picasso96 reports exactly 2 MiB less usable
   memory while the full aperture remains mapped.
4. Test `DMASIZE=1`, `4096`, `64K`, `1M`, `0`, malformed suffixes, decimal
   overflow, and a request that violates the 4 MiB display floor.
5. Run allocator tests for zero/overflow requests, exhaustion, exact reuse,
   adjacent-run recovery, wrong size, interior pointer, and duplicate free.
6. Verify every allocation is page aligned, inside the reserved tail, and does
   not overlap another live allocation or Picasso96 memory.
7. Translate each CPU pointer with `Prm_GetPhysicalAddress()` and verify the bus
   address is within the Radeon framebuffer BAR.
8. Enable `CP=YES` only after base display validation. Verify ring allocation,
   bus-master enable, CP idle/stop during teardown, and direct-MMIO fallback.
9. Verify hardware cursor allocation and updates with `HWSPRITE=YES` and
   software fallback with `HWSPRITE=NO`.
10. Exercise a real second PCI bus-master device using descriptor rings and
    bounce buffers from `Prm_AllocDMABuffer()`. Check endian conversion, guard
    bytes, interrupt teardown, device reset, and that no free occurs before the
    DMA engine is inactive.
11. Repeat warm and cold boots and record bridge model, CPU, Radeon PCI ID,
    VRAM size, tooltypes, and results.

## Acceptance Criteria

- Radeon is loaded only as `Radeon9200.chip` behind `Prometheus.card`.
- Prometheus owns PCI enumeration, board claim, interrupt registration, and DMA
  pool lifetime.
- Radeon state uses `ChipData`; Prometheus handoff data uses `CardData`.
- `DMASIZE` works for Radeon and remains available to existing supported chips.
- Invalid and duplicate frees cannot deadlock the allocator.
- Allocation and reservation arithmetic cannot wrap.
- The public Prometheus DMA ABI remains compatible.
- Display and DMA teardown stop hardware users before memory is released.
- Real peer-device DMA is demonstrated before claiming general bus-master DMA
  support.
