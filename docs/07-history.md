# 07 - History, decisions and open work

This is the condensed chronological record. It exists so a new developer can
understand **why** the driver looks the way it does, and which experiments
already failed. It replaces the older per-topic progress files that used to sit
in the repository root.

All artifact CRCs are historical identities of specific binaries, not current
values. A rebuild changes them.

## 1. Interface evolution

| Interface | Added | Key capabilities |
|---:|---|---|
| 1 | first service | Discovery, PACKET2/immediate triangle submission, fences, bitmap import |
| 2 | semantic records | `Radeon3DExecute`, clear + triangle records |
| 3 | depth functions | Eight depth comparisons |
| 4 | texture state | 15-dword header, samplers, mip levels, alpha test, blending |
| 5 | fog/multitexture | 21-dword header, unit 1, fog, diagnostic invalidation |
| 6 | color formats | CLUT8 (RGB332) and B8G8R8A8 targets |
| 7 | native primitives | Triangle strip/fan |
| 8 | quad lists | Native quads, perspective clip coordinates |
| 9 | hardware TCL | 44-dword header, points/lines, hardware transform+clip |
| 10 | texgen | Object-linear texgen block |
| 11 | normals/lighting | Normal matrices, fixed-function lighting |
| 12 | sphere map | Sphere-map texgen, compact TCL vertex |
| 13 | streaming | Segment pool, `CommitDraw`/`CommitBatch` |
| 14 | state reuse | 3-dword reuse records |
| 15 | state batches | `CommitStateBatch` |
| 16 | ordered commits | In-order commit retirement |
| 17 | aux surfaces | `AllocSurface` from a private 4 MiB pool |
| 18 | indirect dispatch | Trusted `CP_IB_BASE` packets, multi-fence, render transitions |
| 19 | fence coalescing | `NO_FENCE` + `SubmitFence` - **parked, not deployable** |

## 2. Milestones

### 2.1 Bring-up (interfaces 1-2)

- Prometheus handoff validation, legacy COMBIOS init, CRTC0 VGA, 2D
  acceleration, hardware cursor, bounded ROM selection.
- First physical run on 68060/RV280 `5964`: MMIO read/write 1.45/1.33 us,
  8 KiB aperture 6.29 MiB/s, 4096-dword buffered CP 3.860 ms vs direct
  3.170 ms (17.9% saving), forced ring wrap and ordered fences pass.
- `rtg.library` 43.787 has a no-op `LibExpunge`, so `BoardInfo` is
  system-lifetime on the target; sessions still pin it for revisions that can
  expunge (`RADEON3D_CAP_OWNER_PINNED`).
- First triangle readback: 45,056 changed bytes, center pixel `0xffff`.
- `r200test` windowed Gouraud triangle: 7.259 FPS, present time dominating.
- Phase 2 scene (GPU clear + depth + texture + blending): fullscreen
  640x480 improved from 4.353 FPS (CPU clear) to **34.322 FPS**.

### 2.2 MiniGL phases (interfaces 3-12)

- Phase 4 transforms/clipping/depth, Phase 5 textures/blending, Phase 6
  fog/multitexture/primitives, Phase 7 lighting, interface 12 sphere map.
- Fixed-function lighting bug: the emitter enabled TCL lighting but omitted
  `R200_OUTPUT_COLOR_0`, so lit pixels equaled unlit. Fixed in
  `EmitExecuteState()`; validated with `07A7193D` / card `18A453D6`
  (phase7 passed, V19 parity 31/31, 32bpp suite 24/24).
- Sphere-map probe defect: the first probe asserted a normal parallel to the
  eye vector gives (0.5,0.5); the actual reflection `f = u - 2n(n.u)` gives
  `-u` for a unit normal, matching Mesa `t_vb_texgen.c`. The probe was wrong,
  not the hardware. This is the canonical example of checking expectations
  against Mesa before trusting a probe.

### 2.3 Execute optimisation (interface 7-8)

- Validation and command generation now share one traversal; the emitter
  retains the previous draw state within one call (resets every call).
- `MaxBatchDwords` raised 4096 -> 8192. A 16384-dword test reduced gears
  submits from three to two but changed the three-run mean only from 4.930 to
  4.932 FPS while increasing execute ticks, so 8192 was kept for board-lock
  fairness.
- Interface 8 quad lists: workload fell 3320 -> 2760 submitted vertices,
  20336 -> 16946 record dwords, 21834 -> 18338 generated dwords; FPS
  4.971/4.963.

### 2.4 Streaming submission (interface 13-16)

T0 probe decision (`vramstream`, 2026-08-24): VRAM store throughput is flat at
~6.1 MB/s from 8 KiB to 1 MiB blocks; byte-swapped stores cost ~752 ns/dword.
Path 1 (client writes vertices directly into service VRAM segments) chosen over
Path 2 (service copies) because the copy rate equals the store rate while
adding a second buffer and a pass.

T1-T5 implementation and fixes (all preserved in the source):

1. Segment pool must be reserved before Picasso96 allocates, or segments
   overlap later bitmaps.
2. Segment vertex data is little-endian; the frontend byte-swaps in registers
   before the single VRAM store (inline Execute hides this in the ring writer).
3. `LOAD_VBPNTR` needs one descriptor per active attribute; a single descriptor
   cannot express components < stride for textured records.
4. This RV280's texture cache never re-reads rewritten lines at the same
   address; neither register rewrites nor an HDP invalidate evict them, so
   in-place texture updates re-import residency at a fresh address (the stale
   block is kept alive until the replacement exists). `RADEON3D_TEX_CONTENT_*`
   serials force texture state re-emission when a surface is rewritten in
   place.
5. Residency re-import changes only the surface handle; re-emitting the full
   ~900-dword state per update dominated Quake's lightmap uploads, so the
   service detects texture-only deltas and re-emits just the texture atoms.

T5 results: r3dstream raw commits, quad_list VBUF commits, phase5/6 suites
passed; gears fullscreen 640x480x16 improved from a 6.118 FPS clean baseline to
6.897 FPS median (+12.7%); Quake demo1 stayed ~2.985 FPS and draw-bound, so the
next lever is per-draw state emission, not vertex transport.

State-batch/ordered-commit ladder (2026-08-30, 800x600x32 windowed gears, 300
frames each): state batches + ordered commits median **52.456 FPS**, state
batches + drain-before-submit 47.845, inline fallback 39.621.

### 2.5 Phase 2 validated client streams (interface 17): implemented, rejected

Interface 17 (PPC-side CP emission, validated client streams) was implemented
and passed every correctness gate: native validator suite, cross-CPU emitter
proofs, 1,350 submitted streams with zero rejects, all native MiniGL suites.
The performance gate failed: 46.8 FPS median vs the 52.456 accepted baseline.
Attribution: `RadeonCpWait` drain 4.2 ms/frame, 2.5 us/dword validator walk
2.7 ms/frame, trusted copy 1.9 ms/frame.

A CP-to-CP prepare skip in `RadeonPrepare3D` recovered +3.9% on the stream
configuration and is semantically correct (ring ordering plus `CpReserve`
back-pressure; the next Picasso96 operation drains via `Need2DRestore`). It is
retained on mainline but was neutral on the accepted baseline (the per-frame
present blit already forces the drain). The full Phase 2 implementation is on
branch `phase2-cp-stream` (commit `32afdf1`).

### 2.6 Private working memory (2026-09-07)

The Sonnet allocator hook redirects `PUBLIC`/`FAST` requests to PPC RAM even
with `LOCAL` set, so the driver's private execute buffers landed in PPC RAM and
the 68k paid PCI-memory latency. V1 (`MEMF_PUBLIC|FAST|LOCAL`) failed because
the hook turned `0x105` into `0x2105`. V2 requests only `MEMF_LOCAL` and
verifies FAST with `TypeOfMem`. Result: 44.313 -> 54.644 FPS median (+23.3%)
across six bridge reboots; see
[`04-performance.md`](04-performance.md#7-ppc-memory-placement).

### 2.7 Texture-matrix and fused CP copy (2026-09-07)

- `TextureOnlyDelta()` promoted a texture-only capture and returned before the
  common matrix-upload code, so two state-bearing records in one call could
  change both texture and transform but draw the second object with the old
  matrix. The premature return was removed; all five matrix caches stay
  independently checked. Regression proof: 1,188 checks, 0 failures; the
  mutant produced 139 failures.
- `CpBurstCopySwapped()` was fused (movem load, in-register swap, movem store),
  shrinking 212 -> 160 bytes and CP submit from 2.582 to 1.152 ms/frame
  (+5.2% overall: 54.446 -> 57.251 FPS median).

### 2.8 Indirect render (interface 18, 2026-09-10/12)

Prerequisites: descriptor snapshot before prepare, post-prepare revalidation
(generation/board/CP/lease), even-count enforcement with producer-side PACKET2
padding, render transitions (`INDIRECT_RENDER`). Physical validation with chip
`CEB838A6`: PPC-generated triangle readback passed, texture-update acceptance
passed 7,127 checks, and a 600-frame CP-emitter gears run completed 2,031
indirect submissions and 511,620 CP dwords at 68.337 FPS with no errors.
Native bootstrap passed afterwards.

### 2.9 Debug-chip boot hangs (2026-09-17/18)

A DEBUG chip was installed as the active pair and the machine did not return
from boot; an operator cold power cycle was required. Two defects found:

1. The boot-time indirect-buffer probe restored the stale `0x4d4d` CSQ cache
   partition instead of `CP_CSQ_CACHE_PARTITION` (`0x5010`), leaving the CP
   starved for the whole boot.
2. `RadeonDebugOpen` unconditionally ran MMIO/VRAM sampling, CP no-op batches,
   the CP function matrix, the indirect-buffer matrix and the fallback probe
   during `LoadMonDrvs`.

Both are now opt-in behind `RADEON_BOOT_PROBES` (`make DEBUG=1 PROBES=1`,
default 0). A second hung boot with probes off isolated libdebug's KPrintF
path, so `RLOG()` is now a no-op in all builds. Any future debug-chip install
must use the probes-off build first and requires an explicit go-ahead for the
cold boot.

### 2.10 Automatic driver recovery (2026-09-18)

`S:startup-sequence` gained the marker-based automatic restore and the
`C:RTGPresent` RTG-fallback check described in
[`05-build-deploy-run.md`](05-build-deploy-run.md#5-recovery-layers). The first
failsafe version cleared the marker at the end of any boot reaching `EndCLI`;
hardware showed a failed RTG driver falls back to PAL without stalling, so the
marker was cleared and the failed pair retried. `RTGPresent` now gates the
clear.

## 3. Parked and rejected work

### 3.1 Interface-19 fence coalescing (parked)

Commit `d758199` ("interface-19 fence coalescing + 2D drain guard (parked,
default-off)") added:

- `RADEON3D_INDIRECT_NO_FENCE`: dispatch without the per-dispatch fence tail.
- `Radeon3DSubmitFence()`: a fence-only submission closing a run.
- `RadeonCpState.PendingUnfenced`, `RadeonCpUnfencedPending()`,
  `RadeonCpWaitDrained()`: the 2D transition drains fence-less work with
  `CpWaitGuiIdle()` (not `RB_RPTR`, because the CP advances the read pointer
  before an IB fetch completes).
- Host tests `test_indirect_dispatch.py` (94 cases) and
  `test_cp_drain_guard.py` (20 checks).

Status: **parked, not for deployment.** Three hardware attempts at group=2/4
produced a hard wedge, corrupted rendering and a silent submission stall.
Back-to-back indirect fetches without an intervening idle wait are not viable
on this CP/stack. The feature is inert on the shipped stack because the
capability bit is only advertised at interface 19 and the deployed driver does
not carry it.

### 3.2 TEXTSTAGE

The VRAM glyph-staging experiment wedges the 2D engine on the reference
machine. Off by default, documented as broken.

### 3.3 Other rejected/negative results

- 16384-dword batches: no measurable gain over 8192, higher execute ticks.
- Interface-17 client CP streams: correctness passed, performance failed
  (section 2.5).
- `-m68060` emitter CPU flags: matching CRCs and a smaller CPU loop, but a
  separate optimisation candidate that was never mixed into a driver change.
- `FASTWAIT=1` cache-flush omission: controlled experiment only.
- A ring-full timeout is currently treated as CP death and runs full recovery,
  invalidating the session. This is too aggressive for back-pressure and
  remains an open robustness item.

## 4. Open optimisation candidates

From the measured attribution (see
[`04-performance.md`](04-performance.md)):

1. **Merge the commit-chain structural walk into the emitter.**
   `CommitRecords()` walks every record to count draws and verify the offset
   table, then `Radeon3DEmitStream()` parses the same chain again. Have the
   emitter count draws as it consumes, guard `CommitDrawIndex < recordCount`,
   and compare the final count before submission. Removes one complete
   cached-memory pass.
2. **Atom-level output reservation in the emitter.** Every `ExecuteEmitWord()`
   bounds-checks and post-increments `Count`; reserve capacity once per state
   block/matrix/vertex batch and commit `Count` once. Identified as likely
   worth more than hand-written assembly.
3. **2D per-`RenderInfo` surface-layout cache.** `ValidateSurface()` recomputes
   format datatype, limits, pitch/address alignment and biases on every 2D
   callback. Layout is invariant per (board, memory, pitch, format, sizes,
   `FramebufferGpuBase`); key a cache on that, as `ValidateLineSurface()`
   already does. Highest-value item for small fills, glyphs and fragmented
   `ClipRect` workloads (the measured `p96overlap` text gap).
4. **Single classified state comparison.** `EmitExecuteStateCached()` calls
   `SameExecuteState()` and, on mismatch, `TextureOnlyDelta()`, re-comparing
   most fields including all enabled light blocks. One comparison returning
   `equal | texture-only | changed` cuts per-record build cost for multi-draw
   streams with lighting.
5. **Fused 68060 CP ring kernel.** A hand-written kernel could load source
   dwords into `d0-d7`, swap in registers and issue one `movem.l` store. The
   prior movem work bought 13.9% of submit; expect a few percent, not 2x. The
   final-dword readback before `WPTR` must stay.
6. **Ring back-pressure vs recovery.** Distinguish "ring full, retry later"
   from "CP dead" so a paced client cannot lose its session to a hitch.
7. **PPC-resident private metadata.** The session/device structures and some
   surface tables still live in PPC RAM on a WarpOS host; only the three
   execute buffers were moved. The same placement policy could extend further.
8. **Texture-update allocation/coalescing.** Needs its dedicated workload and
   ownership proof.

## 5. Artifact identity log (selected)

| Date | Artifact | Bytes | CRC32 | Note |
|---|---|---|---:|---|
| 2026-08-17 | chip | 52,836 | `1E15D4DC` | interface 7, 8192 cap, 4.930 FPS |
| 2026-08-21 | chip | 55,016 | `B0FE2D03` | interface 8 quad lists |
| 2026-08-24 | `r3dstream` era | - | - | T5 streaming acceptance |
| 2026-09-07 | chip | 76,588 | `07A7193D` | pre-local-memory baseline |
| 2026-09-07 | chip | 76,664 | `23ACBAEC` | local execute memory (+23.3%) |
| 2026-09-07 | chip | 76,760 | `6B860EE0` | texture-matrix fix |
| 2026-09-07 | chip | 76,708 | `9768419A` | + fused CP copy (+5.2%) |
| 2026-09-10 | chip | 77,940 | `7f253b21...` (SHA-256) | interface-18 prerequisites, host-only |
| 2026-09-12 | chip | 77,940 | `CEB838A6` | indirect render physical validation |
| 2026-09-17 | release chip | 77,928 | `F46026B4` | boot-safe debug work baseline |
| 2026-09-17 | debug chip, probes off | 86,504 | `1FE4C2D3` | boot-safe debug class |
| 2026-09-17 | debug chip, probes on | 90,052 | `A7844F5F` | `T:` dumps; never install as active |
| 2026-09-20 | chip | 77,940+ | - | interface-19 parked work (commit `d758199`) |

Matched cards used across these runs: `18A453D6` (known-good), `1DC1ABC0`
(debug card). The card build embeds a date/time, so card CRCs change per
rebuild.

## 6. Working-tree note (documentation snapshot)

At the time this documentation was written, the working tree contained an
uncommitted debug instrumentation block in `src/radeon3d_emit.c`: ten global
`R200EmitDbg*` variables recording the last `Vbuf` descriptor decision
(options, vertex state/count, textured/normal/compact, stride, color/ST
offsets, texgen) for the consumer-side record logger, plus a depth-only clear
fix (`RADEON3D_FRAGMENT_BLEND` with src ZERO/dst ONE and a forced state
re-emission) for the black-boxes-behind-HUD defect. Both must be committed or
reverted before a release; if they change the ABI or performance, update the
relevant documents.

The same pass fixed a pre-existing gate failure: `tools/radeon3d_abi_check.c`
still asserted `RADEON3D_IFACE_VERSION == 18` after commit `d758199` moved the
header to 19, so `make abi-check` failed. The fixture now asserts 19 and also
covers `INDIRECT_RENDER`, `MULTI_FENCE`, `FENCE_COALESCE`,
`RADEON3D_INDIRECT_NO_FENCE`/`RADEON3D_INDIRECT_FLAGS`, and the
`Radeon3DSubmitFence` inline call. `make abi-check` passes for GCC and vbcc.
