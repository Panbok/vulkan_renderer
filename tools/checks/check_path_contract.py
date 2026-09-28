#!/usr/bin/env python3
"""Exercise portable grammar, source identity and cubemap preparation (no GPU).

Every case runs `vkr_bakery project` on a small workspace fixture: managed
path grammar through lowering, legacy source migration through a committed
edit, OBJ/MTL and glTF source snapshots through unbuilt imports, and six-face
probe preparation through `prepare_scene`.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import uuid

import project_jobs as jobs

ROOT = Path(__file__).resolve().parents[2]


class Workspace:
    """One workspace with one project; each scene gets a fresh identifier."""

    def __init__(self, root, bakery):
        self.root = root
        self.bakery = bakery
        self.workspace = root / 'workspace'
        self.project = self.workspace / 'projects' / str(uuid.uuid4())
        self.project.mkdir(parents=True)
        self.manifest = self.project / 'project.json'
        jobs.atomic_json(self.manifest, {'version': 1, 'assets': [], 'scenes': [], 'default_font': None})
        jobs.atomic_json(self.workspace / 'editor/bundles/1/manifest.json',
                         {'version': 1, 'files': [], 'assets': []})
        self.result = root / 'result.json'

    def request(self, operation, scene_id, **fields):
        return {'version': 1, 'operation': operation, 'workspace_root': str(self.workspace),
                'project_path': str(self.manifest), 'scene_id': scene_id,
                'tools': {'mesh': str(self.bakery)}, **fields}

    def run(self, operation, scene_id, **fields):
        job = jobs.Job(self.request(operation, scene_id, **fields), self.result, self.bakery)
        code = job.execute()
        return code, jobs.load_json(self.result)

    def scene(self, document):
        """Writes a version 3 scene fixture; returns (scene id, scene path)."""
        scene_id = str(uuid.uuid4())
        path = self.project / 'scenes' / scene_id / 'scene.json'
        jobs.atomic_json(path, {'version': 3, 'id': scene_id, **document})
        return scene_id, path


def texture_record(path, **fields):
    return {'id': str(uuid.uuid4()), 'kind': 'texture', 'name': 'fixture',
            'artifacts': [{'role': 'texture', 'path': path, 'version': 1}], **fields}


def check_grammar(space, corpus):
    """The shared managed-path corpus through artifact references; valid
    values also serve as record sources."""
    for case in corpus['cases']:
        value = case['value']
        bundle = value.endswith(('.vkb', '.mt', '.fontcfg'))
        record = texture_record('fixture.bin' if bundle else value)
        if case['valid']:
            record['source'] = value
        scene_id, scene_path = space.scene({'assets': [record]})
        if case['valid']:
            artifact = scene_path.parent / record['artifacts'][0]['path']
            artifact.parent.mkdir(parents=True, exist_ok=True)
            artifact.write_bytes(b'fixture')
        code, result = space.run('prepare_scene', scene_id, scene_path=str(scene_path))
        if case['valid']:
            assert code == 0, (case, result)
        else:
            assert code == 1, (case, result)
            assert 'managed path' in result['error'].lower(), (case, result)


def check_link_escape(space):
    outside = space.root / 'outside'
    outside.mkdir()
    (outside / 'file.vkt').write_bytes(b'outside')
    scene_id, scene_path = space.scene({'assets': [texture_record('escape/file.vkt')]})
    link = scene_path.parent / 'escape'
    if os.name == 'nt':
        subprocess.run(['powershell', '-NoProfile', '-Command',
            'New-Item -ItemType Junction -Path $env:VKR_TEST_LINK -Target $env:VKR_TEST_TARGET -ErrorAction Stop | Out-Null'],
            env={**os.environ, 'VKR_TEST_LINK': str(link), 'VKR_TEST_TARGET': str(outside)},
            check=True, capture_output=True)
    else:
        link.symlink_to(outside, target_is_directory=True)
    try:
        code, result = space.run('prepare_scene', scene_id, scene_path=str(scene_path))
        assert code == 1 and 'escapes its owner' in result['error'], result
    finally:
        link.rmdir() if os.name == 'nt' else link.unlink()


def check_migration(space):
    """Only historical source fields are repaired; other strings stay literal."""
    scene_id, scene_path = space.scene({'assets': [
        texture_record('sources/id/mesh.obj', name='literal\\name', source='sources\\id\\mesh.obj',
                       unknown='literal\\data')]})
    (scene_path.parent / 'sources/id').mkdir(parents=True)
    (scene_path.parent / 'sources/id/mesh.obj').write_bytes(b'fixture')
    record = jobs.load_json(scene_path)['assets'][0]
    code, result = space.run('rename_asset', scene_id, scene_path=str(scene_path),
                             asset_id=record['id'], name='literal\\name')
    assert code == 0, result
    migrated = jobs.read_managed_scene(scene_path)['assets'][0]
    assert migrated['source'] == 'sources/id/mesh.obj'
    assert migrated['name'] == 'literal\\name' and migrated['unknown'] == 'literal\\data'
    scene_id, scene_path = space.scene({'assets': [
        texture_record('sources/id/mesh.obj', source='sources\\..\\escape')]})
    code, result = space.run('prepare_scene', scene_id, scene_path=str(scene_path))
    assert code == 1 and 'escapes its owner' in result['error'], result


def snapshot(space, model):
    """Imports `model` without cooking; returns its source snapshot."""
    scene_id = str(uuid.uuid4())
    code, result = space.run('create_scene', scene_id, models=[str(model)],
                             bakes={'prepare_assets': False})
    assert code == 0, result
    scene = jobs.read_managed_scene(result['scene_path'])
    mesh = next(item for item in scene['assets'] if item['kind'] == 'mesh')
    return Path(result['scene_path']).parent / mesh['source']


def check_snapshots(space):
    for name, separator in [('forward', '/'), ('native', os.sep)]:
        source = space.root / name
        (source / 'materials').mkdir(parents=True)
        (source / 'source.obj').write_text('mtllib "materials' + separator + 'surface.mtl" # comment\n',
                                           encoding='utf-8')
        (source / "materials/O'Brien.mtl").write_text('newmtl brien\n', encoding='utf-8')
        (source / 'materials/surface.mtl').write_text('newmtl surface\nmap_Kd "tile%20name.png"\n',
                                                      encoding='utf-8')
        (source / 'materials/tile%20name.png').write_bytes(b'literal percent')
        (source / 'materials/tile name.png').write_bytes(b'incorrect decoded name')
        copied = snapshot(space, source / 'source.obj')
        assert [path.read_bytes() for path in (copied.parent / 'dependencies').iterdir()] == [b'literal percent']
    # An apostrophe is a filename byte, not a quote.
    quoted = space.root / 'apostrophe'
    (quoted / 'materials').mkdir(parents=True)
    (quoted / 'source.obj').write_text("mtllib materials/O'Brien.mtl\n", encoding='utf-8')
    (quoted / "materials/O'Brien.mtl").write_text('newmtl brien\n', encoding='utf-8')
    copied = snapshot(space, quoted / 'source.obj')
    assert "mtllib material_" in copied.read_text(encoding='utf-8')
    if os.name != 'nt':
        source = space.root / 'literal-backslash'
        source.mkdir()
        (source / 'source.obj').write_text('mtllib materials\\surface.mtl\n', encoding='utf-8')
        (source / 'materials\\surface.mtl').write_text('newmtl surface\nmap_Kd tile\\name.png\n',
                                                        encoding='utf-8')
        (source / 'tile\\name.png').write_bytes(b'literal POSIX backslash')
        copied = snapshot(space, source / 'source.obj')
        assert [path.read_bytes() for path in (copied.parent / 'dependencies').iterdir()] == \
            [b'literal POSIX backslash']
    source = space.root / 'gltf'
    source.mkdir()
    (source / 'buffer%20name.bin').write_bytes(b'decode exactly once')
    (source / 'source.gltf').write_text(json.dumps({'asset': {'version': '2.0'},
        'buffers': [{'uri': 'buffer%2520name.bin', 'byteLength': 19}]}), encoding='utf-8')
    copied = snapshot(space, source / 'source.gltf')
    uri = jobs.load_json(copied)['buffers'][0]['uri']
    assert (copied.parent / uri).read_bytes() == b'decode exactly once'
    (source / 'remote.gltf').write_text(json.dumps({'asset': {'version': '2.0'},
        'buffers': [{'uri': 'https://example.com/file.bin', 'byteLength': 1}]}), encoding='utf-8')
    code, result = space.run('create_scene', str(uuid.uuid4()), models=[str(source / 'remote.gltf')],
                             bakes={'prepare_assets': False})
    assert code == 1, result


def check_read_only(space):
    scene_id, scene_path = space.scene({'assets': [
        {'id': 'source', 'kind': 'texture', 'name': 'literal\\name', 'source': 'sources\\source.bin',
         'unknown': 'literal\\data', 'artifacts': [{'role': 'texture', 'path': 'sources/source.bin'}]}]})
    (scene_path.parent / 'sources').mkdir(parents=True)
    (scene_path.parent / 'sources/source.bin').write_bytes(b'preserved source')
    before = scene_path.read_bytes(), space.manifest.read_bytes()
    code, result = space.run('prepare_scene', scene_id, scene_path=str(scene_path), read_only=True,
                             runtime_directory=str(space.root / 'local-runtime'))
    assert code == 0, result
    assert before == (scene_path.read_bytes(), space.manifest.read_bytes())


def check_faces(space):
    scene_id, scene_path = space.scene({
        'assets': [{'id': 'cube', 'name': 'Cube', 'source_kind': 'faces',
                    'base_path': 'builds/fixture/cube', 'extension': 'png',
                    'artifacts': [{'role': 'probe-cube', 'path': 'builds/fixture/cube_r.png'}]}],
        'reflection_probes': [{'enabled': True, 'center': [0, 0, 0], 'extents': [1, 1, 1],
                               'asset': {'scope': 'scene', 'id': 'cube', 'role': 'probe-cube'}}]})
    directory = scene_path.parent / 'builds/fixture'
    directory.mkdir(parents=True)
    for face in jobs.FACES:
        (directory / f'cube_{face}.png').write_bytes(b'fixture')
    code, result = space.run('prepare_scene', scene_id, scene_path=str(scene_path))
    assert code == 0, result
    runtime = jobs.load_json(result['runtime_path'])
    # Compare paths, not spellings: Windows accepts either separator.
    assert Path(runtime['reflection_probes'][0]['cubemap']['base_path']) == (directory / 'cube').resolve()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bakery', default=str(jobs.default_bakery()))
    args = parser.parse_args()
    corpus = json.loads((ROOT / 'tests/fixtures/paths/managed.json').read_text(encoding='utf-8'))
    with tempfile.TemporaryDirectory(prefix='vkr-paths-') as temporary:
        space = Workspace(Path(temporary).resolve(), Path(args.bakery).resolve())
        check_grammar(space, corpus)
        check_link_escape(space)
        check_migration(space)
        check_snapshots(space)
        check_read_only(space)
        check_faces(space)
    print(f"Path contract passed: {len(corpus['cases'])} shared cases, containment, migration, "
          "OBJ/MTL, URI and six-face preparation")


if __name__ == '__main__':
    main()
