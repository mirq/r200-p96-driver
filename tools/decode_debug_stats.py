#!/usr/bin/env python3
"""Decode the Radeon9200.Debug stats block from an amiga_inspect_memory dump.

Finding the block from the host:

    1. read the longword at 0x00000004                  -> SysBase
    2. read 16 bytes at SysBase + 392                   -> exec PortList,
                                                           lh_Type must be 4
    3. walk ln_Succ from lh_Head, reading 160 bytes per node, until the
       magic 'R92D' appears at node + 34 (= sizeof(struct MsgPort))

Then paste the dump here:

    python3 tools/decode_debug_stats.py dump.txt

The dump may be raw hex or the `addr  xx xx ..  ascii` form the bridge emits;
everything that is not a hex byte pair is ignored, and the block is located by
its magic rather than by a fixed offset.
"""

import re
import struct
import sys

MAGIC = 0x52393244

FIELDS = [
    "Magic", "Version", "CpRequested", "CpActive", "DmaRequested",
    "DmaReserved", "BoardMemorySize", "EClockRate", "MmioSamples",
    "MmioReadTicks", "MmioWriteTicks", "ClockTicks", "Reads", "Writes",
    "FillCount", "FillTicks", "FillReads", "FillWrites",
    "DrainCount", "DrainTicks", "DrainReads", "DrainWrites",
    # Version 2
    "FillCalls", "FillTotalTicks", "FillHardware", "FillSoftware",
    # Version 3
    "SpriteExperiment", "SetSpriteCalls", "SetSpritePositionCalls",
    "SetSpriteImageCalls", "SetSpriteColorCalls",
    # Version 4
    "TemplateCalls", "TemplateHardware", "TemplateSoftware",
    "TemplateCacheHits", "TemplateJam1", "TemplateJam2",
    "TemplateOtherMode", "TemplateWidthTotal", "TemplateUploadWords",
    "TemplateMaxWidth", "TemplateMaxHeight",
]


def load(path):
    text = open(path).read()
    out = bytearray()
    for line in text.splitlines():
        # Drop a leading address column and any trailing ASCII gutter.
        line = re.sub(r"^\s*[0-9a-fA-F]{6,8}[: ]", " ", line)
        line = re.sub(r"\s{2,}\S*$", "", line)
        for token in line.split():
            if re.fullmatch(r"[0-9a-fA-F]{2}", token):
                out.append(int(token, 16))
    return bytes(out)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data = load(sys.argv[1])

    start = data.find(struct.pack(">I", MAGIC))
    if start < 0:
        sys.exit("magic R92D not found in %d bytes" % len(data))
    available = (len(data) - start) // 4
    names = FIELDS[:available]
    values = dict(zip(names, struct.unpack_from(
        ">%dI" % len(names), data, start)))
    if len(names) < len(FIELDS):
        print("# note: dump holds %d of %d fields\n" % (len(names),
                                                        len(FIELDS)))

    for name in names:
        print("%-16s %10d  0x%08X" % (name, values[name], values[name]))

    rate = values["EClockRate"]
    if not rate:
        print("\nno timer, cannot derive microseconds")
        return
    us = 1000000.0 / rate

    def per(ticks, count):
        return (ticks / float(count)) * us if count else 0.0

    samples = values["MmioSamples"]
    clock_us = per(values["ClockTicks"], samples)
    print("\n--- derived (1 tick = %.4f us) ---" % us)
    print("MMIO read          %7.2f us" % per(values["MmioReadTicks"], samples))
    print("MMIO write         %7.2f us" % per(values["MmioWriteTicks"], samples))
    print("ReadEClock         %7.2f us  (instrumentation cost)" % clock_us)

    # Each sampled interval brackets the work with two ReadEClock calls and so
    # carries roughly one call's latency; subtract it before reporting.
    clock_ticks = values["ClockTicks"] / float(samples) if samples else 0.0
    for kind in ("Fill", "Drain"):
        count = values.get(kind + "Count", 0)
        if not count:
            continue
        gross = values[kind + "Ticks"] / float(count)
        net = max(gross - clock_ticks, 0.0)
        print("%-6s n=%-6d %7.2f us net (%.2f us gross)  "
              "%.2f reads %.2f writes per call"
              % (kind, count, net * us, gross * us,
                 values[kind + "Reads"] / float(count),
                 values[kind + "Writes"] / float(count)))

    calls = values.get("FillCalls", 0)
    if calls:
        # The outer interval also contains the inner sample's two calls.
        gross = values["FillTotalTicks"] / float(calls)
        net = max(gross - 3.0 * clock_ticks, 0.0)
        print("Call   n=%-6d %7.2f us net (%.2f us gross)  "
              "hw=%d sw=%d (%.1f%% software)"
              % (calls, net * us, gross * us, values["FillHardware"],
                 values["FillSoftware"],
                 100.0 * values["FillSoftware"] / float(calls)))


if __name__ == "__main__":
    main()
