
February 29, 2008:
- Added vbcc specific inline and libraries (thanks to Christoph Fassbach).
- fixed libraries/openpci.h (thanks to Christoph Fassbach).

April 24, 2006:
- Fixed tons of typo for compatibility with C++ renamed class to devclass in Include/libraries/openpci.h, Include/clib/openpci_protos.h, Include/gcc/inline/openpci.h
Include/fd/openpci_lib.fd, doc/OpenPci2.txt, OpenPci2.doc, OpenPci2.rtf, OpenPci2.pdf

June 6, 2005:
- Added SFD support (thanks to Guido Mersmann for example)
- New openpci.library v2.1 for 68K & MOS included
- New OpenPciInfo V1.5 source code
- Cleanup in Src-Tools & Src-DriverExample
- Added PCI_CAPABILITIES_PTR/PCI_NEW_CAPABILITIES (PCI 2.2), 
PCI_STATUS_ERRBITS (to know if an error occured on the PCI bus) & comments in openpci.h

February 25, 2005:
- Thanks to "naTmeg" for OpenPCI 2.0 autodoc & comments/correction in SDK
- Fixed a bug in OpenPCIInfo -> line 182: printf("ClassCode: 0x%06lx\n",dev->class); should be ...dev->devclass); Thanks to "naTmeg"

January 09, 2005:
- New openpci.library/openpci.library.elf v2 with some new functions
pci_physic_to_logic_addr()/pci_obtain_card()/pci_release_card()
- This version is fully compatible with old drivers developped for openpci v1.x but i advise you
to support new pci card lock mechanism (pci_obtain_card()/pci_release_card()) to avoid problems 
with drivers who use the same pci card.
- Fixed read & write_config for Prometheus
- Use lock mechanism of Prometheus, Amithlon, Grex
Warning this library support Mediator and i haven't tested it on 
Mediator, Prometheus, Grex, Amithlon but it could work.
Please test and send me feedback for Mediator, Prometheus, Grex, Amithlon, Pegasos1 & Pegasos2.

May 25, 2003:
- New openpci.library/openpci.library.elf v1.1 with some fix for Pegasos and Grex PPC native version
- New example : DriverExample dir
- Documentation updated
- OpenPciInfo.c cleanup

April 08, 2003:
- New powerpci.library v2.11 in Amithlon/ dir
- New openpci.library v1.04 24/03/03:
openpci.library.elf has some fix for Pegasos, and openpci.library has an Amithlon fix.
- Updates all your includes with the new one.
Includes changes in openpci.h :
New include use the new hardware/byteswap.h support from the MorphOS Team includes.
Old hardware/bebyteswap.h and byteswap.h are removed use the new byteswap.h.

Now just use optimized SWAPLONG(), SWAPWORD() or SWAP2WORD() before it was swapl() and swapw().

Infos:

The openpci.library currently support fully GrexA1200, GrexA4000, Amithlon
Prometheus wrapper is finished but need more testing.

openpci.library.elf is for MorphOS and Pegasos

For Pegasos use openpci.library.elf ONLY and remove openpci.library if it exist,
because openpci.library doesn't support Pegasos.

openpci_8139Device/openpci_8139.device.elf is for MorphOS

Requirements:

To use openpci.library driver with GrexA1200-A4000 or Pegasos board you need:
cybpci.library v2.x or more
68040 or 68060.library by Ralph Schmidt v44 or more 
You can download all this files to: 
http://www.vgr.com/grex/

To use openpci.library driver with Prometheus board you need:
prometheus.library v2.x or more

To use openpci.library driver with Amithlon board you need:
powerpci.library v2.xx or more


