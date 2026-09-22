# 01 - PPC / WarpOS client guide

This is the entry point for anyone writing a PPC consumer of the Radeon3D
service: a WarpOS application, a middleware layer, an engine port, or the
MiniGL R200 backend. It explains how a PPC program reaches a driver that runs
on the 68k side, what the trust boundary is, how memory and caches must be
handled across the two CPUs, and how to use each submission path.

Everything here is the *client* view. Exact structure layouts, LVOs, record
opcodes and validation rules are in
[`02-service-abi-reference.md`](02-service-abi-reference.md); measured costs
are in [`04-performance.md`](04-performance.md).

## 1. The two-CPU model

`Radeon9200.chip` is an AmigaOS shared library whose code runs on the **host
68k CPU**. It owns PCI access, Radeon MMIO, the command processor (CP) ring,
VRAM allocation and Picasso96 display. It exposes the Radeon3D service through
ordinary library vectors.

The **PPC** (WarpOS) side never touches PCI, MMIO, the CP ring or VRAM
allocation. A PPC client:

1. obtains the library base (68k library, loaded by name `Radeon9200.chip`),
2. opens a Radeon3D *session* with `Radeon3DOpen()`,
3. imports or allocates GPU surfaces through the service,
4. optionally leases *streaming segments* (service-owned VRAM that the client
   may write directly),
5. submits work through one of the bounded entry points, and
6. retires work through *fences*.

The service validates every bounded call, translates semantic state into R200
command streams, submits them to the ring, and owns recovery.

### 1.1 How PPC code actually calls the 68k library

There are three practical topologies; the service contract is identical in all
of them, but the call cost is not:

| Topology | How it works | Relative call cost |
|---|---|---|
| 68k host process | A native 68k task opens the library and serves requests from PPC over Exec message ports. This is the shape the MiniGL WarpOS frontend uses (`MGLPPCTransport`, `MGLPPC_HOST_PORT_NAME`). | One Exec round trip per request plus driver time |
| WarpOS cross-CPU call | PPC code calls the 68k library vector through the WarpOS/`amiga.lib` PPC-to-68k bridge. | Cross-CPU context switch per call, no extra process |
| Native 68k | 68k code calls the vectors directly (probes, replay tool, 68k clients). | Cheapest; used for reference measurements |

Two consequences dominate client design:

- **Per-call overhead is large relative to small commands.** Batch many draws
  into one call. The service's own batch limit is
  `RADEON3D_MAX_BATCH_DWORDS` = 8192 dwords per call.
- **Any wait is a cross-CPU stall.** Prefer fence checks and deferred waits to
  blocking `Radeon3DWaitFence()` in the middle of frame construction; use the
  fence only when the buffer will be reused or presented.

The 68k host process topology additionally has an *asynchronous* form: send a
message, keep building the next frame, then wait for the reply. The synchronous
form (send + wait immediately) is simpler and is what the measured reference
numbers use.

### 1.2 What a call costs (reference 68060 @ 50 MHz)

Measured on the reference machine; cycles are derived at 20 ns/cycle. Full
methodology and artifact identities are in
[`04-performance.md`](04-performance.md#2-cycle-cost-table).

| Operation | Measured | Cycles |
|---|---:|---:|
| Radeon MMIO register read / write | 1.45 us / 1.33 us | ~72 / ~66 |
| Framebuffer aperture dword write | 0.61 us | ~30 |
| `HOST_DATA0` register dword write | 1.33 us | ~66 |
| Byte-swapped VRAM dword store | 0.752 us | ~38 |
| CPU-only emitter loop, 20-draw stream, local Fast RAM | 36 us/draw | ~1,800/draw |
| Same loop with the buffers in PPC RAM | 125 us/draw | ~6,240/draw (3.46x) |
| Fenced submission (full GPU idle drain + cache flush) | milliseconds | - |

Implications: a call's fixed cost is dominated by the fence drain, not by
dword count, so one frame should use a few large submissions; crossing the PCI
bridge costs ~30 cycles per dword, so avoid redundant host->VRAM copies; and a
68k-side working buffer accidentally placed in PPC RAM multiplies its access
cost by roughly 3.5-4.8x.

### 1.3 Trust boundary

| Entry point | What the service validates | Trust required |
|---|---|---|
| `Radeon3DExecute` | Every semantic record, surface, format, extent, float, alias, option bit, exact length | Untrusted |
| `Radeon3DCommitDraw` / `Radeon3DCommitBatch` / `Radeon3DCommitStateBatch` | Record structure, offsets, segment leases, state | Untrusted records, trusted segment bytes (they become GPU vertices) |
| `Radeon3DSubmit` | Only the fixed immediate triangle-list stream (interface 1 contract) | Bounded, effectively trusted |
| `Radeon3DDispatchIndirect` | Only the *extent* of a segment range; **packet contents are not inspected** | Fully trusted producer; see the warning below |

`Radeon3DDispatchIndirect()` is the only raw-packet path. It does not sandbox
packets: a malformed stream can corrupt the desktop, wedge the CP, or hang the
GPU (recovery may need a power cycle). Use it only when you own the packet
generator and have a hardware-validated stream.

## 2. Session lifecycle

```c
#include <proto/exec.h>
#include <proto/radeon3d.h>     /* pulls in clib/inline prototypes */

struct Radeon9200Base;          /* extern struct Library *Radeon9200Base */

struct Library *base = OpenLibrary("Radeon9200.chip",
                                   RADEON3D_LIBRARY_VERSION);
if (!base)
    return FAIL;

struct Radeon3DInfo info;
info.Size = sizeof(info);                     /* always set Size first */
struct Radeon3DDevice *dev =
    Radeon3DOpen(RADEON3D_IFACE_VERSION, &info);   /* newest you understand */
if (!dev) {
    CloseLibrary(base);
    return FAIL;
}
/* info.Version is the negotiated interface (<= requested), info.Caps the
 * capability mask, info.Generation the service generation. */
```

Rules:

- Keep the `OpenLibrary` reference open until **after** `Radeon3DClose()`
  returns. The session pins `rtg.library` for you; it does not pin the chip
  library.
- `Radeon3DOpen()` fails cleanly when the board is not attached, the CP is not
  ready, or the service is invalidated. It never half-opens.
- The returned device handle is opaque. It is validated against a live-session
  list on every call, so a stale handle cannot dereference freed memory.
- `Radeon3DClose()` waits (up to 1 s) for the session's last fence, then
  releases segments, surfaces and the owner pin. It is safe to call after a
  failed submission; it triggers bounded recovery if the fence never retires.
- `info.Generation` changes whenever the service is invalidated (board detach,
  CP abort/recovery). Compare it before reusing cached surface handles; a
  generation change means your handles and segments are gone and the session
  must be reopened.

### 2.1 Capability negotiation

`Radeon3DOpen(requestedVersion, &info)` negotiates
`min(requestedVersion, RADEON3D_IFACE_VERSION)`. Capabilities are set from that
negotiated number, plus runtime state (CP ready, pools reserved, debug build).

A robust consumer:

1. asks for the newest interface it was compiled against,
2. reads `info.Version` (the granted interface) and `info.Caps`,
3. requires every capability it cannot fall back from, and refuses to start
   with a clear message otherwise.

Do not assume a capability because the interface number is high enough:
`RADEON3D_CAP_STREAM_SEGMENTS` requires a live segment pool,
`RADEON3D_CAP_AUX_SURFACES` requires a reserved aux pool, and
`RADEON3D_CAP_INDIRECT_DISPATCH`/`INDIRECT_RENDER` require both interface 18+
and the segment pool. A debug-only bit (`RADEON3D_CAP_TEST_INVALIDATE`) must
never be required by production code.

The full capability table is in
[`02-service-abi-reference.md`](02-service-abi-reference.md#4-capability-bits).

## 3. Surfaces

The service works on *surfaces*: a CPU address, a GPU address, a pitch, a
width/height and a format. Two ways to obtain one:

### 3.1 Import a Picasso96 `BitMap` - `Radeon3DImportBitMap()`

- The bitmap must be a P96 on-board allocation in the card's visible VRAM.
- Accepted formats: `RGBFB_CLUT` (1 byte/pixel), `RGBFB_R5G6B5PC` (2),
  `RGBFB_B8G8R8A8` (4).
- The pitch must satisfy the R200 constraints the service validates; a planar
  or Fast-RAM bitmap is rejected.
- The caller keeps ownership of the bitmap and must not free it before
  `Radeon3DReleaseSurface()`.
- Typical use: rendering directly into a screen buffer or window bitmap that
  P96/Intuition also displays.

### 3.2 Allocate from the driver pool - `Radeon3DAllocSurface()`

- Interface 17+, requires `RADEON3D_CAP_AUX_SURFACES`.
- The memory is carved from private VRAM the driver reserved *before*
  Picasso96 saw the card, so it can never collide with a P96 bitmap or an
  `AllocScreenBuffer()` buffer. This is the safe way to get a depth buffer:
  the two allocators do not know about each other, and depth landing inside a
  screen buffer scribbles bands across the frame.
- Pitch is padded to 128 bytes and `Width` reports the *padded* pixel count,
  so `Width` may exceed the requested width.
- Maximum dimension 4096; pool is 4 MiB with at most 8 live surfaces.
- Release with `Radeon3DReleaseSurface()`; surfaces still held when the
  session closes are released with it.

### 3.3 Role rules

Color, depth and texture roles are checked by complete VRAM address range, not
by handle identity:

- depth must be a distinct, non-overlapping surface with the same dimensions
  as color;
- depth writes require `RADEON3D_DRAW_DEPTH_LESS` and a representable D16
  pitch;
- textures are non-overlapping RGB565 or B8G8R8A8, at most 2048x2048, with a
  32-byte-aligned address and representable NPOT pitch;
- the same surface may be sampled by both texture units;
- distinct textures must not partially overlap.

## 4. Streaming segments

A *segment* (interface 13+, `RADEON3D_CAP_STREAM_SEGMENTS`) is a service-owned
VRAM lease that the client writes directly:

```c
struct Radeon3DSegment seg;
seg.Size = sizeof(seg);
if (!Radeon3DAllocSegment(dev, 256UL * 1024UL, &seg))
    ...;
/* seg.CpuAddress : writable mapping the client may store through
 * seg.GpuAddress : GPU base used in commit records
 * seg.Id         : lease id, valid until FreeSegment or device close */
```

Up to 12 segments of at most 256 KiB each (`RADEON3D_MAX_SEGMENTS`,
`RADEON3D_MAX_SEGMENT_BYTES`). The pool is 3 MiB reserved from private VRAM.

Segment rules for PPC producers:

1. **Alignment.** Vertex offsets used in commits must be 4-byte aligned.
   `ByteOffset` in indirect dispatch must be 16-byte aligned.
2. **Endianness.** Segment data is *GPU data*, not host-endian records. The
   hardware fetches little-endian dwords. A PPC (big-endian) producer must
   byte-swap every dword before storing it. The 68k inline path hides this
   inside the ring writer; streaming does not. For a TCL vertex dword that
   holds IEEE-754 floats, the correct operation is a full 32-bit byte swap of
   the float bit pattern (equivalent to `SWAPLONG`).
3. **Cache.** Flush the PPC data cache over the written range before the
   submission call that consumes it. The GPU reads VRAM through PCI, not the
   PPC cache. On WarpOS this is `CacheClearE(address, length,
   CACRF_ClearD)` or equivalent; the exact API depends on your runtime.
   Missing this produces stale vertices that "work" until the cache line is
   evicted, which is the worst kind of intermittent bug.
4. **Lifetime.** Do not free or rewrite a segment range until the fence of the
   submission that references it has retired. Segments are freed
   automatically when the device closes.
5. **Ordering with CPU texture writes.** CPU-written texture data must be
   completed before the draw that samples it; the service drains the final
   texture byte before submitting texture fetches, but the producer is
   responsible for its own cache flush.

## 5. The submission paths

Use the cheapest path that can express your work:

| Path | Interface | Input | Best for |
|---|---|---|---|
| `Radeon3DExecute` | 2+ | Host-endian semantic records | Clears, state changes, moderate geometry, easy correctness |
| `Radeon3DCommitDraw` | 13+ | One header-only draw + segment vertex offset | One large draw |
| `Radeon3DCommitBatch` | 13+ | Chain of header-only draws + offsets | Many draws of one geometry stream (the normal streaming path) |
| `Radeon3DCommitStateBatch` | 15+ | One TCL header + N `{offset,count}` descriptors | Many draws sharing one state block |
| `Radeon3DSubmit` | 1 | Fixed immediate triangle list / PACKET2 | Legacy smoke tests only |
| `Radeon3DDispatchIndirect` | 18+ | Pre-swapped CP packets in a segment | Trusted producer that emits its own R200 packets |
| `Radeon3DSubmitFence` | 19+ | nothing | Closing a run of fence-less indirect dispatches (parked; see below) |

### 5.1 Semantic records - `Radeon3DExecute()`

The record stream is host-endian and self-contained: each record starts with
`{opcode, length}` and carries opaque surface handles. The emitter validates
and consumes it in one pass; the calling task is blocked for the duration, so
the service reads your buffer directly. Keep the record buffer alive for the
call only.

Typical frame:

```c
ULONG records[...];
ULONG *p = records;
/* clear color+depth */
*p++ = RADEON3D_EXEC_CLEAR; *p++ = RADEON3D_EXEC_CLEAR_DWORDS;
*p++ = (ULONG)colorSurface.Handle;
*p++ = (ULONG)depthSurface.Handle;
*p++ = RADEON3D_CLEAR_COLOR | RADEON3D_CLEAR_DEPTH;
*p++ = 0xff203040UL;                 /* packed RGBA */
*p++ = 0x3f800000UL;                 /* depth 1.0f */
*p++ = 0; *p++ = 0; *p++ = width; *p++ = height;
/* draw ... */
ULONG fence;
if (!Radeon3DExecute(dev, records, (ULONG)(p - records),
                     RADEON3D_SUBMIT_FENCE, &fence))
    /* failure: check info.CommitFailStage and the fence error encoding */;
```

The record layouts for every opcode are specified in
[`02-service-abi-reference.md`](02-service-abi-reference.md#10-semantic-record-formats).

### 5.2 Streaming commits - `Radeon3DCommitBatch()`

The client writes vertices into a segment and submits a chain of *header-only*
draw records. Each record's header is the ordinary HW-TCL draw header
(`RADEON3D_EXEC_DRAW_HW_TCL_HEADER_DWORDS` = 44 dwords), and its `dword[10]`
holds the real vertex count; the vertices are fetched from the segment by the
hardware, not carried in the record.

```c
ULONG offsets[N];                 /* per-record byte offset into the segment */
struct Radeon3DCommitBatch batch;
batch.Size = sizeof(batch);
batch.Version = RADEON3D_COMMIT_BATCH_VERSION;
batch.SegmentId = seg.Id;
batch.Records = headers;          /* linked by dword[1] length fields */
batch.RecordDwords = headerDwords;
batch.VertexOffsets = offsets;
batch.RecordCount = n;
batch.Flags = RADEON3D_SUBMIT_FENCE;
ULONG fence;
BOOL ok = Radeon3DCommitBatch(dev, &batch, &fence);
```

The service walks the chain once to verify structure and count, then re-emits
state only where it changed between records (`COMMIT_STATE_REUSE`, interface
14+). The header you submit is *not* copied for commits - it is read directly,
like `Execute` records.

### 5.3 State batches - `Radeon3DCommitStateBatch()`

When many draws share one state block (same texture, matrices, scissor,
options) and differ only in vertex offset and count, this is the least
expensive semantic path: the service copies the header and descriptor array
once, emits the full state from the first descriptor, and then only emits
vertex-fetch draws for the rest.

```c
struct Radeon3DStateBatchDraw draws[192];
struct Radeon3DStateBatch batch;
batch.Size = sizeof(batch);
batch.Version = RADEON3D_STATE_BATCH_VERSION;
batch.Generation = info.Generation;      /* must match the live service */
batch.SegmentId = seg.Id;
batch.Primitive = RADEON3D_EXEC_DRAW_QUADS;   /* or TRIANGLES/STRIP/FAN/... */
batch.Header = tclHeader;                     /* 44+ dwords, complete state */
batch.HeaderDwords = headerDwords;
batch.Draws = draws;
batch.DrawCount = n;                          /* <= 192 */
batch.Flags = RADEON3D_SUBMIT_FENCE;
```

The `Generation` field is the only place a client is asked to echo service
generation explicitly; it catches a stale session before any state is emitted.

### 5.4 Trusted indirect dispatch - `Radeon3DDispatchIndirect()`

The client builds a complete **CP-native** packet stream (pre-byte-swapped for
the little-endian register bus) into a segment and dispatches a range:

```c
struct Radeon3DIndirect ib;
ib.Size = sizeof(ib);
ib.Version = RADEON3D_INDIRECT_VERSION;
ib.SegmentId = seg.Id;
ib.ByteOffset = 0;                 /* 16-byte aligned */
ib.DwordCount = dwords;            /* nonzero, even, <= 8192 */
ib.Flags = 0;                      /* or RADEON3D_INDIRECT_NO_FENCE (parked) */
ULONG fence;
BOOL ok = Radeon3DDispatchIndirect(dev, &ib, &fence);
```

The service writes `PACKET0(CP_IB_BASE, 1)` with the segment GPU address and
dword count, then the standard retiring fence. It validates only the extent:
segment identity, 16-byte offset alignment, even nonzero count within the
lease. Contents are your responsibility.

Odd-length streams must be padded by the producer with one CP-native PACKET2
and included in the count (the service rejects odd counts rather than padding
in place, following Linux `radeon_cp_dispatch_indirect()`).

For rendering, require **both** `RADEON3D_CAP_INDIRECT_DISPATCH` and
`RADEON3D_CAP_INDIRECT_RENDER`. The latter adds the prepare/revalidation,
submitted-state and recovery hooks that make rendering safe; bit 26 alone is
the older fetch/fence smoke path.

> **Parked:** interface-19 fence coalescing
> (`RADEON3D_INDIRECT_NO_FENCE` + `Radeon3DSubmitFence`) is present in the
> ABI but default-off and **not deployable**: three hardware attempts at
> back-to-back indirect fetches produced a hard wedge, corrupted rendering and
> a silent submission stall. See
> [`07-history.md`](07-history.md#31-interface-19-fence-coalescing-parked).

### 5.5 Legacy - `Radeon3DSubmit()`

Interface-1 clients may submit either an all-PACKET2 no-op batch or the fixed
22-register immediate triangle-list stream. It exists for bring-up probes and
must not be used for new work. Require `RADEON3D_CAP_IMMD_TRI_LIST`.

## 6. Fences

A fence is a nonzero `ULONG` token returned by a successful submission when
`RADEON3D_SUBMIT_FENCE` is set (or, for `Radeon3DSubmit`, always internally).

Semantics:

- Fences are **monotonic per session**. `Radeon3DTestFence()` and
  `Radeon3DWaitFence()` accept any fence the session has submitted in the live
  range `(0, LastFence]` when `RADEON3D_CAP_MULTI_FENCE` is present.
- A fence retiring means every earlier submission in ring order has completed,
  including fence-less ones.
- `Radeon3DTestFence()` is non-blocking and also invalidates the host read
  buffer, so it is the cheap way to poll.
- `Radeon3DWaitFence(dev, fence, timeoutMs)`: nonzero timeout is a wall-clock
  budget clamped to 60 s; zero performs one non-blocking test. Wait in
  milliseconds, not spins.
- A failed submission returns FALSE and writes `0x80000000 | stage` (or
  `0x80000000 | (group << 16) | stage`) to `fenceOut`. **Always check the
  BOOL result before using a fence token.**
- The service performs bounded recovery after a failed submit; existing
  sessions are stale afterwards and must be reopened.

Recommended client pattern (double-buffered geometry):

```text
for each frame:
    if (segment slot in use && !TestFence(dev, slot.fence))
        skip/steal slot (never overwrite in-flight vertices)
    build records / vertices into the free slot
    submit with FENCE, store the returned token in the slot
```

Do not hold a wait in the middle of the frame unless the buffer is genuinely
needed; use the test-first pattern above to keep the GPU fed.

## 7. Memory, endianness and cache - PPC checklist

| Concern | Rule |
|---|---|
| Record/commit structures | Host-endian (big-endian on both CPUs). No conversion. |
| CP-native indirect buffers | Little-endian dwords; producer byte-swaps once. |
| Segment vertex data | Little-endian GPU data; PPC producer byte-swaps floats/dwords. |
| PPC cache | Flush written segment ranges before dispatch/commit. |
| 68k working buffers | The driver prefers local Fast RAM for its own working buffers (`MEMF_LOCAL` + `TypeOfMem` FAST check). Keep client-side staging out of PPC RAM if a 68k host will read it. |
| Alignment | Commits: 4-byte vertex offsets. Indirect: 16-byte offsets. Textures: 32-byte aligned. |
| Size | One call <= 8192 dwords total input. `MaxBatchDwords` is authoritative. |
| Lifetime | Bitmaps until `ReleaseSurface`; segments until fence retirement; session until `Radeon3DClose`. |

PPC-RAM placement is a measured, large penalty: moving three private 68k
working buffers out of PPC RAM raised a 600-frame gears median from 44.3 to
54.6 FPS (+23.3%). See
[`04-performance.md`](04-performance.md#7-ppc-memory-placement).

## 8. Worked integration sequence

```text
MGLInit / driver start:
    base = OpenLibrary("Radeon9200.chip", 3)
    info.Size = sizeof(info)
    dev  = Radeon3DOpen(RADEON3D_IFACE_VERSION, &info)
    require CAP_CP_READY, CAP_BITMAP_IMPORT, CAP_PHASE2_EXECUTE
    for streaming: require CAP_STREAM_SEGMENTS
    for indirect:  require CAP_INDIRECT_DISPATCH + CAP_INDIRECT_RENDER

context creation:
    color = ImportBitMap(window/screen buffer)   or AllocSurface(...)
    depth = AllocSurface(w, h, R5G6B5PC)         (aux pool)
    if streaming: AllocSegment(256 KiB) x N

texture upload:
    import a P96 texture bitmap (or aux surface)
    write texels (CPU), flush cache
    Execute a record that binds it with RADEON3D_TEX_CONTENT_* serial

frame:
    Execute  clear record (color [+depth])
    if streaming:
        write TCL vertices into free segment slot, flush cache
        CommitBatch(header-only draw chain + offsets, FENCE)
    else:
        Execute  semantic draw records (FENCE)
    present via the normal window/screen-buffer mechanism
    optionally TestFence before reusing a slot

shutdown:
    Radeon3DReleaseSurface(each)
    Radeon3DFreeSegment(each)
    Radeon3DClose(dev)
    CloseLibrary(base)
```

## 9. Common client mistakes

- **Assuming `OpenLibrary("Radeon9200.chip")` finds a path.** It is opened by
  resident name. `Picasso96` loads it from `LIBS:Picasso96/`; clients open the
  name. A debug chip installed under its own file name breaks every MiniGL
  client because `minigl.library` opens the exact internal name.
- **Holding a wait fence per draw.** Each full idle drain is milliseconds of
  GPU time; batch first, wait once per frame or per buffer rotation.
- **Not flushing the PPC cache over segment writes.** Intermittent stale
  geometry; the classic symptom is "works until memory pressure".
- **Writing host-endian floats into a segment.** The vertex fetcher is
  little-endian; swapped floats produce garbage triangles or CP stalls.
- **Reusing a segment range before its fence retires.** Corruption that looks
  like a driver bug.
- **Requiring interface 19 features.** The shipped/deployed driver may be
  older, and the fence-coalescing path is parked. Negotiate and degrade.
- **Mixing depth into a P96 screen buffer.** Always allocate depth through
  `Radeon3DAllocSurface()`.
- **Treating `Radeon3DDispatchIndirect` as sandboxed.** It is not; it is a
  trusted raw-packet path.
