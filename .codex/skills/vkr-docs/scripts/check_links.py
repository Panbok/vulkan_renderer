#!/usr/bin/env python3
"""Check local file targets in Markdown inline links and reference definitions.

Accept Markdown files or directories. Ignore fenced/inline code, URLs, and
fragment-only links. Check target existence, not heading anchors, reference-label
resolution, or index coverage. Link destinations may use angle brackets for spaces.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit

DESTINATION = r'(<[^>\n]+>|(?:\\.|[^\\()\s]|\([^()\n]*\))+)'
INLINE = re.compile(r'\]\(\s*' + DESTINATION)
REFERENCE = re.compile(r'^\s{0,3}\[[^\]\n]+\]:\s*' + DESTINATION)
FENCE = re.compile(r'^\s{0,3}(`{3,}|~{3,})')
CODE = re.compile(r'(`+).*?\1')


def targets(path: Path):
    fence = ''
    for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        match = FENCE.match(line)
        if match:
            marker = match.group(1)
            if not fence:
                fence = marker
            elif marker[0] == fence[0] and len(marker) >= len(fence):
                fence = ''
            continue
        if fence:
            continue
        line = CODE.sub('', line)
        matches = list(INLINE.finditer(line))
        reference = REFERENCE.match(line)
        if reference:
            matches.append(reference)
        for match in matches:
            destination = re.sub(r'\\([\\() ])', r'\1', match.group(1).strip('<>'))
            parsed = urlsplit(destination)
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            yield number, unquote(parsed.path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='+', type=Path, help='Markdown files or directories')
    args = parser.parse_args()
    files: set[Path] = set()
    for path in args.paths:
        if not path.exists():
            parser.error(f'input does not exist: {path}')
        if path.is_dir():
            files.update(path.rglob('*.md'))
        else:
            files.add(path)
    missing = checked = 0
    for path in sorted(files):
        for line, target in targets(path):
            checked += 1
            if not (path.parent / target).exists():
                print(f'{path}:{line}: missing {target}')
                missing += 1
    print(f'checked {checked} local targets in {len(files)} Markdown files; {missing} missing')
    return 1 if missing else 0


if __name__ == '__main__':
    raise SystemExit(main())
