#!/usr/bin/env python3
"""Isolated cooker regressions: embedded images, MTL origin/collisions, relocation.

No scene render or repository assets are used.
"""
import argparse, base64, json, struct, zlib, subprocess, shutil, hashlib, tempfile
from pathlib import Path
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cooker', required=True)
args = parser.parse_args()
temporary = tempfile.TemporaryDirectory(prefix='vkr-managed-mesh-')
root = Path(temporary.name)

def png(r, g, b):

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress((b'\x00' + bytes([r, g, b]) * 2) * 2)) + chunk(b'IEND', b'')
pixels = png(255, 0, 0)
vertices = struct.pack('<9f', 0, 0, 0, 1, 0, 0, 0, 1, 0)
buffer = vertices + struct.pack('<9f', 0, 0, 1, 0, 0, 1, 0, 0, 1) + struct.pack('<6f', 0, 0, 1, 0, 0, 1) + struct.pack('<3H', 0, 1, 2)
gltf = {'asset': {'version': '2.0'}, 'buffers': [{'byteLength': len(buffer), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(buffer).decode()}], 'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': 36}, {'buffer': 0, 'byteOffset': 36, 'byteLength': 36}, {'buffer': 0, 'byteOffset': 72, 'byteLength': 24}, {'buffer': 0, 'byteOffset': 96, 'byteLength': 6}], 'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3', 'min': [0, 0, 0], 'max': [1, 1, 0]}, {'bufferView': 1, 'componentType': 5126, 'count': 3, 'type': 'VEC3'}, {'bufferView': 2, 'componentType': 5126, 'count': 3, 'type': 'VEC2'}, {'bufferView': 3, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}], 'images': [{'uri': 'data:image/png;base64,' + base64.b64encode(pixels).decode()}], 'textures': [{'source': 0}], 'materials': [{'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}}}], 'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2}, 'indices': 3, 'material': 0}]}], 'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}
(root / 'embedded.gltf').write_text(json.dumps(gltf))
pad = buffer + b'\x00' * (-len(buffer) % 4)
binary = pad + pixels
binary += b'\x00' * (-len(binary) % 4)
glb = json.loads(json.dumps(gltf))
glb['buffers'] = [{'byteLength': len(binary)}]
glb['bufferViews'].append({'buffer': 0, 'byteOffset': len(pad), 'byteLength': len(pixels)})
glb['images'] = [{'bufferView': 4, 'mimeType': 'image/png'}]
j = json.dumps(glb).encode()
j += b' ' * (-len(j) % 4)
(root / 'embedded.glb').write_bytes(struct.pack('<III', 1179937895, 2, 12 + 8 + len(j) + 8 + len(binary)) + struct.pack('<II', len(j), 1313821514) + j + struct.pack('<II', len(binary), 5130562) + binary)
for directory, color in [('a', (255, 0, 0)), ('b', (0, 0, 255))]:
    (root / 'nested' / directory).mkdir(parents=True, exist_ok=True)
    (root / 'nested' / directory / 'same.png').write_bytes(png(*color))
(root / 'nested' / 'test.mtl').write_text('newmtl first\nmap_Kd a/same.png\nnewmtl second\nmap_Kd b/same.png\n')
(root / 'test.obj').write_text('mtllib nested/test.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\nusemtl first\nf 1/1/1 2/2/1 3/3/1\nusemtl second\nf 1/1/1 2/2/1 3/3/1\n')
exe = Path(args.cooker).resolve()
for source in ['embedded.gltf', 'embedded.glb', 'test.obj']:
    bundle = root / (source + '.bundle')
    bundle.mkdir(exist_ok=True)
    result = subprocess.run([str(exe), '--input', str(root / source), '--output', str(bundle / 'mesh.vkb'), '--bundle-root', str(bundle), '--import-id', 'fixture-import'], capture_output=True, text=True)
    print(source, result.returncode, result.stdout, result.stderr)
    assert result.returncode == 0
    blob = (bundle / 'mesh.vkb').read_bytes()
    assert str(root).encode() not in blob
    for material in (bundle / 'materials').glob('*.mt'):
        for line in material.read_text().splitlines():
            k, _, v = line.partition('=')
            if k.endswith('_texture') and v:
                assert v.startswith('./'), (material, line)
                assert (material.parent / v.split('?')[0]).is_file(), (material, line)
    assert len(list((bundle / 'materials').glob('*.mt'))) == (2 if source == 'test.obj' else 1)
    if source == 'test.obj':
        assert len(list((bundle / 'textures').glob('*.*'))) == 2
    print('portable refs OK', hashlib.sha256(blob).hexdigest())
relocated = root / 'relocated'
relocated.mkdir(exist_ok=True)
shutil.copy(root / 'embedded.gltf', relocated / 'embedded.gltf')
out = relocated / 'bundle'
out.mkdir(exist_ok=True)
subprocess.run([str(exe), '--input', str(relocated / 'embedded.gltf'), '--output', str(out / 'mesh.vkb'), '--bundle-root', str(out), '--import-id', 'fixture-import'], check=True)
assert (out / 'mesh.vkb').read_bytes() == (root / 'embedded.gltf.bundle' / 'mesh.vkb').read_bytes()
print('relocated recook byte-identical')
for name in ['missing', 'malformed']:
    bad = json.loads(json.dumps(gltf))
    bad['images'][0]['uri'] = 'missing.png' if name == 'missing' else 'data:image/png;base64,AAAA'
    source = root / (name + '.gltf')
    source.write_text(json.dumps(bad))
    bundle = root / (name + '.bundle')
    bundle.mkdir()
    result = subprocess.run([str(exe), '--input', str(source), '--output', str(bundle / 'mesh.vkb'), '--bundle-root', str(bundle), '--import-id', 'fixture-import'], capture_output=True)
    assert result.returncode != 0 and (not (bundle / 'mesh.vkb').exists()), name
print('missing and malformed images rejected without publishing mesh')
# Metadata variants retain source identity and packed geometry exactly.
mesh = root / 'embedded.gltf.bundle' / 'mesh.vkb'
report = root / 'inspection.json'
subprocess.run([str(exe), '--inspect', '--input', str(mesh), '--output', str(report)], check=True)
info = json.loads(report.read_text())
patch = root / 'patch.json'
variant = root / 'variant.vkb'
patch.write_text(json.dumps({'version': 1, 'source_fingerprint': info['fingerprint'], 'nodes': []}))
subprocess.run([str(exe), '--input', str(mesh), '--output', str(variant), '--source-patches', str(patch)], check=True)
assert variant.read_bytes() == mesh.read_bytes(), 'No-op source variant changed cooked bytes'
patch.write_text(json.dumps({'version': 1, 'source_fingerprint': info['fingerprint'], 'nodes': [
    {'index': 0, 'position': [2, 3, 4], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1], 'visible': False}]}))
subprocess.run([str(exe), '--input', str(mesh), '--output', str(variant), '--source-patches', str(patch)], check=True)
subprocess.run([str(exe), '--inspect', '--input', str(variant), '--output', str(report)], check=True)
assert json.loads(report.read_text())['fingerprint'] == info['fingerprint']
original, modified = mesh.read_bytes(), variant.read_bytes()
stream_offset = struct.unpack_from('<Q', original, 80)[0]
assert original[stream_offset:] == modified[stream_offset:]
assert original != modified
print('Source variant preserves packed geometry and source identity; no-op is byte-identical')
temporary.cleanup()
