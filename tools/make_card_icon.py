#!/usr/bin/env python3
"""Generate an AmigaOS monitor icon carrying Picasso96 ToolTypes.

On the validated setup, Picasso96 passes the ToolTypes array from the active
monitor icon DEVS:Monitors/Radeon.info to InitCard(). An icon beside the card
binary in LIBS:Picasso96 is not the active configuration source.

The file layout is the classic icon.library DiskObject stream, all big-endian:

    DiskObject          78 bytes
    Image               20 bytes   (present because do_Gadget.GadgetRender != 0)
    image planes        depth * 2 * ceil(width/16) * height bytes
    ToolTypes block     LONG (n+1)*4, then n * (LONG len, len bytes)

Pointer fields in the stream are only non-NULL flags; icon.library overwrites
them with real addresses on load.

Usage:
    python3 tools/make_card_icon.py Radeon.info BOARDTYPE=Radeon9200 \
        OUTPUT=VGA DMASIZE=2M CP=YES HWSPRITE=YES
"""

import struct
import sys

MAGIC = 0xE310
VERSION = 1

WBTOOL = 3
NO_ICON_POSITION = 0x80000000

GADGIMAGE = 0x0004
GADGIMMEDIATE = 0x0001
RELVERIFY = 0x0002
BOOLGADGET = 0x0001

ICON_WIDTH = 24
ICON_HEIGHT = 22
ICON_DEPTH = 1


def icon_bitmap():
    """A plain framed box: valid, tiny, and obviously a placeholder."""
    words_per_row = (ICON_WIDTH + 15) // 16
    rows = []
    for y in range(ICON_HEIGHT):
        if y in (0, ICON_HEIGHT - 1):
            bits = (1 << ICON_WIDTH) - 1
        else:
            bits = (1 << (ICON_WIDTH - 1)) | 1
        # Left-justify into the padded word row.
        bits <<= words_per_row * 16 - ICON_WIDTH
        rows.append(bits.to_bytes(words_per_row * 2, "big"))
    return b"".join(rows)


def build(tooltypes):
    gadget = struct.pack(
        ">IhhhhHHHIIIiIHI",
        0,                              # NextGadget
        0, 0,                           # LeftEdge, TopEdge
        ICON_WIDTH, ICON_HEIGHT,        # Width, Height
        GADGIMAGE,                      # Flags
        GADGIMMEDIATE | RELVERIFY,      # Activation
        BOOLGADGET,                     # GadgetType
        1,                              # GadgetRender (non-NULL flag)
        0,                              # SelectRender
        0,                              # GadgetText
        0,                              # MutualExclude
        0,                              # SpecialInfo
        0,                              # GadgetID
        0,                              # UserData
    )
    assert len(gadget) == 44, len(gadget)

    disk_object = struct.pack(">HH", MAGIC, VERSION) + gadget + struct.pack(
        ">BBIIIIIII",
        WBTOOL,                         # do_Type
        0,                              # pad
        0,                              # do_DefaultTool (none)
        1,                              # do_ToolTypes (non-NULL flag)
        NO_ICON_POSITION,               # do_CurrentX
        NO_ICON_POSITION,               # do_CurrentY
        0,                              # do_DrawerData
        0,                              # do_ToolWindow
        4096,                           # do_StackSize
    )
    assert len(disk_object) == 78, len(disk_object)

    planes = icon_bitmap()
    image = struct.pack(
        ">hhhhhIBBI",
        0, 0,                           # LeftEdge, TopEdge
        ICON_WIDTH, ICON_HEIGHT,        # Width, Height
        ICON_DEPTH,                     # Depth
        1,                              # ImageData (non-NULL flag)
        (1 << ICON_DEPTH) - 1,          # PlanePick
        0,                              # PlaneOnOff
        0,                              # NextImage
    )
    assert len(image) == 20, len(image)

    block = struct.pack(">I", (len(tooltypes) + 1) * 4)
    for entry in tooltypes:
        raw = entry.encode("latin-1") + b"\0"
        block += struct.pack(">I", len(raw)) + raw

    return disk_object + image + planes + block


def parse(data):
    """Re-read the stream we just wrote and recover the ToolTypes."""
    magic, version = struct.unpack_from(">HH", data, 0)
    if magic != MAGIC:
        raise ValueError("bad magic %04x" % magic)
    gadget_render = struct.unpack_from(">I", data, 4 + 18)[0]
    tool_types_ptr = struct.unpack_from(">I", data, 78 - 24)[0]
    default_tool_ptr = struct.unpack_from(">I", data, 78 - 28)[0]

    offset = 78
    if gadget_render:
        depth, = struct.unpack_from(">h", data, offset + 8)
        width, height = struct.unpack_from(">hh", data, offset + 4)
        offset += 20 + depth * 2 * ((width + 15) // 16) * height
    if default_tool_ptr:
        length, = struct.unpack_from(">I", data, offset)
        offset += 4 + length

    entries = []
    if tool_types_ptr:
        count, = struct.unpack_from(">I", data, offset)
        offset += 4
        for _ in range(count // 4 - 1):
            length, = struct.unpack_from(">I", data, offset)
            offset += 4
            entries.append(data[offset:offset + length - 1].decode("latin-1"))
            offset += length
    if offset != len(data):
        raise ValueError("trailing bytes: %d != %d" % (offset, len(data)))
    return version, entries


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path, tooltypes = sys.argv[1], sys.argv[2:]
    data = build(tooltypes)
    version, recovered = parse(data)
    if recovered != tooltypes:
        raise SystemExit("round-trip mismatch: %r" % (recovered,))
    with open(path, "wb") as handle:
        handle.write(data)
    print("wrote %s (%d bytes), DiskObject v%d" % (path, len(data), version))
    for entry in recovered:
        print("  ToolType: %s" % entry)


if __name__ == "__main__":
    main()
