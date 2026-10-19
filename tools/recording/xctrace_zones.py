#!/usr/bin/env python3
"""Read VKR's CPU zones from an Instruments trace (ADR-126).

A run with VKR_SIGNPOSTS=1, or `perf.set {signposts: true}`, opens an
os_signpost interval named "Zone" for each zone, in subsystem "dev.vkr",
whose message is the zone's name. Record it with only the os_signpost
instrument, which writes little:

    xcrun xctrace record --instrument os_signpost --attach <pid> \
        --time-limit 5s --output zones.trace

This tool exports the trace's os-signpost table through `xctrace export`,
pairs each interval's begin and end, and prints count, median, p95, max and
total milliseconds per zone. `--json` prints the same as JSON for agents;
`--perfetto OUT` writes a Chrome trace-event file for ui.perfetto.dev with
one track per thread, like `vkr_rec export --perfetto`.

    python3 tools/recording/xctrace_zones.py zones.trace [--json]
        [--perfetto zones.json] [--subsystem dev.vkr]

Exit codes: 0 success, 1 no matching intervals, 2 bad usage or an export
failure.
"""
import argparse
import json
import subprocess
import sys
import xml.etree.ElementTree as ET

SIGNPOST_XPATH = '/trace-toc/run[@number="1"]/data/table[@schema="os-signpost"]'


def export_signposts(trace):
    """The os-signpost table of the trace's first run, as XML text."""
    result = subprocess.run(
        ["xcrun", "xctrace", "export", "--input", trace, "--xpath", SIGNPOST_XPATH],
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


def read_intervals(xml_text, subsystem):
    """Paired intervals as (name, thread, tid, begin_ns, end_ns), and the
    count of begins left without an end."""
    root = ET.fromstring(xml_text)
    schema = root.find(".//schema")
    if schema is None:
        return [], 0
    columns = [col.findtext("mnemonic") for col in schema.findall("col")]
    values = Values()
    open_intervals = {}
    intervals = []
    for row in root.iter("row"):
        values.visit(row)
        cells = {}
        for name, element in zip(columns, list(row)):
            cells[name] = values.resolve(element)
        cell_subsystem = cells.get("subsystem")
        if cell_subsystem is None or cell_subsystem.get("fmt") != subsystem:
            continue
        event = cells["event-type"].get("fmt")
        identifier = cells["identifier"].text
        time_ns = int(cells["time"].text)
        thread = cells["thread"]
        tid_element = thread.find("tid")
        tid = int(values.resolve(tid_element).text) if tid_element is not None else 0
        if event == "Begin":
            message = cells.get("message")
            name = message.get("fmt") if message is not None else "?"
            open_intervals[identifier] = (name, thread.get("fmt"), tid, time_ns)
        elif event == "End" and identifier in open_intervals:
            name, thread_name, begin_tid, begin_ns = open_intervals.pop(identifier)
            intervals.append((name, thread_name, begin_tid, begin_ns, time_ns))
    return intervals, len(open_intervals)


def percentile(sorted_values, share):
    index = min(len(sorted_values) - 1, int(share * (len(sorted_values) - 1) + 0.5))
    return sorted_values[index]


def summarize(intervals):
    by_name = {}
    for name, _, _, begin_ns, end_ns in intervals:
        by_name.setdefault(name, []).append((end_ns - begin_ns) / 1.0e6)
    rows = []
    for name, durations in by_name.items():
        durations.sort()
        rows.append(
            {
                "zone": name,
                "count": len(durations),
                "median_ms": round(percentile(durations, 0.5), 4),
                "p95_ms": round(percentile(durations, 0.95), 4),
                "max_ms": round(durations[-1], 4),
                "total_ms": round(sum(durations), 3),
            }
        )
    rows.sort(key=lambda row: -row["total_ms"])
    return rows


def write_perfetto(intervals, path):
    first_ns = min(begin for _, _, _, begin, _ in intervals)
    events = []
    threads = {}
    for name, thread_name, tid, begin_ns, end_ns in intervals:
        threads.setdefault(tid, thread_name)
        events.append(
            {
                "name": name,
                "ph": "X",
                "pid": 1,
                "tid": tid,
                "ts": (begin_ns - first_ns) / 1000.0,
                "dur": (end_ns - begin_ns) / 1000.0,
            }
        )
    for tid, thread_name in threads.items():
        events.append(
            {
                "name": "thread_name",
                "ph": "M",
                "pid": 1,
                "tid": tid,
                "args": {"name": thread_name},
            }
        )
    with open(path, "w", encoding="utf-8") as output:
        json.dump({"traceEvents": events, "displayTimeUnit": "ms"}, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("trace", help="an Instruments .trace bundle")
    parser.add_argument("--subsystem", default="dev.vkr")
    parser.add_argument("--json", action="store_true", help="print JSON")
    parser.add_argument("--perfetto", help="write a Chrome trace-event file")
    args = parser.parse_args()
    try:
        intervals, unpaired = read_intervals(export_signposts(args.trace), args.subsystem)
    except (RuntimeError, OSError, ET.ParseError) as error:
        print(f"xctrace_zones: {error}", file=sys.stderr)
        return 2
    if not intervals:
        print(f"xctrace_zones: no {args.subsystem} intervals in {args.trace}", file=sys.stderr)
        return 1
    rows = summarize(intervals)
    span_ms = (max(i[4] for i in intervals) - min(i[3] for i in intervals)) / 1.0e6
    if args.perfetto:
        write_perfetto(intervals, args.perfetto)
    if args.json:
        print(json.dumps({"intervals": len(intervals), "unpaired": unpaired,
                          "span_ms": round(span_ms, 3), "zones": rows}, indent=1))
        return 0
    print(f"{len(intervals)} intervals over {span_ms:.1f} ms, {unpaired} unpaired")
    print(f"{'zone':20s} {'count':>7s} {'median':>9s} {'p95':>9s} {'max':>9s} {'total':>10s}")
    for row in rows:
        print(f"{row['zone']:20s} {row['count']:7d} {row['median_ms']:9.3f} "
              f"{row['p95_ms']:9.3f} {row['max_ms']:9.3f} {row['total_ms']:10.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
