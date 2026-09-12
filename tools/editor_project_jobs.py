#!/usr/bin/env python3
"""Prepare portable editor scene transactions and machine-local runtime views.

CLI: --request request.json --result result.json. Request/result version is 1.
Managed documents use v3 scene fields from docs/proposals/editor-projects.md;
artifacts have {role,path,version}, asset references have {scope,id,role}.
Only the C project store publishes project membership. A failed/cancelled job
removes its own staging tree and never edits the previous scene or project.
"""

import argparse
import base64
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import uuid
from urllib.parse import unquote, urlsplit, parse_qs

VERSION = 1
MAX_JSON_BYTES = 16 * 1024 * 1024
MAX_MANAGED_DOCUMENT_BYTES = 1024 * 1024
MAX_IMPORT_BYTES = 8 * 1024 * 1024 * 1024
MAX_IMPORT_FILES = 16384
FACES = ('r', 'l', 'u', 'd', 'f', 'b')


class JobError(Exception):
    pass


class Cancelled(JobError):
    pass


def load_json(path, limit=MAX_JSON_BYTES):
    path = Path(path)
    if not path.is_file() or path.stat().st_size > limit:
        raise JobError(f'Missing or oversized JSON: {path.name}')
    try:
        def duplicate_free(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise JobError(f'Duplicate JSON member: {key}')
                result[key] = value
            return result
        def reject_constant(value):
            raise JobError(f'Non-finite JSON number: {value}')
        value = json.loads(path.read_text(encoding='utf-8'),
                           object_pairs_hook=duplicate_free,
                           parse_constant=reject_constant)
    except (ValueError, UnicodeError) as error:
        raise JobError(f'Invalid JSON {path.name}: {error}') from error
    if not isinstance(value, dict):
        raise JobError(f'Expected a JSON object: {path.name}')
    return value


def validate_managed_document(value):
    """Match the project store's durable document limits before publication."""
    encoded = (json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + '\n').encode('utf-8')
    if len(encoded) > MAX_MANAGED_DOCUMENT_BYTES:
        raise JobError('Managed document exceeds the 1 MiB project-store limit; split the scene into smaller scenes')
    def visit(item, depth):
        if isinstance(item, (dict, list)):
            depth += 1
            if depth > 32:
                raise JobError('Managed document exceeds the project-store nesting limit of 32')
        if isinstance(item, dict):
            for key, child in item.items():
                if len(key.encode('utf-8')) > 255:
                    raise JobError('Managed document key exceeds the project-store limit of 255 UTF-8 bytes')
                visit(child, depth)
        elif isinstance(item, list):
            for child in item:
                visit(child, depth)
    visit(value, 0)


def atomic_json(path, value):
    path = Path(path)
    if path.name == 'scene.json' and value.get('version') == 3:
        validate_managed_document(value)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=path.name + '.', suffix='.tmp',
                                      dir=path.parent)
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8', newline='\n') as output:
            json.dump(value, output, indent=2, ensure_ascii=False, allow_nan=False)
            output.write('\n')
            output.flush()
            os.fsync(output.fileno())
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def source_fingerprint(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{value:016x}'


def identifier(value):
    if not isinstance(value, str):
        raise JobError('Missing scene identifier')
    try:
        parsed = str(uuid.UUID(value))
    except ValueError as error:
        raise JobError('Invalid scene identifier') from error
    if parsed != value:
        raise JobError('Scene identifier must be a canonical UUID')
    return value


def contained(root, value, must_exist=True):
    root = Path(root).resolve()
    if not isinstance(value, str) or not value or '\\' in value or ':' in value or '\x00' in value:
        raise JobError('Invalid managed path')
    relative = Path(value)
    if relative.is_absolute() or any(part in ('..', '') for part in relative.parts):
        raise JobError(f'Managed path escapes its owner: {value}')
    try:
        result = (root / relative).resolve(strict=must_exist)
    except OSError as error:
        raise JobError(f'Missing managed asset: {value}') from error
    if not result.is_relative_to(root):
        raise JobError(f'Managed path escapes its owner: {value}')
    return result


def source_file(value):
    try:
        path = Path(value).expanduser().resolve(strict=True)
    except (TypeError, ValueError, OSError) as error:
        raise JobError(f'Source file is unavailable: {value}') from error
    if not path.is_file():
        raise JobError(f'Source is not a regular file: {path}')
    return path


def legacy_source(value, origin, legacy_root):
    if not isinstance(value, str) or not value or '\x00' in value:
        raise JobError('Missing legacy asset path')
    path = Path(value.split('?', 1)[0])
    candidates = [path] if path.is_absolute() else [origin / path, legacy_root / path]
    if path.suffix.lower() in ('.png', '.jpg', '.jpeg', '.bmp', '.tga', '.hdr'):
        candidates += [Path(str(candidate) + '.vkt') for candidate in list(candidates) if not candidate.is_file()]
    matches = set()
    for candidate in candidates:
        try:
            resolved = candidate.resolve(strict=True)
            if resolved.is_file():
                matches.add(resolved)
        except OSError:
            pass
    if len(matches) != 1:
        raise JobError(f'Legacy dependency is missing or ambiguous: {value}')
    return matches.pop()


def relative_reference(path, owner):
    return './' + Path(path).relative_to(owner).as_posix()


class Job:
    def __init__(self, request, result_path):
        if request.get('version') != VERSION:
            raise JobError('Unsupported project job request version')
        self.request = request
        self.result_path = Path(result_path).resolve()
        self.progress_path = Path(str(self.result_path) + '.progress.json')
        self.workspace = Path(request['workspace_root']).resolve(strict=True)
        self.project_path = Path(request['project_path']).resolve()
        self.project_root = self.project_path.parent
        self.read_only = request.get('read_only', False)
        if not isinstance(self.read_only, bool):
            raise JobError('read_only must be boolean')
        self.runtime_directory = None
        if self.read_only:
            if request.get('operation') != 'prepare_scene':
                raise JobError('Read-only workspace allows opening prepared scenes only')
            raw_directory = request.get('runtime_directory')
            if not isinstance(raw_directory, str) or not Path(raw_directory).is_absolute():
                raise JobError('Read-only opening requires an absolute local runtime_directory')
            self.runtime_directory = Path(raw_directory).resolve()
            if self.runtime_directory.is_relative_to(self.workspace) or self.result_path.is_relative_to(self.workspace):
                raise JobError('Read-only runtime and result files must be outside the workspace')
        if self.project_path.name != 'project.json' or self.project_root.parent != self.workspace / 'projects':
            raise JobError('Project must belong directly to the chosen workspace projects directory')
        identifier(self.project_root.name)
        self.scene_id = identifier(request['scene_id']) if request.get('scene_id') else None
        if self.scene_id is None and request.get('operation') != 'create_project':
            raise JobError('Scene operation requires a scene identifier')
        self.final = self.project_root / 'scenes' / (self.scene_id or 'unused')
        self.legacy_root = Path(request.get('legacy_root') or Path(__file__).resolve().parents[1]).resolve()
        self.stage = None
        self.child = None
        self.spawning = False
        self.bytes_copied = 0
        self.files_copied = 0
        self.warnings = []
        self.assets = []
        self.cancelled = False
        self.final_owned = False
        self.published_builds = []
        self.tools = request.get('tools', {})
        self.sources = {}
        self.pending_project_assets = None
        self.pending_default_font = None
        self.project_builds = []
        self.inspections = {}
        self.texture_seeds = {}
        self.texture_tool_hash = None
        self.asset_display_names = {}
        self.source_display_names = {}

    def ensure_bootstrap(self):
        bundle = self.workspace / 'editor' / 'bundles' / '1'
        manifest_path = bundle / 'manifest.json'
        if manifest_path.is_file():
            manifest = load_json(manifest_path)
            if manifest.get('version') != 1 or not isinstance(manifest.get('files'), list):
                raise JobError('Editor bundle manifest is invalid; repair the installed editor bundle')
            for record in manifest['files']:
                path = contained(bundle, record['path'])
                if not path.is_file() or digest(path) != record.get('sha256'):
                    raise JobError('Editor bootstrap content is missing or changed; repair the installed editor bundle')
            return
        if self.read_only:
            raise JobError('Editor bundle is unavailable; open this workspace with write access to prepare it first')
        source = self.request.get('bootstrap_directory')
        if not source:
            # Library callers may validate CPU-only documents without launching
            # the editor. Installed editor requests always provide this root.
            return
        source = Path(source).resolve(strict=True)
        fonts = source / 'fonts'
        if not (fonts / 'UbuntuMono-cooked.fontcfg').is_file():
            raise JobError('Installed editor bootstrap has no default cooked font')
        parent = bundle.parent
        parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix='.bootstrap-', dir=parent))
        try:
            self.progress('Preparing editor resources', 0.05)
            files = []
            for path in sorted(fonts.rglob('*')):
                if path.is_symlink():
                    raise JobError('Installed editor bundle contains a symlink')
                if path.is_file():
                    destination = self.copy_file(path, staging / path.relative_to(source))
                    files.append({'path': destination.relative_to(staging).as_posix(), 'sha256': digest(destination)})
            assets = []
            for config in sorted((staging / 'fonts').glob('*.fontcfg')):
                config_type = next((line.partition('=')[2].strip() for line in config.read_text(encoding='utf-8').splitlines()
                                    if line.partition('=')[0].strip() == 'type'), '')
                if config_type != 'cooked_mtsdf':
                    continue
                self.validate_bundle_dependencies(staging, config, set())
                asset_id = 'default-scene-font' if config.name == 'UbuntuMono-cooked.fontcfg' else config.stem
                assets.append({'id': asset_id, 'kind': 'font', 'name': config.stem,
                    'artifacts': [{'role': 'font', 'path': config.relative_to(staging).as_posix(), 'version': 1}],
                    'fingerprint': 'sha256:' + digest(config)})
            atomic_json(staging / 'manifest.json', {'version': 1, 'id': 'vkr-editor-bundle-1',
                                                  'assets': assets, 'files': files})
            if bundle.exists():
                raise JobError('An incomplete editor bundle exists; repair it before opening the workspace')
            os.rename(staging, bundle)
        finally:
            if staging.exists():
                shutil.rmtree(staging)

    def prepare_project_font(self):
        source = self.request.get('project_font_source')
        if not source:
            return
        staging_parent = self.project_root / '.staging'
        staging_parent.mkdir(parents=True, exist_ok=True)
        temporary = Path(tempfile.mkdtemp(prefix='project-font-', dir=staging_parent))
        old_stage, old_assets = self.stage, self.assets
        old_bakes = self.request.get('bakes')
        self.stage, self.assets = temporary, []
        self.request['bakes'] = {**(old_bakes or {}), 'prepare_assets': True}
        try:
            reference = self.import_font(source)
            records = self.assets
            for record in records:
                closure = set()
                for product in record['artifacts']:
                    self.validate_bundle_dependencies(temporary, contained(temporary, product['path']), closure)
                record['closure'] = {path.relative_to(temporary).as_posix(): digest(path) for path in closure}
            builds = self.project_root / 'builds'
            builds.mkdir(exist_ok=True)
            for directory in (temporary / 'builds').iterdir():
                destination = builds / directory.name
                if destination.exists():
                    raise JobError('Project font build revision collision')
                os.rename(directory, destination)
                self.project_builds.append(destination)
            project = self.project_document()
            self.pending_project_assets = [*project.get('assets', []), *records]
            self.pending_default_font = {**reference, 'scope': 'project'}
            validate_managed_document(self.project_document())
        finally:
            self.stage, self.assets = old_stage, old_assets
            if old_bakes is None:
                self.request.pop('bakes', None)
            else:
                self.request['bakes'] = old_bakes
            shutil.rmtree(temporary)

    def progress(self, stage, fraction, detail=''):
        if self.cancelled:
            raise Cancelled('Project preparation cancelled')
        atomic_json(self.progress_path, {'version': VERSION, 'status': 'running',
                    'stage': stage, 'progress': fraction, 'detail': detail})
        print(f'{stage}: {detail}', flush=True)

    def cancel(self, *_):
        if self.cancelled:
            return
        self.cancelled = True
        if self.spawning:
            return
        raise Cancelled('Project preparation cancelled')

    def run_tool(self, tool, arguments, label):
        detail = tool
        for flag in ('--input', '--layer', '--config', '--scene'):
            if flag in arguments and arguments.index(flag) + 1 < len(arguments):
                detail = Path(str(arguments[arguments.index(flag) + 1])).name
                break
        self.progress(label, None, detail)
        executable = self.tools.get(tool)
        if not executable:
            raise JobError(f'{label} requires the installed {tool} tool')
        executable = source_file(executable)
        command = [str(executable), *map(str, arguments)]
        try:
            self.spawning = True
            self.child = subprocess.Popen(command, start_new_session=os.name != 'nt',
                                          creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0)
            self.spawning = False
            if self.cancelled:
                raise Cancelled('Project preparation cancelled')
            code = self.child.wait()
        finally:
            self.spawning = False
            if self.child and self.child.poll() is None:
                if os.name == 'nt':
                    subprocess.run(['taskkill', '/PID', str(self.child.pid), '/T', '/F'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
                else:
                    try:
                        os.killpg(self.child.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                try:
                    self.child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    if os.name != 'nt':
                        os.killpg(self.child.pid, signal.SIGKILL)
                    else:
                        self.child.kill()
                    self.child.wait()
            if self.child and self.cancelled and os.name != 'nt':
                try:
                    os.killpg(self.child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            self.child = None
        if code:
            raise JobError(f'{label} failed (exit {code}); see the job log')

    def copy_file(self, source, destination):
        source = source_file(source)
        size = source.stat().st_size
        if self.files_copied >= MAX_IMPORT_FILES or self.bytes_copied + size > MAX_IMPORT_BYTES:
            raise JobError('Import exceeds 16,384 files or 8 GiB; split the import')
        before = digest(source)
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        if digest(destination) != before or digest(source) != before:
            destination.unlink(missing_ok=True)
            raise JobError(f'Source changed while copying: {source.name}')
        self.files_copied += 1
        self.bytes_copied += size
        return destination

    def copy_blob(self, source, directory):
        source = source_file(source)
        extension = source.suffix.lower()
        if len(extension) > 16 or any(not (char.isascii() and (char.isalnum() or char == '.')) for char in extension):
            raise JobError(f'Unsupported asset filename: {source.name}')
        source_hash = digest(source)
        self.source_display_names.setdefault(source_hash, source.stem)
        candidate = Path(str(source) + '.vkt')
        if extension != '.vkt' and candidate.is_file():
            self.texture_seeds[source_hash] = candidate
        destination = Path(directory) / (source_hash + extension)
        if not destination.exists():
            self.copy_file(source, destination)
        return destination

    def artifact(self, kind, name, path, role=None, import_id=None, source=None,
                 **metadata):
        asset_id = str(uuid.uuid4())
        asset = {'id': asset_id, 'kind': kind, 'name': name,
                 'import_id': import_id, 'source': str(source.relative_to(self.stage)) if source else None,
                 'artifacts': [{'role': role or kind, 'path': path.relative_to(self.stage).as_posix(),
                                'version': 1}], 'fingerprint': 'sha256:' + digest(path), **metadata}
        self.assets.append(asset)
        return {'scope': 'scene', 'id': asset_id, 'role': role or kind}

    def snapshot_model(self, source, import_id):
        """Rewrite only copied inputs. Resolve every dependency at its source owner."""
        source = source_file(source)
        if source.stat().st_size > 256 * 1024 * 1024:
            raise JobError('Model source exceeds the 256 MiB parser budget')
        original_digest = digest(source)
        directory = self.stage / 'sources' / import_id
        directory.mkdir(parents=True)
        destination = directory / ('source' + source.suffix.lower())
        dependencies = []
        material_names = []

        def dependency(value, origin):
            uri = urlsplit(value)
            if uri.scheme and not (len(uri.scheme) == 1 and value[1:2] == ':'):
                raise JobError('Remote dependencies are unsupported; download them before importing')
            resolved = source_file(origin / unquote(value))
            copied = self.copy_blob(resolved, directory / 'dependencies')
            dependencies.append({'path': copied.relative_to(directory).as_posix(),
                                 'sha256': digest(copied), 'bytes': copied.stat().st_size})
            return copied

        if source.suffix.lower() in ('.gltf', '.glb'):
            binary = None
            if source.suffix.lower() == '.glb':
                if source.stat().st_size > MAX_IMPORT_BYTES:
                    raise JobError('GLB exceeds import size limit')
                data = source.read_bytes()
                if len(data) < 20 or struct.unpack_from('<III', data) != (0x46546C67, 2, len(data)):
                    raise JobError('Invalid GLB header')
                offset = 12
                chunks = []
                while offset < len(data):
                    if offset + 8 > len(data):
                        raise JobError('Truncated GLB chunk')
                    length, kind = struct.unpack_from('<II', data, offset)
                    offset += 8
                    if length > len(data) - offset:
                        raise JobError('Truncated GLB payload')
                    chunks.append((kind, data[offset:offset + length]))
                    offset += length
                if not chunks or chunks[0][0] != 0x4E4F534A:
                    raise JobError('GLB has no JSON chunk')
                model = json.loads(chunks[0][1])
                binary = chunks[1:]
            else:
                model = load_json(source, 256 * 1024 * 1024)
            material_names = [item.get('name') or f'Material {index + 1}' for index, item in enumerate(model.get('materials', []))]
            for field in ('buffers', 'images'):
                for item in model.get(field, []):
                    uri = item.get('uri')
                    if uri and not uri.startswith('data:'):
                        copied = dependency(uri, source.parent)
                        item['uri'] = copied.relative_to(directory).as_posix()
            if binary is None:
                atomic_json(destination, model)
            else:
                encoded = json.dumps(model, separators=(',', ':')).encode()
                encoded += b' ' * (-len(encoded) % 4)
                chunks = [(0x4E4F534A, encoded), *binary]
                total = 12 + sum(8 + len(chunk) for _, chunk in chunks)
                with destination.open('wb') as output:
                    output.write(struct.pack('<III', 0x46546C67, 2, total))
                    for kind, chunk in chunks:
                        output.write(struct.pack('<II', len(chunk), kind))
                        output.write(chunk)
        elif source.suffix.lower() == '.obj':
            lines = []
            for line in source.read_text(encoding='utf-8-sig').splitlines():
                tokens = shlex.split(line, comments=True, posix=True)
                if not tokens or tokens[0] != 'mtllib':
                    lines.append(line)
                    continue
                if len(tokens) < 2:
                    raise JobError('OBJ mtllib has no filename')
                for library in tokens[1:]:
                    mtl_source = source_file(source.parent / library)
                    mtl = directory / ('material_' + digest(mtl_source) + '.mtl')
                    rewritten = []
                    for material_line in mtl_source.read_text(encoding='utf-8-sig').splitlines():
                        fields = shlex.split(material_line, comments=True, posix=True)
                        if fields and fields[0] == 'newmtl':
                            material_names.append(' '.join(fields[1:]))
                        if fields and (fields[0].startswith('map_') or fields[0] in ('bump', 'disp', 'decal', 'norm')):
                            if len(fields) != 2:
                                raise JobError(f'MTL texture options need explicit conversion: {fields[0]}')
                            copied = dependency(fields[1], mtl_source.parent)
                            if fields[0] not in ('map_Kd', 'map_Ks', 'bump', 'map_bump'):
                                self.warnings.append(f'OBJ channel {fields[0]} is retained in the source snapshot but is not rendered')
                            material_line = fields[0] + ' ' + copied.relative_to(directory).as_posix()
                        rewritten.append(material_line)
                    mtl.write_text('\n'.join(rewritten) + '\n', encoding='utf-8')
                    dependencies.append({'path': mtl.name, 'sha256': digest(mtl), 'bytes': mtl.stat().st_size})
                    lines.append('mtllib ' + mtl.name)
            destination.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        else:
            raise JobError('Models must be GLTF, GLB or OBJ')
        if digest(source) != original_digest:
            raise JobError('Model source changed while collecting dependencies; retry import')
        dependencies.append({'path': destination.name, 'sha256': digest(destination),
                             'bytes': destination.stat().st_size})
        self.sources[import_id] = {'version': 1, 'id': import_id,
            'source': destination.relative_to(self.stage).as_posix(),
            'original_sha256': original_digest, 'material_names': material_names,
            'dependencies': dependencies, 'reimport_status': 'source_snapshot',
            'unsupported_features': list(self.warnings)}
        return destination

    def import_model(self, source):
        import_id = str(uuid.uuid4())
        snapshot = self.snapshot_model(source, import_id)
        bundle = self.stage / 'builds' / import_id
        bundle.mkdir(parents=True)
        mesh = bundle / 'mesh.vkb'
        if self.request.get('bakes', {}).get('prepare_assets') is False:
            asset_id = str(uuid.uuid4())
            self.assets.append({'id': asset_id, 'kind': 'mesh', 'name': Path(source).stem,
                'import_id': import_id, 'source': snapshot.relative_to(self.stage).as_posix(),
                'artifacts': [], 'recipe': {'tool': 'mesh', 'version': 1},
                'fingerprint': 'sha256:' + digest(snapshot)})
            atomic_json(self.stage / 'imports' / (import_id + '.json'), self.sources[import_id])
            return {'scope': 'scene', 'id': asset_id, 'role': 'mesh'}
        self.run_tool('mesh', ['--input', snapshot, '--output', mesh,
                              '--bundle-root', bundle, '--import-id', import_id], 'Cooking model')
        reference = self.artifact('mesh', Path(source).stem, mesh, import_id=import_id, source=snapshot)
        self.index_bundle(bundle, import_id)
        manifest = self.sources[import_id]
        manifest['artifacts'] = [record for record in self.assets if record.get('import_id') == import_id]
        atomic_json(self.stage / 'imports' / (import_id + '.json'), manifest)
        return reference

    def pack_material_textures(self, material):
        lines = material.read_text(encoding='utf-8').splitlines()
        changed = False
        for index, line in enumerate(lines):
            key, separator, value = line.partition('=')
            key, value = key.strip(), value.strip()
            if not separator or not key.endswith('_texture') or not value:
                continue
            raw, marker, query = value.partition('?')
            source = (material.parent / raw).resolve(strict=True)
            if source.suffix.lower() == '.vkt':
                continue
            parameters = parse_qs(query)
            texture_class = parameters.get('tc', parameters.get('class', [None]))[0]
            if texture_class is None:
                texture_class = 'normal_rg' if 'normal' in key else ('color_srgb' if key in ('diffuse_texture', 'base_color_texture', 'emissive_texture') or parameters.get('cs') == ['srgb'] else 'data_mask')
            texture_class = texture_class.replace('_', '-')
            if texture_class not in ('normal-rg', 'data-mask', 'color-srgb', 'color-linear'):
                raise JobError('Unsupported material texture class')
            source_hash = digest(source)
            destination = source.parent / (source_hash + '-' + texture_class + '.vkt')
            if not destination.is_file():
                if self.texture_tool_hash is None:
                    self.texture_tool_hash = digest(source_file(self.tools.get('texture')))
                recipe = {'version': 1, 'source': source_hash, 'class': texture_class,
                          'tool': self.texture_tool_hash, 'shape': '2d', 'strict': True}
                cache_key = hashlib.sha256(json.dumps(recipe, sort_keys=True).encode()).hexdigest()
                cache = self.workspace / 'cache' / 'textures' / cache_key
                cache.mkdir(parents=True, exist_ok=True)
                cached = cache / 'texture.vkt'
                manifest = cache / 'manifest.json'
                valid = False
                if cached.is_file() and manifest.is_file():
                    recorded = load_json(manifest)
                    valid = recorded.get('recipe') == recipe and recorded.get('sha256') == digest(cached)
                if not valid:
                    temporary = cache / (str(uuid.uuid4()) + '.vkt')
                    try:
                        seed = self.texture_seeds.get(source_hash)
                        if seed and seed.is_file():
                            self.copy_file(seed, temporary)
                        # The packer independently validates a seeded cache's source
                        # hash, class and full recipe; mismatch forces a fresh cook.
                        self.run_tool('texture', ['--output', temporary, '--type', '2d', '--layer', source,
                            '--texture-class', texture_class, '--strict', '--no-progress'], 'Preparing texture')
                        if not temporary.is_file():
                            raise JobError('Texture packer did not publish its artifact')
                        os.replace(temporary, cached)
                        atomic_json(manifest, {'version': 1, 'recipe': recipe, 'sha256': digest(cached)})
                    finally:
                        temporary.unlink(missing_ok=True)
                self.copy_file(cached, destination)
            material_name = self.asset_display_names.get(material, material.stem)
            display_name = self.source_display_names.get(source_hash, material_name + ' ' + key.removesuffix('_texture'))
            self.asset_display_names[destination] = display_name
            self.asset_display_names.setdefault(source, display_name)
            relative = os.path.relpath(destination, material.parent).replace(os.sep, '/')
            lines[index] = key + '=./' + relative + ('?' + query if marker else '')
            changed = True
        if changed:
            material.write_text('\n'.join(lines) + '\n', encoding='utf-8')

    def bundle_needs_textures(self, bundle):
        for material in (bundle / 'materials').glob('*.mt'):
            for line in material.read_text(encoding='utf-8').splitlines():
                key, separator, value = line.partition('=')
                if separator and key.strip().endswith('_texture') and value.strip():
                    if Path(value.strip().split('?', 1)[0]).suffix.lower() != '.vkt':
                        return True
        return False

    def texture_repair_bundles(self, scene, root):
        bundles = set()
        for record in scene.get('assets', []):
            for artifact in record.get('artifacts', []):
                parts = Path(artifact['path']).parts
                if len(parts) >= 3 and parts[0] == 'builds':
                    bundle = contained(root, '/'.join(parts[:2]))
                    if self.bundle_needs_textures(bundle):
                        bundles.add(bundle)
        return bundles

    def index_bundle(self, bundle, import_id):
        source_manifest = self.sources.get(import_id)
        manifest_path = self.final / 'imports' / (import_id + '.json')
        if source_manifest is None and manifest_path.is_file():
            source_manifest = load_json(manifest_path)
        names = (source_manifest or {}).get('material_names', [])
        for material in sorted((bundle / 'materials').glob('*.mt')):
            index = material.stem.rsplit('_', 1)[-1]
            if index.isdigit() and int(index) < len(names):
                self.asset_display_names.setdefault(material, names[int(index)])
            self.pack_material_textures(material)
            self.artifact('material', self.asset_display_names.get(material, material.stem), material, import_id=import_id,
                          source_key='material:' + index)
        for texture in sorted((bundle / 'textures').glob('*.*')):
            display_name = self.asset_display_names.get(texture) or self.source_display_names.get(digest(texture), texture.stem)
            self.artifact('texture', display_name, texture, import_id=import_id)

    def import_material(self, source, bundle, import_id, number):
        source = source_file(source)
        destination = bundle / 'materials' / f'{import_id}_{number}.mt'
        destination.parent.mkdir(parents=True, exist_ok=True)
        lines = []
        display_name = source.stem
        for line in source.read_text(encoding='utf-8').splitlines():
            key, separator, value = line.partition('=')
            key = key.strip()
            value = value.strip()
            if separator and key == 'name':
                display_name = value or source.stem
                line = f'name={import_id}_{number}'
            elif separator and key.endswith('_texture') and value:
                path, query, suffix = value.partition('?')
                texture = legacy_source(path, source.parent, self.legacy_root)
                copied = self.copy_blob(texture, bundle / 'textures')
                line = f'{key}=./../textures/{copied.name}' + ('?' + suffix if query else '')
            lines.append(line)
        destination.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        self.asset_display_names[destination] = display_name
        self.pack_material_textures(destination)
        return destination

    def import_cooked_mesh(self, source):
        import_id = str(uuid.uuid4())
        bundle = self.stage / 'builds' / import_id
        bundle.mkdir(parents=True)
        inspect_path = bundle / 'inspection.json'
        self.run_tool('mesh', ['--inspect', '--input', source, '--output', inspect_path], 'Inspecting cooked model')
        inventory = load_json(inspect_path)
        materials = inventory.get('materials')
        if inventory.get('version') != 1 or not isinstance(materials, list):
            raise JobError('Cooker returned an invalid material inventory')
        mesh = self.copy_file(source, bundle / 'mesh.vkb')
        existing_remap = Path(str(source) + '.remap.json')
        old_map = load_json(existing_remap).get('materials') if existing_remap.exists() else None
        remaps = {}
        for old_reference in dict.fromkeys(materials):
            mapped = old_map.get(old_reference) if old_map is not None else old_reference
            if not mapped:
                raise JobError(f'Cooked dependency has no immutable remap: {old_reference}')
            material = legacy_source(mapped, Path(source).parent, self.legacy_root)
            imported = self.import_material(material, bundle, import_id, len(remaps))
            remaps[old_reference] = relative_reference(imported, bundle)
        atomic_json(Path(str(mesh) + '.remap.json'), {'version': 1, 'materials': remaps})
        reference = self.artifact('mesh', Path(source).stem, mesh, import_id=import_id,
                                  reimport_status='source_unavailable', source_fingerprint=inventory.get('fingerprint'))
        self.index_bundle(bundle, import_id)
        atomic_json(self.stage / 'imports' / (import_id + '.json'), {
            'version': 1, 'id': import_id, 'reimport_status': 'source_unavailable',
            'material_remaps': remaps, 'artifacts': [a for a in self.assets if a.get('import_id') == import_id]})
        return reference

    def import_font(self, source):
        source = source_file(source)
        import_id = str(uuid.uuid4())
        bundle = self.stage / 'builds' / import_id
        bundle.mkdir(parents=True)
        copied = self.copy_file(source, bundle / ('font' + source.suffix.lower()))
        if source.suffix.lower() not in ('.ttf', '.otf'):
            raise JobError('Scene font must be a TTF or OTF file')
        face_info = bundle / 'face.json'
        self.run_tool('font', ['--inspect-font', copied, '--output', face_info], 'Inspecting font face')
        face = load_json(face_info)
        if face.get('version') != 1 or face.get('face_index') != 0 or not isinstance(face.get('face'), str):
            raise JobError('Font cooker returned invalid face metadata')
        config = bundle / 'font.fontcfg'
        config.write_text('\n'.join([
            'type=cooked_mtsdf', 'source=' + copied.name, 'file=font.vkfa',
            'face=' + face['face'], 'face_index=0', 'size=32',
            'charset=U+0020-U+007E', 'charset=U+00A0-U+00FF', 'fallback=U+003F',
            'fallback_policy=reject', 'field=mtsdf', 'atlas_width=1024',
            'atlas_height=1024', 'atlas_px_per_em=64', 'distance_range=16',
            'inner_padding_px=0', 'outer_padding_px=0', 'edge_coloring=inktrap',
            'edge_angle_degrees=3', 'edge_seed=1', 'pixel_format=rgba8_unorm',
            'miter_limit=1', 'scanline=true', 'threads=1', '']), encoding='utf-8')
        if self.request.get('bakes', {}).get('prepare_assets') is False:
            asset_id = str(uuid.uuid4())
            self.assets.append({'id': asset_id, 'kind': 'font', 'name': source.stem,
                'import_id': import_id, 'source': copied.relative_to(self.stage).as_posix(),
                'artifacts': [], 'recipe': {'tool': 'font', 'version': 1,
                    'config': config.relative_to(self.stage).as_posix()},
                'fingerprint': 'sha256:' + digest(copied)})
            return {'scope': 'scene', 'id': asset_id, 'role': 'font'}
        self.run_tool('font', ['--config', config], 'Cooking scene font')
        if not (bundle / 'font.vkfa').is_file():
            raise JobError('Font cooker did not publish its artifact')
        return self.artifact('font', source.stem, config, import_id=import_id, source=copied)

    def import_environment(self, value, origin, source_kind=None):
        if isinstance(value, dict) and value.get('path'):
            if value.get('base_path') or value.get('extension'):
                raise JobError('Cubemap mixes a packed path and face paths')
            return self.import_environment(value['path'], origin, 'cubemap')
        if isinstance(value, str):
            source = legacy_source(value, origin, self.legacy_root)
            copied = self.copy_blob(source, self.stage / 'builds' / 'environments')
            return self.artifact('environment', source.stem, copied,
                                 source_kind=source_kind or ('equirect' if source.suffix.lower() == '.hdr' else 'cubemap'))
        if isinstance(value, dict) and value.get('base_path'):
            extension = value.get('extension', 'png').lstrip('.')
            if not extension.isascii() or not extension.isalnum():
                raise JobError('Invalid cube face extension')
            import_id = str(uuid.uuid4())
            directory = self.stage / 'builds' / import_id
            directory.mkdir(parents=True)
            face_paths = []
            output_extension = None
            for face in FACES:
                # Existing loader uses base_path + '_' + face + '.' + extension.
                source = legacy_source(value['base_path'] + '_' + face + '.' + extension,
                                       origin, self.legacy_root)
                actual_extension = source.suffix.lstrip('.')
                if output_extension is not None and actual_extension != output_extension:
                    raise JobError('Cubemap faces use inconsistent source formats')
                output_extension = actual_extension
                copied = self.copy_file(source, directory / ('cube_' + face + '.' + output_extension))
                face_paths.append(copied)
            reference = self.artifact('environment', 'Cubemap', face_paths[0], import_id=import_id,
                                      source_kind='faces', base_path=(directory / 'cube').relative_to(self.stage).as_posix(),
                                      extension=output_extension)
            return reference
        raise JobError('Unsupported environment asset')

    def inspect_mesh(self, mesh):
        mesh = Path(mesh).resolve(strict=True)
        if mesh not in self.inspections:
            directory = (self.runtime_directory / 'inspection') if self.read_only else self.workspace / 'jobs' / 'inspection'
            directory.mkdir(parents=True, exist_ok=True)
            report = directory / (str(uuid.uuid4()) + '.json')
            self.run_tool('mesh', ['--inspect', '--input', mesh, '--output', report], 'Validating cooked model')
            self.inspections[mesh] = load_json(report)
        return self.inspections[mesh]

    @staticmethod
    def mesh_identity(seed, mesh_hash):
        value = int(seed, 16)
        for byte in struct.pack('<Q', int(mesh_hash, 16)):
            value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
        return f'{value:016x}'

    def remap_overlay(self, path, expected, scene):
        overlay = load_json(path)
        if overlay.get('version') != 1 or not isinstance(overlay.get('overrides'), list):
            raise JobError('Unsupported authored override journal')
        seen = set()
        for record in overlay['overrides']:
            key = (record.get('scene_entity'), record.get('gltf_node'))
            if key in seen:
                raise JobError('Duplicate authored override source identity')
            seen.add(key)
            index = record.get('scene_entity')
            entities = scene.get('entities', [])
            if not isinstance(index, int) or not 0 <= index < len(entities):
                raise JobError('Authored override references a missing entity')
            old_hash, new_hash = expected, source_fingerprint(self.scene_id.encode())
            reference = entities[index].get('mesh', {}).get('asset')
            if reference:
                if reference.get('scope') != 'scene':
                    raise JobError('Import the project mesh locally before cloning authored source-node edits')
                asset = next((item for item in self.assets if item['id'] == reference['id']), None)
                if not asset or not asset.get('artifacts'):
                    raise JobError('Authored source-node edits require a built mesh')
                mesh = contained(self.stage, asset['artifacts'][0]['path'])
                info = self.inspect_mesh(mesh)
                if info['nodes']:
                    old_hash = self.mesh_identity(old_hash, info['fingerprint'])
                    new_hash = self.mesh_identity(new_hash, info['fingerprint'])
            elif record.get('gltf_node') != -1:
                raise JobError('Authored source-node edit targets an entity without a mesh')
            if record.get('source_fingerprint') != old_hash:
                raise JobError('Authored overrides conflict with the selected source scene')
            record['source_fingerprint'] = new_hash
        atomic_json(path, overlay)

    def import_scene(self, source):
        source = source_file(source)
        scene = load_json(source)
        if scene.get('version') == 3:
            return self.import_managed_scene(scene, source.parent)
        if scene.get('version', 1) not in (1, 2):
            raise JobError('Unsupported scene JSON version')
        scene = copy.deepcopy(scene)
        environment = scene.get('environment')
        if isinstance(environment, dict):
            field = 'equirect' if environment.get('equirect') else 'cubemap'
            if environment.get(field):
                environment['asset'] = self.import_environment(environment.pop(field), source.parent, field)
        for probe in scene.get('reflection_probes', []):
            if probe.get('cubemap'):
                reference = self.import_environment(probe.pop('cubemap'), source.parent)
                reference['role'] = 'probe-cube'
                self.assets[-1]['artifacts'][0]['role'] = 'probe-cube'
                probe['asset'] = reference
        for volume in ([scene['diffuse_volume']] if scene.get('diffuse_volume') else []):
            if volume.get('path'):
                original = legacy_source(volume.pop('path'), source.parent, self.legacy_root)
                copied = self.copy_blob(original, self.stage / 'builds' / 'volumes')
                volume['asset'] = self.artifact('volume', original.stem, copied)
        mesh_cache = {}
        for entity in scene.get('entities', []):
            entity['id'] = str(uuid.uuid4())
            mesh = entity.get('mesh')
            if mesh and mesh.get('path'):
                original = legacy_source(mesh.pop('path'), source.parent, self.legacy_root)
                if original not in mesh_cache:
                    mesh_cache[original] = self.import_cooked_mesh(original) if original.suffix.lower() == '.vkb' else self.import_model(original)
                mesh['asset'] = mesh_cache[original]
            material = entity.get('shape', {}).get('material')
            if material and material.get('name') and not material.get('path'):
                raise JobError('Legacy named material needs its explicit source path before import')
            if material and material.get('path'):
                original = legacy_source(material.pop('path'), source.parent, self.legacy_root)
                import_id = str(uuid.uuid4())
                bundle = self.stage / 'builds' / import_id
                copied = self.import_material(original, bundle, import_id, 0)
                material['asset'] = self.artifact('material', original.stem, copied, import_id=import_id)
            text = entity.get('text3d')
            if text and isinstance(text.get('font'), str):
                if text['font'] not in ('UbuntuMono', 'UbuntuMono-cooked', 'default-scene-font'):
                    raise JobError(f'Locate the source/config for legacy text font {text["font"]} before importing')
                text['font'] = None
        # Authored overlay is imported separately only after native fingerprint validation.
        overlay = Path(str(source) + '.editor.json')
        if overlay.is_file():
            copied = self.copy_file(overlay, self.stage / 'edits' / 'scene.editor.json')
            expected = source_fingerprint(scene['source_identity'].encode()) if scene.get('source_identity') else source_fingerprint(source.read_bytes())
            self.remap_overlay(copied, expected, scene)
            scene['edit_overlay'] = copied.relative_to(self.stage).as_posix()
        scene.pop('source_identity', None)
        return scene

    def import_managed_scene(self, scene, origin):
        scene = copy.deepcopy(scene)
        if any(a.get('scope') not in (None, 'scene') for a in scene.get('assets', [])):
            raise JobError('Managed scene import has an invalid asset inventory')
        # A managed scene bundle is the dependency closure. Copy only this owner;
        # project/editor references are resolved explicitly below, never by basename.
        for child in origin.iterdir():
            if child.name in ('.runtime', 'scene.json'):
                continue
            if child.is_symlink():
                raise JobError('Resolve symlinks before importing a managed scene')
            if child.is_dir():
                for path in child.rglob('*'):
                    if path.is_symlink():
                        raise JobError('Managed scene contains a symlink')
                    if path.is_file():
                        self.copy_file(path, self.stage / path.relative_to(origin))
            elif child.is_file():
                self.copy_file(child, self.stage / child.name)
        self.assets = scene.get('assets', [])
        if scene.get('edit_overlay'):
            self.remap_overlay(contained(self.stage, scene['edit_overlay'], must_exist=False),
                               source_fingerprint(identifier(scene['id']).encode()), scene)
        source_project_path = origin.parent.parent / 'project.json'
        source_project = load_json(source_project_path) if source_project_path.is_file() else {'assets': []}
        if not scene.get('default_font'):
            scene['default_font'] = source_project.get('default_font')
        remapped_ids = {record['id']: str(uuid.uuid4()) for record in self.assets}
        for record in self.assets:
            record['id'] = remapped_ids[record['id']]
        shared = {}

        def remap(value):
            if isinstance(value, list):
                return [remap(item) for item in value]
            if not isinstance(value, dict):
                return value
            if value.get('scope') == 'scene' and 'id' in value:
                if value['id'] not in remapped_ids:
                    raise JobError('Imported scene references an unknown asset')
                return {**value, 'id': remapped_ids[value['id']]}
            if value.get('scope') == 'project' and 'id' in value:
                if value['id'] not in shared:
                    matches = [record for record in source_project.get('assets', []) if record.get('id') == value['id']]
                    if len(matches) != 1:
                        raise JobError('Imported scene has an unavailable project asset')
                    record = copy.deepcopy(matches[0])
                    new_id = str(uuid.uuid4())
                    products = record.get('artifacts', [])
                    if not products:
                        raise JobError('Imported project asset has no artifact')
                    first = contained(source_project_path.parent, products[0]['path'])
                    bundle_root = first.parent.parent if first.parent.name == 'materials' else first.parent
                    target = self.stage / 'builds' / new_id
                    for path in bundle_root.rglob('*'):
                        if path.is_symlink():
                            raise JobError('Imported project asset contains a symlink')
                        if path.is_file():
                            self.copy_file(path, target / path.relative_to(bundle_root))
                    for product in products:
                        old_path = contained(source_project_path.parent, product['path'])
                        product['path'] = (target / old_path.relative_to(bundle_root)).relative_to(self.stage).as_posix()
                    record.pop('closure', None)
                    record.update(id=new_id, source=None)
                    self.assets.append(record)
                    shared[value['id']] = new_id
                return {**value, 'scope': 'scene', 'id': shared[value['id']]}
            return {key: remap(item) for key, item in value.items()}

        for key in list(scene):
            if key != 'assets':
                scene[key] = remap(scene[key])
        for entity in scene.get('entities', []):
            entity['id'] = str(uuid.uuid4())
        return scene

    def create(self):
        if self.final.exists():
            raise JobError('Scene identifier already exists')
        staging = self.project_root / '.staging'
        staging.mkdir(parents=True, exist_ok=True)
        self.stage = Path(tempfile.mkdtemp(prefix=self.scene_id + '-', dir=staging))
        self.progress('Preparing scene', 0.05)
        if self.request.get('source_scene'):
            scene = self.import_scene(self.request['source_scene'])
        else:
            scene = {'version': 3, 'entities': [], 'environment': {'enabled': False},
                     'reflection_probes': []}
        for model in self.request.get('models', []):
            path = model['source'] if isinstance(model, dict) else model
            entity = {'id': str(uuid.uuid4()), 'name': Path(path).stem, 'parent': None,
                      'transform': {'pos': [0, 0, 0], 'rot': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                      'mesh': {'asset': self.import_model(path), 'pipeline_domain': 'world'}}
            if isinstance(model, dict) and model.get('transform'):
                entity['transform'] = model['transform']
            scene.setdefault('entities', []).append(entity)
        for light in self.request.get('lights', []):
            if not any(kind in light for kind in ('point_light', 'spot_light', 'directional_light', 'rectangle_light')):
                raise JobError('New entities are limited to lights')
            entity = copy.deepcopy(light)
            if 'spot_light' in entity:
                if 'point_light' in entity:
                    raise JobError('A light cannot contain both spot and point components')
                entity['point_light'] = {**entity.pop('spot_light'), 'kind': 2}
            entity['id'] = str(uuid.uuid4())
            scene.setdefault('entities', []).append(entity)
        environment = self.request.get('environment') or {}
        if environment.get('source'):
            scene['environment'] = {key: environment[key] for key in
                ('enabled', 'intensity', 'diffuse_intensity', 'specular_intensity') if key in environment}
            scene['environment'].setdefault('enabled', True)
            scene['environment']['asset'] = self.import_environment(environment['source'], Path.cwd())
        if self.request.get('reflection_probes') is not None:
            scene['reflection_probes'] = copy.deepcopy(self.request['reflection_probes'])
        for probe in scene.get('reflection_probes', []):
            if probe.get('enabled', True) and not probe.get('asset'):
                asset_id = str(uuid.uuid4())
                self.assets.append({'id': asset_id, 'kind': 'probe-cube', 'name': 'Reflection probe',
                    'import_id': asset_id, 'source': None, 'artifacts': [],
                    'recipe': {'tool': 'reflection', 'version': 1}, 'source_kind': 'cubemap'})
                probe['asset'] = {'scope': 'scene', 'id': asset_id, 'role': 'probe-cube'}
        if self.request.get('font_source'):
            scene['default_font'] = self.import_font(self.request['font_source'])
        scene.update(version=3, id=self.scene_id, assets=self.assets)
        scene.setdefault('default_font', None)
        scene.setdefault('edit_overlay', None)
        scene.setdefault('bake_recipes', {})
        self.validate_semantics(scene)
        self.perform_bakes(scene)
        atomic_json(self.stage / 'scene.json', scene)
        self.progress('Validating scene dependencies', 0.85)
        self.validate_semantics(scene)
        pending_bakes = scene['bake_recipes'].get('prepare_assets') is False and (scene['bake_recipes'].get('reflection') or scene['bake_recipes'].get('diffuse'))
        unbuilt = any(not record.get('artifacts') for record in self.assets) or pending_bakes
        if not unbuilt:
            self.lower(scene, self.stage, publish_runtime=False)
        atomic_json(self.stage / 'scene.json', scene)
        self.final.parent.mkdir(parents=True, exist_ok=True)
        if self.final.exists():
            raise JobError('Scene was created by another writer')
        self.final.mkdir()
        self.final_owned = True
        os.replace(self.stage, self.final)
        self.stage = None
        self.progress('Opening scene', 0.95)
        if unbuilt:
            return {'version': VERSION, 'status': 'unbuilt', 'scene_id': self.scene_id,
                    'scene_path': str(self.final / 'scene.json'), 'fonts': [], 'warnings': self.warnings}
        return self.prepare(self.final / 'scene.json')

    def validate_semantics(self, scene):
        if not isinstance(scene.get('entities', []), list) or len(scene.get('entities', [])) > 65536:
            raise JobError('Invalid entity array')
        environment = scene.get('environment') or {}
        for key in ('intensity', 'diffuse_intensity', 'specular_intensity'):
            value = environment.get(key, 1)
            if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value) or value < 0:
                raise JobError(f'Environment {key} must be finite and nonnegative')
        if 'enabled' in environment and not isinstance(environment['enabled'], bool):
            raise JobError('Environment enabled must be boolean')
        probes = scene.get('reflection_probes', [])
        if not isinstance(probes, list) or len(probes) > 64:
            raise JobError('Invalid reflection probe array')
        for probe in probes:
            for field in ('center', 'extents'):
                values = probe.get(field)
                if not isinstance(values, list) or len(values) != 3 or any(
                        not isinstance(v, (int, float)) or isinstance(v, bool) or not math.isfinite(v) for v in values):
                    raise JobError(f'Probe {field} requires three finite values')
                if field == 'extents' and any(v <= 0 for v in values):
                    raise JobError('Probe extents must be positive')
            for field in ('blend_distance', 'intensity', 'diffuse_intensity', 'specular_intensity'):
                value = probe.get(field, 1)
                if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value) or value < 0:
                    raise JobError(f'Probe {field} must be finite and nonnegative')
            if 'enabled' in probe and not isinstance(probe['enabled'], bool):
                raise JobError('Probe enabled must be boolean')

    def run_script(self, script, arguments, label):
        executable = self.tools.get('python') or sys.executable
        # Keep all child handling in run_tool, including cancellation/reaping.
        original = self.tools.get('_script_python')
        self.tools['_script_python'] = executable
        try:
            self.run_tool('_script_python', [Path(__file__).resolve().parent / script, *arguments], label)
        finally:
            if original is None:
                self.tools.pop('_script_python', None)
            else:
                self.tools['_script_python'] = original

    def effective_bake_runtime(self, scene, root):
        result = self.lower(scene, root)
        overlay = scene.get('edit_overlay')
        if not overlay:
            return result
        journal = load_json(contained(root, overlay))
        if journal.get('version') != 1 or not isinstance(journal.get('overrides'), list):
            raise JobError('Unsupported authored override journal')
        runtime = load_json(result['runtime_path'])
        entities = runtime.get('entities', [])
        wrapper_edits, node_edits = {}, {}
        for edit in journal['overrides']:
            index, node = edit.get('scene_entity'), edit.get('gltf_node')
            if not isinstance(index, int) or not 0 <= index < len(entities) or not isinstance(node, int) or node < -1:
                raise JobError('Authored override target is unavailable for baking')
            target = wrapper_edits if node == -1 else node_edits.setdefault(index, {})
            key = index if node == -1 else node
            if key in target:
                raise JobError('Duplicate authored override target')
            target[key] = edit
            expected = source_fingerprint(self.scene_id.encode())
            if entities[index].get('mesh'):
                info = self.inspect_mesh(entities[index]['mesh']['path'])
                if info['nodes']:
                    expected = self.mesh_identity(expected, info['fingerprint'])
            if edit.get('source_fingerprint') != expected:
                raise JobError('Authored override source identity conflicts with scene')

        def apply_wrapper(entity, edit):
            fields = edit['fields']
            if fields & 1:
                entity['transform'] = dict(pos=edit['position'], rot=edit['rotation'], scale=edit['scale'])
            if fields & 2:
                entity['name'] = edit['name']
            if fields & 8:
                intensity, constant, linear, quadratic, distance, inner, outer = edit['point_params']
                entity['point_light'] = dict(color=edit['point_color'], direction_local=edit['point_direction'],
                    intensity=intensity, attenuation=dict(constant=constant, linear=linear, quadratic=quadratic),
                    range=distance, inner_cone_angle=inner, outer_cone_angle=outer, kind=edit['point_kind'],
                    enabled=edit['point_enabled'], casts_shadow=edit.get('point_casts_shadow', False))
            if fields & 16:
                entity['directional_light'] = dict(color=edit['directional_color'], direction_local=edit['directional_direction'],
                    intensity=edit['directional_intensity'][0], enabled=edit['directional_enabled'],
                    sun_angular_diameter_degrees=edit.get('directional_sun_angular_diameter_degrees', [0.53])[0])
            if fields & 32:
                entity['rectangle_light'] = dict(color=edit['rectangle_color'], radiance=edit['rectangle_radiance'][0],
                    size=edit['rectangle_size'], enabled=edit['rectangle_enabled'])

        visible, visiting = {}, set()
        def visibility(index):
            if index in visible:
                return visible[index]
            if index in visiting:
                raise JobError('Entity parent cycle in bake input')
            visiting.add(index)
            edit = wrapper_edits.get(index, {})
            local = edit.get('visible', True)
            parent = entities[index].get('parent')
            if parent is not None and edit.get('inherit', True):
                if not isinstance(parent, int) or not 0 <= parent < len(entities):
                    raise JobError('Invalid entity parent in bake input')
                local = local and visibility(parent)
            visible[index] = local
            visiting.remove(index)
            return local

        bake_root = self.workspace / 'jobs' / ('effective-' + str(uuid.uuid4()))
        bake_root.mkdir(parents=True)
        for index, entity in enumerate(list(entities)):
            if index in wrapper_edits:
                apply_wrapper(entity, wrapper_edits[index])
            wrapper_visible = visibility(index)
            edits = node_edits.get(index, {})
            if entity.get('mesh') and (edits or not wrapper_visible):
                mesh = Path(entity['mesh']['path'])
                report = bake_root / f'{index}.inspection.json'
                self.run_tool('mesh', ['--inspect', '--input', mesh, '--output', report], 'Resolving authored bake geometry')
                info = load_json(report)
                if not info['nodes'] and not wrapper_visible:
                    entity.pop('mesh', None)
                nodes = {node['index']: node for node in info['nodes']}
                if any(node not in nodes or edit.get('source_fingerprint') != self.mesh_identity(source_fingerprint(self.scene_id.encode()), info['fingerprint']) for node, edit in edits.items()):
                    raise JobError('Source-node overrides conflict with the cooked model; resolve before baking')
                node_visible = {}
                def visibility_node(node, trail=None):
                    if node in node_visible:
                        return node_visible[node]
                    trail = set() if trail is None else trail
                    if node in trail:
                        raise JobError('Source-node parent cycle')
                    trail.add(node)
                    edit = edits.get(node, {})
                    parent = nodes[node]['parent']
                    inherited = visibility_node(parent, trail) if parent != -1 else wrapper_visible
                    result = edit.get('visible', True) and (inherited if edit.get('inherit', True) else True)
                    node_visible[node] = result
                    return result
                light_anchors = {}
                def light_anchor(node):
                    if node in light_anchors:
                        return light_anchors[node]
                    parent_node = nodes[node]['parent']
                    parent_index = light_anchor(parent_node) if parent_node != -1 else index
                    anchor = {'parent': parent_index, 'transform': {'matrix': nodes[node]['matrix']}}
                    edit = edits.get(node)
                    if edit:
                        apply_wrapper(anchor, edit)
                    if not visibility_node(node):
                        for component in ('point_light', 'directional_light', 'rectangle_light'):
                            anchor.pop(component, None)
                    light_anchors[node] = len(entities)
                    entities.append(anchor)
                    return light_anchors[node]

                patches = []
                for node in nodes:
                    edit = edits.get(node, {})
                    patch = {'index': node, 'visible': visibility_node(node)}
                    fields = edit.get('fields', 0)
                    if fields & 1:
                        patch.update(position=edit['position'], rotation=edit['rotation'], scale=edit['scale'])
                    if fields & 56:
                        if not nodes[node]['in_scene']:
                            raise JobError('Light override refers to an inactive source node')
                        light_anchor(node)
                        patch['punctual'] = {'kind': 0}
                    patches.append(patch)
                patch_path = bake_root / f'{index}.patches.json'
                atomic_json(patch_path, {'version': 1, 'source_fingerprint': info['fingerprint'], 'nodes': patches})
                variant = bake_root / f'{index}.vkb'
                self.run_tool('mesh', ['--input', mesh, '--output', variant, '--source-patches', patch_path], 'Preparing authored bake geometry')
                remap = load_json(str(mesh) + '.remap.json')
                remap['materials'] = {key: './' + os.path.relpath((mesh.parent / value).resolve(), bake_root).replace(os.sep, '/')
                                      for key, value in remap['materials'].items()}
                atomic_json(str(variant) + '.remap.json', remap)
                if entity.get('mesh'):
                    entity['mesh']['path'] = str(variant)
            if not wrapper_visible:
                for component in ('shape', 'text3d', 'point_light', 'directional_light', 'rectangle_light'):
                    entity.pop(component, None)
        runtime_path = bake_root / 'scene.json'
        atomic_json(runtime_path, runtime)
        result['runtime_path'] = str(runtime_path)
        return result

    def perform_bakes(self, scene, asset_root=None):
        bakes = self.request.get('bakes') or scene.get('bake_recipes') or {}
        scene['bake_recipes'].update(bakes)
        if bakes.get('prepare_assets') is False:
            return
        asset_root = asset_root or self.stage
        if bakes.get('reflection'):
            if not self.tools.get('harness') or not self.tools.get('hdr_packer'):
                raise JobError('Reflection baking requires the installed harness and HDR cubemap packer')
            capture_scene = copy.deepcopy(scene)
            capture_scene['reflection_probes'] = []
            runtime = self.effective_bake_runtime(capture_scene, asset_root)
            for probe in scene.get('reflection_probes', []):
                if not probe.get('enabled', True):
                    continue
                reference = probe.get('asset')
                if not reference or reference.get('scope') != 'scene':
                    raise JobError('Probe capture requires a scene-owned destination')
                record = next((item for item in scene['assets'] if item['id'] == reference['id']), None)
                if record is None:
                    raise JobError('Probe capture destination is missing')
                revision = str(uuid.uuid4())
                destination = asset_root / 'builds' / revision / 'probe.vkt'
                destination.parent.mkdir(parents=True)
                arguments = ['--workspace-root', self.workspace, '--scene', runtime['runtime_path'],
                    '--output', destination, '--position', *probe['center'],
                    '--size', probe.get('resolution', 256),
                    '--near-plane', probe.get('near_plane', 0.1),
                    '--far-plane', probe.get('far_plane', 1000),
                    '--harness', self.tools['harness'], '--packer', self.tools['hdr_packer']]
                if self.tools.get('profile'):
                    arguments += ['--profile', self.tools['profile']]
                self.run_script('bake_reflection_probe.py', arguments, 'Baking reflection probe')
                if not destination.is_file():
                    raise JobError('Reflection baker did not publish a cubemap')
                record['artifacts'] = [{'role': 'probe-cube',
                    'path': destination.relative_to(asset_root).as_posix(), 'version': 1}]
                record['fingerprint'] = 'sha256:' + digest(destination)
                record.pop('closure', None)
        if bakes.get('diffuse'):
            if not self.tools.get('diffuse'):
                raise JobError('Diffuse baking requires the installed diffuse baker')
            bake_scene = copy.deepcopy(scene)
            bake_scene.pop('diffuse_volume', None)
            runtime = self.effective_bake_runtime(bake_scene, asset_root)
            revision = str(uuid.uuid4())
            destination = asset_root / 'builds' / revision / 'volume.vkdv'
            destination.parent.mkdir(parents=True)
            arguments = ['--workspace-root', self.workspace, '--scene', runtime['runtime_path'],
                         '--output', destination, '--baker', self.tools['diffuse']]
            settings = self.request.get('diffuse_settings', {})
            for field in ('bounds', 'grid'):
                if settings.get(field):
                    arguments += ['--' + field, *settings[field]]
            for field in ('voxel_size', 'face_size', 'samples', 'max_depth', 'seed', 'photons', 'photon_radius'):
                if field in settings:
                    arguments += ['--' + field.replace('_', '-'), settings[field]]
            self.run_script('bake_diffuse_volume.py', arguments, 'Baking diffuse volume')
            if not destination.is_file():
                raise JobError('Diffuse baker did not publish its artifact')
            record = {'id': str(uuid.uuid4()), 'kind': 'volume', 'name': 'Diffuse volume',
                'import_id': revision, 'source': None, 'artifacts': [{'role': 'volume',
                    'path': destination.relative_to(asset_root).as_posix(), 'version': 1}],
                'fingerprint': 'sha256:' + digest(destination),
                'recipe': {'tool': 'diffuse', 'version': 1, 'settings': settings}}
            previous_ref = scene.get('diffuse_volume', {}).get('asset', {})
            previous = next((item for item in self.assets if item['id'] == previous_ref.get('id')), None)
            if previous is not None:
                record['id'] = previous['id']
                self.assets.remove(previous)
            self.assets.append(record)
            scene['assets'] = self.assets
            scene['diffuse_volume'] = {'asset': {'scope': 'scene', 'id': record['id'], 'role': 'volume'}}

    def project_document(self):
        project = load_json(self.project_path) if self.project_path.is_file() else {'assets': [], 'default_font': None}
        if self.pending_project_assets is not None:
            project['assets'] = self.pending_project_assets
        if self.pending_default_font is not None:
            project['default_font'] = self.pending_default_font
        return project

    def validate_bundle_dependencies(self, owner, path, visited):
        path = Path(path).resolve(strict=True)
        if path in visited:
            return
        visited.add(path)
        if not path.is_relative_to(owner.resolve()):
            raise JobError('Bundle dependency escapes its managed owner')

        def dependency(value, explicit=True):
            if not isinstance(value, str) or not value or '\\' in value or ':' in value or '\x00' in value:
                raise JobError('Invalid bundle dependency path')
            raw = value.split('?', 1)[0]
            if Path(raw).is_absolute() or explicit and not raw.startswith('./'):
                raise JobError('Managed bundle dependency must be file-relative')
            target = (path.parent / raw).resolve(strict=True)
            if not target.is_file() or not target.is_relative_to(owner.resolve()):
                raise JobError('Bundle dependency is unavailable or escapes its owner')
            self.validate_bundle_dependencies(owner, target, visited)

        if path.suffix.lower() == '.vkb':
            sidecar = Path(str(path) + '.remap.json')
            if not sidecar.is_file() or not sidecar.resolve().is_relative_to(owner.resolve()):
                raise JobError('Managed mesh has no contained immutable material map')
            visited.add(sidecar.resolve())
            remap = load_json(sidecar)
            if set(remap) != {'version', 'materials'} or remap['version'] != 1 or not isinstance(remap['materials'], dict):
                raise JobError('Invalid immutable material map')
            info = self.inspect_mesh(path)
            if set(remap['materials']) != set(info['materials']):
                raise JobError('Immutable material map does not match cooked dependency inventory')
            for value in remap['materials'].values():
                dependency(value)
        elif path.suffix.lower() in ('.mt', '.fontcfg'):
            if path.stat().st_size > MAX_JSON_BYTES:
                raise JobError('Material or font configuration exceeds the parser limit')
            keys = set()
            cooked_font = False
            font_file = False
            for line in path.read_text(encoding='utf-8').splitlines():
                key, separator, value = line.partition('=')
                key = key.strip()
                value = value.strip()
                if not separator or key.startswith('#'):
                    continue
                if path.suffix.lower() == '.mt' and key.endswith('_texture') and value:
                    if key in keys:
                        raise JobError('Material contains a duplicate texture field')
                    keys.add(key)
                    dependency(value)
                elif path.suffix.lower() == '.fontcfg':
                    if key == 'type':
                        cooked_font = value == 'cooked_mtsdf'
                    elif key == 'file':
                        if font_file:
                            raise JobError('Font configuration contains duplicate files')
                        dependency(value, explicit=False)
                        font_file = True
            if path.suffix.lower() == '.fontcfg' and (not cooked_font or not font_file):
                raise JobError('Managed fonts require a cooked MTSDF artifact')

    def lower(self, scene, root, publish_runtime=True):
        if scene.get('version') != 3 or scene.get('id') != self.scene_id:
            raise JobError('Managed scene version or ID mismatch')
        project = self.project_document()
        inventories = {'scene': (root, scene.get('assets', [])),
                       'project': (self.project_root, project.get('assets', []))}
        editor_manifest = self.workspace / 'editor' / 'bundles' / '1' / 'manifest.json'
        if not editor_manifest.is_file():
            editor_manifest = self.workspace / 'editor' / 'bundle.json'
        if editor_manifest.is_file():
            inventories['editor'] = (editor_manifest.parent, load_json(editor_manifest).get('assets', []))
        for _, (owner, records) in inventories.items():
            visited = set()
            ids = set()
            if not isinstance(records, list) or len(records) > MAX_IMPORT_FILES:
                raise JobError('Asset inventory exceeds the supported bound')
            for record in records:
                if not isinstance(record, dict) or not record.get('id') or record['id'] in ids:
                    raise JobError('Duplicate or invalid asset identifier')
                ids.add(record['id'])
                record_paths = set()
                for product_index, product in enumerate(record.get('artifacts', [])):
                    path = contained(owner, product['path'])
                    if not path.is_file():
                        raise JobError('Asset artifact is not a regular file')
                    expected = product.get('fingerprint') or (record.get('fingerprint') if product_index == 0 else None)
                    if expected and expected != 'sha256:' + digest(path):
                        raise JobError(f'Asset bytes changed: {record.get("name", path.name)}; rebuild or reimport it')
                    self.validate_bundle_dependencies(owner, path, record_paths)
                if record.get('source_kind') == 'faces':
                    for face in FACES:
                        face_path = contained(owner, record['base_path'] + '_' + face + '.' + record['extension'])
                        record_paths.add(face_path)
                closure = record.get('closure')
                current = {item.relative_to(owner).as_posix(): digest(item) for item in record_paths}
                if closure is not None and closure != current:
                    raise JobError(f'Asset dependency closure changed: {record.get("name", record["id"])}; rebuild or reimport it')
                if record_paths and not self.read_only:
                    record['closure'] = current
        fonts = {}

        def asset(reference, default_role):
            if not isinstance(reference, dict) or reference.get('scope') not in inventories:
                raise JobError(f'Missing {default_role} asset scope')
            owner, records = inventories[reference['scope']]
            matches = [record for record in records if record.get('id') == reference.get('id')]
            if len(matches) != 1:
                raise JobError(f'Missing or duplicate {default_role} asset: {reference.get("id")}')
            record = matches[0]
            role = reference.get('role', default_role)
            if role != default_role:
                raise JobError(f'Expected {default_role} artifact role, got {role}')
            products = [item for item in record.get('artifacts', []) if item.get('role') == role]
            if len(products) != 1:
                raise JobError(f'Asset {record.get("name", role)} has no unique {role} artifact; rebuild it')
            path = contained(owner, products[0]['path'])
            if not path.is_file():
                raise JobError(f'Asset is not a file: {path.name}')
            return path, record, owner

        def font(reference):
            reference = reference or scene.get('default_font') or project.get('default_font')
            if reference is None or reference == {'scope': 'editor', 'id': 'default-scene-font'} and 'editor' not in inventories:
                # C bootstrap always registers this explicit editor font.
                return 'default-scene-font'
            path, record, _ = asset(reference, 'font')
            name = record['id']
            fonts[name] = {'name': name, 'config': str(path)}
            return name

        def cube(component, default_role, environment=False):
            if 'asset' not in component:
                if component.get('enabled') and not environment:
                    raise JobError('Enabled probe has no cubemap asset; bake it or disable the probe')
                if any(key in component for key in ('equirect', 'cubemap')):
                    raise JobError('Managed environment/probe still contains a legacy path')
                return
            if any(key in component for key in ('equirect', 'cubemap')):
                raise JobError('Managed environment mixes asset and legacy path fields')
            path, record, owner = asset(component.pop('asset'), default_role)
            kind = record.get('source_kind', 'cubemap')
            if kind == 'faces':
                base = contained(owner, record['base_path'], must_exist=False)
                extension = record['extension']
                for face in FACES:
                    contained(owner, str(base.relative_to(owner)) + '_' + face + '.' + extension)
                component['cubemap'] = {'base_path': str(base), 'extension': extension}
            else:
                if environment and kind == 'equirect':
                    component['equirect'] = str(path)
                else:
                    component['cubemap'] = {'path': str(path)}

        runtime = copy.deepcopy(scene)
        runtime['version'] = 2
        runtime['source_identity'] = self.scene_id
        for field in ('id', 'assets', 'default_font', 'bake_recipes', 'edit_overlay'):
            runtime.pop(field, None)
        if isinstance(runtime.get('environment'), dict):
            cube(runtime['environment'], 'environment', True)
        for probe in runtime.get('reflection_probes', []):
            cube(probe, 'probe-cube')
        for volume in ([runtime['diffuse_volume']] if runtime.get('diffuse_volume') else []):
            if 'path' in volume:
                raise JobError('Managed volume contains a legacy path')
            if 'asset' in volume:
                volume['path'] = str(asset(volume.pop('asset'), 'volume')[0])
        for entity in runtime.get('entities', []):
            entity.pop('id', None)
            for component, role in ((entity.get('mesh'), 'mesh'), (entity.get('shape', {}).get('material'), 'material')):
                if not component:
                    continue
                if 'path' in component:
                    raise JobError(f'Managed {role} contains a legacy path')
                if 'asset' not in component:
                    raise JobError(f'Managed {role} requires a typed asset reference')
                if 'asset' in component:
                    component['path'] = str(asset(component.pop('asset'), role)[0])
            if isinstance(entity.get('text3d'), dict):
                entity['text3d']['font'] = font(entity['text3d'].get('font'))
        # Register the selected scene font before any scene text is instantiated.
        font(None)
        overlay = scene.get('edit_overlay')
        edit_path = contained(root, overlay, must_exist=True) if overlay else root / 'edits' / 'scene.editor.json'
        if overlay and not edit_path.is_file():
            raise JobError('Selected authored override journal is unavailable')
        if self.read_only:
            selected_overlay = load_json(edit_path) if overlay else {'version': 1, 'overrides': []}
            edit_path = self.runtime_directory / 'scene.editor.json'
            atomic_json(edit_path, selected_overlay)
        else:
            edit_path.parent.mkdir(parents=True, exist_ok=True)
        cache_key = hashlib.sha256(json.dumps(runtime, sort_keys=True).encode()).hexdigest()
        runtime_root = self.runtime_directory if self.read_only else root / '.runtime'
        runtime_path = runtime_root / (cache_key + '.scene.json')
        if publish_runtime:
            atomic_json(runtime_path, runtime)
        return {'version': VERSION, 'status': 'complete', 'scene_id': self.scene_id,
                'scene_path': str(root / 'scene.json'), 'runtime_path': str(runtime_path),
                'edit_path': str(edit_path), 'fonts': list(fonts.values()), 'warnings': self.warnings}

    def prepare(self, scene_path):
        path = Path(scene_path).resolve(strict=True)
        if path != (self.final / 'scene.json').resolve():
            raise JobError('Scene path does not match its project membership')
        self.progress('Resolving scene assets', 0.2)
        scene_bytes = path.read_bytes()
        project_before = digest(self.project_path) if self.read_only else None
        scene = load_json(path, MAX_MANAGED_DOCUMENT_BYTES)
        self.validate_semantics(scene)
        pending_bakes = scene.get('bake_recipes', {}).get('prepare_assets') is False and (scene.get('bake_recipes', {}).get('reflection') or scene.get('bake_recipes', {}).get('diffuse'))
        if any(not record.get('artifacts') for record in scene.get('assets', [])) or pending_bakes or self.texture_repair_bundles(scene, path.parent):
            if self.read_only:
                raise JobError('Scene has unbuilt assets or needs texture preparation; open with write access and prepare it first')
            return self.prepare_unbuilt(path, scene)
        try:
            result = self.lower(scene, path.parent)
        except (JobError, OSError) as error:
            if self.read_only:
                raise JobError(f'{error}; open with write access to repair the scene, then retry') from error
            raise
        if self.read_only:
            if path.read_bytes() != scene_bytes or digest(self.project_path) != project_before:
                raise JobError('Workspace changed during read-only preparation; retry opening the scene')
            result['manifest_fingerprint'] = source_fingerprint(scene_bytes)
        return result

    def prepare_unbuilt(self, path, scene):
        original_fingerprint = digest(path)
        if scene.get('edit_overlay'):
            load_json(contained(path.parent, scene['edit_overlay']))
        staging = self.project_root / '.staging'
        staging.mkdir(parents=True, exist_ok=True)
        self.stage = Path(tempfile.mkdtemp(prefix=self.scene_id + '-build-', dir=staging))
        self.assets = scene['assets']
        for original in self.texture_repair_bundles(scene, path.parent):
            for sidecar in original.glob('*.vkb.remap.json'):
                for reference, managed_reference in load_json(sidecar).get('materials', {}).items():
                    try:
                        material = legacy_source(reference, original, self.legacy_root)
                        self.asset_display_names[(original / managed_reference).resolve()] = material.stem
                        for line in material.read_text(encoding='utf-8').splitlines():
                            key, separator, value = line.partition('=')
                            if separator and key.strip() == 'name' and value.strip():
                                self.asset_display_names[(original / managed_reference).resolve()] = value.strip()
                            if separator and key.strip().endswith('_texture') and value.strip():
                                source = legacy_source(value.strip().split('?', 1)[0], material.parent, self.legacy_root)
                                self.source_display_names[digest(source)] = source.stem
                                seed = Path(str(source) + '.vkt')
                                if seed.is_file():
                                    self.texture_seeds[digest(source)] = seed
                    except (JobError, OSError, ValueError):
                        pass
            revision = self.stage / 'builds' / str(uuid.uuid4())
            for source in original.rglob('*'):
                if source.is_file():
                    copied = self.copy_file(source, revision / source.relative_to(original))
                    if source.resolve() in self.asset_display_names:
                        self.asset_display_names[copied] = self.asset_display_names[source.resolve()]
            for material in (revision / 'materials').glob('*.mt'):
                self.pack_material_textures(material)
            prefix = original.relative_to(path.parent).as_posix() + '/'
            replacement = revision.relative_to(self.stage).as_posix() + '/'
            for record in self.assets:
                changed = False
                for product in record.get('artifacts', []):
                    if product['path'].startswith(prefix):
                        product['path'] = replacement + product['path'][len(prefix):]
                        product.pop('fingerprint', None)
                        changed = True
                if changed:
                    if isinstance(record.get('source'), str) and record['source'].startswith(prefix):
                        record['source'] = replacement + record['source'][len(prefix):]
                    record.pop('closure', None)
                    product = self.stage / record['artifacts'][0]['path']
                    record['name'] = self.asset_display_names.get(product) or self.source_display_names.get(digest(product), record['name'])
                    record['fingerprint'] = 'sha256:' + digest(product)
        for record in list(self.assets):
            if record.get('artifacts'):
                continue
            if record['kind'] == 'probe-cube':
                continue
            revision = str(uuid.uuid4())
            bundle = self.stage / 'builds' / revision
            bundle.mkdir(parents=True)
            if record['kind'] == 'mesh':
                source = contained(path.parent, record['source'])
                output = bundle / 'mesh.vkb'
                self.run_tool('mesh', ['--input', source, '--output', output,
                    '--bundle-root', bundle, '--import-id', record['import_id']], 'Preparing model')
                self.index_bundle(bundle, record['import_id'])
            elif record['kind'] == 'font':
                config = contained(path.parent, record['recipe']['config'])
                source = contained(path.parent, record['source'])
                self.copy_file(source, bundle / source.name)
                output = self.copy_file(config, bundle / config.name)
                self.run_tool('font', ['--config', output], 'Preparing font')
            else:
                raise JobError(f'No build recipe for {record["kind"]}')
            record['artifacts'] = [{'role': record['kind'],
                                   'path': output.relative_to(self.stage).as_posix(), 'version': 1}]
            record['fingerprint'] = 'sha256:' + digest(output)
            record.pop('closure', None)
        scene['assets'] = self.assets
        self.validate_semantics(scene)
        builds = path.parent / 'builds'
        builds.mkdir(exist_ok=True)
        (self.stage / 'builds').mkdir(exist_ok=True)
        for directory in (self.stage / 'builds').iterdir():
            destination = builds / directory.name
            if destination.exists():
                raise JobError('Build revision collision')
            os.rename(directory, destination)
            self.published_builds.append(destination)
        requested_bakes = self.request.get('bakes', {})
        self.request['bakes'] = {**scene.get('bake_recipes', {}), **requested_bakes, 'prepare_assets': True}
        self.perform_bakes(scene, path.parent)
        result = self.lower(scene, path.parent)
        if digest(path) != original_fingerprint:
            raise JobError('Scene changed while preparing assets; retry the build')
        # Transfer ownership before publication: cancellation may retain an orphan
        # revision, but can never delete a revision referenced by a committed scene.
        validate_managed_document(scene)
        self.published_builds.clear()
        atomic_json(path, scene)
        return result

    def edit_assets(self):
        path = Path(self.request['scene_path']).resolve(strict=True)
        if path != (self.final / 'scene.json').resolve():
            raise JobError('Asset operation scene does not match project membership')
        scene = load_json(path)
        fingerprint = digest(path)
        if scene.get('edit_overlay'):
            load_json(contained(path.parent, scene['edit_overlay']))
        self.validate_semantics(scene)
        self.assets = scene['assets']
        staging = self.project_root / '.staging'
        staging.mkdir(parents=True, exist_ok=True)
        self.stage = Path(tempfile.mkdtemp(prefix=self.scene_id + '-assets-', dir=staging))
        operation = self.request['operation']
        if operation == 'import_assets':
            sources = [*self.request.get('models', []), *self.request.get('sources', [])]
            if not sources:
                raise JobError('Select at least one asset to import')
            for value in sources:
                source = source_file(value['source'] if isinstance(value, dict) else value)
                suffix = source.suffix.lower()
                if suffix in ('.obj', '.gltf', '.glb'):
                    reference = self.import_model(source)
                    if self.request.get('add_instances', False):
                        scene.setdefault('entities', []).append({'id': str(uuid.uuid4()),
                            'name': source.stem, 'parent': None,
                            'transform': {'pos': [0, 0, 0], 'rot': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                            'mesh': {'asset': reference, 'pipeline_domain': 'world'}})
                elif suffix == '.vkb':
                    self.import_cooked_mesh(source)
                elif suffix in ('.ttf', '.otf'):
                    self.import_font(source)
                elif suffix == '.mt':
                    import_id = str(uuid.uuid4())
                    material = self.import_material(source, self.stage / 'builds' / import_id, import_id, 0)
                    self.artifact('material', source.stem, material, import_id=import_id, source=material)
                elif suffix in ('.png', '.jpg', '.jpeg', '.bmp', '.tga', '.hdr', '.vkt'):
                    imported = self.copy_blob(source, self.stage / 'builds' / str(uuid.uuid4()))
                    self.artifact('texture', source.stem, imported, source=imported)
                else:
                    raise JobError(f'Unsupported asset format: {suffix}')
        else:
            matches = [record for record in self.assets if record.get('id') == self.request.get('asset_id')]
            if len(matches) != 1:
                raise JobError('Select one scene-owned asset to rebuild or reimport')
            record = matches[0]
            if operation == 'rename_asset':
                name = self.request.get('name')
                if not isinstance(name, str) or not name.strip() or len(name.strip().encode('utf-8')) > 512 or any(ord(c) < 32 or ord(c) == 127 for c in name):
                    raise JobError('Asset name must be 1–512 UTF-8 bytes without control characters')
                record['name'] = name.strip()
                result = self.lower(scene, path.parent)
                if digest(path) != fingerprint:
                    raise JobError('Scene changed during rename; retry')
                atomic_json(path, scene)
                return result
            asset_id = record['id']
            import_id = record.get('import_id') or str(uuid.uuid4())
            revision = str(uuid.uuid4())
            bundle = self.stage / 'builds' / revision
            bundle.mkdir(parents=True)
            new_source = self.request.get('source')
            if operation == 'reimport_asset':
                if not new_source:
                    raise JobError('Reimport requires a newly selected source file')
                source = source_file(new_source)
            else:
                if not record.get('source'):
                    raise JobError('Source snapshot unavailable; use Reimport to select a source')
                source = contained(path.parent, record['source'])
            if record['kind'] == 'mesh':
                if operation == 'reimport_asset':
                    source = self.snapshot_model(source, revision)
                    record['source'] = source.relative_to(self.stage).as_posix()
                output = bundle / 'mesh.vkb'
                self.run_tool('mesh', ['--input', source, '--output', output,
                    '--bundle-root', bundle, '--import-id', import_id], 'Rebuilding model')
                existing = {(item['kind'], item.get('source_key', item['name'])): item for item in self.assets
                            if item.get('import_id') == import_id and item['id'] != asset_id}
                previous_count = len(self.assets)
                self.index_bundle(bundle, import_id)
                generated = self.assets[previous_count:]
                del self.assets[previous_count:]
                for item in generated:
                    prior = existing.get((item['kind'], item.get('source_key', item['name'])))
                    if prior is not None:
                        item['id'] = prior['id']
                        prior.pop('closure', None)
                        prior.update(item)
                    else:
                        self.assets.append(item)
            elif record['kind'] == 'font':
                previous = self.assets
                self.assets = []
                try:
                    self.import_font(source)
                    generated = self.assets[0]
                finally:
                    self.assets = previous
                output = self.stage / generated['artifacts'][0]['path']
                record.update(source=generated['source'], recipe=generated.get('recipe', {}))
            elif record['kind'] == 'material':
                output = self.import_material(source, bundle, import_id, 0)
                record['source'] = output.relative_to(self.stage).as_posix()
            elif record['kind'] in ('texture', 'environment'):
                output = self.copy_blob(source, bundle)
                record['source'] = output.relative_to(self.stage).as_posix()
            else:
                raise JobError('Use the scene bake controls to rebuild this generated asset')
            role = (record.get('artifacts') or [{}])[0].get('role', record['kind'])
            record.pop('closure', None)
            record.update(id=asset_id, import_id=import_id, import_revision=revision,
                reimport_status='source_snapshot', artifacts=[{'role': role,
                    'path': output.relative_to(self.stage).as_posix(), 'version': 1}],
                fingerprint='sha256:' + digest(output))
            atomic_json(self.stage / 'imports' / (import_id + '-' + revision + '.json'), {
                'version': 1, 'id': import_id, 'revision': revision,
                'source': record['source'], 'asset': copy.deepcopy(record),
                'dependencies': self.sources.get(revision, {}).get('dependencies', [])})
        scene['assets'] = self.assets
        for folder in ('sources', 'builds', 'imports'):
            incoming = self.stage / folder
            if not incoming.exists():
                continue
            destination_root = path.parent / folder
            destination_root.mkdir(exist_ok=True)
            for source in incoming.iterdir():
                destination = destination_root / source.name
                if destination.exists():
                    raise JobError('Asset revision already exists')
                os.rename(source, destination)
                self.published_builds.append(destination)
        result = self.lower(scene, path.parent)
        if digest(path) != fingerprint:
            raise JobError('Scene changed during import; retry without losing the previous revision')
        # Transfer ownership before publication: cancellation may retain an orphan
        # revision, but can never delete a revision referenced by a committed scene.
        validate_managed_document(scene)
        self.published_builds.clear()
        atomic_json(path, scene)
        return result

    def execute(self):
        try:
            self.ensure_bootstrap()
            if not self.read_only:
                self.prepare_project_font()
            if self.request.get('operation') == 'create_project':
                result = {'version': VERSION, 'status': 'complete', 'fonts': [], 'warnings': self.warnings}
            elif self.request.get('operation') == 'create_scene':
                result = self.create()
            elif self.request.get('operation') == 'bake_scene':
                path = Path(self.request['scene_path']).resolve(strict=True)
                if path != (self.final / 'scene.json').resolve():
                    raise JobError('Bake scene does not match project membership')
                result = self.prepare_unbuilt(path, load_json(path))
            elif self.request.get('operation') == 'prepare_scene':
                result = self.prepare(self.request['scene_path'])
            elif self.request.get('operation') in ('import_assets', 'reimport_asset', 'rebuild_asset', 'rename_asset'):
                result = self.edit_assets()
            else:
                raise JobError('Unknown project job operation')
            if result.get('scene_path') and 'manifest_fingerprint' not in result:
                result['manifest_fingerprint'] = source_fingerprint(Path(result['scene_path']).read_bytes())
            project = self.project_document()
            result['project_assets'] = project.get('assets', [])
            result['default_font'] = project.get('default_font') or {'scope': 'editor', 'id': 'default-scene-font'}
            # Once success may be observable, cleanup must not remove its resources.
            self.final_owned = False
            self.project_builds.clear()
            atomic_json(self.result_path, result)
            atomic_json(self.progress_path, {'version': VERSION, 'status': 'complete',
                                            'stage': 'Ready', 'progress': 1.0})
            self.final_owned = False
            self.project_builds.clear()
            return 0
        except (JobError, OSError, ValueError, KeyError, TypeError) as error:
            status = 'cancelled' if self.cancelled else 'failed'
            atomic_json(self.result_path, {'version': VERSION, 'status': status, 'error': str(error)})
            atomic_json(self.progress_path, {'version': VERSION, 'status': status,
                                            'stage': status, 'progress': 0.0, 'detail': str(error)})
            print(str(error), file=sys.stderr, flush=True)
            return 2 if self.cancelled else 1
        finally:
            if self.stage is not None:
                shutil.rmtree(self.stage)
            if self.final_owned:
                shutil.rmtree(self.final)
            for directory in [*self.published_builds, *self.project_builds]:
                if directory.is_dir():
                    shutil.rmtree(directory)
                else:
                    directory.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--request', required=True)
    parser.add_argument('--result', required=True)
    args = parser.parse_args()
    request = None
    try:
        request = load_json(args.request)
        job = Job(request, args.result)
    except (JobError, OSError, KeyError, ValueError) as error:
        safe_result = True
        if isinstance(request, dict) and request.get('read_only') is True and request.get('workspace_root'):
            safe_result = not Path(args.result).resolve().is_relative_to(Path(request['workspace_root']).resolve())
        if safe_result:
            atomic_json(Path(args.result), {'version': VERSION, 'status': 'failed', 'error': str(error)})
        print(str(error), file=sys.stderr)
        return 1
    signal.signal(signal.SIGTERM, job.cancel)
    signal.signal(signal.SIGINT, job.cancel)
    return job.execute()


if __name__ == '__main__':
    sys.exit(main())
