#define __NOLIBBASE__

#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <proto/exec.h>

#include "radeon9200.h"

#define LIB_VERSION 3
#define LIB_REVISION 0

#define USED __attribute__((used))

static const char LibName[] = "Radeon9200.chip";
static const char LibIdString[] =
    "Radeon9200.chip 3.0 (27.8.2026)\r\n";
static const char VerTag[] USED =
    "\0$VER: Radeon9200.chip 3.0 (27.8.2026)";

struct Library *PrometheusBase;

static struct RadeonChipBase *LibOpen(
    __REGA6(struct RadeonChipBase *base));
static LONG LibClose(__REGA6(struct RadeonChipBase *base));
static APTR LibExpunge(__REGA6(struct RadeonChipBase *base));
static LONG LibReserved(void);
static struct RadeonChipBase *LibInit(
    __REGD0(struct RadeonChipBase *base),
    __REGA0(APTR segList),
    __REGA6(struct ExecBase *sysBase));

static APTR FunctionTable[] USED = {
    (APTR)LibOpen,
    (APTR)LibClose,
    (APTR)LibExpunge,
    (APTR)LibReserved,
    (APTR)InitChip,
    (APTR)InitRadeonFeatures,
    (APTR)Radeon3DOpen,
    (APTR)Radeon3DClose,
    (APTR)Radeon3DGetInfo,
    (APTR)Radeon3DDetachOwner,
    (APTR)Radeon3DSubmit,
    (APTR)Radeon3DTestFence,
    (APTR)Radeon3DWaitFence,
    (APTR)Radeon3DImportBitMap,
    (APTR)Radeon3DReleaseSurface,
    (APTR)Radeon3DExecute,
    (APTR)Radeon3DInvalidateForTest,
    (APTR)Radeon3DAllocSegment,
    (APTR)Radeon3DFreeSegment,
    (APTR)Radeon3DCommitDraw,
    (APTR)Radeon3DCommitBatch,
    (APTR)Radeon3DCommitStateBatch,
    (APTR)-1
};

struct RadeonInitTable {
    ULONG BaseSize;
    APTR *Functions;
    APTR DataTable;
    APTR InitFunction;
};

static const struct RadeonInitTable InitTable USED = {
    sizeof(struct RadeonChipBase),
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

static struct RadeonChipBase *LibInit(
    __REGD0(struct RadeonChipBase *base),
    __REGA0(APTR segList),
    __REGA6(struct ExecBase *sysBase))
{
    struct ExecBase *SysBase = sysBase;

    if (!(SysBase->AttnFlags & AFF_68020)) {
        FreeMem((UBYTE *)base - base->Library.lib_NegSize,
                (ULONG)base->Library.lib_NegSize +
                    (ULONG)base->Library.lib_PosSize);
        return NULL;
    }

    base->Library.lib_Node.ln_Type = NT_LIBRARY;
    base->Library.lib_Node.ln_Pri = -50;
    base->Library.lib_Node.ln_Name = (char *)LibName;
    base->Library.lib_Flags = LIBF_CHANGED | LIBF_SUMUSED;
    base->Library.lib_Version = LIB_VERSION;
    base->Library.lib_Revision = LIB_REVISION;
    base->Library.lib_IdString = (char *)LibIdString;
    base->Flags = 0;
    base->Pad = 0;
    base->ExecBase = SysBase;
    base->SegList = segList;
    base->PrometheusBase = NULL;
    base->BoardInfo = NULL;
    InitSemaphore(&base->ServiceLock);
    base->ServiceDevices.mlh_Head =
        (struct MinNode *)&base->ServiceDevices.mlh_Tail;
    base->ServiceDevices.mlh_Tail = NULL;
    base->ServiceDevices.mlh_TailPred =
        (struct MinNode *)&base->ServiceDevices.mlh_Head;
    base->RetiredServiceDevices.mlh_Head =
        (struct MinNode *)&base->RetiredServiceDevices.mlh_Tail;
    base->RetiredServiceDevices.mlh_Tail = NULL;
    base->RetiredServiceDevices.mlh_TailPred =
        (struct MinNode *)&base->RetiredServiceDevices.mlh_Head;
    base->ServiceGeneration = 1;
    base->ServiceSessions = 0;
    base->ServiceNextHandle = 0x80000000UL;
    base->ServiceState = RADEON3D_SERVICE_EMPTY;
    PrometheusBase = NULL;

    return base;
}

static struct RadeonChipBase *LibOpen(
    __REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base->ExecBase;

    if (base->Library.lib_OpenCnt == 0 && !base->PrometheusBase) {
        base->PrometheusBase = OpenLibrary(
            (CONST_STRPTR)"prometheus.library", 2);
        if (!base->PrometheusBase)
            return NULL;
        PrometheusBase = base->PrometheusBase;
    }

    ++base->Library.lib_OpenCnt;
    base->Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    return base;
}

static LONG LibClose(__REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base->ExecBase;

    if (base->Library.lib_OpenCnt == 0)
        return 0;

    if (--base->Library.lib_OpenCnt == 0) {
        if (base->ServiceSessions) {
            base->Library.lib_Flags |= LIBF_DELEXP;
            return 0;
        }
        (void)RadeonReleaseBoard(base, NULL, FALSE);
        if (base->PrometheusBase) {
            CloseLibrary(base->PrometheusBase);
            base->PrometheusBase = NULL;
            PrometheusBase = NULL;
        }

        if (base->Library.lib_Flags & LIBF_DELEXP)
            return (LONG)LibExpunge(base);
    }

    return 0;
}

static APTR LibExpunge(__REGA6(struct RadeonChipBase *base))
{
    struct ExecBase *SysBase = base->ExecBase;
    APTR segList;
    ULONG size;

    if (base->Library.lib_OpenCnt || base->ServiceSessions) {
        base->Library.lib_Flags |= LIBF_DELEXP;
        return NULL;
    }

    (void)RadeonReleaseBoard(base, NULL, FALSE);
    Radeon3DFreeRetiredDevices(base);
    if (base->PrometheusBase) {
        CloseLibrary(base->PrometheusBase);
        base->PrometheusBase = NULL;
        PrometheusBase = NULL;
    }

    segList = base->SegList;
    size = (ULONG)base->Library.lib_NegSize +
           (ULONG)base->Library.lib_PosSize;
    Remove((struct Node *)base);
    FreeMem((UBYTE *)base - base->Library.lib_NegSize, size);
    return segList;
}

static LONG LibReserved(void)
{
    return 0;
}
