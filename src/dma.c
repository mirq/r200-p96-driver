#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <proto/exec.h>

#include "radeon9200.h"
#include "radeon_regs.h"

#define RADEON_DMA_PAGE_SIZE         4096UL
#define RADEON_DMA_PAGE_MASK         (RADEON_DMA_PAGE_SIZE - 1UL)
#define RADEON_DMA_PAGE_CONTINUATION 0xffffffffUL

struct RadeonDmaPage {
    ULONG RunLength;
    ULONG RequestedSize;
};

struct RadeonDmaArena {
    struct SignalSemaphore Lock;
    UBYTE *Base;
    ULONG Size;
    ULONG PageCount;
    ULONG AllocationSize;
    UBYTE ShuttingDown;
    UBYTE Reserved[3];
    struct RadeonDmaPage Pages[1];
};

static APTR ArenaAlloc(struct RadeonDmaArena *arena, ULONG requestedSize,
                       struct ExecBase *sysBase)
{
    struct ExecBase *SysBase = sysBase;
    ULONG alignedSize;
    ULONG requestedPages;
    ULONG bestStart = 0;
    ULONG bestLength = 0xffffffffUL;
    ULONG index;
    APTR result = NULL;

    if (!arena || !requestedSize ||
        requestedSize > ~0UL - RADEON_DMA_PAGE_MASK)
        return NULL;

    alignedSize = (requestedSize + RADEON_DMA_PAGE_MASK) &
                  ~RADEON_DMA_PAGE_MASK;
    requestedPages = alignedSize / RADEON_DMA_PAGE_SIZE;
    if (!requestedPages || requestedPages > arena->PageCount)
        return NULL;

    ObtainSemaphore(&arena->Lock);
    if (arena->ShuttingDown)
        goto done;

    index = 0;
    while (index < arena->PageCount) {
        ULONG start;
        ULONG length;

        if (arena->Pages[index].RunLength != 0) {
            ++index;
            continue;
        }

        start = index;
        while (index < arena->PageCount &&
               arena->Pages[index].RunLength == 0)
            ++index;
        length = index - start;
        if (length >= requestedPages && length < bestLength) {
            bestStart = start;
            bestLength = length;
            if (length == requestedPages)
                break;
        }
    }

    if (bestLength != 0xffffffffUL) {
        ULONG page;

        arena->Pages[bestStart].RunLength = requestedPages;
        arena->Pages[bestStart].RequestedSize = requestedSize;
        for (page = 1; page < requestedPages; ++page) {
            arena->Pages[bestStart + page].RunLength =
                RADEON_DMA_PAGE_CONTINUATION;
            arena->Pages[bestStart + page].RequestedSize = 0;
        }
        result = arena->Base + bestStart * RADEON_DMA_PAGE_SIZE;
    }

done:
    ReleaseSemaphore(&arena->Lock);
    return result;
}

static BOOL ArenaFree(struct RadeonDmaArena *arena, APTR memory,
                      ULONG requestedSize, struct ExecBase *sysBase)
{
    struct ExecBase *SysBase = sysBase;
    ULONG base;
    ULONG address;
    ULONG offset;
    ULONG index;
    ULONG runLength;
    ULONG page;
    BOOL result = FALSE;

    if (!arena || !memory || !requestedSize)
        return FALSE;

    base = (ULONG)arena->Base;
    address = (ULONG)memory;
    if (address < base)
        return FALSE;
    offset = address - base;
    if (offset >= arena->Size || (offset & RADEON_DMA_PAGE_MASK))
        return FALSE;
    index = offset / RADEON_DMA_PAGE_SIZE;

    ObtainSemaphore(&arena->Lock);
    if (arena->ShuttingDown)
        goto done;

    runLength = arena->Pages[index].RunLength;
    if (!runLength || runLength == RADEON_DMA_PAGE_CONTINUATION ||
        arena->Pages[index].RequestedSize != requestedSize ||
        runLength > arena->PageCount - index)
        goto done;

    for (page = 1; page < runLength; ++page) {
        if (arena->Pages[index + page].RunLength !=
            RADEON_DMA_PAGE_CONTINUATION)
            goto done;
    }

    for (page = 0; page < runLength; ++page) {
        arena->Pages[index + page].RunLength = 0;
        arena->Pages[index + page].RequestedSize = 0;
    }
    result = TRUE;

done:
    ReleaseSemaphore(&arena->Lock);
    return result;
}

static BOOL ArenaIsEmpty(const struct RadeonDmaArena *arena)
{
    ULONG page;

    for (page = 0; page < arena->PageCount; ++page) {
        if (arena->Pages[page].RunLength ||
            arena->Pages[page].RequestedSize)
            return FALSE;
    }
    return TRUE;
}

static BOOL ArenaSelfTest(struct RadeonDmaArena *arena,
                          struct ExecBase *sysBase)
{
    APTR first;
    APTR second;
    APTR third;
    APTR merged;
    APTR whole;

    if (ArenaAlloc(arena, 0, sysBase) != NULL ||
        ArenaAlloc(arena, ~0UL, sysBase) != NULL)
        return FALSE;

    first = ArenaAlloc(arena, 1, sysBase);
    if (first != arena->Base || ((ULONG)first & RADEON_DMA_PAGE_MASK) ||
        ArenaFree(arena, first, 2, sysBase) ||
        !ArenaFree(arena, first, 1, sysBase) ||
        ArenaFree(arena, first, 1, sysBase))
        return FALSE;

    if (arena->PageCount >= 4) {
        first = ArenaAlloc(arena, 1, sysBase);
        second = ArenaAlloc(arena, RADEON_DMA_PAGE_SIZE + 1UL, sysBase);
        third = ArenaAlloc(arena, 1, sysBase);
        if (first != arena->Base ||
            second != arena->Base + RADEON_DMA_PAGE_SIZE ||
            third != arena->Base + 3UL * RADEON_DMA_PAGE_SIZE ||
            !ArenaFree(arena, second, RADEON_DMA_PAGE_SIZE + 1UL,
                       sysBase) ||
            !ArenaFree(arena, first, 1, sysBase))
            return FALSE;

        merged = ArenaAlloc(arena, 3UL * RADEON_DMA_PAGE_SIZE, sysBase);
        if (merged != arena->Base ||
            !ArenaFree(arena, merged, 3UL * RADEON_DMA_PAGE_SIZE,
                       sysBase) ||
            !ArenaFree(arena, third, 1, sysBase))
            return FALSE;
    }

    whole = ArenaAlloc(arena, arena->Size, sysBase);
    if (whole != arena->Base ||
        !ArenaFree(arena, whole, arena->Size, sysBase))
        return FALSE;
    return ArenaIsEmpty(arena);
}

static struct RadeonDmaArena *ArenaCreate(UBYTE *base, ULONG size,
                                           struct ExecBase *sysBase)
{
    struct ExecBase *SysBase = sysBase;
    struct RadeonDmaArena *arena;
    ULONG pageCount;
    ULONG extraPages;
    ULONG allocationSize;

    if (!base || !size || ((ULONG)base & RADEON_DMA_PAGE_MASK) ||
        (size & RADEON_DMA_PAGE_MASK))
        return NULL;

    pageCount = size / RADEON_DMA_PAGE_SIZE;
    if (!pageCount)
        return NULL;
    extraPages = pageCount - 1UL;
    if (extraPages >
        (~0UL - sizeof(struct RadeonDmaArena)) /
            sizeof(struct RadeonDmaPage))
        return NULL;
    allocationSize = sizeof(struct RadeonDmaArena) +
                     extraPages * sizeof(struct RadeonDmaPage);

    arena = AllocMem(allocationSize, MEMF_PUBLIC | MEMF_CLEAR);
    if (!arena)
        return NULL;
    InitSemaphore(&arena->Lock);
    arena->Base = base;
    arena->Size = size;
    arena->PageCount = pageCount;
    arena->AllocationSize = allocationSize;

    if (!ArenaSelfTest(arena, sysBase)) {
        FreeMem(arena, allocationSize);
        return NULL;
    }
    return arena;
}

static void ArenaDestroy(struct RadeonDmaArena *arena,
                         struct ExecBase *sysBase)
{
    struct ExecBase *SysBase = sysBase;
    ULONG allocationSize;

    if (!arena)
        return;
    allocationSize = arena->AllocationSize;
    ObtainSemaphore(&arena->Lock);
    arena->ShuttingDown = TRUE;
    ReleaseSemaphore(&arena->Lock);
    FreeMem(arena, allocationSize);
}

BOOL RadeonReserveDmaMemory(struct BoardInfo *bi, ULONG requestedSize)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct ExecBase *sysBase;
    struct RadeonDmaArena *arena;
    ULONG usable;
    ULONG reserved;
    ULONG p96Size;
    ULONG memoryBase;
    UBYTE *dmaBase;

    if (!bi || !data || data->DmaArena || !bi->MemoryBase ||
        bi->MemorySpaceBase != bi->MemoryBase || !requestedSize)
        return FALSE;

    usable = data->InstalledVram;
    if (bi->MemorySize < usable)
        usable = bi->MemorySize;
    if (bi->MemorySpaceSize < usable)
        usable = bi->MemorySpaceSize;
    if (usable < RADEON_FRAMEBUFFER_MIN_SIZE ||
        requestedSize > ~0UL - RADEON_DMA_PAGE_MASK)
        return FALSE;

    reserved = (requestedSize + RADEON_DMA_PAGE_MASK) &
               ~RADEON_DMA_PAGE_MASK;
    if (!reserved ||
        reserved > usable - RADEON_FRAMEBUFFER_MIN_SIZE)
        return FALSE;

    p96Size = usable - reserved;
    memoryBase = (ULONG)bi->MemoryBase;
    if (p96Size > ~0UL - memoryBase ||
        reserved > ~0UL - (memoryBase + p96Size))
        return FALSE;
    dmaBase = (UBYTE *)(memoryBase + p96Size);

    sysBase = bi->ExecBase;
    arena = ArenaCreate(dmaBase, reserved, sysBase);
    if (!arena)
        return FALSE;

    bi->MemorySize = p96Size;
    data->DmaArena = arena;
    RLOG("Radeon9200: reserved %ld KiB DMA arena at %lx\n",
         reserved >> 10, (ULONG)dmaBase);
    return TRUE;
}

void RadeonDestroyDmaMemory(struct BoardInfo *bi)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct RadeonDmaArena *arena;

    if (!data)
        return;
    arena = data->DmaArena;
    data->DmaArena = NULL;
    if (arena)
        ArenaDestroy(arena, bi->ExecBase);
}
