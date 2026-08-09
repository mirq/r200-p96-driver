# 2D Performance Work

This file tracks real-hardware benchmark results, accepted optimizations, and
the order in which unvalidated acceleration candidates should be tested.
P96Speed automation is documented separately in [`p96speed.md`](p96speed.md).

## Reference Configuration

- Amiga 4000, 68060 at 50 MHz
- Picasso96, `rtg.library` 43.538
- RV280 Radeon 9200 `1002:5964`
- P96Speed 1.2, 640x480x16, 13 seconds per test

The closed-driver reference is `Work:P96Speed_org.txt`. The current validated
result is `Work:P96Speed_10pattern2.txt`.

## Current Gaps

| Operation | Closed driver | Validated driver | Remaining gap |
|---|---:|---:|---:|
| RectFill | 11881 | 2593 | 4.6x |
| RectFill Pattern | 11246 | 1387 | 8.1x |
| WritePixel | 134097 | 129429 | 1.04x |
| WriteChunkyPixels | 133 | 131 | 1.02x |
| WritePixelArray8 | 132 | 130 | 1.02x |
| WritePixelLine8 | 9034 | 8260 | 1.09x |
| DrawEllipse | 6543 | 4999 | 1.31x |
| DrawCircle | 7680 | 5465 | 1.41x |
| Draw | 19788 | 1276 | 15.5x |
| Draw Hor/Ver | 24543 | 4030 | 6.1x |
| ScrollRaster X | 1472 | 213 | 6.9x |
| ScrollRaster Y | 1555 | 215 | 7.2x |
| PutText | 8553 | 1018 | 8.4x |
| BlitBitMap | 13673 | 3416 | 4.0x |
| BlitBitMapRastPort | 7586 | 2597 | 2.9x |

CPU-written operations are already within about eight percent of the closed
driver. The remaining large gaps correspond to missing P96 callbacks or to
per-operation MMIO setup, not basic PCI framebuffer bandwidth.

## Accepted Changes

### Cross-surface source copies

`BlitRectNoMaskComplete` opcode `$C` is accelerated for disjoint, same-pitch
on-board surfaces. It improved:

- BlitBitMap: about 242 to 3483-3936 operations/second
- BlitBitMapRastPort: about 237 to 2573-2804 operations/second

All CLUT8, RGB565PC, and BGRA32 cross-surface tests pass.

### Coalesced FIFO reservations

Fill and copy submission reserve all required FIFO entries with one hardware
status read instead of three:

- Fill: one reservation for 9 writes
- Copy: one reservation for 10 writes

Five-run focused copy averages improved from 53.8 to 51.6 ticks (about four
percent). All cache flush and `WAIT_UNTIL` ordering remains unchanged.

### Direct-color solid fills

P96 direct-color pens are framebuffer-byte-order values. The Radeon brush
register expects the GPU's little-endian numeric pixel, so RGB565PC pens require
a 16-bit byte swap and BGRA32 pens require a 32-bit byte swap. With this
conversion enabled:

- RectFill improved from 102 to 2011 operations/second (19.7x).
- Draw Hor/Ver improved from 2939 to 4193 operations/second.
- OpenWindow improved from 25 to 46 operations/second.
- MoveWindow improved from 72 to 184 operations/second.
- SizeWindow improved from 35 to 49 operations/second.

Exact red/cyan edge tests and eight-color readback pass in RGB565PC and BGRA32,
followed by a successful return to CLUT8.

### 8x8 monochrome pattern fills

The hardware path accepts only:

- `JAM2`
- `Pattern.Size <= 3` (1, 2, 4, or 8 rows)
- identical high and low bytes in every 16-bit source row
- source pattern memory outside the framebuffer aperture
- CLUT8, RGB565PC, or BGRA32 destinations
- validated in-VRAM destination rectangles

Everything else synchronizes and uses `BlitPatternDefault`. P96Speed's 16x4
motif is representable by the RV280 8x8 monochrome brush:

```text
8888
2222
8888
2222
```

The 8/16/32/8 hardware sequence passes distinct foreground/background/guard
pens, horizontal and vertical phase, the far rectangle edge, all four outside
guard edges, complete row order for accepted heights 1, 2, 4, and 8, and CLUT8
partial write masks. A true 16-pixel pattern verifies that rejected input waits
for pending hardware before using `BlitPatternDefault`. P96Speed `RectFill
Pattern` improved from 37 to 1387 operations/second (37.5x), independently
proving that the hardware callback is active.

## Rejected Experiment

Removing the destination-cache flush and `WAIT_UNTIL` pair from every solid
fill was correct under synthetic queue stress but did not improve the real
workload. Two P96Speed samples produced RectFill results of 1460 and 1493,
below the validated 2011 result. The barrier removal has therefore been
reverted.

Do not reintroduce it without dependency/cache accounting and a repeatable
P96Speed improvement.

## Experimental CP Validation

The optional R200 command-processor backend was validated on hardware with the
release card CRC32 `0F6ABAAC` (29,444 bytes). The active monitor icon was
`DEVS:Monitors/Radeon.info`, CRC32 `7C5A7ED6` (545 bytes), with
`OUTPUT=VGA`, `DMASIZE=2M`, and `CP=YES`.

Live driver-state inspection after a cold boot confirmed that the private DMA
arena and CP state were both allocated. The CP ring had GPU address
`03E00000`, its initial write pointer was `00000020`, and its `Ready` flag was
one. This distinguishes the run from earlier measurements that silently used
the direct-MMIO fallback because `CP=YES` was absent.

The complete 8/16/32/8 sequence passed fill, pattern, mask, overlapping copy,
cross-surface complete-copy, guard, and direct-color readback tests. No crash
was recorded. Results are retained on the Amiga as:

| Result | CRC32 | Size | Fill256 | ScatterFill4096 | Copy256 |
|---|---:|---:|---:|---:|---:|
| `Work:p96screen_cp_active_8.txt` | `0EA404EC` | 4,677 | 19 | 40 | 20 |
| `Work:p96screen_cp_active_16.txt` | `25D1A6FE` | 4,618 | 20 | 36 | 26 |
| `Work:p96screen_cp_active_32.txt` | `F79C348B` | 4,619 | 32 | 37 | 41 |
| `Work:p96screen_cp_active_8return.txt` | `06879411` | 4,677 | 22 | 39 | 20 |

Five focused P96Speed RectFill samples at 640x480x16 and 13 seconds were 1972,
1984, 1954, 1928, and 1934 operations/second. Their mean is 1954.4 and median
is 1954. The 56 op/s range is 2.9 percent of the mean. The final saved report
is `Work:P96Speed_cp_active_rectfill.txt`, CRC32 `59BCCC53` (1,889 bytes).

The CP-active mean is 24.6 percent below the previous focused direct-MMIO
result of 2593 operations/second and 81.7 percent below the 10693 target. CP is
therefore hardware-correct but does not meet the RectFill performance goal
under rtg.library's per-operation `WaitBlitter` serialization. The controlled
CP-resident/direct-MMIO comparison is recorded below.

### CP-resident MMIO 2D and hardware cursor

The CP now remains initialized for future 3D work, but Picasso96 fill, pattern,
and copy callbacks submit exclusively through direct MMIO. `AccelPending` is a
backend value rather than a Boolean, so `WaitBlitter` drains the backend that
actually submitted the work instead of choosing from CP readiness.

The real RV280 hardware cursor removes rtg.library's software-sprite overlap
save/restore path. With `CP=YES`, `HWSPRITE=YES`, and MMIO 2D, three focused
RectFill samples were 3446, 3431, and 3395 operations/second. Their mean was
3424 and their 51 op/s spread was 1.5 percent. The final report is
`Work:P96Speed_hybrid_mmio_rectfill.txt`, CRC32 `B580AA45` (1889 bytes).

Making zero the explicit driver-owned `HOST_PATH_CNTL` baseline then removed
two redundant reads from every HDP invalidate while retaining a final readback
barrier. Three repeats were 3501, 3496, and 3466 operations/second: mean 3487.7,
median 3496, and a 1.0 percent spread. This is a 1.9 percent improvement over
the otherwise identical hybrid build and 78.5 percent over the earlier 1954.4
CP-active mean. The final report is
`Work:P96Speed_hostshadow_rectfill.txt`, CRC32 `02DF03DB` (1889 bytes).

DEBUG counters prove the intended split during these runs: `CpActive=1`, every
fill used the hardware callback, `FillSoftware=0`, and each fill issued exactly
nine MMIO writes. In the final instrumented build the whole `RadeonFillRect`
call averaged
61.3 us net and drain contribution averaged 44.4 us per fill, for about 105.7
us inside the driver. At 3487.7 operations/second the complete operation takes
286.7 us, leaving roughly 181 us above the driver callbacks.

The DEBUG hooks themselves bracket nested regions with `ReadEClock`, so release
performance must be measured separately. The deployed release card produced
4960, 4956, and 4964 operations/second: mean and median 4960, with an 8 op/s or
0.16 percent spread. This is 42.2 percent above the final instrumented mean and
153.8 percent above the original CP-active mean. The saved report is
`Work:P96Speed_release_mmio_rectfill.txt`, CRC32 `E374B4AB` (1889 bytes).

The final HDP sequence passed the full 8/16/32/8 fill, pattern, mask, overlap,
complete-copy, guard, and direct-color readback suite without a crash:

| Result | CRC32 | Size | Fill256 | ScatterFill4096 | Copy256 |
|---|---:|---:|---:|---:|---:|
| `Work:p96screen_v010final_8.txt` | `CDE47391` | 4675 | 4 | 35 | 5 |
| `Work:p96screen_v010final_16.txt` | `0506FCC6` | 4616 | 6 | 27 | 9 |
| `Work:p96screen_v010final_32.txt` | `2F66E5E5` | 4619 | 12 | 24 | 13 |
| `Work:p96screen_v010final_8return.txt` | `BC93CD8B` | 4675 | 6 | 32 | 5 |

The final release mean remains 53.6 percent below the 10693 target. The cursor and MMIO
changes remove the identified driver-side penalties, but the remaining gap is
still dominated by rtg.library/graphics.library work outside the callbacks.
Moving cursor state from a file-static singleton into per-board `CardData` did
not regress the hot path; the final DEBUG build produced a 3629 op/s smoke
sample after the complete readback sequence.

## Hardware Validation Record

- Current release card: CRC32 `2AC0183B`, 31260 bytes
- Current release result: `Work:P96Speed_release_mmio_rectfill.txt`, CRC32
  `E374B4AB`, 1889 bytes
- Release card: `Radeon9200.card.0.9-pattern`, CRC32 `B5ABB580`, 23088 bytes
- Previous rollback: `Radeon9200.card.0.8-fill`, CRC32 `D2A76396`, 21776 bytes
- Test utility: CRC32 `85036F1D`, 24392 bytes
- Complete P96Speed run: `Work:P96Speed_10pattern1.txt`, CRC32 `C69AA4CB`
- Accepted result after focused repeats: `Work:P96Speed_10pattern2.txt`, CRC32
  `70D67A67`

The complete run measured pattern fill at 1387 operations/second and solid fill
at 1427 operations/second. Repeating the individual tests produced 1387 and
2593 respectively. After a clean reboot, fully fenced `p96screen` complete-copy
timings were 41-45 ticks and fill timings were 18/21/41 ticks for CLUT8,
RGB565PC, and BGRA32, so there is no solid-fill or copy regression. Focused
timings taken immediately after P96Speed were temporarily much higher; reboot
before comparing these synthetic timings. Workbench returned normally after
every mode transition, and no crash was recorded.

## RectFill Performance Plan

### Where the time actually goes

RectFill is the largest remaining gap after the direct-colour and pattern work.
Measured at 640x480x16, 13 seconds, five focused runs on one boot:

| Run | 1 | 2 | 3 | 4 | 5 |
|---|---:|---:|---:|---:|---:|
| RectFill op/s | 1972 | 1984 | 1954 | 1928 | 1934 |

Mean 1954 op/s, spread 2.9 percent, saved as `Work:P96Speed_cp_active_rectfill.txt`.
That is 512 microseconds per operation against the closed driver's 11881 op/s,
or 84 microseconds. About 430 microseconds per operation is unexplained
overhead. The earlier 2593 figure in the gap table above has not been
reproducible on any subsequent boot and should be treated as an outlier, not a
regression baseline.

Per RectFill the driver issues roughly:

- `WaitFifo(9)` — at least one `RBBM_STATUS` read
- nine register writes to program the fill
- `RadeonWaitBlitter` to `WaitIdleAndFlush` — three poll loops over
  `RBBM_STATUS` and `DSTCACHE_CTLSTAT`, one flush write, then the HDP
  read-buffer invalidate at three reads and two writes

The working hypothesis is that PCI MMIO **reads** dominate. Reads are not
posted, so each one pays full bridge turnaround, while writes are posted and
comparatively free. Thirty to sixty reads per operation at a few microseconds
each accounts for the missing 430 microseconds. The competing hypothesis is that
the idle-and-flush handshake has an irreducible hardware latency. These are
distinguishable only by measurement, and everything below Phase 0 is contingent
on which one is true.

### Two null results to learn from

Both were changes made without the ability to measure what they targeted:

- The earliest CP-labelled runs did not have an active ring. It requires
  `data->DmaArena`, which requires `DMASIZE` in the active monitor icon. See
  [`tooltypes.md`](tooltypes.md). Those early results measured the direct-MMIO
  fallback; later `Radeon9200.Debug` statistics distinguish the backends.
- Removing the per-iteration `RadeonDelayUs(1)` from the `WaitFifo` and
  `WaitIdleAndFlush` poll loops produced no improvement. This is consistent with
  the drain being bounded by real GPU and bus time rather than by poll overhead:
  dropping the delay makes the loop spin more times at the same wall-clock cost,
  and burns PCI bandwidth doing it. The change is currently in the tree but is
  **unvalidated**; re-test it under Phase 0 instrumentation and revert it if it
  is still null.

### Phase 0 results

Implemented in `src/radeon_debug.{c,h}`: a DEBUG-only `PA_IGNORE` public port
named `Radeon9200.Debug` with a 124-byte stats block at port + 34, read from the
host by walking exec's PortList and decoded with
`tools/decode_debug_stats.py`. The release card is byte-identical with and
without the instrumentation (CRC32 `0F6ABAAC`), so it costs the shipping build
nothing.

First read, after a boot with the icon in place:

| Field | Value |
|---|---:|
| CpRequested / CpActive | 1 / **1** |
| DmaRequested / DmaReserved | 2097152 / 1 |
| BoardMemorySize | 65011712 |
| EClockRate | 709379 |

ToolType delivery works and **the CP is running for the first time**.
`BoardMemorySize` matches the `dmasize=2048k` row in [`dmasize.md`](dmasize.md)
exactly, which independently confirms the arena.

Measured costs, 2000 samples each:

| Operation | Cost |
|---|---:|
| MMIO read (`RBBM_STATUS`) | 2.73 us |
| MMIO write (`SCRATCH_REG1`) | 2.29 us |
| `ReadEClock` | 10.44 us (instrumentation only) |

**Posted writes are not cheap on this bridge.** A write costs 2.29 us against a
read's 2.73 us, only 16 percent less. The working hypothesis above — that
non-posted reads dominate and writes are comparatively free — is wrong.
Eliminating a write is worth almost as much as eliminating a read, which raises
the priority of the register shadowing in Phase 3.

Per-operation breakdown with the CP active, net of instrumentation:

| Stage | Time | Counted MMIO | Counted MMIO cost |
|---|---:|---|---:|
| Fill submit | 98.9 us | 2 reads, 1 write | 7.7 us |
| Drain (real, excluding no-ops) | 41.8 us | 4 reads, 2 writes | 15.5 us |

The fill submit is the problem, and it is not MMIO. About **91 us per fill is
unaccounted for inside `CpCommit()`**, because the two most expensive things it
does bypass `RadeonRead32`/`RadeonWrite32` and so are not counted:

1. the `pci_inl()` readback of the last ring dword (`src/radeon_cp.c:174`), a
   VRAM read back through the aperture, done purely as paranoia;
2. `host_to_pcicpy()` of 64 bytes, because `CP_RING_ALIGNMENT` pads every
   submission to 16 dwords when a fill needs 14.

For comparison, the direct-MMIO submit path is 9 writes plus one `WaitFifo`
read, about 23 us. **CP submission is currently a little over four times more
expensive than MMIO submission**, which fully explains why enabling the CP did
not help. The ring is not wrong in principle; it is being throttled by a
readback and an oversized copy on every single operation.

`DrainCount` is roughly twice `FillCount` because `SynchronizeEngine()` returns
immediately when `AccelPending` is false; the table above divides by the drains
that actually did work. The drain's 4 reads are one scratch poll plus the
three-read HDP invalidate, so shadowing `HOST_PATH_CNTL` alone saves 8.2 us per
operation.

#### Attribution during a real P96Speed run

The numbers above came from boot-time Workbench drawing. Sampling the counters
immediately before and after a focused 13-second P96Speed RectFill, on the
instrumented debug card with the CP active, gives the benchmark's own mix.
P96Speed reported **1992 op/s** and the driver counted 24840 fills over roughly
12.5 seconds, so every counted fill is benchmark work.

| Quantity | Per operation |
|---|---:|
| Elapsed (1992 op/s) | 502 us |
| `RadeonFillRect`, whole call | 101.1 us |
| `WaitBlitter` drain | 36.5 us |
| **Driver total, net** | **137.6 us** |
| Driver total, gross (adds ~41 us of `ReadEClock`) | ~179 us |

`FillSoftware` was **0**: every benchmark fill took the hardware path, so the
software-fallback theory is dead.

The important result is what is missing. The driver accounts for 36 percent of
elapsed time at most, which leaves **about 320 us per operation, roughly two
thirds, above our callbacks** in rtg.library and graphics.library. The closed
driver completes an entire RectFill in 84 us, which is far less than the time we
spend outside our own code. That overhead therefore cannot be intrinsic to
Picasso96 — something about how this driver presents itself is making rtg take a
slow path.

This caps every optimisation discussed above. Making `RadeonFillRect` and the
drain infinitely fast would move RectFill from 1992 to roughly 2900 op/s, still
four times short of 11881. **Micro-optimising the fill path cannot close this
gap.**

The first place to look is what `RadeonInstallCallbacks()` advertises
(`src/radeon_mode.c:851-856`): the driver clears `BIF_BLITTER`,
`BIF_HARDWARESPRITE`, `BIF_HASSPRITEBUFFER`, `BIF_VBLANKINTERRUPT`,
`BIF_INDISPLAYCHAIN` and others, and sets only `BIF_GRANTDIRECTACCESS` before
conditionally re-adding `BIF_BLITTER`. Capability flags and the set of
implemented callbacks determine which generic paths rtg emulates around every
operation. That investigation, previously Phase 4, is now the only work with
enough headroom to matter.

Revised priority based on this data:

1. **Find out why rtg.library spends ~320 us per operation above the driver.**
   Start with the capability flags and the set of installed callbacks. Nothing
   else can close a 6x gap.
2. Only then, inside the fill path: shadow `HOST_PATH_CNTL` (8.2 us per
   operation) and shadow the setup registers, which is worth more than
   previously assumed because writes cost nearly as much as reads.

Two items from the earlier draft are now retired:

- Dropping the `pci_inl()` ring readback is **not safe**. The ring lives in VRAM
  behind the aperture, so host writes traverse the card's HDP write path while
  the `CP_RB_WPTR` write goes down the register path. The aperture read is what
  drains HDP before the write pointer is bumped. It is a barrier, not paranoia.
- Shrinking `CP_RING_ALIGNMENT` buys nothing at 4 or 8, because a 14-dword
  payload rounds up to 16 either way. Only alignment 2 helps, worth about 4.6 us
  against an unquantified hardware risk.

More fundamentally, CP submission must write more dwords (8 for the fill plus 6
for the fence) than the MMIO path writes registers (9). At roughly 2.5 us per
PCI access in either direction, **the CP cannot beat direct MMIO for
per-operation-fenced work while the ring sits behind the aperture**. It would
need the ring in Fast RAM via PCI GART, which is a large project and is not
justified until the rtg overhead above is understood.

### Phase 0 — Observability, before any more optimisation

Three test rounds have been invalidated by not knowing what was running. Close
that first.

1. Report board `MemorySize` from `tools/p96screen.c` via the Picasso96 API. A
   2 MiB drop is direct proof that `DMASIZE`, and therefore ToolType delivery,
   is live — checkable without a reboot.
2. Publish CP status as a DEBUG-only public exec message port, created only when
   `RadeonCpInitialize()` succeeds. Use `PA_IGNORE` with no signal bit so it is
   safe to create from whatever task calls `InitCard()`, and remove it in
   `RadeonCpShutdown()`. It is then visible instantly over the bridge.
3. Time MMIO from inside the driver: N `RadeonRead32(RBBM_STATUS)` and N
   `RadeonWrite32` against `EClock`. This converts the read-cost hypothesis into
   a number and sets the budget for everything in Phase 1.
4. Instrument `RadeonFillRect` to accumulate read count, write count, and
   `EClock` ticks split between submit and drain, exposed through a memory
   region the bridge can read.

Exit criterion: a per-operation cost breakdown in microseconds. Do not start
Phase 1 without it.

### Phase 1 — Remove MMIO reads, if Phase 0 confirms they dominate

1. Software FIFO accounting, as the Xorg driver does. Track free slots in
   `RadeonBoardData` and only read `RBBM_STATUS` when the estimate is short.
   Saves the read in every fill submit.
2. Shadow `HOST_PATH_CNTL`. It is driver-owned and constant after init, so the
   HDP invalidate can drop from three reads and two writes to two writes.
3. Collapse the remaining drain polls. The `DC_BUSY` wait may already imply the
   `RBBM_ACTIVE` wait; confirm against the Xorg sequence before merging them.

### Phase 2 — Make completion observable without a PCI read

This is the structural fix and the strongest technical argument for finishing
the CP work. The drain has to observe GPU completion, and today it does so with
the most expensive operation available, repeated in a loop.

1. Enable scratch write-back. `RADEON_SCRATCH_ADDR` and `RADEON_SCRATCH_UMSK`
   make the GPU DMA its scratch registers into host memory. Point them at a Fast
   RAM buffer and the fence check becomes a normal CPU memory read of a few
   nanoseconds instead of a multi-microsecond PCI read. `CpConfigureRing()`
   currently sets both to 0, so write-back is disabled
   (`src/radeon_cp.c:195-197`).
2. Enable ring read-pointer write-back via `CP_RB_RPTR_ADDR`, removing the
   `CP_RB_RPTR` read in `CpReserve()`.
3. Drop the paranoid `pci_inl()` ring readback in `CpCommit()`
   (`src/radeon_cp.c:174`) and track the write pointer purely in software.

Note that this reframes the CP: its value on this platform is not batching,
which P96's per-operation `WaitBlitter` denies us anyway, but replacing an
expensive MMIO poll loop with a cheap Fast RAM poll. Keep the ring itself in
VRAM so the GPU fetches locally; only the write-back targets belong in Fast RAM.

### Phase 3 — Reduce setup writes

Lower value than Phase 2 because writes are posted, but cheap to do.

1. Use `DEFAULT_PITCH_OFFSET` and the default scissor registers for fills whose
   pitch and offset are unchanged, dropping `DST_PITCH_OFFSET` from the
   per-operation sequence.
2. Shadow `DP_GUI_MASTER_CNTL`, `DP_WRITE_MASK`, `DP_CNTL` and the brush colour,
   and skip writes when unchanged. P96Speed's RectFill loop holds pen and format
   constant, so most of the nine writes are redundant.

### Phase 4 — Question the per-operation fence

Confirm from the CardDevelop documentation whether `WaitBlitter` after every
`RectFill` is required by the P96 contract or is a consequence of the capability
flags this driver advertises in `BoardInfo`. If the fence can legitimately be
deferred to the point of CPU framebuffer access, that removes the drain from the
hot path entirely and dominates every other item here. Treat any change in this
area as correctness-critical, and note that the barrier-removal experiment
already recorded under "Rejected Experiment" failed on the workload.

## Planned Follow-up Work

1. **DrawLine**: use the Radeon line engine and the now-proven direct-color pen
   conversion. This targets Draw and may also improve circles and ellipses.
2. **BlitTemplate**: implement bounded monochrome host-data expansion for text.
   This targets PutText and CON output, but requires special care because P96's
   `BIF_SYSTEM2SCREENBLITS` warning applies to classic bridges.
3. **Copy hazard tracking**: batch copies only when pending destinations cannot
   alias later sources. This targets scrolling and the remaining bitmap gap.
4. **BlitPattern extension**: consider JAM1, complement, inverse-video, and true
   16-pixel patterns without weakening the validated narrow fallback boundary.
5. **Broader minterms and pitches**: extend `BlitRectNoMaskComplete` to all 16
   ROPs and independently valid source/destination pitches.
6. **InvertRect**: add destination-only `ROP3_Dn`; low risk, but no dedicated
   P96Speed row makes it lower priority.
