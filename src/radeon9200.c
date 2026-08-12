#include <hardware/cia.h>
#include <hardware/byteswap.h>
#include <proto/exec.h>
#include <proto/openpci.h>

#include "radeon9200.h"
#include "radeon_debug.h"
#include "radeon_regs.h"

static const char RadeonBoardName[] = "Radeon9200";

struct RadeonOptions {
    ULONG DmaRequested;
    UBYTE VgaOutput;
    UBYTE DmaSpecified;
    UBYTE DmaValid;
    UBYTE CpEnabled;
    UBYTE HwSpriteEnabled;
    UBYTE HwTextEnabled;
    UBYTE TextStageEnabled;
};

static void ClearBoardData(struct RadeonBoardData *data)
{
    UBYTE *byte = (UBYTE *)data;
    ULONG count = sizeof(*data);

    while (count--)
        *byte++ = 0;
}

static BOOL IsSupportedDevice(UWORD device)
{
    return device == RADEON_DEVICE_RV280_5960 ||
           device == RADEON_DEVICE_RV280_5961 ||
           device == RADEON_DEVICE_RV280_5964;
}

static ULONG BarSize(const struct pci_dev *device, UWORD bar)
{
    ULONG mask;
    ULONG size;

    if (!device || bar >= 6)
        return 0;

    mask = device->base_size[bar];
    if (!mask)
        return 0;

    if (mask & PCI_BASE_ADDRESS_SPACE_IO)
        size = ~(mask & PCI_BASE_ADDRESS_IO_MASK) + 1UL;
    else
        size = ~(mask & PCI_BASE_ADDRESS_MEM_MASK) + 1UL;

    if (!size || (size & (size - 1UL)))
        return 0;

    return size;
}

static BOOL IsMemoryBar(const struct pci_dev *device, UWORD bar)
{
    ULONG mask;

    if (!device || bar >= 6)
        return FALSE;

    mask = device->base_size[bar];
    return mask != 0 && !(mask & PCI_BASE_ADDRESS_SPACE_IO) &&
           (mask & PCI_BASE_ADDRESS_MEM_TYPE_MASK) ==
               PCI_BASE_ADDRESS_MEM_TYPE_32;
}

static char UpperAscii(char value)
{
    if (value >= 'a' && value <= 'z')
        return value - ('a' - 'A');
    return value;
}

static BOOL MatchOption(const char *option, const char *key,
                        const char **value)
{
    if (!option || !key || !value)
        return FALSE;

    while (*key) {
        if (!*option || UpperAscii(*option) != UpperAscii(*key))
            return FALSE;
        ++option;
        ++key;
    }
    *value = option;
    return TRUE;
}

static BOOL ParseDmaSize(const char *text, ULONG *result)
{
    ULONG value = 0;
    ULONG multiplier = 1;
    BOOL haveDigit = FALSE;

    if (!text || !result)
        return FALSE;

    while (*text >= '0' && *text <= '9') {
        ULONG digit = (ULONG)(*text - '0');

        haveDigit = TRUE;
        if (value > (~0UL - digit) / 10UL)
            return FALSE;
        value = value * 10UL + digit;
        ++text;
    }
    if (!haveDigit)
        return FALSE;

    if (*text) {
        if ((text[0] == 'K' || text[0] == 'k') && text[1] == '\0')
            multiplier = 1024UL;
        else if ((text[0] == 'M' || text[0] == 'm') &&
                 text[1] == '\0')
            multiplier = 1024UL * 1024UL;
        else
            return FALSE;
    }
    if (value > ~0UL / multiplier)
        return FALSE;
    *result = value * multiplier;
    return TRUE;
}

static void ParseOptions(char **toolTypes, struct RadeonOptions *options)
{
    const char *value;

    options->DmaRequested = 0;
    options->VgaOutput = TRUE;
    options->DmaSpecified = FALSE;
    options->DmaValid = FALSE;
    options->CpEnabled = FALSE;
    options->HwSpriteEnabled = TRUE;
    options->HwTextEnabled = TRUE;
    /*
     * Off by default: the VRAM-staged BlitTemplate expand wedges the 2D
     * engine on the reference machine and is still being diagnosed. The
     * capability itself is proven (see performance.md); only this submission
     * path is suspect, so it stays opt-in until it passes on hardware.
     */
    options->TextStageEnabled = FALSE;

    if (!toolTypes)
        return;

    while (*toolTypes) {
        const char *option = *toolTypes++;

        if (MatchOption(option, "OUTPUT=", &value)) {
            options->VgaOutput = UpperAscii(value[0]) == 'V' &&
                                 UpperAscii(value[1]) == 'G' &&
                                 UpperAscii(value[2]) == 'A' &&
                                 value[3] == '\0';
        } else if (MatchOption(option, "DMASIZE=", &value)) {
            options->DmaSpecified = TRUE;
            options->DmaValid = ParseDmaSize(value,
                                              &options->DmaRequested);
            if (!options->DmaValid)
                options->DmaRequested = 0;
        } else if (MatchOption(option, "CP=", &value)) {
            options->CpEnabled = UpperAscii(value[0]) == 'Y' &&
                                 UpperAscii(value[1]) == 'E' &&
                                 UpperAscii(value[2]) == 'S' &&
                                 value[3] == '\0';
        } else if (MatchOption(option, "HWSPRITE=", &value)) {
            options->HwSpriteEnabled = UpperAscii(value[0]) == 'Y' &&
                                       UpperAscii(value[1]) == 'E' &&
                                       UpperAscii(value[2]) == 'S' &&
                                       value[3] == '\0';
        } else if (MatchOption(option, "TEXTSTAGE=", &value)) {
            options->TextStageEnabled = !(UpperAscii(value[0]) == 'N' &&
                                          UpperAscii(value[1]) == 'O' &&
                                          value[2] == '\0');
        } else if (MatchOption(option, "HWTEXT=", &value)) {
            options->HwTextEnabled = UpperAscii(value[0]) == 'Y' &&
                                     UpperAscii(value[1]) == 'E' &&
                                     UpperAscii(value[2]) == 'S' &&
                                     value[3] == '\0';
        }
    }
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

void RadeonReleaseBoard(struct RadeonCardBase *base)
{
    struct BoardInfo *bi;
    struct RadeonBoardData *data;

    if (!base || !base->BoardInfo || !base->OpenPciBase)
        return;

    OpenPciBase = base->OpenPciBase;
    bi = base->BoardInfo;
    data = RadeonGetBoardData(bi);
    RDEBUG_CLOSE(bi);

    if (data) {
        RadeonShutdownCursor(bi);
        RadeonShutdownAcceleration(bi);
        RadeonDestroyDmaMemory(bi);
    }

    if (data && data->StartupMode) {
        struct ExecBase *SysBase = bi->ExecBase;

        if (bi->ModeInfo == data->StartupMode)
            bi->ModeInfo = NULL;
        FreeMem(data->StartupMode, sizeof(*data->StartupMode));
        data->StartupMode = NULL;
    }

    if (data && data->Device) {
        if (data->Initialized)
            RadeonMask32(bi, RADEON_CRTC_EXT_CNTL, 0,
                         RADEON_CRTC_DISPLAY_DIS |
                             RADEON_CRTC_HSYNC_DIS |
                             RADEON_CRTC_VSYNC_DIS);
        if (data->CommandSaved)
            pci_write_config_word(PCI_COMMAND, data->SavedCommand,
                                  data->Device);
        if (data->Obtained)
            pci_release_card(data->Device);
    }

    bi->RegisterBase = NULL;
    bi->MemoryBase = NULL;
    bi->MemoryIOBase = NULL;
    bi->MemorySize = 0;
    bi->MemorySpaceBase = NULL;
    bi->MemorySpaceSize = 0;
    bi->RGBFormats = 0;
    if (bi->BoardName == RadeonBoardName)
        bi->BoardName = NULL;
    bi->BoardType = BT_NoBoard;
    bi->GraphicsControllerType = GCT_Unknown;
    bi->PaletteChipType = PCT_Unknown;

    if (data)
        ClearBoardData(data);
    base->BoardInfo = NULL;
}

BOOL FindCard(__REGA0(struct BoardInfo *bi),
              __REGA6(struct RadeonCardBase *base))
{
    struct pci_dev *device = NULL;

    if (!bi || !base || !base->OpenPciBase || base->BoardInfo)
        return FALSE;

    OpenPciBase = base->OpenPciBase;

    while ((device = pci_find_device(0xffffU, 0xffffU, device)) != NULL) {
        struct RadeonBoardData *data;
        ULONG framebufferSize;
        ULONG mmioSize;

        if (device->vendor != RADEON_VENDOR_ATI)
            continue;
        if (!IsSupportedDevice(device->device))
            continue;
        if ((device->devclass >> 16) != PCI_BASE_CLASS_DISPLAY)
            continue;
        if (!pci_obtain_card(device))
            continue;

        framebufferSize = BarSize(device, RADEON_BAR_FRAMEBUFFER);
        mmioSize = BarSize(device, RADEON_BAR_MMIO);
        if (!IsMemoryBar(device, RADEON_BAR_FRAMEBUFFER) ||
            !IsMemoryBar(device, RADEON_BAR_MMIO) ||
            !device->base_address[RADEON_BAR_FRAMEBUFFER] ||
            !device->base_address[RADEON_BAR_MMIO] ||
            framebufferSize < RADEON_FRAMEBUFFER_MIN_SIZE ||
            mmioSize < RADEON_MMIO_MIN_SIZE) {
            pci_release_card(device);
            continue;
        }

        data = RadeonGetBoardData(bi);
        ClearBoardData(data);
        data->Device = device;
        data->DeviceId = device->device;
        data->MmioSize = mmioSize;
        data->Obtained = TRUE;

        bi->MemoryBase =
            (UBYTE *)device->base_address[RADEON_BAR_FRAMEBUFFER];
        bi->RegisterBase = NULL;
        bi->MemoryIOBase =
            (UBYTE *)device->base_address[RADEON_BAR_MMIO];
        bi->MemorySize = framebufferSize;
        bi->MemorySpaceBase = bi->MemoryBase;
        bi->MemorySpaceSize = framebufferSize;
        bi->BoardName = (char *)RadeonBoardName;
        bi->BoardType = BT_Radeon;
        bi->GraphicsControllerType = GCT_Radeon;
        bi->PaletteChipType = PCT_Radeon;

        base->BoardInfo = bi;
        return TRUE;
    }

    return FALSE;
}

BOOL InitCard(__REGA0(struct BoardInfo *bi),
              __REGA1(char **toolTypes),
              __REGA6(struct RadeonCardBase *base))
{
    struct RadeonBoardData *data;
    struct RadeonOptions options;
    UWORD command;

    if (!bi || !base || base->BoardInfo != bi ||
        !base->OpenPciBase)
        return FALSE;

    data = RadeonGetBoardData(bi);
    if (!data || !data->Device || !data->Obtained)
        return FALSE;

    ParseOptions(toolTypes, &options);
    if (!options.VgaOutput) {
        RadeonReleaseBoard(base);
        return FALSE;
    }

    OpenPciBase = base->OpenPciBase;
    data->SavedCommand = pci_read_config_word(PCI_COMMAND, data->Device);
    data->CommandSaved = TRUE;
    command = data->SavedCommand | PCI_COMMAND_MEMORY;
    pci_write_config_word(PCI_COMMAND, command, data->Device);
    if ((pci_read_config_word(PCI_COMMAND, data->Device) &
         PCI_COMMAND_MEMORY) == 0) {
        RadeonReleaseBoard(base);
        return FALSE;
    }

    RLOG("Radeon9200: initializing device %lx\n", (ULONG)data->DeviceId);
    if (!RadeonInitializeHardware(bi)) {
        RadeonReleaseBoard(base);
        return FALSE;
    }

    data->Initialized = TRUE;
    if (!RadeonShowStartupScreen(bi)) {
        RLOG("Radeon9200: startup VGA mode failed\n");
        RadeonReleaseBoard(base);
        return FALSE;
    }
    if (options.DmaSpecified && !options.DmaValid) {
        RLOG("Radeon9200: ignoring invalid DMASIZE ToolType\n");
    } else if (options.DmaRequested &&
               !RadeonReserveDmaMemory(bi, options.DmaRequested)) {
        RLOG("Radeon9200: DMASIZE request could not be reserved\n");
    }

    (void)RadeonInitializeAcceleration(bi, options.CpEnabled,
                                       options.TextStageEnabled);
    RDEBUG_OPEN(bi, options.CpEnabled, options.DmaRequested,
                options.HwSpriteEnabled);
    RadeonInstallCallbacks(bi, options.HwSpriteEnabled,
                           options.HwTextEnabled);

    RLOG("Radeon9200: VGA CRTC0 startup screen active\n");
    return TRUE;
}
