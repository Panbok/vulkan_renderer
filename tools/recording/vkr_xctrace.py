#!/usr/bin/env python3
"""Read VKR's CPU zones and Metal GPU work from an Instruments trace.

    python3 tools/recording/vkr_xctrace.py zones TRACE [--json]
        [--perfetto OUT] [--subsystem dev.vkr]
    python3 tools/recording/vkr_xctrace.py gpu TRACE [--json]
        [--perfetto OUT] [--process NAME]
    python3 tools/recording/vkr_xctrace.py timeline TRACE --perfetto OUT
        [--process NAME] [--subsystem dev.vkr]

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

Apple's GPU counters (occupancy, limiters) import into a recording with
`vkr_rec counters TRACE --recording BUNDLE`, or from the editor's
Recordings window (ADR-130).

Exit codes: 0 success, 1 no matching rows, 2 bad usage or an export failure.
"""
import argparse
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

TABLE_XPATH = '/trace-toc/run[@number="1"]/data/table[@schema="{}"]'
GPU_LABEL = re.compile(r"^Command Buffer \d+:(?P<label>.*?)\s+\((?P<process>.*)\)\s+0x")


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
    for command in (zones, gpu):
        command.add_argument("--json", action="store_true", help="print JSON")
        command.add_argument("--perfetto", help="write a Chrome trace-event file")
    for command in (zones, gpu, timeline):
        command.add_argument("trace", help="an Instruments .trace bundle")
    args = parser.parse_args()
    runners = {"zones": run_zones, "gpu": run_gpu, "timeline": run_timeline}
    try:
        return runners[args.command](args)
    except (RuntimeError, OSError, ET.ParseError, ValueError) as error:
        print(f"vkr_xctrace: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
