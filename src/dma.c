#include "radeon9200.h"

#define PRIVATE_VRAM_ALIGNMENT 4096UL
#define PRIVATE_VRAM_MASK      (PRIVATE_VRAM_ALIGNMENT - 1UL)

APTR RadeonAllocatePrivateVram(struct BoardInfo *bi, ULONG requestedSize)
{
    ULONG size;

    if (!bi || !bi->MemoryBase || !requestedSize ||
        requestedSize > ~0UL - PRIVATE_VRAM_MASK)
        return NULL;
    size = (requestedSize + PRIVATE_VRAM_MASK) & ~PRIVATE_VRAM_MASK;
    if (size > bi->MemorySize)
        return NULL;
    bi->MemorySize = (bi->MemorySize - size) & ~PRIVATE_VRAM_MASK;
    return (APTR)((ULONG)bi->MemoryBase + bi->MemorySize);
}

BOOL RadeonFreePrivateVram(struct BoardInfo *bi, APTR memory,
                           ULONG requestedSize)
{
    ULONG offset;
    ULONG size;

    if (!bi || !bi->MemoryBase || !memory || !requestedSize ||
        requestedSize > ~0UL - PRIVATE_VRAM_MASK ||
        (ULONG)memory < (ULONG)bi->MemoryBase)
        return FALSE;
    offset = (ULONG)memory - (ULONG)bi->MemoryBase;
    size = (requestedSize + PRIVATE_VRAM_MASK) & ~PRIVATE_VRAM_MASK;
    if (offset != bi->MemorySize || offset > bi->MemorySpaceSize ||
        size > bi->MemorySpaceSize - offset)
        return FALSE;
    bi->MemorySize += size;
    return TRUE;
}
