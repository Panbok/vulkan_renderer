#!/usr/bin/env python3
"""Render one managed material on a canonical sphere in an isolated harness.

Recipe 2: radius 1, 32 latitude/64 longitude intervals, neutral constant
ambient with two rectangular softboxes, 35 degree camera, manual exposure 1, AgX,
no temporal/post-process effects. The interactive renderer is never used.
Callers own the bounded thumbnail cache.
"""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import uuid

from bake_reflection_probe import digest, confined_path, valid_scene_manifest
from editor_project_jobs import atomic_json, load_json

RECIPE = 'material-sphere-v2'
MAX_LOG_BYTES = 4 * 1024 * 1024
_child = None
_cancelled = False


def stop_child():
    """Stop the complete renderer subtree before releasing job-owned files."""
    global _child
    if _child is None or _child.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(_child.pid), '/T', '/F'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       check=False)
    else:
        # Harness capture children have their own process groups; walking the
        # parent relation also covers those groups before the parent exits.
        listing = subprocess.run(['ps', '-axo', 'pid=,ppid='], capture_output=True,
                                 text=True, check=True).stdout
        relations = [tuple(map(int, line.split())) for line in listing.splitlines()]
        descendants = {_child.pid}
        while True:
            expanded = descendants | {pid for pid, parent in relations if parent in descendants}
            if expanded == descendants:
                break
            descendants = expanded
        for pid in descendants:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            _child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        for pid in descendants:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    _child.wait()


def cancel(*_):
    global _cancelled
    _cancelled = True
    stop_child()
    raise InterruptedError('Material preview cancelled')


def executable(name, override):
    suffix = '.exe' if os.name == 'nt' else ''
    root = Path(__file__).resolve().parent.parent
    candidates = [Path(override)] if override else [
        Path(__file__).resolve().parent / (name + suffix),
        root / 'bin' / (name + suffix),
        root / 'build_release' / 'tools' / (name + suffix),
        root / 'build_release' / 'tools' / 'Release' / (name + suffix),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise ValueError(f'Missing installed {name}; supply its executable path')


def run(command, log, environment):
    global _child
    if _cancelled:
        raise InterruptedError('Material preview cancelled')
    # Stream to disk: capture output is not retained unbounded in Python memory.
    with log.open('wb') as output:
        _child = subprocess.Popen(list(map(str, command)), stdout=output,
                                  stderr=subprocess.STDOUT, env=environment)
        try:
            code = _child.wait(timeout=180)
        except BaseException:
            stop_child()
            raise
        finally:
            _child = None
    if log.stat().st_size > MAX_LOG_BYTES:
        raise ValueError('Material preview exceeded its 4 MiB diagnostic limit')
    text = log.read_text(encoding='utf-8', errors='replace')
    if code:
        raise RuntimeError(f'{Path(command[0]).name} failed ({code}); see {log}')
    return text


def sphere_source(path):
    # Same UV/ring and triangle convention as the existing geometry sphere.
    latitude, longitude = 32, 64
    with path.open('w', encoding='utf-8', newline='\n') as output:
        output.write('mtllib sphere.mtl\no MaterialPreviewSphere\nusemtl preview_surface\n')
        for lat in range(latitude + 1):
            phi = math.pi * lat / latitude
            for lon in range(longitude + 1):
                theta = 2 * math.pi * lon / longitude
                x = math.sin(phi) * math.cos(theta)
                y = math.cos(phi)
                z = math.sin(phi) * math.sin(theta)
                output.write(f'v {x:.9g} {y:.9g} {z:.9g}\n')
                output.write(f'vn {x:.9g} {y:.9g} {z:.9g}\n')
                output.write(f'vt {lon / longitude:.9g} {1 - lat / latitude:.9g}\n')
        ring = longitude + 1
        for lat in range(latitude):
            for lon in range(longitude):
                a, b = lat * ring + lon + 1, (lat + 1) * ring + lon + 1
                # Skip degenerate pole triangles; normals and UV seam remain.
                triangles = []
                if lat != latitude - 1:
                    triangles.append((a, b, b + 1))
                if lat != 0:
                    triangles.append((a, b + 1, a + 1))
                for triangle in triangles:
                    output.write('f ' + ' '.join(f'{i}/{i}/{i}' for i in triangle) + '\n')
    path.with_suffix('.mtl').write_text('newmtl preview_surface\nKd 0.5 0.5 0.5\n', encoding='utf-8')


# Neutral studio: the mean of the former generated sky gradient as a uniform
# ambient, and softboxes at its former panel directions, angular extents and
# radiance. Each entry is (direction toward the panel, width/height degrees,
# radiance). No active-scene sky, exposure or external image is involved.
STUDIO_AMBIENT = [0.24, 0.24, 0.24]
STUDIO_SOFTBOXES = (((0.42, 0.15, 0.89), (39.0, 73.0), 1.2),
                    ((0.51, 0.52, -0.69), (67.0, 62.0), 3.0))
STUDIO_DISTANCE = 6.0


def normalized(vector):
    length = math.sqrt(sum(component * component for component in vector))
    return [component / length for component in vector]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]]


def quaternion_from_basis(x, y, z):
    """Returns [x, y, z, w] for the rotation whose matrix columns are x, y, z."""
    trace = x[0] + y[1] + z[2]
    if trace > 0:
        scale = math.sqrt(trace + 1.0) * 2.0
        return [(y[2] - z[1]) / scale, (z[0] - x[2]) / scale,
                (x[1] - y[0]) / scale, 0.25 * scale]
    if x[0] > y[1] and x[0] > z[2]:
        scale = math.sqrt(1.0 + x[0] - y[1] - z[2]) * 2.0
        return [0.25 * scale, (y[0] + x[1]) / scale, (z[0] + x[2]) / scale,
                (y[2] - z[1]) / scale]
    if y[1] > z[2]:
        scale = math.sqrt(1.0 + y[1] - x[0] - z[2]) * 2.0
        return [(y[0] + x[1]) / scale, 0.25 * scale, (z[1] + y[2]) / scale,
                (z[0] - x[2]) / scale]
    scale = math.sqrt(1.0 + z[2] - x[0] - y[1]) * 2.0
    return [(z[0] + x[2]) / scale, (z[1] + y[2]) / scale, 0.25 * scale,
            (x[1] - y[0]) / scale]


def studio_softboxes():
    """Rectangle lights emit along local -Z, so local +Z points at the panel."""
    entities = []
    for index, (direction, extent_degrees, radiance) in enumerate(STUDIO_SOFTBOXES):
        forward = normalized(direction)
        right = normalized(cross([0.0, 1.0, 0.0], forward))
        up = cross(forward, right)
        size = [2.0 * STUDIO_DISTANCE * math.tan(math.radians(degrees) * 0.5)
                for degrees in extent_degrees]
        entities.append({
            'name': f'Studio softbox {index + 1}', 'parent': None,
            'transform': {'pos': [component * STUDIO_DISTANCE for component in forward],
                          'rot': quaternion_from_basis(right, up, forward),
                          'scale': [1, 1, 1]},
            'rectangle_light': {'color': [1, 1, 1], 'radiance': radiance, 'size': size}})
    return entities


def publication(text, root):
    candidates = []
    for line in text.splitlines():
        if line.startswith('{') and '"report"' in line:
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue
            if item.get('report') and item.get('sha256'):
                candidates.append(item)
    if not candidates:
        raise ValueError('Material snapshot did not publish a verified report')
    item = candidates[-1]
    report = confined_path(root, item['report'], 'Preview report')
    if digest(report) != item['sha256']:
        raise ValueError('Material snapshot report digest mismatch')
    return report, load_json(report)


def render(args):
    workspace = Path(args.workspace).resolve(strict=True)
    if load_json(workspace / 'workspace.json').get('version') != 1:
        raise ValueError('Select a supported initialized workspace')
    material = Path(args.input).resolve(strict=True)
    if not material.is_relative_to(workspace) or material.suffix != '.mt' or not material.is_file():
        raise ValueError('Material preview input must be a managed .mt file')
    output = Path(args.output).resolve()
    cache = (workspace / 'cache' / 'thumbnails').resolve()
    if not cache.is_relative_to(workspace) or not output.is_relative_to(cache) or output.suffix != '.png':
        raise ValueError('Preview output must remain in workspace/cache/thumbnails')
    harness = executable('vkr_harness', args.harness)
    cooker = executable('vkr_mesh_cooker', args.mesh_cooker)
    jobs = workspace / 'jobs'
    jobs.mkdir(exist_ok=True)
    if not jobs.resolve().is_relative_to(workspace):
        raise ValueError('Workspace jobs folder escapes the workspace')
    job = Path(tempfile.mkdtemp(prefix='material-preview-', dir=jobs))
    run_root = None
    success = False
    environment = dict(os.environ)
    environment.setdefault("VKR_HARNESS_RENDERER_BACKEND",
                           "metal" if sys.platform == "darwin" else "vulkan")
    for key in ('MTL_DEBUG_LAYER', 'MTL_SHADER_VALIDATION', 'VK_INSTANCE_LAYERS',
                'VKR_SCENE_PATH', 'VKR_AUTOLOAD_SCENE'):
        environment.pop(key, None)
    try:
        source = job / 'sphere.obj'
        sphere_source(source)
        bundle = job / 'sphere'
        bundle.mkdir()
        mesh = bundle / 'sphere.vkb'
        run([cooker, '--input', source, '--output', mesh, '--bundle-root', bundle,
             '--import-id', str(uuid.uuid4())], job / 'cook.log', environment)
        remap_path = Path(str(mesh) + '.remap.json')
        remap = load_json(remap_path)
        mappings = remap.get('materials')
        if remap.get('version') != 1 or not isinstance(mappings, dict) or len(mappings) != 1:
            raise ValueError('Canonical sphere must have exactly one material range')
        remap['materials'] = {key: './' + os.path.relpath(material, bundle).replace(os.sep, '/')
                              for key in mappings}
        atomic_json(remap_path, remap)
        transform = {'pos': [0, 0, 0], 'rot': [0, 0, 0, 1], 'scale': [1, 1, 1]}
        scene = {'version': 2, 'environment': {'enabled': True, 'intensity': 1,
                 'diffuse_intensity': 1, 'specular_intensity': 1,
                 'constant': STUDIO_AMBIENT},
                 'reflection_probes': [], 'entities': [
                     {'name': 'Canonical sphere', 'parent': None, 'transform': transform,
                      'mesh': {'path': mesh.as_posix(), 'pipeline_domain': 'world'}},
                     *studio_softboxes()]}
        scene_path = job / 'preview.scene.json'
        atomic_json(scene_path, scene)
        case = {'schema_version': 1, 'asset_context': 'managed_workspace',
                'id': 'local.material_preview', 'suite': 'local',
                'description': RECIPE, 'scene': scene_path.relative_to(workspace).as_posix(),
                'seed': 1, 'resolution': [args.size, args.size], 'boot': 'full',
                'target': 'offscreen', 'present': 'none', 'target_image_count': 2,
                'cache': 'isolated_cold', 'fixed_delta': 1 / 60, 'repetitions': 1,
                'repetition_timeout_ms': 120000, 'asset_ready_timeout_ms': 90000,
                'frames': {'warmup': 4, 'measure': 1},
                'renderer': {'editor': False, 'skybox': True, 'shadow_preset': 'balanced',
                             'shadow_cascades': 4, 'taa_enabled': False, 'tonemap_enabled': True,
                             'fxaa_enabled': True, 'exposure_mode': 'manual', 'manual_exposure': 1,
                             'display_transform': 'agx', 'bloom_enabled': False, 'gtao_enabled': False,
                             'image_sharpness': 0, 'ibl_probe_limit': 0, 'render_mode': 'default'},
                'camera': {'mode': 'static', 'position': [0, 0, 4], 'yaw': -90, 'pitch': 0,
                           'vertical_fov_degrees': 35, 'near_plane': 0.1, 'far_plane': 20},
                'captures': [{'at_frame': 0, 'channels': ['final_color']}],
                'assertions': [{'metric': 'visibility.gbuffer.resolve_invalid', 'stat': 'max', 'max': 0}]}
        profile = {'schema_version': 1, 'id': 'local.material_preview', 'authoritative': False,
                   'dirty_policy': 'allow', 'environment': {'target': 'offscreen',
                   'required_present': 'none', 'require_actual_present': False},
                   'instrumentation': {'gpu_timing': False, 'event_subjects': False},
                   'execution': {'minimum_repetitions': 1, 'warmup_stability_window': 2,
                   'warmup_max_drift_ratio': 1, 'require_warmup_stability': False,
                   'exclusive_gpu_lane': False}, 'required_metrics': []}
        case_path, profile_path = job / 'case.json', job / 'profile.json'
        atomic_json(case_path, case)
        atomic_json(profile_path, profile)
        text = run([harness, 'snapshot', '--repo-root', workspace,
                    '--case', case_path.relative_to(workspace),
                    '--profile', profile_path.relative_to(workspace)], job / 'render.log', environment)
        report_path, report = publication(text, workspace)
        run_root = report_path.parent
        if report.get('status') != 'pass' or report.get('case', {}).get('id') != case['id']:
            raise ValueError('Material snapshot did not pass for the requested preview')
        valid_scene_manifest(load_json(run_root / 'scene-content-manifest.json'), scene_path, workspace)
        captures = [item for item in report.get('captures', []) if item.get('channel') == 'final_color']
        if len(captures) != 1:
            raise ValueError('Material snapshot must contain one final-color capture')
        capture = captures[0]
        if (capture.get('width'), capture.get('height')) != (args.size, args.size):
            raise ValueError('Material snapshot has unexpected dimensions')
        png = confined_path(run_root, capture.get('data_path'), 'Preview image')
        if digest(png) != capture.get('data_sha256'):
            raise ValueError('Material snapshot image digest mismatch')
        with png.open('rb') as image:
            header = image.read(24)
        if (header[:8] != b'\x89PNG\r\n\x1a\n' or header[12:16] != b'IHDR' or
                struct.unpack('>II', header[16:24]) != (args.size, args.size)):
            raise ValueError('Material snapshot is not the requested bounded PNG')
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + '.', delete=False) as target:
            temporary = Path(target.name)
            with png.open('rb') as image:
                shutil.copyfileobj(image, target)
            target.flush()
            os.fsync(target.fileno())
        try:
            os.replace(temporary, output)
        finally:
            temporary.unlink(missing_ok=True)
        success = True
        print(json.dumps({'status': 'ready', 'recipe': RECIPE, 'output': str(output),
                          'sha256': digest(output)}), flush=True)
    finally:
        stop_child()
        if success or _cancelled:
            shutil.rmtree(job)
            if run_root and run_root.is_relative_to(workspace / 'build' / '_artifacts' / 'snapshot'):
                shutil.rmtree(run_root)
        else:
            print(f'Material preview diagnostic job: {job}', file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--workspace', required=True)
    parser.add_argument('--size', type=int, choices=(128, 256), default=128)
    parser.add_argument('--harness')
    parser.add_argument('--mesh-cooker')
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, cancel)
    signal.signal(signal.SIGINT, cancel)
    try:
        render(args)
    except (ValueError, OSError, RuntimeError, InterruptedError, subprocess.SubprocessError) as error:
        print(f'Material preview failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
