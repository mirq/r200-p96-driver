#
# OpenPciDump gcc makefile
#

NAME = OpenPciDump

DEB =
#DEB = -DDEBUG

# -fomit-frame-pointer is evil on m68k gcc!
CC_68K		= gcc -idirafter /os-include
LD_68K		= gcc
LCFLAGS_68K	= $(DEB) -Wall -O2 -m68020-60 -mregparm=3 -noixemul -nostdlib
LDFLAGS_68K	= -noixemul -nostdlib
LIBS_68K	= -lamiga

CC_PPC		= ppc-morphos-gcc -idirafter /os-include
LD_PPC		= ppc-morphos-gcc
LCFLAGS_PPC	= $(DEB) -Wall -O2 -mcpu=603e -mmultiple -fomit-frame-pointer -noixemul
LDFLAGS_PPC	= -noixemul
LIBS_PPC	= -labox


##############################################################################
# object files

OBJS_68K	= $(NAME).o68k
OBJS_PPC	= $(NAME).oPPC
HEADERS	= 

##############################################################################
# standard targets

.PHONY:	all install clean


all: $(NAME).elf

install:
	copy clone $(NAME).elf c:

clean:
	$(RM) $(OBJS_68K) $(OBJS_PPC)

##############################################################################
# dependencies

$(NAME).oPPC:

##############################################################################

.SUFFIXES:
.SUFFIXES: .c .oPPC .o68K

%.o68K:	%.c
	$(CC_68K) $(LCFLAGS_68K) -c $< -o $@

%.oPPC:	%.c
	$(CC_PPC) $(LCFLAGS_PPC) -c $< -o $@

$(NAME):     $(OBJS_68K)
	$(LD_68K) $(LDFLAGS_68K) $^ -o $@ $(LIBS_68K)

$(NAME).elf: $(OBJS_PPC)
	$(LD_PPC) $(LDFLAGS_PPC) $^ -o $@.db $(LIBS_PPC)
	ppc-morphos-strip -o $@ --remove-section=.comment $@.db
	@#ppc-morphos-objdump --syms $@ | grep *UND*

