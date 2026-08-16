#!/usr/bin/env python3
"""Decode the Radeon9200.Debug stats block from an amiga_inspect_memory dump.

Finding the block from the host:

    1. read the longword at 0x00000004                  -> SysBase
    2. read 16 bytes at SysBase + 392                   -> exec PortList,
                                                           lh_Type must be 4
    3. walk ln_Succ from lh_Head, reading 678 bytes per node, until the
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
    # Version 5
    "CompleteCalls", "CompleteHardware", "CompleteSoftware",
    "CompleteUnequalPitch", "CompleteOpcodeReject",
    "CompleteOverlapReject", "CompleteSurfaceSoftware",
    "CompleteSurfaceReject", "CompleteAccelUnavailable",
]

# Version 6: four parallel arrays indexed by OPS, appended in this order.
OPS = ["Fill", "Invert", "Copy", "Pattern", "Template", "Complete",
       "Line", "Drain"]
for _array in ("OpCalls", "OpHardware", "OpSoftware", "OpTicks"):
    FIELDS.extend("%s_%s" % (_array, _op) for _op in OPS)

# Version 7 / 8
FIELDS.extend(["VramWriteTicks", "MonoFromMemory", "MonoProbeSample",
                "MonoProbeSampleAlt"])

# Version 9
FIELDS.extend([
    "FifoWaitCalls", "FifoWaitPolls", "FifoWaitMaxPolls",
    "FifoWaitFailures", "IdleWaitCalls", "IdleWaitPolls",
    "IdleWaitMaxPolls", "IdleWaitFailures", "RecoveryCalls",
    "RecoverySuccess", "RecoveryFailure", "CompleteSubmitCalls",
    "CompleteSubmitSuccess", "LastWaitStatus", "LastWaitKind",
    "LastWaitPending", "FinalAccelState",
])

# Version 10
FIELDS.extend("CompleteOpcode_%X" % opcode for opcode in range(16))

# Version 11
FIELDS.extend([
    "CompleteValidateTicks", "CompleteSubmitTicks", "CompleteDefaultTicks",
    "CompleteValidateMaxTicks", "CompleteSubmitMaxTicks",
    "CompleteDefaultMaxTicks",
])

# Version 12
FIELDS.extend([
    "VramSmallBytes", "VramSmallTicks", "VramBurstBytes",
    "VramBurstTicks", "VramDrainValue",
])

# Version 13
FIELDS.extend([
    "CpProbeDwords", "CpBufferedTicks", "CpDirectTicks",
    "CpBufferedSuccess", "CpDirectSuccess",
])

# Version 14
FIELDS.extend([
    "CpWrapBefore", "CpWrapAfter", "CpWrapSuccess",
    "CpNearFullSuccess", "CpReserveTimeoutSuccess", "CpFirstFence",
    "CpSecondFence", "CpFenceOrderSuccess", "BoardLockChecks",
    "BoardLockOwned", "BoardLockOwnedByOther",
])

# Version 15
FIELDS.extend([
    "CpFenceZeroPollSuccess", "CpFenceZeroPollTicks",
    "CpFenceTimeoutSuccess", "CpFenceTimeoutTicks",
])

# Version 16
FIELDS.extend([
    "FallbackDrainSkipped", "FallbackDrainRequired", "FallbackProbeCalls",
    "FallbackProbeTicks", "FallbackProbeSuccess",
])

# Version 17
FIELDS.extend([
    "BlitRectBoundsSuccess", "BlitRectProbeCalls", "BlitRectProbeTicks",
])

# Version 18
FIELDS.extend(["SurfaceCacheHits", "SurfaceCacheMisses"])

KNOWN_VERSION = 18

PROBE = {0: "not run", 1: "SUPPORTED", 2: "wrong pixels",
         3: "submit failed", 4: "skipped"}


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
    elif available > len(FIELDS):
        print("# note: dump has %d unknown trailing fields\n" %
              (available - len(FIELDS)))

    if values.get("Version", 0) > KNOWN_VERSION:
        print("# warning: stats version %d is newer than decoder version %d\n" %
              (values["Version"], KNOWN_VERSION))

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
    if "MonoFromMemory" in values:
        print("\nmono-source-from-memory probe: %s  (first 4 dst bytes %08X)"
              % (PROBE.get(values["MonoFromMemory"], "?"),
                 values["MonoProbeSample"]))
        print("  MSB_TO_LSB %08X   LSB_TO_MSB %08X   (0xFF00FF00 = Amiga order)"
              % (values["MonoProbeSample"], values["MonoProbeSampleAlt"]))
    if values.get("VramWriteTicks"):
        vram = per(values["VramWriteTicks"], samples)
        mmio = per(values["MmioWriteTicks"], samples)
        print("VRAM write         %7.2f us  (%.1fx cheaper than an MMIO write)"
              % (vram, mmio / vram if vram else 0.0))
    for label, byte_field, tick_field in (
            ("VRAM small", "VramSmallBytes", "VramSmallTicks"),
            ("VRAM burst", "VramBurstBytes", "VramBurstTicks")):
        byte_count = values.get(byte_field, 0)
        ticks = values.get(tick_field, 0)
        if byte_count and ticks:
            mibps = byte_count * rate / float(ticks) / (1024.0 * 1024.0)
            print("%-18s %7.2f MiB/s  (%d bytes, %.2f us)" %
                  (label, mibps, byte_count, ticks * us))
    if values.get("CpProbeDwords"):
        print("CP buffered        %7.2f us  success=%d" %
              (values.get("CpBufferedTicks", 0) * us,
               values.get("CpBufferedSuccess", 0)))
        print("CP direct          %7.2f us  success=%d" %
              (values.get("CpDirectTicks", 0) * us,
               values.get("CpDirectSuccess", 0)))
    if "CpWrapSuccess" in values:
        print("CP wrap            success=%d  wptr=%d -> %d" %
              (values["CpWrapSuccess"], values["CpWrapBefore"],
               values["CpWrapAfter"]))
        print("CP reserve         near-full=%d timeout=%d" %
              (values["CpNearFullSuccess"],
               values["CpReserveTimeoutSuccess"]))
        print("CP fence order     success=%d  %d -> %d" %
              (values["CpFenceOrderSuccess"], values["CpFirstFence"],
               values["CpSecondFence"]))
        if "CpFenceTimeoutTicks" in values:
            print("CP fence zero      success=%d  %.3f ms" %
                  (values["CpFenceZeroPollSuccess"],
                   values["CpFenceZeroPollTicks"] * us / 1000.0))
            print("CP fence timeout   success=%d  %.3f ms (requested 20 ms)" %
                  (values["CpFenceTimeoutSuccess"],
                   values["CpFenceTimeoutTicks"] * us / 1000.0))
        checks = values["BoardLockChecks"]
        unowned = max(checks - values["BoardLockOwned"] -
                      values["BoardLockOwnedByOther"], 0)
        print("BoardLock          checks=%d current=%d other=%d unowned=%d" %
              (checks, values["BoardLockOwned"],
               values["BoardLockOwnedByOther"], unowned))
    if "FallbackDrainSkipped" in values:
        print("fallback drains     skipped=%d required=%d" %
              (values["FallbackDrainSkipped"],
               values["FallbackDrainRequired"]))
        if values.get("FallbackProbeCalls"):
            print("fallback probe      calls=%d %.3f us/call success=%d" %
                  (values["FallbackProbeCalls"],
                   per(values["FallbackProbeTicks"],
                       values["FallbackProbeCalls"]),
                   values["FallbackProbeSuccess"]))
    if "BlitRectBoundsSuccess" in values:
        print("BlitRect bounds    success=%d" %
              values["BlitRectBoundsSuccess"])
        if values.get("BlitRectProbeCalls"):
            print("BlitRect validate  calls=%d %.3f us/call" %
                  (values["BlitRectProbeCalls"],
                   per(values["BlitRectProbeTicks"],
                       values["BlitRectProbeCalls"])))
    if "SurfaceCacheHits" in values:
        print("surface cache      hits=%d misses=%d" %
              (values["SurfaceCacheHits"], values["SurfaceCacheMisses"]))

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

    complete = values.get("CompleteCalls", 0)
    if complete:
        accounted = (values["CompleteHardware"] +
                     values["CompleteSoftware"])
        print("Complete n=%-6d hw=%d sw=%d dropped=%d  "
              "unequal-pitch=%d opcode=%d overlap=%d surface-sw=%d "
              "surface-reject=%d accel-off=%d"
              % (complete, values["CompleteHardware"],
                 values["CompleteSoftware"], complete - accounted,
                 values["CompleteUnequalPitch"],
                 values["CompleteOpcodeReject"],
                 values["CompleteOverlapReject"],
                 values["CompleteSurfaceSoftware"],
                 values["CompleteSurfaceReject"],
                 values["CompleteAccelUnavailable"]))

    if "OpCalls_Fill" not in values:
        return
    print("\n--- per-callback (version 6) ---")
    print("%-10s %8s %8s %8s %10s %10s" %
          ("callback", "calls", "hw", "sw", "us/call", "total ms"))
    total = 0.0
    for op in OPS:
        calls = values["OpCalls_%s" % op]
        if not calls:
            continue
        gross = values["OpTicks_%s" % op] / float(calls)
        # Two ReadEClock calls bracket each sample; charge one call's latency.
        net = max(gross - clock_ticks, 0.0)
        spent = net * calls * us / 1000.0
        total += spent
        print("%-10s %8d %8d %8d %10.2f %10.1f" %
              (op, calls, values["OpHardware_%s" % op],
               values["OpSoftware_%s" % op], net * us, spent))
    print("%-10s %8s %8s %8s %10s %10.1f" %
          ("TOTAL", "", "", "", "", total))
    if "FifoWaitCalls" in values:
        print("\n--- bounded waits (version 9) ---")
        print("FIFO calls=%d polls=%d max=%d failures=%d" %
              (values["FifoWaitCalls"], values["FifoWaitPolls"],
               values["FifoWaitMaxPolls"], values["FifoWaitFailures"]))
        print("idle calls=%d polls=%d max=%d failures=%d" %
              (values["IdleWaitCalls"], values["IdleWaitPolls"],
               values["IdleWaitMaxPolls"], values["IdleWaitFailures"]))
        print("recovery calls=%d success=%d failure=%d final-state=%d" %
              (values["RecoveryCalls"], values["RecoverySuccess"],
               values["RecoveryFailure"], values["FinalAccelState"]))
        print("complete submits=%d success=%d" %
              (values["CompleteSubmitCalls"],
               values["CompleteSubmitSuccess"]))
        print("last failure kind=%d pending=%d RBBM_STATUS=%08X" %
              (values["LastWaitKind"], values["LastWaitPending"],
               values["LastWaitStatus"]))
    if "CompleteOpcode_0" in values:
        used = [(opcode, values["CompleteOpcode_%X" % opcode])
                for opcode in range(16)
                if values["CompleteOpcode_%X" % opcode]]
        print("complete opcodes: %s" % " ".join(
            "%X=%d" % pair for pair in used))
    if "CompleteValidateTicks" in values:
        print("complete phase ms: validate=%.3f submit=%.3f default=%.3f" %
              (values["CompleteValidateTicks"] * us / 1000.0,
               values["CompleteSubmitTicks"] * us / 1000.0,
               values["CompleteDefaultTicks"] * us / 1000.0))
        print("complete phase max us: validate=%.2f submit=%.2f default=%.2f" %
              (values["CompleteValidateMaxTicks"] * us,
               values["CompleteSubmitMaxTicks"] * us,
               values["CompleteDefaultMaxTicks"] * us))


if __name__ == "__main__":
    main()
