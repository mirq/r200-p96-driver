# DMASIZE Implementation Notes

## Status

This document originally recorded how the `DMASIZE` ToolType works in:

```text
/home/mirek/warp3d-r9200/Prometheus/PrometheusCard
```

and the contract selected for this `Radeon9200.card` driver.

Version 0.5 implements the private reservation scope:

- Strict, overflow-checked parsing of decimal bytes and optional `K`/`M`.
- Final duplicate occurrence wins; a malformed final value disables the arena.
- Checked 4096-byte alignment and a 4 MiB minimum P96 display-memory floor.
- Transactional metadata creation before reducing `BoardInfo.MemorySize`.
- An opaque per-board arena with a semaphore-protected page-map allocator.
- Startup allocator checks covering zero/overflow requests, exact-size free,
  invalid-size free, double free, best-fit reuse, adjacent free runs, and full
  arena recovery.
- Cleanup before board state and PCI ownership are released.

The arena has no public card-library vectors in 0.5. OpenPCI `MEM_PCI`
publication, client address translation, and real inter-card DMA remain
unimplemented separate scopes.

### Real-Hardware Validation

The implementation was cold-booted on the documented 68060/RV280 target with
a 64 MiB usable aperture:

| ToolType | P96 `MemorySize` | Result |
| --- | ---: | --- |
| `dmasize=2048k` | 65,011,712 | 2 MiB private tail active |
| `dmasize=1Kfoo` | 67,108,864 | malformed value rejected nonfatally |
| `dmasize=00001` | 67,104,768 | one byte rounded to one 4096-byte page |
| `dmasize=24576` | 67,084,288 | six-page arena and self-test accepted |
| `dmasize=0064m` | 67,108,864 | full-VRAM request rejected by display floor |

With the 2 MiB reservation active, a Radeon `1024x768x8` Workbench and the
8/16/32/8 P96 screen sequence remain stable with correct direct-color
readback and no recorded crash.

## Summary

`DMASIZE` is a request to hide part of the graphics card's VRAM from
Picasso96. The hidden region is taken from the high end of usable VRAM and can
then be managed by the card driver as private, DMA-capable board memory.

It is not the same as allocating host DMA memory. In particular, it must not be
implemented by calling:

```c
pci_allocdma_mem(size, MEM_PCI);
```

The intended memory layout is:

```text
MemoryBase                                                   end of usable VRAM
    |                                                               |
    v                                                               v
    +-------------------------+-------------------------------------+
    | Picasso96-managed VRAM  | driver-private DMASIZE arena        |
    +-------------------------+-------------------------------------+
                              ^
                              DmaBase

    <----- BoardInfo.MemorySize ----->
    <------------ physical/usable VRAM span ------------------------>
```

`BoardInfo.MemorySize` must stop at `DmaBase`, while
`BoardInfo.MemorySpaceSize` must continue to describe the complete framebuffer
aperture.

The recommended initial implementation is a private Radeon arena. It can be
used by future internal Radeon resources and, if explicitly designed, by
Radeon-specific extra library vectors. It cannot automatically become the
OpenPCI 2.1 `MEM_PCI` provider.

## Reference Implementation

### ToolType Detection

The reference parser is in:

```text
PrometheusCard/card.c:179-249
```

It first uses:

```c
#define HAS_TOOLTYPES (bi->GetVSyncState != NULL)
```

as a compatibility test for a Picasso96 version that supplies ToolTypes. It
then walks the null-terminated `char **ToolTypes` array and compares the first
eight characters case-insensitively with `DMASIZE=`.

The recognized reference forms include decimal bytes and `K` or `M` suffixes:

```text
DMASIZE=65536
DMASIZE=64K
DMASIZE=1M
```

The reference accumulates decimal digits and shifts the current value by 10 or
20 bits when it sees `K` or `M`.

### VRAM Reservation

The reservation is committed in:

```text
PrometheusCard/card.c:299-306
```

The effective reference operation is:

```c
if (dma_size > 0 && dma_size <= bi->MemorySize) {
    cb->cb_DMAMemGranted = TRUE;
    bi->MemorySize -= dma_size;
    InitDMAMemory(cb,
                  (APTR)((ULONG)bi->MemoryBase + bi->MemorySize),
                  dma_size);
}
```

This makes the private arena the high tail of VRAM. Picasso96 sees only the
reduced `MemorySize` after `InitCard()` returns.

The Picasso96 example driver uses the same distinction between allocatable
display memory and the full memory aperture:

```text
Picasso96Develop/HardWare/PiccoloSD64.card.asm:400-410
```

It subtracts `RESERVEDMEMORY` from `MemorySize` while leaving
`MemorySpaceSize` at the complete physical size.

### Reference Allocator

The reference allocator is in:

```text
PrometheusCard/dma.c
```

It uses:

- A sorted linked list of `DMAMemChunk` records.
- A free/allocated state represented by `dmc_Owner == NULL`.
- Best-fit allocation.
- Splitting when a free chunk is larger than the request.
- Coalescing with free predecessor and successor chunks.
- A `SignalSemaphore` around allocator operations.
- A memory pool for chunk metadata.
- 4096-byte alignment for returned addresses.

The allocator metadata structures are declared in:

```text
PrometheusCard/card.h:22-52
```

The arena is initialized as one free chunk by `InitDMAMemory()` in:

```text
PrometheusCard/dma.c:155-178
```

### Extra Card Library Vectors

The reference appends two functions after the standard `FindCard` and
`InitCard` vectors:

```text
PrometheusCard/vbcc_libinit.c:79-90
PrometheusCard/prometheus_card_lib.sfd:9-12
```

The resulting vector order is:

| LVO | Function | Registers |
| ---: | --- | --- |
| `-30` | `FindCard` | `A0 = BoardInfo` |
| `-36` | `InitCard` | `A0 = BoardInfo`, `A1 = ToolTypes` |
| `-42` | `AllocDMAMem` | `D0 = size` |
| `-48` | `FreeDMAMem` | `A0 = memory`, `D0 = size` |

These allocation vectors are a Prometheus extension. They are not standard
Picasso96 card-driver vectors.

## Prometheus And OpenPCI Publication Path

The reference card does not register an arbitrary VRAM range with Picasso96 or
OpenPCI. Instead, `prometheus.library` knows how to reach the allocator.

In:

```text
/home/mirek/warp3d-r9200/Prometheus/PromLib/driver_2.c:211
```

the provider name is hardcoded as:

```c
#define CARD_NAME "Picasso96/Prometheus.card"
```

`Prm_AllocDMABuffer()` then opens that card and calls its `AllocDMAMem` vector:

```text
PromLib/driver_2.c:2248-2267
```

`Prm_FreeDMABuffer()` uses the corresponding free vector:

```text
PromLib/driver_2.c:2304-2323
```

OpenPCI 2.1's Prometheus backend forwards its DMA calls to these Prometheus
functions. The local exact OpenPCI binary contains wrappers at text offsets
`0x1a68` and `0x1a7c` that call Prometheus LVOs `-96` and `-102`, corresponding
to `Prm_AllocDMABuffer` and `Prm_FreeDMABuffer`.

The later published OpenPCI source expresses the same behavior directly:

```c
APTR PROMETHEUSpci_allocdma_mem(unsigned long size, unsigned long flags)
{
    return Prm_AllocDMABuffer(size);
}

void PROMETHEUSpci_freedma_mem(APTR buffer, unsigned long size)
{
    Prm_FreeDMABuffer(buffer, size);
}
```

The `flags` argument is ignored by that Prometheus backend.

This has an important consequence for `Radeon9200.card`:

- OpenPCI does not discover DMA providers by inspecting the active P96 card.
- `pci_allocdma_mem()` has no device argument and cannot select a Radeon board.
- The Prometheus path always reaches `Picasso96/Prometheus.card`.
- Adding `AllocDMAMem` to `Radeon9200.card` will not make OpenPCI call it.
- There is no OpenPCI 2.1 API for publishing a new provider range from this
  driver.

Therefore the current statement in `README.md` that OpenPCI publication is
unresolved remains correct. The private reservation itself is implementable;
generic OpenPCI publication is not implementable in this card driver alone.

## Intended Scope

Three possible scopes must not be confused:

| Scope | Feasibility | Recommendation |
| --- | --- | --- |
| Private memory for Radeon driver internals | Implementable entirely in `Radeon9200.card` | Implement first |
| Memory exposed through Radeon-specific extra vectors | Implementable with a new documented ABI | Optional after private allocator validation |
| Provider for OpenPCI `pci_allocdma_mem(MEM_PCI)` | Not implementable in this card alone | Keep out of scope |

Potential internal users include a future command ring, fence/writeback area,
cursor image, scratch memory, upload staging, and private rendering resources.
Those regions must never be returned to or freed by an external client.

## Recommended ToolType Contract

### Accepted Syntax

Accept an exact case-insensitive key followed by a strictly checked value:

```text
DMASIZE=0
DMASIZE=4096
DMASIZE=64K
DMASIZE=64k
DMASIZE=1M
DMASIZE=1m
```

The value is decimal bytes with an optional single suffix:

| Suffix | Multiplier |
| --- | ---: |
| none | `1` |
| `K` or `k` | `1024` |
| `M` or `m` | `1048576` |

### Rejected Syntax

Reject values such as:

```text
DMASIZE=
DMASIZE=-1
DMASIZE= 1M
DMASIZE=1 M
DMASIZE=1MB
DMASIZE=1Kfoo
DMASIZE=K
DMASIZE=1MM
DMASIZE=4294967296
```

No whitespace, sign, decimal point, second suffix, or trailing text should be
accepted.

### Zero And Invalid Values

Recommended behavior:

- `DMASIZE=0` explicitly disables the private arena.
- An absent ToolType leaves the private arena disabled.
- A malformed value leaves the private arena disabled.
- A value that cannot fit leaves the private arena disabled.
- A failed arena metadata allocation leaves the private arena disabled.
- Display initialization should remain usable when DMA reservation is disabled.
- Any future feature that requires the arena must remain disabled if the arena
  was not created.

This preserves the nonfatal behavior of the reference while avoiding a partial
or overlapping reservation.

If duplicate `DMASIZE` entries are supported, use the final occurrence. A
malformed final occurrence should disable the request rather than silently use
an earlier value. Rejecting all duplicates is also safe, but must be documented
before implementation.

### ToolType Array Safety

Always check the pointer itself:

```c
if (toolTypes != NULL) {
    while (*toolTypes != NULL) {
        /* parse */
        ++toolTypes;
    }
}
```

If compatibility with old Picasso96 versions is retained, also use the
reference's `bi->GetVSyncState != NULL` signal. This project targets a pinned
Picasso96 3.6 ABI, so it should not rely on this signal instead of checking
`toolTypes`.

Parse all supported options in one pass. The current
`ToolTypesRequestVga()` helper can be replaced with an options parser rather
than scanning the same array separately for every ToolType.

An illustrative options structure is:

```c
struct RadeonOptions {
    ULONG DmaRequested;
    UBYTE VgaOutput;
    UBYTE DmaSpecified;
    UBYTE DmaValid;
    UBYTE Reserved;
};
```

## Checked Parsing

The parser must detect overflow before every arithmetic operation. For each
decimal digit `digit`:

```c
if (value > (ULONG_MAX - digit) / 10UL)
    return FALSE;

value = value * 10UL + digit;
```

Before applying a suffix multiplier:

```c
if (value > ULONG_MAX / multiplier)
    return FALSE;

value *= multiplier;
```

On this freestanding target, `ULONG_MAX` can be represented by a local constant
such as `~0UL` if the selected headers do not provide it.

## Checked Reservation Arithmetic

### Usable Span

The arena must be reserved from the minimum span proven to be:

- Physically installed VRAM.
- CPU-visible through BAR0.
- Addressable by the Radeon memory controller for the intended use.

For the current milestone, the first two bounds are represented by:

```c
usable = min(data->InstalledVram,
             min(bi->MemorySize, bi->MemorySpaceSize));
```

Hardware initialization already clamps `InstalledVram` to the validated Radeon
aperture, but retaining all three bounds makes the publication contract clear.

### Alignment

Use a 4096-byte alignment, matching the reference's returned-address
alignment. Round the reserved size up with checked arithmetic:

```c
if (requested > ULONG_MAX - 4095UL)
    reject_request();

reserved = (requested + 4095UL) & ~4095UL;
```

The current VRAM probe accepts a reported size only when it is 1 MiB aligned,
and validated BAR sizes are powers of two. Therefore subtracting a 4096-byte
aligned reservation produces a 4096-byte aligned private base.

### Minimum Display Memory

Do not allow the reservation to consume all usable VRAM. An initial
conservative condition is:

```c
reserved <= usable - RADEON_FRAMEBUFFER_MIN_SIZE
```

with the subtraction performed only after confirming:

```c
usable >= RADEON_FRAMEBUFFER_MIN_SIZE
```

`RADEON_FRAMEBUFFER_MIN_SIZE` currently describes the minimum accepted BAR and
VRAM size, not a fully derived mode-memory requirement. It is a safe initial
floor. It can later be replaced by a proven maximum requirement for mandatory
display state and the minimum supported mode.

### Half-Open Ranges

Track ranges as half-open intervals:

```text
[0, p96Size)             Picasso96-managed VRAM offsets
[p96Size, usable)        private DMASIZE offsets
```

This permits straightforward checked membership tests and avoids inclusive-end
overflow.

### Transactional Commit

Do not reduce `BoardInfo.MemorySize` until all allocator metadata has been
successfully created. The commit order should be:

1. Parse and validate the requested byte count.
2. Probe and validate installed VRAM.
3. Calculate aligned ranges with checked arithmetic.
4. Allocate and initialize all arena metadata.
5. Publish the reduced `BoardInfo.MemorySize` and opaque arena pointer with no
   fallible operation remaining.

If any step before the commit fails, free temporary metadata and leave
`MemorySize` unchanged.

Illustrative reservation logic:

```c
usable = data->InstalledVram;

if (request_valid && request != 0 &&
    align_up_checked(request, 4096, &reserved) &&
    usable >= RADEON_FRAMEBUFFER_MIN_SIZE &&
    reserved <= usable - RADEON_FRAMEBUFFER_MIN_SIZE) {
    p96Size = usable - reserved;

    if (pointer_add_checked(bi->MemoryBase, p96Size, &dmaBase) &&
        RadeonDmaArenaCreate(dmaBase, reserved, &arena)) {
        bi->MemorySize = p96Size;
        data->DmaArena = arena;
    }
}

/* MemorySpaceSize remains the validated full aperture established earlier. */
```

The exact helper names are illustrative and need not be introduced if the same
checks remain clearer inside one function.

## Per-Board State

`BoardInfo.CardData` is exactly 64 bytes. Version 0.5 removed redundant cached
framebuffer, MMIO, and BAR-size fields in favor of the canonical `BoardInfo`
members, then stored one opaque `RadeonDmaArena *` in `RadeonBoardData`.
Version 0.6 uses the remaining bytes for the Radeon GPU framebuffer base and
bounded 2D-engine state. The existing `RadeonBoardDataFitsCardData` assertion
remains active.

The arena owns its effective base, size, semaphore, and allocation metadata.
This avoids keeping three copies of the same range state in `CardData`.

The implemented internal layout is conceptually:

```c
struct RadeonDmaArena {
    struct SignalSemaphore Lock;
    UBYTE *Base;
    ULONG Size;
    ULONG PageCount;
    ULONG AllocationSize;
    UBYTE ShuttingDown;
    struct RadeonDmaPage Pages[];
};

struct RadeonDmaPage {
    ULONG RunLength;
    ULONG RequestedSize;
};
```

Free pages contain zero metadata. An allocated run records its page count and
original request on the first page; continuation pages use a reserved marker.
Allocation scans contiguous free runs and chooses the smallest fitting run.
Free validates the exact page-aligned pointer and original request size, then
clears the run. Adjacent free runs coalesce implicitly because free pages are
contiguous zero entries.

## Allocator Behavior

### Initialization

Arena creation:

1. Reject a null base, zero size, or unaligned base/size.
2. Calculate page-map metadata size with checked arithmetic.
3. Allocate one cleared context containing the complete map.
4. Initialize the semaphore and immutable range fields.
5. Run the allocator self-test before returning the arena for publication.

### Allocation

The private allocator:

1. Reject a zero request.
2. Round the request up to 4096 bytes with checked arithmetic.
3. Obtain the arena semaphore.
4. Find the smallest contiguous free page run large enough for the request.
5. Mark its first and continuation pages and record the original request size.
6. Return `DmaBase + page_index * 4096`.
7. Release the semaphore on every exit path.

Rounding each allocation to 4096 bytes is simpler and less wasteful than the
reference's method of adding an entire page and aligning an interior pointer.
It also means every block boundary remains page aligned.

### Free

The private free path:

1. Reject a null pointer or zero size.
2. Obtain the arena semaphore.
3. Require an in-range, page-aligned address at the start of an allocated run.
4. Validate the supplied size and every continuation marker.
5. Clear the complete run; adjacent free space then coalesces implicitly.
6. Release the semaphore on every exit path.

Allocation and free are private C helpers in 0.5, not card-library vectors.

### Concurrency And Context

- Allocation and free must be serialized.
- The functions must not be called from interrupt context.
- Arena teardown must not race with allocation or free.
- No code may wait for Radeon hardware while holding the allocator semaphore.
- Any future engine semaphore needs an explicit lock order relative to the
  allocator semaphore.

### Internal Versus Client Allocations

If the arena later stores both driver internals and client allocations, divide
it explicitly. Internal command-ring, fence, cursor, or scratch blocks cannot
be represented as ordinary client allocations and cannot be accepted by the
public free vector.

A possible future private layout is:

```text
[P96 boundary guard]
[cursor storage]
[command ring]
[fence/writeback/scratch]
[upload staging]
[client-visible private heap]
[final guard]
```

The initial implementation does not need all of these regions, but its range
model must not prevent adding them safely.

## Address Meanings

The allocator returns a logical CPU address inside the framebuffer BAR mapping.
That address has several distinct representations:

| Representation | Meaning |
| --- | --- |
| CPU/logical address | `DmaBase + offset`, returned by the allocator |
| Framebuffer offset | Logical address minus `Framebuffer`, after validation |
| PCI bus address | Result of the bridge's logical-to-physical conversion |
| Radeon GPU address | Address derived from validated Radeon framebuffer/MC configuration |

These values must not be treated as interchangeable.

An external PCI bus-master client needs the bridge-visible PCI address. On the
Prometheus OpenPCI backend, it can use:

```c
pci_logic_to_physic_addr(logicalAddress, pcidev);
```

An internal Radeon command processor needs a validated Radeon GPU address,
normally derived from the framebuffer offset and memory-controller setup. It
must not blindly use the CPU pointer or an OpenPCI bus address.

Memory reads and writes must continue to follow this project's OpenPCI access
rules. Returning a pointer identifies the range; it does not authorize direct
volatile CPU dereferences inside the driver.

## Optional Radeon-Specific Extra Vectors

If a cooperating Warp3D or other client needs allocations, append two vectors
after `InitCard` in `src/library.c`:

```c
static APTR FunctionTable[] = {
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibReserved,
    (APTR)FindCard,
    (APTR)InitCard,
    (APTR)RadeonAllocDMAMemory,
    (APTR)RadeonFreeDMAMemory,
    (APTR)-1
};
```

Suggested registerized signatures matching the historical extension are:

```c
APTR RadeonAllocDMAMemory(
    __REGD0(ULONG size),
    __REGA6(struct RadeonCardBase *base));

void RadeonFreeDMAMemory(
    __REGA0(APTR memory),
    __REGD0(ULONG size),
    __REGA6(struct RadeonCardBase *base));
```

Appending preserves the standard vector positions:

| LVO | Function |
| ---: | --- |
| `-30` | `FindCard` |
| `-36` | `InitCard` |
| `-42` | Radeon allocation extension |
| `-48` | Radeon free extension |

The vectors can use `base->BoardInfo` to locate the per-board arena. They must
return failure when no successfully initialized board and arena are present.

This legacy shape has no board argument, so it becomes ambiguous if one card
library instance manages multiple Radeon boards. The current implementation
already supports only one active `base->BoardInfo`. If multi-board support is
added, either reject legacy allocation in ambiguous cases or define a new
versioned API carrying an explicit board handle.

If these vectors become a public contract:

- Document them in an SFD/FD or dedicated client header.
- Bump the card library version only when the ABI is ready.
- Require clients to hold an `OpenLibrary()` reference while memory is live.
- Define whether frees from a different task are accepted.
- Define cleanup behavior for leaked client allocations.
- Verify generated m68k entry code and register placement.

If the arena remains internal, do not add public vectors or bump the library
version merely to imitate `Prometheus.card`.

## Library And Board Lifetime

The current card base contains one `BoardInfo` pointer, and
`RadeonReleaseBoard()` performs board cleanup when the final card-library open
is closed or initialization fails.

DMA cleanup must be added before `ClearBoardData()`:

1. Prevent new internal or client allocations.
2. Stop or disable any engine component using private memory.
3. Destroy allocation metadata and the arena context.
4. Clear `DmaArena`, `DmaBase`, and `DmaSize`.
5. Restore PCI state and release the device.
6. Clear the remaining board data.

VRAM itself is not returned to Exec. Destroying the arena only removes driver
metadata and ownership records. On board teardown, Picasso96 is also losing the
board, so there is no need to republish the hidden tail dynamically.

External clients must keep the card library open while allocations are live.
This prevents the final close from destroying the arena during a legitimate
call. A caller that uses vectors without opening the library violates the
library ABI and cannot be made safe by allocator metadata.

## Picasso96 Flags

The reference sets:

```c
bi->Flags |= BIF_GRANTDIRECTACCESS;
```

while parsing a positive `DMASIZE` value. This behavior should not be copied as
a DMA marker.

In the Picasso96 3.6 header:

```text
Picasso96Develop/PrivateInclude/boardinfo.h:655
```

`BIF_GRANTDIRECTACCESS` means all data on the board can be accessed at any time
without calling `SetMemoryMode()`. It describes framebuffer access semantics,
not the presence of a DMA arena.

Set or clear this flag independently, only after the Radeon memory-mode and CPU
access behavior justifies it. No Picasso96 flag is required merely to hide the
high VRAM tail through `MemorySize`.

## Problems In The Reference That Must Not Be Copied

### Permissive And Overflowing Parser

`card.c:224-239` ignores unrecognized characters and permits `K` or `M` at any
position. Examples such as `1Mgarbage`, `1MM`, or mixed suffix/digit sequences
can produce surprising values. Decimal accumulation and shifts are unchecked
and can wrap a 32-bit `ULONG`.

### Null ToolType Pointer

The reference tests `GetVSyncState` but does not separately check `ToolTypes`
before dereferencing it.

### Flag Set Before Reservation Success

`BIF_GRANTDIRECTACCESS` is set as soon as a positive value is parsed, even if
the board is not found, the request is too large, or the required Prometheus
version is unavailable.

### Full-VRAM Reservation Allowed

The condition accepts `dma_size == bi->MemorySize`, which can leave Picasso96
with no display memory.

### Allocation Size Overflow And Waste

The reference uses:

```c
size = (size + ALIGNMENT + 3) & ~3;
```

This addition can overflow. It also reserves roughly one extra 4096-byte page
for every allocation, even when the free block is already page aligned.

### Partial Arena Initialization

`InitDMAMemory()` allocates the initial chunk descriptor after the caller has
already reduced `MemorySize` and marked DMA granted. If descriptor allocation
fails, VRAM remains hidden but the allocator has no usable free block.

### Semaphore Leaks

In `dma.c:113-123`, the block-not-found and double-free paths return without
releasing `cb_MemSem`. One invalid free can deadlock all later allocator calls.

### Supplied Free Size Not Validated

`FreeDMAMemory()` checks only whether `memsize` is zero. It does not verify that
the supplied size matches the original request, despite the public contract
saying it must match.

### Uninitialized Or Stale Aligned Addresses

Free and newly split chunks do not consistently initialize or clear
`dmc_AlignedAddr`. Lookup is based on this field, making stale metadata
possible.

### Single Global Arena

Allocator state is held in the card library base and
`cb_DMAMemGranted` prevents another arena. That matches the old one-board
extension but is not a suitable foundation for explicit multi-board support.

### Error Reporting

Oversized or otherwise unusable requests are silently ignored. The Radeon
implementation may preserve nonfatal display initialization, but should expose
the effective arena size through debug output or a future query API so users do
not assume that an invalid request succeeded.

## Implemented File Map

### `src/radeon9200.c`

- Parses `OUTPUT=VGA` and `DMASIZE=` in one null-safe pass.
- Applies final-occurrence semantics and keeps malformed DMA requests nonfatal.
- Creates the arena after hardware and startup output succeed, before final
  callback publication.
- Destroys arena metadata before clearing board state and releasing PCI access.

### `src/radeon9200.h`

- Stores one opaque `RadeonDmaArena *` in the fixed `CardData` state.
- Retains the compile-time `RadeonBoardDataFitsCardData` assertion.
- Uses canonical `BoardInfo` framebuffer/MMIO fields instead of redundant
  cached pointers, preserving the 64-byte ABI limit.

### `src/dma.c`

- Performs checked reservation alignment and high-tail range accounting.
- Allocates one page-map context transactionally.
- Implements semaphore-protected best-fit allocation and exact-size free.
- Runs allocator consistency and recovery checks before publishing the arena.

### `src/library.c`

- Identifies the current implementation as version 0.6; `DMASIZE` itself was
  introduced in version 0.5.
- Keeps the standard function table unchanged; no client allocation ABI exists.

### `Makefile`

- Links `src/dma.c` and `src/radeon_accel.c` while preserving startup and
  library as the first objects.

## Verification Plan

### Build And ABI

1. Run `make` with warnings treated as errors.
2. Confirm `RadeonBoardDataFitsCardData` still compiles.
3. Inspect the resident and function table in the built card.
4. If extra vectors are added, verify `FindCard`, `InitCard`, allocate, and free
   remain at LVOs `-30`, `-36`, `-42`, and `-48`.
5. Inspect generated m68k code to verify `D0`, `A0`, and `A6` argument placement.

### Parser Cases

| Input | Expected effective request |
| --- | ---: |
| no `DMASIZE` | `0` |
| `DMASIZE=0` | `0` |
| `DMASIZE=1` | `4096` after arena alignment |
| `DMASIZE=4096` | `4096` |
| `DMASIZE=64K` | `65536` |
| `DMASIZE=64k` | `65536` |
| `DMASIZE=1M` | `1048576` |
| `DMASIZE=1m` | `1048576` |
| `DMASIZE=` | invalid, arena disabled |
| `DMASIZE=-1` | invalid, arena disabled |
| `DMASIZE=1MB` | invalid, arena disabled |
| `DMASIZE=1Mfoo` | invalid, arena disabled |
| decimal overflow | invalid, arena disabled |
| suffix multiplication overflow | invalid, arena disabled |
| larger than usable VRAM | arena disabled |
| leaves less than minimum display memory | arena disabled |

Parser and arithmetic checks can be exercised without hardware, but they do
not replace target validation. Project policy prohibits mocked hardware tests.

### Range Accounting

On real hardware, verify:

1. No ToolType leaves `MemorySize` equal to usable installed VRAM.
2. `DMASIZE=1M` reduces `MemorySize` by exactly 1 MiB.
3. `MemorySpaceSize` remains equal to the complete validated BAR0 aperture.
4. `DmaBase == Framebuffer + MemorySize`.
5. `[Framebuffer, Framebuffer + MemorySize)` and the DMA arena do not overlap.
6. The DMA arena does not extend beyond installed VRAM.
7. Picasso96 bitmap allocations never enter the private range.
8. Mode setting and framebuffer use continue to work with the reduced size.

### Allocator Behavior

Verify on target hardware:

1. Every returned pointer is 4096-byte aligned.
2. Every returned range is completely inside the private arena.
3. Simultaneous live allocations do not overlap.
4. A zero-size request returns `NULL`.
5. An overflowing request returns `NULL`.
6. Exhaustion returns `NULL` without corrupting the list.
7. Freeing and reallocating an exact block works.
8. Freeing adjacent blocks coalesces them.
9. Freeing the wrong pointer changes no state.
10. Freeing with the wrong size changes no state.
11. Double free changes no state and does not leave the semaphore locked.
12. Repeated allocation/free cycles recover the complete arena.
13. Concurrent task use remains serialized.
14. Board cleanup releases all metadata even after partial initialization.

### Address Translation And DMA

If the memory is exposed to another PCI client, separately verify:

1. `pci_logic_to_physic_addr()` accepts an allocated logical address.
2. The translated bus address is inside the Radeon framebuffer's PCI range.
3. The DMA initiator can address the complete requested block.
4. Transfers use the correct endian and cache-publication rules.
5. Guard bytes around the target block remain unchanged.
6. No transfer touches Picasso96-managed memory.

Successful allocation and address translation do not prove working DMA. A real
device-to-device transfer is required before claiming OpenPCI inter-card DMA
support.

## Recommended Implementation Sequence

1. Completed: strict ToolType parser and checked alignment arithmetic.
2. Completed: opaque per-board arena state.
3. Completed: transactional high-VRAM reservation.
4. Completed: real-hardware verification of reduced P96 `MemorySize`.
5. Completed: private page-map allocator and startup self-test.
6. Pending: integrate internal Radeon users only after their address and
   lifetime rules are established.
7. Pending: add a client ABI only when a concrete client and safe board-lifetime
   contract require one.
8. Pending: test logical-to-physical translation and real inter-card DMA
   separately.
9. Required: keep OpenPCI `MEM_PCI` publication marked unsupported unless the
   bridge or OpenPCI provider architecture is deliberately changed.

## Acceptance Criteria

The version 0.5 private scope satisfies:

- Parsing is strict and overflow-safe.
- Reservation arithmetic is checked and page aligned.
- Picasso96 cannot allocate the private high-VRAM range.
- `MemorySpaceSize` still describes the full aperture.
- Metadata initialization is transactional.
- Allocation, free, best-fit reuse, and implicit coalescing are bounded and
  semaphore-safe within the private initialization/lifetime scope.
- Every error path leaves memory accounting and locks consistent.
- Cleanup handles both failed and successful initialization.
- No unrelated capability flag is used as a DMA marker.
- Real-hardware tests demonstrate non-overlap and stable display operation.

The following remain outside the completed private scope: public allocation,
concurrent client teardown, PCI address translation, and real peer transfers.

The implementation must not claim generic OpenPCI `MEM_PCI` publication unless
the separate hardcoded Prometheus provider path has also been changed and
validated.

## Primary Source References

| Subject | Source |
| --- | --- |
| ToolType parser | `/home/mirek/warp3d-r9200/Prometheus/PrometheusCard/card.c:192-249` |
| High-VRAM reservation | `/home/mirek/warp3d-r9200/Prometheus/PrometheusCard/card.c:299-306` |
| Allocator | `/home/mirek/warp3d-r9200/Prometheus/PrometheusCard/dma.c` |
| Allocator structures | `/home/mirek/warp3d-r9200/Prometheus/PrometheusCard/card.h:22-52` |
| Extra function vectors | `/home/mirek/warp3d-r9200/Prometheus/PrometheusCard/vbcc_libinit.c:79-90` |
| Extra-vector SFD | `/home/mirek/warp3d-r9200/Prometheus/PrometheusCard/prometheus_card_lib.sfd:9-12` |
| Prometheus provider name | `/home/mirek/warp3d-r9200/Prometheus/PromLib/driver_2.c:211` |
| Prometheus allocation bridge | `/home/mirek/warp3d-r9200/Prometheus/PromLib/driver_2.c:2208-2323` |
| P96 reserved-memory example | `Picasso96Develop/HardWare/PiccoloSD64.card.asm:400-410` |
| P96 direct-access flag meaning | `Picasso96Develop/PrivateInclude/boardinfo.h:655` |
| OpenPCI DMA API documentation | `OpenPci2.1-SDK290208/doc/OpenPCI2.txt:848-910` |
| Current Radeon ToolType code | `ParseOptions()` in `src/radeon9200.c` |
| Current Radeon reservation | `RadeonReserveDmaMemory()` in `src/dma.c` |
| Current Radeon arena state | `src/radeon9200.h` and `src/dma.c` |
| Current card function table | `src/library.c:34-42` |
