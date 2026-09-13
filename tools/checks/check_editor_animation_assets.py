#!/usr/bin/env python3
"""CPU-only oracle for immutable mesh/bank publication and rebuild rollback."""
import argparse
import base64
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import uuid


def animated_triangle(path):
    blob = bytearray()
    views = []
    accessors = []

    def accessor(values, code, component, kind, count):
        blob.extend(b'\0' * (-len(blob) % 4))
        offset = len(blob)
        blob.extend(struct.pack('<' + code * len(values), *values))
        views.append({'buffer': 0, 'byteOffset': offset, 'byteLength': len(blob) - offset})
        accessors.append({'bufferView': len(views) - 1, 'componentType': component,
                          'count': count, 'type': kind})
        return len(accessors) - 1

    positions = accessor([0, 0, 0, 1, 0, 0, 0, 1, 0], 'f', 5126, 'VEC3', 3)
    normals = accessor([0, 0, 1] * 3, 'f', 5126, 'VEC3', 3)
    joints = accessor([0] * 12, 'H', 5123, 'VEC4', 3)
    weights = accessor([1, 0, 0, 0] * 3, 'f', 5126, 'VEC4', 3)
    indices = accessor([0, 1, 2], 'H', 5123, 'SCALAR', 3)
    times = accessor([0, 1], 'f', 5126, 'SCALAR', 2)
    translations = accessor([0, 0, 0, 2, 0, 0], 'f', 5126, 'VEC3', 2)
    document = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': [{'name': 'Character', 'mesh': 0, 'skin': 0, 'children': [1]}, {'name': 'Joint'}],
        'skins': [{'joints': [1]}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': positions, 'NORMAL': normals,
            'JOINTS_0': joints, 'WEIGHTS_0': weights}, 'indices': indices, 'material': 0}]}],
        'materials': [{'name': 'SkinFixture', 'pbrMetallicRoughness': {
            'baseColorFactor': [0.7, 0.7, 0.7, 1], 'metallicFactor': 0, 'roughnessFactor': 1}}],
        'animations': [{'name': 'Translate', 'samplers': [{'input': times, 'output': translations}],
            'channels': [{'sampler': 0, 'target': {'node': 1, 'path': 'translation'}}]}],
        'accessors': accessors, 'bufferViews': views,
        'buffers': [{'byteLength': len(blob), 'uri': 'data:application/octet-stream;base64,' +
                     base64.b64encode(blob).decode()}]}
    path.write_text(json.dumps(document))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesh-cooker', required=True)
    parser.add_argument('--animation-cooker', required=True)
    args = parser.parse_args()
    module_path = Path(__file__).resolve().parents[1] / 'editor_project_jobs.py'
    spec = importlib.util.spec_from_file_location('editor_project_jobs', module_path)
    jobs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(jobs)
    with tempfile.TemporaryDirectory(prefix='vkr-animation-publication-') as temporary:
        root = Path(temporary)
        workspace = root / '.vkreditor'
        project = workspace / 'projects' / str(uuid.uuid4())
        project.mkdir(parents=True)
        manifest = project / 'project.json'
        manifest.write_text('{"version":1,"assets":[],"scenes":[],"default_font":null}')
        source = root / 'triangle.gltf'
        animated_triangle(source)
        source_bytes = source.read_bytes()
        request = {'version': 1, 'operation': 'create_scene',
            'workspace_root': str(workspace), 'project_path': str(manifest),
            'scene_id': str(uuid.uuid4()), 'scene_name': 'Animation publication fixture',
            'models': [str(source)], 'bakes': {},
            'tools': {'mesh': str(Path(args.mesh_cooker).resolve()),
                      'animation': str(Path(args.animation_cooker).resolve())}}
        result_path = root / 'result.json'
        assert jobs.Job(request, result_path).execute() == 0, result_path.read_text()
        result = jobs.load_json(result_path)
        scene_path = Path(result['scene_path'])
        managed = jobs.load_json(scene_path)
        mesh = next(record for record in managed['assets'] if record['kind'] == 'mesh')
        assert [item['role'] for item in mesh['artifacts']] == ['mesh', 'animation']
        assert mesh['animation_count'] == 1
        assert managed['entities'][0]['animation']['asset'] == {
            'scope': 'scene', 'id': mesh['id'], 'role': 'animation'}
        runtime = jobs.load_json(result['runtime_path'])
        bank = Path(runtime['entities'][0]['animation']['path'])
        mesh_path = Path(runtime['entities'][0]['mesh']['path'])
        assert bank.is_file() and bank.parent == mesh_path.parent
        assert source.read_bytes() == source_bytes
        first_bank = bank.read_bytes()
        # Controls survive regeneration; the new pair gets one revision owner.
        controller = {'version': 1, 'cycle': 2, 'root': 0, 'initial': 0,
                      'parameters': [], 'nodes': [[0, 0, 0, 0, 0, 0, [], []]],
                      'states': [], 'transitions': []}
        managed['entities'][0]['animation'].update(clip=0, playing=False, rate=-0.5,
                                                  loop=False, controller=controller)
        jobs.atomic_json(scene_path, managed)
        rebuild = dict(request, operation='rebuild_asset', scene_path=str(scene_path), asset_id=mesh['id'])
        assert jobs.Job(rebuild, result_path).execute() == 0, result_path.read_text()
        updated = jobs.load_json(scene_path)
        latest = jobs.load_json(jobs.load_json(result_path)['runtime_path'])
        controls = latest['entities'][0]['animation']
        assert controls['controller'] == controller
        assert controls['playing'] is False and controls['rate'] == -0.5 and controls['loop'] is False
        assert Path(controls['path']).read_bytes() == first_bank
        assert Path(controls['path']).parent == Path(latest['entities'][0]['mesh']['path']).parent
        assert Path(controls['path']).parent != bank.parent and bank.read_bytes() == first_bank
        # Bank cooker failure must leave the previously published scene/pair intact.
        before = scene_path.read_bytes()
        failed = dict(rebuild, tools={'mesh': request['tools']['mesh']})
        assert jobs.Job(failed, result_path).execute() == 1
        assert 'animation tool' in jobs.load_json(result_path)['error']
        assert scene_path.read_bytes() == before
        assert Path(controls['path']).read_bytes() == first_bank
        # Reimporting the same rig without clips removes the generated binding.
        static = json.loads(source.read_text())
        static.pop('animations')
        source.write_text(json.dumps(static))
        reimport = dict(rebuild, operation='reimport_asset', source=str(source))
        assert jobs.Job(reimport, result_path).execute() == 0, result_path.read_text()
        static_scene = jobs.load_json(scene_path)
        assert 'animation' not in static_scene['entities'][0]
        record = next(item for item in static_scene['assets'] if item['id'] == mesh['id'])
        assert [item['role'] for item in record['artifacts']] == ['mesh']
        assert 'animation' not in jobs.load_json(jobs.load_json(result_path)['runtime_path'])['entities'][0]
    print('Animation publication: import, paired rebuild, authored controls, failure rollback, static reimport passed')


if __name__ == '__main__':
    main()
