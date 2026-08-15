#ifndef PROTO_RADEON3D_H
#define PROTO_RADEON3D_H

#include <exec/libraries.h>
#include <clib/radeon3d_protos.h>

#ifndef __NOLIBBASE__
extern struct Library *Radeon9200Base;
#endif

#if defined(NO_INLINE_LIBCALLS)
#elif defined(__GNUC__)
#include <inline/radeon3d.h>
#elif defined(__VBCC__)
#include <inline/radeon3d_protos.h>
#endif

#endif
