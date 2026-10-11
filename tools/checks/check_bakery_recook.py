#!/usr/bin/env python3
"""Check host-neutral build texture names, their recipes and `recook_textures`.

One model holds three materials: a metal-rough MASK material with a normal
map (a cutout and a paired normal/roughness bake from files), a spec-gloss
material with a normal map (converted images, packed and paired), and a plain
base colour the project texture packer packs. The check asserts that:

- every texture a build material names is listed in the build's
  `textures/recipes.json`, and its name is the SHA-256 of its recipe, class,
  tier and speed (mesh-cook recipes) or of its source image (packed images),
  with no encoding family in either, so another host derives the same name;
- a second final cook names the same textures, while preview and fast cooks
  name theirs apart;
- `recook_textures` restores deleted textures and replaces a texture of
  another GPU family under the same names, with the same bytes on this host,
  leaves materials, the mesh and the inventory untouched, and then finds
  nothing to do;
- scene preparation accepts cooked textures whose bytes differ from the
  cooking host's (closures record `platform` for `.vkt`);
- a share of the project reports no rebuild and no texture without a
  recipe, and the shared copy, stripped of its `.vkt` files as a depot clone
  is, gets them back from `recook_textures`.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import shutil
import struct
import tempfile
import uuid
import zlib

import project_jobs as jobs


def write_png(path, size, seed):
    generator = random.Random(seed)
    rows = b''.join(b'\0' + bytes(generator.getrandbits(8) for _ in range(size * 4))
                    for _ in range(size))

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def write_model(directory):
    """Three triangles, one per material."""
    positions = struct.pack('<9f', 0, 0, 0, 1, 0, 0, 0, 1, 0)
    uvs = struct.pack('<6f', 0, 0, 1, 0, 0, 1)
    indices = struct.pack('<3H', 0, 1, 2) + b'\0\0'
    binary = positions + uvs + indices
    (directory / 'model.bin').write_bytes(binary)
    names = ['color.png', 'metalrough.png', 'normal.png', 'diffuse.png', 'specgloss.png',
             'plain.png']
    for seed, name in enumerate(names):
        write_png(directory / name, 32, seed + 1)
    primitive = {'attributes': {'POSITION': 0, 'TEXCOORD_0': 1}, 'indices': 2}
    (directory / 'model.gltf').write_text(json.dumps({
        'asset': {'version': '2.0'},
        'extensionsUsed': ['KHR_materials_pbrSpecularGlossiness'],
        'buffers': [{'byteLength': len(binary), 'uri': 'model.bin'}],
        'bufferViews': [{'buffer': 0, 'byteLength': 36},
                        {'buffer': 0, 'byteOffset': 36, 'byteLength': 24},
                        {'buffer': 0, 'byteOffset': 60, 'byteLength': 6}],
        'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
                       'min': [0, 0, 0], 'max': [1, 1, 0]},
                      {'bufferView': 1, 'componentType': 5126, 'count': 3, 'type': 'VEC2'},
                      {'bufferView': 2, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}],
        'images': [{'uri': name} for name in names],
        'textures': [{'source': index} for index in range(len(names))],
        'materials': [
            {'name': 'masked', 'alphaMode': 'MASK', 'normalTexture': {'index': 2},
             'pbrMetallicRoughness': {'baseColorTexture': {'index': 0},
                                      'metallicRoughnessTexture': {'index': 1}}},
            {'name': 'specgloss', 'normalTexture': {'index': 2},
             'extensions': {'KHR_materials_pbrSpecularGlossiness': {
                 'diffuseTexture': {'index': 3}, 'specularGlossinessTexture': {'index': 4}}}},
            {'name': 'plain', 'pbrMetallicRoughness': {'baseColorTexture': {'index': 5}}}],
        'meshes': [{'primitives': [dict(primitive, material=index) for index in range(3)]}],
        'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}))


def build_textures(scene_path):
    """(build folder, {material file: bytes}, {texture name: bytes}, recipes)."""
    scene = jobs.read_managed_scene(scene_path)
    record = next(asset for asset in scene['assets'] if asset['kind'] == 'mesh')
    build = (Path(scene_path).parent / record['artifacts'][0]['path']).parent
    materials = {path.name: path.read_bytes() for path in (build / 'materials').glob('*.mt')}
    names = set()
    for text in materials.values():
        for line in text.decode().splitlines():
            key, _, value = line.partition('=')
            if key.endswith('_texture') and value:
                reference = value.split('?')[0]
                assert reference.startswith('./../textures/'), reference
                names.add(reference[len('./../textures/'):])
    textures = {name: (build / 'textures' / name).read_bytes() for name in names}
    recipes = json.loads((build / 'textures' / 'recipes.json').read_text())['textures']
    return build, materials, textures, recipes


def check_names(build, textures, recipes):
    """Each named texture has a recipe its name derives from, host-neutrally."""
    mesh_recipes = 0
    image_recipes = 0
    for name in textures:
        assert name in recipes, (name, sorted(recipes))
        entry = recipes[name]
        suffix = ('-fast' if entry['speed'] == 'fast' else '') + \
                 ('-preview' if entry['tier'] == 'preview' else '')
        assert name.endswith(f'-{entry["class"]}{suffix}.vkt'), (name, entry)
        if 'source' in entry:
            source = build / 'textures' / entry['source']
            assert name.startswith(hashlib.sha256(source.read_bytes()).hexdigest()), name
            image_recipes += 1
        else:
            recipe = entry['recipe']
            assert 'astc' not in recipe and 'bc-' not in recipe, recipe
            identity = f'{recipe}|{entry["class"]}|{entry["tier"]}|{entry["speed"]}'
            assert name.startswith(hashlib.sha256(identity.encode()).hexdigest()[:16]), name
            mesh_recipes += 1
    return mesh_recipes, image_recipes


def family(data):
    format_ = struct.unpack('<I', data[12:16])[0]
    return 'bc' if format_ in (141, 142, 145, 146) else 'astc' if 157 <= format_ <= 184 else 'other'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bakery', default=str(jobs.default_bakery()))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vkr-recook-') as temporary:
        root = Path(temporary).resolve()
        workspace = root / 'workspace' / '.vkreditor'
        project_id = str(uuid.uuid4())
        project = workspace / 'projects' / project_id
        project.mkdir(parents=True)
        manifest = project / 'project.json'
        manifest.write_text(json.dumps({'version': 1, 'id': project_id, 'name': 'Recook',
                                        'assets': [], 'scenes': [], 'default_font': None}))
        sources = root / 'sources'
        sources.mkdir()
        write_model(sources)
        environment = {'VKR_BAKERY_CACHE': str(root / 'cache')}

        def run(request, name, code=0):
            result_path = root / f'{name}.json'
            job = jobs.Job(dict({'version': 1, 'workspace_root': str(workspace),
                                 'project_path': str(manifest)}, **request),
                           result_path, bakery=args.bakery, environment=environment)
            assert job.execute() == code, job.output
            return jobs.load_json(result_path)

        def create(name, **settings):
            return run(dict({'operation': 'create_scene', 'scene_id': str(uuid.uuid4()),
                             'scene_name': name, 'models': [str(sources / 'model.gltf')],
                             'bakes': {}}, **settings), f'create-{name}')

        final = create('final')
        build, materials, textures, recipes = build_textures(final['scene_path'])
        mesh_recipes, image_recipes = check_names(build, textures, recipes)
        assert mesh_recipes >= 4 and image_recipes >= 1, (mesh_recipes, image_recipes)
        assert {family(data) for data in textures.values()} == {'astc'} or \
            {family(data) for data in textures.values()} == {'bc'}

        again = create('again')
        assert set(build_textures(again['scene_path'])[2]) == set(textures), \
            'A second final cook names the same textures'
        preview = create('preview', texture_tier='preview')
        preview_build, _, preview_textures, preview_recipes = build_textures(preview['scene_path'])
        check_names(preview_build, preview_textures, preview_recipes)
        assert not set(preview_textures) & set(textures), 'Preview names collide with final'
        assert all(preview_recipes[name]['tier'] == 'preview' for name in preview_textures)
        fast = create('fast', texture_encode_speed='fast')
        fast_build, _, fast_textures, fast_recipes = build_textures(fast['scene_path'])
        check_names(fast_build, fast_textures, fast_recipes)
        assert not set(fast_textures) & set(textures), 'Fast names collide with final'
        assert all('-fast' in name and 'astc' not in name for name in fast_textures), fast_textures

        # Deleted textures and a texture of the other GPU family come back
        # under the same names with this host's bytes.
        scene_path = Path(final['scene_path'])
        scene_bytes = scene_path.read_bytes()
        inventory = scene_path.parent / json.loads(scene_bytes)['inventory']
        inventory_bytes = inventory.read_bytes()
        mesh_bytes = (build / 'mesh.vkb').read_bytes()
        names = sorted(textures)
        for name in names[1:]:
            (build / 'textures' / name).unlink()
        foreign = bytearray(textures[names[0]])
        foreign[12:16] = struct.pack('<I', 145 if family(textures[names[0]]) == 'astc' else 157)
        (build / 'textures' / names[0]).write_bytes(bytes(foreign))
        recook = {'operation': 'recook_textures', 'scene_id': final['scene_id'],
                  'scene_path': final['scene_path']}
        recooked = run(recook, 'recook')
        assert recooked['recooked'] == len(textures) and recooked['needs_rebuild'] == [], recooked
        assert recooked['mesh_cooks'] == 1, recooked
        for name in names:
            assert (build / 'textures' / name).read_bytes() == textures[name], name
        assert {path.name: path.read_bytes() for path in (build / 'materials').glob('*.mt')} == materials
        assert (build / 'mesh.vkb').read_bytes() == mesh_bytes
        assert scene_path.read_bytes() == scene_bytes and inventory.read_bytes() == inventory_bytes
        assert not list((project / '.staging').glob('recook-*')), 'No staging folder is left'
        idle = run(recook, 'recook-idle')
        assert idle['recooked'] == 0 and idle['current'] == len(textures), idle

        # Preparation accepts other bytes for a cooked texture, as another
        # host's cook has, and records `platform` for it.
        changed = bytearray(textures[names[1]])
        changed[-1] ^= 0xff
        (build / 'textures' / names[1]).write_bytes(bytes(changed))
        run({'operation': 'prepare_scene', 'scene_id': final['scene_id'],
             'scene_path': final['scene_path']}, 'prepare')
        prepared = jobs.read_managed_scene(scene_path)
        closures = [asset.get('closure') or {} for asset in prepared['assets']]
        assert any(path.endswith('.vkt') for closure in closures for path in closure)
        assert all(value == 'platform' for closure in closures for path, value in closure.items()
                   if path.endswith('.vkt')), closures
        (build / 'textures' / names[1]).write_bytes(textures[names[1]])

        # A share needs no rebuild; its copy without .vkt files cooks them.
        document = json.loads(manifest.read_text())
        document['scenes'] = [{'id': final['scene_id'], 'name': 'final',
                               'path': f'scenes/{final["scene_id"]}/scene.json'}]
        manifest.write_text(json.dumps(document))
        destination = root / 'shared' / 'Recook'
        shared = run({'operation': 'share_project', 'destination': str(destination)}, 'share')
        assert shared['needs_rebuild'] == [] and shared['cooked_without_source_count'] == 0, shared
        assert shared['recookable_textures'] == len(textures), shared
        relative = build.relative_to(workspace)
        copy = destination / '.vkreditor' / relative
        assert (copy / 'textures' / 'recipes.json').is_file()
        for path in (copy / 'textures').glob('*.vkt'):
            path.unlink()
        shared_workspace = destination / '.vkreditor'
        shared_scene = shared_workspace / scene_path.relative_to(workspace)
        restored = run({'operation': 'recook_textures', 'workspace_root': str(shared_workspace),
                        'project_path': str(shared_workspace / manifest.relative_to(workspace)),
                        'scene_id': final['scene_id'], 'scene_path': str(shared_scene)},
                       'recook-shared')
        assert restored['recooked'] == len(textures), restored
        for name in names:
            assert (copy / 'textures' / name).read_bytes() == textures[name], name
        shutil.rmtree(destination)
    print(f'Bakery recook: {mesh_recipes} mesh-cook and {image_recipes} image recipes name their '
          'textures host-neutrally, repeat, keep preview and fast apart, re-cook deleted and '
          'foreign textures in place, accept other hosts\' bytes, and restore a shared copy')


if __name__ == '__main__':
    main()
