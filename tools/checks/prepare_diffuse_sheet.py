#!/usr/bin/env python3
"""Cook the thin-sheet witnesses and apply their custom VKR material fields.

Run after ./build_release.sh. glTF supplies geometry and standard material
properties; the sibling .mt files own VKR's thin-sheet authoring subset.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
fixtures = Path('tests/fixtures/rendering/diffuse_sheet')
cooker = root / 'build_release/tools/vkr_mesh_cooker'
for variant in ('on', 'off', 'black', 'cutout'):
    stem = 'diffuse_sheet_' + variant
    source = fixtures / (stem + '.gltf')
    subprocess.run([str(cooker), '--input', str(source), '--output',
                    str(source.with_suffix('.vkb'))], cwd=root, check=True)
    materials = list((root / 'assets/materials' / stem).glob('*.mt'))
    assert len(materials) == 1, materials
    generated = dict(line.split('=', 1) for line in materials[0].read_text().splitlines() if '=' in line)
    authored = dict(line.split('=', 1) for line in (root / source.with_suffix('.mt')).read_text().splitlines() if '=' in line)
    authored['name'] = generated['name']
    materials[0].write_text(''.join(f'{key}={value}\n' for key, value in authored.items()))
