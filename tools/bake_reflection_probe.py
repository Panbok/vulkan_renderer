#!/usr/bin/env python3
"""Bake a static scene's radiance into a portable HDR reflection cubemap."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid

REPO = Path(__file__).resolve().parents[1]
RECIPE_VERSION = 1
FACES = ('px', 'nx', 'py', 'ny', 'pz', 'nz')
CHANNEL = 'hdr_post_transmission'
SCENE_MANIFEST_KIND = 'vkr.harness.scene-content-manifest'


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            result.update(block)
    return 'sha256:' + result.hexdigest()


def confined_path(root, relative, label):
    if not isinstance(relative, str) or not relative:
        raise ValueError(f'{label} path is missing')
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError(f'{label} path escapes its artifact root')
    return path


def output_paths(value):
    output = Path(value).resolve()
    if output.suffix != '.vkt':
        raise ValueError('--output must name a .vkt file')
    return output, Path(str(output) + '.bake.json').resolve()


def executable(name, override):
    if override:
        path = Path(override).resolve()
    else:
        suffix = '.exe' if os.name == 'nt' else ''
        path = REPO / 'build_release' / 'tools' / (name + suffix)
        if not path.is_file():
            path = path.parent / 'Release' / (name + suffix)
    if not path.is_file():
        raise ValueError(f'Missing {name}; build with build_release.sh/.bat first')
    return path


def atomic_json(path, value):
    with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                     prefix=path.name + '.', suffix='.tmp', delete=False) as output:
        temporary = Path(output.name)
        json.dump(value, output, indent=2)
        output.write('\n')
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def valid_scene_manifest(manifest, scene=None):
    if not isinstance(manifest, dict):
        raise ValueError('Scene-content manifest is not an object')
    if manifest.get('schema_version') != 1 or manifest.get('kind') != SCENE_MANIFEST_KIND:
        raise ValueError('Unexpected scene-content manifest contract')
    if not isinstance(manifest.get('scene'), str) or not manifest['scene']:
        raise ValueError('Scene-content manifest is missing its scene')
    if scene is not None and manifest['scene'] != scene.relative_to(REPO).as_posix():
        raise ValueError('Scene-content manifest scene differs from the requested scene')
    if not isinstance(manifest.get('sha256'), str) or not manifest['sha256'].startswith('sha256:'):
        raise ValueError('Scene-content manifest is missing its digest')
    assets = manifest.get('assets')
    if not isinstance(assets, list) or not assets:
        raise ValueError('Scene-content manifest has no assets')
    paths = set()
    for asset in assets:
        if not isinstance(asset, dict) or not isinstance(asset.get('path'), str):
            raise ValueError('Scene-content manifest contains an invalid asset')
        path = confined_path(REPO, asset['path'], 'Scene asset')
        if path in paths or not path.is_file() or not isinstance(asset.get('bytes'), int):
            raise ValueError('Scene-content manifest contains an unavailable asset')
        if (asset['bytes'] < 0 or asset['bytes'] >= 1 << 64 or
                path.stat().st_size != asset['bytes'] or digest(path) != asset.get('sha256')):
            raise ValueError('Scene input changed since the harness manifest was written')
        paths.add(path)
    hash_state = hashlib.sha256()
    for asset in sorted(assets, key=lambda item: item['path']):
        encoded_path = asset['path'].encode('utf-8')
        hash_state.update(len(encoded_path).to_bytes(4, 'big'))
        hash_state.update(encoded_path)
        hash_state.update(asset['sha256'].encode('ascii'))
        hash_state.update(asset['bytes'].to_bytes(8, 'big'))
    if manifest['sha256'] != 'sha256:' + hash_state.hexdigest():
        raise ValueError('Scene-content manifest aggregate digest mismatch')
    return paths


def protect_source_assets(output, sidecar, scene, manifest):
    sources = valid_scene_manifest(manifest, scene)
    sources.add(scene)
    conflicts = [path for path in (output, sidecar) if path in sources]
    if conflicts:
        raise ValueError('Output or bake metadata would overwrite a scene source asset')


def report_fingerprint(report):
    comparison = report.get('comparison')
    if not isinstance(comparison, dict):
        raise ValueError('Snapshot report is missing comparison fingerprints')
    names = ('environment_fingerprint', 'workload_fingerprint', 'policy_fingerprint')
    values = tuple(comparison.get(name) for name in names)
    if not all(isinstance(value, str) and value.startswith('sha256:') for value in values):
        raise ValueError('Snapshot report has invalid comparison fingerprints')
    return values


def report_provenance(report):
    provenance = report.get('provenance')
    if not isinstance(provenance, dict):
        raise ValueError('Snapshot report is missing provenance')
    names = ('git_sha', 'dirty', 'binary_sha256', 'gpu', 'gpu_vendor_id', 'gpu_device_id', 'driver')
    if any(name not in provenance for name in names):
        raise ValueError('Snapshot report provenance is incomplete')
    return {name: provenance[name] for name in names}


def check_child_report(run, parent, face):
    children = parent.get('auxiliary_runs')
    if not isinstance(children, list) or len(children) != 1:
        raise ValueError('Snapshot report must have exactly one child run')
    child = children[0]
    if child.get('status') != 'pass':
        raise ValueError('Snapshot child run did not pass')
    if tuple(child.get(name) for name in ('environment_fingerprint', 'workload_fingerprint',
                                          'policy_fingerprint')) != report_fingerprint(parent):
        raise ValueError('Snapshot child fingerprints do not match the parent')
    child_path = confined_path(run, child.get('report'), 'Snapshot child report')
    if not child_path.is_file() or digest(child_path) != child.get('sha256'):
        raise ValueError('Snapshot child report digest mismatch')
    child_report = json.loads(child_path.read_text())
    if child_report.get('status') != 'pass' or child_report.get('case', {}).get('id') != 'local.probe_bake.' + face:
        raise ValueError('Snapshot child report is not the requested cubemap face')


def capture_from_report(run, report, face, size):
    captures = report.get('captures')
    matching = [item for item in captures if item.get('channel') == CHANNEL] if isinstance(captures, list) else []
    if len(matching) != 1:
        raise ValueError('Snapshot report must contain one HDR post-transmission capture')
    capture = matching[0]
    # The harness report vocabulary currently serializes linear capture color space as "none".
    expected = {'checkpoint_frame': 0, 'canonical_encoding': 'RGBA16_FLOAT_LE',
                'value_kind': 'color', 'color_space': 'none', 'origin': 'top_left',
                'width': size, 'height': size, 'mip': 0, 'layer': 0}
    if any(capture.get(name) != value for name, value in expected.items()):
        raise ValueError('Unexpected HDR capture contract')
    raw = confined_path(run, capture.get('data_path'), 'HDR capture')
    metadata_path = confined_path(run, capture.get('metadata_path'), 'HDR capture metadata')
    if not raw.is_file() or not metadata_path.is_file() or digest(raw) != capture.get('data_sha256'):
        raise ValueError('HDR capture payload path/digest mismatch')
    if digest(metadata_path) != capture.get('metadata_sha256'):
        raise ValueError('HDR capture metadata digest mismatch')
    metadata = json.loads(metadata_path.read_text())
    fields = ('channel', 'capture_version', 'producer_resource', 'value_kind', 'color_space',
              'source_format', 'canonical_encoding', 'origin', 'width', 'height',
              'source_row_pitch', 'mip', 'layer', 'source_frame_index', 'submit_serial',
              'data_sha256')
    if metadata.get('schema_version') != 1 or any(metadata.get(name) != capture.get(name) for name in fields):
        raise ValueError('HDR capture metadata does not match its report row')
    # Child metadata paths are child-root relative; the parent report rebases them.
    if confined_path(metadata_path.parent.parent, metadata.get('data_path'), 'Metadata payload') != raw:
        raise ValueError('HDR metadata payload differs from its report row')
    return {'face': face, 'raw': str(raw), 'sha256': capture['data_sha256'],
            'metadata_sha256': capture['metadata_sha256']}


def check(output, sidecar):
    try:
        metadata = json.loads(sidecar.read_text())
        if (metadata.get('schema_version') != 1 or metadata.get('recipe_version') != RECIPE_VERSION or
                metadata.get('source_channel') != CHANNEL or digest(output) != metadata.get('output_sha256')):
            return False
        scene = confined_path(REPO, metadata.get('scene'), 'Bake scene')
        if not scene.is_file() or not scene.is_relative_to(REPO / 'assets' / 'scenes'):
            return False
        protect_source_assets(output, sidecar, scene, metadata.get('scene_manifest'))
        captures = metadata.get('captures')
        if not isinstance(captures, list) or [item.get('face') for item in captures] != list(FACES):
            return False
        provenance = captures[0].get('provenance') if captures else None
        return (isinstance(provenance, dict) and
                all(isinstance(item.get('sha256'), str) and isinstance(item.get('report_sha256'), str)
                    and isinstance(item.get('metadata_sha256'), str) and item.get('provenance') == provenance
                    for item in captures))
    except (OSError, TypeError, ValueError, json.JSONDecodeError):
        return False


def bake(args, output, sidecar):
    scene = confined_path(REPO, args.scene, 'Scene')
    if not scene.is_file() or not scene.is_relative_to(REPO / 'assets' / 'scenes'):
        raise ValueError('--scene must name an existing scene under assets/scenes')
    if not args.position or not all(math.isfinite(value) for value in args.position):
        raise ValueError('--position requires three finite world coordinates')
    if args.size < 1 or args.size > 2048 or args.size & (args.size - 1):
        raise ValueError('--size must be a power of two from 1 to 2048')
    if not (math.isfinite(args.near_plane) and math.isfinite(args.far_plane) and
            0 < args.near_plane < args.far_plane):
        raise ValueError('Require finite 0 < near-plane < far-plane')
    harness = executable('vkr_harness', args.harness)
    packer = executable('vkr_hdr_cube_packer', args.packer)
    job_id = uuid.uuid4().hex
    cases = REPO / 'tools' / 'cases' / 'local' / ('probe_bake_' + job_id)
    job = REPO / 'build' / '_artifacts' / 'probe_bake' / job_id
    cases.mkdir(parents=True)
    job.mkdir(parents=True)
    captures = []
    runs = []
    scene_manifest = None
    provenance = None
    environment = dict(os.environ)
    for name in ('MTL_DEBUG_LAYER', 'MTL_SHADER_VALIDATION', 'VK_INSTANCE_LAYERS'):
        environment.pop(name, None)
    try:
        for face in FACES:
            case = {
                'schema_version': 1, 'id': 'local.probe_bake.' + face, 'suite': 'local',
                'description': 'Static HDR probe face; global illumination retained, local probes disabled.',
                'scene': scene.relative_to(REPO).as_posix(), 'seed': 1,
                'resolution': [args.size, args.size], 'boot': 'full', 'target': 'offscreen',
                'present': 'none', 'target_image_count': 2, 'cache': 'isolated_cold',
                'fixed_delta': 1 / 60, 'repetitions': 1, 'repetition_timeout_ms': 300000,
                'asset_ready_timeout_ms': 240000, 'frames': {'warmup': 4, 'measure': 1},
                'renderer': {'editor': False, 'skybox': True, 'shadow_preset': 'balanced',
                             'shadow_cascades': 4, 'taa_enabled': False, 'tonemap_enabled': False,
                             'fxaa_enabled': False, 'exposure_mode': 'manual', 'manual_exposure': 1,
                             'display_transform': 'agx', 'bloom_enabled': False, 'gtao_enabled': False,
                             'image_sharpness': 0, 'ibl_probe_limit': 0, 'render_mode': 'default'},
                'camera': {'mode': 'cubemap_' + face, 'position': args.position,
                           'vertical_fov_degrees': 90, 'near_plane': args.near_plane,
                           'far_plane': args.far_plane},
                'captures': [{'at_frame': 0, 'channels': [CHANNEL]}],
                'assertions': [{'metric': 'visibility.gbuffer.resolve_invalid', 'stat': 'max', 'max': 0}],
            }
            path = cases / (face + '.case.json')
            path.write_text(json.dumps(case, indent=2) + '\n')
            shutil.copyfile(path, job / path.name)
            command = [str(harness), 'snapshot', '--case', path.relative_to(REPO).as_posix(),
                       '--profile', 'tools/profiles/local-offscreen.json']
            print(f'Capturing {face} at {args.size}x{args.size}', flush=True)
            result = subprocess.run(command, cwd=REPO, env=environment, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=330)
            (job / (face + '.log')).write_text(result.stdout)
            if result.returncode:
                raise RuntimeError(f'{face} capture failed ({result.returncode}); see {job}')
            publications = [json.loads(line) for line in result.stdout.splitlines()
                            if line.startswith('{') and '"report"' in line]
            if not publications:
                raise RuntimeError(f'{face} produced no report; see {job}')
            publication = publications[-1]
            report_path = confined_path(REPO, publication.get('report'), 'Snapshot report')
            snapshot_root = REPO / 'build' / '_artifacts' / 'snapshot'
            if not report_path.is_relative_to(snapshot_root.resolve()) or not report_path.is_file():
                raise RuntimeError('Capture report is outside the snapshot artifact tree')
            if digest(report_path) != publication.get('sha256'):
                raise RuntimeError('Capture report digest mismatch')
            report = json.loads(report_path.read_text())
            if report.get('status') != 'pass' or report.get('case', {}).get('id') != case['id']:
                raise RuntimeError(f'{face} report did not pass for the requested case')
            report_fingerprint(report)
            current_provenance = report_provenance(report)
            if provenance is not None and current_provenance != provenance:
                raise RuntimeError('Capture provenance changed during the six faces')
            provenance = current_provenance
            run = report_path.parent
            runs.append(run)
            manifest = json.loads((run / 'scene-content-manifest.json').read_text())
            valid_scene_manifest(manifest, scene)
            if scene_manifest is not None and scene_manifest != manifest:
                raise RuntimeError('Scene inputs changed during the six captures; rebake from a stable scene')
            scene_manifest = manifest
            check_child_report(run, report, face)
            capture = capture_from_report(run, report, face, args.size)
            capture['report_sha256'] = publication['sha256']
            capture['provenance'] = current_provenance
            captures.append(capture)
            shutil.copyfile(report_path, job / (face + '.report.json'))
        protect_source_assets(output, sidecar, scene, scene_manifest)
        output.parent.mkdir(parents=True, exist_ok=True)
        command = [str(packer), '--size', str(args.size), '--output', str(output)]
        for capture in captures:
            command += ['--face', capture['raw']]
        subprocess.run(command, cwd=REPO, check=True)
        metadata = {'schema_version': 1, 'recipe_version': RECIPE_VERSION,
                    'scene': scene.relative_to(REPO).as_posix(), 'position': args.position,
                    'size': args.size, 'near_plane': args.near_plane, 'far_plane': args.far_plane,
                    'lighting': 'direct + emissive + global environment; local probes disabled',
                    'source_channel': CHANNEL, 'capture_to_cube': 'flip each face vertically once',
                    'scene_manifest': scene_manifest, 'output_sha256': digest(output),
                    'captures': [{key: value for key, value in item.items() if key != 'raw'}
                                 for item in captures]}
        atomic_json(sidecar, metadata)
        atomic_json(job / 'bake.json', metadata)
        for run in runs:
            shutil.rmtree(run)
        print(json.dumps({'status': 'baked', 'output': str(output),
                          'sha256': metadata['output_sha256'], 'evidence': str(job)}))
    finally:
        shutil.rmtree(cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scene')
    parser.add_argument('--position', type=float, nargs=3)
    parser.add_argument('--size', type=int, default=256)
    parser.add_argument('--near-plane', type=float, default=.1)
    parser.add_argument('--far-plane', type=float, default=1000)
    parser.add_argument('--output', required=True)
    parser.add_argument('--harness')
    parser.add_argument('--packer')
    parser.add_argument('--check', action='store_true', help='Check the saved bake against its recorded inputs')
    args = parser.parse_args()
    try:
        output, sidecar = output_paths(args.output)
        if args.check:
            current = check(output, sidecar)
            print('current' if current else 'stale')
            return 0 if current else 1
        if not args.scene:
            parser.error('--scene is required for baking')
        bake(args, output, sidecar)
        return 0
    except (OSError, TypeError, ValueError, json.JSONDecodeError, RuntimeError,
            subprocess.SubprocessError) as error:
        print(f'Probe bake failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
