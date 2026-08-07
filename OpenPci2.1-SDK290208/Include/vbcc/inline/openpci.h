#ifndef _VBCCINLINE_OPENPCIB_H
#define _VBCCINLINE_OPENPCIB_H

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif

UWORD __pci_bus(__reg("a6") struct Library *)="\tjsr\t-30(a6)";
#define pci_bus() __pci_bus(OpenPciBase)

UBYTE __pci_inb(__reg("a6") struct Library *, __reg("a0") void * address)="\tjsr\t-36(a6)";
#define pci_inb(address) __pci_inb(OpenPciBase, (void *)(address))

void __pci_outb(__reg("a6") struct Library *, __reg("d0") ULONG value, __reg("a0") void * address)="\tjsr\t-42(a6)";
#define pci_outb(value, address) __pci_outb(OpenPciBase, (value), (void *)(address))

UWORD __pci_inw(__reg("a6") struct Library *, __reg("a0") void * address)="\tjsr\t-48(a6)";
#define pci_inw(address) __pci_inw(OpenPciBase, (void *)(address))

void __pci_outw(__reg("a6") struct Library *, __reg("d0") ULONG value, __reg("a0") void * address)="\tjsr\t-54(a6)";
#define pci_outw(value, address) __pci_outw(OpenPciBase, (value), (void *)(address))

ULONG __pci_inl(__reg("a6") struct Library *, __reg("a0") void * address)="\tjsr\t-60(a6)";
#define pci_inl(address) __pci_inl(OpenPciBase, (void *)(address))

void __pci_outl(__reg("a6") struct Library *, __reg("d0") ULONG value, __reg("a0") void * address)="\tjsr\t-66(a6)";
#define pci_outl(value, address) __pci_outl(OpenPciBase, (value), (void *)(address))

void __pci_to_hostcpy(__reg("a6") struct Library *, __reg("a0") void * pcimemsrc, __reg("a1") void * memdest, __reg("d0") ULONG packetsize)="\tjsr\t-72(a6)";
#define pci_to_hostcpy(pcimemsrc, memdest, packetsize) __pci_to_hostcpy(OpenPciBase, (pcimemsrc), (memdest), (packetsize))

void __host_to_pcicpy(__reg("a6") struct Library *, __reg("a0") void * memsrc, __reg("a1") void * pcimemdest, __reg("d0") ULONG packetsize)="\tjsr\t-78(a6)";
#define host_to_pcicpy(memsrc, pcimemdest, packetsize) __host_to_pcicpy(OpenPciBase, (memsrc), (pcimemdest), (packetsize))

void __pci_to_pcicpy(__reg("a6") struct Library *, __reg("a0") void * pcimemsrc, __reg("a1") void * pcimemdest, __reg("d0") ULONG packetsize)="\tjsr\t-84(a6)";
#define pci_to_pcicpy(pcimemsrc, pcimemdest, packetsize) __pci_to_pcicpy(OpenPciBase, (pcimemsrc), (pcimemdest), (packetsize))

struct  pci_dev * __pci_find_device(__reg("a6") struct Library *, __reg("d0") ULONG vendor, __reg("d1") ULONG device, __reg("a0") struct pci_dev * pcidev)="\tjsr\t-90(a6)";
#define pci_find_device(vendor, device, pcidev) __pci_find_device(OpenPciBase, (vendor), (device), (pcidev))

struct  pci_dev * __pci_find_class(__reg("a6") struct Library *, __reg("d0") ULONG devclass, __reg("a0") struct pci_dev * pcidev)="\tjsr\t-96(a6)";
#define pci_find_class(devclass, pcidev) __pci_find_class(OpenPciBase, (devclass), (pcidev))

struct  pci_dev * __pci_find_slot(__reg("a6") struct Library *, __reg("d0") ULONG bus, __reg("d1") ULONG devfn)="\tjsr\t-102(a6)";
#define pci_find_slot(bus, devfn) __pci_find_slot(OpenPciBase, (bus), (devfn))

UBYTE __pci_read_config_byte(__reg("a6") struct Library *, __reg("d0") ULONG registernum, __reg("a0") struct pci_dev * pcidev)="\tjsr\t-108(a6)";
#define pci_read_config_byte(registernum, pcidev) __pci_read_config_byte(OpenPciBase, (registernum), (pcidev))

UWORD __pci_read_config_word(__reg("a6") struct Library *, __reg("d0") ULONG registernum, __reg("a0") struct pci_dev * pcidev)="\tjsr\t-114(a6)";
#define pci_read_config_word(registernum, pcidev) __pci_read_config_word(OpenPciBase, (registernum), (pcidev))

ULONG __pci_read_config_long(__reg("a6") struct Library *, __reg("d0") ULONG registernum, __reg("a0") struct pci_dev * pcidev)="\tjsr\t-120(a6)";
#define pci_read_config_long(registernum, pcidev) __pci_read_config_long(OpenPciBase, (registernum), (pcidev))

void __pci_write_config_byte(__reg("a6") struct Library *, __reg("d0") ULONG registernum, __reg("d1") ULONG val, __reg("a0") struct pci_dev * pcidev)="\tjsr\t-126(a6)";
#define pci_write_config_byte(registernum, val, pcidev) __pci_write_config_byte(OpenPciBase, (registernum), (val), (pcidev))

void __pci_write_config_word(__reg("a6") struct Library *, __reg("d0") ULONG registernum, __reg("d1") ULONG val, __reg("a0") struct pci_dev * pcidev)="\tjsr\t-132(a6)";
#define pci_write_config_word(registernum, val, pcidev) __pci_write_config_word(OpenPciBase, (registernum), (val), (pcidev))

void __pci_write_config_long(__reg("a6") struct Library *, __reg("d0") ULONG registernum, __reg("d1") ULONG val, __reg("a0") struct pci_dev * pcidev)="\tjsr\t-138(a6)";
#define pci_write_config_long(registernum, val, pcidev) __pci_write_config_long(OpenPciBase, (registernum), (val), (pcidev))

BOOL __pci_set_master(__reg("a6") struct Library *, __reg("a0") struct  pci_dev * pcidev)="\tjsr\t-144(a6)";
#define pci_set_master(pcidev) __pci_set_master(OpenPciBase, (pcidev))

BOOL __pci_add_intserver(__reg("a6") struct Library *, __reg("a0") struct Interrupt * PciInterrupt, __reg("a1") struct pci_dev * pcidev)="\tjsr\t-150(a6)";
#define pci_add_intserver(PciInterrupt, pcidev) __pci_add_intserver(OpenPciBase, (PciInterrupt), (pcidev))

void __pci_rem_intserver(__reg("a6") struct Library *, __reg("a0") struct Interrupt * PciInterrupt, __reg("a1") struct pci_dev * pcidev)="\tjsr\t-156(a6)";
#define pci_rem_intserver(PciInterrupt, pcidev) __pci_rem_intserver(OpenPciBase, (PciInterrupt), (pcidev))

APTR __pci_allocdma_mem(__reg("a6") struct Library *, __reg("d0") ULONG size, __reg("d1") ULONG flags)="\tjsr\t-162(a6)";
#define pci_allocdma_mem(size, flags) __pci_allocdma_mem(OpenPciBase, (size), (flags))

void __pci_freedma_mem(__reg("a6") struct Library *, __reg("a0") APTR buffer, __reg("d0") ULONG size)="\tjsr\t-168(a6)";
#define pci_freedma_mem(buffer, size) __pci_freedma_mem(OpenPciBase, (buffer), (size))

APTR __pci_logic_to_physic_addr(__reg("a6") struct Library *, __reg("a0") APTR PciLogicalAddr, __reg("a1") struct pci_dev * pcidev)="\tjsr\t-174(a6)";
#define pci_logic_to_physic_addr(PciLogicalAddr, pcidev) __pci_logic_to_physic_addr(OpenPciBase, (PciLogicalAddr), (pcidev))

APTR __pci_physic_to_logic_addr(__reg("a6") struct Library *, __reg("a0") APTR PciPhysicalAddr, __reg("a1") struct pci_dev * pcidev)="\tjsr\t-180(a6)";
#define pci_physic_to_logic_addr(PciPhysicalAddr, pcidev) __pci_physic_to_logic_addr(OpenPciBase, (PciPhysicalAddr), (pcidev))

BOOL __pci_obtain_card(__reg("a6") struct Library *, __reg("a0") struct pci_dev * pcidev)="\tjsr\t-186(a6)";
#define pci_obtain_card(pcidev) __pci_obtain_card(OpenPciBase, (pcidev))

void __pci_release_card(__reg("a6") struct Library *, __reg("a0") struct pci_dev * pcidev)="\tjsr\t-192(a6)";
#define pci_release_card(pcidev) __pci_release_card(OpenPciBase, (pcidev))

#endif /*  _VBCCINLINE_OPENPCIB_H  */
