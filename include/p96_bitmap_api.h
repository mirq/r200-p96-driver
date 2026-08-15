#ifndef P96_BITMAP_API_H
#define P96_BITMAP_API_H

#include <exec/libraries.h>
#include <graphics/gfx.h>
#include <libraries/Picasso96.h>

extern struct Library *P96Base;

#ifdef __VBCC__
#include <inline/Picasso96_protos.h>
#else
#include <inline/macros.h>

#define p96AllocBitMap(sizeX, sizeY, depth, flags, friendBitmap, rgbFormat) \
    LP6(0x1e, struct BitMap *, p96AllocBitMap, \
        ULONG, sizeX, d0, ULONG, sizeY, d1, ULONG, depth, d2, \
        ULONG, flags, d3, struct BitMap *, friendBitmap, a0, \
        RGBFTYPE, rgbFormat, d7, , P96Base)

#define p96FreeBitMap(bitmap) \
    LP1NR(0x24, p96FreeBitMap, struct BitMap *, bitmap, a0, , P96Base)

#define p96GetBitMapAttr(bitmap, attribute) \
    LP2(0x2a, ULONG, p96GetBitMapAttr, struct BitMap *, bitmap, a0, \
        ULONG, attribute, d0, , P96Base)

#define p96BestModeIDTagList(tags) \
    LP1(0x3c, ULONG, p96BestModeIDTagList, struct TagItem *, tags, a0, , \
        P96Base)

#define p96GetModeIDAttr(mode, attribute) \
    LP2(0x54, ULONG, p96GetModeIDAttr, ULONG, mode, d0, \
        ULONG, attribute, d1, , P96Base)

#define p96OpenScreenTagList(tags) \
    LP1(0x5a, struct Screen *, p96OpenScreenTagList, struct TagItem *, tags, \
        a0, , P96Base)

#define p96CloseScreen(screen) \
    LP1(0x60, BOOL, p96CloseScreen, struct Screen *, screen, a0, , P96Base)
#endif

#define P96_BITMAP_WIDTH          0UL
#define P96_BITMAP_HEIGHT         1UL
#define P96_BITMAP_MEMORY         3UL
#define P96_BITMAP_BYTES_PER_ROW  4UL
#define P96_BITMAP_BYTES_PER_PIXEL 5UL
#define P96_BITMAP_RGB_FORMAT     7UL
#define P96_BITMAP_IS_P96         8UL
#define P96_BITMAP_IS_ON_BOARD    9UL

#define P96_MODE_WIDTH      0UL
#define P96_MODE_HEIGHT     1UL
#define P96_MODE_DEPTH      2UL
#define P96_MODE_RGB_FORMAT 5UL
#define P96_MODE_IS_P96     6UL

#define P96_BESTMODE_DUMMY          (TAG_USER + 96UL)
#define P96_BESTMODE_FORMATS_ALLOWED (P96_BESTMODE_DUMMY + 1UL)
#define P96_BESTMODE_NOMINAL_WIDTH  (P96_BESTMODE_DUMMY + 3UL)
#define P96_BESTMODE_NOMINAL_HEIGHT (P96_BESTMODE_DUMMY + 4UL)
#define P96_BESTMODE_DEPTH          (P96_BESTMODE_DUMMY + 5UL)

#define P96_SCREEN_DUMMY      (TAG_USER + 0x20000UL + 96UL)
#define P96_SCREEN_DEPTH      (P96_SCREEN_DUMMY + 5UL)
#define P96_SCREEN_DISPLAY_ID (P96_SCREEN_DUMMY + 0x12UL)
#define P96_SCREEN_SHOW_TITLE (P96_SCREEN_DUMMY + 0x14UL)
#define P96_SCREEN_BEHIND     (P96_SCREEN_DUMMY + 0x15UL)
#define P96_SCREEN_QUIET      (P96_SCREEN_DUMMY + 0x16UL)
#define P96_SCREEN_RGB_FORMAT (P96_SCREEN_DUMMY + 0x1dUL)

#endif
