#!/usr/bin/env python3
"""Re-run first-party translation units for warnings or static analysis.

The script reads `compile_commands.json` from a configured build tree and
re-runs every first-party translation unit with its recorded command. The
`warnings` mode adds `-fsyntax-only`; the `analyze` mode runs the clang static
analyzer. Diagnostics are de-duplicated by location and message, because a
header diagnostic otherwise repeats once per including translation unit.

The analyzer defines `__clang_analyzer__`; `logger.h` then turns `assert_log`
into an analysis assumption, so documented preconditions are not reported.

Examples:
  python3 tools/checks/run_clang_checks.py warnings --build-dir build_release
  python3 tools/checks/run_clang_checks.py analyze --output .scratch/analyze.txt
"""

import argparse
import collections
import json
import os
import re
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIRST_PARTY = ('lib/', 'renderer/', 'runtime/', 'editor/', 'app/', 'tools/',
               'tests/', 'examples/')
THIRD_PARTY_SOURCES = ('stb_', 'cgltf')
DIAGNOSTIC = re.compile(r'^(.+?):(\d+):(\d+): (warning|error): (.*)$')
FLAG = re.compile(r'\[([-\w=,.]+)\]\s*$')
DROPPED_WITH_VALUE = {'-o', '-MF', '-MT', '-MQ'}
DROPPED = {'-c', '-MD', '-MMD'}


def repo_relative(path):
    try:
        return Path(path).resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return None


def is_first_party(path):
    relative = repo_relative(path)
    if relative is None or not relative.startswith(FIRST_PARTY):
        return False
    if '/vendor/' in relative or '_deps/' in relative:
        return False
    return not Path(relative).name.startswith(THIRD_PARTY_SOURCES)


def load_units(build_dir, prefixes):
    database = build_dir / 'compile_commands.json'
    if not database.is_file():
        raise SystemExit(f'{database} is missing; configure the build first')
    units = {}
    for entry in json.loads(database.read_text()):
        source = Path(entry['directory'], entry['file']).resolve()
        if not is_first_party(source):
            continue
        relative = repo_relative(source)
        if prefixes and not relative.startswith(tuple(prefixes)):
            continue
        # Multi-configuration generators list one entry per configuration.
        units.setdefault(relative, entry)
    return units


def unit_arguments(entry):
    arguments = (entry['arguments'] if 'arguments' in entry
                 else shlex.split(entry['command']))
    result = []
    skip = False
    for argument in arguments:
        if skip:
            skip = False
            continue
        if argument in DROPPED_WITH_VALUE:
            skip = True
            continue
        if argument in DROPPED:
            continue
        result.append(argument)
    return result


def run_unit(mode, entry, extra):
    arguments = unit_arguments(entry) + extra
    if mode == 'warnings':
        arguments.append('-fsyntax-only')
    else:
        arguments += ['--analyze', '-o', os.devnull, '-Xclang',
                      '-analyzer-output=text', '-fno-color-diagnostics']
    process = subprocess.run(arguments, cwd=entry['directory'],
                             capture_output=True, text=True, timeout=1800)
    return process.returncode, process.stderr


def collect(output, directory, diagnostics):
    for line in output.splitlines():
        match = DIAGNOSTIC.match(line)
        if not match:
            continue
        path = match.group(1)
        if not os.path.isabs(path):
            path = os.path.join(directory, path)
        relative = repo_relative(path)
        if relative is None or not is_first_party(path):
            continue
        message = match.group(5)
        flag = FLAG.search(message)
        kind = flag.group(1) if flag else message[:60]
        key = (relative, int(match.group(2)), int(match.group(3)),
               match.group(4), message)
        diagnostics[key] = kind


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('mode', choices=('warnings', 'analyze'))
    parser.add_argument('--build-dir', default='build_release')
    parser.add_argument('--jobs', type=int,
                        default=max(1, (os.cpu_count() or 2) // 2))
    parser.add_argument('--output', help='write the full report here')
    parser.add_argument('--path', action='append', default=[],
                        help='restrict to repository-relative prefixes')
    parser.add_argument('--extra-flag', action='append', default=[],
                        help='append a compiler flag to every unit')
    parser.add_argument('--fail-on-diagnostics', action='store_true')
    args = parser.parse_args()

    build_dir = (ROOT / args.build_dir).resolve()
    units = load_units(build_dir, args.path)
    diagnostics = {}
    failures = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            relative: pool.submit(run_unit, args.mode, entry, args.extra_flag)
            for relative, entry in units.items()
        }
        for relative, future in sorted(futures.items()):
            code, output = future.result()
            collect(output, units[relative]['directory'], diagnostics)
            if code != 0 and args.mode == 'warnings':
                failures.append(relative)

    by_kind = collections.Counter(diagnostics.values())
    by_file = collections.Counter(key[0] for key in diagnostics)
    lines = [f'{len(diagnostics)} unique diagnostics in {len(by_file)} files '
             f'from {len(units)} translation units ({args.mode}, '
             f'{args.build_dir})', '', '== by kind ==']
    lines += [f'{count:6d}  {kind}' for kind, count in by_kind.most_common()]
    lines += ['', '== by file ==']
    lines += [f'{count:6d}  {path}' for path, count in by_file.most_common()]
    lines += ['', '== diagnostics ==']
    lines += [f'{path}:{line}:{column}: {severity}: {message}'
              for path, line, column, severity, message in sorted(diagnostics)]
    if failures:
        lines += ['', '== units that failed to compile ==', *failures]
    report = '\n'.join(lines) + '\n'
    if args.output:
        Path(args.output).write_text(report)
        print('\n'.join(lines[:3 + len(by_kind)]))
        print(f'full report: {args.output}')
    else:
        sys.stdout.write(report)
    if failures or (args.fail_on_diagnostics and diagnostics):
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
