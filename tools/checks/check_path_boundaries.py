#!/usr/bin/env python3
"""Reject new path-boundary bypasses in shipping consumers.

This is a source policy guard, not a behavioral correctness test. Filesystem
implementations and vendor code own native APIs; diagnostic programs under
tools/checks are outside the shipping-consumer scope.
"""
import ast
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
NARROW_IO = re.compile(r'(?<![\w:])(?:std::)?(?:fopen|freopen|remove|rename)\s*\(')
failures = []
for directory in ('app/src', 'editor/src', 'runtime/src', 'tools'):
    for path in (ROOT / directory).rglob('*'):
        if path.suffix not in ('.c', '.cpp') or 'checks' in path.relative_to(ROOT).parts:
            continue
        text = path.read_text(encoding='utf-8')
        for line, value in enumerate(text.splitlines(), 1):
            if NARROW_IO.search(value):
                failures.append(f'{path.relative_to(ROOT)}:{line}: use the UTF-8 filesystem owner')

for path in (ROOT / 'tools').glob('editor_*.py'):
    tree = ast.parse(path.read_text(encoding='utf-8'))
    for node in ast.walk(tree):
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                and node.func.id == 'str' and len(node.args) == 1):
            value = node.args[0]
            if (isinstance(value, ast.Call) and isinstance(value.func, ast.Attribute)
                    and value.func.attr == 'relative_to'):
                failures.append(f'{path.relative_to(ROOT)}:{node.lineno}: serialize a portable reference')

# These executable-relative callers receive canonical '/' paths from the
# platform API. Project and asset roots must go through their owning resolver.
parent_exceptions = {
    'editor/src/editor_projects.c': "strrchr(projects->bootstrap_directory, '/')",
    'runtime/src/vkr_sample_runtime.c': "strrchr(bootstrap_fonts, '/')",
}
for filename, approved in parent_exceptions.items():
    for line, value in enumerate((ROOT / filename).read_text(encoding='utf-8').splitlines(), 1):
        if 'strrchr(' in value and ("'/'" in value or "'\\\\'" in value) and approved not in value:
            failures.append(f'{filename}:{line}: parent paths belong to the filesystem/project owner')

if failures:
    raise SystemExit('\n'.join(failures))
print('Path boundary guard passed (shipping native I/O, managed serialization, request parents)')
