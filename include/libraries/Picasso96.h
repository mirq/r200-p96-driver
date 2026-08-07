/*
 * Minimal Picasso96 public definitions required by CardDevelop 3.6.
 * The local CardDevelop archive contains boardinfo.h but omits this public
 * include. Values and layouts match the Picasso96 developer header.
 * Original Picasso96.h copyright 1996-1998 Alexander Kneer and Tobias Abt.
 */
#ifndef LIBRARIES_PICASSO96_H
#define LIBRARIES_PICASSO96_H

#include <exec/nodes.h>
#include <exec/types.h>
#include <utility/tagitem.h>

typedef enum {
    RGBFB_NONE,
    RGBFB_CLUT,
    RGBFB_R8G8B8,
    RGBFB_B8G8R8,
    RGBFB_R5G6B5PC,
    RGBFB_R5G5B5PC,
    RGBFB_A8R8G8B8,
    RGBFB_A8B8G8R8,
    RGBFB_R8G8B8A8,
    RGBFB_B8G8R8A8,
    RGBFB_R5G6B5,
    RGBFB_R5G5B5,
    RGBFB_B5G6R5PC,
    RGBFB_B5G5R5PC,
    RGBFB_YUV422CGX,
    RGBFB_YUV411,
    RGBFB_YUV411PC,
    RGBFB_YUV422,
    RGBFB_YUV422PC,
    RGBFB_YUV422PA,
    RGBFB_YUV422PAPC,
    RGBFB_MaxFormats
} RGBFTYPE;

#define RGBFF_NONE       (1UL << RGBFB_NONE)
#define RGBFF_CLUT       (1UL << RGBFB_CLUT)
#define RGBFF_R8G8B8     (1UL << RGBFB_R8G8B8)
#define RGBFF_B8G8R8     (1UL << RGBFB_B8G8R8)
#define RGBFF_R5G6B5PC   (1UL << RGBFB_R5G6B5PC)
#define RGBFF_R5G5B5PC   (1UL << RGBFB_R5G5B5PC)
#define RGBFF_A8R8G8B8   (1UL << RGBFB_A8R8G8B8)
#define RGBFF_A8B8G8R8   (1UL << RGBFB_A8B8G8R8)
#define RGBFF_R8G8B8A8   (1UL << RGBFB_R8G8B8A8)
#define RGBFF_B8G8R8A8   (1UL << RGBFB_B8G8R8A8)
#define RGBFF_R5G6B5     (1UL << RGBFB_R5G6B5)
#define RGBFF_R5G5B5     (1UL << RGBFB_R5G5B5)
#define RGBFF_B5G6R5PC   (1UL << RGBFB_B5G6R5PC)
#define RGBFF_B5G5R5PC   (1UL << RGBFB_B5G5R5PC)
#define RGBFF_YUV422CGX  (1UL << RGBFB_YUV422CGX)
#define RGBFF_YUV411     (1UL << RGBFB_YUV411)
#define RGBFF_YUV411PC   (1UL << RGBFB_YUV411PC)
#define RGBFF_YUV422     (1UL << RGBFB_YUV422)
#define RGBFF_YUV422PC   (1UL << RGBFB_YUV422PC)
#define RGBFF_YUV422PA   (1UL << RGBFB_YUV422PA)
#define RGBFF_YUV422PAPC (1UL << RGBFB_YUV422PAPC)

#define RGBFF_HICOLOR \
    (RGBFF_R5G6B5PC | RGBFF_R5G5B5PC | RGBFF_R5G6B5 | RGBFF_R5G5B5 | \
     RGBFF_B5G6R5PC | RGBFF_B5G5R5PC)
#define RGBFF_TRUECOLOR (RGBFF_R8G8B8 | RGBFF_B8G8R8)
#define RGBFF_TRUEALPHA \
    (RGBFF_A8R8G8B8 | RGBFF_A8B8G8R8 | RGBFF_R8G8B8A8 | RGBFF_B8G8R8A8)

struct RenderInfo {
    APTR Memory;
    WORD BytesPerRow;
    WORD pad;
    RGBFTYPE RGBFormat;
};

#define P96BD_Dummy (TAG_USER + 0x50000UL + 96UL)

#endif
