/*
 * mglprobe2 - distinguish LoadSeg vs OpenLibrary failure for minigl.library.
 */
#include <exec/types.h>
#include <exec/libraries.h>
/* BPTR from exec/types.h */
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>

int main(void)
{
    struct Library *base;
    BPTR seg;

    printf("mglprobe2 start\n");

    base = OpenLibrary((CONST_STRPTR)"dos.library", 0);
    printf("control dos.library      %08lx\n", (unsigned long)base);
    if (base)
        CloseLibrary(base);

    seg = LoadSeg((CONST_STRPTR)"LIBS:minigl.library");
    printf("LoadSeg LIBS:minigl      %08lx\n", (unsigned long)seg);
    if (seg)
        UnLoadSeg(seg);

    base = OpenLibrary((CONST_STRPTR)"LIBS:minigl.library", 0);
    printf("OpenLibrary full path    %08lx\n", (unsigned long)base);
    if (base)
        CloseLibrary(base);

    base = OpenLibrary((CONST_STRPTR)"minigl.library", 0);
    printf("OpenLibrary bare name    %08lx\n", (unsigned long)base);
    if (base)
        CloseLibrary(base);

    printf("mglprobe2 done\n");
    return 0;
}
