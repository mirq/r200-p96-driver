# Streaming Submission Migration Plan

Status: **T1 complete, T2 implemented and partially validated** — interface 13 on `streaming-submit`, write-through frontend on `streaming-frontend`
(84ef99a); segment fetch proven on hardware by `tools/radeon3dstream`
(commit draw renders identically to the inline control). Next: T2,
the write-through MiniGL frontend on `streaming-frontend`.

## Problem

Quake demo1 measures **2.861 fps** through the interface-9 hardware-TCL stack,
while the same game reaches **~12 fps** through the classic Warp3D Voodoo
driver. The GPUs are not the differentiator — the RV280 sits idle almost the
whole frame. Per-frame attribution from `phase0_timedemo.log` (338.7 s run):

| Section | ms/frame | Share | Note |
|---|---:|---:|---|
| Frontend (MiniGL record building) | 98.6 | 28% | |
| Serialize (staging slot fill) | 24.5 | 7% | |
| Execute (service validate+copy) | 99.5 | 29% | **~9.4 ms per Execute** |
| Present | 27.6 | 8% | fullscreen flip path |
| Wait (actual GPU consumption) | 1.0 | 0.3% | chip starved |

Two compounding problems:

1. **Tiny batches times heavy fixed cost.** 770362 primitives went out in
   10635 Executes (~72 primitives per submit). A texture bind is an ordering
   barrier today, and Quake binds nearly per-face, so batches stay tiny while
   every Execute pays full validation + trusted-copy + ring burst-copy.
2. **Double copy architecture.** Client fills a chip-RAM slot, the service
   AllocMems a trusted copy, walks every dword, then bursts it into the CP
   ring. Classic Warp3D drivers do none of this: they stream payload once into
   card-visible memory and enqueue pointers. We replicate that *shape*, except
   the service keeps ring/WPTR/recovery ownership because this card also drives
   the Picasso96 desktop.

## Goal and success bar

Beat the Voodoo: **Quake demo1 median > 12 fps** on `192.168.1.21:2345`
(3-run cold-boot median, phase-0 protocol). Secondary guardrails:

- Gears fullscreen 16bpp >= 5.734 fps, windowed 32bpp >= 6.376 fps
  (no small-scene regression from per-commit overheads).
- Full acceptance suite stays green at depth 8 fullscreen / 16 / 32.
- Desktop 2D stability unaffected (long gears/WB soak, grey screen => power
  cycle before drawing conclusions).

## Branches and safety

| Repo | Branch | Scope |
|---|---|---|
| p96-driver | `streaming-submit` | interface 10, segment pool, commit path |
| PiStorm_SDK_v10 | `streaming-frontend` | write-through EmitHardwareBatch |

- `main` in both repos remains the stable deployed pair; nothing merges before
  the physical acceptance suite passes.
- Interface version gates pairing: a streaming `minigl.library` against a
  non-streaming `Radeon9200.chip` fails cleanly at `Radeon3DOpen`.
- Before first streaming deploy: fresh `LIBS:minigl.library.pre-streaming` and
  `LIBS:Picasso96/Radeon9200.chip.pre-streaming` backups next to the existing
  `.pre-hw-tcl` ones.

## Design

### Service (interface 10)

- `Radeon3DAllocSegment(device, bytes, &segmentId, &mappedPointer)` /
  `Radeon3DFreeSegment`. Pool of large segments allocated from private VRAM
  (`RadeonAllocatePrivateVram`), exported as plain writable memory to the
  context-owning task. Trust boundary is unchanged: single owner task, no
  memory protection on this OS.
- `Radeon3DCommitDraw(device, commit*)` where commit carries `{segmentId,
  offsetBytes, dwordCount, primitive walk info, state serial cookie, texture
  references[]}`. The handler enqueues either
  - **Path 1:** a vertex-fetch draw packet (`3D_DRAW_VBUF_2`-class) pointing
    at the segment, or
  - **Path 2:** a fused validate-and-burst-copy of the segment into the ring
    (today's emit minus the trusted-copy allocation and separate walk).
- Reset/recovery invalidates committed-but-unexecuted segments via the
  generation counter (`deviceEpoch` concept reused client-side).
- Texture residency pins against outstanding commits; eviction barrier only
  for mutation/deletion. Bind becomes a commit attribute (T3), not a barrier.
- Ring-space exhaustion returns a distinct backpressure code so the client can
  flush commits and retry.

### Client (MiniGL)

- `EmitHardwareBatch` streams header + packed-color TCL vertices directly into
  the mapped segment. Deferred-state snapshots, serial bumps, and all existing
  ordering barriers (clear/present/readback/resize/mutation/context destroy)
  stay exactly as they are.
- Commit replaces `Radeon3DExecute` for geometry; clear/present/readback keep
  the legacy record channel initially (they are rare and stateful).

## Phases

### T0 — Probe and decision gate — **COMPLETE, Path 1 selected**

`tools/vramstream.c` measured on the physical machine (2026-08-24, cold boot,
A4000 / TF4060 68060@50 / Prometheus / RV280, writes into an offscreen
onboard scratch bitmap — never the visible framebuffer). EClock calibrated
against `Delay(50)`: 709754 ticks vs 709379 reported (0.05%).

| Case | Throughput |
|---|---:|
| fast-RAM sequential store (TF4060 SDRAM) | 27.7 MB/s |
| fast-RAM copy | 23.0 MB/s |
| VRAM sequential store, 8 K blocks | 6.16 MB/s |
| VRAM sequential store, 64 K blocks | 6.16 MB/s |
| VRAM sequential store, 256 K blocks | 6.01 MB/s |
| VRAM sequential store, 1 M blocks | 6.07 MB/s |
| VRAM byte-swapped store, 256 K / 1 M | 5.32 / 5.44 MB/s |
| fast -> VRAM copy, 256 K / 1 M | 5.40 / 5.37 MB/s |

VRAM store throughput is flat from 8 K to 1 M blocks at ~6.1 MB/s
(~648 ns/dword posted writes); byte-swapped stores cost ~752 ns/dword.
The 4 MB/s gate passes with ~1.35x margin — the Prometheus bus is a real
constraint, not a formality. (An earlier probe revision reported GB/s-scale
numbers; that was an EClock calibration bug — `ev_lo` tick count used as the
rate — and is superseded by this table.)

Cost decomposition of the current Execute path: ~76 K generated dwords/frame
at the 648 ns/dword bus floor is ~49 ms/frame of unavoidable ring traffic;
the remaining ~50 ms of the 99.5 ms Execute is validation + trusted-copy +
service indirection. The machinery costs ~2.4x the raw bus write per dword.

**Decision: Path 1** — client streams headers/vertices directly into
service-allocated VRAM segments; `COMMIT_DRAW` enqueues vertex-fetch draws.
Path 2's copy rate (5.4 MB/s) equals Path 1's store rate while adding a
second buffer and a copy pass, so Path 1 wins on simplicity and cost.

Consequences for the goal (beat 12 fps):
- Bus floor: 76 K dwords/frame at 12 fps = 3.7 MB/s = ~70% of the
  byte-swapped store floor. Feasible but tight; stream-volume reduction
  (skip fog dword when disabled, minimal texture state per batch) matters.
- Projected after T1-T4: frontend ~20 ms (T3 cuts per-batch overhead ~10x),
  bus writes ~45-50 ms, present ~18 ms, remainder Quake-side. Realistic
  landing zone ~8-10 fps; reaching 12 needs the stream-volume work and a
  re-measurement checkpoint after T3 to pick the next lever.

Remaining T1 entry task: verify the vertex-fetch packet encoding
(`3D_DRAW_VBUF_2`, opcode 0x3400-class, `PRIM_WALK_IND` walk type) against
the libdrm/Mesa r200 register definitions, then smoke-test a single VBUF
triangle on hardware before any client-side work. The driver currently
defines only prim-walk-inline ring packets (`R200_CP_VC_CNTL_PRIM_WALK_RING`
in `radeon_regs.h`); no VBUF/INDX packet exists yet.

### T1 — Interface 10 service work (`streaming-submit`)

Segment pool, ALLOC/FREE/COMMIT opcodes, generation invalidation, residency
pinning by outstanding commits, backpressure return, removal of the
per-Execute AllocMem. ABI header synced to the SDK vendored copy;
`tools/radeon3d_abi_check.c` updated; `make abi-check` green on GCC and vbcc;
`nm -u` empty.

### T2 — Write-through frontend (`streaming-frontend`) — implemented

Implemented as slot-homogeneous commit mode. A submit slot is either
*commit* (header-only draw records, vertices written straight into the
slot's segment half) or *execute* (legacy inline records); clears force an
execute slot, so a slot never mixes the two. The existing two-slot fence
ping-pong maps onto the two segment halves, so vertex-buffer reuse needs no
extra waits. `Radeon3DCommitBatch` carries the whole slot in one ring
submit; the service re-emits only the state that changed between records.

Bugs found and fixed during bring-up:

* The emitter struct is allocated without `MEMF_CLEAR`, so the new
  `CommitVbuf` flag started as garbage and plain `Execute` rejected every
  record (`length != headerDwords`). Reset it per call.
* `LOAD_VBPNTR` needs *components* and *stride* as separate fields: the
  generated prefix is 5-10 dwords while the record stride is 7 (compact) or
  10. Emitting the prefix count as the stride fetched overlapping vertices.
* Merged commit records must not grow: their vertices live in the segment,
  so record length and generated length both stay put and only the vertex
  count in the header advances.

Validation so far: `r3dstream` (raw segment commit) and `quad_list`
(66 batches merging into 6 records) pass on hardware. `cull_shade` and
`phase4_smoke` fail — but they fail **identically on a clean pre-streaming
baseline boot** (own chip + own library, caps `001ff7ff`), so they are
pre-existing defects on the branch, not streaming regressions. Gears
baseline for comparison: 6.118 fps fullscreen 640x480x16.

Remaining: gears/acceptance numbers with the streaming pair (the machine
wedged after the baseline run and needs a cold power cycle).

### T3 — Texture bind as commit attribute (both branches)

List-primitive batches merge across texture binds; mutation/deletion still
barriers. Expected effect on Quake: Execute count collapses from ~10.6 k to
hundreds per timedemo.

### T4 — Present-path slimming (optional, separable)

Attribute the 27.6 ms/frame present cost (pre-flip fence drains vs vblank
wait); remove avoidable drains. Only after T1-T3 numbers are in.

### T5 — Acceptance

Physical machine only, cold-boot discipline, one variable per run:

1. Full suite: `hw_tcl_probe`, `phase4_smoke`, `phase5_smoke` (+`--stress`),
   `phase6_primitives`, `phase6_acceptance --depth 8 --fullscreen`,
   `--depth 16`, `--depth 32`, `cull_shade_smoke`, quad_list,
   `perspective_multitex_smoke`, `phaseE`.
2. Gears benchmarks vs baselines above.
3. Quake phase-0 loop x3 cold boots, median vs 2.861 and the 12 fps bar.
4. CP context-loss recovery with in-flight commits (acceptance covers it;
   additionally force a reset mid-stream once).
5. Desktop soak: 30 min WB + windowed gears, no grey screens.

## Risks

| Risk | Mitigation |
|---|---|
| Aperture write throughput too low for Path 1 | T0 gate decides; Path 2 fallback removes most fixed cost regardless |
| Vertex-fetch packet semantics differ from expectation | Verify opcodes/format in T0 research; smoke-test VBUF draw alone in T1 before any client work |
| Reset invalidation bugs corrupt subsequent frames | Generation-checked segments; forced-reset test in T5 |
| Commit-fence aliasing (historical Phase 6 hang class) | Start with single outstanding geometry commit fence; relax only if WAIT_TICKS stays negligible |
| Small-scene regression from per-commit overhead | Guardrail gears numbers; commit batching amortizes headers |
| Mismatched library/chip deploys | Interface version gate + version print in r3dinfo |

## Measurement history (for comparison after T5)

| Configuration | Quake demo1 | Gears FS 16bpp | Gears win 32bpp |
|---|---:|---:|---:|
| CPU transform era | 2.613 fps | 4.967 fps | 5.316 fps |
| HW TCL interface 9 (2026-08-22) | 2.861 fps | 5.734 fps | 6.376 fps |
| Warp3D Voodoo reference | ~12 fps | n/a | n/a |
| Streaming target | **> 12 fps** | >= 5.734 fps | >= 6.376 fps |
