/*
   OpenPciDump v0.1 coded by Titan Email : bvernoux@wanadoo.fr
   15/08/2002
   - First version
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <proto/exec.h>

#include <proto/openpci.h>
#include <libraries/openpci.h>

static char version[]="$VER: OpenPciDump v0.1 (15/08/2002)"; 

struct Library *OpenPciBase = NULL;

void dumpmem(unsigned long addr, void *mem, unsigned long len);

void usage(void)
{
 printf("OpenPciDump v0.1 (15/08/2002) created by Titan\n");
 printf("Usage: OpenPciDump <Addr> <Size (max 4096)>\n");
 printf("Example: OpenPciDump 0x65432100 255\n");

 exit(0);
}

void main(int argc, char **argv)
{

 	unsigned char bus=0;
 	unsigned long addr,size;
 	unsigned char *buff;
 
 	if (argc<3 || argc>3) usage();

	if( sscanf(argv[1],"0x%08lx",&addr)!=1 )
			usage();

	size=atol(argv[2]);
	if(size>4096)
		usage();
	
	buff=malloc(size+1);
	if(buff==0)
		printf("Not Enough Memory for Allocation\n");
		
	if(( OpenPciBase=(struct Library *)OpenLibrary("openpci.library",0))==NULL)
	{
   	printf("Error in opening openpci.library v0 or more\n");
   	return;
	}	

	bus=pci_bus();
	if(bus>0)
	{  
		switch(bus)
		{
			case MediatorA1200Bus:
										printf("Bus Mediator A1200 detected\n\n");
										break;
			case MediatorZ4Bus:
										printf("Bus Mediator Z4 detected\n\n");
										break;
			case PrometheusBus:
										printf("Bus Prometheus detected\n\n");
										break;
			case GrexA1200Bus:
										printf("Bus Grex A1200 detected\n\n");
										break;
			case GrexA4000Bus:
										printf("Bus Grex A4000 detected\n\n");
										break;
			case PegasosBus:
										printf("Bus Pegasos detected\n\n");
										break;
			case PowerPciBus:		
										printf("Amithlon PCI Bus detected\n\n");
										break;
			default:
										printf("PCI Bus not detected\n\n");
		}   		
	}		

	pci_to_hostcpy((void *)(addr),buff,size);
	printf("Addr : 0x%08lx Size : %ld\n",addr,size);
	dumpmem(addr,buff,size);
	free(buff);
		
 CloseLibrary( (struct Library*)OpenPciBase ); 

}

void dumpmem(unsigned long addr, void *mem, unsigned long len)
{
  unsigned char *p;

  if (!mem || !len) { return; }

  p = (unsigned char *) mem;

  printf("\n");

  do
  {
    unsigned char b, c, str[17];

	 printf("$%08lx  ",addr);
    for (b = 0; b < 16; b++)
    {
      c = *p++;
      str[b] = ((c >= ' ') && (c <= 'z')) ? c : '.';
      str[b + 1] = 0;
      printf("%02lx ", c);
      if (--len == 0) break;
    }
    addr+=16;

    while (++b < 16)
    {
      printf("   ");
    }

    printf("  %s\n", str);
  } while (len);

  printf("\n\n");
}

