#!/usr/bin/env python3
"""Bake a scene's static diffuse transport into a portable DVOL volume."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

REPO = Path(__file__).resolve().parents[1]
VERSION = 1
DVOL_MAGIC = 0x4C4F5644
DVOL_ENDIAN = 0x01020304
DVOL_HEADER_BYTES = 112
DVOL_PROBE_BYTES = 116
DVOL_CELL_BYTES = 4


def digest(path):
    state = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            state.update(block)
    return 'sha256:' + state.hexdigest()


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                     prefix=path.name + '.', suffix='.tmp', delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write('\n')
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def atomic_copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=destination.name + '.',
                                     suffix='.tmp', delete=False) as stream:
        temporary = Path(stream.name)
        with source.open('rb') as input_stream:
            while block := input_stream.read(1024 * 1024):
                stream.write(block)
    try:
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def existing_path(value, label):
    if not value:
        raise ValueError(f'{label} is required')
    path = Path(value).resolve(strict=True)
    if not path.is_file():
        raise ValueError(f'{label} must name a regular file')
    return path


def output_paths(value):
    output = Path(value).resolve()
    if output.suffix != '.vkdv':
        raise ValueError('--output must name a .vkdv file')
    return output, Path(str(output) + '.bake.json').resolve()


def executable(override):
    if override:
        candidate = Path(override).resolve()
    else:
        suffix = '.exe' if os.name == 'nt' else ''
        candidate = REPO / 'build_release' / 'tools' / ('vkr_diffuse_baker' + suffix)
        if not candidate.is_file():
            candidate = candidate.parent / 'Release' / candidate.name
    if not candidate.is_file():
        raise ValueError('Missing vkr_diffuse_baker; build with build_release.sh/.bat first')
    return candidate


def finite(values, label):
    if not all(math.isfinite(value) for value in values):
        raise ValueError(f'{label} must be finite')


def validate_recipe(args):
    finite(args.bounds, '--bounds') if args.bounds else None
    if args.bounds and any(args.bounds[index] >= args.bounds[index + 3] for index in range(3)):
        raise ValueError('--bounds requires min < max on every axis')
    if any(value < 2 for value in args.grid) or math.prod(args.grid) > 256:
        raise ValueError('--grid dimensions must be at least 2 with at most 256 probes')
    if args.voxel_size is not None and (not math.isfinite(args.voxel_size) or args.voxel_size <= 0):
        raise ValueError('--voxel-size must be finite and positive')
    if not 1 <= args.face_size <= 32:
        raise ValueError('--face-size must be from 1 through 32')
    if not 1 <= args.samples <= 65536 or not 1 <= args.max_depth <= 64:
        raise ValueError('--samples must be 1..65536 and --max-depth must be 1..64')
    if not 0 <= args.photons <= 16_000_000:
        raise ValueError('--photons must be from 0 through 16000000')
    if not 0 <= args.seed <= 0xffffffff:
        raise ValueError('--seed must be an unsigned 32-bit value')
    if args.photon_radius is not None and (not math.isfinite(args.photon_radius) or
                                           args.photon_radius <= 0):
        raise ValueError('--photon-radius must be finite and positive')


def recipe_arguments(args, photon_radius=None):
    command = ['--scene', str(existing_path(args.scene, '--scene')), '--grid',
               *(str(value) for value in args.grid)]
    if args.bounds:
        command += ['--bounds', *(format(value, '.9g') for value in args.bounds)]
    if args.voxel_size is not None:
        command += ['--voxel-size', format(args.voxel_size, '.9g')]
    command += ['--face-size', str(args.face_size), '--samples', str(args.samples),
                '--max-depth', str(args.max_depth), '--seed', str(args.seed),
                '--photons', str(args.photons)]
    if photon_radius is not None:
        command += ['--photon-radius', format(photon_radius, '.9g')]
    return command


def relative_or_absolute_dependency(value, scene):
    if not isinstance(value, str) or not value:
        raise ValueError('Bake manifest contains an invalid dependency path')
    raw = Path(value)
    candidates = [raw] if raw.is_absolute() else [REPO / raw, scene.parent / raw]
    resolved = []
    for candidate in candidates:
        try:
            path = candidate.resolve(strict=True)
        except OSError:
            continue
        if path.is_file() and path not in resolved:
            resolved.append(path)
    if len(resolved) != 1:
        raise ValueError(f'Bake dependency is unavailable or ambiguous: {value}')
    return resolved[0]


def inspect_manifest(path, scene):
    try:
        manifest = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f'Baker did not write a valid inspect manifest: {error}') from error
    if not isinstance(manifest, dict) or manifest.get('version') != VERSION:
        raise ValueError('Unexpected diffuse-volume inspect manifest version')
    dependencies = manifest.get('dependencies')
    if not isinstance(dependencies, list) or not dependencies:
        raise ValueError('Diffuse-volume inspect manifest has no dependencies')
    required_counts = ('triangles', 'materials', 'lights', 'probes', 'valid_probes',
                       'cells', 'valid_cells')
    if any(not isinstance(manifest.get(name), int) or manifest[name] < 0 for name in required_counts):
        raise ValueError('Diffuse-volume inspect manifest has invalid bake counts')
    atmosphere = manifest.get('atmosphere')
    if (not isinstance(atmosphere, dict) or not isinstance(atmosphere.get('enabled'), bool) or
            not isinstance(atmosphere.get('model_version'), int) or
            not isinstance(atmosphere.get('params_hash'), str) or
            not isinstance(atmosphere.get('sh_deringing'), (int, float)) or
            not math.isfinite(float(atmosphere['sh_deringing'])) or
            atmosphere['sh_deringing'] < 0):
        raise ValueError('Diffuse-volume inspect manifest has invalid atmosphere provenance')
    paths = []
    for value in dependencies:
        path = relative_or_absolute_dependency(value, scene)
        if path not in paths:
            paths.append(path)
    if scene not in paths:
        raise ValueError('Diffuse-volume inspect manifest does not include its scene')
    records = [{'path': str(path), 'bytes': path.stat().st_size, 'sha256': digest(path)}
               for path in sorted(paths)]
    return manifest, records


def protect_sources(paths, *targets):
    source_set = set(paths)
    conflicts = [target for target in targets if target is not None and target.resolve() in source_set]
    if conflicts:
        raise ValueError('Output, metadata, or manifest would overwrite a bake source asset')


def distinct_targets(*targets):
    resolved = [target.resolve() for target in targets if target is not None]
    if len(resolved) != len(set(resolved)):
        raise ValueError('Output, metadata, and manifest must have distinct paths')


def manifest_spacing(manifest, args):
    candidates = (manifest.get('spacing'), manifest.get('probe_spacing'))
    for candidate in candidates:
        if isinstance(candidate, (int, float)):
            spacing = [float(candidate)] * 3
        elif isinstance(candidate, list) and len(candidate) == 3:
            spacing = [float(value) for value in candidate]
        else:
            continue
        if all(math.isfinite(value) and value > 0 for value in spacing):
            return min(spacing)
    if args.bounds:
        spacing = [(args.bounds[index + 3] - args.bounds[index]) / (args.grid[index] - 1)
                   for index in range(3)]
        if all(value > 0 and math.isfinite(value) for value in spacing):
            return min(spacing)
    raise ValueError('Inspect manifest lacks probe spacing for the default photon radius')


def verify_dvol(path):
    data = path.read_bytes()
    if len(data) < DVOL_HEADER_BYTES:
        raise ValueError('DVOL output is shorter than its v1 header')
    u32 = lambda offset: struct.unpack_from('<I', data, offset)[0]
    u64 = lambda offset: struct.unpack_from('<Q', data, offset)[0]
    if (u32(0) != DVOL_MAGIC or u32(4) != VERSION or u32(8) != DVOL_ENDIAN or
            u32(12) != DVOL_HEADER_BYTES or u64(16) != len(data) or
            any(u32(offset) != 0 for offset in (44, 104, 108))):
        raise ValueError('DVOL output has an invalid v1 header')
    dimensions = (u32(24), u32(28), u32(32))
    if any(value < 2 or value > 256 for value in dimensions):
        raise ValueError('DVOL output has invalid dimensions')
    probe_count = math.prod(dimensions)
    cell_count = math.prod(value - 1 for value in dimensions)
    cell_offset = DVOL_HEADER_BYTES + probe_count * DVOL_PROBE_BYTES
    if (probe_count > 256 or u32(36) != probe_count or u32(40) != cell_count or
            u64(72) != DVOL_HEADER_BYTES or u64(80) != cell_offset or
            u32(88) != DVOL_PROBE_BYTES or u32(92) != DVOL_CELL_BYTES or
            len(data) != cell_offset + cell_count * DVOL_CELL_BYTES):
        raise ValueError('DVOL output layout does not match its header')
    origin = struct.unpack_from('<3f', data, 48)
    spacing = struct.unpack_from('<3f', data, 60)
    if not all(math.isfinite(value) for value in origin) or not all(value > 0 and math.isfinite(value)
                                                                    for value in spacing):
        raise ValueError('DVOL output has invalid volume coordinates')
    header = bytearray(data[:DVOL_HEADER_BYTES])
    header[100:104] = b'\0\0\0\0'
    if (u32(96) != (zlib.crc32(data[DVOL_HEADER_BYTES:]) & 0xffffffff) or
            u32(100) != (zlib.crc32(header) & 0xffffffff)):
        raise ValueError('DVOL output checksum mismatch')
    return {'format': 'DVOL', 'version': VERSION, 'bytes': len(data), 'sha256': digest(path),
            'dimensions': list(dimensions), 'origin': list(origin), 'spacing': list(spacing)}


def run_baker(command, log):
    result = subprocess.run(command, cwd=REPO, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    log.write_text(result.stdout, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(f'vkr_diffuse_baker failed ({result.returncode}); see {log.parent}')


def run_inspect(baker, args, job, name):
    manifest = job / (name + '.manifest.json')
    command = [str(baker), *recipe_arguments(args), '--inspect', '--manifest', str(manifest)]
    (job / (name + '.command.json')).write_text(json.dumps(command, indent=2) + '\n', encoding='utf-8')
    run_baker(command, job / (name + '.log'))
    return manifest


def output_metadata(scene, baker, args, photon_radius, dependencies, dvol, manifest):
    return {
        'version': VERSION,
        'scene': {'source': args.scene, 'canonical': str(scene)},
        'tool_sha256': digest(baker),
        'wrapper_sha256': digest(Path(__file__).resolve()),
        'recipe': {
            'bounds': args.bounds,
            'grid': args.grid,
            'voxel_size': args.voxel_size,
            'face_size': args.face_size,
            'samples': args.samples,
            'max_depth': args.max_depth,
            'seed': args.seed,
            'photons': args.photons,
            'photon_radius': photon_radius,
        },
        'dependencies': dependencies,
        'baker_manifest': manifest,
        'atmosphere': manifest['atmosphere'],
        'output': dvol,
        'output_sha256': dvol['sha256'],
    }


def same_closure(before, after):
    return before == after


def legacy_scene_enables_atmosphere(metadata, paths):
    # Dependency records are sorted by path, so use the recorded scene identity.
    # Old sidecars cannot establish transport for an enabled atmosphere.
    try:
        scene = Path(metadata['scene']['canonical']).resolve(strict=True)
        if scene not in paths:
            return True
        root = json.loads(scene.read_text(encoding='utf-8'))
        atmosphere = root.get('atmosphere')
        return isinstance(atmosphere, dict) and atmosphere.get('enabled', True) is not False
    except (KeyError, OSError, TypeError, json.JSONDecodeError):
        return True


def valid_atmosphere_provenance(atmosphere):
    return (isinstance(atmosphere, dict) and isinstance(atmosphere.get('enabled'), bool) and
            isinstance(atmosphere.get('model_version'), int) and
            isinstance(atmosphere.get('params_hash'), str) and
            isinstance(atmosphere.get('sh_deringing'), (int, float)) and
            math.isfinite(float(atmosphere['sh_deringing'])) and atmosphere['sh_deringing'] >= 0)


def check(output, sidecar):
    try:
        metadata = json.loads(sidecar.read_text(encoding='utf-8'))
        if not isinstance(metadata, dict) or metadata.get('version') != VERSION:
            return False
        dvol = verify_dvol(output)
        if dvol['sha256'] != metadata.get('output_sha256'):
            return False
        dependencies = metadata.get('dependencies')
        if not isinstance(dependencies, list) or not dependencies:
            return False
        atmosphere = metadata.get('atmosphere')
        paths = []
        for record in dependencies:
            if not isinstance(record, dict) or not isinstance(record.get('path'), str):
                return False
            path = Path(record['path']).resolve(strict=True)
            if not path.is_file() or path.stat().st_size != record.get('bytes') or digest(path) != record.get('sha256'):
                return False
            paths.append(path)
        if atmosphere is None:
            if legacy_scene_enables_atmosphere(metadata, paths):
                return False
        elif not valid_atmosphere_provenance(atmosphere):
            return False
        protect_sources(paths, output, sidecar)
        return True
    except (OSError, TypeError, ValueError, json.JSONDecodeError, struct.error):
        return False


def bake(args, output, sidecar, manifest_destination):
    validate_recipe(args)
    scene = existing_path(args.scene, '--scene')
    baker = executable(args.baker)
    job = REPO / 'build' / '_artifacts' / 'diffuse_volume_bake' / uuid.uuid4().hex
    job.mkdir(parents=True)
    inspect_path = run_inspect(baker, args, job, 'inspect')
    inspect, dependencies = inspect_manifest(inspect_path, scene)
    distinct_targets(output, sidecar, manifest_destination)
    protect_sources([Path(record['path']) for record in dependencies], output, sidecar, manifest_destination)
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(dir=output.parent, prefix=output.name + '.', suffix='.vkdv')
    os.close(descriptor)
    temporary = Path(temporary_name)
    temporary.unlink()
    bake_manifest = job / 'bake.manifest.json'
    command = [str(baker), *recipe_arguments(args, args.photon_radius), '--output', str(temporary),
               '--manifest', str(bake_manifest)]
    (job / 'bake.command.json').write_text(json.dumps(command, indent=2) + '\n', encoding='utf-8')
    try:
        run_baker(command, job / 'bake.log')
        baked_manifest, after_dependencies = inspect_manifest(bake_manifest, scene)
        if not same_closure(dependencies, after_dependencies):
            raise RuntimeError('Bake source closure changed while the volume was prepared')
        dvol = verify_dvol(temporary)
        final_inspect_path = run_inspect(baker, args, job, 'inspect_after')
        _, final_dependencies = inspect_manifest(final_inspect_path, scene)
        if not same_closure(dependencies, final_dependencies):
            raise RuntimeError('Bake source closure changed before publication')
        photon_radius = args.photon_radius or manifest_spacing(baked_manifest, args) * 0.25
        metadata = output_metadata(scene, baker, args, photon_radius, dependencies, dvol, baked_manifest)
        os.replace(temporary, output)
        atomic_json(sidecar, metadata)
        atomic_copy(bake_manifest, manifest_destination)
        atomic_json(job / 'bake.json', metadata)
    finally:
        temporary.unlink(missing_ok=True)
    print(json.dumps({'status': 'baked', 'output': str(output), 'sha256': dvol['sha256'],
                      'evidence': str(job)}))


def inspect(args, manifest_destination):
    validate_recipe(args)
    scene = existing_path(args.scene, '--scene')
    baker = executable(args.baker)
    job = REPO / 'build' / '_artifacts' / 'diffuse_volume_bake' / uuid.uuid4().hex
    job.mkdir(parents=True)
    temporary_manifest = run_inspect(baker, args, job, 'inspect')
    manifest, dependencies = inspect_manifest(temporary_manifest, scene)
    protect_sources([Path(record['path']) for record in dependencies], manifest_destination)
    atomic_copy(temporary_manifest, manifest_destination)
    atomic_json(job / 'inspect.json', {'version': VERSION, 'manifest': manifest,
                                       'dependencies': dependencies, 'tool_sha256': digest(baker)})
    print(json.dumps({'status': 'inspected', 'manifest': str(manifest_destination),
                      'dependencies': len(dependencies), 'evidence': str(job)}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scene')
    parser.add_argument('--output')
    parser.add_argument('--manifest')
    parser.add_argument('--baker')
    parser.add_argument('--inspect', action='store_true', help='Only prepare and publish the room dependency manifest')
    parser.add_argument('--check', action='store_true', help='Validate a saved volume and its recorded inputs')
    parser.add_argument('--bounds', type=float, nargs=6)
    parser.add_argument('--grid', type=int, nargs=3, default=[4, 4, 4])
    parser.add_argument('--voxel-size', type=float)
    parser.add_argument('--face-size', type=int, default=16)
    parser.add_argument('--samples', type=int, default=64)
    parser.add_argument('--max-depth', type=int, default=12)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--photons', type=int, default=1_000_000)
    parser.add_argument('--photon-radius', type=float)
    args = parser.parse_args()
    try:
        if args.check:
            if not args.output:
                parser.error('--output is required with --check')
            output, sidecar = output_paths(args.output)
            current = check(output, sidecar)
            print('current' if current else 'stale')
            return 0 if current else 1
        if not args.scene:
            parser.error('--scene is required')
        if args.inspect:
            if args.output:
                parser.error('--inspect does not write --output')
            if not args.manifest:
                parser.error('--manifest is required with --inspect')
            inspect(args, Path(args.manifest).resolve())
            return 0
        if not args.output:
            parser.error('--output is required')
        output, sidecar = output_paths(args.output)
        manifest = Path(args.manifest).resolve() if args.manifest else Path(str(output) + '.manifest.json').resolve()
        bake(args, output, sidecar, manifest)
        return 0
    except (OSError, TypeError, ValueError, RuntimeError, json.JSONDecodeError, struct.error,
            subprocess.SubprocessError) as error:
        print(f'Diffuse-volume bake failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
