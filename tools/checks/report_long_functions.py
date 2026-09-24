#!/usr/bin/env python3
"""Report first-party C, C++ and Objective-C functions by length.

The scan is lexical: a definition starts at a column-zero signature, which may
wrap over up to eight lines until one ends with `{`, and closes at the next
column-zero `}`. Treat the result as a
readability diagnostic, not a parsed fact. `--max-lines` turns the report into
a check for production sources (everything outside `tests/`).

Examples:
  python3 tools/checks/report_long_functions.py --threshold 150
  python3 tools/checks/report_long_functions.py --quiet --max-lines 300
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROOTS = ('lib/src', 'renderer/src', 'runtime/src', 'editor/src', 'app/src',
         'tools', 'tests/src', 'examples')
EXTENSIONS = ('.c', '.h', '.m', '.cpp', '.inc')
# An attribute macro such as API_AVAILABLE(macos(26.0)) may follow the
# parameter list.
SIGNATURE = re.compile(r'^[A-Za-z_][\w\s\*\(\),\[\]&:<>]*\)\s*(const\s*)?'
                       r'([A-Z_][A-Z0-9_]*\([^;{}]*\)\s*)?\{\s*$')
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


SIGNATURE_START = re.compile(r'^[A-Za-z_][\w\s\*\(\),\[\]&:<>]*\($')
SIGNATURE_LINES = 8
NOT_FUNCTIONS = ('typedef', 'struct', 'enum', 'union', 'extern "C"')


def signature_end(lines, index):
    """Returns the line that opens the body of a signature starting here."""
    line = lines[index]
    if line.startswith(NOT_FUNCTIONS) or not line or line[0] in ' \t#/{}':
        return None
    if SIGNATURE.match(line):
        return index
    # A signature may wrap its parameters or put its name on the next line.
    if '(' not in line and not re.match(r'^[A-Za-z_][\w\s\*]*$', line):
        return None
    joined = line
    for next_index in range(index + 1,
                            min(index + SIGNATURE_LINES, len(lines))):
        following = lines[next_index]
        if not following.startswith((' ', '\t')) and '(' in joined:
            return None
        joined += ' ' + following.strip()
        if ';' in following or '=' in following.split('(')[0]:
            return None
        if SIGNATURE.match(joined):
            return next_index
    return None


def functions(relative):
    lines = (ROOT / relative).read_text(encoding='utf-8',
                                        errors='replace').split('\n')
    start = None
    index = 0
    while index < len(lines):
        if start is None:
            end = signature_end(lines, index)
            if end is not None:
                start = index
                index = end + 1
                continue
            index += 1
            continue
        if lines[index].startswith('}'):
            yield index - start + 1, start + 1, line_signature(lines, start)
            start = None
        index += 1


def line_signature(lines, start):
    text = lines[start].strip()
    return text[:100]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--threshold', type=int, default=150,
                        help='list functions longer than this many lines')
    parser.add_argument('--max-lines', type=int,
                        help='fail if a production function is longer')
    parser.add_argument('--quiet', action='store_true',
                        help='print only the summary and any violations')
    args = parser.parse_args()

    rows = []
    for relative in sources():
        for length, line, signature in functions(relative):
            rows.append((length, relative.as_posix(), line, signature))
    rows.sort(reverse=True)
    listed = [row for row in rows if row[0] > args.threshold]
    if not args.quiet:
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
              f'{args.max_lines} lines:')
        for length, path, line, signature in violations:
            print(f'{length:5d}  {path}:{line}  {signature}')
        return 1
    print(f'report_long_functions: {len(rows)} functions; no production '
          f'function exceeds {args.max_lines} lines')
    return 0


if __name__ == '__main__':
    sys.exit(main())
