#!/usr/bin/env python3
"""Check the `share_project` project job on a small synthetic workspace.

The source workspace holds one project with two scenes. Scene A keeps an old
and a live edit overlay, an old and a live inventory, and an old and a live
build revision whose triangle mesh is cooked by the mesh tool; its material
names a texture relative to itself and a cooked texture with no source. Its
overlay names a workspace material that shadows the content root's, a terrain
heightfield under `.vkreditor/assets/terrain/`, and a missing model. Scene B
names a material only the content root holds, whose texture has a cooked
`.vkt` sibling, and a surface theme whose tag and rule materials, and their
textures, travel with it. The check asserts that the destination holds exactly the live
files and their closure at the workspace paths the editor reads, that stale
overlays, inventories and builds, cooked siblings, locks, job folders and
Scripts build output stay behind, that the overlay copy wins over the content
root, that no file is a hard link of its source, that the result reports
files, bytes, missing references, cooked textures and the build that needs a
rebuild for want of texture recipes, that `.vkrdepotignore` and the merge
units of `.vkrdepotmerge` are written, and that a destination inside the
source workspace or a non-empty
one is refused and left as it was. `untracked_dependencies` then lists the
files the content root serves the project apart from engine content, and
with `adopt` copies them into the workspace, after which it lists none.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import uuid

import project_jobs as jobs

PNG = bytes.fromhex(
    '89504e470d0a1a0a0000000d4948445200000001000000010806000000'
    '1f15c4890000000d49444154789c63f8cfc0f01f0005000201a3e0d1d6'
    '0000000049454e44ae426082')


def triangle(directory, name):
    """A glTF triangle with normals, UVs and one material, beside its .bin."""
    positions = struct.pack('<9f', 0, 0, 0, 1, 0, 0, 0, 1, 0)
    normals = struct.pack('<9f', 0, 0, 1, 0, 0, 1, 0, 0, 1)
    uvs = struct.pack('<6f', 0, 0, 1, 0, 0, 1)
    indices = struct.pack('<3H', 0, 1, 2) + b'\0\0'
    binary = positions + normals + uvs + indices
    (directory / f'{name}.bin').write_bytes(binary)
    (directory / f'{name}.gltf').write_text(json.dumps({
        'asset': {'version': '2.0'},
        'buffers': [{'byteLength': len(binary), 'uri': f'{name}.bin'}],
        'bufferViews': [{'buffer': 0, 'byteLength': 36},
                        {'buffer': 0, 'byteOffset': 36, 'byteLength': 36},
                        {'buffer': 0, 'byteOffset': 72, 'byteLength': 24},
                        {'buffer': 0, 'byteOffset': 96, 'byteLength': 6}],
        'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
                       'min': [0, 0, 0], 'max': [1, 1, 0]},
                      {'bufferView': 1, 'componentType': 5126, 'count': 3, 'type': 'VEC3'},
                      {'bufferView': 2, 'componentType': 5126, 'count': 3, 'type': 'VEC2'},
                      {'bufferView': 3, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}],
        'materials': [{'name': 'red', 'pbrMetallicRoughness': {'baseColorFactor': [1, 0, 0, 1]}}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2},
                                    'indices': 3, 'material': 0}]}],
        'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}))


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, bytes):
        path.write_bytes(data)
    elif isinstance(data, str):
        path.write_text(data)
    else:
        path.write_text(json.dumps(data))


def tree(directory):
    return {path.relative_to(directory).as_posix() for path in directory.rglob('*')
            if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bakery', default=str(jobs.default_bakery()))
    args = parser.parse_args()
    bakery = Path(args.bakery).resolve()
    with tempfile.TemporaryDirectory(prefix='vkr-share-') as temporary:
        root = Path(temporary).resolve()
        content = root / 'content'
        source = root / 'source'
        workspace = source / '.vkreditor'
        project_id = str(uuid.uuid4())
        scene_a = str(uuid.uuid4())
        scene_b = str(uuid.uuid4())
        terrain = str(uuid.uuid4())
        import_id = str(uuid.uuid4())
        live_build = str(uuid.uuid4())
        old_build = str(uuid.uuid4())
        project = workspace / 'projects' / project_id
        a = project / 'scenes' / scene_a
        b = project / 'scenes' / scene_b

        # The content root: a material only it holds, with a source texture
        # and its cooked sibling, and a material the workspace shadows.
        write(content / 'assets/materials/stone.mt',
              'name=stone\nbase_color_texture=./../textures/stone.png?cs=srgb&tc=color_srgb\n')
        write(content / 'assets/textures/stone.png', PNG)
        write(content / 'assets/textures/stone.png.vkt', b'cooked stone')
        write(content / 'assets/materials/overlay.mt', 'name=content copy\n')

        # The workspace overlay of assets/ and the terrain heightfield.
        write(source / 'assets/materials/overlay.mt',
              'name=overlay\nnormal_texture=assets/textures/overlay.png\n')
        write(source / 'assets/textures/overlay.png', PNG)
        write(workspace / f'assets/terrain/{terrain}.vkrhf', b'heightfield')
        write(workspace / 'workspace.json', {'version': 1, 'id': str(uuid.uuid4())})
        write(workspace / 'jobs/old/request.json', {})
        write(workspace / 'VkrEditorBakery.lock', b'')

        # Project documents, Scripts sources and their build output.
        write(project / 'project.json', {
            'version': 1, 'id': project_id, 'name': 'Share Fixture', 'assets': [],
            'default_font': None,
            'scenes': [{'id': scene_a, 'name': 'A', 'path': f'scenes/{scene_a}/scene.json'},
                       {'id': scene_b, 'name': 'B', 'path': f'scenes/{scene_b}/scene.json'}]})
        write(project / 'game.json', {'version': 1, 'game': {'name': 'Share Fixture'}})
        write(project / 'world.scene.json', {'version': 2, 'entities': []})
        write(project / 'world.editor.json', {'version': 5, 'overrides': []})
        write(project / f'project-{project_id}.lock', b'')
        write(project / 'Scripts/Spin/Spin.script.json', {'version': 1, 'kind': 'module'})
        write(project / 'Scripts/Spin/spin.c', 'void vkr_module_Spin(void) {}\n')
        write(project / 'Scripts/.build/spin.o', b'object')
        write(project / '.staging/leftover/file.txt', 'staged')

        # Scene A: the live build is a cooked triangle with a material that
        # names a relative cooked texture without a source and a workspace
        # texture; the old build and old documents are stale.
        sources = a / 'sources' / import_id
        sources.mkdir(parents=True)
        triangle(sources, 'source')
        for build in (live_build, old_build):
            revision = a / 'builds' / build
            revision.mkdir(parents=True)
            cooked = subprocess.run(
                [str(bakery), 'tool', 'mesh', '--input', str(sources / 'source.gltf'),
                 '--output', str(revision / 'mesh.vkb'), '--bundle-root', str(revision),
                 '--import-id', import_id], capture_output=True, text=True)
            assert cooked.returncode == 0, cooked.stdout + cooked.stderr
        live = a / 'builds' / live_build
        material = next((live / 'materials').glob('*.mt'))
        with material.open('a') as handle:
            handle.write('base_color_texture=./../textures/packed.vkt?cs=srgb\n'
                         'emissive_texture=assets/textures/overlay.png\n')
        write(live / 'textures/packed.vkt', b'cooked without source')
        material_path = material.relative_to(a).as_posix()
        record = {'id': str(uuid.uuid4()), 'kind': 'mesh', 'name': 'triangle', 'import_id': import_id,
                  'import_revision': live_build,
                  'source': f'sources/{import_id}/source.gltf',
                  'artifacts': [{'role': 'mesh', 'path': f'builds/{live_build}/mesh.vkb', 'version': 1}],
                  'closure': {f'builds/{live_build}/mesh.vkb': '0' * 64, material_path: '0' * 64}}
        live_inventory = f'inventory/{uuid.uuid4()}.json'
        old_inventory = f'inventory/{uuid.uuid4()}.json'
        live_overlay = f'edits/{uuid.uuid4()}.json'
        old_overlay = f'edits/{uuid.uuid4()}.json'
        write(a / live_inventory, {'version': 1, 'assets': [record]})
        write(a / old_inventory, {'version': 1, 'assets': [dict(
            record, artifacts=[{'role': 'mesh', 'path': f'builds/{old_build}/mesh.vkb', 'version': 1}],
            closure={})]})
        write(a / live_overlay, {'version': 5, 'overrides': [
            {'material': 'assets/materials/overlay.mt'},
            {'terrain': {'heightfield': f'assets/terrain/{terrain}.vkrhf'}},
            {'mesh': 'assets/models/missing.vkb'}]})
        write(a / old_overlay, {'version': 5, 'overrides': [{'material': 'assets/materials/old.mt'}]})
        write(a / 'imports' / f'{import_id}.json', {'version': 1, 'id': import_id})
        write(a / 'imports' / f'{import_id}-{live_build}.json', {'version': 1, 'id': import_id})
        write(a / 'imports' / f'{import_id}-{old_build}.json', {'version': 1, 'id': import_id})
        write(a / 'scene.json', {'version': 5, 'id': scene_a, 'entities': [], 'reflection_probes': [],
                                 'default_font': None, 'edit_overlay': live_overlay,
                                 'inventory': live_inventory})
        write(a / f'scene-{scene_a}.lock', b'')

        # Scene B names a material only the content root holds, and a
        # surface theme whose tag and rule materials only it names.
        write(source / 'assets/surfaces/lab.surfaces', {
            'version': 2, 'materials': {'plaster': 'assets/materials/lab_wall.mt'},
            'rules': [{'tags': ['#lab'], 'faces': 'floor', 'material': 'assets/materials/lab_floor.mt'}]})
        write(source / 'assets/materials/lab_wall.mt',
              'name=lab wall\nbase_color_texture=assets/textures/lab_wall.png\n')
        write(source / 'assets/textures/lab_wall.png', PNG)
        write(source / 'assets/materials/lab_floor.mt', 'name=lab floor\n')
        b_overlay = f'edits/{uuid.uuid4()}.json'
        write(b / b_overlay, {'version': 5, 'overrides': [{'material': 'assets/materials/stone.mt'}],
                              'theme': 'assets/surfaces/lab.surfaces'})
        write(b / 'scene.json', {'version': 5, 'id': scene_b, 'entities': [], 'reflection_probes': [],
                                 'default_font': None, 'edit_overlay': b_overlay})

        base = {'version': 1, 'operation': 'share_project', 'workspace_root': str(workspace),
                'project_path': str(project / 'project.json'), 'legacy_root': str(content)}
        result_path = root / 'result.json'
        source_before = tree(source)

        # A destination inside the source workspace is refused.
        inside = source / 'shared'
        job = jobs.Job(dict(base, destination=str(inside)), result_path, bakery)
        assert job.execute() == 1, job.output
        assert 'outside the source workspace' in jobs.load_json(result_path)['error']
        assert not inside.exists() and tree(source) == source_before

        # A non-empty destination is refused and kept.
        occupied = root / 'occupied'
        write(occupied / 'notes.txt', 'keep me')
        job = jobs.Job(dict(base, destination=str(occupied)), result_path, bakery)
        assert job.execute() == 1, job.output
        assert 'empty or absent' in jobs.load_json(result_path)['error']
        assert tree(occupied) == {'notes.txt'}

        destination = root / 'shared' / 'Share Fixture'
        job = jobs.Job(dict(base, destination=str(destination)), result_path, bakery)
        assert job.execute() == 0, job.output
        result = jobs.load_json(result_path)
        assert tree(source) == source_before, 'the source workspace is unchanged'

        p = f'.vkreditor/projects/{project_id}'
        pa = f'{p}/scenes/{scene_a}'
        pb = f'{p}/scenes/{scene_b}'
        mesh_materials = {f'{pa}/{path.relative_to(a).as_posix()}'
                          for path in (live / 'materials').glob('*.mt')}
        expected = {
            '.vkrdepotignore', '.vkrdepotmerge',
            f'{p}/project.json', f'{p}/game.json', f'{p}/world.scene.json', f'{p}/world.editor.json',
            f'{p}/Scripts/Spin/Spin.script.json', f'{p}/Scripts/Spin/spin.c',
            f'{pa}/scene.json', f'{pa}/{live_overlay}', f'{pa}/{live_inventory}',
            f'{pa}/builds/{live_build}/mesh.vkb', f'{pa}/builds/{live_build}/mesh.vkb.remap.json',
            f'{pa}/builds/{live_build}/textures/packed.vkt',
            f'{pa}/sources/{import_id}/source.gltf', f'{pa}/sources/{import_id}/source.bin',
            f'{pa}/imports/{import_id}.json', f'{pa}/imports/{import_id}-{live_build}.json',
            f'{pb}/scene.json', f'{pb}/{b_overlay}',
            f'.vkreditor/assets/terrain/{terrain}.vkrhf',
            'assets/materials/overlay.mt', 'assets/textures/overlay.png',
            'assets/materials/stone.mt', 'assets/textures/stone.png',
            'assets/surfaces/lab.surfaces', 'assets/materials/lab_wall.mt',
            'assets/textures/lab_wall.png', 'assets/materials/lab_floor.mt',
        } | mesh_materials
        shared = tree(destination)
        assert shared == expected, (sorted(shared - expected), sorted(expected - shared))
        assert not list(destination.parent.glob('.*share-*')), 'no staging folder is left'

        # The workspace overlay wins over the content root, and every file is
        # its own copy.
        assert (destination / 'assets/materials/overlay.mt').read_text().startswith('name=overlay')
        for relative in shared - {'.vkrdepotignore', '.vkrdepotmerge'}:
            copy = destination / relative
            origin = (workspace / relative[len('.vkreditor/'):] if relative.startswith('.vkreditor/')
                      else source / relative if (source / relative).is_file() else content / relative)
            assert copy.read_bytes() == origin.read_bytes(), relative
            assert copy.stat().st_ino != origin.stat().st_ino and copy.stat().st_nlink == 1, relative

        assert result['status'] == 'complete', result
        assert result['destination'] == destination.as_posix(), result
        assert result['project_path'] == (destination / p / 'project.json').as_posix(), result
        assert result['files'] == len(expected) - 2, result
        assert result['bytes'] == sum((destination / f).stat().st_size for f in expected
                                      if f not in ('.vkrdepotignore', '.vkrdepotmerge')), result
        assert result['missing'] == ['assets/models/missing.vkb'], result['missing']
        assert result['missing_count'] == 1, result
        assert result['cooked_without_source'] == [f'{pa[len(".vkreditor/"):]}/builds/{live_build}'
                                                   '/textures/packed.vkt'], result
        # The live build holds a cooked texture no recipe covers, as a cook
        # before texture recipes left it: the editor offers its Rebuild.
        assert result['needs_rebuild'] == [{'scene': scene_a, 'asset_id': record['id'],
                                            'name': 'triangle'}], result['needs_rebuild']
        assert [scene['id'] for scene in result['scenes']] == [scene_a, scene_b], result
        assert sum(scene['files'] for scene in result['scenes']) + result['project']['files'] == \
            result['files'], result
        assert any('missing' in warning for warning in result['warnings']), result

        ignore = (destination / '.vkrdepotignore').read_text().splitlines()
        for pattern in ('.vkreditor/jobs/', '.vkreditor/logs/', '.vkreditor/cache/', '.vkreditor/build/',
                        '.vkreditor/editor/', '.vkreditor/workspace.json', '.vkreditor/local/',
                        '.runtime/', '*.vkt', '*.lock'):
            assert pattern in ignore, pattern
        assert all(line.startswith('#') or not line or line in ignore for line in ignore)

        # A second share into the now non-empty destination is refused.
        job = jobs.Job(dict(base, destination=str(destination)), result_path, bakery)
        assert job.execute() == 1, job.output
        assert tree(destination) == expected

        # Untracked dependencies: what the content root serves this project
        # and git does not track there. This content root is no checkout, so
        # only an identity git tracks in the repository counts as engine
        # content; the workspace's own copies are the project's already.
        write(content / 'assets/materials/default.world.mt', 'name=engine\n')
        write(b / b_overlay, {'version': 5, 'overrides': [
            {'material': 'assets/materials/stone.mt'},
            {'material': 'assets/materials/default.world.mt'}]})
        dependencies = dict(base, operation='untracked_dependencies')
        job = jobs.Job(dependencies, result_path, bakery)
        assert job.execute() == 0, job.output
        listed = jobs.load_json(result_path)
        assert sorted(entry['path'] for entry in listed['untracked']) == \
            ['assets/materials/stone.mt', 'assets/textures/stone.png'], listed
        assert listed['adopted'] == 0 and not (source / 'assets/materials/stone.mt').exists()
        job = jobs.Job(dict(dependencies, adopt=True), result_path, bakery)
        assert job.execute() == 0, job.output
        assert jobs.load_json(result_path)['adopted'] == 2
        assert (source / 'assets/materials/stone.mt').read_bytes() == \
            (content / 'assets/materials/stone.mt').read_bytes()
        job = jobs.Job(dependencies, result_path, bakery)
        assert job.execute() == 0, job.output
        assert jobs.load_json(result_path)['untracked'] == [], 'Adopted files are the project\'s'
    print('Bakery share: live files, stale overlays/inventories/builds left out, asset closure through '
          'the workspace overlay, content root and terrain folder, cooked siblings skipped, cooked '
          'textures without sources reported, missing references reported, ignore file, refused '
          'destinations, untracked dependencies and their adoption passed')


if __name__ == '__main__':
    main()
