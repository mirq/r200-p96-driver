/*
 * mglprobe - minimal diagnostic for the 68k host's MiniGLOpen() failure.
 * Prints OpenLibrary("minigl.library", 12) and v0 results plus the library
 * version fields, without touching any LVO. Safe to run anywhere.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>

static void Report(const char *what, struct Library *base)
{
    if (!base) {
        printf("%-8s NULL\n", what);
        return;
    }
    printf("%-8s base=%08lx name='%s' ver=%lu rev=%lu flags=%04lx\n",
           what, (unsigned long)base,
           base->lib_Node.ln_Name ? base->lib_Node.ln_Name : "(null)",
           (unsigned long)base->lib_Version,
           (unsigned long)base->lib_Revision,
           (unsigned long)base->lib_Flags);
}

int main(void)
{
    struct Library *l12;
    struct Library *l0;

    printf("mglprobe start\n");
    l12 = OpenLibrary((CONST_STRPTR)"minigl.library", 12);
    Report("v12:", l12);
    if (l12)
        CloseLibrary(l12);

    l0 = OpenLibrary((CONST_STRPTR)"minigl.library", 0);
    Report("v0:", l0);
    if (l0)
        CloseLibrary(l0);

    printf("mglprobe done\n");
    return 0;
}
