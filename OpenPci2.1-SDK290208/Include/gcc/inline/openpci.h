/* This inline has been modified by hand i hope all is ok i haven't tested it */

#ifndef _INLINE_OPENPCI_H
#define _INLINE_OPENPCI_H

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif /* !__INLINE_MACROS_H */

#ifndef OPENPCI_BASE_NAME
#define OPENPCI_BASE_NAME OpenPciBase
#endif /* !OPENPCI_BASE_NAME */

#define pci_bus() \
	LP0(0x1e, unsigned short, pci_bus, \
	, OPENPCI_BASE_NAME)

#define pci_inb(address) \
	LP1(0x24, unsigned char, pci_inb, unsigned long, address, a0, \
	, OPENPCI_BASE_NAME)

#define pci_inw(address) \
	LP1(0x30, unsigned short, pci_inw, unsigned long, address, a0, \
	, OPENPCI_BASE_NAME)

#define pci_inl(address) \
	LP1(0x3c, unsigned long, pci_inl, unsigned long, address, a0, \
	, OPENPCI_BASE_NAME)

#define pci_outb(value, address) \
	LP2NR(0x2a, pci_outb, unsigned char, value, d0, unsigned long, address, a0, \
	, OPENPCI_BASE_NAME)

#define pci_outw(value, address) \
	LP2NR(0x36, pci_outw, unsigned short, value, d0, unsigned long, address, a0, \
	, OPENPCI_BASE_NAME)

#define pci_outl(value, address) \
	LP2NR(0x42, pci_outl, unsigned long, value, d0, unsigned long, address, a0, \
	, OPENPCI_BASE_NAME)

#define pci_to_hostcpy(pcimemsrc, memdest, packetsize) \
	LP3NR(0x48, pci_to_hostcpy, void *, pcimemsrc, a0, void *, memdest, a1, unsigned long, packetsize, d0, \
	, OPENPCI_BASE_NAME)

#define host_to_pcicpy(memsrc, pcimemdest, packetsize) \
	LP3NR(0x4e, host_to_pcicpy, void *, memsrc, a0, void *, pcimemdest, a1, unsigned long, packetsize, d0, \
	, OPENPCI_BASE_NAME)

#define pci_to_pcicpy(pcimemsrc, pcimemdest, packetsize) \
	LP3NR(0x54, pci_to_pcicpy, void *, pcimemsrc, a0, void *, pcimemdest, a1, unsigned long, packetsize, d0, \
	, OPENPCI_BASE_NAME)

#define pci_find_device(vendor, device, pcidev) \
	LP3(0x5a, struct  pci_dev *, pci_find_device, unsigned short, vendor, d0, unsigned short, device, d1, struct pci_dev *, pcidev, a0, \
	, OPENPCI_BASE_NAME)

#define pci_find_class(devclass, pcidev) \
	LP2(0x60, struct  pci_dev *, pci_find_class, unsigned long, devclass, d0, struct  pci_dev *, pcidev, a0, \
	, OPENPCI_BASE_NAME)

#define pci_find_slot(bus, devfn) \
	LP2(0x66, struct  pci_dev *, pci_find_slot, unsigned char, bus, d0, unsigned long, devfn, d1, \
	, OPENPCI_BASE_NAME)

#define pci_read_config_byte(registernum, pcidev) \
	LP2(0x6c, unsigned char, pci_read_config_byte, unsigned char, registernum, d0, struct  pci_dev *, pcidev, a0, \
	, OPENPCI_BASE_NAME)

#define pci_read_config_word(registernum, pcidev) \
	LP2(0x72, unsigned short, pci_read_config_word, unsigned char, registernum, d0, struct  pci_dev *, pcidev, a0, \
	, OPENPCI_BASE_NAME)

#define pci_read_config_long(registernum, pcidev) \
	LP2(0x78, unsigned long, pci_read_config_long, unsigned char, registernum, d0, struct  pci_dev *, pcidev, a0, \
	, OPENPCI_BASE_NAME)

#define pci_write_config_byte(registernum, val, pcidev) \
	LP3NR(0x7e, pci_write_config_byte, unsigned char, registernum, d0, unsigned char, val, d1, struct  pci_dev *, pcidev, a0, \
	, OPENPCI_BASE_NAME)

#define pci_write_config_word(registernum, val, pcidev) \
	LP3NR(0x84, pci_write_config_word, unsigned char, registernum, d0, unsigned short, val, d1, struct  pci_dev *, pcidev, a0, \
	, OPENPCI_BASE_NAME)

#define pci_write_config_long(registernum, val, pcidev) \
	LP3NR(0x8a, pci_write_config_long, unsigned char, registernum, d0, unsigned long, val, d1, struct  pci_dev *, pcidev, a0, \
	, OPENPCI_BASE_NAME)

#define pci_set_master(pcidev) \
	LP1(0x90, BOOL, pci_set_master, struct  pci_dev *, pcidev, a0, \
	, OPENPCI_BASE_NAME)

#define pci_add_intserver(PciInterrupt, pcidev) \
	LP2(0x96, BOOL, pci_add_intserver, struct Interrupt *, PciInterrupt, a0, struct pci_dev *, pcidev, a1, \
	, OPENPCI_BASE_NAME)

#define pci_rem_intserver(PciInterrupt, pcidev) \
	LP2NR(0x9c, pci_rem_intserver, struct Interrupt *, PciInterrupt, a0, struct pci_dev *, pcidev, a1, \
	, OPENPCI_BASE_NAME)

#define pci_allocdma_mem(size, flags) \
	LP2(0xa2, APTR, pci_allocdma_mem, unsigned long, size, d0, unsigned long, flags, d1, \
	, OPENPCI_BASE_NAME)

#define pci_freedma_mem(buffer, size) \
	LP2NR(0xa8, pci_freedma_mem, APTR, buffer, a0, unsigned long, size, d0, \
	, OPENPCI_BASE_NAME)

#define pci_logic_to_physic_addr(PciLogicalAddr, pcidev) \
	LP2(0xae, APTR, pci_logic_to_physic_addr, APTR, PciLogicalAddr, a0, struct pci_dev *, pcidev, a1, \
	, OPENPCI_BASE_NAME)

#define pci_physic_to_logic_addr(PciPhysicalAddr, pcidev) \
	LP2(0xb4, APTR, pci_physic_to_logic_addr, APTR, PciPhysicalAddr, a0, struct pci_dev *, pcidev, a1, \
	, OPENPCI_BASE_NAME)

#define pci_obtain_card(pcidev) \
	LP1(0xba, BOOL, pci_obtain_card, struct  pci_dev *, pcidev, a0, \
	, OPENPCI_BASE_NAME)

#define pci_release_card(pcidev) \
	LP1NR(0xc0, pci_release_card, struct  pci_dev *, pcidev, a0, \
	, OPENPCI_BASE_NAME)

#endif /* !_INLINE_OPENPCI_H */
