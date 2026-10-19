#ifndef RADEON3D_EMIT_H
#define RADEON3D_EMIT_H

/* Shared float-bit validators used by the service's stream validation
 * (Radeon3DSubmit's immediate triangle list) and by host-side fixtures.
 * The semantic record emitter that this header once declared was removed
 * with the record path; only these helpers remain. */

#include <exec/types.h>

#include "radeon3d.h"

static inline ULONG UnsignedFloatBits(ULONG value)
{
    ULONG top = 0;
    ULONG scan = value;

    if (!value)
        return 0;
    while (scan >>= 1)
        ++top;
    return ((127UL + top) << 23) |
           ((value << (23UL - top)) & 0x007fffffUL);
}

static inline ULONG UnsignedHalfFloatBits(ULONG value)
{
    ULONG bits = UnsignedFloatBits(value);
    return bits ? bits - (1UL << 23) : 0;
}

static inline BOOL ValidFloat(ULONG bits)
{
    return (bits & 0x7f800000UL) != 0x7f800000UL;
}

static inline BOOL ValidPositiveFloat(ULONG bits)
{
    return bits != 0 && !(bits & 0x80000000UL) &&
           (bits & 0x7f800000UL) != 0x7f800000UL;
}

static inline BOOL ValidScreenCoordinate(ULONG bits, ULONG limit)
{
    return !(bits & 0x80000000UL) &&
           (bits & 0x7f800000UL) != 0x7f800000UL &&
           bits <= UnsignedFloatBits(limit);
}

#endif
