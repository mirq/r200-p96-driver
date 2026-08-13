#ifndef PROMETHEUS_API_H
#define PROMETHEUS_API_H

#include <exec/libraries.h>
#include <inline/macros.h>
#include <prometheus.h>

extern struct Library *PrometheusBase;

#define PROM_PCI_COMMAND         0x04U
#define PROM_PCI_COMMAND_MEMORY  0x0002U
#define PROM_PCI_COMMAND_MASTER  0x0004U
#define PROM_PCI_ROM_ADDRESS     0x30U
#define PROM_PCI_ROM_ENABLE      0x00000001UL
#define PROM_PCI_ROM_MASK        0xfffff800UL

#define PromReadConfigWord(board, offset) \
    LP2(0x30, UWORD, PromReadConfigWord, PCIBoard *, board, a0, \
        UBYTE, offset, d0, , PrometheusBase)

#define PromReadConfigLong(board, offset) \
    LP2(0x2a, ULONG, PromReadConfigLong, PCIBoard *, board, a0, \
        UBYTE, offset, d0, , PrometheusBase)

#define PromWriteConfigWord(board, value, offset) \
    LP3NR(0x42, PromWriteConfigWord, PCIBoard *, board, a0, \
          UWORD, value, d0, UBYTE, offset, d1, , PrometheusBase)

#define PromWriteConfigLong(board, value, offset) \
    LP3NR(0x3c, PromWriteConfigLong, PCIBoard *, board, a0, \
          ULONG, value, d0, UBYTE, offset, d1, , PrometheusBase)

#define PromAllocDma(size) \
    LP1(0x60, APTR, PromAllocDma, ULONG, size, d0, , PrometheusBase)

#define PromFreeDma(memory, size) \
    LP2NR(0x66, PromFreeDma, APTR, memory, a0, ULONG, size, d0, \
          , PrometheusBase)

#define PromPhysicalAddress(address) \
    LP1(0x6c, APTR, PromPhysicalAddress, APTR, address, d0, \
        , PrometheusBase)

#endif
