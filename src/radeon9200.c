#include <hardware/cia.h>
#include <hardware/byteswap.h>
#include <proto/exec.h>

#include "radeon9200.h"
#include "radeon_debug.h"
#include "radeon_regs.h"
#include "prometheus_api.h"
#include "prometheus_radeon.h"

static void ClearBoardData(struct RadeonBoardData *data)
{
    UBYTE *byte = (UBYTE *)data;
    ULONG count = sizeof(*data);

    while (count--)
        *byte++ = 0;
}

ULONG RadeonRead32(struct BoardInfo *bi, ULONG reg)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    UBYTE *mmio = bi ? (UBYTE *)bi->MemoryIOBase : NULL;

    if (!data || !mmio || data->MmioSize < sizeof(ULONG) ||
        (reg & 3UL) || reg > data->MmioSize - sizeof(ULONG))
        return 0;

    RDEBUG_COUNT_READ();
    return SWAPLONG(*(volatile ULONG *)(mmio + reg));
}

BOOL RadeonWrite32(struct BoardInfo *bi, ULONG reg, ULONG value)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    UBYTE *mmio = bi ? (UBYTE *)bi->MemoryIOBase : NULL;

    if (!data || !mmio || data->MmioSize < sizeof(ULONG) ||
        (reg & 3UL) || reg > data->MmioSize - sizeof(ULONG))
        return FALSE;

    RDEBUG_COUNT_WRITE();
    *(volatile ULONG *)(mmio + reg) = SWAPLONG(value);
    return TRUE;
}

BOOL RadeonMask32(struct BoardInfo *bi, ULONG reg, ULONG clear, ULONG set)
{
    ULONG value;
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    UBYTE *mmio = bi ? (UBYTE *)bi->MemoryIOBase : NULL;

    if (!data || !mmio || data->MmioSize < sizeof(ULONG) ||
        (reg & 3UL) || reg > data->MmioSize - sizeof(ULONG))
        return FALSE;

    value = RadeonRead32(bi, reg);
    return RadeonWrite32(bi, reg, (value & ~clear) | set);
}

ULONG RadeonReadIndexed(struct BoardInfo *bi, ULONG index)
{
    if (!RadeonWrite32(bi, RADEON_MM_INDEX, index))
        return 0;
    return RadeonRead32(bi, RADEON_MM_DATA);
}

BOOL RadeonWriteIndexed(struct BoardInfo *bi, ULONG index, ULONG value)
{
    return RadeonWrite32(bi, RADEON_MM_INDEX, index) &&
           RadeonWrite32(bi, RADEON_MM_DATA, value);
}

BOOL RadeonMaskIndexed(struct BoardInfo *bi, ULONG index, ULONG andMask,
                       ULONG orMask)
{
    ULONG value = RadeonReadIndexed(bi, index);
    return RadeonWriteIndexed(bi, index, (value & andMask) | orMask);
}

ULONG RadeonReadPll(struct BoardInfo *bi, UBYTE index)
{
    if (!RadeonWrite32(bi, RADEON_CLOCK_CNTL_INDEX,
                        (ULONG)index & RADEON_PLL_INDEX_MASK))
        return 0;
    return RadeonRead32(bi, RADEON_CLOCK_CNTL_DATA);
}

BOOL RadeonWritePll(struct BoardInfo *bi, UBYTE index, ULONG value)
{
    if (!RadeonWrite32(bi, RADEON_CLOCK_CNTL_INDEX,
                        ((ULONG)index & RADEON_PLL_INDEX_MASK) |
                            RADEON_PLL_WR_EN) ||
        !RadeonWrite32(bi, RADEON_CLOCK_CNTL_DATA, value))
        return FALSE;
    return RadeonWrite32(bi, RADEON_CLOCK_CNTL_INDEX, 0);
}

BOOL RadeonMaskPll(struct BoardInfo *bi, UBYTE index, ULONG andMask,
                   ULONG orMask)
{
    ULONG value = RadeonReadPll(bi, index);
    return RadeonWritePll(bi, index, (value & andMask) | orMask);
}

void RadeonDelayUs(ULONG microseconds)
{
    volatile struct CIA *cia = (volatile struct CIA *)0x00bfe001UL;

    while (microseconds) {
        ULONG chunk = microseconds > 100000UL ? 100000UL : microseconds;
        ULONG count = ((chunk << 4) + 15UL) / 22UL;
        while (count--) {
            volatile UBYTE value = cia->ciapra;
            (void)value;
        }
        microseconds -= chunk;
    }
}

static void RadeonFinishBoardRelease(struct RadeonChipBase *base,
                                     struct BoardInfo *bi)
{
    struct ExecBase *SysBase = base->ExecBase;
    struct RadeonBoardData *data;

    PrometheusBase = base->PrometheusBase;
    data = RadeonGetBoardData(bi);
    RDEBUG_CLOSE(bi);

    if (data) {
        RadeonShutdownCursor(bi);
        RadeonShutdownAcceleration(bi);
        if (data->Initialized)
            RadeonMask32(bi, RADEON_CRTC_EXT_CNTL, 0,
                         RADEON_CRTC_DISPLAY_DIS |
                             RADEON_CRTC_HSYNC_DIS |
                             RADEON_CRTC_VSYNC_DIS);
    }

    if (data && data->StartupMode) {
        if (bi->ModeInfo == data->StartupMode)
            bi->ModeInfo = NULL;
        FreeMem(data->StartupMode, sizeof(*data->StartupMode));
        data->StartupMode = NULL;
    }

    if (data)
        ClearBoardData(data);
    ObtainSemaphore(&base->ServiceLock);
    base->ServiceState = RADEON3D_SERVICE_EMPTY;
    ReleaseSemaphore(&base->ServiceLock);
}

BOOL RadeonReleaseBoard(struct RadeonChipBase *base,
                        struct BoardInfo *expectedBoard,
                        BOOL allowInitializing)
{
    struct ExecBase *SysBase;
    struct BoardInfo *bi;

    if (!base)
        return FALSE;

    SysBase = base->ExecBase;
    bi = expectedBoard ? expectedBoard : base->BoardInfo;
    if (!bi)
        return FALSE;
    ObtainSemaphore(&bi->BoardLock);
    ObtainSemaphore(&base->ServiceLock);
    if (base->BoardInfo != bi ||
        base->ServiceState == RADEON3D_SERVICE_DETACHING ||
        (base->ServiceState == RADEON3D_SERVICE_INITIALIZING &&
         !allowInitializing)) {
        ReleaseSemaphore(&base->ServiceLock);
        ReleaseSemaphore(&bi->BoardLock);
        return FALSE;
    }
    base->ServiceState = RADEON3D_SERVICE_DETACHING;
    base->BoardInfo = NULL;
    Radeon3DAdvanceGeneration(base);
    ReleaseSemaphore(&base->ServiceLock);

    RadeonFinishBoardRelease(base, bi);
    ReleaseSemaphore(&bi->BoardLock);
    return TRUE;
}

BOOL InitChip(__REGA0(struct BoardInfo *bi),
              __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;
    struct PrometheusRadeonHandoff handoff;
    struct RadeonBoardData *data;
    UBYTE *source;
    UBYTE *destination;
    ULONG count;

    if (!bi || !base || !SysBase || !base->PrometheusBase)
        return FALSE;
    source = (UBYTE *)bi->CardData;
    destination = (UBYTE *)&handoff;
    for (count = 0; count < sizeof(handoff); ++count)
        *destination++ = *source++;
    if (handoff.Magic != PROM_RADEON_HANDOFF_MAGIC || !handoff.Board ||
        !bi->MemoryBase || !bi->MemoryIOBase ||
        handoff.FramebufferSize < RADEON_FRAMEBUFFER_MIN_SIZE ||
        handoff.MmioSize < RADEON_MMIO_MIN_SIZE)
        return FALSE;

    ObtainSemaphore(&base->ServiceLock);
    if (base->ServiceState != RADEON3D_SERVICE_EMPTY || base->BoardInfo) {
        ReleaseSemaphore(&base->ServiceLock);
        return FALSE;
    }
    base->ServiceState = RADEON3D_SERVICE_INITIALIZING;
    base->BoardInfo = bi;
    ReleaseSemaphore(&base->ServiceLock);

    data = RadeonGetBoardData(bi);
    ClearBoardData(data);
    data->Device = handoff.Board;
    data->DeviceId = handoff.DeviceId;
    data->MmioSize = handoff.MmioSize;
    PrometheusBase = base->PrometheusBase;
    bi->GraphicsControllerType = GCT_Radeon;
    bi->PaletteChipType = PCT_Radeon;
    bi->MemorySize = handoff.FramebufferSize;
    bi->MemorySpaceBase = bi->MemoryBase;
    bi->MemorySpaceSize = handoff.FramebufferSize;

    RLOG("Radeon9200: initializing device %lx\n", (ULONG)data->DeviceId);
    if (!RadeonInitializeHardware(bi)) {
        (void)RadeonReleaseBoard(base, bi, TRUE);
        return FALSE;
    }

    data->Initialized = TRUE;
    if (!RadeonShowStartupScreen(bi)) {
        RLOG("Radeon9200: startup VGA mode failed\n");
        (void)RadeonReleaseBoard(base, bi, TRUE);
        return FALSE;
    }
    ObtainSemaphore(&base->ServiceLock);
    if (base->BoardInfo != bi ||
        base->ServiceState != RADEON3D_SERVICE_INITIALIZING) {
        ReleaseSemaphore(&base->ServiceLock);
        (void)RadeonReleaseBoard(base, bi, TRUE);
        return FALSE;
    }
    base->ServiceState = RADEON3D_SERVICE_ATTACHED;
    ReleaseSemaphore(&base->ServiceLock);
    RLOG("Radeon9200: VGA CRTC0 startup screen active\n");
    return TRUE;
}

BOOL InitRadeonFeatures(__REGA0(struct BoardInfo *bi),
                        __REGD0(ULONG features),
                        __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base ? base->ExecBase : NULL;

    if (!bi || !base || !SysBase)
        return FALSE;
    ObtainSemaphore(&base->ServiceLock);
    if (base->BoardInfo != bi ||
        base->ServiceState != RADEON3D_SERVICE_ATTACHED) {
        ReleaseSemaphore(&base->ServiceLock);
        return FALSE;
    }
    PrometheusBase = base->PrometheusBase;
    (void)RadeonInitializeAcceleration(
        bi, (features & PROM_RADEON_FEATURE_CP) != 0,
        (features & PROM_RADEON_FEATURE_TEXTSTAGE) != 0);
    RDEBUG_OPEN(bi, (features & PROM_RADEON_FEATURE_CP) != 0,
                bi->MemorySpaceSize - bi->MemorySize,
                (features & PROM_RADEON_FEATURE_HWSPRITE) != 0);
    RadeonInstallCallbacks(
        bi, (features & PROM_RADEON_FEATURE_HWSPRITE) != 0,
        (features & PROM_RADEON_FEATURE_HWTEXT) != 0);
    RDEBUG_FALLBACK_PROBE(bi);
    if (RadeonCpIsReady(bi)) {
        Radeon3DAdvanceGeneration(base);
        base->ServiceState = RADEON3D_SERVICE_READY;
    }
    ReleaseSemaphore(&base->ServiceLock);
    return TRUE;
}
