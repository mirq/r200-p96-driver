#include <exec/types.h>

void *memcpy(void *destination, const void *source, unsigned long bytes);
void *memset(void *destination, int value, unsigned long bytes);

/* GCC may lower large structure copies/clears to these calls even in a
 * freestanding build. The chip intentionally does not link a C runtime. */
void *memcpy(void *destination, const void *source, unsigned long bytes)
{
    UBYTE *out=(UBYTE *)destination;
    const UBYTE *in=(const UBYTE *)source;
    while (bytes--) *out++=*in++;
    return destination;
}

void *memset(void *destination, int value, unsigned long bytes)
{
    UBYTE *out=(UBYTE *)destination;
    while (bytes--) *out++=(UBYTE)value;
    return destination;
}
