#ifndef PROMETHEUS_RADEON_H
#define PROMETHEUS_RADEON_H

#include <exec/types.h>

#define PROM_RADEON_HANDOFF_MAGIC 0x50524d52UL

/* Reserved is an existing UWORD in the handoff ABI. Keep output selection
 * within it so Prometheus.card and Radeon9200.chip remain size-compatible. */
#define PROM_RADEON_OUTPUT_MASK 0x0003U
#define PROM_RADEON_OUTPUT_VGA  0x0000U
#define PROM_RADEON_OUTPUT_DVI  0x0001U

struct PrometheusRadeonHandoff {
    ULONG Magic;
    APTR Board;
    APTR RomBase;
    ULONG RomSize;
    ULONG FramebufferSize;
    ULONG MmioSize;
    UWORD DeviceId;
    UWORD Reserved;
};

#define PROM_RADEON_FEATURE_CP         (1UL << 0)
#define PROM_RADEON_FEATURE_HWSPRITE   (1UL << 1)
#define PROM_RADEON_FEATURE_HWTEXT     (1UL << 2)
#define PROM_RADEON_FEATURE_TEXTSTAGE  (1UL << 3)

#endif
