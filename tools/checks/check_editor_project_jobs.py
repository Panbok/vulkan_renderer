#!/usr/bin/env python3
"""CPU transaction checks: copied imports, relocation, missing assets and rollback."""
import argparse
import base64
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import tempfile
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesh-cooker', required=True)
    parser.add_argument('--font-cooker')
    parser.add_argument('--texture-packer', required=True)
    parser.add_argument('--diffuse-baker')
    args = parser.parse_args()
    module_path = Path(__file__).resolve().parents[1] / 'editor_project_jobs.py'
    spec = importlib.util.spec_from_file_location('editor_project_jobs', module_path)
    jobs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(jobs)
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
        result_path = root / 'result.json'
        assert jobs.Job(request, result_path).execute() == 0
        result = jobs.load_json(result_path)
        scene_path = Path(result['scene_path'])
        managed = jobs.load_json(scene_path)
        runtime = jobs.load_json(result['runtime_path'])
        assert managed['version'] == 3 and runtime['version'] == 2
        assert manifest.read_bytes() == original_manifest, 'Job published project membership'
        assert '.staging' not in json.dumps(managed)
        assert str(source) not in json.dumps(managed)
        assert len(managed['assets']) >= 2
        assert any(item['kind'] == 'material' and item['name'] == 'test' for item in managed['assets'])
        assert any(item['kind'] == 'texture' and item['name'] == 'pixel' for item in managed['assets'])
        assert Path(runtime['entities'][0]['mesh']['path']).is_file()
        unbuilt_request = dict(request, scene_id=str(uuid.uuid4()), bakes={'prepare_assets': False})
        assert jobs.Job(unbuilt_request, result_path).execute() == 0
        unbuilt_result = jobs.load_json(result_path)
        assert unbuilt_result['status'] == 'unbuilt' and 'runtime_path' not in unbuilt_result
        unbuilt_scene = jobs.load_json(unbuilt_result['scene_path'])
        assert not unbuilt_scene['assets'][0]['artifacts']
        finish = dict(unbuilt_request, operation='prepare_scene', scene_path=unbuilt_result['scene_path'])
        assert jobs.Job(finish, result_path).execute() == 0
        assert jobs.load_json(result_path)['status'] == 'complete'
        assert jobs.load_json(unbuilt_result['scene_path'])['assets'][0]['artifacts']
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
        node_result = jobs.load_json(result_path)
        node_scene = jobs.load_json(node_result['scene_path'])
        node_runtime = jobs.load_json(node_result['runtime_path'])
        node_mesh = Path(node_runtime['entities'][0]['mesh']['path'])
        node_job = jobs.Job(node_request, result_path)
        node_info = node_job.inspect_mesh(node_mesh)
        node_hash = jobs.Job.mesh_identity(jobs.source_fingerprint(node_request['scene_id'].encode()), node_info['fingerprint'])
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
            check = subprocess.run([str(Path(args.diffuse_baker).resolve()), '--scene', effective_node['runtime_path'],
                '--inspect', '--manifest', str(root / 'edited-light.json'), '--grid', '2', '2', '2',
                '--bounds', '-1', '-1', '-1', '10', '10', '10'], capture_output=True, text=True)
            assert check.returncode == 0, check.stdout + check.stderr
            assert 'lights=1 ' in check.stdout, 'Source punctual was duplicated or edited light omitted'

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
            command = [str(Path(args.diffuse_baker).resolve()), '--scene', str(legacy),
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
            with_overlay = jobs.load_json(scene_path)
            with_overlay['edit_overlay'] = 'edits/readonly-selected.json'
            jobs.atomic_json(scene_path, with_overlay)
            before = workspace_snapshot()
            assert jobs.Job(readonly_request, result_path).execute() == 0
            copied_overlay = Path(jobs.load_json(result_path)['edit_path'])
            assert copied_overlay != selected_overlay and jobs.load_json(copied_overlay) == journal
            assert workspace_snapshot() == before, 'Read-only overlay copy modified the workspace'
            scene_path.write_bytes(authored_before)
            selected_overlay.unlink()
            unbuilt_readonly = jobs.load_json(scene_path)
            unbuilt_readonly['assets'][0]['artifacts'] = []
            jobs.atomic_json(scene_path, unbuilt_readonly)
            before = workspace_snapshot()
            assert jobs.Job(readonly_request, result_path).execute() == 1
            assert 'write access' in jobs.load_json(result_path)['error']
            assert workspace_snapshot() == before, 'Read-only open prepared unbuilt assets'
            scene_path.write_bytes(authored_before)
            readonly_scene = jobs.load_json(scene_path)
            before_document = scene_path.read_bytes()
            readonly_scene['assets'][0]['fingerprint'] = 'sha256:' + '0' * 64
            jobs.atomic_json(scene_path, readonly_scene)
            before = workspace_snapshot()
            assert jobs.Job(readonly_request, result_path).execute() == 1
            assert workspace_snapshot() == before, 'Read-only validation repaired stale content'
            scene_path.write_bytes(before_document)

        # Publication must reject documents that the C store cannot read.
        oversized = jobs.load_json(scene_path)
        oversized['large_extension'] = 'x' * jobs.MAX_MANAGED_DOCUMENT_BYTES
        original_bytes = scene_path.read_bytes()
        try:
            jobs.atomic_json(scene_path, oversized)
            raise AssertionError('Oversized managed scene was published')
        except jobs.JobError as error:
            assert '1 MiB' in str(error)
        assert scene_path.read_bytes() == original_bytes

        missing = dict(request, scene_id=str(uuid.uuid4()), models=[str(source / 'missing.obj')])
        assert jobs.Job(missing, result_path).execute() == 1
        assert not (project / 'scenes' / missing['scene_id']).exists()
        assert not list((project / '.staging').iterdir()), 'Failed job left staging payloads'
        assert manifest.read_bytes() == original_manifest

        # The selected journal is authoritative; an absent explicit revision is a load error.
        local_prepare = dict(request, operation='prepare_scene', scene_path=str(scene_path))
        original_scene = scene_path.read_bytes()
        bad = jobs.load_json(scene_path)
        bad['edit_overlay'] = 'edits/missing.json'
        jobs.atomic_json(scene_path, bad)
        assert jobs.Job(local_prepare, result_path).execute() == 1
        scene_path.write_bytes(original_scene)
        bad = jobs.load_json(scene_path)
        bad['assets'][0]['fingerprint'] = 'sha256:' + '0' * 64
        jobs.atomic_json(scene_path, bad)
        assert jobs.Job(local_prepare, result_path).execute() == 1
        scene_path.write_bytes(original_scene)

        # Bake-only lowering consumes a saved authored wrapper transform.
        overlay = scene_path.parent / 'edits' / 'test.json'
        jobs.atomic_json(overlay, {'version': 1, 'overrides': [{'scene_entity': 0, 'gltf_node': -1,
            'source_fingerprint': jobs.source_fingerprint(request['scene_id'].encode()), 'fields': 7,
            'name': 'Edited wrapper', 'position': [2, 3, 4], 'rotation': [0, 0, 0, 1],
            'scale': [1, 1, 1], 'visible': True, 'inherit': True}]})
        effective = jobs.load_json(scene_path)
        effective['edit_overlay'] = 'edits/test.json'
        job = jobs.Job(local_prepare, result_path)
        baked = jobs.load_json(job.effective_bake_runtime(effective, scene_path.parent)['runtime_path'])
        assert baked['entities'][0]['transform']['pos'] == [2, 3, 4]
        assert baked['entities'][0]['name'] == 'Edited wrapper'
        assert scene_path.read_bytes() == original_scene

        mesh_record = next(item for item in managed['assets'] if item['kind'] == 'mesh')
        asset_operation = dict(local_prepare, operation='rename_asset', asset_id=mesh_record['id'], name='Renamed model')
        assert jobs.Job(asset_operation, result_path).execute() == 0
        renamed = next(item for item in jobs.load_json(scene_path)['assets'] if item['id'] == mesh_record['id'])
        assert renamed['name'] == 'Renamed model' and renamed['import_id'] == mesh_record['import_id']

        # Inject cancellation immediately after scene publication; referenced revisions survive.
        rebuild = dict(local_prepare, operation='rebuild_asset', asset_id=mesh_record['id'])
        transaction = jobs.Job(rebuild, result_path)
        write_json = jobs.atomic_json
        fired = False
        def cancel_after_commit(path, value):
            nonlocal fired
            write_json(path, value)
            if Path(path) == scene_path and not fired:
                fired = True
                transaction.cancel()
        jobs.atomic_json = cancel_after_commit
        try:
            assert transaction.execute() == 2
        finally:
            jobs.atomic_json = write_json
        assert fired
        after = jobs.load_json(scene_path)
        for item in after['assets']:
            for product in item.get('artifacts', []):
                assert (scene_path.parent / product['path']).is_file(), 'Committed asset removed by cancellation cleanup'
        assert jobs.Job(local_prepare, result_path).execute() == 0

        relocated_scene = Path(prepare['scene_path'])
        document = jobs.load_json(relocated_scene)
        document['assets'][0]['artifacts'][0]['path'] = '../outside.vkb'
        jobs.atomic_json(relocated_scene, document)
        assert jobs.Job(prepare, result_path).execute() == 1
        assert jobs.load_json(result_path)['status'] == 'failed'
        print('Project jobs: copied source closure, cooked-only remap, relocation, missing-input rollback and traversal rejection passed')


if __name__ == '__main__':
    main()
