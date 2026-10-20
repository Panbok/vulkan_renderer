#!/usr/bin/env python3
"""Read VKR's CPU zones and Metal GPU work from an Instruments trace.

    python3 tools/recording/vkr_xctrace.py zones TRACE [--json]
        [--perfetto OUT] [--subsystem dev.vkr]
    python3 tools/recording/vkr_xctrace.py gpu TRACE [--json]
        [--perfetto OUT] [--process NAME]
    python3 tools/recording/vkr_xctrace.py timeline TRACE --perfetto OUT
        [--process NAME] [--subsystem dev.vkr]
    python3 tools/recording/vkr_xctrace.py counters TRACE --recording BUNDLE
        [--bucket-us 50] [--subsystem dev.vkr] [--json]

`zones` reads the os-signpost table (ADR-126). A run with VKR_SIGNPOSTS=1,
or `perf.set {signposts: true}`, opens an os_signpost interval named "Zone"
for each zone, in subsystem "dev.vkr", whose message is the zone's name.
Record it with only the os_signpost instrument, which writes little:

    xcrun xctrace record --instrument os_signpost --attach <pid> \
        --time-limit 5s --output zones.trace

`gpu` reads the metal-gpu-intervals table of a Metal System Trace:

    xcrun xctrace record --template "Metal System Trace" --attach <pid> \
        --time-limit 3s --output gpu.trace

Metal labels each command encoder with the render graph pass that opened
it, so GPU time sums per pass label and channel (Vertex, Fragment,
Compute) for the process named NAME (default vkr_editor). The tiled
pipeline batches passes into one compute encoder under its first pass's
label (ADR-127), so a label's time covers the batch. Vertex and fragment
work of one encoder overlap on a tiled GPU; channel sums are not frame time.
A 3 s Metal System Trace of Bistro was 43 MB and left a 195 MB ktrace in
$TMPDIR; delete it after the run.

Both print a per-name table (count, median, p95, max, total ms), or JSON
with --json. --perfetto writes a Chrome trace-event file for ui.perfetto.dev:
zones on one track per thread, GPU intervals on one track per channel.
`timeline` writes both into one file. Both tables share the trace's clock:
record a Metal System Trace with `--instrument os_signpost` added, of a run
with signposts on. The os_signpost instrument's window starts later and
ends later than the Metal tables', so the timeline is complete only where
both cover.

`counters` imports Apple's GPU counters (ALU, texture and memory limiters,
vertex, fragment and compute occupancy, bandwidth) into a recording, whose
Performance window HW tab then shows them per frame and per pass (ADR-130).
Record the Metal GPU Counters and os_signpost instruments while a recording
runs with signposts on (`perf.set {signposts: true}`):

    xcrun xctrace record --instrument "Metal GPU Counters" \
        --instrument os_signpost --attach <pid> --time-limit 3s \
        --output counters.trace

With signposts on, each frame emits a "Clock" signpost carrying the CPU
clock the recording uses, which places the trace's samples on it. The
counters cover the whole GPU, including other processes' work. Samples are
averaged into buckets of --bucket-us microseconds and written to the
bundle's gpu_counters.bin (runtime/src/assets/vkr_recording_format.h). A
3 s trace of headless Bistro was 141 MB and exported 0.9 GB of counter rows,
which the import streams; delete the trace and $TMPDIR's ktrace after it.

Exit codes: 0 success, 1 no matching rows, 2 bad usage or an export failure.
"""
import argparse
import json
import math
import os
import re
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
from array import array

TABLE_XPATH = '/trace-toc/run[@number="1"]/data/table[@schema="{}"]'
GPU_LABEL = re.compile(r"^Command Buffer \d+:(?P<label>.*?)\s+\((?P<process>.*)\)\s+0x")
# One cell of a row as xctrace writes it: a value with an id, or a ref.
ROW_CELL = re.compile(r'<([\w-]+) (?:id="(\d+)"(?: fmt="[^"]*")?>([^<]*)</\1>|ref="(\d+)"/>)')

# The bundle file and its layout (vkr_recording_format.h).
COUNTERS_FILE = "gpu_counters.bin"
COUNTERS_MAGIC = b"VKRGPUCT"
COUNTERS_VERSION = 1
COUNTER_MAX = 64
COUNTER_NAME_MAX = 63
COUNTER_BUCKET_MAX = 1 << 20
UNIT_PERCENT, UNIT_GB_PER_SECOND, UNIT_COUNT = 0, 1, 2


def export_table(trace, schema):
    """One table of the trace's first run, as XML text."""
    result = subprocess.run(
        ["xcrun", "xctrace", "export", "--input", trace, "--xpath",
         TABLE_XPATH.format(schema)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "xctrace export failed")
    return result.stdout


class Values:
    """Resolves xctrace's `id`/`ref` deduplication: a value appears in full
    once with an id, and later rows refer to it by `ref`."""

    def __init__(self):
        self.by_id = {}

    def visit(self, element):
        for node in element.iter():
            node_id = node.get("id")
            if node_id is not None and node_id not in self.by_id:
                self.by_id[node_id] = node

    def resolve(self, element):
        ref = element.get("ref")
        return self.by_id.get(ref, element) if ref is not None else element


def table_rows(xml_text):
    """Each row as {mnemonic: element}, with references resolved."""
    root = ET.fromstring(xml_text)
    schema = root.find(".//schema")
    if schema is None:
        return
    columns = [col.findtext("mnemonic") for col in schema.findall("col")]
    values = Values()
    for row in root.iter("row"):
        values.visit(row)
        yield {name: values.resolve(element)
               for name, element in zip(columns, list(row))}


def read_zones(xml_text, subsystem):
    """Paired zone intervals as (name, track, track_name, begin_ns, end_ns),
    and the count of begins left without an end."""
    open_intervals = {}
    intervals = []
    for cells in table_rows(xml_text):
        cell_subsystem = cells.get("subsystem")
        if cell_subsystem is None or cell_subsystem.get("fmt") != subsystem:
            continue
        event = cells["event-type"].get("fmt")
        identifier = cells["identifier"].text
        time_ns = int(cells["time"].text)
        thread = cells["thread"]
        tid_element = thread.find("tid")
        tid = int(tid_element.text) if tid_element is not None and tid_element.text else 0
        if event == "Begin":
            message = cells.get("message")
            name = message.get("fmt") if message is not None else "?"
            open_intervals[identifier] = (name, tid, thread.get("fmt"), time_ns)
        elif event == "End" and identifier in open_intervals:
            name, begin_tid, thread_name, begin_ns = open_intervals.pop(identifier)
            intervals.append((name, begin_tid, thread_name, begin_ns, time_ns))
    return intervals, len(open_intervals)


def read_gpu(xml_text, process):
    """The process's GPU intervals as (label, channel_index, channel,
    begin_ns, end_ns, frame), and the count of other processes' rows."""
    intervals = []
    others = 0
    channels = {}
    for cells in table_rows(xml_text):
        label_cell = cells.get("event-label")
        match = GPU_LABEL.match(label_cell.get("fmt") or "") if label_cell is not None else None
        if not match or not match.group("process").startswith(process + " ("):
            others += 1
            continue
        channel = cells["channel-name"].get("fmt") or "?"
        channel_index = channels.setdefault(channel, len(channels) + 1)
        begin_ns = int(cells["start"].text)
        end_ns = begin_ns + int(cells["duration"].text)
        frame = cells.get("frame-number")
        intervals.append((match.group("label"), channel_index, channel, begin_ns,
                          end_ns, frame.get("fmt") if frame is not None else ""))
    return intervals, others


def percentile(sorted_values, share):
    index = min(len(sorted_values) - 1, int(share * (len(sorted_values) - 1) + 0.5))
    return sorted_values[index]


def summarize(keyed_durations):
    rows = []
    for key, durations in keyed_durations.items():
        durations.sort()
        rows.append({
            "name": key,
            "count": len(durations),
            "median_ms": round(percentile(durations, 0.5), 4),
            "p95_ms": round(percentile(durations, 0.95), 4),
            "max_ms": round(durations[-1], 4),
            "total_ms": round(sum(durations), 3),
        })
    rows.sort(key=lambda row: -row["total_ms"])
    return rows


def perfetto_events(intervals, pid, process_name, track_names):
    """Intervals as (name, track, begin_ns, end_ns), times from the trace's
    start in microseconds, so events of one trace line up."""
    events = [{"name": "process_name", "ph": "M", "pid": pid,
               "args": {"name": process_name}}]
    for track, track_name in track_names.items():
        events.append({"name": "thread_name", "ph": "M", "pid": pid,
                       "tid": track, "args": {"name": track_name}})
    for name, track, begin_ns, end_ns in intervals:
        events.append({"name": name, "ph": "X", "pid": pid, "tid": track,
                       "ts": begin_ns / 1000.0, "dur": (end_ns - begin_ns) / 1000.0})
    return events


def zone_events(intervals):
    return perfetto_events([(n, t, b, e) for n, t, _, b, e in intervals], 1,
                           "CPU zones", {t: tn for _, t, tn, _, _ in intervals})


def gpu_events(intervals, process):
    return perfetto_events([(l, c, b, e) for l, c, _, b, e, _ in intervals], 2,
                           "GPU " + process, {c: n for _, c, n, _, _, _ in intervals})


def write_perfetto(path, events):
    with open(path, "w", encoding="utf-8") as output:
        json.dump({"traceEvents": events, "displayTimeUnit": "ms"}, output)


def print_table(header, rows):
    print(header)
    print(f"{'name':34s} {'count':>7s} {'median':>9s} {'p95':>9s} {'max':>9s} {'total':>10s}")
    for row in rows:
        print(f"{row['name'][:34]:34s} {row['count']:7d} {row['median_ms']:9.3f} "
              f"{row['p95_ms']:9.3f} {row['max_ms']:9.3f} {row['total_ms']:10.1f}")


def read_counter_info(xml_text):
    """{counter id: (name, unit)} from the gpu-counter-info table."""
    counters = {}
    for cells in table_rows(xml_text):
        counter_id = int(cells["counter-id"].text)
        name = (cells["name"].text or "").strip()[:COUNTER_NAME_MAX]
        kind = cells["type"].text or ""
        if kind == "Percentage":
            unit = UNIT_PERCENT
        elif "Bandwidth" in name:
            unit = UNIT_GB_PER_SECOND
        else:
            unit = UNIT_COUNT
        counters.setdefault(counter_id, (name, unit))
    return counters


def read_clock_offsets(xml_text, subsystem):
    """Per "Clock" signpost, the CPU clock it carries minus its trace time."""
    offsets = []
    for cells in table_rows(xml_text):
        cell_subsystem = cells.get("subsystem")
        name = cells.get("name")
        message = cells.get("message")
        if (cell_subsystem is None or cell_subsystem.get("fmt") != subsystem or
                name is None or name.get("fmt") != "Clock" or message is None):
            continue
        # The message's own text groups digits; its uint64 holds the value.
        value = message.find("uint64")
        text = value.text if value is not None else re.sub(r"\D", "", message.get("fmt") or "")
        try:
            host_ns = int(text or "")
        except ValueError:
            continue
        offsets.append(host_ns - int(cells["time"].text))
    return offsets


def stream_counter_values(trace):
    """Yields (time ns, counter id, value) of the gpu-counter-value table,
    streamed from xctrace, whose export of a few seconds is a gigabyte."""
    process = subprocess.Popen(
        ["xcrun", "xctrace", "export", "--input", trace, "--xpath",
         TABLE_XPATH.format("gpu-counter-value")],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1 << 20)
    # Every cell value by id: ids are dense, so an array holds them.
    values = array("d")
    try:
        for line in process.stdout:
            start = line.find("<row>")
            if start < 0:
                continue
            row = []
            for match in ROW_CELL.finditer(line, start):
                if match.group(2) is not None:
                    value_id = int(match.group(2))
                    if value_id >= len(values):
                        values.extend([math.nan] * (value_id + 1 - len(values) + 4096))
                    value = float(match.group(3))
                    values[value_id] = value
                    row.append(value)
                else:
                    row.append(values[int(match.group(4))])
            if len(row) >= 3:
                yield int(row[0]), int(row[1]), row[2]
    finally:
        process.stdout.close()
        stderr = process.stderr.read()
        if process.wait() != 0:
            raise RuntimeError(stderr.strip() or "xctrace export failed")


def write_counters(path, counters, sums, samples, bucket_count, bucket_ns,
                   first_ns, anchors, spread_ns):
    """Writes the bundle file beside its stream, replacing an earlier one."""
    header = COUNTERS_MAGIC + struct.pack("<IIIIQII", COUNTERS_VERSION, len(counters),
                                          bucket_count, bucket_ns, first_ns, anchors,
                                          min(spread_ns, 0xFFFFFFFF))
    names = bytearray()
    for name, unit in counters:
        encoded = name.encode("utf-8")[:COUNTER_NAME_MAX]
        names += struct.pack("<BB", unit, len(encoded)) + encoded
    names += b"\0" * (-(len(header) + len(names)) % 4)
    temporary = path + ".tmp"
    with open(temporary, "wb") as output:
        output.write(header)
        output.write(names)
        for index in range(len(counters)):
            means = array("f", (s / n if n else math.nan
                                for s, n in zip(sums[index], samples[index])))
            means.extend([math.nan] * (bucket_count - len(means)))
            if sys.byteorder != "little":
                means.byteswap()
            output.write(means.tobytes())
    os.replace(temporary, path)


def run_counters(args):
    if not os.path.isfile(os.path.join(args.recording, "stream.vkrs")):
        print(f"vkr_xctrace: {args.recording} is not a recording bundle", file=sys.stderr)
        return 2
    if args.bucket_us < 10 or args.bucket_us > 10000:
        print("vkr_xctrace: --bucket-us lies in [10, 10000]", file=sys.stderr)
        return 2
    info = read_counter_info(export_table(args.trace, "gpu-counter-info"))
    if not info:
        print(f"vkr_xctrace: no GPU counters in {args.trace}; record the Metal GPU "
              "Counters instrument", file=sys.stderr)
        return 1
    offsets = sorted(read_clock_offsets(export_table(args.trace, "os-signpost"),
                                        args.subsystem))
    if not offsets:
        print(f"vkr_xctrace: no {args.subsystem} Clock signposts in {args.trace}; record "
              "the os_signpost instrument of a run with signposts on", file=sys.stderr)
        return 1
    offset_ns = offsets[len(offsets) // 2]
    spread_ns = offsets[-1] - offsets[0]

    # Counters in id order; buckets from the trace's time zero.
    ids = sorted(info)[:COUNTER_MAX]
    slot = {counter_id: index for index, counter_id in enumerate(ids)}
    bucket_ns = args.bucket_us * 1000
    sums = [array("d") for _ in ids]
    samples = [array("I") for _ in ids]
    rows = 0
    last_bucket = -1
    for time_ns, counter_id, value in stream_counter_values(args.trace):
        index = slot.get(counter_id)
        if index is None or not math.isfinite(value):
            continue
        bucket = time_ns // bucket_ns
        if bucket >= COUNTER_BUCKET_MAX:
            break
        counter_sums = sums[index]
        if bucket >= len(counter_sums):
            grow = bucket + 1 - len(counter_sums) + 1024
            counter_sums.extend([0.0] * grow)
            samples[index].extend([0] * grow)
        counter_sums[bucket] += value
        samples[index][bucket] += 1
        last_bucket = max(last_bucket, bucket)
        rows += 1
    if rows == 0:
        print(f"vkr_xctrace: no GPU counter samples in {args.trace}", file=sys.stderr)
        return 1
    # Buckets before the first sample and after the last are left out; the
    # file's first bucket starts that many buckets after the trace's time
    # zero, on the CPU clock.
    first_bucket = min(next((b for b, n in enumerate(counter_samples) if n), last_bucket)
                       for counter_samples in samples)
    bucket_count = last_bucket + 1 - first_bucket
    for index in range(len(ids)):
        del sums[index][last_bucket + 1:]
        del samples[index][last_bucket + 1:]
        del sums[index][:first_bucket]
        del samples[index][:first_bucket]
    first_ns = offset_ns + first_bucket * bucket_ns
    path = os.path.join(args.recording, COUNTERS_FILE)
    write_counters(path, [info[i] for i in ids], sums, samples, bucket_count, bucket_ns,
                   first_ns, len(offsets), spread_ns)
    result = {"path": path, "bytes": os.path.getsize(path), "counters": len(ids),
              "samples": rows, "buckets": bucket_count, "bucket_us": args.bucket_us,
              "span_ms": round(bucket_count * bucket_ns / 1.0e6, 3),
              "clock_anchors": len(offsets), "clock_spread_us": round(spread_ns / 1000.0, 3)}
    if args.json:
        print(json.dumps(result, indent=1))
    else:
        print(f"{rows} samples of {len(ids)} counters over {result['span_ms']:.1f} ms in "
              f"{bucket_count} buckets of {args.bucket_us} us; {len(offsets)} clock "
              f"signposts within {result['clock_spread_us']:.1f} us; wrote {path} "
              f"({result['bytes']} bytes)")
    return 0


def run_zones(args):
    intervals, unpaired = read_zones(export_table(args.trace, "os-signpost"), args.subsystem)
    if not intervals:
        print(f"vkr_xctrace: no {args.subsystem} intervals in {args.trace}", file=sys.stderr)
        return 1
    durations = {}
    for name, _, _, begin_ns, end_ns in intervals:
        durations.setdefault(name, []).append((end_ns - begin_ns) / 1.0e6)
    rows = summarize(durations)
    span_ms = (max(i[4] for i in intervals) - min(i[3] for i in intervals)) / 1.0e6
    if args.perfetto:
        write_perfetto(args.perfetto, zone_events(intervals))
    if args.json:
        print(json.dumps({"intervals": len(intervals), "unpaired": unpaired,
                          "span_ms": round(span_ms, 3), "zones": rows}, indent=1))
    else:
        print_table(f"{len(intervals)} intervals over {span_ms:.1f} ms, {unpaired} unpaired", rows)
    return 0


def run_gpu(args):
    intervals, others = read_gpu(export_table(args.trace, "metal-gpu-intervals"), args.process)
    if not intervals:
        print(f"vkr_xctrace: no GPU intervals of {args.process} in {args.trace}", file=sys.stderr)
        return 1
    by_label = {}
    by_channel = {}
    frames = set()
    for label, _, channel, begin_ns, end_ns, frame in intervals:
        ms = (end_ns - begin_ns) / 1.0e6
        by_label.setdefault(label, []).append(ms)
        by_channel.setdefault(channel, []).append(ms)
        frames.add(frame)
    span_ms = (max(i[4] for i in intervals) - min(i[3] for i in intervals)) / 1.0e6
    label_rows = summarize(by_label)
    channel_rows = summarize(by_channel)
    if args.perfetto:
        write_perfetto(args.perfetto, gpu_events(intervals, args.process))
    if args.json:
        print(json.dumps({"intervals": len(intervals), "other_rows": others,
                          "frames": len(frames), "span_ms": round(span_ms, 3),
                          "channels": channel_rows, "labels": label_rows}, indent=1))
    else:
        print_table(f"{len(intervals)} GPU intervals in {len(frames)} frames over "
                    f"{span_ms:.1f} ms ({others} rows of other processes)", channel_rows)
        print()
        print_table("by encoder label", label_rows)
    return 0


def run_timeline(args):
    zones, unpaired = read_zones(export_table(args.trace, "os-signpost"), args.subsystem)
    gpu, others = read_gpu(export_table(args.trace, "metal-gpu-intervals"), args.process)
    if not zones and not gpu:
        print(f"vkr_xctrace: no zones or GPU intervals in {args.trace}", file=sys.stderr)
        return 1
    write_perfetto(args.perfetto, zone_events(zones) + gpu_events(gpu, args.process))
    print(json.dumps({"zones": len(zones), "unpaired": unpaired, "gpu_intervals": len(gpu),
                      "other_rows": others, "perfetto": args.perfetto}))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    zones = commands.add_parser("zones", help="CPU zones from os-signpost")
    zones.add_argument("--subsystem", default="dev.vkr")
    gpu = commands.add_parser("gpu", help="Metal GPU intervals by encoder label")
    gpu.add_argument("--process", default="vkr_editor")
    timeline = commands.add_parser("timeline", help="zones and GPU intervals in one file")
    timeline.add_argument("--process", default="vkr_editor")
    timeline.add_argument("--subsystem", default="dev.vkr")
    timeline.add_argument("--perfetto", required=True, help="the trace-event file")
    counters = commands.add_parser("counters", help="import GPU counters into a recording")
    counters.add_argument("--recording", required=True, help="the recording bundle")
    counters.add_argument("--bucket-us", type=int, default=50,
                          help="microseconds the samples are averaged over")
    counters.add_argument("--subsystem", default="dev.vkr")
    counters.add_argument("--json", action="store_true", help="print JSON")
    for command in (zones, gpu):
        command.add_argument("--json", action="store_true", help="print JSON")
        command.add_argument("--perfetto", help="write a Chrome trace-event file")
    for command in (zones, gpu, timeline, counters):
        command.add_argument("trace", help="an Instruments .trace bundle")
    args = parser.parse_args()
    runners = {"zones": run_zones, "gpu": run_gpu, "timeline": run_timeline,
               "counters": run_counters}
    try:
        return runners[args.command](args)
    except (RuntimeError, OSError, ET.ParseError, ValueError) as error:
        print(f"vkr_xctrace: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
