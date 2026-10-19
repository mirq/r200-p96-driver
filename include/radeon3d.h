#ifndef RADEON3D_H
#define RADEON3D_H

#include <exec/types.h>

#define RADEON3D_LIBRARY_VERSION 3UL
#define RADEON3D_IFACE_VERSION   20UL

#define RADEON3D_CAP_CP_READY     (1UL << 0)
#define RADEON3D_CAP_SINGLE_BOARD (1UL << 1)
#define RADEON3D_CAP_OWNER_PINNED  (1UL << 2)
#define RADEON3D_CAP_PACKET2_SUBMIT (1UL << 3)
#define RADEON3D_CAP_FENCES         (1UL << 4)
#define RADEON3D_CAP_BITMAP_IMPORT  (1UL << 5)
#define RADEON3D_CAP_IMMD_TRI_LIST  (1UL << 6)
/* Bits 7-20 and 22-24 retired with the semantic record path (the 0.40
 * remove-semantic change): PHASE2_EXECUTE, DEPTH_FUNCS, TEXTURE_STATE,
 * FOG_MULTITEX, COLOR_TARGET_FORMATS, NATIVE_TRI_PRIMITIVES,
 * NATIVE_QUAD_LISTS, HW_TRANSFORM_CLIP, HW_TEXGEN, HW_NORMALS,
 * HW_LIGHTING, HW_SPHERE_MAP, COMPACT_TCL_VERTEX, COMMIT_STATE_REUSE,
 * COMMIT_STATE_BATCH, ORDERED_COMMITS. The positions are never reused
 * below interface 21. TEST_INVALIDATE keeps bit 11 (the diagnostic
 * recovery hook is unrelated to the record path). */
/* Diagnostic builds only; the ABI vector remains present in release builds. */
#define RADEON3D_CAP_TEST_INVALIDATE        (1UL << 11)
#define RADEON3D_CAP_STREAM_SEGMENTS        (1UL << 21)
/* Radeon3DAllocSurface() is available and its pool was reserved. */
#define RADEON3D_CAP_AUX_SURFACES           (1UL << 25)
/* Radeon3DDispatchIndirect() is available (interface 18, segment pool live). */
#define RADEON3D_CAP_INDIRECT_DISPATCH      (1UL << 26)
/* Optional interface-18 render transitions; require with INDIRECT_DISPATCH
 * for rendering, not just fetch/fence smoke tests. Not a packet sandbox. */
#define RADEON3D_CAP_INDIRECT_RENDER        (1UL << 27)
/* Test/WaitFence validate any fence the session has submitted, not only
 * device->LastFence, so a consumer may keep several submissions in flight
 * and retire them in order. Interface-18 hosts that need it must check this
 * bit; without it only the latest fence is waitable (single-fence model). */
#define RADEON3D_CAP_MULTI_FENCE            (1UL << 28)
/* Interface 19 fence coalescing. The producer may dispatch indirect
 * buffers WITHOUT a fence (RADEON3D_INDIRECT_NO_FENCE) and close a run of
 * such dispatches later with Radeon3DSubmitFence(), which retires only
 * when every earlier submission has fully drained. In-order CP execution
 * makes the closing fence cover all preceding dispatches, so fewer full
 * idle drains are paid while every fence keeps its exact "everything up
 * to the fence serial is complete" meaning. A service without this bit
 * rejects RADEON3D_INDIRECT_NO_FENCE and has no Radeon3DSubmitFence. */
#define RADEON3D_CAP_FENCE_COALESCE         (1UL << 29)
/* Interface 20 PPC engine lease (docs/09-ppc-direct-ring-design.md). The
 * service may grant an exclusive ring lease: during the lease the PPC is
 * the only ring writer and the 68k refuses every other submission and
 * falls back to software 2D. Advertised only for interface-20 sessions
 * with a live CP and streaming pool, and only when the driver was built
 * with PPC ring support (make PPCRING=0 compiles it out - the fallback
 * flag). A consumer without the capability, or with a refused
 * Radeon3DAcquireLease, uses the existing Radeon3DDispatchIndirect path
 * unchanged. */
#define RADEON3D_CAP_PPC_RING               (1UL << 30)

#define RADEON3D_MAX_BATCH_DWORDS 8192UL
#define RADEON3D_IMMD_MAX_VERTICES 255UL
#define RADEON3D_IMMD_VERTEX_DWORDS 3UL

/* Streaming segments (interface 13). A segment is service-allocated private
 * VRAM that the context-owning task writes through its CpuAddress. A trusted
 * producer builds CP-native indirect buffers in a segment and dispatches
 * them with Radeon3DDispatchIndirect(); segments are freed automatically
 * when the device closes. */
#define RADEON3D_MAX_SEGMENTS      12UL
#define RADEON3D_MAX_SEGMENT_BYTES (256UL * 1024UL)

#define RADEON3D_SEGMENT_VERSION 1UL

struct Radeon3DSegment {
    ULONG Size;
    ULONG Version;
    ULONG Id;
    APTR CpuAddress;
    ULONG GpuAddress;
    ULONG Bytes;
};

#define RADEON3D_SEGMENT_V1_SIZE 24UL

typedef char Radeon3DSegmentV1SizeCheck[
    sizeof(struct Radeon3DSegment) == RADEON3D_SEGMENT_V1_SIZE ? 1 : -1];

/* Trusted indirect dispatch (interface 18). The caller builds a complete
 * CP packet stream into a leased segment and Radeon3DDispatchIndirect()
 * enqueues it with one PACKET0 write of CP_IB_BASE/CP_IB_BUFSZ. The
 * service validates only the extent: segment identity, 16-byte offset
 * alignment, nonzero even dword count and the byte range against the lease.
 * DwordCount includes padding and must not exceed RADEON3D_MAX_BATCH_DWORDS.
 * The producer pads odd streams with one CP-native PACKET2 before flushing
 * and dispatch; the service rejects odd counts rather than padding in place.
 * This follows Linux v2.6.39 radeon_cp_dispatch_indirect(). The stream
 * contents are the trusted producer's responsibility. Extent validation
 * bounds only the initial IB fetch, not packet effects: packets can access
 * registers and VRAM elsewhere, corrupt the desktop or wedge the GPU.
 * Rendering requires both INDIRECT_DISPATCH and INDIRECT_RENDER caps;
 * the latter adds prepare/revalidation, submitted-state and recovery hooks.
 * The producer must flush its data cache over the stream before dispatch. A
 * retiring fence is always appended, so waiting on the returned fence
 * proves the indirect buffer was consumed. */
#define RADEON3D_INDIRECT_VERSION 1UL

/* Interface 19, requires RADEON3D_CAP_FENCE_COALESCE. Suppresses the
 * per-dispatch fence. The dispatch is still ordered by the CP ring, but
 * nothing proves it was consumed until a later submission's fence (or
 * Radeon3DSubmitFence) retires. The caller MUST close every run of
 * no-fence dispatches with a fence before it relies on retirement,
 * releases referenced storage or presents. fenceOut is set to 0. */
#define RADEON3D_INDIRECT_NO_FENCE (1UL << 1)
#define RADEON3D_INDIRECT_FLAGS    RADEON3D_INDIRECT_NO_FENCE

struct Radeon3DIndirect {
    ULONG Size;
    ULONG Version;
    ULONG SegmentId;
    ULONG ByteOffset;
    ULONG DwordCount;
    ULONG Flags;
};

#define RADEON3D_INDIRECT_V1_SIZE 24UL

typedef char Radeon3DIndirectV1SizeCheck[
    sizeof(struct Radeon3DIndirect) == RADEON3D_INDIRECT_V1_SIZE ? 1 : -1];

#define RADEON3D_SUBMIT_FENCE (1UL << 0)
#define RADEON3D_SUBMIT_FLAGS  RADEON3D_SUBMIT_FENCE

/* The RADEON3D_EXEC_* record ABI (clear/draw/reuse opcodes, record size
 * constants, DRAW_/TEX_/TEXGEN_/FRAGMENT_/VERTEX_/TRANSFORM_/BLEND_/
 * LIGHT_/MATERIAL_ options and vertex layouts) was removed with the
 * semantic record path. The only remaining client streams are the fixed
 * immediate triangle list (docs/02 section 11) and trusted CP-native
 * indirect buffers (docs/02 section 9). */

struct Radeon3DDevice;
struct BitMap;

/* Keep the chip-library reference open until after Radeon3DClose returns. */

/*
 * Public structures use only fixed-width AmigaOS types and the normal m68k
 * two-byte ABI alignment. Callers set Size before opening or querying.
 */

/* Where a timing sample came from: one sample is written at the end of
 * every submission call that reached the service, success or failure.
 * Types 1-4 belong to the retired semantic record path; a sample ring
 * captured by an older driver may still contain them. */
#define RADEON3D_SAMPLE_SUBMIT       5UL
#define RADEON3D_SAMPLE_DISPATCH     6UL

#define RADEON3D_SAMPLE_RING_SIZE 1024UL

/* One submission timing sample. Tick fields are raw EClock ticks; convert
 * with EClockHz from the info block. The writer holds the service lock and
 * clears Seq before changing an entry and stores the new Seq last. Readers
 * copy an entry only while Seq matches the expected value, then verify Seq
 * again after the copy. */
struct Radeon3DSample {
    ULONG Seq;             /* global sequence number of this sample */
    ULONG WallTicks;       /* EClock low word at completion */
    ULONG Type;            /* RADEON3D_SAMPLE_* */
    ULONG Result;          /* nonzero when the submission succeeded */
    ULONG RecordDwords;    /* host dwords the call received */
    ULONG GeneratedDwords; /* CP dwords it produced */
    ULONG CopyTicks;       /* staging copy (Submit's trusted snapshot; else 0) */
    ULONG BuildTicks;      /* validation plus command build */
    ULONG SubmitTicks;     /* CP ring submit including fence work */
};

struct Radeon3DInfo {
    ULONG Size;
    ULONG Version;
    ULONG Generation;
    ULONG DeviceId;
    ULONG Caps;
    ULONG InstalledVram;
    ULONG Picasso96Vram;
    ULONG MaxBatchDwords;
    /* V2: cumulative submission phase attribution, present in release
     * builds too. Microsecond totals across all sessions of this service;
     * read them after a workload to attribute submission time between the
     * trusted-stream copy, validation, and the ring submit. The historical
     * "Exec" names are kept for the frozen V2/V4 info ABI. */
    ULONG ExecCalls;
    ULONG ExecRecordDwords;
    ULONG ExecGeneratedDwords;
    ULONG ExecCopyMicros;
    ULONG ExecBuildMicros;
    ULONG ExecSubmitMicros;
    /* V3: which check rejected the most recent submission, zero if none
     * has failed. Stage numbers are internal to the service and only
     * meaningful when read right after a failed call. */
    ULONG CommitFailStage;
    /* V4: release-safe per-submission timing ring. SampleRing points at a
     * driver-owned RADEON3D_SAMPLE_RING_SIZE-entry Radeon3DSample array
     * written under the service lock; SampleSeq counts the samples written
     * since the driver loaded. EClockHz is the timer.device EClock rate, so
     * raw tick fields convert to time as ticks * 1e6 / EClockHz. All four
     * fields read zero on a driver that never opened its timer. */
    APTR SampleRing;
    ULONG SampleRingEntries;
    ULONG SampleSeq;
    ULONG EClockHz;
};

#define RADEON3D_INFO_V1_SIZE 32UL
#define RADEON3D_INFO_V2_SIZE 56UL
#define RADEON3D_INFO_V3_SIZE 60UL
#define RADEON3D_INFO_V4_SIZE 76UL

/* requestedVersion is the newest interface version understood by the caller. */

/*
 * Radeon3DSubmit accepts PACKET2 no-ops and the bounded immediate triangle-list
 * stream documented in docs/02-service-abi-reference.md. General register and
 * draw packets are restricted to the separate trusted interface-18 indirect
 * path. The retired semantic record interface kept its handles as the opaque
 * values returned in Radeon3DSurface.Handle; the indirect path references
 * segments by id instead.
 */

typedef char Radeon3DInfoV4SizeCheck[
    sizeof(struct Radeon3DInfo) == RADEON3D_INFO_V4_SIZE ? 1 : -1];
#ifdef __GNUC__
typedef char Radeon3DInfoV1PrefixCheck[
    __builtin_offsetof(struct Radeon3DInfo, ExecCalls) ==
        RADEON3D_INFO_V1_SIZE ? 1 : -1];
#endif

#define RADEON3D_SURFACE_VERSION 1UL

#define RADEON3D_FORMAT_R5G6B5PC  1UL
#define RADEON3D_FORMAT_B8G8R8A8  2UL
#define RADEON3D_FORMAT_CLUT8      3UL

struct Radeon3DSurface {
    ULONG Size;
    ULONG Version;
    ULONG Generation;
    APTR CpuAddress;
    ULONG GpuAddress;
    ULONG Pitch;
    ULONG Width;
    ULONG Height;
    ULONG Format;
    APTR Handle;
};

#define RADEON3D_SURFACE_V1_SIZE 40UL

typedef char Radeon3DSurfaceV1SizeCheck[
    sizeof(struct Radeon3DSurface) == RADEON3D_SURFACE_V1_SIZE ? 1 : -1];

/* The caller owns an imported BitMap until Radeon3DReleaseSurface returns. */

/* Service-allocated surfaces (interface 17). Radeon3DAllocSurface() carves
 * the surface out of private VRAM the driver reserved before Picasso96 saw
 * the card, so it can never overlap a p96 bitmap or an AllocScreenBuffer()
 * buffer. That matters for depth: the two allocators do not know about each
 * other, and depth landing inside a screen buffer scribbles bands across the
 * displayed frame. The service pads Pitch up to the 128-byte tile alignment
 * the R200 depth unit needs and reports the padded pixel count in Width, so
 * Width may exceed the requested width. Release with Radeon3DReleaseSurface(),
 * which returns the VRAM to the pool; surfaces still held when the device
 * closes are released with it. */
#define RADEON3D_AUX_POOL_BYTES     (4UL * 1024UL * 1024UL)
#define RADEON3D_MAX_AUX_SURFACES   8UL
#define RADEON3D_AUX_MAX_DIMENSION  4096UL

/*
 * Interface-20 PPC engine lease (docs/09-ppc-direct-ring-design.md).
 *
 * Radeon3DAcquireLease() grants an exclusive ring lease to the calling
 * session: while the lease is live the PPC is the only ring writer, the
 * service rejects every other submission (failure stages 120-125 are
 * written to CommitFailStage), Picasso96 2D falls back to software, and
 * the lease dies with the session or after an expiry bound. The
 * descriptors are the 68k-mapped addresses the PPC aliases 1:1
 * (Phase-0-verified). RingCpuAddress is the CP ring base, Bar2Base the
 * RV280 MMIO base (CP_RB_RPTR 0x0710, CP_RB_WPTR 0x0714, SCRATCH_REG0
 * 0x15e0). NextFence is the first scratch sequence the PPC may use; the
 * CP consumes scratch writes in ring order, so releasing with the last
 * submitted fence retires every lease submission.
 *
 * Radeon3DReleaseLease(device, lastFence) drains via that fence (bounded
 * wait), re-arms the 68k ring state and restores 2D on the next P96
 * operation. Every path is fail-closed: without the capability, or when
 * the lease is refused or already held, the caller uses the existing
 * bounded entry points unchanged.
 */
#define RADEON3D_LEASE_VERSION   1UL

struct Radeon3DLease {
    ULONG Size;             /* in: caller's buffer size; out: tier filled */
    ULONG Version;          /* out: RADEON3D_LEASE_VERSION */
    ULONG Generation;       /* out: service generation at grant */
    ULONG Flags;            /* out: RADEON3D_LEASE_* */
    APTR  RingCpuAddress;   /* out: ring base, 68k-mapped, PPC-aliased */
    ULONG RingGpuAddress;   /* out: ring base as the CP fetches it */
    ULONG RingDwords;       /* out: ring size in dwords */
    ULONG RingMask;         /* out: RingDwords - 1 */
    ULONG Bar2Base;         /* out: RV280 MMIO base (BAR2) */
    ULONG Bar0Base;         /* out: framebuffer aperture base (BAR0) */
    ULONG NextFence;        /* out: first scratch sequence for the lease */
    ULONG HeartbeatMs;      /* out: service lease-expiry bound */
    ULONG MaxBatchDwords;   /* out: same as info.MaxBatchDwords */
};

#define RADEON3D_LEASE_V1_SIZE  52UL
#define RADEON3D_LEASE_FLAGS    0UL

typedef char Radeon3DLeaseV1SizeCheck[
    sizeof(struct Radeon3DLease) == RADEON3D_LEASE_V1_SIZE ? 1 : -1];

#endif
