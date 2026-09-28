#!/usr/bin/env python3
"""CPU transaction checks: copied imports, relocation, missing assets and rollback."""
import argparse
import base64
import copy
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import uuid
import zlib


def tool_command(executable, tool):
    """Command prefix for a cooker; vkr_bakery runs cookers as `tool <name>`."""
    if Path(executable).stem == 'vkr_bakery':
        return [str(executable), 'tool', tool]
    return [str(executable)]

def write_png(path, width, height, rgb):
    """Writes an opaque 8-bit RGB PNG filled with one color."""
    raw = b''.join(b'\0' + bytes(rgb) * width for _ in range(height))

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def paired_gltf(path, normal_uri, roughness_factors):
    """One triangle per material; every material shares one normal map."""
    positions = struct.pack('<9f', 0, 0, 0, 1, 0, 0, 0, 1, 0)
    uvs = struct.pack('<6f', 0, 0, 1, 0, 0, 1)
    indices = struct.pack('<3H', 0, 1, 2) + b'\0\0'
    binary = positions + uvs + indices
    count = len(roughness_factors)
    path.write_text(json.dumps({'asset': {'version': '2.0'},
        'buffers': [{'byteLength': len(binary),
                     'uri': 'data:application/octet-stream;base64,' + base64.b64encode(binary).decode()}],
        'bufferViews': [{'buffer': 0, 'byteLength': 36}, {'buffer': 0, 'byteOffset': 36, 'byteLength': 24},
                        {'buffer': 0, 'byteOffset': 60, 'byteLength': 6}],
        'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
                       'min': [0, 0, 0], 'max': [1, 1, 0]},
                      {'bufferView': 1, 'componentType': 5126, 'count': 3, 'type': 'VEC2'},
                      {'bufferView': 2, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}],
        'images': [{'uri': normal_uri}],
        'samplers': [{'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497}],
        'textures': [{'source': 0, 'sampler': 0}],
        'materials': [{'normalTexture': {'index': 0},
                       'pbrMetallicRoughness': {'roughnessFactor': factor}}
                      for factor in roughness_factors],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'TEXCOORD_0': 1},
                                    'indices': 2, 'material': index}]}
                   for index in range(count)],
        'nodes': [{'mesh': index} for index in range(count)],
        'scenes': [{'nodes': list(range(count))}], 'scene': 0}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesh-cooker', required=True)
    parser.add_argument('--font-cooker')
    parser.add_argument('--texture-packer', required=True)
    parser.add_argument('--diffuse-baker')
    parser.add_argument('--collision-cooker')
    args = parser.parse_args()
    import project_jobs as jobs
    with tempfile.TemporaryDirectory(prefix='vkr-project-job-') as temporary:
        root = Path(temporary)
        workspace = root / 'workspace' / '.vkreditor'
        project = workspace / 'projects' / str(uuid.uuid4())
        project.mkdir(parents=True)
        manifest = project / 'project.json'
        manifest.write_text('{"version":1,"assets":[],"scenes":[],"default_font":null}')
        original_manifest = manifest.read_bytes()
        source = root / 'sources'
        (source / 'nested').mkdir(parents=True)
        # No renderer invocation: this triangle isolates source-copy semantics.
        model = source / 'test.obj'
        model.write_text('mtllib nested/test.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n'
                         'vt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\n'
                         'usemtl test\nf 1/1/1 2/2/1 3/3/1\n')
        (source / 'nested' / 'test.mtl').write_text('newmtl test\nKd 1 0 0\nmap_Kd pixel.png\n')
        (source / 'nested' / 'pixel.png').write_bytes(base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aTWQAAAAASUVORK5CYII='))
        request = {'version': 1, 'operation': 'create_scene',
                   'workspace_root': str(workspace), 'project_path': str(manifest),
                   'scene_id': str(uuid.uuid4()), 'scene_name': 'Copied import',
                   'models': [str(model)], 'bakes': {},
                   'tools': {'mesh': str(Path(args.mesh_cooker).resolve()), 'texture': str(Path(args.texture_packer).resolve())}}
        if args.collision_cooker:
            request['tools']['collision'] = str(Path(args.collision_cooker).resolve())
        result_path = root / 'result.json'
        # An unresolved LFS source must fail before publishing a scene.
        pointer = source / 'pointer.obj'
        pointer.write_text('version https://git-lfs.github.com/spec/v1\n'
                           'oid sha256:' + '0' * 64 + '\nsize 12659983\n')
        pointer_request = dict(request, scene_id=str(uuid.uuid4()), models=[str(pointer)],
                               atmosphere={'enabled': True}, environment={'intensity': 0.5})
        assert jobs.Job(pointer_request, result_path).execute() == 1
        assert 'Git LFS pointer' in result_path.read_text()
        assert not (project / 'scenes' / pointer_request['scene_id']).exists()
        assert manifest.read_bytes() == original_manifest
        assert not list((project / '.staging').iterdir())
        # Resolved content publishes; a physical-sky request keeps its sky-light
        # scale and starts with the default cloud layer.
        pointer.write_bytes(model.read_bytes())
        assert jobs.Job(pointer_request, result_path).execute() == 0
        pointer_runtime = jobs.load_json(jobs.load_json(result_path)['runtime_path'])
        assert Path(pointer_runtime['entities'][0]['mesh']['path']).is_file()
        assert pointer_runtime['atmosphere'] == {'enabled': True}
        assert pointer_runtime['clouds'] == {'enabled': True}
        assert pointer_runtime['environment'] == {'intensity': 0.5, 'enabled': True}
        assert jobs.Job(request, result_path).execute() == 0
        result = jobs.load_json(result_path)
        scene_path = Path(result['scene_path'])
        managed = jobs.read_managed_scene(scene_path)
        runtime = jobs.load_json(result['runtime_path'])
        assert managed['version'] == 5 and runtime['version'] == 2
        # Scene v4 and later keep records in an immutable inventory revision.
        stored = jobs.load_json(scene_path)
        assert 'assets' not in stored and (scene_path.parent / stored['inventory']).is_file()
        # Version 5 keeps entity blocks in components; the runtime keeps each
        # entity's document id, which overlays bind through (ADR-076).
        assert 'mesh' not in stored['entities'][0] and 'mesh' in stored['entities'][0]['components']
        assert [entity['id'] for entity in runtime['entities']] == [entity['id'] for entity in managed['entities']]
        first_id, second_id = str(uuid.uuid4()), str(uuid.uuid4())
        internal = [{'id': first_id, 'name': 'A', 'parent': None,
                     'transform': {'pos': [0, 0, 0], 'rot': [0, 0, 0, 1], 'scale': [1, 1, 1]}},
                    {'id': second_id, 'name': 'B', 'parent': 0, 'shape': {'type': 'cube'}}]
        document = jobs.document_from_internal(copy.deepcopy({'entities': internal}))
        assert document['entities'][1]['parent'] == first_id
        assert document['entities'][1]['components'] == {'shape': {'type': 'cube'}}
        assert jobs.document_to_internal(document)['entities'] == internal
        overlay = {'version': 5, 'document_ids': [first_id, second_id],
                   'overrides': [{'scene_entity': 1, 'gltf_node': -1}, {'scene_entity': 0, 'gltf_node': -1}]}
        jobs.remap_overlay_indices(overlay, [internal[1], {'id': str(uuid.uuid4())}])
        assert [record['scene_entity'] for record in overlay['overrides']] == [0, -1]
        assert overlay['document_ids'][0] == second_id
        assert manifest.read_bytes() == original_manifest, 'Job published project membership'
        assert '.staging' not in json.dumps(managed)
        assert str(source) not in json.dumps(managed)
        assert len(managed['assets']) >= 2
        assert all('\\' not in item.get('source', '') for item in managed['assets'] if item.get('source'))
        assert any(item['kind'] == 'material' and item['name'] == 'test' for item in managed['assets'])
        assert any(item['kind'] == 'texture' and item['name'] == 'pixel' for item in managed['assets'])
        assert Path(runtime['entities'][0]['mesh']['path']).is_file()
        unbuilt_request = dict(request, scene_id=str(uuid.uuid4()), bakes={'prepare_assets': False})
        assert jobs.Job(unbuilt_request, result_path).execute() == 0
        unbuilt_result = jobs.load_json(result_path)
        assert unbuilt_result['status'] == 'unbuilt' and 'runtime_path' not in unbuilt_result
        unbuilt_scene = jobs.read_managed_scene(unbuilt_result['scene_path'])
        assert not unbuilt_scene['assets'][0]['artifacts']
        finish = dict(unbuilt_request, operation='prepare_scene', scene_path=unbuilt_result['scene_path'])
        assert jobs.Job(finish, result_path).execute() == 0
        assert jobs.load_json(result_path)['status'] == 'complete'
        assert jobs.read_managed_scene(unbuilt_result['scene_path'])['assets'][0]['artifacts']
        if args.diffuse_baker:
            # An open triangle encloses no room: the real baker's inspection
            # proves every cell invalid, so the scene publishes without a volume.
            open_request = dict(request, scene_id=str(uuid.uuid4()), bakes={'diffuse': True},
                                diffuse_settings={'grid': [2, 2, 2], 'bounds': [-1, -1, -1, 2, 2, 2]},
                                tools=dict(request['tools'], diffuse=str(Path(args.diffuse_baker).resolve())))
            assert jobs.Job(open_request, result_path).execute() == 0
            open_result = jobs.load_json(result_path)
            assert any('Diffuse volume skipped' in warning for warning in open_result['warnings'])
            open_scene = jobs.read_managed_scene(open_result['scene_path'])
            assert 'diffuse_volume' not in open_scene
            assert not any(item['kind'] == 'volume' for item in open_scene['assets'])
            assert 'diffuse_volume' not in jobs.load_json(open_result['runtime_path'])
            builds = Path(open_result['scene_path']).parent / 'builds'
            assert all(any(revision.iterdir()) for revision in builds.iterdir()), 'Skipped bake left a revision'
            # Only the no-room status is accepted; an invalid recipe still fails.
            invalid_request = dict(open_request, scene_id=str(uuid.uuid4()), diffuse_settings={'grid': [1, 2, 2]})
            assert jobs.Job(invalid_request, result_path).execute() == 1
            assert 'Baking diffuse volume failed (exit 1)' in jobs.load_json(result_path)['error']
            assert not (project / 'scenes' / invalid_request['scene_id']).exists()
        # Materials sharing a normal map share its encoded normal; only the
        # roughness pair varies by factor. Derived variants live once in the
        # workspace cache, bundles hold byte-identical copies, and a second
        # import reuses the cache instead of recooking.
        write_png(source / 'shared_normal.png', 8, 8, (128, 128, 255))
        paired_model = source / 'paired.gltf'
        paired_gltf(paired_model, 'shared_normal.png', [0.5, 0.8])
        paired_request = dict(request, scene_id=str(uuid.uuid4()), models=[str(paired_model)])
        assert jobs.Job(paired_request, result_path).execute() == 0
        generated = workspace / 'cache' / 'generated' / 'normalrough_v2'
        normals = sorted(generated.glob('*_normal*.vkt'))
        roughness = sorted(generated.glob('*_metalrough*.vkt'))
        assert len(normals) == 1 and len(roughness) == 2, (normals, roughness)
        assert '_roughness_' not in normals[0].name
        paired_root = Path(jobs.load_json(result_path)['scene_path']).parent
        assert not list(paired_root.glob('builds/*/textures/generated')), 'Bundle kept derived intermediates'
        paired_textures = {}
        for material in paired_root.glob('builds/*/materials/*.mt'):
            for line in material.read_text(encoding='utf-8').splitlines():
                key, _, value = line.partition('=')
                if key in ('normal_texture', 'metallic_roughness_texture'):
                    resolved = (material.parent / value.split('?')[0]).resolve()
                    paired_textures.setdefault(key, set()).add(resolved)
        assert len(paired_textures['normal_texture']) == 1
        assert len(paired_textures['metallic_roughness_texture']) == 2
        cached_bytes = {path.read_bytes() for path in [*normals, *roughness]}
        assert all(path.read_bytes() in cached_bytes
                   for paths in paired_textures.values() for path in paths)
        stamps = {path: path.stat().st_mtime_ns for path in [*normals, *roughness]}
        reuse_request = dict(paired_request, scene_id=str(uuid.uuid4()))
        assert jobs.Job(reuse_request, result_path).execute() == 0
        assert {path: path.stat().st_mtime_ns for path in stamps} == stamps, 'Cached variants were recooked'
        assert len(list(generated.glob('*.vkt'))) == 3

        # Source-node transforms use an immutable geometry-preserving bake variant.
        gltf_path = source / 'node.gltf'
        binary = struct.pack('<9f3H', 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 2)
        gltf_path.write_text(json.dumps({'asset': {'version': '2.0'},
            'buffers': [{'byteLength': len(binary), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(binary).decode()}],
            'bufferViews': [{'buffer': 0, 'byteLength': 36}, {'buffer': 0, 'byteOffset': 36, 'byteLength': 6}],
            'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3', 'min': [0, 0, 0], 'max': [1, 1, 0]},
                          {'bufferView': 1, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}],
            'materials': [{}], 'meshes': [{'primitives': [{'attributes': {'POSITION': 0}, 'indices': 1, 'material': 0}]}],
            'extensionsUsed': ['KHR_lights_punctual'], 'extensions': {'KHR_lights_punctual': {'lights': [{'type': 'point'}]}},
            'nodes': [{'mesh': 0, 'extensions': {'KHR_lights_punctual': {'light': 0}}},
                      {'children': [0], 'matrix': [1, 0, 0, 0, 0.25, 1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 1]}],
            'scenes': [{'nodes': [1]}], 'scene': 0}))
        node_request = dict(request, scene_id=str(uuid.uuid4()), models=[str(gltf_path)])
        assert jobs.Job(node_request, result_path).execute() == 0
        # A repository glTF keeps the mesh cooker's texture layout: an image it
        # names under objects/ lives under assets/textures. Outside the
        # repository's assets tree the same model must fail instead of guessing.
        legacy = root / 'legacy'
        texture = legacy / 'assets' / 'textures' / 'props' / 'pixel.png'
        texture.parent.mkdir(parents=True)
        texture.write_bytes((source / 'nested' / 'pixel.png').read_bytes())
        textured = json.loads(gltf_path.read_text())
        textured['images'] = [{'uri': 'objects/props/pixel.png'}]
        textured['textures'] = [{'source': 0}]  # Only sampled images are sources.
        repository_model = legacy / 'assets' / 'models' / 'legacy.gltf'
        repository_model.parent.mkdir(parents=True)
        repository_model.write_text(json.dumps(textured))
        legacy_request = dict(request, scene_id=str(uuid.uuid4()),
                              models=[str(repository_model)], legacy_root=str(legacy))
        legacy_result = root / 'legacy_result.json'
        assert jobs.Job(legacy_request, legacy_result).execute() == 0
        legacy_root = Path(jobs.load_json(legacy_result)['scene_path']).parent
        snapshot = next(legacy_root.glob('sources/*/source.gltf'))
        copied = snapshot.parent / jobs.load_json(snapshot)['images'][0]['uri']
        assert copied.read_bytes() == texture.read_bytes()
        outside_model = source / 'outside.gltf'
        outside_model.write_text(json.dumps(textured))
        outside_request = dict(legacy_request, scene_id=str(uuid.uuid4()),
                               models=[str(outside_model)])
        assert jobs.Job(outside_request, legacy_result).execute() == 1
        assert 'Source file is unavailable' in legacy_result.read_text()
        node_result = jobs.load_json(result_path)
        node_scene = jobs.read_managed_scene(node_result['scene_path'])
        node_runtime = jobs.load_json(node_result['runtime_path'])
        node_mesh = Path(node_runtime['entities'][0]['mesh']['path'])
        node_job = jobs.Job(node_request, result_path)
        node_info = node_job.inspect_mesh(node_mesh)
        node_hash = jobs.mesh_identity(jobs.source_fingerprint(node_request['scene_id'].encode()), node_info['fingerprint'])
        node_root = Path(node_result['scene_path']).parent
        jobs.atomic_json(node_root / 'edits' / 'node.json', {'version': 1, 'overrides': [
            {'scene_entity': 0, 'gltf_node': 0, 'source_fingerprint': node_hash, 'fields': 9,
             'position': [5, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1],
             'point_color': [1, 0.5, 0.25], 'point_direction': [1, 0, 0],
             'point_params': [10, 1, 0.1, 0.2, 20, 0, 0.7], 'point_kind': 0,
             'point_enabled': True, 'point_casts_shadow': True}]})
        node_scene['edit_overlay'] = 'edits/node.json'
        effective_node = node_job.effective_bake_runtime(node_scene, node_root)
        variant_path = Path(jobs.load_json(effective_node['runtime_path'])['entities'][0]['mesh']['path'])
        original_bytes, variant_bytes = node_mesh.read_bytes(), variant_path.read_bytes()
        offset = struct.unpack_from('<Q', original_bytes, 80)[0]
        assert original_bytes != variant_bytes and original_bytes[offset:] == variant_bytes[offset:]
        assert node_job.inspect_mesh(variant_path)['fingerprint'] == node_info['fingerprint']
        baked_nodes = jobs.load_json(effective_node['runtime_path'])['entities']
        lights = [entity['point_light'] for entity in baked_nodes if 'point_light' in entity]
        assert len(lights) == 1 and lights[0]['kind'] == 0 and lights[0]['direction_local'] == [1, 0, 0]
        assert any(entity.get('transform', {}).get('matrix', [0] * 16)[4] == 0.25 for entity in baked_nodes)
        if args.diffuse_baker:
            import subprocess
            check = subprocess.run([*tool_command(Path(args.diffuse_baker).resolve(), 'diffuse-baker'), '--scene', effective_node['runtime_path'],
                '--inspect', '--manifest', str(root / 'edited-light.json'), '--grid', '2', '2', '2',
                '--bounds', '-1', '-1', '-1', '10', '10', '10'], capture_output=True, text=True)
            assert check.returncode == 0, check.stdout + check.stderr
            assert 'lights=1 ' in check.stdout, 'Source punctual was duplicated or edited light omitted'

        # Physics journals preserve nested identities and own their cooked closure.
        for version in (2, 3):
            journal = jobs.load_json(node_root / 'edits' / 'node.json')
            journal['version'] = version
            jobs.atomic_json(node_root / 'edits' / 'node.json', journal)
            node_job.effective_bake_runtime(node_scene, node_root)
        if args.collision_cooker:
            import subprocess
            cooked = workspace / 'shared-shape.vkc'
            check = subprocess.run([*tool_command(request['tools']['collision'], 'collision'), '--input', str(gltf_path),
                '--output', str(cooked), '--kind', 'mesh'], capture_output=True, text=True)
            assert check.returncode == 0, check.stdout + check.stderr
            cooked_bytes = cooked.read_bytes()
            journal = {'version': 3, 'collision_settings': {'version': 1, 'names': ['Layer ' + str(i) for i in range(16)], 'matrix': [65535] * 16}, 'overrides': [{
                'scene_entity': 0, 'gltf_node': 0, 'source_fingerprint': node_hash,
                'fields': 0, 'physics': {
                    'attachment': {'enabled': True, 'source': {
                        'scene_entity': 0, 'gltf_node': 1, 'fingerprint': node_hash}},
                    'joints': [{'enabled': True, 'target': {
                        'scene_entity': 0, 'gltf_node': 1, 'fingerprint': node_hash}}],
                    'colliders': [{'shape': 4, 'asset': 'shared-shape.vkc'}]}}]}
            jobs.atomic_json(node_root / 'edits' / 'node.json', journal)
            jobs.write_managed_scene(node_root / 'scene.json', node_scene)
            clone = dict(request, scene_id=str(uuid.uuid4()), models=[],
                         source_scene=str(node_root / 'scene.json'))
            assert jobs.Job(clone, result_path).execute() == 0, result_path.read_text()
            clone_result = jobs.load_json(result_path)
            clone_path = Path(clone_result['scene_path'])
            clone_scene = jobs.read_managed_scene(clone_path)
            cloned = jobs.load_json(clone_path.parent / clone_scene['edit_overlay'])
            assert cloned['version'] == 3 and cloned['collision_settings'] == journal['collision_settings']
            record = cloned['overrides'][0]
            clone_hash = jobs.mesh_identity(jobs.source_fingerprint(clone['scene_id'].encode()), node_info['fingerprint'])
            assert record['source_fingerprint'] == clone_hash
            assert record['physics']['attachment']['source']['fingerprint'] == clone_hash
            assert record['physics']['joints'][0]['target']['fingerprint'] == clone_hash
            copied_asset = record['physics']['colliders'][0]['asset']
            assert copied_asset.startswith(f'projects/{project.name}/scenes/{clone["scene_id"]}/builds/collisions/')
            assert (workspace / copied_asset).read_bytes() == cooked_bytes
            assert jobs.load_json(node_root / 'edits' / 'node.json') == journal
            # Legacy collider paths use the runtime project root, not scene-file location.
            legacy_physics = source / 'legacy-physics.json'
            jobs.atomic_json(legacy_physics, {'version': 2, 'source_identity': 'physics-legacy',
                'entities': [{'mesh': {'path': str(node_mesh)}}]})
            legacy_journal = json.loads(json.dumps(journal))
            legacy_hash = jobs.mesh_identity(jobs.source_fingerprint(b'physics-legacy'), node_info['fingerprint'])
            for reference, field, required in jobs.overlay_references(legacy_journal['overrides'][0]):
                reference[field] = legacy_hash
            jobs.atomic_json(Path(str(legacy_physics) + '.editor.json'), legacy_journal)
            legacy_clone = dict(clone, scene_id=str(uuid.uuid4()), source_scene=str(legacy_physics),
                                legacy_root=str(workspace))
            assert jobs.Job(legacy_clone, result_path).execute() == 0, result_path.read_text()
            legacy_result = jobs.load_json(result_path)
            legacy_scene = jobs.read_managed_scene(legacy_result['scene_path'])
            legacy_overlay = jobs.load_json(Path(legacy_result['scene_path']).parent / legacy_scene['edit_overlay'])
            assert (workspace / legacy_overlay['overrides'][0]['physics']['colliders'][0]['asset']).read_bytes() == cooked_bytes
            # Corrupt native geometry fails atomically before a destination appears.
            damaged = bytearray(cooked_bytes)
            damaged[-1] ^= 1
            cooked.write_bytes(damaged)
            bad_clone = dict(clone, scene_id=str(uuid.uuid4()))
            assert jobs.Job(bad_clone, result_path).execute() == 1
            assert not (project / 'scenes' / bad_clone['scene_id']).exists()
            assert not list((project / '.staging').iterdir())
            cooked.write_bytes(cooked_bytes)
            journal['overrides'][0]['physics']['joints'][0]['target']['fingerprint'] = '0123456789abcdef'
            jobs.atomic_json(node_root / 'edits' / 'node.json', journal)
            bad_clone = dict(clone, scene_id=str(uuid.uuid4()))
            assert jobs.Job(bad_clone, result_path).execute() == 1
            assert not (project / 'scenes' / bad_clone['scene_id']).exists()
            # Detaching a cloned scene must not need the original workspace.
            detached = root / 'detached-scene'
            shutil.copytree(clone_path.parent, detached)
            cooked.unlink()
            detached_clone = dict(clone, scene_id=str(uuid.uuid4()), source_scene=str(detached / 'scene.json'))
            assert jobs.Job(detached_clone, result_path).execute() == 0, result_path.read_text()
            detached_result = jobs.load_json(result_path)
            detached_scene = jobs.read_managed_scene(detached_result['scene_path'])
            detached_overlay = jobs.load_json(Path(detached_result['scene_path']).parent / detached_scene['edit_overlay'])
            assert (workspace / detached_overlay['overrides'][0]['physics']['colliders'][0]['asset']).read_bytes() == cooked_bytes
            # Restore the source journal for independent relocation checks below.
            node_scene.pop('edit_overlay')
            jobs.write_managed_scene(node_root / 'scene.json', node_scene)

        model.unlink()
        assert any((scene_path.parent / 'sources').rglob('source.obj'))

        # Cooked-only migration inventories transitive materials with the real decoder.
        legacy = root / 'legacy.json'
        legacy.write_text(json.dumps({'version': 2, 'entities': [
            {'mesh': {'path': runtime['entities'][0]['mesh']['path']}}]}))
        copied_request = dict(request, scene_id=str(uuid.uuid4()), models=[], source_scene=str(legacy))
        assert jobs.Job(copied_request, result_path).execute() == 0
        copied_runtime = jobs.load_json(jobs.load_json(result_path)['runtime_path'])
        mesh_path = Path(copied_runtime['entities'][0]['mesh']['path'])
        remap = jobs.load_json(str(mesh_path) + '.remap.json')
        assert len(remap['materials']) == 1
        assert all((mesh_path.parent / target).is_file() for target in remap['materials'].values())

        relocated = root / 'relocated' / '.vkreditor'
        shutil.copytree(workspace, relocated)
        relocated_project = relocated / 'projects' / project.name
        prepare = dict(request, operation='prepare_scene', workspace_root=str(relocated),
                       project_path=str(relocated_project / 'project.json'),
                       scene_path=str(relocated_project / 'scenes' / request['scene_id'] / 'scene.json'))
        assert jobs.Job(prepare, result_path).execute() == 0
        reopened = jobs.load_json(jobs.load_json(result_path)['runtime_path'])
        assert reopened['entities'][0]['mesh']['path'].startswith(str(relocated.resolve()))

        if args.diffuse_baker:
            import subprocess
            inspect_path = root / 'diffuse.manifest.json'
            command = [*tool_command(Path(args.diffuse_baker).resolve(), 'diffuse-baker'), '--scene', str(legacy),
                       '--inspect', '--manifest', str(inspect_path), '--grid', '2', '2', '2',
                       '--bounds', '-1', '-1', '-1', '2', '2', '2']
            # A managed material and its identity/remap sidecar are read through
            # the shared decoder, without invoking transport or a GPU.
            subprocess.run(command, check=True)
            inspect = jobs.load_json(inspect_path)
            assert inspect_path.is_file()

        if args.font_cooker:
            bootstrap = root / 'bootstrap'
            (bootstrap / 'fonts').mkdir(parents=True)
            repository = Path(__file__).resolve().parents[2]
            for name in ('UbuntuMono-cooked.fontcfg', 'UbuntuMono-cooked.vkfa', 'UbuntuMono-R.ttf'):
                shutil.copyfile(repository / 'assets' / 'fonts' / name, bootstrap / 'fonts' / name)
            font_request = dict(request, operation='create_project', bootstrap_directory=str(bootstrap),
                                project_font_source=str(repository / 'assets' / 'fonts' / 'UbuntuMono-R.ttf'),
                                tools={'font': str(Path(args.font_cooker).resolve())})
            font_request.pop('scene_id')
            assert jobs.Job(font_request, result_path).execute() == 0
            project_result = jobs.load_json(result_path)
            assert project_result['default_font']['scope'] == 'project'
            assert len(project_result['project_assets']) == 1
            product = project_result['project_assets'][0]['artifacts'][0]['path']
            assert (project / product).is_file()
            assert jobs.load_json(workspace / 'editor' / 'bundles' / '1' / 'manifest.json')['assets'][0]['id'] == 'default-scene-font'
            assert manifest.read_bytes() == original_manifest

        if args.font_cooker:
            # Opening a second editor must not change any workspace byte or directory.
            readonly_root = root / 'read-only-runtime'
            readonly_request = dict(request, operation='prepare_scene', scene_path=str(scene_path),
                                    read_only=True, runtime_directory=str(readonly_root))
            def workspace_snapshot():
                return {path.relative_to(workspace).as_posix(): jobs.digest(path) if path.is_file() else None
                        for path in workspace.rglob('*')}
            before = workspace_snapshot()
            assert jobs.Job(readonly_request, result_path).execute() == 0
            readonly_result = jobs.load_json(result_path)
            assert Path(readonly_result['runtime_path']).is_relative_to(readonly_root.resolve())
            assert Path(readonly_result['edit_path']).is_relative_to(readonly_root.resolve())
            assert jobs.load_json(readonly_result['edit_path']) == {'version': 1, 'overrides': []}
            assert workspace_snapshot() == before, 'Read-only opening modified the workspace'
            authored_before = scene_path.read_bytes()
            selected_overlay = scene_path.parent / 'edits' / 'readonly-selected.json'
            journal = {'version': 1, 'overrides': []}
            jobs.atomic_json(selected_overlay, journal)
            with_overlay = jobs.read_managed_scene(scene_path)
            with_overlay['edit_overlay'] = 'edits/readonly-selected.json'
            jobs.write_managed_scene(scene_path, with_overlay)
            before = workspace_snapshot()
            assert jobs.Job(readonly_request, result_path).execute() == 0
            copied_overlay = Path(jobs.load_json(result_path)['edit_path'])
            assert copied_overlay != selected_overlay and jobs.load_json(copied_overlay) == journal
            assert workspace_snapshot() == before, 'Read-only overlay copy modified the workspace'
            scene_path.write_bytes(authored_before)
            selected_overlay.unlink()
            unbuilt_readonly = jobs.read_managed_scene(scene_path)
            unbuilt_readonly['assets'][0]['artifacts'] = []
            jobs.write_managed_scene(scene_path, unbuilt_readonly)
            before = workspace_snapshot()
            assert jobs.Job(readonly_request, result_path).execute() == 1
            assert 'write access' in jobs.load_json(result_path)['error']
            assert workspace_snapshot() == before, 'Read-only open prepared unbuilt assets'
            scene_path.write_bytes(authored_before)
            readonly_scene = jobs.read_managed_scene(scene_path)
            before_document = scene_path.read_bytes()
            readonly_scene['assets'][0]['fingerprint'] = 'sha256:' + '0' * 64
            jobs.write_managed_scene(scene_path, readonly_scene)
            before = workspace_snapshot()
            assert jobs.Job(readonly_request, result_path).execute() == 1
            assert workspace_snapshot() == before, 'Read-only validation repaired stale content'
            scene_path.write_bytes(before_document)

        # Publication must reject documents that the C store cannot read.
        oversized = jobs.read_managed_scene(scene_path)
        oversized['large_extension'] = 'x' * jobs.MAX_MANAGED_DOCUMENT_BYTES
        original_bytes = scene_path.read_bytes()
        try:
            jobs.write_managed_scene(scene_path, oversized)
            raise AssertionError('Oversized managed scene was published')
        except jobs.JobError as error:
            assert '1 MiB' in str(error)
        assert scene_path.read_bytes() == original_bytes
        # Records beyond the 1 MiB manifest limit publish through the inventory.
        large = jobs.read_managed_scene(scene_path)
        filler = [dict(large['assets'][0], id=str(uuid.uuid4()), name='x' * 900) for _ in range(1500)]
        large['assets'] = large['assets'] + filler
        jobs.write_managed_scene(scene_path, large)
        stored = jobs.load_json(scene_path)
        assert scene_path.stat().st_size < 64 * 1024
        assert (scene_path.parent / stored['inventory']).stat().st_size > jobs.MAX_MANAGED_DOCUMENT_BYTES
        assert len(jobs.read_managed_scene(scene_path)['assets']) == len(large['assets'])
        scene_path.write_bytes(original_bytes)

        missing = dict(request, scene_id=str(uuid.uuid4()), models=[str(source / 'missing.obj')])
        assert jobs.Job(missing, result_path).execute() == 1
        assert not (project / 'scenes' / missing['scene_id']).exists()
        assert not list((project / '.staging').iterdir()), 'Failed job left staging payloads'
        assert manifest.read_bytes() == original_manifest

        # The selected journal is authoritative; an absent explicit revision is a load error.
        local_prepare = dict(request, operation='prepare_scene', scene_path=str(scene_path))
        original_scene = scene_path.read_bytes()
        bad = jobs.read_managed_scene(scene_path)
        bad['edit_overlay'] = 'edits/missing.json'
        jobs.write_managed_scene(scene_path, bad)
        assert jobs.Job(local_prepare, result_path).execute() == 1
        scene_path.write_bytes(original_scene)
        bad = jobs.read_managed_scene(scene_path)
        bad['assets'][0]['fingerprint'] = 'sha256:' + '0' * 64
        jobs.write_managed_scene(scene_path, bad)
        assert jobs.Job(local_prepare, result_path).execute() == 1
        scene_path.write_bytes(original_scene)

        # Bake-only lowering consumes a saved authored wrapper transform.
        overlay = scene_path.parent / 'edits' / 'test.json'
        jobs.atomic_json(overlay, {'version': 1, 'overrides': [{'scene_entity': 0, 'gltf_node': -1,
            'source_fingerprint': jobs.source_fingerprint(request['scene_id'].encode()), 'fields': 7,
            'name': 'Edited wrapper', 'position': [2, 3, 4], 'rotation': [0, 0, 0, 1],
            'scale': [1, 1, 1], 'visible': True, 'inherit': True}]})
        effective = jobs.read_managed_scene(scene_path)
        effective['edit_overlay'] = 'edits/test.json'
        job = jobs.Job(local_prepare, result_path)
        baked = jobs.load_json(job.effective_bake_runtime(effective, scene_path.parent)['runtime_path'])
        assert baked['entities'][0]['transform']['pos'] == [2, 3, 4]
        assert baked['entities'][0]['name'] == 'Edited wrapper'
        assert scene_path.read_bytes() == original_scene

        mesh_record = next(item for item in managed['assets'] if item['kind'] == 'mesh')
        asset_operation = dict(local_prepare, operation='rename_asset', asset_id=mesh_record['id'], name='Renamed model')
        assert jobs.Job(asset_operation, result_path).execute() == 0
        renamed = next(item for item in jobs.read_managed_scene(scene_path)['assets'] if item['id'] == mesh_record['id'])
        assert renamed['name'] == 'Renamed model' and renamed['import_id'] == mesh_record['import_id']

        # Inject cancellation immediately after scene publication; referenced revisions survive.
        rebuild = dict(local_prepare, operation='rebuild_asset', asset_id=mesh_record['id'])
        legacy_scene = jobs.read_managed_scene(scene_path)
        legacy_mesh = next(item for item in legacy_scene['assets'] if item['id'] == mesh_record['id'])
        legacy_mesh['source'] = legacy_mesh['source'].replace('/', '\\')
        valid_source = legacy_mesh['source']
        legacy_mesh['source'] = '..\\outside.obj'
        jobs.write_managed_scene(scene_path, legacy_scene)
        invalid_scene = scene_path.read_bytes()
        assert jobs.Job(rebuild, result_path).execute() == 1
        assert 'escapes its owner' in jobs.load_json(result_path)['error']
        assert scene_path.read_bytes() == invalid_scene
        legacy_mesh['source'] = valid_source
        jobs.write_managed_scene(scene_path, legacy_scene)
        committed_before = scene_path.read_bytes()
        transaction = jobs.Job(rebuild, result_path,
                               environment={'VKR_BAKERY_FAULT_CANCEL_AFTER': str(scene_path)})
        assert transaction.execute() == 2, transaction.output
        assert jobs.load_json(result_path)['status'] == 'cancelled'
        assert scene_path.read_bytes() != committed_before, 'Cancellation fired before the commit'
        after = jobs.read_managed_scene(scene_path)
        rebuilt_mesh = next(item for item in after['assets'] if item['id'] == mesh_record['id'])
        assert '\\' not in rebuilt_mesh['source'], 'Legacy source path was not normalized'
        for item in after['assets']:
            for product in item.get('artifacts', []):
                assert (scene_path.parent / product['path']).is_file(), 'Committed asset removed by cancellation cleanup'
        assert jobs.Job(local_prepare, result_path).execute() == 0

        relocated_scene = Path(prepare['scene_path'])
        document = jobs.read_managed_scene(relocated_scene)
        document['assets'][0]['artifacts'][0]['path'] = '../outside.vkb'
        jobs.write_managed_scene(relocated_scene, document)
        assert jobs.Job(prepare, result_path).execute() == 1
        assert jobs.load_json(result_path)['status'] == 'failed'
        # Project assets (ADR-076): an import needs no scene, publishes its
        # builds under the project and returns the inventory the C store
        # publishes; any scene then resolves the model by {scope: project}.
        shared_source = root / 'shared'
        shared_source.mkdir()
        shared_model = shared_source / 'shared.obj'
        shared_model.write_text('mtllib shared.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n'
                                'vt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\n'
                                'usemtl shared\nf 1/1/1 2/2/1 3/3/1\n')
        (shared_source / 'shared.mtl').write_text('newmtl shared\nKd 0 1 0\nmap_Kd pixel.png\n')
        (shared_source / 'pixel.png').write_bytes((source / 'nested' / 'pixel.png').read_bytes())
        project_import = {'version': 1, 'operation': 'import_project_assets',
                          'workspace_root': str(workspace), 'project_path': str(manifest),
                          'sources': [str(shared_model)], 'tools': request['tools']}
        before_import = manifest.read_bytes()
        assert jobs.Job(project_import, result_path).execute() == 0
        imported = jobs.load_json(result_path)
        assert manifest.read_bytes() == before_import, 'Job published project membership'
        shared_mesh = next(item for item in imported['project_assets'] if item['kind'] == 'mesh')
        assert (project / shared_mesh['artifacts'][0]['path']).is_file()
        assert any(item['kind'] == 'texture' for item in imported['project_assets'])
        assert set(imported['imported_assets']) <= {item['id'] for item in imported['project_assets']}
        assert not list((project / '.staging').iterdir())
        document = jobs.load_json(manifest)
        document['assets'] = imported['project_assets']
        manifest.write_text(json.dumps(document))
        shared_request = dict(request, scene_id=str(uuid.uuid4()), models=[])
        assert jobs.Job(shared_request, result_path).execute() == 0
        shared_path = Path(jobs.load_json(result_path)['scene_path'])
        shared_scene = jobs.read_managed_scene(shared_path)
        shared_scene['entities'].append({'id': str(uuid.uuid4()), 'name': 'Shared', 'parent': None,
            'transform': {'pos': [0, 0, 0], 'rot': [0, 0, 0, 1], 'scale': [1, 1, 1]},
            'mesh': {'asset': {'scope': 'project', 'id': shared_mesh['id'], 'role': 'mesh'},
                     'pipeline_domain': 'world'}})
        jobs.write_managed_scene(shared_path, shared_scene)
        shared_prepare = dict(shared_request, operation='prepare_scene', scene_path=str(shared_path))
        assert jobs.Job(shared_prepare, result_path).execute() == 0
        shared_runtime = jobs.load_json(jobs.load_json(result_path)['runtime_path'])
        mesh_paths = [Path(entity['mesh']['path']) for entity in shared_runtime['entities'] if 'mesh' in entity]
        assert mesh_paths and all(path.is_file() and
                                  path.resolve().is_relative_to((project / 'builds').resolve())
                                  for path in mesh_paths)
        # A Content drop places the existing project mesh by reference at its
        # drop point without copying it into the scene; a stale reference
        # fails and leaves the scene untouched.
        placed = dict(shared_request, operation='add_entities', scene_path=str(shared_path), lights=[],
                      models=[{'asset': {'scope': 'project', 'id': shared_mesh['id']}, 'name': 'Placed',
                               'transform': {'pos': [2, 0, -3], 'rot': [0, 0, 0, 1], 'scale': [1, 1, 1]}}])
        assets_before = len(jobs.read_managed_scene(shared_path).get('assets', []))
        assert jobs.Job(placed, result_path).execute() == 0
        placed_index = jobs.load_json(result_path)['added_scene_entity']
        placed_scene = jobs.read_managed_scene(shared_path)
        placed_entity = placed_scene['entities'][placed_index]
        assert placed_entity['name'] == 'Placed' and placed_entity['transform']['pos'] == [2, 0, -3]
        assert placed_entity['mesh']['asset'] == {'scope': 'project', 'id': shared_mesh['id'], 'role': 'mesh'}
        assert len(placed_scene.get('assets', [])) == assets_before, 'Placement copied the project mesh'
        stale = dict(placed, models=[{'asset': {'scope': 'project', 'id': str(uuid.uuid4())}, 'name': 'Gone'}])
        before_stale = shared_path.read_bytes()
        assert jobs.Job(stale, result_path).execute() == 1
        assert shared_path.read_bytes() == before_stale, 'Failed placement changed the scene'
        # A prefab instance copies another project scene's entities under one
        # new root with new ids and parents inside the copy; project assets
        # stay shared. A scene outside the project fails.
        document = jobs.load_json(manifest)
        document['scenes'] = [{'id': shared_request['scene_id'], 'name': 'Shared',
                               'path': 'scenes/' + shared_request['scene_id'] + '/scene.json'}]
        manifest.write_text(json.dumps(document))
        prefab_source = jobs.read_managed_scene(shared_path)
        target_request = dict(request, scene_id=str(uuid.uuid4()), models=[])
        assert jobs.Job(target_request, result_path).execute() == 0
        target_path = Path(jobs.load_json(result_path)['scene_path'])
        instance = dict(target_request, operation='add_entities', scene_path=str(target_path), lights=[],
                        prefabs=[{'scene_id': shared_request['scene_id'], 'name': 'Instance'}])
        assert jobs.Job(instance, result_path).execute() == 0
        root_index = jobs.load_json(result_path)['added_scene_entity']
        instanced = jobs.read_managed_scene(target_path)['entities']
        copies = instanced[root_index + 1:]
        assert instanced[root_index]['name'] == 'Instance' and instanced[root_index]['parent'] is None
        assert len(copies) == len(prefab_source['entities'])
        assert all(copy_entity['parent'] == root_index for copy_entity, source_entity
                   in zip(copies, prefab_source['entities']) if source_entity.get('parent') is None)
        assert not {entity['id'] for entity in copies} & {entity['id'] for entity in prefab_source['entities']}
        assert copies[-1]['mesh']['asset'] == {'scope': 'project', 'id': shared_mesh['id'], 'role': 'mesh'}
        # A prefab's scene-scoped mesh is copied into the instancing scene.
        document['scenes'].append({'id': request['scene_id'], 'name': 'Source',
                                   'path': 'scenes/' + request['scene_id'] + '/scene.json'})
        manifest.write_text(json.dumps(document))
        scoped = dict(instance, prefabs=[{'scene_id': request['scene_id']}])
        assert jobs.Job(scoped, result_path).execute() == 0
        scoped_result = jobs.load_json(result_path)
        scoped_scene = jobs.read_managed_scene(target_path)
        scoped_mesh = scoped_scene['entities'][scoped_result['added_scene_entity'] + 1]['mesh']['asset']
        assert scoped_mesh['scope'] == 'scene'
        assert scoped_mesh['id'] not in {item['id'] for item in jobs.read_managed_scene(scene_path)['assets']}
        copied_record = next(item for item in scoped_scene['assets'] if item['id'] == scoped_mesh['id'])
        assert (target_path.parent / copied_record['artifacts'][0]['path']).is_file()
        assert Path(jobs.load_json(scoped_result['runtime_path'])['entities'][-1]['mesh']['path']).is_file()
        # Deleting a scene asset the scene uses fails; once unused it goes.
        in_use = dict(scoped, operation='delete_asset', asset_id=scoped_mesh['id'], prefabs=[])
        assert jobs.Job(in_use, result_path).execute() == 1
        assert any(item['id'] == scoped_mesh['id'] for item in jobs.read_managed_scene(target_path)['assets'])
        unused_scene = jobs.read_managed_scene(target_path)
        unused_scene['entities'] = [entity for entity in unused_scene['entities']
                                    if entity.get('mesh', {}).get('asset', {}).get('id') != scoped_mesh['id']]
        jobs.write_managed_scene(target_path, unused_scene)
        assert jobs.Job(in_use, result_path).execute() == 0
        assert not any(item['id'] == scoped_mesh['id'] for item in jobs.read_managed_scene(target_path)['assets'])
        outside = dict(instance, prefabs=[{'scene_id': str(uuid.uuid4())}])
        before_outside = target_path.read_bytes()
        assert jobs.Job(outside, result_path).execute() == 1
        assert target_path.read_bytes() == before_outside, 'Failed instancing changed the scene'
        # Import preflight: counts and unresolved dependencies without writing
        # the workspace; a located legacy root resolves the missing mesh.
        legacy = root / 'legacy'
        (legacy / 'models').mkdir(parents=True)
        (legacy / 'models' / 'present.png').write_bytes((source / 'nested' / 'pixel.png').read_bytes())
        (legacy / 'models' / 'kit.gltf').write_text(json.dumps({
            'asset': {'version': '2.0'},
            'buffers': [{'uri': 'kit%20data.bin', 'byteLength': 4}],
            'images': [{'uri': 'present.png'}, {'uri': 'gone.png'}]}))
        located_root = root / 'located'
        (located_root / 'models').mkdir(parents=True)
        (located_root / 'models' / 'crate.vkb').write_bytes(b'placeholder')
        legacy_scene = legacy / 'street.scene.json'
        legacy_scene.write_text(json.dumps({'version': 2, 'entities': [
            {'name': 'Kit', 'mesh': {'path': 'models/kit.gltf'}},
            {'name': 'Crate', 'mesh': {'path': 'models/crate.vkb'}}]}))
        Path(str(legacy_scene) + '.editor.json').write_text('{}')

        def tree_digest(directory):
            return {str(path.relative_to(directory)): path.read_bytes()
                    for path in sorted(directory.rglob('*')) if path.is_file()}

        workspace_before = tree_digest(workspace)
        inspect = {'version': 1, 'operation': 'inspect_scene', 'workspace_root': str(workspace),
                   'project_path': str(manifest), 'source_scene': str(legacy_scene),
                   'legacy_root': str(root / 'nowhere')}
        assert jobs.Job(inspect, result_path).execute() == 0
        report = jobs.load_json(result_path)
        assert report['scene_version'] == 2 and report['entities'] == 2 and report['meshes'] == 2
        assert report['saved_edits'] is True
        assert report['missing_count'] == 3, report
        assert 'kit.gltf: gone.png' in report['missing'] and 'kit.gltf: kit data.bin' in report['missing']
        assert 'models/crate.vkb' in report['missing']
        assert not any('present.png' in item for item in report['missing'])
        assert jobs.Job(dict(inspect, legacy_root=str(located_root)), result_path).execute() == 0
        assert jobs.load_json(result_path)['missing_count'] == 2
        assert tree_digest(workspace) == workspace_before, 'Inspection wrote the workspace'
        assert report['missing_images'] == 1
        # A model whose only missing dependency is an image imports with a
        # placeholder once the user accepts placeholders, and fails without.
        gray = root / 'gray'
        gray.mkdir()
        gray_model = gray / 'gray.obj'
        gray_model.write_text('mtllib gray.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n'
                              'vt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\n'
                              'usemtl gray\nf 1/1/1 2/2/1 3/3/1\n')
        (gray / 'gray.mtl').write_text('newmtl gray\nKd 1 1 1\nmap_Kd absent.png\n')
        placeholder_import = {'version': 1, 'operation': 'import_project_assets',
                              'workspace_root': str(workspace), 'project_path': str(manifest),
                              'sources': [str(gray_model)], 'tools': request['tools']}
        assert jobs.Job(placeholder_import, result_path).execute() == 1
        assert jobs.Job(dict(placeholder_import, use_placeholders=True), result_path).execute() == 0
        placed = jobs.load_json(result_path)
        assert any('absent.png was replaced by a placeholder' in item for item in placed['warnings'])
        print('Project jobs: copied source closure, cooked-only remap, relocation, missing-input rollback, '
              'traversal rejection, project-scoped import, placement by reference, prefab instances, asset deletion, import preflight and image placeholders passed')


if __name__ == '__main__':
    main()
