#!/usr/bin/env python3
"""Check preview-tier imports and `finalize_textures` on a two-texture model.

A scene imported with `texture_tier: preview` publishes preview-named textures
and derived bakes, marks its mesh record and reports `preview_assets`.
`finalize_textures` republishes the same asset identity at the final tier,
and its materials and textures then match a scene imported at the final tier
directly. These run with `texture_encoding: uastc`, as on a host without
ASTC. With `texture_encoding: astc` preview and final imports hold native
ASTC 4x4 textures under their own names, and on x86-64 builds, where it is
the default, `texture_encoding: bc` holds BC7 colours and data and BC5
normals. A `deferred` import names no
texture until `finalize_textures` adds final ones to the same asset, and a
finalize given a `ready_log` records every material it rebuilt, with the
published definition, before it exits. On
macOS, `texture_encode_speed: fast` encodes ASTC with the system encoder
under `-astc-fast` names, BC turns into `-bc-fast` colours, and UASTC
imports stay unchanged.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import platform
import random
import struct
import tempfile
import uuid
import zlib

import project_jobs as jobs


def write_png(path, width, height, seed):
    generator = random.Random(seed)
    rows = b''.join(b'\0' + bytes(generator.getrandbits(8) for _ in range(width * 3))
                    for _ in range(height))

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def write_model(path):
    """One triangle whose material names a base color and a normal map."""
    positions = struct.pack('<9f', 0, 0, 0, 1, 0, 0, 0, 1, 0)
    uvs = struct.pack('<6f', 0, 0, 1, 0, 0, 1)
    indices = struct.pack('<3H', 0, 1, 2) + b'\0\0'
    binary = positions + uvs + indices
    path.write_text(json.dumps({
        'asset': {'version': '2.0'},
        'buffers': [{'byteLength': len(binary), 'uri': 'data:application/octet-stream;base64,' +
                     base64.b64encode(binary).decode()}],
        'bufferViews': [{'buffer': 0, 'byteLength': 36},
                        {'buffer': 0, 'byteOffset': 36, 'byteLength': 24},
                        {'buffer': 0, 'byteOffset': 60, 'byteLength': 6}],
        'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
                       'min': [0, 0, 0], 'max': [1, 1, 0]},
                      {'bufferView': 1, 'componentType': 5126, 'count': 3, 'type': 'VEC2'},
                      {'bufferView': 2, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}],
        'images': [{'uri': 'color.png'}, {'uri': 'normal.png'}],
        'textures': [{'source': 0}, {'source': 1}],
        'materials': [{'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}},
                       'normalTexture': {'index': 1}}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'TEXCOORD_0': 1},
                                    'indices': 2, 'material': 0}]}],
        'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}))


def scene_state(result):
    """The mesh record and each material's texture lines by content digest."""
    scene_path = Path(result['scene_path'])
    scene = jobs.read_managed_scene(scene_path)
    record = next(asset for asset in scene['assets'] if asset['kind'] == 'mesh')
    mesh = scene_path.parent / record['artifacts'][0]['path']
    materials = []
    references = []
    for material in sorted((mesh.parent / 'materials').glob('*.mt')):
        lines = []
        for line in material.read_text().splitlines():
            key, _, value = line.partition('=')
            if key.endswith('_texture') and value:
                path = value.split('?')[0]
                references.append(path)
                digest = hashlib.sha256((material.parent / path).read_bytes()).hexdigest()
                lines.append(f'{key}={digest}?{value.partition("?")[2]}')
            elif key != 'name':
                lines.append(line)
        materials.append('\n'.join(lines))
    return record, materials, references


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bakery', default=str(jobs.default_bakery()))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vkr-tiers-') as temporary:
        root = Path(temporary)
        workspace = root / 'workspace' / '.vkreditor'
        project = workspace / 'projects' / str(uuid.uuid4())
        project.mkdir(parents=True)
        manifest = project / 'project.json'
        manifest.write_text('{"version":1,"assets":[],"scenes":[],"default_font":null}')
        sources = root / 'sources'
        sources.mkdir()
        write_png(sources / 'color.png', 64, 64, 1)
        write_png(sources / 'normal.png', 64, 64, 2)
        write_model(sources / 'model.gltf')
        environment = {'VKR_BAKERY_CACHE': str(root / 'cache')}

        def run(request, name):
            result_path = root / f'{name}.json'
            job = jobs.Job(request, result_path, bakery=args.bakery, environment=environment)
            assert job.execute() == 0, job.output
            return jobs.load_json(result_path)

        def create(tier, encoding='uastc', speed=None):
            request = {'version': 1, 'operation': 'create_scene', 'workspace_root': str(workspace),
                       'project_path': str(manifest), 'scene_id': str(uuid.uuid4()),
                       'scene_name': f'Model {tier}', 'models': [str(sources / 'model.gltf')],
                       'bakes': {}}
            if encoding:
                request['texture_encoding'] = encoding
            if tier:
                request['texture_tier'] = tier
            if speed:
                request['texture_encode_speed'] = speed
            return run(request, f'create-{tier}-{encoding}-{speed}')

        preview = create('preview')
        assert preview['preview_assets'] == 1, preview
        record, _, references = scene_state(preview)
        assert record['texture_tier'] == 'preview'
        assert any(reference.endswith('-preview.vkt') for reference in references), references
        generated = workspace / 'cache' / 'generated' / 'normalrough_v2'
        assert list(generated.glob('*.preview.vkt')), 'Preview pair bake is named apart'

        finalized = run({'version': 1, 'operation': 'finalize_textures',
                         'workspace_root': str(workspace), 'project_path': str(manifest),
                         'scene_id': preview['scene_id'], 'scene_path': preview['scene_path'],
                         'texture_encoding': 'uastc'},
                        'finalize')
        assert finalized['preview_assets'] == 0, finalized
        final_record, final_materials, final_references = scene_state(finalized)
        assert final_record['id'] == record['id'] and 'texture_tier' not in final_record
        assert not any('preview' in reference for reference in final_references), final_references
        assert [path for path in generated.glob('*.vkt') if not path.name.endswith('.preview.vkt')]

        direct = create(None)
        assert direct['preview_assets'] == 0
        _, direct_materials, _ = scene_state(direct)
        assert final_materials == direct_materials, 'Finalized textures differ from a final import'

        def formats(result, record, references):
            mesh = Path(result['scene_path']).parent / record['artifacts'][0]['path']
            return {struct.unpack('<I', (mesh.parent / 'materials' / reference)
                                  .read_bytes()[12:16])[0] for reference in references}

        # Native ASTC: preview and final textures are named apart and hold
        # ASTC 4x4 blocks (vkFormat 157 unorm, 158 sRGB).
        astc_preview = create('preview', 'astc')
        assert astc_preview['preview_assets'] == 1, astc_preview
        record, _, references = scene_state(astc_preview)
        assert any(reference.endswith('-astc-preview.vkt') for reference in references), references
        assert list(generated.glob('*.astc.preview.vkt')), 'ASTC preview pair bake is named apart'
        assert formats(astc_preview, record, references) <= {157, 158}
        astc_final = create(None, 'astc')
        assert astc_final['preview_assets'] == 0, astc_final
        record, _, references = scene_state(astc_final)
        packed = [reference for reference in references if '-color-' in reference]
        assert packed and all(reference.endswith('-astc.vkt') for reference in packed), packed
        assert list(generated.glob('*.astc.vkt')), 'ASTC pair bake is named apart'
        assert formats(astc_final, record, references) <= {157, 158}

        # Fast encode speed: UASTC has no fast encoder and ignores it; on
        # macOS ASTC comes from the system encoder under its own names and
        # settings identity, never replacing astcenc outputs.
        _, fast_uastc_materials, _ = scene_state(create(None, 'uastc', 'fast'))
        assert fast_uastc_materials == direct_materials, 'Fast speed changed UASTC textures'
        if platform.system() == 'Darwin':
            astc_fast = create(None, 'astc', 'fast')
            record, _, references = scene_state(astc_fast)
            packed = [reference for reference in references if '-color-' in reference]
            assert packed and all(reference.endswith('-astc-fast.vkt') for reference in packed), packed
            fast_bakes = list(generated.glob('*.astc-fast.vkt'))
            assert fast_bakes and list(generated.glob('*.astc.vkt')), 'Fast pair bake is named apart'
            assert all(b'astc-4x4-system' in bake.read_bytes() for bake in fast_bakes)
            assert formats(astc_fast, record, references) <= {157, 158}

        # Native BC, built on x86-64 only: BC7 colours and data (vkFormat 145
        # unorm, 146 sRGB) and BC5 normals (141) under -bc names, with
        # identities naming each class's encoder. It is the default encoding
        # there, and the fast speed encodes colours with bc7e's fastest
        # profile under -bc-fast names.
        if platform.machine().lower() in ('amd64', 'x86_64'):
            bc_final = create(None, 'bc')
            record, bc_materials, references = scene_state(bc_final)
            packed = [reference for reference in references if '-color-' in reference]
            assert packed and all(reference.endswith('-bc.vkt') for reference in packed), packed
            assert formats(bc_final, record, references) == {141, 145, 146}
            mesh = Path(bc_final['scene_path']).parent / record['artifacts'][0]['path']
            texture_bytes = [(mesh.parent / 'materials' / reference).read_bytes()
                             for reference in references]
            for profile in (b'bc7-bc7e-veryfast-v1', b'bc7-bc7e-default-v1', b'bc5-rgbcx-v1'):
                assert any(profile in data for data in texture_bytes), profile
            assert list(generated.glob('*.bc.vkt')), 'BC pair bake is named apart'
            _, default_materials, _ = scene_state(create(None, None))
            assert default_materials == bc_materials, 'BC is not the default encoding'
            bc_fast = create(None, 'bc', 'fast')
            record, _, references = scene_state(bc_fast)
            packed = [reference for reference in references if '-color-' in reference]
            assert packed and all(reference.endswith('-bc-fast.vkt') for reference in packed), packed
            assert list(generated.glob('*.bc-fast.vkt')), 'Fast BC pair bake is named apart'
            mesh = Path(bc_fast['scene_path']).parent / record['artifacts'][0]['path']
            assert b'bc7-bc7e-ultrafast-v1' in (mesh.parent / 'materials' / packed[0]).read_bytes()
            assert formats(bc_fast, record, references) == {141, 145, 146}
        else:
            bad = jobs.Job({'version': 1, 'operation': 'create_scene',
                            'workspace_root': str(workspace), 'project_path': str(manifest),
                            'scene_id': str(uuid.uuid4()), 'scene_name': 'No BC', 'models': [],
                            'bakes': {}, 'texture_encoding': 'bc'},
                           root / 'no-bc.json', bakery=args.bakery, environment=environment)
            assert bad.execute() == 1, 'bc needs the x86-64 encoders'

        # Deferred: materials keep their factors and name no texture, and the
        # asset awaits finalization, which adds final ASTC textures.
        deferred = create('deferred', 'astc')
        assert deferred['preview_assets'] == 1, deferred
        deferred_record, _, deferred_references = scene_state(deferred)
        assert deferred_record['texture_tier'] == 'deferred' and not deferred_references
        ready = root / 'ready.jsonl'
        filled = run({'version': 1, 'operation': 'finalize_textures',
                      'workspace_root': str(workspace), 'project_path': str(manifest),
                      'scene_id': deferred['scene_id'], 'scene_path': deferred['scene_path'],
                      'texture_encoding': 'astc', 'ready_log': str(ready),
                      'material_priority': ['not_a_material']}, 'finalize-deferred')
        assert filled['preview_assets'] == 0, filled
        filled_record, _, filled_references = scene_state(filled)
        assert filled_record['id'] == deferred_record['id'] and 'texture_tier' not in filled_record
        assert filled_references and formats(filled, filled_record, filled_references) <= {157, 158}
        filled_mesh = Path(filled['scene_path']).parent / filled_record['artifacts'][0]['path']
        assert len(jobs.ready_records(ready, filled_mesh.parent / 'materials')) == 1

        # Project library: a deferred Content import names no texture until
        # finalize_project_assets rebuilds it at the final tier; the editor
        # publishes each returned inventory, as done here.
        def library_references(record):
            mesh = project / record['artifacts'][0]['path']
            return [line.partition('=')[2].split('?')[0]
                    for material in (mesh.parent / 'materials').glob('*.mt')
                    for line in material.read_text().splitlines()
                    if line.partition('=')[0].endswith('_texture') and line.partition('=')[2]], mesh

        library = run({'version': 1, 'operation': 'import_project_assets',
                       'workspace_root': str(workspace), 'project_path': str(manifest),
                       'sources': [str(sources / 'model.gltf')], 'texture_tier': 'deferred',
                       'texture_encoding': 'astc'}, 'library-import')
        assert library['preview_assets'] == 1, library
        library_mesh = next(item for item in library['project_assets'] if item['kind'] == 'mesh')
        assert library_mesh['texture_tier'] == 'deferred'
        assert not library_references(library_mesh)[0]
        document = jobs.load_json(manifest)
        document['assets'] = library['project_assets']
        manifest.write_text(json.dumps(document))
        library_ready = root / 'library-ready.jsonl'
        library_final = run({'version': 1, 'operation': 'finalize_project_assets',
                             'workspace_root': str(workspace), 'project_path': str(manifest),
                             'texture_encoding': 'astc', 'ready_log': str(library_ready)},
                            'library-finalize')
        assert library_final['preview_assets'] == 0, library_final
        assert library_final['finalized_assets'] == [library_mesh['id']], library_final
        final_mesh = next(item for item in library_final['project_assets']
                          if item['id'] == library_mesh['id'])
        assert 'texture_tier' not in final_mesh
        references, mesh = library_references(final_mesh)
        assert len(jobs.ready_records(library_ready, mesh.parent / 'materials')) == 1
        assert references and {struct.unpack('<I', (mesh.parent / 'materials' / reference)
                                              .read_bytes()[12:16])[0]
                               for reference in references} <= {157, 158}, references
        assert not list((project / '.staging').iterdir())

        rejected = root / 'rejected.json'
        bad = jobs.Job({'version': 1, 'operation': 'create_scene', 'workspace_root': str(workspace),
                        'project_path': str(manifest), 'scene_id': str(uuid.uuid4()),
                        'scene_name': 'Bad', 'models': [], 'bakes': {}, 'texture_tier': 'draft'},
                       rejected, bakery=args.bakery, environment=environment)
        assert bad.execute() == 1 and 'texture_tier' in rejected.read_text()
        bad = jobs.Job({'version': 1, 'operation': 'create_scene', 'workspace_root': str(workspace),
                        'project_path': str(manifest), 'scene_id': str(uuid.uuid4()),
                        'scene_name': 'Bad', 'models': [], 'bakes': {},
                        'texture_encoding': 'bc7'},
                       rejected, bakery=args.bakery, environment=environment)
        assert bad.execute() == 1 and 'texture_encoding' in rejected.read_text()
        bad = jobs.Job({'version': 1, 'operation': 'create_scene', 'workspace_root': str(workspace),
                        'project_path': str(manifest), 'scene_id': str(uuid.uuid4()),
                        'scene_name': 'Bad', 'models': [], 'bakes': {},
                        'texture_encode_speed': 'slow'},
                       rejected, bakery=args.bakery, environment=environment)
        assert bad.execute() == 1 and 'texture_encode_speed' in rejected.read_text()
        bad = jobs.Job({'version': 1, 'operation': 'create_scene', 'workspace_root': str(workspace),
                        'project_path': str(manifest), 'scene_id': str(uuid.uuid4()),
                        'scene_name': 'Bad', 'models': [], 'bakes': {},
                        'material_priority': 'first'},
                       rejected, bakery=args.bakery, environment=environment)
        assert bad.execute() == 1 and 'material_priority' in rejected.read_text()
    print('Texture tiers: preview import, preview naming, finalize to the final tier, '
          'equality with a final import, native ASTC, native BC, fast encode speed, deferred scene, '
          'ready log and project library imports passed')


if __name__ == '__main__':
    main()
