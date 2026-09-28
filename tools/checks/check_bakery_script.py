#!/usr/bin/env python3
"""Check the C script producers of `vkr_bakery` on a two-file module.

Covers the hot-reload library (loaded and called here), the static archive,
a warm build served from the cache, a header edit that recompiles every unit
including it, a source edit that recompiles one unit, compiler errors and
warnings mapped to VKR-SCRIPT codes with file, line and column, an unknown
description field, and a bundle that ships the module's archive.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import project_jobs as jobs


def cook(bakery, root, *extra):
    completed = subprocess.run(
        [str(bakery), 'cook', 'mod/walker.script.json', '--root', str(root), '--cache',
         str(root / 'cache'), '--json', *extra],
        cwd=root, capture_output=True, text=True, timeout=300)
    events = [json.loads(line) for line in completed.stdout.splitlines() if line.startswith('{')]
    return completed.returncode, events


def ran(events):
    return sorted(Path(event['source']).name for event in events
                  if event.get('ev') == 'start' and event.get('producer') == 'script_object'
                  and event.get('source'))


def call(library):
    """Loads the library in a fresh process, as a hot reload would."""
    completed = subprocess.run(
        [sys.executable, '-c', 'import ctypes, sys; '
         'print(ctypes.CDLL(sys.argv[1]).behavior_speed())', str(library)],
        capture_output=True, text=True, timeout=60, check=True)
    return int(completed.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bakery', default=str(jobs.default_bakery()))
    args = parser.parse_args()
    if sys.platform != 'darwin':
        print('Bakery scripts: unavailable; the check loads a macOS dylib')
        return
    bakery = Path(args.bakery).resolve()
    with tempfile.TemporaryDirectory(prefix='vkr-script-') as temporary:
        root = Path(temporary)
        (root / 'mod' / 'include').mkdir(parents=True)
        (root / 'mod' / 'include' / 'speed.h').write_text('int speed_scale(int value);\n')
        (root / 'mod' / 'behavior.c').write_text(
            '#include "speed.h"\n'
            '__attribute__((visibility("default"))) int behavior_speed(void) '
            '{ return speed_scale(SPEED); }\n')
        (root / 'mod' / 'scale.c').write_text(
            '#include "speed.h"\nint speed_scale(int value) { return value * 3; }\n')
        description = {'language': 'c', 'sources': ['behavior.c', 'scale.c'],
                       'include_roots': ['include'], 'defines': {'SPEED': 2},
                       'standard': 'c11'}
        (root / 'mod' / 'walker.script.json').write_text(json.dumps(description))

        code, events = cook(bakery, root)
        assert code == 0 and ran(events) == ['behavior.c', 'scale.c'], events
        library = root / 'mod' / 'libwalker.dylib'
        assert call(library) == 6
        assert (root / 'mod' / 'libwalker.a').read_bytes().startswith(b'!<arch>\n')

        code, events = cook(bakery, root)
        assert code == 0 and ran(events) == [], events

        with open(root / 'mod' / 'include' / 'speed.h', 'a') as header:
            header.write('/* edited */\n')
        code, events = cook(bakery, root)
        assert code == 0 and ran(events) == ['behavior.c', 'scale.c'], events

        (root / 'mod' / 'scale.c').write_text(
            '#include "speed.h"\nint speed_scale(int value) { int unused; return value * 5; }\n')
        code, events = cook(bakery, root)
        assert code == 0 and ran(events) == ['scale.c'], events
        warnings = [event for event in events if event.get('ev') == 'diag'
                    and event['code'] == 'VKR-SCRIPT-0101']
        assert warnings and warnings[0]['line'] == 2 and warnings[0]['column'] > 0, events
        assert call(library) == 10

        (root / 'mod' / 'scale.c').write_text(
            '#include "speed.h"\nint speed_scale(int value) { return value * missing; }\n')
        code, events = cook(bakery, root)
        errors = [event for event in events if event.get('ev') == 'diag'
                  and event['code'] == 'VKR-SCRIPT-0100']
        assert code == 1 and errors, events
        assert Path(errors[0]['source']).name == 'scale.c' and errors[0]['line'] == 2
        assert call(library) == 10, 'a failed build keeps the published library'

        (root / 'mod' / 'walker.script.json').write_text(json.dumps({**description, 'opt': 3}))
        code, events = cook(bakery, root)
        assert code != 0 and any(event.get('code', '').startswith('VKR-REC-')
                                 for event in events if event.get('ev') == 'diag'), events

        # A bundle ships the module's archive beside its content.
        (root / 'mod' / 'walker.script.json').write_text(json.dumps(description))
        (root / 'mod' / 'scale.c').write_text(
            '#include "speed.h"\nint speed_scale(int value) { return value * 3; }\n')
        code, events = cook(bakery, root)
        assert code == 0, events
        (root / 'scene.json').write_text('{"version": 2, "entities": []}')
        recipe = root / 'game.bundle.json'
        recipe.write_text(json.dumps({'version': 1, 'name': 'game', 'scene': 'scene.json',
                                      'scripts': ['mod/walker.script.json']}))
        out = root / 'out'
        bundled = subprocess.run(
            [str(bakery), 'bundle', str(recipe), '--root', str(root), '--out', str(out),
             '--cache', str(root / 'cache')], capture_output=True, text=True, timeout=300)
        assert bundled.returncode == 0, bundled.stdout + bundled.stderr
        assert json.loads((out / 'bundle.json').read_text())['scripts'] == [
            'scripts/libwalker.a']
        assert (out / 'scripts' / 'libwalker.a').read_bytes() == (
            root / 'mod' / 'libwalker.a').read_bytes()
    print('Bakery scripts: library and archive, warm cache, header and source edits, '
          'VKR-SCRIPT errors and warnings, unknown fields and bundled archives passed')


if __name__ == '__main__':
    main()
