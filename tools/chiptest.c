/*
 * chiptest - user-mode chip-library load test (2026-09-18).
 *
 * Reproduces the card's chip-load call for the release and the -debug
 * library name and reports exactly what exec/DOS did. It never calls a chip
 * entry point and opens/closes only, so it is safe to run beside the live
 * driver. Purpose: prove or disprove that the -debug file name itself fails
 * to load, which is the only remaining difference between the working
 * release pair and the failing debug-card boots.
 */
#include <exec/libraries.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>

static void CheckFile(const char *path)
{
    BPTR lock = Lock((CONST_STRPTR)path, 0);
    struct FileInfoBlock *fib;
    LONG ok = 0;

    if (lock) {
        fib = (struct FileInfoBlock *)AllocMem(sizeof(*fib),
                                               MEMF_PUBLIC | MEMF_CLEAR);
        if (fib && Examine(lock, fib))
            ok = fib->fib_Size;
        if (fib)
            FreeMem(fib, sizeof(*fib));
        UnLock(lock);
    }
    printf("%-40s lock=%s size=%ld\n", path, lock ? "OK" : "FAIL",
           (long)ok);
}

static void TryName(const char *name)
{
    struct Library *base;

    printf("opening '%s' ...\n", name);
    fflush(stdout);
    base = OpenLibrary((CONST_STRPTR)name, 1);
    if (base) {
        printf("  OK base=%08lx node='%s' ver=%lu rev=%lu opencnt=%lu\n",
               (unsigned long)base,
               base->lib_Node.ln_Name ? base->lib_Node.ln_Name : "(null)",
               (unsigned long)base->lib_Version,
               (unsigned long)base->lib_Revision,
               (unsigned long)base->lib_OpenCnt);
        CloseLibrary(base);
        printf("  closed\n");
    } else {
        printf("  FAILED (OpenLibrary returned NULL)\n");
    }
    fflush(stdout);
}

int main(void)
{
    printf("chiptest: chip name load probe\n");
    CheckFile("LIBS:Picasso96/Radeon9200.chip");
    CheckFile("LIBS:Picasso96/Radeon9200-debug.chip");
    TryName("picasso96/Radeon9200.chip");
    TryName("picasso96/Radeon9200-debug.chip");
    printf("chiptest: done\n");
    return 0;
}
