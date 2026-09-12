#include <exec/types.h>
#include <exec/ports.h>
#include <proto/exec.h>
#include <stdio.h>
#include "../src/radeon_debug.h"

#define STATS_LONGS (sizeof(struct RadeonDebugStats) / sizeof(ULONG))

int main(void)
{
    struct MsgPort *port;
    struct RadeonDebugStats snapshot;
    const ULONG *stats;
    ULONG index;

    Forbid();
    port = FindPort((CONST_STRPTR)RADEON_DEBUG_PORT);
    if (!port) {
        Permit();
        printf("DBGDUMP status=no_port\n");
        return 20;
    }
    stats = (const ULONG *)((const UBYTE *)port + sizeof(*port));
    if (stats[0] != RADEON_DEBUG_MAGIC || stats[1] != RADEON_DEBUG_VERSION) {
        Permit();
        printf("DBGDUMP status=version_mismatch\n");
        return 20;
    }
    CopyMem((APTR)stats, &snapshot, sizeof(snapshot));
    Permit();
    stats = (const ULONG *)&snapshot;
    printf("DBGDUMP port=%08lx\n", (unsigned long)port);
    for (index = 0; index < STATS_LONGS; ++index)
        printf("%08lx\n", (unsigned long)stats[index]);
    return 0;
}
