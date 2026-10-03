#!/usr/bin/env python3
"""Syntax-check the instrumented sources with VKR_METRICS_ENABLED=0.

ADR-015 lets a build compile the metric writers out. Nothing builds that
configuration routinely, and it breaks as soon as a helper or local exists
only for instrumented code. This check reruns every first-party translation
unit that mentions the metrics API, including the `.inc` files a unit
includes, from an existing compile database with the macro set to 0 and
`-fsyntax-only`. The build's own flags, including -Werror, still apply.

Usage:
  python3 tools/checks/check_metrics_disabled.py BUILD_DIR/compile_commands.json
"""

import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIRST_PARTY = ('lib/src/', 'renderer/src/', 'runtime/src/', 'editor/src/',
               'app/src/', 'tools/', 'tests/src/', 'examples/')
METRICS_USE = re.compile(r'VKR_METRICS|vkr_metrics_')
INCLUDE_INC = re.compile(r'^\s*#\s*include\s+"([^"]+\.inc)"', re.M)


def unit_text(source):
    """The unit's own text plus the `.inc` files it includes directly."""
    text = source.read_text(encoding='utf-8', errors='replace')
    parts = [text]
    for name in INCLUDE_INC.findall(text):
        included = source.parent / name
        if included.is_file():
            parts.append(included.read_text(encoding='utf-8', errors='replace'))
    return '\n'.join(parts)


def split_command(command):
    """A compile database command as the compiler receives it. Windows
    commands follow the C runtime's quoting rules, under which backslashes in
    paths are literal; POSIX shell splitting would remove them."""
    if os.name != 'nt':
        return shlex.split(command)
    import ctypes
    from ctypes import wintypes
    split = ctypes.windll.shell32.CommandLineToArgvW
    split.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    split.restype = ctypes.POINTER(wintypes.LPWSTR)
    count = ctypes.c_int(0)
    argv = split(command, ctypes.byref(count))
    if not argv:
        raise ctypes.WinError()
    try:
        return [argv[index] for index in range(count.value)]
    finally:
        ctypes.windll.kernel32.LocalFree(argv)


def disabled_command(entry, wrapper):
    """The entry's compiler invocation, syntax-only, with metrics compiled
    out. The CMake PCH wrapper is replaced by an equivalent header with no
    precompiled file beside it, so the instrumented PCH is not reused."""
    arguments = entry.get('arguments') or split_command(entry['command'])
    out = []
    skip = 0
    for index, argument in enumerate(arguments):
        if skip:
            skip -= 1
            continue
        if argument in ('-o', '-MF', '-MT', '-MQ'):
            skip = 1
            continue
        if argument in ('-c', '-MD', '-MMD'):
            continue
        following = arguments[index + 1] if index + 1 < len(arguments) else ''
        # Clang targeting MSVC receives the PCH through cc1 options:
        # -Xclang -include-pch -Xclang <pch> -Xclang -include -Xclang <header>.
        value = arguments[index + 3] if index + 3 < len(arguments) else ''
        if (argument == '-Xclang' and
                following in ('-include-pch', '-include') and
                'cmake_pch' in value):
            if following == '-include':
                out += ['-include', wrapper]
            skip = 3
            continue
        if argument.startswith('-Xarch_') and 'cmake_pch' in following:
            # CMake may scope the PCH include to one architecture.
            continue
        if argument.startswith('-include') and 'cmake_pch' in argument:
            out += ['-include', wrapper]
            continue
        if argument.startswith('-DVKR_METRICS_ENABLED='):
            out.append('-DVKR_METRICS_ENABLED=0')
            continue
        out.append(argument)
    return out + ['-fsyntax-only']


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2
    database = Path(sys.argv[1])
    if not database.is_file():
        print(f'check_metrics_disabled: {database} not found; build first')
        return 2
    entries = json.loads(database.read_text())

    units = []
    for entry in entries:
        source = Path(entry['file'])
        try:
            relative = source.resolve().relative_to(ROOT).as_posix()
        except ValueError:
            continue
        if not relative.startswith(FIRST_PARTY) or 'vendor/' in relative:
            continue
        if METRICS_USE.search(unit_text(source)):
            units.append((relative, entry))

    with tempfile.TemporaryDirectory() as directory:
        wrapper = os.path.join(directory, 'vkr_pch_wrapper.h')
        with open(wrapper, 'w') as handle:
            handle.write('#pragma clang system_header\n'
                         f'#include "{ROOT / "lib/src/vkr_pch.h"}"\n')

        def check(unit):
            relative, entry = unit
            result = subprocess.run(disabled_command(entry, wrapper),
                                    cwd=entry['directory'],
                                    capture_output=True, text=True)
            return relative, result.returncode, result.stderr

        with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            results = list(pool.map(check, units))

    failures = [row for row in results if row[1] != 0]
    for relative, _, stderr in failures:
        print(f'{relative}:')
        for line in stderr.splitlines():
            if ': error:' in line or ': warning:' in line:
                print('  ' + line.replace(str(ROOT) + '/', ''))
    print(f'check_metrics_disabled: {len(results) - len(failures)} of '
          f'{len(results)} instrumented units compile with '
          'VKR_METRICS_ENABLED=0')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
