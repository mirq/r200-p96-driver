#!/usr/bin/env python3
"""Frame-time visualizer for Radeon9200 driver timing captures.

Inputs (one or more text files, e.g. saved `radeon3dinfo` output):

    R3DCLOCK hz=709379 entries=1024 seq=738
    R3DSAMPLE seq=1 wall=45234 type=1 ok=1 in=4096 out=5310 copy=0 build=9821 submit=412

Each R3DSAMPLE is one submission (Execute / CommitDraw / CommitBatch /
StateBatch / raw Submit) with raw EClock ticks for the copy, build and
submit phases. The tool groups submissions into frames by wall-clock gaps
and attributes driver CPU time per phase, so you can see which phase
dominates a frame and whether the driver or the engine is the bottleneck.

Also understands 2D DEBUG captures: `--debug dump.txt` takes the raw hex
dump consumed by decode_debug_stats.py and shows per-callback time shares.

Usage:

    python3 tools/perfplot.py capture.txt
    python3 tools/perfplot.py capture.txt --out frames.png
    python3 tools/perfplot.py capture.txt --frame-gap-ms 2 --no-frames
    python3 tools/perfplot.py --debug debugdump.txt

matplotlib is optional; without it the tool prints an ASCII rendering.
"""

import argparse
import os
import re
import struct
import sys

DESCRIPTION = "Visualize Radeon9200 per-submission timing samples."

TYPE_NAMES = {
    1: "Execute",
    2: "CommitDraw",
    3: "CommitBatch",
    4: "StateBatch",
    5: "Submit",
}

DEBUG_OPS = ["Fill", "Invert", "Copy", "Pattern", "Template", "Complete",
             "Line", "Drain"]


def parse_u32_delta(later, earlier):
    """Unsigned 32-bit EClock low-word delta."""
    return (later - earlier) & 0xFFFFFFFF


def parse_samples(paths):
    """Return (samples, sequence_breaks).

    A run starts at each R3DCLOCK line; following R3DSAMPLE lines belong to
    that run until the next R3DCLOCK. Multiple files are concatenated.
    """
    clock_re = re.compile(
        r"R3DCLOCK\s+hz=(\d+)\s+entries=(\d+)\s+seq=(\d+)")
    sample_re = re.compile(
        r"R3DSAMPLE\s+seq=(\d+)\s+wall=(\d+)\s+type=(\d+)\s+ok=(\d+)\s+"
        r"in=(\d+)\s+out=(\d+)\s+copy=(\d+)\s+build=(\d+)\s+submit=(\d+)")

    samples = []
    current_seq = None
    current_hz = 0
    run = -1
    broken = 0
    for path in paths:
        with open(path) as handle:
            for line in handle:
                match = clock_re.search(line)
                if match:
                    current_hz = int(match.group(1))
                    current_seq = None
                    run += 1
                    continue
                match = sample_re.search(line)
                if not match:
                    continue
                seq, wall, kind, ok, din, dout, copy, build, submit = \
                    (int(g) for g in match.groups())
                if current_seq is not None and seq != current_seq + 1:
                    broken += 1  # torn read or ring overlap between dumps
                current_seq = seq
                samples.append({
                    "file": os.path.basename(path),
                    "run": run, "hz": current_hz,
                    "seq": seq, "wall": wall, "type": kind, "ok": ok,
                    "in": din, "out": dout,
                    "copy": copy, "build": build, "submit": submit,
                })
    return samples, broken


def ticks_to_us(hz, ticks):
    return ticks * 1e6 / hz if hz else float(ticks)


def median_gap_us(samples):
    """Median wall gap between consecutive samples of a run."""
    ordered = samples
    gaps = [parse_u32_delta(b["wall"], a["wall"]) * 1e6 / a["hz"]
            for a, b in zip(ordered, ordered[1:]) if a["run"] == b["run"]]
    return percentile(gaps, 0.5)


def auto_gap_us(samples):
    """Adaptive frame threshold: 4x the median gap, floored at 2 ms.

    Submissions inside one frame arrive close together (sub-frame gaps);
    the present/swap gap between frames is much larger. The median gap
    estimates the sub-frame spacing, so 4x it separates frames without
    knowing the workload, while the floor keeps quiet workloads honest.
    An explicit --frame-gap-ms always wins.
    """
    return max(2000.0, 4.0 * median_gap_us(samples))


def segment_frames(samples, gap_us):
    """Split wall-ordered samples into frames at gaps above gap_us.

    Wall ticks are the raw 32-bit EClock low word; deltas are taken
    relative to the first sample, so a capture needs no special case for
    wrap-around as long as it spans less than one wrap (~100 minutes).
    """
    if not samples:
        return []
    ordered = samples
    origin = ordered[0]["wall"]
    run = ordered[0]["run"]
    run_offset = 0.0
    for index, sample in enumerate(ordered):
        if sample["run"] != run:
            run_offset = ordered[index - 1]["t"] + gap_us
            origin = sample["wall"]
            run = sample["run"]
        sample["t"] = run_offset + ticks_to_us(
            sample["hz"], parse_u32_delta(sample["wall"], origin))
    frames = []
    current = [ordered[0]]
    for previous, sample in zip(ordered, ordered[1:]):
        if sample["run"] != previous["run"] or \
                sample["t"] - previous["t"] > gap_us:
            frames.append(current)
            current = [sample]
        else:
            current.append(sample)
    frames.append(current)
    return frames


def segment_frames_by_marker(samples, marker_type):
    """Split samples into frames at each sample of marker_type.

    Robust when the client's inter-submission stalls are comparable to the
    inter-frame gap (e.g. gears waits for vblank between the clear and the
    first draw, so gaps alone cannot separate frames). Each frame starts
    at a marker sample, which for gears is the Execute clear record.
    """
    frames = []
    current = []
    ordered = samples
    origin = ordered[0]["wall"]
    run = ordered[0]["run"]
    run_offset = 0.0
    for index, sample in enumerate(ordered):
        if sample["run"] != run:
            run_offset = ordered[index - 1]["t"] + 1.0
            origin = sample["wall"]
            run = sample["run"]
        sample["t"] = run_offset + ticks_to_us(
            sample["hz"], parse_u32_delta(sample["wall"], origin))
    for sample in ordered:
        if current and (sample["run"] != current[-1]["run"] or
                        sample["type"] == marker_type):
            frames.append(current)
            current = [sample]
        else:
            current.append(sample)
    if current:
        frames.append(current)
    return frames


def summarize_frame(frame):
    hz = frame[0]["hz"]

    def phase_us(field):
        return sum(s[field] for s in frame) * 1e6 / hz

    return {
        "start": frame[0]["t"],
        "driver_us": phase_us("copy") + phase_us("build") +
                     phase_us("submit"),
        "copy_us": phase_us("copy"),
        "build_us": phase_us("build"),
        "submit_us": phase_us("submit"),
        "subs": len(frame),
        "in": sum(s["in"] for s in frame),
        "out": sum(s["out"] for s in frame),
        "failed": sum(1 for s in frame if not s["ok"]),
    }


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def print_run_summary(samples, broken):
    phases = {"copy": 0, "build": 0, "submit": 0}
    types = {}
    for sample in samples:
        for phase in phases:
            phases[phase] += sample[phase]
        name = TYPE_NAMES.get(sample["type"], "?%d" % sample["type"])
        count, failed = types.get(name, (0, 0))
        types[name] = (count + 1, failed + (0 if sample["ok"] else 1))

    rates = sorted(set(s["hz"] for s in samples))
    print("samples: %d, EClock %s Hz%s" % (
        len(samples), ", ".join(format(rate, ",") for rate in rates),
        ", %d sequence breaks" % broken if broken else ""))
    print("")
    print("%-12s %8s %8s %12s %12s %12s" %
          ("type", "count", "failed", "copy ms", "build ms", "submit ms"))
    for name, (count, failed) in sorted(types.items()):
        subset = [s for s in samples
                  if TYPE_NAMES.get(s["type"]) == name]
        print("%-12s %8d %8d %12.2f %12.2f %12.2f" % (
            name, count, failed,
            sum(ticks_to_us(s["hz"], s["copy"]) for s in subset) / 1e3,
            sum(ticks_to_us(s["hz"], s["build"]) for s in subset) / 1e3,
            sum(ticks_to_us(s["hz"], s["submit"]) for s in subset) / 1e3))
    total = sum(phases.values())
    if total:
        print("")
        print("phase totals: copy %.1f%%  build %.1f%%  submit %.1f%%" % (
            100.0 * phases["copy"] / total,
            100.0 * phases["build"] / total,
            100.0 * phases["submit"] / total))


def print_frame_summary(frames, gap_us):
    if not frames:
        return
    summaries = [summarize_frame(f) for f in frames]
    driver = [f["driver_us"] for f in summaries]
    periods = [b["start"] - a["start"] for a, b in zip(summaries,
                                                       summaries[1:])]
    print("")
    print("frames: %d (%s)" % (
        len(frames),
        "frame marker" if gap_us <= 0 else "gap threshold %.0f us" % gap_us))
    print("driver cpu per frame us: median %.1f  mean %.1f  "
          "p95 %.1f  max %.1f" % (
              percentile(driver, 0.5),
              sum(driver) / len(driver),
              percentile(driver, 0.95),
              max(driver)))
    if periods:
        median_period = percentile(periods, 0.5)
        idle = [p - d for p, d in zip(periods, driver)]
        print("frame period us: median %.1f  p95 %.1f  -> ~%.2f FPS"
              % (median_period, percentile(periods, 0.95),
                 1e6 / median_period if median_period > 0 else 0.0))
        print("non-driver time per frame: median %.1f us "
              "(%.0f%% of period)" % (
                  percentile(idle, 0.5),
                  100.0 * percentile(idle, 0.5) / max(median_period, 1.0)))
    worst = sorted(summaries, key=lambda f: -f["driver_us"])[:5]
    print("worst frames:")
    for frame in worst:
        print("  t=%9.0f us  driver %7.1f us  (build %6.1f submit %6.1f "
              "copy %5.1f)  subs=%d  dwords in/out %d/%d%s" % (
                  frame["start"], frame["driver_us"], frame["build_us"],
                  frame["submit_us"], frame["copy_us"], frame["subs"],
                  frame["in"], frame["out"],
                  "  FAILED SUBS: %d" % frame["failed"]
                  if frame["failed"] else ""))


def ascii_frames(frames, width=64):
    summaries = [summarize_frame(f) for f in frames]
    peak = max(f["driver_us"] for f in summaries) or 1.0
    print("")
    print("driver us per frame ('#' build, 'o' submit, '.' copy; "
          "%.1f us/col):" % (peak / width))
    for index, frame in enumerate(summaries):
        n_build = int(round(frame["build_us"] / peak * width))
        n_submit = int(round(frame["submit_us"] / peak * width))
        n_copy = int(round(frame["copy_us"] / peak * width))
        bar = ("#" * n_build + "o" * n_submit + "." * n_copy)[:width]
        bar = bar.ljust(width, "-")
        print("%5d %7.0f |%s|" % (index, frame["driver_us"], bar))


def build_figure(frames):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    summaries = [summarize_frame(f) for f in frames]
    indexes = list(range(1, len(summaries) + 1))
    fig, axes = plt.subplots(3, 1, figsize=(13, 10))

    axes[0].bar(indexes, [f["build_us"] for f in summaries],
                label="build", color="#4062bb")
    axes[0].bar(indexes, [f["submit_us"] for f in summaries],
                bottom=[f["build_us"] for f in summaries],
                label="submit", color="#59a5d8")
    axes[0].bar(indexes, [f["copy_us"] for f in summaries],
                bottom=[f["build_us"] + f["submit_us"]
                        for f in summaries],
                label="copy", color="#9ac6c5")
    axes[0].set_ylabel("driver CPU us")
    axes[0].set_title("driver time per frame by phase")
    axes[0].legend(loc="upper right", fontsize="small")

    starts = [f["start"] for f in summaries]
    periods = [b - a for a, b in zip(starts, starts[1:])]
    if periods:
        axes[1].plot(starts[1:], periods, ".", ms=4, color="#5b3758",
                     label="period (start-to-start)")
        axes[1].plot(starts[1:], [f["driver_us"] for f in summaries[1:]],
                     ".", ms=4, color="#c96567", label="driver cpu")
        axes[1].set_xlabel("t (us)")
        axes[1].set_ylabel("us")
        axes[1].set_title("frame period vs driver time "
                          "(the gap is engine/layers time)")
        axes[1].legend(fontsize="small")

    axes[2].hist([f["driver_us"] for f in summaries], bins=40,
                 color="#4062bb")
    axes[2].set_xlabel("driver CPU us per frame")
    axes[2].set_ylabel("frames")
    axes[2].set_title("frame driver-time distribution")

    fig.tight_layout()
    return fig


def load_debug_stats(path):
    """Reuse decode_debug_stats.py's field map for a 2D DEBUG dump."""
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
    import decode_debug_stats as dds

    data = dds.load(path)
    start = data.find(struct.pack(">I", dds.MAGIC))
    if start < 0:
        sys.exit("magic R92D not found in %s" % path)
    available = (len(data) - start) // 4
    names = dds.FIELDS[:available]
    return dict(zip(names, struct.unpack_from(">%dI" % len(names), data,
                                              start)))


def print_debug_summary(values):
    rate = values.get("EClockRate", 0)
    if not rate:
        print("no EClockRate in debug block")
        return
    us = 1e6 / rate
    clock_ticks = (values.get("ClockTicks", 0) /
                   float(values.get("MmioSamples", 1) or 1))
    print("")
    print("--- 2D per-callback (DEBUG block) ---")
    print("%-10s %9s %9s %11s %12s" %
          ("callback", "calls", "hw", "us/call", "total ms"))
    for op in DEBUG_OPS:
        calls = values.get("OpCalls_%s" % op, 0)
        if not calls:
            continue
        net = max(values["OpTicks_%s" % op] / float(calls) - clock_ticks,
                  0.0)
        print("%-10s %9d %9d %11.2f %12.1f" % (
            op, calls, values.get("OpHardware_%s" % op, 0),
            net * us, net * calls * us / 1e3))
    if values.get("ExecuteCalls"):
        calls = values["ExecuteCalls"]
        print("")
        print("3D execute: calls=%d per call us: build=%.2f submit=%.2f" % (
            calls,
            values.get("ExecuteBuildTicks", 0) * us / calls,
            values.get("ExecuteSubmitTicks", 0) * us / calls))


def main():
    parser = argparse.ArgumentParser(description=DESCRIPTION)
    parser.add_argument("capture", nargs="*",
                        help="radeon3dinfo output file(s) with R3DSAMPLE "
                             "lines")
    parser.add_argument("--debug",
                        help="raw Radeon9200.Debug hex dump for 2D "
                             "per-callback attribution")
    parser.add_argument("--frame-gap-ms", type=float, default=None,
                        help="wall-clock gap that starts a new frame "
                             "(default: adaptive, 4x the median gap "
                             "floored at 2 ms; 0 disables grouping)")
    parser.add_argument("--frame-marker", type=int, default=None,
                        metavar="TYPE",
                        help="split frames at each sample of this type "
                             "instead of by gap (e.g. 1 = Execute clear; "
                             "overrides --frame-gap-ms)")
    parser.add_argument("--no-frames", action="store_true",
                        help="skip frame segmentation and plots")
    parser.add_argument("--out", help="write a matplotlib PNG here")
    parser.add_argument("--show", action="store_true",
                        help="open a matplotlib window")
    args = parser.parse_args()

    if not args.capture and not args.debug:
        parser.error("need a capture file or --debug")

    if args.debug:
        print_debug_summary(load_debug_stats(args.debug))

    if not args.capture:
        return

    samples, broken = parse_samples(args.capture)
    if not samples:
        print("no R3DSAMPLE lines found in: %s" % ", ".join(args.capture))
        return
    print_run_summary(samples, broken)

    if args.no_frames or (args.frame_gap_ms is not None and
                          args.frame_gap_ms <= 0 and
                          args.frame_marker is None):
        return
    if args.frame_marker is not None:
        gap_us = 0.0
        frames = segment_frames_by_marker(samples, args.frame_marker)
    else:
        gap_us = (auto_gap_us(samples) if args.frame_gap_ms is None
                  else args.frame_gap_ms * 1e3)
        frames = segment_frames(samples, gap_us)
    print_frame_summary(frames, gap_us)
    ascii_frames(frames)

    if args.out or args.show:
        try:
            import matplotlib  # noqa: F401
        except ImportError:
            print("matplotlib not available; ASCII output only")
            return
        figure = build_figure(frames)
        if args.out:
            figure.savefig(args.out, dpi=120)
            print("wrote %s" % args.out)
        if args.show:
            import matplotlib.pyplot as plt
            plt.show()


if __name__ == "__main__":
    main()
