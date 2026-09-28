#!/usr/bin/env python3
"""Check `vkr_bakery bundle` on a small content tree with an independent reader.

The tree exercises each closure rule: a scene names a material and cubemap
faces, the material names a texture through a query string, the texture ships
as its `.vkt` sibling, a recipe includes a directory, and two identical files
share one chunk. The `.vkpak` is parsed here from its documented layout, not
through the runtime reader, and every entry is compared with its source.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

import project_jobs as jobs

HEADER = struct.Struct('<4sIIIQQQQQ32s40s')
CHUNK = struct.Struct('<32sQQIIQ')
ENTRY = struct.Struct('<IIIIII')


def read_pack(path):
    data = path.read_bytes()
    (magic, version, flags, _, catalog_offset, catalog_size, table_offset,
     table_size, total_size, index_sha, _) = HEADER.unpack_from(data, 0)
    assert magic == b'VKPK' and version == 1 and flags == 0, (magic, version)
    assert total_size == len(data)
    catalog = data[catalog_offset:catalog_offset + catalog_size]
    table = data[table_offset:table_offset + table_size]
    assert hashlib.sha256(catalog + table).digest() == index_sha
    chunks = [CHUNK.unpack_from(table, i * CHUNK.size)
              for i in range(table_size // CHUNK.size)]
    assert [c[0] for c in chunks] == sorted(c[0] for c in chunks)
    for sha, offset, size, alignment, _, _ in chunks:
        assert offset % alignment == 0
        assert hashlib.sha256(data[offset:offset + size]).digest() == sha
    count = struct.unpack_from('<I', catalog, 0)[0]
    strings = catalog[8 + count * ENTRY.size:]
    entries = {}
    for i in range(count):
        offset, length, chunk, loader, _, _ = ENTRY.unpack_from(catalog, 8 + i * ENTRY.size)
        identity = strings[offset:offset + length].decode()
        _, chunk_offset, chunk_size, _, _, _ = chunks[chunk]
        entries[identity] = (data[chunk_offset:chunk_offset + chunk_size], loader)
    assert list(entries) == sorted(entries, key=lambda text: text.encode())
    return entries, len(chunks)


def write(root, relative, data):
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bakery', default=str(jobs.default_bakery()))
    args = parser.parse_args()
    bakery = Path(args.bakery).resolve()
    with tempfile.TemporaryDirectory(prefix='vkr-bundle-') as temporary:
        root = Path(temporary) / 'root'
        faces = {f'assets/textures/sky_{face}.png': f'face {face}' for face in 'rludfb'}
        write(root, 'assets/scenes/room.scene.json', json.dumps({
            'version': 2,
            'entities': [{'name': 'Wall', 'material': 'assets/materials/wall.mt'}],
            'reflection_probes': [{'cubemap': {'base_path': 'assets/textures/sky',
                                               'extension': 'png'}}],
            'label': 'not a file'}))
        write(root, 'assets/materials/wall.mt',
              'name=wall\nbase_color_texture=assets/textures/brick.png?cs=srgb\n')
        write(root, 'assets/textures/brick.png', 'source pixels')
        write(root, 'assets/textures/brick.png.vkt', 'cooked pixels')
        for face, text in faces.items():
            write(root, face, text)
            write(root, face + '.vkt', 'unused cooked face')
        write(root, 'assets/fonts/ui.bin', 'same bytes')
        write(root, 'assets/fonts/copy/ui.bin', 'same bytes')
        write(root, 'assets/unused.txt', 'not in the closure')
        recipe = root / 'room.bundle.json'
        recipe.write_text(json.dumps({'version': 1, 'name': 'room',
                                      'scene': 'assets/scenes/room.scene.json',
                                      'include': ['assets/fonts']}))
        out = Path(temporary) / 'out'
        command = [str(bakery), 'bundle', str(recipe), '--root', str(root), '--out', str(out),
                   '--cache', str(Path(temporary) / 'cache')]
        completed = subprocess.run(command, capture_output=True, text=True, timeout=120)
        assert completed.returncode == 0, completed.stdout + completed.stderr

        entries, chunk_count = read_pack(out / 'content' / 'room.vkpak')
        expected = {'assets/scenes/room.scene.json', 'assets/materials/wall.mt',
                    'assets/textures/brick.png.vkt', 'assets/fonts/ui.bin',
                    'assets/fonts/copy/ui.bin', *faces}
        assert set(entries) == expected, sorted(set(entries) ^ expected)
        for identity, (data, _) in entries.items():
            assert data == (root / identity).read_bytes(), identity
        assert chunk_count == len(entries) - 1, 'identical files share one chunk'
        assert entries['assets/scenes/room.scene.json'][1] == 1
        assert entries['assets/materials/wall.mt'][1] == 4
        assert entries['assets/textures/brick.png.vkt'][1] == 3
        description = json.loads((out / 'bundle.json').read_text())
        assert description['scene'] == 'assets/scenes/room.scene.json'
        assert description['packs'] == ['content/room.vkpak']
        assert set(description['products']) == expected

        # A missing cubemap face fails the bundle and names the face.
        (root / 'assets/textures/sky_b.png').unlink()
        failed = subprocess.run(command + ['--json'], capture_output=True, text=True, timeout=120)
        assert failed.returncode == 1, failed.stdout + failed.stderr
        diagnostics = [json.loads(line) for line in failed.stdout.splitlines()
                       if line.startswith('{')]
        assert any(event.get('ev') == 'diag' and event.get('source') ==
                   'assets/textures/sky_b.png' for event in diagnostics), failed.stdout
    print('Bakery bundle: closure, .vkt substitution, cubemap faces, directory includes, '
          'chunk deduplication, catalog order, index hash and missing inputs passed')


if __name__ == '__main__':
    main()
