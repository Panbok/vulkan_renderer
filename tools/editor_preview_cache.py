#!/usr/bin/env python3
"""Evict completed VKR thumbnail cache files before an isolated preview job."""
import argparse
from pathlib import Path
import re


def prune(directory: Path, limit: int) -> None:
    directory = directory.resolve(strict=True)
    if directory.name != 'thumbnails' or directory.parent.name != 'cache':
        raise ValueError('Expected a managed cache/thumbnails directory')
    pattern = re.compile(r'[0-9a-f]{16}-(128|256)\.png(?:\.log)?')
    entries = []
    total = 0
    for index, path in enumerate(directory.iterdir()):
        if index >= 65536:
            raise ValueError('Thumbnail cache enumeration exceeds 65536 entries')
        if path.is_symlink() or not pattern.fullmatch(path.name) or not path.is_file():
            continue
        stat = path.stat()
        entries.append((stat.st_mtime_ns, path.name, path, stat.st_size))
        total += stat.st_size
    # Keep 1 MiB available for the incoming bounded PNG and its short diagnostic.
    for _, _, path, size in sorted(entries):
        if total <= limit - 1024 * 1024:
            break
        path.unlink(missing_ok=True)
        total -= size


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--limit-mib', type=int, default=512)
    arguments = parser.parse_args()
    if not 8 <= arguments.limit_mib <= 4096:
        parser.error('Cache limit must be between 8 and 4096 MiB')
    prune(arguments.directory, arguments.limit_mib * 1024 * 1024)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
