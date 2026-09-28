#!/usr/bin/env python3
"""Check the spec-gloss conversion memo of managed glTF imports.

Importing a spec-gloss model converts its images to metallic-roughness in
memory and packs them as textures in the shared generated root, without an
intermediate image file, and records the conversion in a memo keyed by the
source images and factors. A second import must publish the same textures; an
edited source image must convert again instead of reusing the old textures; a
packed texture deleted from the shared root must be converted and packed again,
byte-identically, instead of being referenced while missing. The cook records each material whose
textures it finished in a `ready_log` as it writes it, with paths that
outlive the revision. KTX2 headers are read here independently of the
importer.
"""
import argparse
import base64
import json
from pathlib import Path
import random
import struct
import tempfile
import uuid
import zlib

import project_jobs as jobs

SIZE = 160


def write_png(path, seed):
    generator = random.Random(seed)
    rows = b''.join(b'\0' + bytes(generator.getrandbits(8) for _ in range(SIZE * 4))
                    for _ in range(SIZE))

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', SIZE, SIZE, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def ktx2_extent(path):
    """Pixel width and height from a KTX2 header."""
    data = path.read_bytes()
    assert data[:12] == b'\xabKTX 20\xbb\r\n\x1a\n', path
    return struct.unpack('<II', data[20:28])


def write_model(path):
    """One triangle with a textured spec-gloss material."""
    positions = struct.pack('<9f', 0, 0, 0, 1, 0, 0, 0, 1, 0)
    uvs = struct.pack('<6f', 0, 0, 1, 0, 0, 1)
    indices = struct.pack('<3H', 0, 1, 2) + b'\0\0'
    binary = positions + uvs + indices
    path.write_text(json.dumps({
        'asset': {'version': '2.0'},
        'extensionsUsed': ['KHR_materials_pbrSpecularGlossiness'],
        'buffers': [{'byteLength': len(binary), 'uri': 'data:application/octet-stream;base64,' +
                     base64.b64encode(binary).decode()}],
        'bufferViews': [{'buffer': 0, 'byteLength': 36},
                        {'buffer': 0, 'byteOffset': 36, 'byteLength': 24},
                        {'buffer': 0, 'byteOffset': 60, 'byteLength': 6}],
        'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
                       'min': [0, 0, 0], 'max': [1, 1, 0]},
                      {'bufferView': 1, 'componentType': 5126, 'count': 3, 'type': 'VEC2'},
                      {'bufferView': 2, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}],
        'images': [{'uri': 'diffuse.png'}, {'uri': 'specgloss.png'}],
        'textures': [{'source': 0}, {'source': 1}],
        'materials': [{'extensions': {'KHR_materials_pbrSpecularGlossiness': {
            'diffuseTexture': {'index': 0}, 'specularGlossinessTexture': {'index': 1}}}}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'TEXCOORD_0': 1},
                                    'indices': 2, 'material': 0}]}],
        'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bakery', default=str(jobs.default_bakery()))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vkr-sg-memo-') as temporary:
        root = Path(temporary)
        workspace = root / 'workspace' / '.vkreditor'
        project = workspace / 'projects' / str(uuid.uuid4())
        project.mkdir(parents=True)
        manifest = project / 'project.json'
        manifest.write_text('{"version":1,"assets":[],"scenes":[],"default_font":null}')
        sources = root / 'sources'
        sources.mkdir()
        write_png(sources / 'diffuse.png', 1)
        write_png(sources / 'specgloss.png', 2)
        write_model(sources / 'model.gltf')
        generated = workspace / 'cache' / 'generated' / 'gltf_sg3'
        environment = {'VKR_BAKERY_CACHE': str(root / 'cache')}

        def converted():
            """Packed converted texture names in the shared root."""
            assert not list(generated.glob('*.png')), 'A converted image was written as a file'
            return sorted(path.name for path in generated.glob('*.vkt'))

        def create(name, ready=None):
            result_path = root / f'{name}.json'
            request = {'version': 1, 'operation': 'create_scene', 'workspace_root': str(workspace),
                       'project_path': str(manifest), 'scene_id': str(uuid.uuid4()),
                       'scene_name': name, 'models': [str(sources / 'model.gltf')], 'bakes': {}}
            if ready:
                request['ready_log'] = str(ready)
            job = jobs.Job(request, result_path, bakery=args.bakery, environment=environment)
            assert job.execute() == 0, job.output
            scene_path = Path(jobs.load_json(result_path)['scene_path'])
            record = next(asset for asset in jobs.read_managed_scene(scene_path)['assets']
                          if asset['kind'] == 'mesh')
            mesh = scene_path.parent / record['artifacts'][0]['path']
            materials = list((mesh.parent / 'materials').glob('*.mt'))
            lines = sorted(line for material in materials
                           for line in material.read_text().splitlines()
                           if line.split('=')[0].endswith('_texture') and '=' in line
                           and line.partition('=')[2])
            created.clear()
            created.append(mesh.parent / 'materials')
            referenced.clear()
            referenced.update((material.parent / line.partition('=')[2].split('?')[0]).read_bytes()
                              for material in materials for line in material.read_text().splitlines()
                              if line.split('=')[0].endswith('_texture') and line.partition('=')[2])
            return lines

        referenced = set()
        created = []

        ready = root / 'ready.jsonl'
        first = create('first', ready)
        records = jobs.ready_records(ready, created[0])
        assert len(records) == 1 and '/.staging/' in next(iter(records.values()))['path'], \
            'The cook records its finished material while the revision is staged'
        images = converted()
        assert len(images) == 2 and len(list((generated / 'memo').glob('*.sgm'))) == 1, images
        for image in images:
            assert ktx2_extent(generated / image) == (SIZE, SIZE), image
            assert (generated / image).read_bytes() in referenced, image

        assert create('again') == first, 'A memo hit publishes the same textures'
        assert converted() == images

        write_png(sources / 'diffuse.png', 3)
        edited = create('edited')
        assert edited != first, 'An edited source image reused the old conversion'
        assert len(converted()) == 4 and len(list((generated / 'memo').glob('*.sgm'))) == 2

        write_png(sources / 'diffuse.png', 1)
        base = next(image for image in images if image.startswith('basecolor_'))
        packed = (generated / base).read_bytes()
        (generated / base).unlink()
        assert create('restored') == first
        assert (generated / base).is_file(), 'A missing packed texture was not converted again'
        assert (generated / base).read_bytes() == packed, 'The repacked texture differs'
    print('Spec-gloss memo: repeat import, source edit, missing packed texture and KTX2 '
          'extents passed')


if __name__ == '__main__':
    main()
