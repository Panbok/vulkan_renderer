#!/usr/bin/env python3
"""Read the bounded Metal diagnostic segments without starting a renderer."""

import argparse
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--last", type=int, default=80, help="number of recent records")
    args = parser.parse_args()
    if args.last < 1:
        parser.error("--last must be positive")

    records = []
    for segment in range(2):
        path = args.directory / f"metal-diagnostics.{segment}.jsonl"
        try:
            if path.stat().st_size > 4 * 1024 * 1024:
                parser.error(f"{path} exceeds the segment size bound")
            with path.open("rb") as source:
                for line_number, line in enumerate(source, 1):
                    try:
                        if len(line) > 4096 or not line.endswith(b"\n"):
                            raise ValueError("incomplete or oversized record")
                        record = json.loads(line)
                        if not isinstance(record, dict) or any(
                            type(record.get(field)) is not int or record[field] < 0
                            for field in ("seq", "submitted", "completed")
                        ) or any(
                            not isinstance(record.get(field), str)
                            for field in ("event", "detail")
                        ):
                            raise ValueError("invalid diagnostic record fields")
                        records.append(record)
                    except (ValueError, UnicodeError) as error:
                        print(f"{path}:{line_number}: skipped: {error}", file=sys.stderr)
        except OSError as error:
            print(f"{path}: {error}", file=sys.stderr)

    if not records:
        print("No readable diagnostic records.", file=sys.stderr)
        return 1
    records.sort(key=lambda record: record["seq"])
    print(
        f"Read {len(records)} records, sequences {records[0]['seq']}..{records[-1]['seq']}. "
        "Rotation may have removed earlier history; these are CPU observations.",
        file=sys.stderr,
    )
    for record in records[-args.last:]:
        print(json.dumps(record, ensure_ascii=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
