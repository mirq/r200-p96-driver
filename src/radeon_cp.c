#include <exec/memory.h>
#include <hardware/byteswap.h>
#include <proto/exec.h>
#include <proto/openpci.h>

#include "r200_microcode.h"
#include "radeon9200.h"
#include "radeon_regs.h"

#define CP_RING_SIZE          (1024UL * 1024UL)
#define CP_RING_DWORDS        (CP_RING_SIZE / sizeof(ULONG))
#define CP_RING_MASK          (CP_RING_DWORDS - 1UL)
#define CP_RING_ALIGNMENT     16UL
#define CP_MAX_COMMAND_DWORDS 64UL
#define CP_FENCE_DWORDS       6UL
#define CP_TIMEOUT_POLLS      100000UL

#define CP_RB_CNTL_VALUE \
    (17UL | (9UL << 8) | (1UL << 18) | RADEON_RB_NO_UPDATE)
#define CP_CSQ_CACHE_PARTITION 0x00004d4dUL
#define CP_TEST_BEFORE         0xcafedeadUL
#define CP_TEST_AFTER          0xdeadbeefUL
#define CP_CACHE_FLUSH_ALL     0x0000000fUL

struct RadeonCpState {
    APTR RingMemory;
    ULONG RingGpuAddress;
    ULONG WritePointer;
    ULONG PendingFence;
    ULONG NextFence;
    ULONG SavedBusCntl;
    UWORD SavedCommand;
    UBYTE Ready;
    UBYTE BusConfigured;
    UBYTE StagingStorage[CP_MAX_COMMAND_DWORDS * sizeof(ULONG) + 7UL];
};

static BOOL CpWaitGuiIdle(struct BoardInfo *bi)
{
    ULONG count;

    for (count = 0; count < CP_TIMEOUT_POLLS; ++count) {
        if ((RadeonRead32(bi, RADEON_RBBM_STATUS) &
             RADEON_RBBM_FIFOCNT_MASK) >= 64UL)
            break;
        RadeonDelayUs(1);
    }
    if (count == CP_TIMEOUT_POLLS)
        return FALSE;

    for (count = 0; count < CP_TIMEOUT_POLLS; ++count) {
        if (!(RadeonRead32(bi, RADEON_RBBM_STATUS) &
              RADEON_RBBM_ACTIVE))
            return TRUE;
        RadeonDelayUs(1);
    }
    return FALSE;
}

static BOOL CpReset(struct BoardInfo *bi)
{
    ULONG reset;
    ULONG count;

    if (!RadeonWrite32(bi, RADEON_CP_CSQ_MODE, 0) ||
        !RadeonWrite32(bi, RADEON_CP_CSQ_CNTL, 0))
        return FALSE;
    (void)RadeonRead32(bi, RADEON_CP_CSQ_CNTL);

    reset = RadeonRead32(bi, RADEON_RBBM_SOFT_RESET);
    if (!RadeonWrite32(bi, RADEON_RBBM_SOFT_RESET,
                       reset | RADEON_SOFT_RESET_CP))
        return FALSE;
    (void)RadeonRead32(bi, RADEON_RBBM_SOFT_RESET);
    RadeonDelayUs(200);
    if (!RadeonWrite32(bi, RADEON_RBBM_SOFT_RESET,
                       reset & ~RADEON_SOFT_RESET_CP))
        return FALSE;
    (void)RadeonRead32(bi, RADEON_RBBM_SOFT_RESET);
    RadeonDelayUs(1000);

    for (count = 0; count < CP_TIMEOUT_POLLS; ++count) {
        if (!(RadeonRead32(bi, RADEON_RBBM_STATUS) &
              RADEON_CP_CMDSTRM_BUSY))
            return TRUE;
        RadeonDelayUs(1);
    }
    return FALSE;
}

static BOOL CpLoadMicrocode(struct BoardInfo *bi)
{
    ULONG index;

    if (!CpWaitGuiIdle(bi) ||
        !RadeonWrite32(bi, RADEON_CP_ME_RAM_ADDR, 0))
        return FALSE;

    for (index = 0; index < RADEON_R200_CP_MICROCODE_COUNT; ++index) {
        if (!RadeonWrite32(bi, RADEON_CP_ME_RAM_DATAH,
                           RadeonR200CpMicrocode[index][1]) ||
            !RadeonWrite32(bi, RADEON_CP_ME_RAM_DATAL,
                           RadeonR200CpMicrocode[index][0]))
            return FALSE;
    }
    return TRUE;
}

static BOOL CpReserve(struct BoardInfo *bi,
                      struct RadeonCpState *state, ULONG dwords)
{
    ULONG count;

    for (count = 0; count < CP_TIMEOUT_POLLS; ++count) {
        ULONG readPointer = RadeonRead32(bi, RADEON_CP_RB_RPTR) &
                            CP_RING_MASK;
        ULONG available = (readPointer - state->WritePointer - 1UL) &
                          CP_RING_MASK;

        if (available >= dwords)
            return TRUE;
        RadeonDelayUs(1);
    }
    return FALSE;
}

static BOOL CpCommit(struct BoardInfo *bi, struct RadeonCpState *state,
                     const ULONG *commands, ULONG commandCount)
{
    ULONG stagingAddress;
    ULONG *staging;
    ULONG paddedCount;
    ULONG firstCount;
    ULONG remainingCount;
    ULONG newWritePointer;
    ULONG lastIndex;
    ULONG expectedLast;
    ULONG index;

    if (!state || !state->Ready || !commands || !commandCount ||
        commandCount > CP_MAX_COMMAND_DWORDS)
        return FALSE;
    if (commandCount > ~0UL - (CP_RING_ALIGNMENT - 1UL))
        return FALSE;
    paddedCount = (commandCount + CP_RING_ALIGNMENT - 1UL) &
                  ~(CP_RING_ALIGNMENT - 1UL);
    if (!paddedCount || paddedCount >= CP_RING_DWORDS ||
        !CpReserve(bi, state, paddedCount))
        return FALSE;

    stagingAddress = ((ULONG)state->StagingStorage + 7UL) & ~7UL;
    staging = (ULONG *)stagingAddress;
    for (index = 0; index < commandCount; ++index)
        staging[index] = SWAPLONG(commands[index]);
    for (; index < paddedCount; ++index)
        staging[index] = SWAPLONG(RADEON_CP_PACKET2);

    firstCount = CP_RING_DWORDS - state->WritePointer;
    if (firstCount > paddedCount)
        firstCount = paddedCount;
    host_to_pcicpy(staging,
                   (UBYTE *)state->RingMemory +
                       state->WritePointer * sizeof(ULONG),
                   firstCount * sizeof(ULONG));
    remainingCount = paddedCount - firstCount;
    if (remainingCount)
        host_to_pcicpy(staging + firstCount, state->RingMemory,
                       remainingCount * sizeof(ULONG));

    lastIndex = (state->WritePointer + paddedCount - 1UL) & CP_RING_MASK;
    expectedLast = paddedCount == commandCount
                       ? commands[commandCount - 1UL]
                       : RADEON_CP_PACKET2;
    if (SWAPLONG(pci_inl((ULONG)state->RingMemory +
                         lastIndex * sizeof(ULONG))) != expectedLast)
        return FALSE;

    newWritePointer = (state->WritePointer + paddedCount) & CP_RING_MASK;
    if (!RadeonWrite32(bi, RADEON_CP_RB_WPTR, newWritePointer))
        return FALSE;
    (void)RadeonRead32(bi, RADEON_CP_RB_WPTR);
    state->WritePointer = newWritePointer;
    return TRUE;
}

static BOOL CpConfigureRing(struct BoardInfo *bi,
                            struct RadeonCpState *state)
{
    ULONG startup[2];

    if (!RadeonWrite32(bi, RADEON_CP_RB_WPTR_DELAY, 0x40UL) ||
        !RadeonWrite32(bi, RADEON_CP_RB_CNTL, CP_RB_CNTL_VALUE) ||
        !RadeonWrite32(bi, RADEON_CP_RB_BASE,
                       state->RingGpuAddress) ||
        !RadeonWrite32(bi, RADEON_CP_RB_RPTR_ADDR, 0) ||
        !RadeonWrite32(bi, RADEON_SCRATCH_ADDR, 0) ||
        !RadeonWrite32(bi, RADEON_SCRATCH_UMSK, 0) ||
        !RadeonWrite32(bi, RADEON_CP_RB_CNTL,
                       CP_RB_CNTL_VALUE | RADEON_RB_RPTR_WR_ENA) ||
        !RadeonWrite32(bi, RADEON_CP_RB_RPTR_WR, 0) ||
        !RadeonWrite32(bi, RADEON_CP_RB_WPTR, 0) ||
        !RadeonWrite32(bi, RADEON_CP_RB_CNTL, CP_RB_CNTL_VALUE))
        return FALSE;
    RadeonDelayUs(10);
    state->WritePointer = 0;

    if (!RadeonWrite32(bi, RADEON_CP_CSQ_MODE, 0x00001050UL) ||
        !RadeonWrite32(bi, RADEON_CP_RB_WPTR_DELAY, 0) ||
        !RadeonWrite32(bi, RADEON_CP_CSQ_MODE,
                       CP_CSQ_CACHE_PARTITION) ||
        !RadeonWrite32(bi, RADEON_CP_CSQ_CNTL,
                       RADEON_CSQ_PRIBM_INDBM))
        return FALSE;
    (void)RadeonRead32(bi, RADEON_CP_CSQ_CNTL);

    state->Ready = TRUE;
    startup[0] = RADEON_CP_PACKET0(RADEON_ISYNC_CNTL, 0);
    startup[1] = RADEON_ISYNC_ANY2D_IDLE3D |
                 RADEON_ISYNC_ANY3D_IDLE2D |
                 RADEON_ISYNC_WAIT_IDLEGUI |
                 RADEON_ISYNC_CPSCRATCH_IDLEGUI;
    return CpCommit(bi, state, startup, 2);
}

static BOOL CpRingTest(struct BoardInfo *bi,
                       struct RadeonCpState *state)
{
    ULONG test[2];
    ULONG count;

    if (!RadeonWrite32(bi, RADEON_SCRATCH_REG0, CP_TEST_BEFORE))
        return FALSE;
    test[0] = RADEON_CP_PACKET0(RADEON_SCRATCH_REG0, 0);
    test[1] = CP_TEST_AFTER;
    if (!CpCommit(bi, state, test, 2))
        return FALSE;

    for (count = 0; count < CP_TIMEOUT_POLLS; ++count) {
        if (RadeonRead32(bi, RADEON_SCRATCH_REG0) == CP_TEST_AFTER)
            return TRUE;
        RadeonDelayUs(1);
    }
    return FALSE;
}

static BOOL CpWaitCommandStreamIdle(struct BoardInfo *bi)
{
    ULONG count;

    for (count = 0; count < CP_TIMEOUT_POLLS; ++count) {
        if (!(RadeonRead32(bi, RADEON_RBBM_STATUS) &
              RADEON_CP_CMDSTRM_BUSY))
            return TRUE;
        RadeonDelayUs(1);
    }
    return FALSE;
}

static BOOL CpInvalidateHostReadBuffer(struct BoardInfo *bi)
{
    ULONG host = RadeonRead32(bi, RADEON_HOST_PATH_CNTL);

    if (!RadeonWrite32(bi, RADEON_HOST_PATH_CNTL,
                       host | RADEON_HDP_READ_BUFFER_INVALIDATE))
        return FALSE;
    (void)RadeonRead32(bi, RADEON_HOST_PATH_CNTL);
    if (!RadeonWrite32(bi, RADEON_HOST_PATH_CNTL, host))
        return FALSE;
    (void)RadeonRead32(bi, RADEON_HOST_PATH_CNTL);
    return TRUE;
}

static void CpDisable(struct BoardInfo *bi,
                      struct RadeonCpState *state, BOOL orderly)
{
    BOOL idle = TRUE;

    if (!state)
        return;
    if (orderly && state->Ready && state->PendingFence &&
        !RadeonCpWait(bi))
        idle = FALSE;
    state->Ready = FALSE;
    state->PendingFence = 0;
    (void)RadeonWrite32(bi, RADEON_CP_CSQ_MODE, 0);
    (void)RadeonWrite32(bi, RADEON_CP_CSQ_CNTL, 0);
    (void)RadeonRead32(bi, RADEON_CP_CSQ_CNTL);
    if (!CpWaitCommandStreamIdle(bi))
        idle = FALSE;
    if (!idle)
        (void)CpReset(bi);
}

static void CpRestoreBus(struct BoardInfo *bi,
                         struct RadeonCpState *state)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    if (!data || !state || !state->BusConfigured)
        return;
    (void)RadeonWrite32(bi, RADEON_BUS_CNTL, state->SavedBusCntl);
    pci_write_config_word(PCI_COMMAND, state->SavedCommand,
                          data->Device);
    state->BusConfigured = FALSE;
}

BOOL RadeonCpInitialize(struct BoardInfo *bi)
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct RadeonCpState *state;
    ULONG memoryBase;
    ULONG ringAddress;
    ULONG ringOffset;

    if (!SysBase || !data || data->CpState || !data->DmaArena ||
        !data->Device || !bi->MemoryBase || !bi->MemoryIOBase)
        return FALSE;

    state = AllocMem(sizeof(*state), MEMF_PUBLIC | MEMF_CLEAR);
    if (!state)
        return FALSE;
    data->CpState = state;
    state->RingMemory = RadeonAllocateDmaMemory(bi, CP_RING_SIZE);
    if (!state->RingMemory)
        goto fail;

    memoryBase = (ULONG)bi->MemoryBase;
    ringAddress = (ULONG)state->RingMemory;
    if (ringAddress < memoryBase)
        goto fail;
    ringOffset = ringAddress - memoryBase;
    if (ringOffset > data->InstalledVram ||
        CP_RING_SIZE > data->InstalledVram - ringOffset ||
        ringOffset > bi->MemorySpaceSize ||
        CP_RING_SIZE > bi->MemorySpaceSize - ringOffset ||
        data->FramebufferGpuBase > ~0UL - ringOffset)
        goto fail;
    state->RingGpuAddress = data->FramebufferGpuBase + ringOffset;
    if (state->RingGpuAddress & 4095UL)
        goto fail;

    state->SavedCommand = pci_read_config_word(PCI_COMMAND,
                                                 data->Device);
    state->SavedBusCntl = RadeonRead32(bi, RADEON_BUS_CNTL);
    state->BusConfigured = TRUE;
    if (!pci_set_master(data->Device) ||
        !(pci_read_config_word(PCI_COMMAND, data->Device) &
          PCI_COMMAND_MASTER) ||
        !RadeonWrite32(bi, RADEON_BUS_CNTL,
                       state->SavedBusCntl & ~RADEON_BUS_MASTER_DIS) ||
        !CpReset(bi) || !CpLoadMicrocode(bi) ||
        !CpConfigureRing(bi, state) || !CpRingTest(bi, state))
        goto fail;

    state->NextFence = 1;
    RLOG("Radeon9200: R200 CP ready ring=%lx gpu=%lx rptr=%lx "
         "wptr=%lx\n",
         (ULONG)state->RingMemory, state->RingGpuAddress,
         RadeonRead32(bi, RADEON_CP_RB_RPTR),
         RadeonRead32(bi, RADEON_CP_RB_WPTR));
    return TRUE;

fail:
    RLOG("Radeon9200: R200 CP unavailable; retaining direct MMIO\n");
    RadeonCpShutdown(bi);
    return FALSE;
}

void RadeonCpAbort(struct BoardInfo *bi)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    if (!data || !data->CpState)
        return;
    CpDisable(bi, data->CpState, FALSE);
    CpRestoreBus(bi, data->CpState);
}

void RadeonCpShutdown(struct BoardInfo *bi)
{
    struct ExecBase *SysBase = bi ? bi->ExecBase : NULL;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct RadeonCpState *state;

    if (!SysBase || !data || !data->CpState)
        return;
    state = data->CpState;
    CpDisable(bi, state, TRUE);
    CpRestoreBus(bi, state);
    if (state->RingMemory)
        (void)RadeonFreeDmaMemory(bi, state->RingMemory,
                                  CP_RING_SIZE);
    data->CpState = NULL;
    FreeMem(state, sizeof(*state));
}

BOOL RadeonCpIsReady(struct BoardInfo *bi)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);

    return data && data->CpState && data->CpState->Ready;
}

BOOL RadeonCpSubmit(struct BoardInfo *bi, const ULONG *commands,
                    ULONG commandCount)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct RadeonCpState *state;
    ULONG batch[CP_MAX_COMMAND_DWORDS];
    ULONG sequence;
    ULONG index;

    if (!data || !data->CpState || !data->CpState->Ready ||
        !commands || !commandCount ||
        commandCount > CP_MAX_COMMAND_DWORDS - CP_FENCE_DWORDS)
        return FALSE;
    state = data->CpState;
    for (index = 0; index < commandCount; ++index)
        batch[index] = commands[index];

    sequence = state->NextFence++;
    if (!sequence)
        sequence = state->NextFence++;
    batch[index++] = RADEON_CP_PACKET0(RADEON_DSTCACHE_CTLSTAT, 0);
    batch[index++] = CP_CACHE_FLUSH_ALL;
    batch[index++] = RADEON_CP_PACKET0(RADEON_WAIT_UNTIL, 0);
    batch[index++] = RADEON_WAIT_2D_IDLECLEAN |
                     RADEON_WAIT_HOST_IDLECLEAN |
                     RADEON_WAIT_DMA_GUI_IDLE;
    batch[index++] = RADEON_CP_PACKET0(RADEON_SCRATCH_REG0, 0);
    batch[index++] = sequence;

    if (!CpCommit(bi, state, batch, index))
        return FALSE;
    state->PendingFence = sequence;
    return TRUE;
}

BOOL RadeonCpWait(struct BoardInfo *bi)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct RadeonCpState *state;
    ULONG count;

    if (!data || !data->CpState || !data->CpState->Ready)
        return FALSE;
    state = data->CpState;
    if (!state->PendingFence)
        return TRUE;

    for (count = 0; count < CP_TIMEOUT_POLLS; ++count) {
        if (RadeonRead32(bi, RADEON_SCRATCH_REG0) ==
            state->PendingFence) {
            if (!CpInvalidateHostReadBuffer(bi))
                return FALSE;
            state->PendingFence = 0;
            return TRUE;
        }
        RadeonDelayUs(1);
    }
    return FALSE;
}
