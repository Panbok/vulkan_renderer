#!/usr/bin/env python3
"""Exercise native UTF-8 argv, glTF buffers/textures and KTX output boundaries.

Synthetic single-triangle assets isolate path I/O; no scene render is involved.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from urllib.parse import quote
import zlib


def png():
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress((b"\0" + bytes([255, 0, 0]) * 2) * 2)) + chunk(b"IEND", b""))


def run(command):
    completed = subprocess.run([str(value) for value in command], capture_output=True)
    if completed.returncode:
        raise AssertionError(f"{command!r} exited {completed.returncode}\n"
                             + completed.stdout.decode("utf-8", errors="replace")
                             + completed.stderr.decode("utf-8", errors="replace"))


def check(root, cooker, hdr, packer):
    root.mkdir(parents=True)
    buffer_name = "\u0431\u0443\u0444\u0435\u0440 #%.bin"
    image_name = "\u0442\u0435\u043a\u0441\u0442\u0443\u0440\u0430 #%.png"
    vertices = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0)
    buffer = vertices + struct.pack("<9f", 0, 0, 1, 0, 0, 1, 0, 0, 1) + struct.pack("<6f", 0, 0, 1, 0, 0, 1) + struct.pack("<3H", 0, 1, 2)
    (root / buffer_name).write_bytes(buffer)
    (root / image_name).write_bytes(png())
    document = {"asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(buffer), "uri": quote(buffer_name)}],
        "bufferViews": [{"buffer": 0, "byteOffset": offset, "byteLength": length} for offset, length in [(0, 36), (36, 36), (72, 24), (96, 6)]],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"}],
        "images": [{"uri": quote(image_name)}], "textures": [{"source": 0}],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0}
    source = root / "\u043c\u043e\u0434\u0435\u043b\u044c.gltf"
    source.write_text(json.dumps(document), encoding="utf-8")
    bundle = root / "\u0440\u0435\u0437\u0443\u043b\u044c\u0442\u0430\u0442"
    bundle.mkdir()
    output = bundle / "\u0441\u0435\u0442\u043a\u0430.vkb"
    run([cooker, "--input", source, "--output", output, "--bundle-root", bundle, "--import-id", "native-path-fixture"])
    assert output.stat().st_size > 0
    textures = list((bundle / "textures").glob("*.png"))
    assert textures and textures[0].read_bytes() == png(), "glTF texture identity changed"
    if packer:
        texture_output = root / "\u0442\u0435\u043a\u0441\u0442\u0443\u0440\u0430.vkt"
        run([packer, "--type", "2d", "--layer", root / image_name, "--output", texture_output])
        assert texture_output.read_bytes().startswith(b"\xabKTX 20\xbb\r\n\x1a\n")
    if hdr:
        faces = []
        for face in range(6):
            path = root / f"\u0433\u0440\u0430\u043d\u044c-{face}.rgba16f"
            path.write_bytes(struct.pack("<4e", 1, 0.5, 0.25, 1) * 4)
            faces += ["--face", path]
        cube = root / "\u043a\u0443\u0431.vkt"
        run([hdr, "--size", "2", "--output", cube, *faces])
        assert cube.read_bytes().startswith(b"\xabKTX 20\xbb\r\n\x1a\n")
    print(json.dumps({"path_characters": len(str(output)), "mesh_sha256": hashlib.sha256(output.read_bytes()).hexdigest(), "textures": len(textures)}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cooker", type=Path, required=True)
    parser.add_argument("--hdr", type=Path)
    parser.add_argument("--packer", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vkr-native-path-") as temporary:
        root = Path(temporary)
        check(root / "\u043a\u043e\u0440\u043e\u0442\u043a\u0438\u0439", args.cooker.resolve(), args.hdr.resolve() if args.hdr else None, args.packer.resolve() if args.packer else None)
        nested = root / "\u0434\u043b\u0438\u043d\u043d\u044b\u0439"
        while len(str(nested)) < 350:
            nested /= "nested-segment-0123456789"
        check(nested, args.cooker.resolve(), args.hdr.resolve() if args.hdr else None, args.packer.resolve() if args.packer else None)
    print("native UTF-8 and long-path fixture passed")


if __name__ == "__main__":
    main()
