"""Read static template GLB geometry independently of Blender authoring.

Check binary/accessor bounds, finite geometry and node transforms, unit normals,
triangle indices, and the exported scene bounds using Python's standard library.
"""

import argparse
import json
import math
from pathlib import Path
import struct


COMPONENTS = {5120: ('b', 1), 5121: ('B', 1), 5122: ('h', 2),
              5123: ('H', 2), 5125: ('I', 4), 5126: ('f', 4)}
WIDTHS = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def multiply(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4))
             for c in range(4)] for r in range(4)]


def transform(matrix, point):
    return [sum(matrix[row][column] * point[column] for column in range(3))
            + matrix[row][3] for row in range(3)]


def trs(translation, quaternion, scale):
    assert len(translation) == 3 and len(quaternion) == 4 and len(scale) == 3
    assert all(math.isfinite(value) for value in (*translation, *quaternion, *scale))
    assert abs(sum(value * value for value in quaternion) - 1.0) < .001
    x, y, z, w = quaternion
    result = [[1 - 2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w), translation[0]],
              [2*(x*y+z*w), 1 - 2*(x*x+z*z), 2*(y*z-x*w), translation[1]],
              [2*(x*z-y*w), 2*(y*z+x*w), 1 - 2*(x*x+y*y), translation[2]],
              [0, 0, 0, 1]]
    for row in range(3):
        for column in range(3):
            result[row][column] *= scale[column]
    return result


class Glb:
    def __init__(self, path):
        raw = path.read_bytes()
        assert len(raw) >= 20, 'Truncated GLB header'
        magic, version, length = struct.unpack_from('<III', raw)
        assert magic == 0x46546c67 and version == 2 and length == len(raw)
        offset = 12
        self.binary = b''
        self.doc = None
        while offset < length:
            assert offset + 8 <= length, 'Truncated GLB chunk header'
            size, kind = struct.unpack_from('<II', raw, offset)
            offset += 8
            assert size % 4 == 0 and offset + size <= length, 'Invalid GLB chunk bounds'
            chunk = raw[offset:offset+size]
            if kind == 0x4e4f534a:
                assert self.doc is None, 'Multiple JSON chunks'
                self.doc = json.loads(chunk)
            elif kind == 0x004e4942:
                assert not self.binary, 'Multiple binary chunks'
                self.binary = chunk
            offset += size
        assert self.doc is not None and self.binary, 'Missing GLB payload'
        assert self.doc.get('asset', {}).get('version') == '2.0'
        buffers = self.doc.get('buffers', [])
        assert len(buffers) == 1 and 'uri' not in buffers[0], 'GLB must be self-contained'
        assert 0 < buffers[0]['byteLength'] <= len(self.binary)
        self.buffer_length = buffers[0]['byteLength']
        self.cache = {}
        self.parents = {}
        nodes = self.doc.get('nodes', [])
        for index, node in enumerate(nodes):
            for child in node.get('children', []):
                assert isinstance(child, int) and 0 <= child < len(nodes), 'Invalid child node'
                assert child not in self.parents, 'Multiple node parents'
                self.parents[child] = index

    def accessor(self, index):
        if index in self.cache:
            return self.cache[index]
        accessor = self.doc['accessors'][index]
        assert 'sparse' not in accessor, 'This validator requires dense accessors'
        view = self.doc['bufferViews'][accessor['bufferView']]
        assert view.get('buffer', 0) == 0
        view_base = view.get('byteOffset', 0)
        assert view_base >= 0 and view['byteLength'] > 0
        assert view_base + view['byteLength'] <= self.buffer_length
        code, size = COMPONENTS[accessor['componentType']]
        width = WIDTHS[accessor['type']]
        stride = view.get('byteStride', width*size)
        relative_base = accessor.get('byteOffset', 0)
        assert relative_base >= 0 and relative_base % size == 0
        assert stride >= width*size and stride % size == 0
        assert accessor['count'] > 0
        assert relative_base + (accessor['count']-1)*stride + width*size <= view['byteLength']
        base = view_base + relative_base
        values = [list(struct.unpack_from('<'+code*width, self.binary, base+index*stride))
                  for index in range(accessor['count'])]
        if accessor.get('normalized') and accessor['componentType'] != 5126:
            maximum = {5120: 127, 5121: 255, 5122: 32767, 5123: 65535}[accessor['componentType']]
            values = [[max(-1, value/maximum) for value in row] for row in values]
        assert all(math.isfinite(value) for row in values for value in row)
        self.cache[index] = values
        return values

    def node_transforms(self):
        cache = {}
        visiting = set()

        def evaluate(index):
            if index in cache:
                return cache[index]
            assert index not in visiting, 'Cycle in node hierarchy'
            visiting.add(index)
            node = self.doc['nodes'][index]
            if 'matrix' in node:
                assert not any(name in node for name in ('translation', 'rotation', 'scale'))
                values = node['matrix']
                assert len(values) == 16 and all(math.isfinite(value) for value in values)
                matrix = [[values[row+column*4] for column in range(4)] for row in range(4)]
                assert all(abs(matrix[3][column] - float(column == 3)) < 1e-6
                           for column in range(4)), 'Non-affine node matrix'
            else:
                matrix = trs(node.get('translation', [0, 0, 0]),
                             node.get('rotation', [0, 0, 0, 1]),
                             node.get('scale', [1, 1, 1]))
            if index in self.parents:
                matrix = multiply(evaluate(self.parents[index]), matrix)
            assert all(math.isfinite(value) for row in matrix for value in row)
            visiting.remove(index)
            cache[index] = matrix
            return matrix

        return [evaluate(index) for index in range(len(self.doc.get('nodes', [])))]


def inspect(path):
    glb = Glb(path)
    document = glb.doc
    assert not document.get('extensionsRequired'), 'Unsupported required extension'
    assert not document.get('animations') and not document.get('skins'), 'Expected static geometry'
    triangles = 0
    vertices = 0
    primitives = 0
    rest = glb.node_transforms()
    lower = [math.inf]*3
    upper = [-math.inf]*3
    for node_index, node in enumerate(document.get('nodes', [])):
        if 'mesh' not in node:
            continue
        assert 0 <= node['mesh'] < len(document['meshes'])
        for primitive in document['meshes'][node['mesh']]['primitives']:
            assert primitive.get('mode', 4) == 4 and not primitive.get('targets')
            attributes = primitive['attributes']
            assert document['accessors'][attributes['POSITION']]['type'] == 'VEC3'
            assert document['accessors'][attributes['NORMAL']]['type'] == 'VEC3'
            positions = glb.accessor(attributes['POSITION'])
            normals = glb.accessor(attributes['NORMAL'])
            assert len(normals) == len(positions)
            for normal in normals:
                assert .95 < sum(value*value for value in normal) < 1.05
            for accessor_index in attributes.values():
                assert len(glb.accessor(accessor_index)) == len(positions)
            if 'indices' in primitive:
                index_accessor = document['accessors'][primitive['indices']]
                assert index_accessor['type'] == 'SCALAR'
                assert index_accessor['componentType'] in (5121, 5123, 5125)
                assert not index_accessor.get('normalized')
                indices = glb.accessor(primitive['indices'])
                assert len(indices) % 3 == 0
                assert all(0 <= value[0] < len(positions) for value in indices)
                triangles += len(indices)//3
            else:
                assert len(positions) % 3 == 0
                triangles += len(positions)//3
            primitives += 1
            vertices += len(positions)
            for position in positions:
                world = transform(rest[node_index], position)
                assert all(math.isfinite(value) for value in world)
                for axis in range(3):
                    lower[axis] = min(lower[axis], world[axis])
                    upper[axis] = max(upper[axis], world[axis])
    assert triangles > 0 and all(math.isfinite(value) for value in (*lower, *upper))
    assert all(low <= high for low, high in zip(lower, upper))
    return {'path': str(path), 'bytes': path.stat().st_size, 'triangles': triangles,
            'vertices': vertices, 'primitives': primitives, 'nodes': len(rest),
            'bounds': [lower, upper], 'finite_geometry_and_transforms': True,
            'unit_normals': True, 'valid_triangle_indices': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', type=Path, nargs='+')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    reports = [inspect(path.resolve()) for path in args.paths]
    text = json.dumps({'status': 'passed', 'assets': reports}, indent=2)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text+'\n', encoding='utf-8')
    print(text)


if __name__ == '__main__':
    main()
