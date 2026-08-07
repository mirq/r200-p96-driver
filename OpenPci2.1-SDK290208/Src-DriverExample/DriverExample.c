/*

25 March 2005
- Cleaned a bit source code & removed warnings
 
Objective of this example:
--------------------------
- Find a PCI card on the bus (pci_find_device()).
- Try to obtain/lock the PCI card (pci_obtain_card())
- Enable PCI IO, Mem address space (pci_write_config_word()) and enable Busmaster (pci_set_master()).
- DMA memory allocation (pci_allocdma_mem())
- Physical->Logical/Virtual addr conversion example (pci_physic_to_logic_addr())
- Logical/Virtual->Physical addr conversion example with Logical/Virtual DMA memory 
-> Physical DMA memory conversion (pci_logic_to_physic_addr()).
- Physical DMA memory alignement (AlignMemory()).
- Release/unlock PCI card previously locked by pci_obtain_card() (pci_release_card())
- Add and remove an interrupt server, PPC MOS and 68k openpci pci_add_intserver()/pci_rem_intserver().
*/

#include <stdio.h>

#include <proto/exec.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/interrupts.h>

/* OpenPci Library */
#include <proto/openpci.h>
#include <libraries/openpci.h>

#include "declgate.h" /* required for MOS Gates */

#include "SDI_compiler.h" /* found in Aminet http://us.aminet.net/dev/c/CLib-SDI.lha */

struct Library *OpenPciBase;

/* Our DMA memory size to allocate */
#define DMA_MEM_SIZE 65535

struct MyBase
{
   ULONG driverstuff;
}Base;

/* Prototypes */

#ifdef __SASC
__interrupt __asm int PCICARDINTCode( REG( a1, struct MyBase* Base) );
#else
int DECLFUNC_1(PCICARDINTCode, a1, struct MyBase*, &Base);
#endif

APTR AlignMemory(APTR ptr, ULONG alignsize);

/* MorphOS GATE for Interrupt Code */

#ifdef __MORPHOS__

int DECLFUNC_1(PCICARDINTCode, a1, struct MyBase*, &Base);

static const
struct EmulLibEntry _gate_PCICARDINTCode = {TRAP_LIBNR, 0, (void (*)(void)) _PCICARDINTCode};

#else /* __MORPHOS__ */

#define _gate_PCICARDINTCode PCICARDINTCode

#endif /* __MORPHOS__ */

struct Library *OpenPciBase = NULL;

int main(int argc, char **argv)
{
	/* PCICARD Hardware Interrupt */
	struct Interrupt PCICARDInterrupt; 
    
    /* Our pci device structure */
    struct pci_dev *pcidev=NULL;
    
	/* Our DMA memory allocated */
	APTR dmamem=NULL;

    /* Logical addr (seen by CPU side) */
    APTR logical_addr=NULL;
    
    /* Our physical DMA memory (seen by PCI Bus side) */
    APTR physical_dmamem=NULL;
    /* Our physical memory (seen by PCI Bus side) aligned */
    APTR physicaldmamem_aligned=NULL;

    unsigned long dma_mem_maxsize=0;

	/* We open trhe latest OpenPci.library ONLY */

	if(( OpenPciBase=(struct Library *)OpenLibrary("openpci.library",MIN_OPENPCI_VERSION))==NULL)
	{
   		printf("Error in opening openpci.library v%d or more\n",MIN_OPENPCI_VERSION);
   		exit(10);
	}

    /* 
        First we find the device in this example it's a Realtek RTL8139 chipset 
        (with vendorid=0x10ec and deviceid=0x8139) who support I/O, Mem address space 
        and Busmaster for DMA transfert from/to PCI card <-> DMA memory.
    */
    pcidev=pci_find_device(0x10ec,0x8139,NULL);

    /* If the device is found */
    if(pcidev)
    {
        
		/*
			We try to obtain the Pci Card
		*/
		if( pci_obtain_card(pcidev) )
		{
			printf("PCI card obtained/locked successfully\n");
		}else
		{
			printf("Failed to obtain/lock PCI card\nPci card probably already locked by an other driver\n");
			CloseLibrary(OpenPciBase);
			exit(10);
		}
		
		/* 
            We Enable MEMADDR and IO ADDR space 
            (see libraries/openpci.h for all other PCI commands)        
        */
		pci_write_config_word(PCI_COMMAND,PCI_COMMAND_IO|PCI_COMMAND_MEMORY,pcidev);
        
        /* 
            The RTL8139 chipset use Busmastering for DMA access to send and receive data
            We activate the Busmaster
        */                                
	if( pci_set_master(pcidev) )
        {
		printf("Bus Master enabled\n");
        }else
        {
            printf("No Bus Master capable\n");
			/* Release/unlock PCI card previously locked by pci_obtain_card() */
			pci_release_card(pcidev);
			CloseLibrary(OpenPciBase);
			exit(10);
        }
            
        /* 
            DMA Memory Allocation 
        */
        dmamem=pci_allocdma_mem(DMA_MEM_SIZE,MEM_NONCACHEABLE);

        if(dmamem!=NULL)
        {	
           /* Convert a physical addr to logical/virtual addr */
           logical_addr=pci_physic_to_logic_addr( (APTR)0x1234,pcidev); 

           /* 
           The value 0x1234 who represant a physical addr seen by Pci Bus side
           is just an example maybe this addr does not exist and has no logical addr 
           */
           if(logical_addr!=NULL)
           {
                      printf("Error to convert this Physical addr -> Logical/Virtual addr\n");
		/* We could exit there */
           }

            /* 
                 We convert this logical address (seen by CPU side) to a physical address (seen by PCI Bus)
            */
            physical_dmamem=pci_logic_to_physic_addr(dmamem,pcidev);    
            if(physical_dmamem!=NULL)
            {
                /* 
					We align physical DMA memory with an align size of 64 bytes
                */
                physicaldmamem_aligned=AlignMemory(physical_dmamem,64L);    

                /*
                    We calculate the maximum memory available after alignment
                */                
                dma_mem_maxsize=DMA_MEM_SIZE-((ULONG)physicaldmamem_aligned-(ULONG)physical_dmamem);
                
                /* 
		Now we can use DMA physical aligned memory with the PCI chipset
                    and becarefull to don't exceed the dma_mem_maxsize.
                */

				/* We fill the Pci Card Interruption structure */
				PCICARDInterrupt.is_Node.ln_Type = NT_INTERRUPT;
				PCICARDInterrupt.is_Node.ln_Pri=0;
				PCICARDInterrupt.is_Node.ln_Name = "PCICARD INT";
				PCICARDInterrupt.is_Data=&Base;
				PCICARDInterrupt.is_Code=(void (* )())(&_gate_PCICARDINTCode);

				printf("We add interrupt server\n");
				/* We add the interrupt server */
				if(pci_add_intserver(&PCICARDInterrupt,pcidev)==FALSE)
				{
					printf("Error to add the PCICARD Interrupt server !\n");
					/* Release/unlock PCI card previously locked by pci_obtain_card() */
					pci_freedma_mem(dmamem,DMA_MEM_SIZE);
					pci_release_card(pcidev);
					CloseLibrary(OpenPciBase);
					exit(10);
				}

				/*
					Your driver code init
					....
				*/

				/* We clean all stuff */

				/* We remove our interrupt server */
				pci_rem_intserver(&PCICARDInterrupt,pcidev);			

                /* Test finished
		    We free (willy ;-) the DMA memory
                */
                pci_freedma_mem(dmamem,DMA_MEM_SIZE);
               
				/* Release/unlock PCI card previously locked by pci_obtain_card() */
				pci_release_card(pcidev);

            }else
            {
				printf("Error to convert logical to physical DMA memory\n");
				pci_freedma_mem(dmamem,DMA_MEM_SIZE);
				pci_release_card(pcidev);
				CloseLibrary(OpenPciBase);
                exit(10);
            }
        
        }else
        {
			printf("Not enough memory for the DMA Buffer : %ld bytes required\n",DMA_MEM_SIZE);
			pci_release_card(pcidev);
			CloseLibrary(OpenPciBase);
			exit(10);
        }
                    
    }else
    {
        printf("Error no PCI card found with vendorid=0x10ec and deviceid=0x8139\n");
		CloseLibrary(OpenPciBase);
		exit(10);
    }    

	CloseLibrary(OpenPciBase);
	exit(0);
}

/*
	PCI Card Interrupt code
*/
#ifdef __SASC
__interrupt __asm int PCICARDINTCode( REG( a1, struct MyBase* Base) )
#else
int DECLFUNC_1(PCICARDINTCode, a1, struct MyBase*, &Base)
#endif
{

#ifdef __MORPHOS__
	DECLARG_1(a1, struct MyBase*, Base)
#endif

	/*
		Your interrupt code
		....
	*/

	/* It's not our Interrupt */
	return 0;

	/* It's our interrupt */
    return 1;
}

/* 
    Our align memory function 
    INPUT: 
			- ptr=Memory pointer to align
            - alignsize=Size of the alignement requiered
    OUTPUT:
            - Memory aligned
*/
APTR AlignMemory(APTR ptr, ULONG alignsize)
{
    return( (APTR) (((ULONG) ptr + alignsize - 1) & -alignsize) );    
}   
