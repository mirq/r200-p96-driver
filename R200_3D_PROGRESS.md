# R200 3D Implementation Progress

This is a chronological engineering and physical-validation log. Older phase
descriptions are preserved as historical checkpoints and may be superseded by
later sections.

Current status: `Radeon9200.chip` 3.0 exposes Radeon3D interface 17 with bounded
submission, client-written VRAM streaming segments, batched semantic commits,
hardware TCL, CLUT8/RGB565/BGRA surface support, and recovery. It does not
expose general client-controlled register writes. Chronological performance
results and exact artifacts appear below.

## Initial implementation checkpoint (historical)

- `Radeon9200.chip` version 2 with append-only LVOs for an opaque version-1
  discovery session: open, close and query info.
- Fixed-layout `Radeon3DInfo`, GCC/vbcc inline calls, FD/SFD descriptions and
  dual-compiler layout/call fixtures.
- Explicit `EMPTY`, `INITIALIZING`, `ATTACHED`, `READY` and `DETACHING`
  board-service states.
- Generation invalidation on board detach and CP abort.
- Active-session registration so random, closed and concurrently closing
  pointers are not dereferenced before membership validation.
- A private Prometheus owner-detach LVO used on initialization aborts.
- Transactional Prometheus PCI ownership: a Radeon is claimed before hardware
  initialization, busy boards are skipped, and every initialization abort
  releases the claim after hardware shutdown.
- Boot-order-safe shared DMA adoption: if RTL8139 or another PCI client creates
  the fixed 2 MiB early arena first, a smaller positive monitor `DMASIZE`
  request adopts and excludes the full existing arena instead of aborting
  Radeon initialization on an exact-size mismatch.
- DEBUG-only 8 KiB/64 KiB aperture measurements using a temporary owned VRAM
  reservation and the CP-style final readback drain.
- DEBUG-only 4096-dword buffered/direct CP no-op measurements. Both paths use
  identical padding, wrap, readback, WPTR and scratch-completion rules.
- DEBUG-only CP checks for forced ring wrap, a near-full reservation, bounded
  impossible-reservation timeout and ordered back-to-back fences.
- DEBUG callback-entry sampling of `BoardInfo.BoardLock` ownership.
- Decoder support for debug stats version 14.
- A vbcc-built `radeon3dinfo` runtime probe.
- A direct, bounded 4096-dword CP streaming path. Every service submission has
  an internal service-owned fence, while the public fence result remains
  optional.
- Public submit, test-fence and wait-fence vectors at `-66`, `-72` and `-78`.
- Active-call tracking that retains the session owner pin across a concurrent
  close without adding `OpenLibrary` overhead to each batch.
- A vbcc-built `radeon3dphase1` probe for malformed input, bounds, fences and
  physical ring wrap.
- Tracked P96 bitmap import/release at LVOs `-84` and `-90`. Import accepts
  on-board, 64-byte-pitch R5G6B5PC or B8G8R8A8 allocations, validates their
  complete extent against P96-visible VRAM and returns CPU/GPU addresses from
  the owning `BoardInfo`.
- Trusted submission staging: validation and ring emission consume the same
  copied batch instead of rereading mutable caller memory.
- A documented bounded immediate triangle-list stream, bound to a live,
  16-byte-aligned imported RGB565 surface. Target dimensions and pitch are
  derived from the import; 3 to 255 vertices carry finite in-bounds XY and
  packed RGBA. All other `PACKET0` and `PACKET3` streams fail closed.
- 2D/3D arbitration drains pending MMIO before service submission and waits for
  pending CP work, restores the direct-MMIO baseline and invalidates cached 2D
  state before the next accelerated Picasso96 operation.

Service clients discover the already-loaded library by its Exec node name,
`Radeon9200.chip`; only `Prometheus.card` uses the `Picasso96/` disk path when
loading the chip during Picasso96 initialization.

vbcc tools use `-O=1`. The installed vbcc 0.9h m68k backend was verified to
miscompile explicit `__reg()` library calls at `-O=2`, omitting argument-register
loads before the inline `jsr`; `-O=1` emits the required `a6`, address and data
register setup.

`Radeon3DInfo.MaxBatchDwords` is 8192 when the service opens successfully.
`RADEON3D_CAP_IMMD_TRI_LIST` identifies the constrained draw contract described
in `RADEON3D_SUBMISSION.md`. It is not a general register-write capability.
Clients must keep their `OpenLibrary` reference until every service session has
been closed.

The lock rule established at this checkpoint is `BoardLock` before
`ServiceLock` when both are needed. Runtime recovery invalidates the service
while holding `BoardLock`. Service metadata paths release `ServiceLock` before
an operation that may acquire `BoardLock`; the subsequently implemented submit
path acquires `BoardLock` first, validates the session under `ServiceLock`, then
releases `ServiceLock` before touching hardware. DEBUG telemetry records
whether Picasso96 already owns `BoardLock` on callback entry.

## Build

```sh
make clean
make abi-check
make r3d-tools
make
make DEBUG=1
```

Release objects use `build/`; DEBUG objects use `build-debug/`. DEBUG output is
named `Radeon9200-debug.chip` and `Prometheus-debug.card`, so it cannot silently
replace the release artifacts. The DEBUG card opens
`Picasso96/Radeon9200-debug.chip` explicitly.

## First hardware validation (historical interface 1)

1. Install the matching version-2 chip and card together. For measurement runs,
   install `Radeon9200-debug.chip` beside the release chip and install
   `Prometheus-debug.card` as the active `Prometheus.card`.
2. Enable `CP=YES` and cold boot the physical RV280 machine.
3. Run `radeon3dinfo`. Expected result begins with `R3DINFO status=ok`, reports
   interface version 1, includes `RADEON3D_CAP_CP_READY`, and reports the
   submission capability appropriate to the current build.
4. Read at least 670 bytes per Exec port-list node and decode the
   `Radeon9200.Debug` block with `tools/decode_debug_stats.py`.
5. Record `VramSmallBytes/Ticks`, `VramBurstBytes/Ticks`,
   `CpBufferedTicks/Success`, `CpDirectTicks/Success`, CP functional checks and
   `BoardLock` ownership counts with CPU, bridge, board ID and cold-boot state.
6. Confirm Workbench, cursor, screen changes and existing 2D tests remain intact.

### Hardware Result - 68060 / RV280 5964

The first version-14 run completed on the physical 68060 system with an RTL8139
installed. Workbench remained active at 1024x768, AmigaBridge stayed reachable,
and no crash was recorded.

| Check | Result |
|---|---:|
| Service discovery | v2 library, v1 interface, generation 2 |
| Device/caps | `5964`, CP ready + single board (`0x00000003`) |
| Installed/P96 VRAM | 64 MiB / 64,995,328 bytes |
| EClock | 709,379 Hz |
| MMIO read/write | 1.45 us / 1.33 us per access |
| 8 KiB aperture write | 881 ticks, 6.29 MiB/s |
| 64 KiB aperture write | 7,387 ticks, 6.00 MiB/s |
| 4096-dword buffered CP | 2,738 ticks, 3.860 ms |
| 4096-dword direct CP | 2,249 ticks, 3.170 ms |
| Direct-path saving | 17.9%, 1.217x faster |
| Forced wrap | success, WPTR 258,112 -> 16 |
| Near-full reserve / bounded timeout | success / success |
| Ordered fences | success, sequence 1 -> 2 |
| `BoardLock` callback samples | 171,535 current-task owned, 0 other/unowned |

MuScan 46.1 reports the 68060 MMU mapping for the complete Prometheus aperture
`0x40000000-0x5fffffff` as `CacheInhibit I/O space`. The measured aperture rate
is above the Phase 0 5 MiB/s reassessment threshold, but only narrowly. The
direct prototype avoids an intermediate staging copy and should be retained as
an internal two-span service implementation; the result does not justify a raw
direct-ring public ABI.

Three consecutive `radeon3dinfo` runs succeeded and left the chip library open
count at its owner-only value of one.

## Owner Lifetime

The deployed `rtg.library` 43.787 was inspected directly. Its `LibClose`
decrements the open count, but `LibExpunge` is a no-op, matching the observed
zero-open-count resident library and proving that its `BoardInfo` objects are
system-lifetime on the target configuration.

The service also protects revisions that can expunge: each active
`Radeon3DDevice` holds one `rtg.library` open reference and reports
`RADEON3D_CAP_OWNER_PINNED`. If delayed expunge is pending when the final owner
pin closes, the service detaches Radeon while `BoardInfo` is still valid, then
releases the pin. Clients still keep their chip-library reference until after
`Radeon3DClose` returns.

This removes the owner-lifetime blocker for Phase 1 submission work.

The physical owner-pin hold test observed `rtg.library` at open count one and
the chip at two while a service session was active. Both returned to their
zero/owner-only baselines after close. Three further probes succeeded without a
crash.

## Phase 1 Submission Slice

The submission slice accepts one to 8192 exact `PACKET2` dwords plus the
documented untextured immediate triangle-list stream. It appends a service-owned
cache flush, 2D/3D idle-clean wait and scratch fence to every accepted batch.
Host dwords are copied once, validated from that trusted copy, then byte-swapped
directly into up to two physical ring spans. The existing aperture readback and
WPTR MMIO drain remain in place. Unknown flags, packet words, zero-length and
oversized batches fail before the ring is touched.

On the physical 68060/RV280 target, repeated `radeon3dphase1` runs passed. Each
run submitted 70 fenced 4096-dword batches, or 286,720 client dwords, which
crosses the 262,144-dword ring boundary. Fences advanced through 422 at unchanged
generation 2. The deliberately malformed packet, oversized count, unknown
flags and future-fence query were rejected.

The same probe allocated a 320x240 bitmap using the public screen as its friend,
imported CPU address `0x40185300` as GPU address `0x00185300` with 640-byte pitch
and R5G6B5PC format, explicitly released it, re-imported it for automatic session
cleanup, and rejected a normal planar bitmap. Afterwards `rtg.library` returned
to open count zero, the chip returned to owner-only count one, and no crash was
recorded.

At this checkpoint the last physically installed build reported caps
`0x0000003f`. A then-uninstalled host-built dynamic-contract release reported
`0x0000007f`; both reported `max_batch_dwords=4096`. Later sections supersede
this installation and batch-limit status.

### Hardware Triangle Result - 68060 / RV280 5964

The first exact-state attempt rendered successfully on the physical card. The
probe cleared an imported 320x240 R5G6B5PC bitmap, submitted one immediate
triangle with XY plus packed RGBA vertices, waited for its internal fence and
read back 45,056 changed bytes. The center pixel was `0xffff`.

The transition build then issued a Picasso96 `BltBitMap` immediately after the
3D fence. The copied destination pixel was also `0xffff`, confirming the CP wait,
2D baseline restore and subsequent direct-MMIO operation on hardware. Two full
runs passed consecutively; fences advanced from 73 to 145 at generation 2. Each
run also repeated the 286,720-dword wrap test. `rtg.library` returned to open
count zero, the chip returned to owner-only count one, and no crash was recorded.

The installed transition build is 51,196 bytes with CRC32 `F49B9F4B`. Host
SHA-256 is `8a829b2617457bb32dfed2e423dec0ea89220b92e6692a2853198c1d31adcb3e`.
The matching probe SHA-256 is
`38340a2c5e28a75e10f2e17c15d5f6bde97f98756892d6d62abe20d32675fa5c`.

## Dynamic Triangle Contract And r200test Presentation

The host-built service now accepts an exact fixed-state prefix followed by 3 to
255 triangle-list vertices. The validator resolves the live imported target,
requires RGB565 and a 16-byte-aligned color offset, derives scissor and pitch
from that target, rejects NaN/infinity/negative/out-of-bounds XY, and verifies
the packet and control counts. `RADEON3D_SUBMISSION.md` is the public contract.

The expanded `radeon3dphase1` probe builds two dynamic triangles and adds
negative cases for packet/control counts, partial triangles, trailing dwords,
NaN, infinity, negative and out-of-bounds coordinates, and released targets.
GCC/vbcc ABI checks and release/debug driver builds pass.

`/home/mirek/r200_minigl/r200test` now builds a vbcc 68060 AmigaOS executable.
Windowed mode opens a public-screen window and presents a dynamic-size imported
RGB565 target through fenced `ClipBlit`/`WaitBlit`. Fullscreen mode validates a
P96 RGB565 mode, opens it through Intuition, imports two screen buffers, and
uses exact safe/display replies for `ChangeScreenBuffer` ownership. Both modes
render an animated Gouraud triangle, use responsive bounded fence waits, handle
`q`, Ctrl-C and EClock timeout exits, and print live timing plus a stable
`R200TEST_SUMMARY` line.

### Physical Dynamic And Windowed Result - 68060 / RV280 5964

The release was installed and cold-booted through the new physical bridge at
`192.168.1.21:2345`. Service discovery reported generation 2, caps
`0x0000007f`, 64 MiB installed VRAM and a 4096-dword maximum batch.

The public Workbench screen was 8-bit, exposing an accidental allocation
assumption in the original probe. `AllocBitMap` and `p96AllocBitMap` with that
screen as friend both produced on-board CLUT8; no-friend RGB565 allocated in
Fast RAM. The final path briefly opens a private 16-bit P96 screen selected for
R5G6B5PC, allocates the target from its bitmap friend, closes that screen, then
imports the retained on-board target. The successful target was CPU
`0x404241c0`, GPU `0x004241c0`, pitch 640, RGB565 and 16-byte aligned.

The expanded phase probe passed all malformed-input cases, two dynamic
triangles, framebuffer readback, immediate 3D-to-2D transition, 70 fenced
4096-dword submissions and physical ring wrap:

```text
R3DPHASE1 status=ok submits=70 dwords=286720 last_fence=71 generation=2
caps=0000007f triangle_bytes=53888 triangle_sample=e781 transition=e971
```

The windowed test displayed a moving RGB Gouraud triangle correctly. A
five-second timeout run completed 36 measured frames at 7.191 FPS with zero
errors/timeouts/resets. Physical `q` exit then completed 278 measured frames in
38.294 seconds at 7.259 FPS:

```text
R200TEST_SUMMARY version=1 mode=window size=320x240 requested_s=0.000
exit=q frames=278 elapsed_s=38.294 fps_avg=7.259 frame_ms_avg=137.750
submit_ms_avg=0.453 wait_ms_avg=0.107 present_ms_avg=81.520
generation=2 errors=0 timeouts=0 resets=0
```

The test now holds its public-screen lock for the complete window lifetime and
accepts both translated and raw `Q`. After exit, `radeon3dinfo` still succeeded,
the chip and `rtg.library` returned to their observed owner/system baselines of
one open each, repeated one-second cleanup left those counts unchanged, and no
crash was recorded.

### Physical Fullscreen Result - 68060 / RV280 5964

Fullscreen mode required a standard `OpenScreenTagList` screen after P96 mode
selection and validation; a screen opened directly by `p96OpenScreenTagList`
did not provide an Intuition primary `ScreenBuffer` on this installation. The
working path allocates the primary plus copied secondary buffer, imports both,
and uses separate safe/display ports. Both messages belong to the DBufInfo of
the newly requested buffer. Cleanup consumes outstanding replies, restores the
primary buffer, and waits for its exact display and safe replies before freeing
anything.

The physical five-second 640x480 timeout run completed 22 frames at 4.310 FPS:

```text
R200TEST_SUMMARY version=1 mode=fullscreen size=640x480 requested_s=5.000
exit=timeout frames=22 elapsed_s=5.103 fps_avg=4.310 frame_ms_avg=231.981
submit_ms_avg=0.378 wait_ms_avg=0.084 present_ms_avg=20.734 generation=2
errors=0 timeouts=0 resets=0
```

An indefinite run exited through injected raw-key `q` after 145 frames. Its
summary reported 35.029 seconds, 4.139 FPS, generation 2 and zero
errors/timeouts/resets. Both runs restored the desktop screen list and recorded
no crash. The final installed test binary is `Work:r200test`, size 19208, CRC32
`6C70076A`, and SHA-256
`1b70c91fc617fe7674623068f912f32468906673fb90917d624431f81dcee7b0`.

That exact final binary passed the required presentation intervals. Fullscreen
completed 262 frames over 60.179 seconds at 4.353 FPS:

```text
R200TEST_SUMMARY version=1 mode=fullscreen size=640x480 requested_s=60.000
exit=timeout frames=262 elapsed_s=60.179 fps_avg=4.353
frame_ms_avg=229.691 submit_ms_avg=0.387 wait_ms_avg=0.089
present_ms_avg=19.183 generation=2 errors=0 timeouts=0 resets=0
```

Windowed mode then exited through raw-key `q` after 525 frames over 72.374
seconds at 7.253 FPS:

```text
R200TEST_SUMMARY version=1 mode=window size=320x240 requested_s=0.000
exit=q frames=525 elapsed_s=72.374 fps_avg=7.253 frame_ms_avg=137.855
submit_ms_avg=0.427 wait_ms_avg=0.133 present_ms_avg=81.437
generation=2 errors=0 timeouts=0 resets=0
```

The fullscreen screen and windowed visitor both closed cleanly, the normal
screen list remained, and no crash was recorded.

## Phase 2 Semantic Service And Final Hardware Gate

`Radeon9200.chip` version 3 appends `Radeon3DExecute` at LVO `-96`. Interface-v2
sessions receive `RADEON3D_CAP_PHASE2_EXECUTE`; interface-v1 sessions retain
the original packet contract and cannot execute semantic records. Clear and
triangle records carry opaque imported-surface handles, fixed options and
bounded vertices. The service copies records once, validates complete VRAM
ranges, formats, dimensions, pitches, finite coordinates, aliases, scissors,
options and exact lengths, then generates complete trusted R200 state. It does
not expose new register or packet submission.

The final scene exercises GPU color/depth clear, animated Gouraud triangles,
D16 `LESS` depth writes, RGB565/B8G8R8A8 texturing, nearest/bilinear filtering,
source-alpha blending and hardware scissor. A CPU bitmap-font overlay is drawn
only after the fence while the target is safe. Startup passes 25 short-record,
malformed-state, alias, NaN, interface-v1 and cross-session rejection checks.
First-frame readback passed these exact PC-order samples:

```text
R200TEST_VERIFY clear=c410 depth=e0ff nearest=00f8 bilinear=0f80
alpha=e0fb scissor=1ff8 guard=c410
```

GPU clear removed the mapped-PCI clear bottleneck. Fullscreen improved from
4.353 FPS for the presentation-only CPU-clear milestone to 34.322 FPS for the
complete Phase 2 scene:

```text
R200TEST_SUMMARY version=2 mode=fullscreen size=640x480 requested_s=60.000
exit=timeout frames=2060 elapsed_s=60.018 fps_avg=34.322
frame_ms_avg=29.135 submit_ms_avg=1.369 wait_ms_avg=1.994
present_ms_avg=25.484 generation=2 errors=0 timeouts=0 resets=0
scene=phase2 verify=pass validation=pass
```

The final windowed acceptance completed 830 frames over 75.073 seconds:

```text
R200TEST_SUMMARY version=2 mode=window size=320x240 requested_s=75.000
exit=timeout frames=830 elapsed_s=75.073 fps_avg=11.055
frame_ms_avg=90.450 submit_ms_avg=1.876 wait_ms_avg=0.798
present_ms_avg=87.328 generation=2 errors=0 timeouts=0 resets=0
scene=phase2 verify=pass validation=pass
```

Delayed `sgrab` captures verified the complete scene in windowed and fullscreen
modes. Fullscreen restored the primary screen and normal screen list; no crash
was recorded. The installed files are:

```text
LIBS:Picasso96/Radeon9200.chip size=44360 CRC32=05C98E75
SHA-256=7f8cba12ffc125bd993775dac4089fcc9423be26aee6bf9b5f681219244d4c77
Work:r200test size=24408 CRC32=4EFFEDCB
SHA-256=fc9567a2e0e6a6785fa439fd0ea3a64ba67c812501d44fe6c090e86921d7e916
```

Explicit forced stale-generation rejection was still a Phase 1 follow-up at
this checkpoint; later session and recovery acceptance tests cover stale
handles and generation changes.

## Phase 3 Independent MiniGL_R200 Bootstrap

The independent `/home/mirek/r200_minigl/MiniGL_R200` tree now builds a
resident `minigl.library` 7.0 without compiling classic MiniGL rendering code.
Its generated table preserves all 186 stack-argument vectors in canonical
order with contiguous LVOs from `-30` through `-1140`. The checker compares
vector identity, order and every offset against the immutable classic ABI.

The bootstrap implements one-task context ownership, window, best-mode
fullscreen and explicit `mglCreateContextFromID` targets, RGB565 bitmap import,
color clear, immediate Gouraud `GL_TRIANGLES`, bounded fence waits, windowed
`ClipBlit` and fullscreen double buffering. Fullscreen teardown waits for exact
safe/display messages and restores the primary `ScreenBuffer` before release.
Unsupported vectors remain callable and report `GL_INVALID_OPERATION` with an
active context.

Physical acceptance passed 20 window frames, 100 best-mode fullscreen frames
and 100 explicit-mode-ID fullscreen frames at 640x480. Each run reported
`abi_tail=pass`, exercising the final public vector at `-1140`. While a
fullscreen owner was active, a second task was rejected with `context creation
failed`; after owner completion the library open count was zero. Delayed
`sgrab` verified the fullscreen clear and RGB Gouraud triangle, and no crash was
recorded.

```text
LIBS:minigl.library size=9192 CRC32=0C9AE9A5
SHA-256=b9efc5a6f4eb474659aa732f6b278c49b96411a7355c4e81d8314c9eb3d6e50c
Work:minigl_r200_smoke size=15432 CRC32=4D58ED80
SHA-256=0ee6e6ba8cd05cd8e7769ee8a6ff73ac08e9096b26b923e3b4119858e3ad724d
```

## Phase 4 Transforms, Geometry And Depth

Radeon3D interface 3 adds `RADEON3D_CAP_PHASE4_DEPTH_FUNCS` and a trusted
three-bit depth comparison in semantic draw options. Zero still means `LESS`,
so interface-2 records are unchanged; interface-2 sessions reject nonzero
comparison bits. The service maps all eight comparisons to bounded R200
`RB3D_ZSTENCILCNTL` values without exposing register writes.

The independent MiniGL frontend now implements model-view/projection stacks,
load/multiply/translate/rotate/scale, frustum/ortho and GLU camera transforms;
homogeneous clipping and viewport/depth mapping; triangle, fan, strip, quad,
quad-strip and polygon conversion; generic vertex/color arrays and indexed
draws; and an imported D16 target with clear, mask, enable and compare state.

The physical Phase 4 acceptance rendered transformed untextured gears and
clipping/array/indexed/depth fixtures for 12 window frames and 80 fullscreen
frames. Both modes passed exact first-frame samples:

```text
MINIGL_PHASE4_VERIFY depth=00f8 transformed=1f00 clipped=1ff8
MINIGL_PHASE4 mode=window transforms=pass clipping=pass arrays=pass indexed=pass depth=pass frames=12
MINIGL_PHASE4 mode=fullscreen transforms=pass clipping=pass arrays=pass indexed=pass depth=pass frames=80
```

All eight depth compare records and matrix stack underflow/overflow were
exercised. Delayed `sgrab` captures showed the expected clipped magenta region,
three transformed gears, array triangles and indexed quad. Fullscreen restored
the normal screen set, `minigl.library` returned to open count zero and no crash
was recorded. Installed artifacts are:

```text
LIBS:Picasso96/Radeon9200.chip size=44480 CRC32=E6AEDE27
SHA-256=17a99efe65c4bc43d9394a1d7a4305906f98e679a3a7de9fe70062d1b8143e4e
LIBS:minigl.library size=21068 CRC32=44FD9BF0
SHA-256=20a8581380a0007133e4b92df43363e87ef7978690bb44701747fa003303abcc
Work:minigl_phase4 size=26208 CRC32=B79489A0
SHA-256=12aba47708d41a8b91058e40a767db573c82a55f914b8c52840b4c738b85024c
```

## Phase 5 Single-Unit Textures And Blending

Radeon3D interface 4 advertises `RADEON3D_CAP_PHASE5_TEXTURE_STATE` and adds a
capability-gated 15-dword semantic draw header. It carries validated texture
backing offset/dimensions, replace/modulate, wrap, independent filters, POT mip
count, alpha test and blend factors. The service calculates and validates every
mip extent, emits deterministic `PP_MISC`, and includes diffuse color in v4
textured immediate packets. Original v2/v3 headers and behavior remain intact.

The independent MiniGL frontend implements per-object BGRA CPU mirrors,
supported source conversion, image/subimage updates, unpack row/skip/alignment,
complete rectangular mip metadata, immediate and array texture coordinates,
all six min filters, nearest/linear magnification, repeat/clamp,
replace/modulate, alpha test, blending, P96 residency, eviction/re-upload and
fence-safe redefinition/deletion.

Physical window and fullscreen acceptance passed with exact results:

```text
MINIGL_PHASE5_VERIFY texture=00f8 subimage=e007 alpha=1f00 blend=0f80
MINIGL_PHASE5 mode=window image=pass subimage=pass mip=pass wrap=pass env=pass blend=pass alpha=pass stress=pass frames=12
MINIGL_PHASE5 mode=fullscreen image=pass subimage=pass mip=pass wrap=pass env=pass blend=pass alpha=pass stress=pass frames=80
MINIGL_PHASE5 residency=pass objects=48 size=512x512
```

The residency run exceeded P96 VRAM, handled Fast-RAM fallback import rejection,
evicted resident objects and re-uploaded the earliest object from its CPU
mirror. Delayed captures showed repeated nearest checks, filtered mip sampling,
modulation and texcoord-array geometry. Phase 4 regression passed, fullscreen
restored cleanly, library open count returned to zero and no crash was recorded.

```text
LIBS:Picasso96/Radeon9200.chip size=46316 CRC32=0F903D74
SHA-256=4a070a19cf7e64ecfc2ae678bc2188c666418d2535ab5d5a560df329a9bccc9e
LIBS:minigl.library size=27776 CRC32=0D1E9256
SHA-256=de66d8cfc8d48fcdebfdace6afe9a03e4de726a99518419dcc93d551e0ca0a6f
Work:minigl_phase5 size=21372 CRC32=D55A3B58
SHA-256=778c9e7a0d20963c180d1b2c5c85503c4aca59eb60467858026a194378c09939
```

## Phase 6 Completion And Optimization

Radeon3D interface 5 advertises fog/multitexture and a diagnostic generation
invalidation capability. V5 records use a 21-dword header and nine-dword public
vertices while requiring the v4 state bit, so all older layouts remain exact.
The service validates both texture trees, unit-1 routing/combiners, fog amount,
surface roles and the immediate hardware word count before emitting state.

The independent frontend now supports two texture units, linear/exp/exp2 fog,
scissor, points, clipped lines/strips/loops, compiled array locking, FastPath v2,
transactional window resize and service-generation recovery. Recovery discards
the lost frame, reopens interface 5, reimports color/depth targets and lazily
re-uploads texture CPU mirrors.

The post-Phase-6 review added generated-stream budgeting in MiniGL, preventing
record batches from expanding beyond the service's then-4096-dword command
buffer.
Driver recovery now reloads and self-tests the CP before moving Radeon3D from
ATTACHED back to READY. The diagnostic invalidation hook uses that production
path rather than generation-only invalidation. GCC/vbcc builds and ABI checks
passed; the physical acceptance results below subsequently verified context
loss recovery.

Physical acceptance results:

```text
R3DINFO status=ok library_version=3 interface_version=5 generation=2 device=5964 caps=00000fff installed_vram=67108864 p96_vram=64995328 max_batch_dwords=4096
MINIGL_PHASE6 mode=window fog=linear,exp,exp2 multitex=pass env1=modulate,replace scissor=pass points=pass lines=pass frames=20
MINIGL_PHASE6 mode=fullscreen fog=linear,exp,exp2 multitex=pass env1=modulate,replace scissor=pass points=pass lines=pass frames=80
MINIGL_PHASE6_ACCEPT arrays=pass fastpath=v2 window=move,overlap resize=pass context_loss=recovered fullscreen=not-run
MINIGL_PHASE6_ACCEPT fullscreen_resize=rejected
MINIGL_PHASE5 residency=pass objects=48 size=512x512
```

Phase 4 and Phase 5 exact-pixel window/fullscreen regressions also passed.
Expunge/reload succeeded, Workbench was restored, no crash was recorded, and
the installed library open count returned to zero.

```text
LIBS:Picasso96/Radeon9200.chip size=48684 CRC32=3C3DA7D7
SHA-256=27534e4063e28da2389f2ba16148750569a6561364e71672fcd7837ec0c2665f
LIBS:minigl.library size=39616 CRC32=5904B1D4
SHA-256=05f8ec29f87188363f94592acaa08518ceb1562b85e930fcc958ab307c7c63cb
Work:phase6_primitives size=16024 CRC32=5DDCEA3E
SHA-256=23798364612f9c82f139ff25058ecaf43e0ebcc8d81e10028c39bcdb399500dd
Work:phase6_acceptance size=18924 CRC32=8235AD1C
SHA-256=f05d39f3bf5fb51502d994fb19e4a6ea4144dbb7a481345294d7e27ae0bd4558
```

## Interface 6 Color Targets

Interface 6 advertises `RADEON3D_CAP_COLOR_TARGET_FORMATS`. The semantic clear
and draw paths now derive RB3D format and pixel pitch from imported CLUT8,
R5G6B5PC or B8G8R8A8 targets. CLUT8 is interpreted as direct RGB332 and uses
R200 output dithering; interface-5 sessions remain restricted to RGB565 color
targets. The raw interface-v1 immediate packet validator is unchanged and
remains RGB565-only.

The independent MiniGL frontend accepts `mglChoosePixelDepth(8|16|32)`, retains
D16 depth independently of color depth, reports the selected format through
`mglLockBack()`, and reallocates window targets without falling back through
RGB565. Eight-bit rendering is fullscreen-only and installs a private 3-3-2
palette; arbitrary public-screen CLUT palettes cannot represent RGB332 without
per-frame CPU remapping.

Physical RV280 `5964` verification on 16 August 2026 passed semantic triangles,
exact RGB565/BGRX32 readback, bounded dithered RGB332 readback, and the legacy
interface-5 rejection check:

```text
R3DFORMAT depth=8 format=3 pitch=320 sample=0000004f
R3DFORMAT depth=16 format=1 pitch=640 sample=00003933
R3DFORMAT depth=32 format=2 pitch=1280 sample=cc663300
R3DFORMATS status=ok result=0 legacy_v5=rejected
MINIGL_PHASE6_ACCEPT depth=8 fullscreen_resize=rejected
MINIGL_PHASE6_ACCEPT depth=16 fullscreen_resize=rejected
MINIGL_PHASE6_ACCEPT depth=32 fullscreen_resize=rejected
MINIGL_PHASE6_ACCEPT depth=32 arrays=pass fastpath=v2 window=move,overlap resize=pass batch_budget=pass context_loss=recovered fullscreen=not-run
```

The B8G8R8A8 display format's fourth byte is unused by Picasso96 and read back
as zero, so the validated 32-bit target is BGRX32 rather than an alpha-bearing
offscreen framebuffer. The installed interface-v6 artifacts were:

```text
LIBS:Picasso96/Radeon9200.chip size=49584 CRC32=C4966B3E
LIBS:minigl.library size=39400 CRC32=C4FAD297
```

## Interface 7 Native Triangle Primitives

Interface 7 advertises `RADEON3D_CAP_NATIVE_TRI_PRIMITIVES` and accepts native
triangle-strip and triangle-fan execute records. They reuse the validated draw
state and vertex layouts, while the driver emits R200 primitive types 6 and 5
instead of expanding every primitive into a triangle list. Interface 6 and
older sessions reject the new opcodes.

Physical RV280 validation on 16 August 2026 passed the Phase 4, Phase 5, and
Phase 6 acceptance suites. The 20-frame 640x480 textured gears workload fell
from 15 seconds to 13 seconds with MiniGL native strip/fan submission and
identical-state triangle-list coalescing. Installed artifacts are:

```text
LIBS:Picasso96/Radeon9200.chip size=49216 CRC32=047A2DB8
LIBS:minigl.library size=44680 CRC32=A85E1481
```

## Interface 7 Execute Optimization

Physical 68060/RV280 profiling on 17 August 2026 showed the semantic execute
path walking every public vertex once for validation and again for command
generation. Validation and emission now share one traversal. Invalid records
still reject the complete private generated stream before any CP submission.

The generated-stream emitter also retains the exact previous draw state within
one `Radeon3DExecute()` call. Consecutive records omit state only when every
surface pointer, option, scissor, texture, fragment, depth, fog and phase-6 field
matches. The cache resets for every call, so it makes no assumption across
submissions, clients, recovery or intervening 2D work.

`MaxBatchDwords` is now 8192. Existing clients compiled for 4096 continue to
clamp locally; the updated MiniGL frontend reduces the gears workload from seven
submits to three per frame. A 16384-dword test reduced submits to two but changed
the three-run mean only from 4.930 to 4.932 FPS while increasing execute ticks,
so 8192 was retained for better board-lock fairness.

The stable fullscreen gears command measured 4.919, 4.925 and 4.946 FPS, a
4.930 FPS mean. The previous MiniGL-only checkpoint was 4.684 FPS. The rebuilt
phase-1 probe passed malformed-input checks, fences, physical ring wrap and 70
fenced 8192-dword batches (573,440 client dwords). Phase 4, Phase 5, Phase 6
primitives and Phase 6 acceptance all passed on the final binaries:

```text
SYS:Libs/Picasso96/Radeon9200.chip size=52836 CRC32=1E15D4DC
LIBS:minigl.library size=45892 CRC32=A8C19BCD
R3DINFO max_batch_dwords=8192
MINIGL_PHASE6 fog=linear,exp,exp2 multitex=pass scissor=pass points=pass lines=pass
MINIGL_PHASE6_ACCEPT arrays=pass fastpath=v2 window=move,overlap resize=pass batch_budget=pass
```

## Interface 8 Native Quad Lists

Interface 8 advertises `RADEON3D_CAP_NATIVE_QUAD_LISTS` and accepts
`RADEON3D_EXEC_DRAW_QUADS` records containing 4 through 252 vertices in groups
of four. The service emits the R200 native quad-list primitive (`0x0d`) and
rejects the opcode for interface-7 sessions. MiniGL uses the Mesa/R200 order
`0,1,3,2` to preserve the two triangles of each four-vertex triangle strip and
coalesces up to 63 independent quads per semantic record.

Physical 68060/RV280 validation on 21 August 2026 followed a cold reboot with
the release driver. The interface probe reported version 8 and caps
`000077ff`. `radeon3dformats` passed 8/16/32-bit targets, two native quads with
independent color readback, and interface-5/interface-7 rejection. MiniGL's
65-quad boundary/order smoke test passed with four draw records. Phase 4,
Phase 5, Phase 6 primitives, cull/shade, windowed acceptance, and fullscreen
Phase 4/Phase 6/acceptance all passed.

Two consecutive fullscreen 640x480x16 gears profiles measured 4.971 and 4.963
FPS. Relative to the immediately preceding instrumented 4.692-FPS build, quad
coalescing changed the per-frame workload from 3320 to 2760 submitted vertices,
27 to 25 draw records, 20336 to 16946 semantic record dwords, and 21834 to
18338 generated CP dwords. Installed artifacts were:

```text
LIBS:Picasso96/Radeon9200.chip size=55016 CRC32=B0FE2D03
LIBS:minigl.library size=75468 CRC32=BD7E94CA
R3DQUADS count=2 first=0000ff00 second=00ff0000
R200_QUAD_LIST status=ok caps=000077ff attempts=65 used=65 draw_records=4
```

## Interface 12 Hardware Sphere-Map Texgen

Interface 12 advertises `RADEON3D_CAP_HW_SPHERE_MAP` and accepts
`RADEON3D_TEXGEN_MODE_SPHERE_MAP` in the existing texgen record shape. The
service selects the R200 sphere texgen input, requires per-vertex normals and
both generated S and T, and enables local-viewer evaluation with hardware
normal normalization. Interface-11 sessions reject the mode.

The first sphere probe failed at its four corner samples while its centre
passed. That was a probe defect, not a driver defect: it asserted that a normal
parallel to the eye vector yields (0.5,0.5), whereas the reflection
`f = u - 2n(n.u)` gives `-u` for a unit normal, matching Mesa's
`src/mesa/tnl/t_vb_texgen.c`. Its four corners generated s/t of (0.752,0.668),
(0.378,0.743), (0.378,0.257) and (0.752,0.332), all outside the accepted band,
while the interpolated centre averaged to (0.565,0.500) inside it. The probe now
carries, per vertex, the object-space normal that the submitted inverse
model-view maps onto that vertex's own reflection bisector for `r = (0,0,1)`, so
every vertex generates exactly (0.5,0.5) and the whole quad samples one texel.
Because each corner requires a different normal, a wrong normal-matrix
orientation or a non-sphere texgen input still moves the corners off it.

Physical 68060/RV280 validation followed a cold reboot with the uncommitted tree
below (`ac3d819` plus modified `Agents.md`, `RADEON3D_SUBMISSION.md`,
`README.md`, `include/radeon3d.h`, `src/radeon3d_service.c`, `src/radeon_regs.h`,
`tools/radeon3d_abi_check.c`, `tools/radeon3dformats.c`,
`tools/radeon3dphase1.c`). Board `1002:5964` generation 2, 64 MiB installed VRAM,
64995328-byte Picasso96 aperture, 8192-dword maximum batch, validated ToolTypes,
320x240 B8G8R8A8 target with a 64x64 B8G8R8A8 texture. `radeon3dformats` passed
in full, including all four corner samples and the centre:

```text
LIBS:Picasso96/Radeon9200.chip size=64096 CRC32=3A2A9D53
LIBS:Picasso96/radeon3dformats size=16120 CRC32=4909E011
R3DINFO interface_version=12 caps=000ff7ff
R3DSPHEREMAP samples=5ac3e7ff,5ac3e7ff,5ac3e7ff,5ac3e7ff,5ac3e7ff expected=5ac3e7ff
R3DFORMATS status=ok result=0 sphere=ok legacy_v11_sphere=rejected
```

## Phase 2 Validated Streams (2026-08-30): implemented, measured, rejected

Interface 17 (validated client CP streams, PPC-side emission) was implemented
and physically validated for correctness on the 68060/RV280 target — native
validator suite, cross-CPU emitter proofs, 1350 submitted streams with zero
rejects, and every native MiniGL suite passing. The performance gate failed:
800x600x32 windowed envmap gears reached 46.8 FPS median against the accepted
interface-16 state-batch median of 52.456. Per-stage counters attributed the
cost to the per-submit `RadeonCpWait` drain (4.2 ms/frame), the 2.5 us/dword
validator walk (2.7 ms/frame), and the trusted copy (1.9 ms/frame).

A CP-to-CP prepare skip in `RadeonPrepare3D` (skip drain+restore when the
engine is READY and pending work is already CP) recovered +3.9% on the stream
configuration and is semantically correct: the ring orders consecutive 3D
submissions, `CpReserve()` provides back-pressure, and the next Picasso96
operation still drains through `Need2DRestore`. All native suites pass with
the skip. On the accepted interface-16 baseline it measured neutral
(51.540 vs 51.690 FPS single cold-boot runs) because the per-frame present
blit already forces the drain. The skip is retained on the mainline; the full
Phase 2 implementation is preserved on the `phase2-cp-stream` branch
(commit `32afdf1`, includes the V5 stage counters and measurement records).

## Fixed-function lighting output fix (2026-09-04)

MiniGL submitted a valid 133-dword interface-11 lighting record with options
`0x7700`, normal matrices, one enabled light, material state and a 31-dword
light block, but lit pixels remained identical to unlit pixels. The emitter
enabled and populated the TCL lighting engine but wrote
`R200_SE_TCL_OUTPUT_VTX_COMP_SEL` without `R200_OUTPUT_COLOR_0`; the computed
primary colour was therefore not exported. `EmitExecuteState()` now adds
`R200_OUTPUT_COLOR_0` when lighting is active, matching Mesa classic R200.

Physical 68060/RV280 validation used `Radeon9200.chip` CRC `07A7193D` with the
matched `Prometheus.card` CRC `18A453D6`. The MiniGL directional probe changed
from an unchanged white pixel to a distinct lit value, phase7 passed all
lighting checks, V19 parity passed 31/31, the core phase4/5/6 regressions passed,
and the 32bpp MiniGL testsuite completed 24/24 PASS.

## Private 68k Working Memory (2026-09-07)

The WarpOS/Sonnet task allocator was placing Radeon3D's private generated
commands, emitter/captures and state-batch staging in PPC RAM. The 68k then
paid PCI-memory access cost while constructing each batch. The allocator hook
redirects PUBLIC/FAST requests even when LOCAL is set, so the corrected
preference requests only MEMF_LOCAL and checks TypeOfMem for FAST afterward.
Rejected Chip RAM is freed; ordinary public allocation remains the fallback.
No GPU surface/vertex allocation, cache ordering, packet validation or ABI changes.

Three bridge-cold-rebooted 600-frame PPC DLL precalc gears runs per build,
640x480x32 fullscreen BLIT, no sync, two buffers, quiet priority -1 host:

- Original `07A7193D`: 44.510, 44.313, 44.313 FPS; median 44.313.
- Corrected `23ACBAEC`: 54.446, 54.844, 54.644 FPS; median 54.644 (+23.3%).
- All runs: zero errors, 2,031 executions / 600 presents, clean exits.

The matched `18A453D6` card, native library `2F05868E`, PPC DLL `31878AB6`,
host `98DE2066`, compiler flags and mode data were unchanged. Live inspection
confirmed all three working buffers moved to local RAM. Native phase5 stress,
phase6 primitives/acceptance and the PPC 7,127-check texture-update test passed.
The fix is active; both `.previous` files retain the original matched pair.
These are bridge reboots, not physical power cycles; the recalled approximately
100 FPS is not recovered. See [EXECUTE_MEMORY.md](EXECUTE_MEMORY.md) for the
failed first preference, allocation tests, exact flags, raw logs and metadata.

## Texture-Matrix And CP Copy Fixes (2026-09-07)

Texture-only state changes could return before updating independently cached
MVP/texgen/normal matrices. A two-record hardware probe reproduced stale
transforms on `23ACBAEC`; the corrected common matrix path passes all four
controls/regressions and 1,188 CPU checks without redundant lighting uploads.
Texture-only correction `6B860EE0` was validated before changing CP code.

The CP copy then fused source load, byte swapping and the existing MOVEM ring
store, eliminating its eight-word stack stage. Fences, readbacks, wrap handling
and publication remain unchanged. Actual 68k RAM tests passed 2,816 copy and
1,966 commit cases, including four wraps and sequence rollover; host sanitizer
coverage also passed the exhaustive 5,934-commit schedule.

Three independently bridge-rebooted 600-frame runs per build (640x480x32 BLIT,
two buffers, no sync, priority -1), with the texture correction in both builds:

- Old CP: 53.956, 54.446, 54.844 FPS; median 54.446.
- Fused CP: 57.034, 57.471, 57.251 FPS; median 57.251 (+5.2%).
- CP submit elapsed: 2.582 -> 1.152 ms/frame, unchanged dword counts.

Native phase5 stress, phase6 primitives/acceptance, phase7 lighting, paired RGB
tests and the 7,127-check PPC texture-update test all pass. Active matched pair
is `9768419A` / `18A453D6`; both `.previous` files hold `6B860EE0` / `18A453D6`.
Compiler flags, native library, PPC DLL/host and mode data remain unchanged.
See [CP_TEXTURE_FIXES.md](CP_TEXTURE_FIXES.md) for exact scope, bytecode proof,
reboot method, samples, tests and remaining limits.

## Indirect Render Prerequisites (2026-09-10, Host Only)

The trusted interface-18 indirect path now advertises optional
`RADEON3D_CAP_INDIRECT_RENDER` (bit 27) alongside bit 26, only when the session
version and streaming-segment pool make indirect dispatch eligible. Interface
18 and the request/vector layouts are unchanged. Rendering consumers must
require both bits; bit 26 alone is the older fetch/fence smoke path.

`Radeon3DDispatchIndirect()` checks basic pointers and minimum descriptor size,
then copies the fixed 24-byte V1 descriptor to local storage. All subsequent
validation, segment selection, IB commands and count telemetry use only that
snapshot. It validates the request and live lease before `RadeonPrepare3D()`.
Since prepare can recover successfully while invalidating
the session, dispatch then rechecks active device usability, the unchanged
service generation/board, CP readiness and the unchanged lease under
`ServiceLock`, with `BoardLock` held throughout. Stale state never reaches IB
setup or submission. Existing CSQ partition/endian setup stays after prepare.
Only a successful nonzero fence updates `LastFence` and marks 3D submitted;
false submission or a missing fence recovers once without retry. Existing
experimental diagnostics remain, with new rejection stages 107 (prepare) and
108 (post-prepare stale state).

Header and submission-contract text now state the actual trust boundary:
extent checks bound the initial IB fetch only. Trusted packets can access
registers and VRAM elsewhere, corrupt the desktop or hang the GPU; there is
no packet sandbox. No emitter changes are part of these prerequisites.

A descriptor-snapshot safety follow-up reproduced the former post-validation
reread: changing the caller's offset/count/segment during Prepare changed the
submitted IB address. The fixed path retains the validated address and count,
even for an exact-end lease range, and retains snapshot count telemetry on both
successful and failed submits. A header-only allocation is rejected before
copying beyond its Size field. No unrelated overhead was optimized; benchmark
work remains the priority and nothing from this follow-up will be deployed now.

The final even-length guard follows the verified Linux v2.6.39
`radeon_cp_dispatch_indirect()` source in `radeon_state.c` (linked in the
submission contract): IB data must contain an even number of dwords, and odd
streams are padded with a Type-2 CP packet before `IB_BASE/BUFSZ`. The service
now rejects odd snapshot counts at stage 101 before BoardLock or Prepare,
without changing the experimental CSQ/endian setup. Padding belongs to the
producer: append one CP-native, once-swapped PACKET2 for an odd stream, include
it in the lease extent/count and cache flush, and submit the even count.
This is source-based hardware padding, not an arbitrary no-op workaround.
Interface 18 and the fixed 24-byte request layout remain unchanged.

Validation on the host, from commit
`98192a217cf4fb7366c6937cb4817247f7cd9706` plus the preserved pre-existing
dirty tree and these uncommitted prerequisites:

- `python3 tools/test_indirect_dispatch.py`: PASS, 79 cases. Extracts the
  production dispatch, usability and capability functions, structures and
  constants; runs with 32-bit ULONG and ASan/UBSan. Covers hook ordering,
  valid ID 0, malformed requests, bounds/overflow, prepare failure, successful
  prepare with generation/session/board/CP/lease invalidation, failed submits,
  missing fences, recovery without retry, `LastFence` before Mark, short
  descriptors, and Prepare-time caller mutation with snapshot commands/counts.
  Odd counts 1/3/8191 reject before hooks; even counts 2/8192 accept. Existing
  upper-bound and lease-overrun cases use even counts to retain their coverage.
- `python3 tools/test_execute_memory.py`: PASS, existing allocation regression.
- `git diff --check`: PASS.
- Isolated release matched-pair build: PASS using
  `/opt/amiga/bin/m68k-amigaos-gcc` 6.5.0b and unchanged Makefile flags. Chip
  compilation is warning-free with `-Werror`. The existing card recipe does
  not enable `-Werror` and emits const/pointer-sign/unused-variable warnings
  in untouched Prometheus sources; those defects remain outside this scope.

Build command:

```sh
make BUILD_DIR=/tmp/opencode/ibdraw/driver TARGET=/tmp/opencode/ibdraw/Radeon9200.chip CARD_TARGET=/tmp/opencode/ibdraw/Prometheus.card
```

The first link failed because the existing Makefile prefixes absolute targets
with the build directory. Creating the expected intermediate directories
`driver/tmp/opencode/ibdraw` and
`driver/prometheus-card/tmp/opencode/ibdraw` under `/tmp/opencode/ibdraw`, then
rerunning the same command, completed both artifacts without Makefile edits.
The descriptor-snapshot and even-length follow-ups reused those directories
and rebuilt the chip without warnings; the matched card was already up to date.
Final artifacts in `/tmp/opencode/ibdraw`:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `Radeon9200.chip` | 77940 | `7f253b21a6af38403c17330a4d8c9505de252481f1fb439dd1d3c02a41ba9636` |
| `Prometheus.card` | 7796 | `b90c0a54b919b2ab8af2e198e55ace3ddad62ff6f53b69818107262f988d902b` |

At the end of that prerequisite-only step, the new render path was **NOT hardware validated**. The then-installed
older chip CRC `5341F8BF` performed fetch/fence smoke tests only, not the new
PPC fixed clear/triangle render test. No hardware was contacted, no artifacts
were deployed, and no commit was made for this work.

## Physical CP-emitter validation and repository consolidation (2026-09-12)

The later consumer work cold-loaded chip `CEB838A6` (77940 bytes) with the
unchanged known-good card `18A453D6`. Triangle readback passed; CP texture-update
acceptance passed 7,127 checks over two contexts and 18 updates; the final
windowed 800x600x32 gears run completed 600 frames, 2,031 IB submissions and
511,620 CP dwords at 68.337 FPS, with no GL or dispatch errors. Native bootstrap
passed after the runs. Source and raw evidence are in the cooperating MiniGL
repository's `backend_r200/HOST_LOADER_REGRESSION.md` and `host_loader_evidence/`.

The 68k host's separate Sonnet loader misclassification and PPC wait-policy
regressions were fixed in the consumer, not by further driver changes. Its
quiet fullscreen 640x480x16, three-buffer precalc result recovered to 134.529
FPS; that fullscreen run uses semantic submission, not fullscreen CP emission.

Earlier cache-partition/endian/priming explanations remain historical hypotheses:
the successful functional runs do not isolate those settings as performance
causes. Debug-only bring-up probes are not a release workload or an instruction
to recover hardware automatically after a hard hang. Consolidation corrected
debug-stat version/count reporting and missing GPU-address log arguments; no
release rendering behavior was changed by that diagnostic bookkeeping cleanup.

## Debug-chip boot hang and probe gating (2026-09-17)

A freshly built DEBUG chip (version 23 StateBatchFail diagnostics, size 97548,
CRC `B2D34778`) was installed as the active pair on the physical machine with
the matching debug card (`1DC1ABC0`, 7804 bytes). The machine did not return
from that boot: the bridge never came up and an operator cold-power cycle was
required. Recovery used the documented `S:startup-sequence` right-mouse-button
block; both `.previous` files pre-existed and were the working release pair.
The installed release pair was re-verified unchanged afterwards (chip
`F46026B4`, card `18A453D6`).

Post-mortem code review found two defects in the DEBUG-only boot path:

1. The boot-time indirect-buffer probe's case 4 "restore" wrote the stale
   `0x4d4d` CSQ cache partition. Production moved to `CP_CSQ_CACHE_PARTITION`
   (`0x5010`) on 2026-09-08 because 64-dword indirect buffers time out under
   `0x4d4d`; the probe therefore left the CP in the starved partition for the
   whole boot. Fixed to restore `CP_CSQ_CACHE_PARTITION`.
2. `RadeonDebugOpen` unconditionally ran MMIO/VRAM sampling, CP no-op batches,
   the CP function matrix, the indirect-buffer matrix, and the 8192-call
   fallback probe during `LoadMonDrvs`. Any of these can wedge the engine at
   a point from which the machine cannot recover without a cold cycle, and no
   emulator or bridge control can observe a pre-bridge hang. These probes are
   where an earlier debug build differed from every booting release build.

Both are now opt-in: DEBUG builds gate the boot-time engine experiments behind
`RADEON_BOOT_PROBES`, selected with `make DEBUG=1 PROBES=1` (default 0). The
passive debug port, run counters, and per-call accounting always stay active,
and the version-23 StateBatchFail capture is unaffected. Artifacts built and
verified locally, hardware NOT touched:

| Artifact | Bytes | CRC32 |
| --- | ---: | --- |
| `Radeon9200.chip` (release) | 77928 | `F46026B4` (byte-identical to installed) |
| DEBUG chip, probes off | 93092 | `FE2B54BA` |
| DEBUG chip, probes on | 97548 | `DA8CAF5F` |

The exact cause of the 2026-09-17 boot hang remains unidentified; the probe
matrix and the partition regression are the most plausible candidates. Any
future debug-chip install must use the probes-off build first, with the
operator's explicit go-ahead for the cold boot.

## Boot-safe debug build and automatic driver recovery (2026-09-18)

The second hung boot isolated the cause further: with the boot-time engine
probes compiled out, the DEBUG chip still differed from release by linking
`-ldebug` (kdebug.o `KPrintF`). Analysis of libdebug's kdebug.o shows every
put-char path jumps through a cached library base and exec RawDoFmt/raw
console output. RLOG (KPrintF) is therefore now a no-op in all chip builds;
the chip is silent in DEBUG exactly as in release. Side effects fixed: the
`-Werror` unused-variable fallout in `radeon_bios.c` (`i2c`) and the removal
of the only new binary difference between the debug and release init paths
aside from counter/accounting code.

Also gated behind `RADEON_BOOT_PROBES`: the DEBUG-only `DumpStreamFile`
(first Execute/CommitBatch stream dumps) which ran dos.library calls through
the chip's uninitialized `_DOSBase` from inside render callbacks.

New local artifacts (hardware not touched at this point):

| Artifact | Bytes | CRC32 |
| --- | ---: | --- |
| Release chip | 77928 | `F46026B4` (still byte-identical) |
| DEBUG chip, probes off (silent, boot-safe class) | 86504 | `1FE4C2D3` |
| DEBUG chip, probes on (needs `-lc`; `T:` dumps) | 90052 | `A7844F5F` |

The card build embeds `__DATE__ ", " __TIME__` (`vbcc_libinit.c:74`), so its
CRC changes per rebuild; a deployed card is verified by the push CRC check,
not by a stable reference value.

`S:startup-sequence` gained an automatic recovery layer: marker
`S:driver-boot-failed` written just before `LoadMonDrvs`, deleted only after
a full boot; the next boot restores both `.previous` files if the marker
survived and writes `RAM:driver-autorecovered` as proof. The right-mouse
TestRMB block stays as the manual override. Deployed `S:startup-sequence`
CRC `FD2550C7` (3397 bytes); backup of the previous sequence in
`S:startup-sequence.backup-rmb`. The recovery block shape was verified
on the live machine by pushing the identical statements to `S:recov-test`
and executing the file (nested `IF EXISTS` + copies + evidence file);
inline multi-line script execution through the bridge was found
unreliable (long/nested blocks) and is not a substitute for executing
real files.

## RTG-fallback-aware failsafe (2026-09-18)

The first failsafe version cleared `S:driver-boot-failed` at the end of any
boot that reached `EndCLI`. Hardware showed that a failed RTG driver does not
stall the machine: it falls back to the native PAL display and the sequence
completes, so the marker was cleared and the next boot retried the failed
pair. Added `C:RTGPresent` (tools/rtgpresent.c, 3236 bytes, CRC `10F8C296`):
it locks the default public screen, treats mode IDs with the RTG
(`0x80000000`) flag or a >900-wide screen as RTG, and returns 5 (WARN)
otherwise. The startup sequence now clears the marker only when RTGPresent
succeeds, so a PAL-fallback boot leaves the marker and the next boot restores
both `.previous` files. Deployed sequence CRC `2FC1B8FC` (3767 bytes);
RTGPresent live-tested on the RTG Workbench (reports OK).

Note: the deployment directory on the Amiga is `C:RTGPresent` (AmigaDOS
`C:`), not the local `build/` path.
