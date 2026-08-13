/*
 * Legacy Radeon COMBIOS initialization support.
 *
 * The table formats and execution order are derived from NetBSD's
 * sys/dev/pci/radeonfb_bios.c:
 *
 * Copyright (c) 2006 Itronix Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of Itronix Inc. may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ITRONIX INC. "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL ITRONIX INC. BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE.
 */

#include <exec/memory.h>
#include <proto/exec.h>

#include "radeon9200.h"
#include "radeon_regs.h"
#include "prometheus_api.h"
#include "prometheus_radeon.h"

#define BIOS_MAX_SIZE       0x00100000UL
#define BIOS_MIN_TABLE      0x00000060UL
#define BIOS_HEADER_POINTER 0x00000048UL

#define COMBIOS_ASIC_INIT_1 0x0cU
#define COMBIOS_PLL_INFO    0x30U
#define COMBIOS_PLL_INIT    0x46U
#define COMBIOS_MEM_CONFIG  0x48U
#define COMBIOS_ASIC_INIT_2 0x4eU
#define COMBIOS_DYN_CLOCK   0x52U
#define COMBIOS_MISC_INFO   0x5eU

#define TABLE_FLAG_MASK          0xe000U
#define TABLE_INDEX_MASK         0x1fffU
#define TABLE_COMMAND_MASK       0x00ffU
#define TABLE_WRITE_INDEXED      0x0000U
#define TABLE_WRITE_DIRECT       0x2000U
#define TABLE_MASK_INDEXED       0x4000U
#define TABLE_MASK_DIRECT        0x6000U
#define TABLE_DELAY              0x8000U
#define TABLE_SPECIAL            0xa000U
#define TABLE_WAIT_MC_BUSY       0x03U
#define TABLE_WAIT_MEMORY_READY  0x08U

#define PLL_FLAG_MASK       0xc0U
#define PLL_WRITE           0x00U
#define PLL_MASK_BYTE       0x40U
#define PLL_WAIT            0x80U
#define PLL_WAIT_150US      1U
#define PLL_WAIT_5MS        2U
#define PLL_WAIT_MC_BUSY    3U
#define PLL_WAIT_DLL_READY  4U
#define PLL_WAIT_POWER_BIT  5U

struct RadeonBios {
    UBYTE *Allocation;
    ULONG AllocationSize;
    UBYTE *Image;
    ULONG ImageSize;
    UWORD Header;
    UBYTE Revision;
    UBYTE DirectorySize;
};

static BOOL BiosHas(const struct RadeonBios *bios, ULONG offset, ULONG size)
{
    return bios && offset <= bios->ImageSize &&
           size <= bios->ImageSize - offset;
}

static UBYTE Bios8(const struct RadeonBios *bios, ULONG offset)
{
    return BiosHas(bios, offset, 1) ? bios->Image[offset] : 0;
}

static UWORD Bios16(const struct RadeonBios *bios, ULONG offset)
{
    if (!BiosHas(bios, offset, 2))
        return 0;
    return (UWORD)bios->Image[offset] |
           ((UWORD)bios->Image[offset + 1] << 8);
}

static ULONG Bios32(const struct RadeonBios *bios, ULONG offset)
{
    if (!BiosHas(bios, offset, 4))
        return 0;
    return (ULONG)bios->Image[offset] |
           ((ULONG)bios->Image[offset + 1] << 8) |
           ((ULONG)bios->Image[offset + 2] << 16) |
           ((ULONG)bios->Image[offset + 3] << 24);
}

static BOOL RomSignatureValid(const UBYTE *rom, ULONG size)
{
    return rom && size >= 2 && rom[0] == 0x55 && rom[1] == 0xaa;
}

static struct PrometheusRadeonHandoff *RadeonHandoff(struct BoardInfo *bi)
{
    struct PrometheusRadeonHandoff *handoff;

    if (!bi)
        return NULL;
    handoff = (struct PrometheusRadeonHandoff *)bi->CardData;
    return handoff->Magic == PROM_RADEON_HANDOFF_MAGIC ? handoff : NULL;
}

static ULONG RomSize(struct BoardInfo *bi)
{
    struct PrometheusRadeonHandoff *handoff = RadeonHandoff(bi);

    if (!handoff || handoff->RomSize < 512 ||
        handoff->RomSize > BIOS_MAX_SIZE ||
        (handoff->RomSize & (handoff->RomSize - 1UL)))
        return 0;
    return handoff->RomSize;
}

static BOOL CopyDisabledRom(struct BoardInfo *bi, UBYTE *destination,
                            ULONG size)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct PrometheusRadeonHandoff *handoff = RadeonHandoff(bi);
    ULONG seprom;
    ULONG viph;
    ULONG bus;
    ULONG crtc;
    ULONG crtc2;
    ULONG ext;

    if (!data || !handoff || !destination)
        return FALSE;

    seprom = RadeonRead32(bi, RADEON_SEPROM_CNTL1);
    viph = RadeonRead32(bi, RADEON_VIPH_CONTROL);
    bus = RadeonRead32(bi, RADEON_BUS_CNTL);
    crtc = RadeonRead32(bi, RADEON_CRTC_GEN_CNTL);
    crtc2 = RadeonRead32(bi, RADEON_CRTC2_GEN_CNTL);
    ext = RadeonRead32(bi, RADEON_CRTC_EXT_CNTL);

    RadeonWrite32(bi, RADEON_SEPROM_CNTL1,
                  (seprom & ~RADEON_SCK_PRESCALE_MASK) |
                      (0x0cUL << RADEON_SCK_PRESCALE_SHIFT));
    RadeonWrite32(bi, RADEON_VIPH_CONTROL, viph & ~RADEON_VIPH_EN);
    RadeonWrite32(bi, RADEON_BUS_CNTL, bus & ~RADEON_BUS_BIOS_DIS_ROM);
    RadeonWrite32(bi, RADEON_CRTC_GEN_CNTL,
                  (crtc & ~RADEON_CRTC_EN) |
                      RADEON_CRTC_DISP_REQ_EN_B |
                      RADEON_CRTC_EXT_DISP_EN);
    RadeonWrite32(bi, RADEON_CRTC2_GEN_CNTL,
                  (crtc2 & ~RADEON_CRTC2_EN) |
                      RADEON_CRTC2_DISP_REQ_EN_B);
    RadeonWrite32(bi, RADEON_CRTC_EXT_CNTL,
                  (ext & ~RADEON_CRTC_CRT_ON) |
                      RADEON_CRTC_SYNC_TRISTAT |
                      RADEON_CRTC_DISPLAY_DIS);

    {
        volatile UBYTE *source = (volatile UBYTE *)handoff->RomBase;
        ULONG index;

        for (index = 0; index < size; ++index)
            destination[index] = source[index];
    }

    RadeonWrite32(bi, RADEON_CRTC_EXT_CNTL, ext);
    RadeonWrite32(bi, RADEON_CRTC2_GEN_CNTL, crtc2);
    RadeonWrite32(bi, RADEON_CRTC_GEN_CNTL, crtc);
    RadeonWrite32(bi, RADEON_BUS_CNTL, bus);
    RadeonWrite32(bi, RADEON_VIPH_CONTROL, viph);
    RadeonWrite32(bi, RADEON_SEPROM_CNTL1, seprom);
    return RomSignatureValid(destination, size);
}

static BOOL LoadRom(struct BoardInfo *bi, struct RadeonBios *bios)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct PrometheusRadeonHandoff *handoff = RadeonHandoff(bi);
    struct ExecBase *SysBase = bi->ExecBase;
    ULONG size;
    ULONG savedRom;
    ULONG enabledRom;
    APTR physicalRom;

    if (!data || !handoff || !data->Device || !bios || !handoff->RomBase)
        return FALSE;

    size = RomSize(bi);
    if (!size)
        return FALSE;

    bios->Allocation = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!bios->Allocation)
        return FALSE;
    bios->AllocationSize = size;

    savedRom = PromReadConfigLong(data->Device, PROM_PCI_ROM_ADDRESS);
    enabledRom = savedRom | PROM_PCI_ROM_ENABLE;
    if (!(enabledRom & PROM_PCI_ROM_MASK)) {
        physicalRom = PromPhysicalAddress(handoff->RomBase);
        if (!physicalRom) {
            FreeMem(bios->Allocation, bios->AllocationSize);
            bios->Allocation = NULL;
            bios->AllocationSize = 0;
            return FALSE;
        }
        enabledRom = ((ULONG)physicalRom & PROM_PCI_ROM_MASK) |
                     PROM_PCI_ROM_ENABLE;
    }
    PromWriteConfigLong(data->Device, enabledRom, PROM_PCI_ROM_ADDRESS);
    {
        volatile UBYTE *source = (volatile UBYTE *)handoff->RomBase;
        ULONG index;

        for (index = 0; index < size; ++index)
            bios->Allocation[index] = source[index];
    }

    if (!RomSignatureValid(bios->Allocation, size))
        CopyDisabledRom(bi, bios->Allocation, size);

    PromWriteConfigLong(data->Device, savedRom, PROM_PCI_ROM_ADDRESS);
    return RomSignatureValid(bios->Allocation, size);
}

static BOOL SelectX86Image(struct RadeonBios *bios, UWORD deviceId)
{
    ULONG offset = 0;

    if (!bios || !bios->Allocation)
        return FALSE;

    while (offset <= bios->AllocationSize &&
           0x1cUL <= bios->AllocationSize - offset) {
        UBYTE *image = bios->Allocation + offset;
        ULONG remaining = bios->AllocationSize - offset;
        UWORD pcir;
        UWORD vendor;
        UWORD device;
        UWORD blocks;
        ULONG imageSize;
        ULONG checksum = 0;
        ULONG index;
        UBYTE codeType;
        UBYTE indicator;

        if (!RomSignatureValid(image, remaining))
            return FALSE;

        pcir = (UWORD)image[0x18] | ((UWORD)image[0x19] << 8);
        if (pcir > remaining || remaining - pcir < 0x18)
            return FALSE;
        if (image[pcir] != 'P' || image[pcir + 1] != 'C' ||
            image[pcir + 2] != 'I' || image[pcir + 3] != 'R')
            return FALSE;

        vendor = (UWORD)image[pcir + 4] | ((UWORD)image[pcir + 5] << 8);
        device = (UWORD)image[pcir + 6] | ((UWORD)image[pcir + 7] << 8);
        blocks = (UWORD)image[pcir + 0x10] |
                 ((UWORD)image[pcir + 0x11] << 8);
        codeType = image[pcir + 0x14];
        indicator = image[pcir + 0x15];
        imageSize = (ULONG)blocks * 512UL;
        if (!blocks || imageSize > remaining)
            return FALSE;

        for (index = 0; index < imageSize; ++index)
            checksum += image[index];

        if (vendor == RADEON_VENDOR_ATI && device == deviceId &&
            codeType == 0 && (checksum & 0xffUL) == 0) {
            bios->Image = image;
            bios->ImageSize = imageSize;
            return TRUE;
        }

        if (indicator & 0x80)
            break;
        offset += imageSize;
    }

    return FALSE;
}

static BOOL ParseHeader(struct RadeonBios *bios)
{
    UWORD header;

    if (!BiosHas(bios, BIOS_HEADER_POINTER, 2))
        return FALSE;
    header = Bios16(bios, BIOS_HEADER_POINTER);
    if (header < BIOS_MIN_TABLE || !BiosHas(bios, header, 8))
        return FALSE;
    if ((bios->Image[header + 4] == 'A' &&
         bios->Image[header + 5] == 'T' &&
         bios->Image[header + 6] == 'O' &&
         bios->Image[header + 7] == 'M') ||
        (bios->Image[header + 4] == 'M' &&
         bios->Image[header + 5] == 'O' &&
         bios->Image[header + 6] == 'T' &&
         bios->Image[header + 7] == 'A'))
        return FALSE;

    bios->Header = header;
    bios->Revision = Bios8(bios, header);
    bios->DirectorySize = Bios8(bios, header + 6);
    return bios->Revision <= 0x10 && bios->DirectorySize >= 0x32 &&
           BiosHas(bios, header, bios->DirectorySize);
}

static UWORD TableOffset(const struct RadeonBios *bios, UWORD field)
{
    UWORD offset;

    if (!bios || field + 1U >= bios->DirectorySize)
        return 0;
    offset = Bios16(bios, (ULONG)bios->Header + field);
    if (offset < BIOS_MIN_TABLE || !BiosHas(bios, offset, 1))
        return 0;
    return offset;
}

static UWORD MiscTableOffset(const struct RadeonBios *bios, UWORD field)
{
    UWORD misc = TableOffset(bios, COMBIOS_MISC_INFO);
    UWORD offset;

    if (!misc || Bios8(bios, misc) == 0 || !BiosHas(bios, misc + field, 2))
        return 0;
    offset = Bios16(bios, misc + field);
    if (offset < BIOS_MIN_TABLE || !BiosHas(bios, offset, 1))
        return 0;
    return offset;
}

static BOOL WaitPll(struct BoardInfo *bi, ULONG mask, BOOL set, UWORD count)
{
    while (count--) {
        ULONG value = RadeonReadPll(bi, RADEON_CLK_PWRMGT_CNTL);
        if (((value & mask) != 0) == set)
            return TRUE;
    }
    return FALSE;
}

static BOOL WaitMemoryReady(struct BoardInfo *bi, UWORD count)
{
    while (count--) {
        if ((RadeonReadIndexed(bi, RADEON_MEM_STR_CNTL) &
             RADEON_MEM_PWRUP_COMPLETE) == RADEON_MEM_PWRUP_COMPLETE)
            return TRUE;
    }
    return FALSE;
}

static BOOL LoadInitTable(struct BoardInfo *bi,
                          const struct RadeonBios *bios, UWORD table)
{
    ULONG offset = table;
    ULONG entries = 0;

    if (!table)
        return TRUE;

    while (BiosHas(bios, offset, 2) && entries++ < 4096) {
        UWORD value = Bios16(bios, offset);
        UWORD flag;
        UWORD index;
        ULONG andMask;
        ULONG orMask;
        UWORD count;

        offset += 2;
        if (!value)
            return TRUE;
        flag = value & TABLE_FLAG_MASK;
        index = value & TABLE_INDEX_MASK;

        switch (flag) {
        case TABLE_WRITE_INDEXED:
            if (!BiosHas(bios, offset, 4) ||
                !RadeonWriteIndexed(bi, index, Bios32(bios, offset)))
                return FALSE;
            offset += 4;
            break;
        case TABLE_WRITE_DIRECT:
            if (!BiosHas(bios, offset, 4) ||
                !RadeonWrite32(bi, index, Bios32(bios, offset)))
                return FALSE;
            offset += 4;
            break;
        case TABLE_MASK_INDEXED:
            if (!BiosHas(bios, offset, 8))
                return FALSE;
            andMask = Bios32(bios, offset);
            orMask = Bios32(bios, offset + 4);
            if (!RadeonMaskIndexed(bi, index, andMask, orMask))
                return FALSE;
            offset += 8;
            break;
        case TABLE_MASK_DIRECT:
            if (!BiosHas(bios, offset, 8))
                return FALSE;
            andMask = Bios32(bios, offset);
            orMask = Bios32(bios, offset + 4);
            if (!RadeonMask32(bi, index, ~andMask, orMask))
                return FALSE;
            offset += 8;
            break;
        case TABLE_DELAY:
            if (!BiosHas(bios, offset, 2))
                return FALSE;
            RadeonDelayUs(Bios16(bios, offset));
            offset += 2;
            break;
        case TABLE_SPECIAL:
            if (!BiosHas(bios, offset, 2))
                return FALSE;
            count = Bios16(bios, offset);
            offset += 2;
            if ((value & TABLE_COMMAND_MASK) == TABLE_WAIT_MC_BUSY) {
                if (!WaitPll(bi, RADEON_MC_BUSY, FALSE, count))
                    return FALSE;
            } else if ((value & TABLE_COMMAND_MASK) ==
                       TABLE_WAIT_MEMORY_READY) {
                if (!WaitMemoryReady(bi, count))
                    return FALSE;
            } else {
                return FALSE;
            }
            break;
        default:
            return FALSE;
        }
    }

    return FALSE;
}

static BOOL LoadPllTable(struct BoardInfo *bi,
                         const struct RadeonBios *bios, UWORD table)
{
    ULONG offset = table;
    ULONG entries = 0;

    if (!table)
        return TRUE;

    while (BiosHas(bios, offset, 1) && entries++ < 4096) {
        UBYTE value = Bios8(bios, offset++);
        UBYTE index = value & RADEON_PLL_INDEX_MASK;

        if (!value)
            return TRUE;

        switch (value & PLL_FLAG_MASK) {
        case PLL_WRITE:
            if (!BiosHas(bios, offset, 4) ||
                !RadeonWritePll(bi, index, Bios32(bios, offset)))
                return FALSE;
            offset += 4;
            break;
        case PLL_MASK_BYTE:
            if (!BiosHas(bios, offset, 3))
                return FALSE;
            {
                UBYTE lane = Bios8(bios, offset++);
                ULONG shift;
                ULONG byteMask;
                ULONG andMask;
                ULONG orMask;

                if (lane > 3)
                    return FALSE;
                shift = (ULONG)lane * 8UL;
                byteMask = 0xffUL << shift;
                andMask = ~byteMask |
                          ((ULONG)Bios8(bios, offset++) << shift);
                orMask = (ULONG)Bios8(bios, offset++) << shift;
                if (!RadeonMaskPll(bi, index, andMask, orMask))
                    return FALSE;
            }
            break;
        case PLL_WAIT:
            switch (index) {
            case PLL_WAIT_150US:
                RadeonDelayUs(150);
                break;
            case PLL_WAIT_5MS:
                RadeonDelayUs(5000);
                break;
            case PLL_WAIT_MC_BUSY:
                if (!WaitPll(bi, RADEON_MC_BUSY, FALSE, 1000))
                    return FALSE;
                break;
            case PLL_WAIT_DLL_READY:
                if (!WaitPll(bi, RADEON_DLL_READY, TRUE, 1000))
                    return FALSE;
                break;
            case PLL_WAIT_POWER_BIT:
            {
                ULONG power = RadeonReadPll(bi, RADEON_CLK_PWRMGT_CNTL);
                if (power & RADEON_CLK_PWRMGT_CNTL24) {
                    if (!RadeonMaskPll(bi, RADEON_MCLK_CNTL,
                                       0xffff0000UL,
                                       RADEON_SET_ALL_SRCS_TO_PCI))
                        return FALSE;
                    RadeonDelayUs(10000);
                    if (!RadeonWritePll(bi, RADEON_CLK_PWRMGT_CNTL,
                                        power &
                                            ~RADEON_CLK_PWRMGT_CNTL24))
                        return FALSE;
                    RadeonDelayUs(10000);
                }
                break;
            }
            default:
                return FALSE;
            }
            break;
        default:
            return FALSE;
        }
    }

    return FALSE;
}

static UWORD FindRamResetTable(const struct RadeonBios *bios, UWORD memory)
{
    ULONG offset = memory;

    if (!memory)
        return 0;
    while (BiosHas(bios, offset, 1) && Bios8(bios, offset))
        ++offset;
    if (!BiosHas(bios, offset, 3))
        return 0;
    offset += 3;
    return offset <= 0xffffUL ? (UWORD)offset : 0;
}

static BOOL ResetSdram(struct BoardInfo *bi,
                       const struct RadeonBios *bios, UWORD table)
{
    ULONG offset = table;
    ULONG entries = 0;

    if (!table)
        return TRUE;

    while (BiosHas(bios, offset, 1) && entries++ < 4096) {
        UBYTE index = Bios8(bios, offset++);

        if (index == 0xff)
            return TRUE;
        if (index == 0x0f) {
            if (!WaitMemoryReady(bi, 20000))
                return FALSE;
        } else {
            ULONG orMask;
            if (!BiosHas(bios, offset, 2))
                return FALSE;
            orMask = Bios16(bios, offset);
            offset += 2;
            if (!RadeonMaskIndexed(bi, RADEON_MEM_SDRAM_MODE_REG,
                                   RADEON_SDRAM_MODE_MASK, orMask) ||
                !RadeonMaskIndexed(bi, RADEON_MEM_SDRAM_MODE_REG,
                                   RADEON_B3MEM_RESET_MASK,
                                   (ULONG)index << 24))
                return FALSE;
        }
    }

    return FALSE;
}

static void LoadClockInfo(struct BoardInfo *bi,
                           const struct RadeonBios *bios)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    UWORD pll = bios ? TableOffset(bios, COMBIOS_PLL_INFO) : 0;
    ULONG refClock = 27000;
    ULONG minPll = 125000;
    ULONG maxPll = 400000;
    UWORD refDivider = 12;
    ULONG hardwareDivider;

    if (pll && BiosHas(bios, pll, 0x1a)) {
        ULONG value;
        value = (ULONG)Bios16(bios, pll + 0x0e) * 10UL;
        if (value)
            refClock = value;
        value = Bios16(bios, pll + 0x10);
        if (value >= 2 && value <= RADEON_PPLL_REF_DIV_MASK)
            refDivider = (UWORD)value;
        value = Bios32(bios, pll + 0x12) * 10UL;
        if (value)
            minPll = value;
        value = Bios32(bios, pll + 0x16) * 10UL;
        if (value)
            maxPll = value;
        value = (ULONG)Bios16(bios, pll + 0x08) * 10000UL;
        data->MemoryClockHz = value;
    }

    if (!refClock || refDivider < 2 || !minPll || minPll > maxPll) {
        refClock = 27000;
        refDivider = 12;
        minPll = 125000;
        maxPll = 400000;
    }

    hardwareDivider = RadeonReadPll(bi, RADEON_PPLL_REF_DIV) &
                      RADEON_PPLL_REF_DIV_MASK;
    if (hardwareDivider >= 2)
        refDivider = (UWORD)hardwareDivider;

    data->RefClockKHz = refClock;
    data->RefDivider = refDivider;
    data->MinPllKHz = minPll;
    data->MaxPllKHz = maxPll;
    RLOG("Radeon9200: PLL ref=%ldkHz div=%ld range=%ld-%ldkHz\n",
         refClock, (ULONG)refDivider, minPll, maxPll);
}

static ULONG BiosMemorySize(const struct RadeonBios *bios)
{
    UWORD detected = MiscTableOffset(bios, 0x07);
    UWORD memory = TableOffset(bios, COMBIOS_MEM_CONFIG);
    ULONG megabytes = 0;

    if (detected && BiosHas(bios, detected, 7) &&
        Bios8(bios, detected) < 3)
        megabytes = Bios16(bios, detected + 5);

    if (!megabytes && memory && BiosHas(bios, memory - 1, 2)) {
        UBYTE revision = Bios8(bios, memory - 1);
        megabytes = Bios8(bios, memory);
        if (revision >= 1)
            megabytes *= 2;
    }

    if (megabytes < 4 || megabytes > 512)
        return 0;
    return megabytes * 1024UL * 1024UL;
}

static BOOL PostCard(struct BoardInfo *bi, const struct RadeonBios *bios,
                      ULONG biosMemory)
{
    UWORD asic1 = TableOffset(bios, COMBIOS_ASIC_INIT_1);

    if (!asic1 || !LoadInitTable(bi, bios, asic1))
        return FALSE;

    if (bios->Revision < 9) {
        UWORD pll = TableOffset(bios, COMBIOS_PLL_INIT);
        UWORD asic2 = TableOffset(bios, COMBIOS_ASIC_INIT_2);
        UWORD asic3 = MiscTableOffset(bios, 0x03);
        UWORD asic4 = MiscTableOffset(bios, 0x05);
        UWORD memory = TableOffset(bios, COMBIOS_MEM_CONFIG);
        UWORD reset = FindRamResetTable(bios, memory);
        UWORD dynamic = TableOffset(bios, COMBIOS_DYN_CLOCK);

        if (!LoadPllTable(bi, bios, pll) ||
            !LoadInitTable(bi, bios, asic2) ||
            !LoadInitTable(bi, bios, asic4) ||
            !ResetSdram(bi, bios, reset) ||
            !LoadInitTable(bi, bios, asic3))
            return FALSE;
        if (biosMemory &&
            !RadeonWrite32(bi, RADEON_CONFIG_MEMSIZE, biosMemory))
            return FALSE;
        if (!LoadPllTable(bi, bios, dynamic))
            return FALSE;
    }

    return TRUE;
}

static BOOL IsPosted(struct BoardInfo *bi)
{
    ULONG crtc = RadeonRead32(bi, RADEON_CRTC_GEN_CNTL);
    ULONG crtc2 = RadeonRead32(bi, RADEON_CRTC2_GEN_CNTL);
    ULONG memory = RadeonRead32(bi, RADEON_CONFIG_MEMSIZE);

    return ((crtc | crtc2) & RADEON_CRTC_EN) != 0 || memory != 0;
}

static BOOL SetFramebufferLocation(struct BoardInfo *bi, ULONG apertureSize)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    APTR physical;
    APTR physicalEnd;
    ULONG start;
    ULONG end;
    ULONG location;
    ULONG startPage;
    ULONG endPage;
    ULONG crtc;
    ULONG crtc2;
    ULONG ext;
    ULONG host;
    ULONG agp;
    ULONG newAgp;
    ULONG count;

    if (!bi->MemorySpaceBase || !apertureSize ||
        apertureSize - 1UL > ~0UL - (ULONG)bi->MemorySpaceBase)
        return FALSE;
    physical = PromPhysicalAddress(bi->MemorySpaceBase);
    physicalEnd = PromPhysicalAddress(
        (APTR)((ULONG)bi->MemorySpaceBase + apertureSize - 1UL));
    if (!physicalEnd ||
        (ULONG)physicalEnd != (ULONG)physical + apertureSize - 1UL)
        return FALSE;
    /* PCI address zero is a valid framebuffer BAR on Prometheus. */
    start = (ULONG)physical;
    if ((start & 0xffffUL) || !apertureSize ||
        apertureSize - 1UL > 0xffffffffUL - start)
        return FALSE;
    end = start + apertureSize - 1UL;
    startPage = start >> 16;
    endPage = end >> 16;
    if (endPage >= 0xffffUL)
        return FALSE;
    location = startPage | (endPage << 16);

    crtc = RadeonRead32(bi, RADEON_CRTC_GEN_CNTL);
    crtc2 = RadeonRead32(bi, RADEON_CRTC2_GEN_CNTL);
    ext = RadeonRead32(bi, RADEON_CRTC_EXT_CNTL);
    host = RadeonRead32(bi, RADEON_HOST_PATH_CNTL);
    agp = RadeonRead32(bi, RADEON_MC_AGP_LOCATION);
    newAgp = agp;
    if ((agp & 0xffffUL) != endPage + 1UL)
        newAgp = (endPage << 16) | (endPage + 1UL);
    RadeonWrite32(bi, RADEON_CRTC_GEN_CNTL,
                  crtc | RADEON_CRTC_DISP_REQ_EN_B);
    RadeonWrite32(bi, RADEON_CRTC2_GEN_CNTL,
                  crtc2 | RADEON_CRTC2_DISP_REQ_EN_B);
    RadeonWrite32(bi, RADEON_CRTC_EXT_CNTL,
                  ext | RADEON_CRTC_DISPLAY_DIS);
    RadeonDelayUs(100000);

    RadeonWrite32(bi, RADEON_HOST_PATH_CNTL, 0);
    for (count = 0; count < 100000; ++count) {
        if (RadeonRead32(bi, RADEON_MEM_STR_CNTL) & RADEON_MC_IDLE)
            break;
        RadeonDelayUs(1);
    }
    if (count == 100000) {
        RadeonWrite32(bi, RADEON_HOST_PATH_CNTL, host);
        RadeonWrite32(bi, RADEON_MC_AGP_LOCATION, agp);
        RadeonWrite32(bi, RADEON_CRTC_GEN_CNTL, crtc);
        RadeonWrite32(bi, RADEON_CRTC2_GEN_CNTL, crtc2);
        RadeonWrite32(bi, RADEON_CRTC_EXT_CNTL, ext);
        return FALSE;
    }

    RadeonWrite32(bi, RADEON_MC_FB_LOCATION, location);
    RadeonWrite32(bi, RADEON_MC_AGP_LOCATION, newAgp);
    RadeonWrite32(bi, RADEON_DISPLAY_BASE_ADDR, start);
    RadeonWrite32(bi, RADEON_DISPLAY2_BASE_ADDR, start);
    RadeonWrite32(bi, RADEON_OV0_BASE_ADDR, start);
    data->FramebufferGpuBase = start;
    RadeonDelayUs(100000);

    RadeonWrite32(bi, RADEON_CRTC_GEN_CNTL, crtc);
    RadeonWrite32(bi, RADEON_CRTC2_GEN_CNTL, crtc2);
    RadeonWrite32(bi, RADEON_CRTC_EXT_CNTL, ext);
    return TRUE;
}

BOOL RadeonInitializeHardware(struct BoardInfo *bi)
{
    struct RadeonBoardData *data = RadeonGetBoardData(bi);
    struct ExecBase *SysBase = bi->ExecBase;
    struct RadeonBios bios = {0};
    BOOL posted;
    BOOL haveBios = FALSE;
    ULONG memory;
    ULONG biosMemory = 0;
    ULONG aperture;
    ULONG framebufferSize = bi->MemorySpaceSize;

    posted = IsPosted(bi);
    RLOG("Radeon9200: card is %s\n", posted ? "posted" : "cold");

    if (LoadRom(bi, &bios) && SelectX86Image(&bios, data->DeviceId) &&
        ParseHeader(&bios)) {
        haveBios = TRUE;
        RLOG("Radeon9200: COMBIOS revision %ld\n", (ULONG)bios.Revision);
    }

    if (haveBios)
        biosMemory = BiosMemorySize(&bios);
    if (!posted) {
        if (!haveBios) {
            if (bios.Allocation)
                FreeMem(bios.Allocation, bios.AllocationSize);
            return FALSE;
        }
        if (!PostCard(bi, &bios, biosMemory)) {
            RLOG("Radeon9200: cold COMBIOS initialization failed\n");
            if (bios.Allocation)
                FreeMem(bios.Allocation, bios.AllocationSize);
            return FALSE;
        }
        RLOG("Radeon9200: cold COMBIOS initialization complete\n");
    }

    LoadClockInfo(bi, haveBios ? &bios : NULL);
    memory = RadeonRead32(bi, RADEON_CONFIG_MEMSIZE);
    if (!posted && biosMemory) {
        memory = biosMemory;
        RadeonWrite32(bi, RADEON_CONFIG_MEMSIZE, memory);
    } else if (memory < RADEON_FRAMEBUFFER_MIN_SIZE ||
        (memory & 0x000fffffUL) != 0 || memory > 0x20000000UL) {
        memory = biosMemory;
        if (memory)
            RadeonWrite32(bi, RADEON_CONFIG_MEMSIZE, memory);
    }

    if (bios.Allocation)
        FreeMem(bios.Allocation, bios.AllocationSize);

    if (memory < RADEON_FRAMEBUFFER_MIN_SIZE)
        return FALSE;

    aperture = RadeonRead32(bi, RADEON_CONFIG_APER_SIZE);
    if (aperture < RADEON_FRAMEBUFFER_MIN_SIZE ||
        (aperture & (aperture - 1UL)) ||
        aperture > framebufferSize)
        aperture = framebufferSize;
    if (aperture > 0x04000000UL)
        aperture = 0x04000000UL;
    if (memory > aperture)
        memory = aperture;
    data->InstalledVram = memory;

    if (!SetFramebufferLocation(bi, aperture)) {
        RLOG("Radeon9200: framebuffer location setup failed\n");
        return FALSE;
    }

    bi->MemorySize = memory;
    bi->MemorySpaceSize = aperture;
    bi->MemoryClock = data->MemoryClockHz;
    RLOG("Radeon9200: %ld MiB usable VRAM\n", memory >> 20);
    return TRUE;
}
