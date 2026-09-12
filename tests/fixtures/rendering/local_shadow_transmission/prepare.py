#!/usr/bin/env python3
"""Cook the local transmission matrix and apply its authored material controls.

Run from any directory after ./build_release.sh. No renderer process is started.
The glTF owns geometry and texture references; the adjacent .mt files own exact
numeric material factors, including zero dielectric reflectance and diffuse sheets.
"""

import json
from pathlib import Path
import subprocess


fixture = Path(__file__).resolve().parent
root = fixture.parents[3]
source = fixture / "local_shadow_transmission_matrix.gltf"
tool_dir = root / "build_release/tools"
subprocess.run(
    [str(tool_dir / "vkr_vkt_packer"), "--input-dir", str(fixture),
     "--texture-class", "data-mask", "--strict", "--force"],
    cwd=root, check=True,
)
subprocess.run(
    [str(tool_dir / "vkr_mesh_cooker"), "--input", str(source.relative_to(root)),
     "--output", str(source.with_suffix(".vkb").relative_to(root))],
    cwd=root, check=True,
)

material_dir = root / "assets/materials/local_shadow_transmission_matrix"
for index, material in enumerate(json.loads(source.read_text())["materials"]):
    generated = list(material_dir.glob(f"gltf_mat_*_{index}.mt"))
    if len(generated) != 1:
        raise RuntimeError(f"expected one generated material {index}: {generated}")
    path = generated[0]
    fields = dict(line.split("=", 1) for line in path.read_text().splitlines()
                  if "=" in line)
    generated_name = fields["name"]
    authored = fixture / (material["name"] + ".mt")
    fields.update(line.split("=", 1) for line in authored.read_text().splitlines()
                  if "=" in line)
    fields["name"] = generated_name
    path.write_text("".join(f"{key}={value}\n" for key, value in fields.items()))

print(f"Prepared {source.with_suffix('.vkb').relative_to(root)}")
