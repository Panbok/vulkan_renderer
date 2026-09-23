#!/usr/bin/env python3
"""Report first-party C, C++ and Objective-C functions by length.

The scan is lexical: a definition starts at a column-zero signature line that
ends with `{` and closes at the next column-zero `}`. Treat the result as a
readability diagnostic, not a parsed fact. `--max-lines` turns the report into
a check for production sources (everything outside `tests/`).

Examples:
  python3 tools/checks/report_long_functions.py --threshold 150
  python3 tools/checks/report_long_functions.py --max-lines 300
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROOTS = ('lib/src', 'renderer/src', 'runtime/src', 'editor/src', 'app/src',
         'tools', 'tests/src', 'examples')
EXTENSIONS = ('.c', '.h', '.m', '.cpp', '.inc')
SIGNATURE = re.compile(r'^[A-Za-z_][\w\s\*\(\),\[\]&:<>]*\)\s*(const\s*)?\{\s*$')
SKIPPED_PREFIXES = ('stb_', 'cgltf')


def sources():
    for root in ROOTS:
        for path in sorted((ROOT / root).rglob('*')):
            relative = path.relative_to(ROOT)
            if (path.suffix not in EXTENSIONS or not path.is_file() or
                    'vendor' in relative.parts or
                    path.name.startswith(SKIPPED_PREFIXES) or
                    path.name.endswith('_data.inc')):
                continue
            yield relative


def functions(relative):
    lines = (ROOT / relative).read_text(encoding='utf-8',
                                        errors='replace').split('\n')
    start = None
    for index, line in enumerate(lines):
        if start is None:
            if (SIGNATURE.match(line) and
                    not line.startswith(('typedef', 'struct', 'enum',
                                         'union', 'extern "C"'))):
                start = index
            continue
        if line.startswith('}'):
            yield index - start + 1, start + 1, line_signature(lines, start)
            start = None


def line_signature(lines, start):
    text = lines[start].strip()
    return text[:100]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--threshold', type=int, default=150,
                        help='list functions longer than this many lines')
    parser.add_argument('--max-lines', type=int,
                        help='fail if a production function is longer')
    args = parser.parse_args()

    rows = []
    for relative in sources():
        for length, line, signature in functions(relative):
            rows.append((length, relative.as_posix(), line, signature))
    rows.sort(reverse=True)
    listed = [row for row in rows if row[0] > args.threshold]
    print(f'{len(rows)} functions; {len(listed)} longer than '
          f'{args.threshold} lines')
    for length, path, line, signature in listed:
        print(f'{length:5d}  {path}:{line}  {signature}')
    if args.max_lines is None:
        return 0
    violations = [row for row in rows
                  if row[0] > args.max_lines and not row[1].startswith('tests/')]
    if violations:
        print(f'{len(violations)} production functions exceed '
              f'{args.max_lines} lines')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
