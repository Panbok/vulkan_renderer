#!/usr/bin/env python3
"""Check first-party C sources against the repository .clang-format.

Covers `.c` and `.h` files owned by VKR. Objective-C, shader and `.inc`
sources are excluded: the repository contract forbids applying an unverified C
formatter to Slang or Metal source, and Objective-C needs a reviewed diff.

The formatting baseline was produced with clang-format 19. Other major
versions can disagree on line breaking, so the check reports a skip instead
of a failure when the installed major version differs; set
VKR_CLANG_FORMAT to select a specific binary.

Examples:
  python3 tools/checks/check_format.py
  python3 tools/checks/check_format.py --fix renderer/src/vkr_renderer.c
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REQUIRED_MAJOR = 19
ROOTS = ('lib/src', 'renderer/src', 'runtime/src', 'editor/src', 'app/src',
         'tools', 'tests/src', 'examples')
THIRD_PARTY_PREFIXES = ('stb_', 'cgltf')


def first_party_sources():
    for root in ROOTS:
        for path in sorted((ROOT / root).rglob('*')):
            if path.suffix not in ('.c', '.h') or not path.is_file():
                continue
            relative = path.relative_to(ROOT)
            if 'vendor' in relative.parts or '__pycache__' in relative.parts:
                continue
            if path.name.startswith(THIRD_PARTY_PREFIXES):
                continue
            yield relative


def clang_format_binary():
    binary = os.environ.get('VKR_CLANG_FORMAT') or shutil.which('clang-format')
    if not binary:
        return None, None
    version = subprocess.run([binary, '--version'], capture_output=True,
                             text=True).stdout
    match = re.search(r'version (\d+)\.', version)
    return binary, int(match.group(1)) if match else None


def is_formatted(binary, relative):
    process = subprocess.run([binary, '--dry-run', '-Werror', str(relative)],
                             cwd=ROOT, capture_output=True, text=True)
    return relative, process.returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--fix', action='store_true',
                        help='rewrite drifted files in place')
    parser.add_argument('files', nargs='*',
                        help='repository-relative files (default: all)')
    args = parser.parse_args()

    binary, major = clang_format_binary()
    if not binary:
        print('check_format: skipped, clang-format was not found')
        return 0
    if major != REQUIRED_MAJOR:
        print(f'check_format: skipped, clang-format {major} differs from the '
              f'formatting baseline version {REQUIRED_MAJOR}')
        return 0

    files = ([Path(f) for f in args.files] if args.files
             else list(first_party_sources()))
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        results = list(pool.map(lambda f: is_formatted(binary, f), files))
    drifted = [relative for relative, clean in results if not clean]
    if drifted and args.fix:
        subprocess.run([binary, '-i', *map(str, drifted)], cwd=ROOT,
                       check=True)
        print(f'check_format: formatted {len(drifted)} files')
        return 0
    if drifted:
        print(f'check_format: {len(drifted)} of {len(files)} files differ '
              'from .clang-format:')
        for relative in drifted:
            print(f'  {relative.as_posix()}')
        print('Run python3 tools/checks/check_format.py --fix <files> and '
              'review the diff.')
        return 1
    print(f'check_format: {len(files)} files match .clang-format')
    return 0


if __name__ == '__main__':
    sys.exit(main())
