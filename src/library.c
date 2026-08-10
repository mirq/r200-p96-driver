#define __NOLIBBASE__

#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <proto/exec.h>

#include "radeon9200.h"

#define LIB_VERSION 0
#define LIB_REVISION 12

#define USED __attribute__((used))

static const char LibName[] = "Radeon9200.card";
static const char CardName[] = "Radeon9200";
static const char LibIdString[] =
    "Radeon9200.card 0.12 (10.8.2026)\r\n";
static const char VerTag[] USED =
    "\0$VER: Radeon9200.card 0.12 (10.8.2026)";

struct Library *OpenPciBase;

static struct RadeonCardBase *LibOpen(
    __REGA6(struct RadeonCardBase *base));
static LONG LibClose(__REGA6(struct RadeonCardBase *base));
static APTR LibExpunge(__REGA6(struct RadeonCardBase *base));
static LONG LibReserved(void);
static struct RadeonCardBase *LibInit(
    __REGD0(struct RadeonCardBase *base),
    __REGA0(APTR segList),
    __REGA6(struct ExecBase *sysBase));

static APTR FunctionTable[] USED = {
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibReserved,
    (APTR)FindCard,
    (APTR)InitCard,
    (APTR)-1
};

struct RadeonInitTable {
    ULONG BaseSize;
    APTR *Functions;
    APTR DataTable;
    APTR InitFunction;
};

static const struct RadeonInitTable InitTable USED = {
    sizeof(struct RadeonCardBase),
    FunctionTable,
    NULL,
    (APTR)LibInit
};

static const struct Resident RomTag USED = {
    RTC_MATCHWORD,
    (struct Resident *)&RomTag,
    (APTR)(&RomTag + 1),
    RTF_AUTOINIT,
    LIB_VERSION,
    NT_LIBRARY,
    0,
    (char *)LibName,
    (char *)LibIdString,
    (APTR)&InitTable
};

static struct RadeonCardBase *LibInit(
    __REGD0(struct RadeonCardBase *base),
    __REGA0(APTR segList),
    __REGA6(struct ExecBase *sysBase))
{
    struct ExecBase *SysBase = sysBase;

    if (!(SysBase->AttnFlags & AFF_68020)) {
        FreeMem((UBYTE *)base - base->Card.LibBase.lib_NegSize,
                (ULONG)base->Card.LibBase.lib_NegSize +
                    (ULONG)base->Card.LibBase.lib_PosSize);
        return NULL;
    }

    base->Card.LibBase.lib_Node.ln_Type = NT_LIBRARY;
    base->Card.LibBase.lib_Node.ln_Pri = -50;
    base->Card.LibBase.lib_Node.ln_Name = (char *)LibName;
    base->Card.LibBase.lib_Flags = LIBF_CHANGED | LIBF_SUMUSED;
    base->Card.LibBase.lib_Version = LIB_VERSION;
    base->Card.LibBase.lib_Revision = LIB_REVISION;
    base->Card.LibBase.lib_IdString = (char *)LibIdString;
    base->Card.ExecBase = SysBase;
    base->Card.SegList = segList;
    base->Card.Name = (char *)CardName;
    base->OpenPciBase = NULL;
    base->BoardInfo = NULL;
    OpenPciBase = NULL;

    return base;
}

static struct RadeonCardBase *LibOpen(
    __REGA6(struct RadeonCardBase *base))
{
    struct ExecBase *SysBase = base->Card.ExecBase;

    if (base->Card.LibBase.lib_OpenCnt == 0) {
        base->OpenPciBase = OpenLibrary((CONST_STRPTR)"openpci.library",
                                       MIN_OPENPCI_VERSION);
        if (!base->OpenPciBase)
            return NULL;
        OpenPciBase = base->OpenPciBase;
    }

    ++base->Card.LibBase.lib_OpenCnt;
    base->Card.LibBase.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    return base;
}

static LONG LibClose(__REGA6(struct RadeonCardBase *base))
{
    struct ExecBase *SysBase = base->Card.ExecBase;

    if (base->Card.LibBase.lib_OpenCnt == 0)
        return 0;

    if (--base->Card.LibBase.lib_OpenCnt == 0) {
        RadeonReleaseBoard(base);
        if (base->OpenPciBase) {
            CloseLibrary(base->OpenPciBase);
            base->OpenPciBase = NULL;
            OpenPciBase = NULL;
        }

        if (base->Card.LibBase.lib_Flags & LIBF_DELEXP)
            return (LONG)LibExpunge(base);
    }

    return 0;
}

static APTR LibExpunge(__REGA6(struct RadeonCardBase *base))
{
    struct ExecBase *SysBase = base->Card.ExecBase;
    APTR segList;
    ULONG size;

    if (base->Card.LibBase.lib_OpenCnt) {
        base->Card.LibBase.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }

    RadeonReleaseBoard(base);
    if (base->OpenPciBase) {
        CloseLibrary(base->OpenPciBase);
        base->OpenPciBase = NULL;
        OpenPciBase = NULL;
    }

    segList = base->Card.SegList;
    size = (ULONG)base->Card.LibBase.lib_NegSize +
           (ULONG)base->Card.LibBase.lib_PosSize;
    Remove((struct Node *)base);
    FreeMem((UBYTE *)base - base->Card.LibBase.lib_NegSize, size);
    return segList;
}

static LONG LibReserved(void)
{
    return 0;
}
