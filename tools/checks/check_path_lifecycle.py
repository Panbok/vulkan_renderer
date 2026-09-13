#!/usr/bin/env python3
"""Fresh-process Bakery lifecycle using a synthetic mesh; no renderer or GPU."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from urllib.parse import quote
import uuid
import zlib


def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def save(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesh-cooker', required=True)
    parser.add_argument('--texture-packer', required=True)
    args = parser.parse_args()
    script = Path(__file__).resolve().parents[1] / 'editor_project_jobs.py'
    with tempfile.TemporaryDirectory(prefix='vkr-lifecycle-') as temporary:
        root = Path(temporary).resolve()
        # Long paths exercise the same argument/file boundaries as nested staging.
        workspace = root / ('nested-' + 'a' * 70) / ('nested-' + 'b' * 70) / 'проект with spaces' / '.vkreditor'
        project = workspace / 'projects' / str(uuid.uuid4())
        project.mkdir(parents=True)
        manifest = project / 'project.json'
        save(manifest, {'version': 1, 'assets': [], 'scenes': [], 'default_font': None})
        source = root / 'исходники with spaces'
        source.mkdir()
        buffer_name = 'буфер%20#данные.bin'
        binary = struct.pack('<9f3H', 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 2)
        (source / buffer_name).write_bytes(binary)
        def chunk(tag, data):
            return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
        pixels = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b'\0\xff\0\0')) + chunk(b'IEND', b'')
        image_name = 'текстура%20.png'
        (source / image_name).write_bytes(pixels)
        model = source / 'меш.gltf'
        save(model, {'asset': {'version': '2.0'},
            'buffers': [{'uri': quote(buffer_name), 'byteLength': len(binary)}],
            'bufferViews': [{'buffer': 0, 'byteLength': 36}, {'buffer': 0, 'byteOffset': 36, 'byteLength': 6}],
            'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3', 'min': [0, 0, 0], 'max': [1, 1, 0]}, {'bufferView': 1, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}],
            'images': [{'uri': quote(image_name)}], 'textures': [{'source': 0}],
            'materials': [{'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}}}],
            'meshes': [{'primitives': [{'attributes': {'POSITION': 0}, 'indices': 1, 'material': 0}]}],
            'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0})
        request = {'version': 1, 'operation': 'create_scene', 'workspace_root': str(workspace),
            'project_path': str(manifest), 'scene_id': str(uuid.uuid4()), 'scene_name': 'Path fixture',
            'models': [str(model)], 'bakes': {}, 'tools': {'mesh': str(Path(args.mesh_cooker).resolve()), 'texture': str(Path(args.texture_packer).resolve())}}
        sequence = []
        def run(operation, **fields):
            value = dict(request, operation=operation, **fields)
            request_path = root / 'request.json'
            result_path = root / 'result.json'
            save(request_path, value)
            result = subprocess.run([sys.executable, str(script), '--request', str(request_path), '--result', str(result_path)], capture_output=True, encoding='utf-8', errors='replace')
            if result.returncode:
                raise AssertionError(f'{operation} failed:\n{result.stdout}\n{result.stderr}\n{load(result_path)}')
            response = load(result_path)
            assert response['status'] == 'complete', response
            sequence.append(operation)
            return response
        result = run('create_scene')
        scene_path = Path(result['scene_path'])
        assert len(str(scene_path)) > 260
        scene = load(scene_path)
        mesh = next(item for item in scene['assets'] if item['kind'] == 'mesh')
        asset_id = mesh['id']
        old_artifact = scene_path.parent / mesh['artifacts'][0]['path']
        old_digest = digest(old_artifact)
        # Persist an authored journal and assert it survives every transition.
        overlay = scene_path.parent / 'edits/автор.json'
        overlay.parent.mkdir(exist_ok=True)
        save(overlay, {'version': 1, 'overrides': [], 'fixture_note': 'saved edits'})
        overlay_digest = digest(overlay)
        scene['edit_overlay'] = 'edits/автор.json'
        # Deliberately exercise compatibility loading of pre-fix Windows sources.
        mesh['source'] = mesh['source'].replace('/', '\\')
        save(scene_path, scene)
        result = run('prepare_scene', scene_path=str(scene_path))
        result = run('rebuild_asset', scene_path=str(scene_path), asset_id=asset_id)
        rebuilt = load(scene_path)
        assert next(item for item in rebuilt['assets'] if item['id'] == asset_id)['source'].find('\\') == -1
        assert digest(old_artifact) == old_digest
        data = bytearray(binary)
        struct.pack_into('<f', data, 12, 2.0)
        (source / buffer_name).write_bytes(data)
        updated_model = load(model)
        updated_model['accessors'][0]['max'] = [2, 1, 0]
        save(model, updated_model)
        result = run('reimport_asset', scene_path=str(scene_path), asset_id=asset_id, source=str(model))
        reimported = load(scene_path)
        mesh = next(item for item in reimported['assets'] if item['id'] == asset_id)
        assert digest(scene_path.parent / mesh['artifacts'][0]['path']) != old_digest
        assert digest(old_artifact) == old_digest and digest(overlay) == overlay_digest
        for item in reimported['assets']:
            for reference in [item.get('source'), *[artifact['path'] for artifact in item.get('artifacts', [])]]:
                if reference:
                    assert '\\' not in reference and not Path(reference).is_absolute()
        relocated = root / 'перенесено' / '.vkreditor'
        shutil.move(str(workspace), str(relocated))
        request['workspace_root'] = str(relocated)
        request['project_path'] = str(relocated / 'projects' / project.name / 'project.json')
        relocated_scene = relocated / scene_path.relative_to(workspace)
        result = run('prepare_scene', scene_path=str(relocated_scene))
        runtime = load(result['runtime_path'])
        assert Path(runtime['entities'][0]['mesh']['path']).is_relative_to(relocated)
        assert digest(relocated / overlay.relative_to(workspace)) == overlay_digest
        assert digest(relocated / old_artifact.relative_to(workspace)) == old_digest
        print(json.dumps({'status': 'passed', 'fresh_process_operations': sequence, 'asset_id_preserved': asset_id, 'old_revision_sha256': old_digest, 'unicode_long_paths': True, 'saved_edits_preserved': True}))


if __name__ == '__main__':
    main()
