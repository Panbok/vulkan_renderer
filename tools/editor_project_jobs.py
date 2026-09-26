#!/usr/bin/env python3
"""Prepare portable editor scene transactions and machine-local runtime views.

CLI: --request request.json --result result.json. Request/result version is 1.
Managed documents use v3 scene fields from docs/proposals/editor-projects.md;
v4 moves the scene's asset records into a named immutable inventory revision;
artifacts have {role,path,version}, asset references have {scope,id,role}.
Only the C project store publishes project membership. A failed/cancelled job
removes its own staging tree and never edits the previous scene or project.
"""

import argparse
import base64
import copy
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import time
import uuid
from urllib.parse import unquote, urlsplit, parse_qs

VERSION = 1
MAX_JSON_BYTES = 16 * 1024 * 1024
MAX_MANAGED_DOCUMENT_BYTES = 1024 * 1024
# Scene v4 keeps its asset records in an immutable inventory revision, so the
# scene manifest stays small while large imports keep every record.
MANAGED_SCENE_VERSION = 4
MAX_INVENTORY_BYTES = 16 * 1024 * 1024
MAX_IMPORT_BYTES = 8 * 1024 * 1024 * 1024
MAX_IMPORT_FILES = 16384
FACES = ('r', 'l', 'u', 'd', 'f', 'b')
# bake_diffuse_volume.py status: no closed-room cell, so no volume published.
DIFFUSE_NO_ROOM_CELLS = 3
# Workspace cleanup grace periods. Revisions and staging wait a day so an
# editor still streaming a replaced revision keeps its files; unlisted scene
# and project directories wait an hour past any in-flight creation.
UNREFERENCED_GRACE_SECONDS = 24 * 60 * 60
ORPHAN_GRACE_SECONDS = 60 * 60
JOB_RETENTION_SECONDS = 7 * 24 * 60 * 60
CLEANUP_OPERATIONS = ('create_project', 'create_scene', 'bake_scene', 'delete_scene', 'delete_project',
                      'add_entities',
                      'import_assets', 'reimport_asset', 'rebuild_asset', 'rename_asset')


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


def validate_managed_document(value, limit=MAX_MANAGED_DOCUMENT_BYTES):
    """Match the project store's durable document limits before publication."""
    encoded = (json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + '\n').encode('utf-8')
    if len(encoded) > limit:
        raise JobError(f'Managed document exceeds the {limit // (1024 * 1024)} MiB project-store limit; '
                       'split the scene into smaller scenes')
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
    if path.name == 'scene.json' and value.get('version') in (3, MANAGED_SCENE_VERSION):
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


def read_managed_scene(path):
    """Loads a managed scene with its asset records inline, as jobs edit it.
    Version 3 stores them in scene.json; version 4 names an inventory revision."""
    path = Path(path)
    scene = load_json(path, MAX_MANAGED_DOCUMENT_BYTES)
    if scene.get('version') != MANAGED_SCENE_VERSION:
        return scene
    reference = scene.pop('inventory', None)
    if not isinstance(reference, str) or 'assets' in scene:
        raise JobError('Managed scene needs exactly one inventory reference')
    inventory = load_json(contained(path.parent, reference), MAX_INVENTORY_BYTES)
    if inventory.get('version') != 1 or not isinstance(inventory.get('assets'), list):
        raise JobError('Managed scene inventory is invalid')
    scene['assets'] = inventory['assets']
    return scene


def validate_managed_scene(scene, reference='inventory/' + '0' * 36 + '.json'):
    """Splits an in-memory scene into its v4 manifest and inventory revision,
    checking each against its own durable limit."""
    document = {key: value for key, value in scene.items() if key != 'assets'}
    document.update(version=MANAGED_SCENE_VERSION, inventory=reference)
    inventory = {'version': 1, 'assets': scene.get('assets', [])}
    validate_managed_document(document)
    validate_managed_document(inventory, MAX_INVENTORY_BYTES)
    return document, inventory


def write_managed_scene(path, scene):
    """Publishes an immutable inventory revision, then atomically points
    scene.json at it; a failure before the replacement leaves the previous
    scene and its inventory intact."""
    path = Path(path)
    reference = 'inventory/' + str(uuid.uuid4()) + '.json'
    document, inventory = validate_managed_scene(scene, reference)
    atomic_json(path.parent / reference, inventory)
    atomic_json(path, document)
    scene['version'] = MANAGED_SCENE_VERSION


def document_references(value, digests, revisions):
    """Collects content digests and owner-relative build revisions that a
    managed document names anywhere in its structure."""
    if isinstance(value, dict):
        for child in value.values():
            document_references(child, digests, revisions)
    elif isinstance(value, list):
        for child in value:
            document_references(child, digests, revisions)
    elif isinstance(value, str):
        text = value.removeprefix('sha256:')
        if len(text) == 64 and all(c in '0123456789abcdef' for c in text):
            digests.add(text)
        parts = value.removeprefix('./').split('/')
        if len(parts) > 1 and parts[0] == 'builds':
            revisions.add(parts[1])


def remove_tree(path):
    """Removes a directory or file without following links; returns bytes freed."""
    if path.is_symlink() or not path.exists():
        path.unlink(missing_ok=True)
        return 0
    if path.is_file():
        size = path.stat().st_size
        path.unlink()
        return size
    size = sum(child.stat().st_size for child in path.rglob('*') if child.is_file() and not child.is_symlink())
    shutil.rmtree(path)
    return size


def older_than(path, seconds, now):
    return now - path.lstat().st_mtime > seconds


def publish_directory(source, destination):
    """Publish a prepared directory without replacing another writer's entry."""
    if os.name == 'nt':
        os.rename(source, destination)
        return
    # Reserve an empty destination exclusively; POSIX rename may otherwise
    # replace a directory created by a concurrent writer.
    destination.mkdir()
    try:
        os.replace(source, destination)
    except BaseException:
        destination.rmdir()
        raise


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


def validate_managed_path(value):
    """Check serialized segments before Path can erase empty or dot segments."""
    if isinstance(value, str) and '..' in value.split('/'):
        raise JobError(f'Managed path escapes its owner: {value}')
    if (not isinstance(value, str) or not value or
            any(character in value for character in ('\\', ':', '\x00')) or
            any(part in ('', '.', '..') for part in value.split('/'))):
        raise JobError(f'Invalid managed path: {value!r}')
    return value


def managed_reference(path, owner):
    """Serialize a host path at the managed-document boundary."""
    try:
        value = Path(path).relative_to(owner).as_posix()
    except ValueError as error:
        raise JobError(f'Managed path {path} is outside owner {owner}') from error
    return validate_managed_path(value)


def migrate_source_references(document, label):
    """Repair only historical source fields, never arbitrary document strings."""
    def source(record, field):
        value = record.get('source')
        if value is not None:
            try:
                record['source'] = validate_managed_path(value.replace('\\', '/') if isinstance(value, str) else value)
            except JobError as error:
                raise JobError(f'{label}: {field}.source: {error}') from error
    for index, record in enumerate(document.get('assets', [])):
        source(record, f'assets[{index}]')
    # Import records carry both the snapshot source and an optional asset copy.
    if document.get('version') == 1 and 'id' in document and ('dependencies' in document or 'material_remaps' in document):
        source(document, 'import')
        if isinstance(document.get('asset'), dict):
            source(document['asset'], 'asset')
        for index, record in enumerate(document.get('artifacts', [])):
            source(record, f'artifacts[{index}]')
    return document


def contained(root, value, must_exist=True):
    root = Path(root).resolve()
    validate_managed_path(value)
    try:
        result = (root / value).resolve(strict=must_exist)
    except OSError as error:
        raise JobError(f'Managed path open failed: {value!r}, owner {root}: {error}') from error
    if not result.is_relative_to(root):
        raise JobError(f'Managed path escapes its owner: {value}')
    return result


def model_tokens(line):
    """OBJ/MTL whitespace, quotes and comments; backslashes are filename bytes."""
    tokens = []
    token = []
    quote = None
    for character in line:
        if quote:
            if character == quote:
                quote = None
            else:
                token.append(character)
        elif character in ('"', "'") and not token:
            quote = character
        elif character == '#':
            break
        elif character.isspace():
            if token:
                tokens.append(''.join(token))
                token = []
        else:
            token.append(character)
    if quote:
        raise JobError('Unterminated quote in OBJ/MTL filename')
    if token:
        tokens.append(''.join(token))
    return tokens


def gltf_uri_path(value):
    uri = urlsplit(value)
    if uri.scheme or uri.netloc:
        raise JobError('Remote or absolute URI dependencies are unsupported; download them before importing')
    if uri.query or uri.fragment:
        raise JobError(f'glTF dependency URI has unsupported query or fragment: {value}')
    try:
        return unquote(uri.path, encoding='utf-8', errors='strict')
    except UnicodeError as error:
        raise JobError(f'Invalid UTF-8 glTF dependency URI: {value}') from error


def source_file(value):
    try:
        path = Path(value).expanduser()
        if os.name == 'nt':
            native = str(path).replace('/', '\\')
            extended = native.startswith('\\\\?\\')
            filesystem_extended = (native[4:8].upper() == 'UNC\\' or
                                   (len(native) >= 7 and native[4].isalpha() and native[5:7] == ':\\'))
            if (path.drive and not path.root) or native.startswith('\\\\.\\') or (extended and not filesystem_extended):
                raise ValueError('Drive-relative and device paths are unsupported')
        path = path.resolve(strict=True)
    except (TypeError, ValueError, OSError) as error:
        raise JobError(f'Source file is unavailable: {value}: {error}') from error
    if not path.is_file():
        raise JobError(f'Source is not a regular file: {path}')
    with path.open('rb') as stream:
        if stream.read(128).startswith(b'version https://git-lfs.github.com/spec/v1\n'):
            raise JobError(f'Source is a Git LFS pointer, not asset data: {path}. '
                           'Run git lfs pull in the source repository, then import again.')
    return path


def legacy_source(value, origin, legacy_root):
    if not isinstance(value, str) or not value or '\x00' in value:
        raise JobError('Missing legacy asset path')
    path = Path(value.split('?', 1)[0])
    candidates = [path] if path.is_absolute() else [origin / path, legacy_root / path]
    if path.suffix.lower() in ('.png', '.jpg', '.jpeg', '.bmp', '.tga'):
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


def gltf_texture_source(value, origin, legacy_root):
    """Locates a glTF image the way the mesh cooker does for repository models
    (tools/assets/mesh_loader_gltf.c): beside the model first, then, for a model
    inside the repository's assets tree, under assets and assets/textures, again
    under assets/textures without a legacy `objects/` prefix, and by file name
    in assets/textures. A cooked `.vkt` stands in for a missing source at each
    place. Models elsewhere resolve only beside themselves."""
    direct = origin / value
    candidates = [direct]
    assets = legacy_root / 'assets'
    try:
        repository_model = origin.resolve().is_relative_to(assets.resolve())
    except OSError:
        repository_model = False
    if repository_model:
        textures = assets / 'textures'
        candidates += [assets / value, textures / value]
        if value.startswith('objects/') and len(value) > len('objects/'):
            candidates.append(textures / value[len('objects/'):])
        candidates.append(textures / Path(value).name)
    for candidate in candidates:
        for path in (candidate, Path(str(candidate) + '.vkt')):
            if path.is_file():
                return path
    return direct


_CLONEFILE = None


def clone_file(source, destination):
    """Clones `source` copy-on-write where the file system can (APFS on macOS):
    the clone shares the source's blocks until either file changes, so a large
    closure imported from the same volume costs almost no space. Returns False
    when the caller must copy the bytes instead."""
    global _CLONEFILE
    if _CLONEFILE is None:
        _CLONEFILE = False
        if sys.platform == 'darwin':
            try:
                function = ctypes.CDLL('/usr/lib/libSystem.B.dylib', use_errno=True).clonefile
                function.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
                function.restype = ctypes.c_int
                _CLONEFILE = function
            except (OSError, AttributeError):
                pass
    if not _CLONEFILE or _CLONEFILE(os.fsencode(source), os.fsencode(destination), 0) != 0:
        return False
    # A clone keeps the source's mode; a copy is writable by its owner.
    os.chmod(destination, stat.S_IMODE(os.stat(destination).st_mode) | stat.S_IWUSR)
    return True


def relative_reference(path, owner):
    return './' + managed_reference(path, owner)


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
        if self.scene_id is None and request.get('operation') not in ('create_project', 'delete_project'):
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
        self.collision_inspections = set()
        self.texture_seeds = {}
        self.texture_tool_hash = None
        # Content-addressed derived textures shared by every import in the
        # workspace; bundles hold clones of the variants their materials use.
        self.generated_root = self.workspace / 'cache' / 'generated'
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
                    files.append({'path': managed_reference(destination, staging), 'sha256': digest(destination)})
            assets = []
            for config in sorted((staging / 'fonts').glob('*.fontcfg')):
                config_type = next((line.partition('=')[2].strip() for line in config.read_text(encoding='utf-8').splitlines()
                                    if line.partition('=')[0].strip() == 'type'), '')
                if config_type != 'cooked_mtsdf':
                    continue
                self.validate_bundle_dependencies(staging, config, set())
                # Phosphor atlases draw editor icons; they are not scene fonts.
                if config.name.startswith('Phosphor'):
                    continue
                asset_id = 'default-scene-font' if config.name == 'UbuntuMono-cooked.fontcfg' else config.stem
                assets.append({'id': asset_id, 'kind': 'font', 'name': config.stem,
                    'artifacts': [{'role': 'font', 'path': managed_reference(config, staging), 'version': 1}],
                    'fingerprint': 'sha256:' + digest(config)})
            atomic_json(staging / 'manifest.json', {'version': 1, 'id': 'vkr-editor-bundle-1',
                                                  'assets': assets, 'files': files})
            if bundle.exists():
                raise JobError('An incomplete editor bundle exists; repair it before opening the workspace')
            publish_directory(staging, bundle)
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
                record['closure'] = {managed_reference(path, temporary): digest(path) for path in closure}
            builds = self.project_root / 'builds'
            builds.mkdir(exist_ok=True)
            for directory in (temporary / 'builds').iterdir():
                destination = builds / directory.name
                if destination.exists():
                    raise JobError('Project font build revision collision')
                publish_directory(directory, destination)
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

    def run_tool(self, tool, arguments, label, accepted=()):
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
        if code and code not in accepted:
            raise JobError(f'{label} failed (exit {code}); see the job log')
        return code

    def copy_file(self, source, destination):
        source = source_file(source)
        size = source.stat().st_size
        if self.files_copied >= MAX_IMPORT_FILES or self.bytes_copied + size > MAX_IMPORT_BYTES:
            raise JobError('Import exceeds 16,384 files or 8 GiB; split the import')
        before = digest(source)
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not clone_file(source, destination):
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
                 'import_id': import_id, 'source': managed_reference(source, self.stage) if source else None,
                 'artifacts': [{'role': role or kind, 'path': managed_reference(path, self.stage),
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

        def dependency(value, origin, texture=False):
            located = (gltf_texture_source(value, origin, self.legacy_root)
                       if texture else origin / value)
            resolved = source_file(located)
            copied = self.copy_blob(resolved, directory / 'dependencies')
            dependencies.append({'path': managed_reference(copied, directory),
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
                        copied = dependency(gltf_uri_path(uri), source.parent,
                                            texture=field == 'images')
                        item['uri'] = managed_reference(copied, directory)
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
                tokens = model_tokens(line)
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
                        fields = model_tokens(material_line)
                        if fields and fields[0] == 'newmtl':
                            material_names.append(' '.join(fields[1:]))
                        if fields and (fields[0].startswith('map_') or fields[0] in ('bump', 'disp', 'decal', 'norm')):
                            if len(fields) != 2:
                                raise JobError(f'MTL texture options need explicit conversion: {fields[0]}')
                            copied = dependency(fields[1], mtl_source.parent)
                            if fields[0] not in ('map_Kd', 'map_Ks', 'bump', 'map_bump'):
                                self.warnings.append(f'OBJ channel {fields[0]} is retained in the source snapshot but is not rendered')
                            material_line = fields[0] + ' ' + managed_reference(copied, directory)
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
            'source': managed_reference(destination, self.stage),
            'original_sha256': original_digest, 'material_names': material_names,
            'dependencies': dependencies, 'reimport_status': 'source_snapshot',
            'unsupported_features': list(self.warnings)}
        return destination

    def cook_model_animation(self, record, source, mesh):
        """Publish mesh and compatible bank in one immutable asset revision."""
        info = self.inspect_mesh(mesh)
        if not info.get('skin_count') or not info.get('animation_count'):
            return
        bank = Path(mesh).parent / 'animation.vka'
        self.run_tool('animation', ['--input', source, '--output', bank], 'Cooking animations')
        if not bank.is_file():
            raise JobError('Animation cooker did not publish its bank')
        record['artifacts'].append({'role': 'animation',
            'path': managed_reference(bank, self.stage), 'version': 1,
            'fingerprint': 'sha256:' + digest(bank)})
        record['animation_count'] = info['animation_count']

    @staticmethod
    def bind_model_animations(scene, inventories):
        for entity in scene.get('entities', []):
            reference = entity.get('mesh', {}).get('asset')
            if not isinstance(reference, dict):
                continue
            _, records = inventories.get(reference.get('scope'), (None, []))
            record = next((item for item in records if item.get('id') == reference.get('id')), None)
            if record is None:
                continue
            bank = {**reference, 'role': 'animation'}
            products = [item for item in record.get('artifacts', []) if item.get('role') == 'animation']
            animation = entity.get('animation')
            if products and animation is None:
                entity['animation'] = {'asset': bank}
            elif not products and isinstance(animation, dict) and animation.get('asset') == bank:
                # A reimport can legitimately replace an animated model with a static one.
                entity.pop('animation')

    def import_model(self, source):
        import_id = str(uuid.uuid4())
        snapshot = self.snapshot_model(source, import_id)
        bundle = self.stage / 'builds' / import_id
        bundle.mkdir(parents=True)
        mesh = bundle / 'mesh.vkb'
        if self.request.get('bakes', {}).get('prepare_assets') is False:
            asset_id = str(uuid.uuid4())
            self.assets.append({'id': asset_id, 'kind': 'mesh', 'name': Path(source).stem,
                'import_id': import_id, 'source': managed_reference(snapshot, self.stage),
                'artifacts': [], 'recipe': {'tool': 'mesh', 'version': 1},
                'fingerprint': 'sha256:' + digest(snapshot)})
            atomic_json(self.stage / 'imports' / (import_id + '.json'), self.sources[import_id])
            return {'scope': 'scene', 'id': asset_id, 'role': 'mesh'}
        self.generated_root.mkdir(parents=True, exist_ok=True)
        self.run_tool('mesh', ['--input', snapshot, '--output', mesh,
                              '--bundle-root', bundle, '--import-id', import_id,
                              '--generated-root', self.generated_root], 'Cooking model')
        reference = self.artifact('mesh', Path(source).stem, mesh, import_id=import_id, source=snapshot)
        self.cook_model_animation(self.assets[-1], snapshot, mesh)
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
                # The key names the inputs only. The packer owns its encoding policy:
                # a rebuilt packer revalidates a cached file instead of recooking it.
                recipe = {'version': 2, 'source': source_hash, 'class': texture_class,
                          'shape': '2d', 'strict': True}
                cache_key = hashlib.sha256(json.dumps(recipe, sort_keys=True).encode()).hexdigest()
                cache = self.workspace / 'cache' / 'textures' / cache_key
                cache.mkdir(parents=True, exist_ok=True)
                cached = cache / 'texture.vkt'
                manifest = cache / 'manifest.json'
                recorded = load_json(manifest) if cached.is_file() and manifest.is_file() else {}
                intact = recorded.get('recipe') == recipe and recorded.get('sha256') == digest(cached)
                if not intact or recorded.get('tool') != self.texture_tool_hash:
                    temporary = cache / (str(uuid.uuid4()) + '.vkt')
                    try:
                        seed = cached if intact else self.texture_seeds.get(source_hash)
                        if seed and seed.is_file():
                            self.copy_file(seed, temporary)
                        # The packer independently validates a seed's source hash,
                        # class and full recipe; a mismatch forces a fresh cook.
                        self.run_tool('texture', ['--output', temporary, '--type', '2d', '--layer', source,
                            '--texture-class', texture_class, '--strict', '--no-progress'], 'Preparing texture')
                        if not temporary.is_file():
                            raise JobError('Texture packer did not publish its artifact')
                        os.replace(temporary, cached)
                        atomic_json(manifest, {'version': 1, 'recipe': recipe, 'sha256': digest(cached),
                                               'tool': self.texture_tool_hash})
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
                'import_id': import_id, 'source': managed_reference(copied, self.stage),
                'artifacts': [], 'recipe': {'tool': 'font', 'version': 1,
                    'config': managed_reference(config, self.stage)},
                'fingerprint': 'sha256:' + digest(copied)})
            return {'scope': 'scene', 'id': asset_id, 'role': 'font'}
        self.run_tool('font', ['--config', config], 'Cooking scene font')
        if not (bundle / 'font.vkfa').is_file():
            raise JobError('Font cooker did not publish its artifact')
        return self.artifact('font', source.stem, config, import_id=import_id, source=copied)

    def import_cubemap(self, value, origin):
        """Imports a reflection-probe cube; image skies were replaced by the atmosphere."""
        if isinstance(value, dict) and value.get('path'):
            if value.get('base_path') or value.get('extension'):
                raise JobError('Cubemap mixes a packed path and face paths')
            return self.import_cubemap(value['path'], origin)
        if isinstance(value, str):
            source = legacy_source(value, origin, self.legacy_root)
            copied = self.copy_blob(source, self.stage / 'builds' / 'environments')
            return self.artifact('environment', source.stem, copied, source_kind='cubemap')
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
                                      source_kind='faces', base_path=managed_reference(directory / 'cube', self.stage),
                                      extension=output_extension)
            return reference
        raise JobError('Unsupported cubemap asset')

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

    @staticmethod
    def checked_overlay(overlay):
        if (not isinstance(overlay, dict) or type(overlay.get('version')) is not int or
                overlay['version'] not in (1, 2, 3) or not isinstance(overlay.get('overrides'), list)):
            raise JobError('Unsupported authored override journal')
        if any(not isinstance(record, dict) for record in overlay['overrides']):
            raise JobError('Invalid authored override record')
        return overlay

    @staticmethod
    def overlay_references(record):
        yield record, 'source_fingerprint', True
        physics = record.get('physics')
        if physics is None:
            return
        if not isinstance(physics, dict):
            raise JobError('Invalid physics override')
        attachment = physics.get('attachment')
        if attachment is not None:
            if not isinstance(attachment, dict) or not isinstance(attachment.get('source'), dict):
                raise JobError('Invalid physics attachment source')
            yield attachment['source'], 'fingerprint', attachment.get('enabled', False)
        joints = physics.get('joints', [])
        if not isinstance(joints, list) or len(joints) > 16:
            raise JobError('Invalid physics joint list')
        for joint in joints:
            if not isinstance(joint, dict) or not isinstance(joint.get('target'), dict):
                raise JobError('Invalid physics joint target')
            yield joint['target'], 'fingerprint', joint.get('enabled', False)

    @staticmethod
    def overlay_colliders(overlay):
        for record in overlay['overrides']:
            physics = record.get('physics')
            if physics is None:
                continue
            if not isinstance(physics, dict) or not isinstance(physics.get('colliders', []), list):
                raise JobError('Invalid physics collider list')
            colliders = physics.get('colliders', [])
            if len(colliders) > 32:
                raise JobError('Physics body exceeds collider capacity')
            for collider in colliders:
                if not isinstance(collider, dict) or not isinstance(collider.get('asset', ''), str):
                    raise JobError('Invalid collision asset reference')
                value = collider.get('asset', '')
                if collider.get('shape') in (3, 4) and not value:
                    raise JobError('Geometry collider requires a cooked collision asset')
                if value:
                    yield collider

    def overlay_identity(self, reference, seed, scene, root):
        index, node = reference.get('scene_entity'), reference.get('gltf_node')
        entities = scene.get('entities', [])
        if (type(index) is not int or not 0 <= index < len(entities) or
                type(node) is not int or node < -1):
            raise JobError('Authored override references a missing source entity')
        mesh = entities[index].get('mesh', {})
        asset_reference = mesh.get('asset')
        mesh_path = mesh.get('path')
        if asset_reference:
            if asset_reference.get('scope') != 'scene':
                raise JobError('Import the project mesh locally before cloning authored source-node edits')
            inventory = scene.get('assets', self.assets)
            asset = next((item for item in inventory if item['id'] == asset_reference['id']), None)
            if not asset or not asset.get('artifacts'):
                raise JobError('Authored source-node edits require a built mesh')
            mesh_path = contained(root, asset['artifacts'][0]['path'])
        if mesh_path:
            info = self.inspect_mesh(mesh_path)
            if node != -1 and node not in {entry['index'] for entry in info['nodes']}:
                raise JobError('Authored override references a missing cooked source node')
            if info['nodes']:
                return self.mesh_identity(seed, info['fingerprint'])
        elif node != -1:
            raise JobError('Authored source-node edit targets an entity without a mesh')
        return seed

    def inspect_collision(self, path):
        path = source_file(path)
        if path.suffix.lower() != '.vkc' or not 48 <= path.stat().st_size <= 64 * 1024 * 1024:
            raise JobError('Collision dependency must be a bounded cooked .vkc file')
        fingerprint = digest(path)
        key = (path, fingerprint)
        if key not in self.collision_inspections:
            self.run_tool('collision', ['--inspect', '--input', path], 'Validating cooked collision')
            if digest(path) != fingerprint:
                raise JobError('Collision dependency changed during validation')
            self.collision_inspections.add(key)
        return path

    def import_overlay_collision(self, value, source_root, source_scene):
        if Path(value).is_absolute():
            return source_file(value)
        if source_scene is None:
            # Legacy runtime resolves physics assets against PROJECT_SOURCE_DIR.
            return source_file(self.legacy_root / value)
        validate_managed_path(value)
        parts = Path(value).parts
        # A detached scene bundle still carries its original workspace-relative
        # references. Its own closure can be resolved without the old workspace.
        if (len(parts) > 4 and parts[0] == 'projects' and parts[2] == 'scenes' and
                parts[3] == source_scene['id']):
            return source_file(contained(source_root, '/'.join(parts[4:])))
        if source_root.parent.name != 'scenes' or source_root.parent.parent.parent.name != 'projects':
            raise JobError('External collision dependency requires its original managed workspace')
        source_workspace = source_root.parent.parent.parent.parent
        return source_file(contained(source_workspace, value))

    def remap_overlay(self, path, expected, scene, source_root=None, managed=False):
        overlay = self.checked_overlay(load_json(path))
        seen = set()
        for record in overlay['overrides']:
            key = (record.get('scene_entity'), record.get('gltf_node'))
            if any(type(value) is not int for value in key):
                raise JobError('Invalid authored override source identity')
            if key in seen:
                raise JobError('Duplicate authored override source identity')
            seen.add(key)
            for reference, field, required in self.overlay_references(record):
                if not required and reference.get(field) == '0000000000000000':
                    continue
                old_hash = self.overlay_identity(reference, expected, scene, self.stage)
                if reference.get(field) != old_hash:
                    raise JobError('Authored overrides conflict with the selected source scene')
                reference[field] = self.overlay_identity(
                    reference, source_fingerprint(self.scene_id.encode()), scene, self.stage)
        for collider in self.overlay_colliders(overlay):
            source = self.import_overlay_collision(collider['asset'], source_root,
                                                   scene if managed else None)
            self.inspect_collision(source)
            copied = self.copy_blob(source, self.stage / 'builds' / 'collisions')
            final_path = self.final / copied.relative_to(self.stage)
            collider['asset'] = managed_reference(final_path, self.workspace)
        atomic_json(path, overlay)

    def validate_overlay_dependencies(self, overlay, root):
        for collider in self.overlay_colliders(overlay):
            value = collider['asset']
            validate_managed_path(value)
            final_reference = managed_reference(self.final, self.workspace)
            if value.startswith(final_reference + '/'):
                path = contained(root, value[len(final_reference) + 1:])
            else:
                path = contained(self.workspace, value)
            self.inspect_collision(path)

    def import_scene(self, source):
        source = source_file(source)
        scene = load_json(source)
        if scene.get('version') in (3, MANAGED_SCENE_VERSION):
            return self.import_managed_scene(read_managed_scene(source), source.parent)
        if scene.get('version', 1) not in (1, 2):
            raise JobError('Unsupported scene JSON version')
        scene = copy.deepcopy(scene)
        environment = scene.get('environment')
        if isinstance(environment, dict):
            for field in ('equirect', 'cubemap'):
                if field in environment:
                    environment.pop(field)
                    self.warnings.append(f'Dropped the removed environment {field} sky image; '
                                         'author atmosphere or environment.constant instead')
        for probe in scene.get('reflection_probes', []):
            if probe.get('cubemap'):
                reference = self.import_cubemap(probe.pop('cubemap'), source.parent)
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
            self.remap_overlay(copied, expected, scene, source.parent)
            scene['edit_overlay'] = managed_reference(copied, self.stage)
        scene.pop('source_identity', None)
        return scene

    def import_managed_scene(self, scene, origin):
        scene = migrate_source_references(copy.deepcopy(scene), origin / 'scene.json')
        if any(a.get('scope') not in (None, 'scene') for a in scene.get('assets', [])):
            raise JobError('Managed scene import has an invalid asset inventory')
        # A managed scene bundle is the dependency closure. Copy only this owner;
        # project/editor references are resolved explicitly below, never by basename.
        for child in origin.iterdir():
            if child.name in ('.runtime', 'scene.json', 'inventory'):
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
        for import_path in (self.stage / 'imports').glob('*.json'):
            record = load_json(import_path)
            migrated = migrate_source_references(copy.deepcopy(record), import_path)
            if migrated != record:
                atomic_json(import_path, migrated)
        self.assets = scene.get('assets', [])
        if scene.get('edit_overlay'):
            self.remap_overlay(contained(self.stage, scene['edit_overlay'], must_exist=False),
                               source_fingerprint(identifier(scene['id']).encode()), scene, origin, managed=True)
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
                        product['path'] = managed_reference(target / old_path.relative_to(bundle_root), self.stage)
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

    def append_entities(self, scene):
        models = self.request.get('models', [])
        lights = self.request.get('lights', [])
        if not isinstance(models, list) or not isinstance(lights, list):
            raise JobError('Models and lights must be arrays')
        first = len(scene.setdefault('entities', []))
        if first + len(models) + len(lights) > 65536:
            raise JobError('Adding entities exceeds the scene entity limit')
        for model in models:
            source = source_file(model['source'] if isinstance(model, dict) else model)
            if source.suffix.lower() == '.vkb':
                reference = self.import_cooked_mesh(source)
            elif source.suffix.lower() in ('.obj', '.gltf', '.glb'):
                reference = self.import_model(source)
            else:
                raise JobError('Models must be OBJ, glTF, GLB or cooked VKB files')
            entity = {'id': str(uuid.uuid4()), 'name': source.stem, 'parent': None,
                      'transform': {'pos': [0, 0, 0], 'rot': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                      'mesh': {'asset': reference, 'pipeline_domain': 'world'}}
            if isinstance(model, dict) and model.get('transform'):
                entity['transform'] = copy.deepcopy(model['transform'])
            scene['entities'].append(entity)
        for light in lights:
            kinds = ('point_light', 'spot_light', 'directional_light', 'rectangle_light')
            if not isinstance(light, dict) or sum(kind in light for kind in kinds) != 1:
                raise JobError('Each light requires exactly one light component')
            if any(component in light for component in ('mesh', 'shape', 'text3d')):
                raise JobError('New light entities cannot contain model, shape or text components')
            entity = copy.deepcopy(light)
            if 'spot_light' in entity:
                entity['point_light'] = {**entity.pop('spot_light'), 'kind': 2}
            entity['id'] = str(uuid.uuid4())
            scene['entities'].append(entity)
        return first

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
        self.append_entities(scene)
        # The physical sky supplies the sky, sun and global IBL; the environment
        # block keeps only its sky-light scales. An imported scene keeps its
        # authored sun, medium and cloud layer; a new physical sky starts with
        # the default cloud layer.
        atmosphere = self.request.get('atmosphere') or {}
        if atmosphere.get('enabled'):
            environment = self.request.get('environment') or {}
            scene['atmosphere'] = {**(scene.get('atmosphere') or {}), 'enabled': True}
            scene.setdefault('clouds', {'enabled': True})
            scene['environment'] = {key: environment[key] for key in
                ('enabled', 'intensity', 'diffuse_intensity', 'specular_intensity') if key in environment}
            scene['environment'].setdefault('enabled', True)
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
        scene.update(version=MANAGED_SCENE_VERSION, id=self.scene_id, assets=self.assets)
        scene.setdefault('default_font', None)
        scene.setdefault('edit_overlay', None)
        scene.setdefault('bake_recipes', {})
        self.validate_semantics(scene)
        self.perform_bakes(scene)
        validate_managed_scene(scene)
        self.progress('Validating scene dependencies', 0.85)
        self.validate_semantics(scene)
        pending_bakes = scene['bake_recipes'].get('prepare_assets') is False and (scene['bake_recipes'].get('reflection') or scene['bake_recipes'].get('diffuse'))
        unbuilt = any(not record.get('artifacts') for record in self.assets) or pending_bakes
        if not unbuilt:
            self.lower(scene, self.stage, publish_runtime=False)
        write_managed_scene(self.stage / 'scene.json', scene)
        self.final.parent.mkdir(parents=True, exist_ok=True)
        if self.final.exists():
            raise JobError('Scene was created by another writer')
        publish_directory(self.stage, self.final)
        self.final_owned = True
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
        if any(key in environment for key in ('asset', 'equirect', 'cubemap')):
            raise JobError('Scene environment images were removed; author atmosphere or environment.constant')
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

    def run_script(self, script, arguments, label, accepted=()):
        executable = self.tools.get('python') or sys.executable
        # Keep all child handling in run_tool, including cancellation/reaping.
        original = self.tools.get('_script_python')
        self.tools['_script_python'] = executable
        try:
            return self.run_tool('_script_python', [Path(__file__).resolve().parent / script, *arguments],
                                 label, accepted)
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
        journal = self.checked_overlay(load_json(contained(root, overlay)))
        runtime = load_json(result['runtime_path'])
        entities = runtime.get('entities', [])
        wrapper_edits, node_edits = {}, {}
        for edit in journal['overrides']:
            for reference, field, required in self.overlay_references(edit):
                if not required and reference.get(field) == '0000000000000000':
                    continue
                expected = self.overlay_identity(reference, source_fingerprint(self.scene_id.encode()), runtime, root)
                if reference.get(field) != expected:
                    raise JobError('Authored physics/source identity conflicts with scene')
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
                    sun_angular_diameter_degrees=edit.get('directional_sun_angular_diameter_degrees', [0.53])[0],
                    temperature_kelvin=edit.get('directional_temperature_kelvin', [0.0])[0],
                    atmosphere_sun=edit.get('directional_atmosphere_sun', True))
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
                    'path': managed_reference(destination, asset_root), 'version': 1}]
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
            code = self.run_script('bake_diffuse_volume.py', arguments, 'Baking diffuse volume',
                                   (DIFFUSE_NO_ROOM_CELLS,))
            previous_ref = scene.get('diffuse_volume', {}).get('asset', {})
            previous = next((item for item in self.assets if item['id'] == previous_ref.get('id')), None)
            if code == DIFFUSE_NO_ROOM_CELLS:
                # An all-invalid volume renders like no volume (ADR-054). A
                # previous volume describes other geometry, so it is dropped.
                destination.parent.rmdir()
                if previous is not None:
                    self.assets.remove(previous)
                scene.pop('diffuse_volume', None)
                warning = ('Diffuse volume skipped: no interpolation cell lies inside a closed room, '
                           'so the scene keeps environment and reflection-probe diffuse lighting')
                self.warnings.append(warning)
                print(f'Warning: {warning}', flush=True)
            else:
                if not destination.is_file():
                    raise JobError('Diffuse baker did not publish its artifact')
                record = {'id': str(uuid.uuid4()), 'kind': 'volume', 'name': 'Diffuse volume',
                    'import_id': revision, 'source': None, 'artifacts': [{'role': 'volume',
                        'path': managed_reference(destination, asset_root), 'version': 1}],
                    'fingerprint': 'sha256:' + digest(destination),
                    'recipe': {'tool': 'diffuse', 'version': 1, 'settings': settings}}
                if previous is not None:
                    record['id'] = previous['id']
                    self.assets.remove(previous)
                self.assets.append(record)
                scene['diffuse_volume'] = {'asset': {'scope': 'scene', 'id': record['id'], 'role': 'volume'}}
            scene['assets'] = self.assets

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
        if scene.get('version') not in (3, MANAGED_SCENE_VERSION) or scene.get('id') != self.scene_id:
            raise JobError('Managed scene version or ID mismatch')
        project = self.project_document()
        inventories = {'scene': (root, scene.get('assets', [])),
                       'project': (self.project_root, project.get('assets', []))}
        editor_manifest = self.workspace / 'editor' / 'bundles' / '1' / 'manifest.json'
        if not editor_manifest.is_file():
            editor_manifest = self.workspace / 'editor' / 'bundle.json'
        if editor_manifest.is_file():
            inventories['editor'] = (editor_manifest.parent, load_json(editor_manifest).get('assets', []))
        self.bind_model_animations(scene, inventories)
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
                current = {managed_reference(item, owner): digest(item) for item in record_paths}
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

        def cube(component, default_role):
            if 'asset' not in component:
                if component.get('enabled'):
                    raise JobError('Enabled probe has no cubemap asset; bake it or disable the probe')
                if 'cubemap' in component:
                    raise JobError('Managed probe still contains a legacy path')
                return
            if 'cubemap' in component:
                raise JobError('Managed probe mixes asset and legacy path fields')
            path, record, owner = asset(component.pop('asset'), default_role)
            kind = record.get('source_kind', 'cubemap')
            if kind == 'faces':
                base = contained(owner, record['base_path'], must_exist=False)
                extension = record['extension']
                for face in FACES:
                    contained(owner, managed_reference(base, owner) + '_' + face + '.' + extension)
                component['cubemap'] = {'base_path': str(base), 'extension': extension}
            else:
                component['cubemap'] = {'path': str(path)}

        runtime = copy.deepcopy(scene)
        runtime['version'] = 2
        runtime['source_identity'] = self.scene_id
        for field in ('id', 'assets', 'default_font', 'bake_recipes', 'edit_overlay'):
            runtime.pop(field, None)
        for probe in runtime.get('reflection_probes', []):
            cube(probe, 'probe-cube')
        for volume in ([runtime['diffuse_volume']] if runtime.get('diffuse_volume') else []):
            if 'path' in volume:
                raise JobError('Managed volume contains a legacy path')
            if 'asset' in volume:
                volume['path'] = str(asset(volume.pop('asset'), 'volume')[0])
        for entity in runtime.get('entities', []):
            entity.pop('id', None)
            for component, role in ((entity.get('mesh'), 'mesh'), (entity.get('animation'), 'animation'),
                                    (entity.get('shape', {}).get('material'), 'material')):
                if not component:
                    continue
                if 'path' in component:
                    raise JobError(f'Managed {role} contains a legacy path')
                if 'asset' not in component:
                    raise JobError(f'Managed {role} requires a typed asset reference')
                if 'asset' in component:
                    resolved, record, _ = asset(component.pop('asset'), role)
                    if role == 'animation':
                        if set(component) - {'clip', 'rate', 'loop', 'playing', 'controller'}:
                            raise JobError('Animation has unknown playback fields')
                        if 'controller' in component and not isinstance(component['controller'], dict):
                            raise JobError('Animation controller must be a versioned object')
                        clip = component.get('clip', 0)
                        if not isinstance(clip, int) or isinstance(clip, bool) or clip < 0 or clip >= record.get('animation_count', 0):
                            raise JobError('Animation clip is unavailable in the rebuilt bank; select a valid clip')
                        rate = component.get('rate', 1)
                        if not isinstance(rate, (int, float)) or isinstance(rate, bool) or not math.isfinite(rate):
                            raise JobError('Animation playback rate must be finite')
                        if any(not isinstance(component.get(key, True), bool) for key in ('loop', 'playing')):
                            raise JobError('Animation loop and playing must be boolean')
                    component['path'] = str(resolved)
            if isinstance(entity.get('text3d'), dict):
                entity['text3d']['font'] = font(entity['text3d'].get('font'))
        # Register the selected scene font before any scene text is instantiated.
        font(None)
        overlay = scene.get('edit_overlay')
        edit_path = contained(root, overlay, must_exist=True) if overlay else root / 'edits' / 'scene.editor.json'
        if overlay and not edit_path.is_file():
            raise JobError('Selected authored override journal is unavailable')
        if overlay:
            self.validate_overlay_dependencies(self.checked_overlay(load_json(edit_path)), root)
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
        scene = migrate_source_references(read_managed_scene(path), path)
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
                for reference, material_reference in load_json(sidecar).get('materials', {}).items():
                    try:
                        material = legacy_source(reference, original, self.legacy_root)
                        self.asset_display_names[(original / material_reference).resolve()] = material.stem
                        for line in material.read_text(encoding='utf-8').splitlines():
                            key, separator, value = line.partition('=')
                            if separator and key.strip() == 'name' and value.strip():
                                self.asset_display_names[(original / material_reference).resolve()] = value.strip()
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
            prefix = managed_reference(original, path.parent) + '/'
            replacement = managed_reference(revision, self.stage) + '/'
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
                self.generated_root.mkdir(parents=True, exist_ok=True)
                self.run_tool('mesh', ['--input', source, '--output', output,
                    '--bundle-root', bundle, '--import-id', record['import_id'],
                    '--generated-root', self.generated_root], 'Preparing model')
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
                                   'path': managed_reference(output, self.stage), 'version': 1}]
            record['fingerprint'] = 'sha256:' + digest(output)
            record.pop('closure', None)
            if record['kind'] == 'mesh':
                self.cook_model_animation(record, source, output)
        scene['assets'] = self.assets
        self.validate_semantics(scene)
        builds = path.parent / 'builds'
        builds.mkdir(exist_ok=True)
        (self.stage / 'builds').mkdir(exist_ok=True)
        for directory in (self.stage / 'builds').iterdir():
            destination = builds / directory.name
            if destination.exists():
                raise JobError('Build revision collision')
            publish_directory(directory, destination)
            self.published_builds.append(destination)
        requested_bakes = self.request.get('bakes', {})
        self.request['bakes'] = {**scene.get('bake_recipes', {}), **requested_bakes, 'prepare_assets': True}
        self.perform_bakes(scene, path.parent)
        result = self.lower(scene, path.parent)
        if digest(path) != original_fingerprint:
            raise JobError('Scene changed while preparing assets; retry the build')
        # Transfer ownership before publication: cancellation may retain an orphan
        # revision, but can never delete a revision referenced by a committed scene.
        validate_managed_scene(scene)
        self.published_builds.clear()
        write_managed_scene(path, scene)
        return result

    def edit_assets(self):
        path = Path(self.request['scene_path']).resolve(strict=True)
        if path != (self.final / 'scene.json').resolve():
            raise JobError('Asset operation scene does not match project membership')
        scene = migrate_source_references(read_managed_scene(path), path)
        fingerprint = digest(path)
        if scene.get('edit_overlay'):
            load_json(contained(path.parent, scene['edit_overlay']))
        self.validate_semantics(scene)
        self.assets = scene['assets']
        staging = self.project_root / '.staging'
        staging.mkdir(parents=True, exist_ok=True)
        self.stage = Path(tempfile.mkdtemp(prefix=self.scene_id + '-assets-', dir=staging))
        operation = self.request['operation']
        added_entity = None
        if operation == 'add_entities':
            if not self.request.get('models') and not self.request.get('lights'):
                raise JobError('Select a model or light to add')
            added_entity = self.append_entities(scene)
            self.validate_semantics(scene)
        elif operation == 'import_assets':
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
                elif suffix in ('.png', '.jpg', '.jpeg', '.bmp', '.tga', '.vkt'):
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
                    raise JobError('Asset name must be 1Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р В РІР‚В Р В Р’В Р Р†Р вЂљРЎв„ўР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р Р†Р вЂљРІвЂћСћР В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р’В Р Р†Р вЂљР’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р РЋРІвЂћСћР В Р’В Р В Р вЂ№Р В Р вЂ Р Р†Р вЂљРЎвЂєР РЋРЎвЂєР В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В Р вЂ Р В РІР‚С™Р вЂ™Р’В Р В Р’В Р вЂ™Р’В Р В РІР‚в„ўР вЂ™Р’В Р В Р’В Р В РІР‚В Р В Р’В Р Р†Р вЂљРЎв„ўР В Р Р‹Р Р†РІР‚С›РЎС›Р В Р’В Р вЂ™Р’В Р В Р’В Р В РІР‚в„–Р В Р’В Р В Р вЂ№Р В Р вЂ Р Р†Р вЂљРЎвЂєР РЋРЎвЂє512 UTF-8 bytes without control characters')
                record['name'] = name.strip()
                result = self.lower(scene, path.parent)
                if digest(path) != fingerprint:
                    raise JobError('Scene changed during rename; retry')
                write_managed_scene(path, scene)
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
                    record['source'] = managed_reference(source, self.stage)
                output = bundle / 'mesh.vkb'
                self.generated_root.mkdir(parents=True, exist_ok=True)
                self.run_tool('mesh', ['--input', source, '--output', output,
                    '--bundle-root', bundle, '--import-id', import_id,
                    '--generated-root', self.generated_root], 'Rebuilding model')
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
                record['source'] = managed_reference(output, self.stage)
            elif record['kind'] in ('texture', 'environment'):
                output = self.copy_blob(source, bundle)
                record['source'] = managed_reference(output, self.stage)
            else:
                raise JobError('Use the scene bake controls to rebuild this generated asset')
            role = (record.get('artifacts') or [{}])[0].get('role', record['kind'])
            record.pop('closure', None)
            record.update(id=asset_id, import_id=import_id, import_revision=revision,
                reimport_status='source_snapshot', artifacts=[{'role': role,
                    'path': managed_reference(output, self.stage), 'version': 1}],
                fingerprint='sha256:' + digest(output))
            record.pop('animation_count', None)
            if record['kind'] == 'mesh':
                self.cook_model_animation(record, source, output)
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
                if source.is_dir():
                    publish_directory(source, destination)
                else:
                    os.rename(source, destination)
                self.published_builds.append(destination)
        result = self.lower(scene, path.parent)
        if digest(path) != fingerprint:
            raise JobError('Scene changed during import; retry without losing the previous revision')
        # Transfer ownership before publication: cancellation may retain an orphan
        # revision, but can never delete a revision referenced by a committed scene.
        validate_managed_scene(scene)
        self.published_builds.clear()
        write_managed_scene(path, scene)
        if added_entity is not None:
            result['added_scene_entity'] = added_entity
        return result

    def cleanup_after_success(self):
        """Runs workspace cleanup after a published write. The result is already
        durable, so a cleanup failure or cancellation only leaves garbage."""
        if self.read_only or self.request.get('operation') not in CLEANUP_OPERATIONS:
            return
        try:
            self.collect_garbage()
        except (JobError, OSError, ValueError, KeyError, TypeError) as error:
            print(f'Warning: workspace cleanup stopped: {error}', flush=True)

    def collect_garbage(self):
        """Removes workspace data that no listed scene can reach. Bundles hold
        clones or copies of cache files, so a cache entry is kept only while a
        scene record names its content digest; when any scene is unreadable the
        caches are left alone. Returns (paths removed, bytes freed)."""
        now = time.time()
        removed = []
        freed = 0

        def drop(path):
            nonlocal freed
            freed += remove_tree(path)
            removed.append(path)

        digests = set()
        readable = True
        projects = self.workspace / 'projects'
        for project in sorted(projects.iterdir()) if projects.is_dir() else []:
            if project.is_symlink() or not project.is_dir():
                continue
            try:
                identifier(project.name)
            except JobError:
                continue
            manifest = project / 'project.json'
            if not manifest.is_file():
                if project != self.project_root and older_than(project, ORPHAN_GRACE_SECONDS, now):
                    drop(project)
                continue
            try:
                document = load_json(manifest, MAX_MANAGED_DOCUMENT_BYTES)
                members = {entry['id'] for entry in document.get('scenes', [])}
            except (JobError, KeyError, TypeError):
                readable = False
                continue
            document_references(document.get('assets', []), digests, set())
            staging = project / '.staging'
            for child in sorted(staging.iterdir()) if staging.is_dir() else []:
                if older_than(child, UNREFERENCED_GRACE_SECONDS, now):
                    drop(child)
            scenes = project / 'scenes'
            for scene_root in sorted(scenes.iterdir()) if scenes.is_dir() else []:
                if scene_root.is_symlink() or not scene_root.is_dir():
                    continue
                # A recent unlisted scene may await its membership save; it
                # stays live until the grace period proves it abandoned.
                if (scene_root.name not in members and scene_root != self.final and
                        older_than(scene_root, ORPHAN_GRACE_SECONDS, now)):
                    drop(scene_root)
                    continue
                try:
                    scene = read_managed_scene(scene_root / 'scene.json')
                    raw = load_json(scene_root / 'scene.json', MAX_MANAGED_DOCUMENT_BYTES)
                    overlay = (load_json(contained(scene_root, scene['edit_overlay']))
                               if scene.get('edit_overlay') else {})
                except (JobError, OSError, KeyError, TypeError, ValueError):
                    readable = False
                    continue
                revisions = set()
                document_references([scene, overlay], digests, revisions)
                for revision in sorted((scene_root / 'builds').glob('*')):
                    if revision.name not in revisions and older_than(revision, UNREFERENCED_GRACE_SECONDS, now):
                        drop(revision)
                current = raw.get('inventory')
                for inventory in sorted((scene_root / 'inventory').glob('*.json')):
                    if (managed_reference(inventory, scene_root) != current and
                            older_than(inventory, UNREFERENCED_GRACE_SECONDS, now)):
                        drop(inventory)
        if readable:
            freed_before = freed
            textures = self.workspace / 'cache' / 'textures'
            for entry in sorted(textures.iterdir()) if textures.is_dir() else []:
                manifest = entry / 'manifest.json'
                try:
                    recorded = load_json(manifest) if manifest.is_file() else None
                except JobError:
                    recorded = {}
                if recorded is None:
                    if older_than(entry, UNREFERENCED_GRACE_SECONDS, now):
                        drop(entry)
                elif (recorded.get('recipe', {}).get('version') != 2 or
                      recorded.get('sha256') not in digests):
                    drop(entry)
            generated = self.workspace / 'cache' / 'generated'
            index_path = self.workspace / 'cache' / 'generated-index.json'
            try:
                index = load_json(index_path) if index_path.is_file() else {}
            except JobError:
                index = {}
            kept = {}
            for path in sorted(generated.rglob('*')) if generated.is_dir() else []:
                if path.is_symlink() or not path.is_file():
                    continue
                relative = managed_reference(path, generated)
                info = path.stat()
                known = index.get(relative)
                if known and known[0] == info.st_size and known[1] == info.st_mtime_ns:
                    content = known[2]
                else:
                    content = digest(path)
                if content in digests:
                    kept[relative] = [info.st_size, info.st_mtime_ns, content]
                elif '.tmp' not in path.name or older_than(path, UNREFERENCED_GRACE_SECONDS, now):
                    drop(path)
            for directory in sorted(generated.rglob('*'), reverse=True) if generated.is_dir() else []:
                if directory.is_dir() and not directory.is_symlink() and not any(directory.iterdir()):
                    directory.rmdir()
            if generated.is_dir():
                atomic_json(index_path, kept)
            if freed > freed_before:
                print(f'Workspace cleanup: removed unused cache entries ({(freed - freed_before) / 2**20:.1f} MiB)',
                      flush=True)
        jobs = self.workspace / 'jobs'
        for job in sorted(jobs.iterdir()) if jobs.is_dir() else []:
            if job != self.result_path.parent and older_than(job, JOB_RETENTION_SECONDS, now):
                drop(job)
        if removed:
            print(f'Workspace cleanup: removed {len(removed)} unreferenced paths, '
                  f'{freed / 2**20:.1f} MiB', flush=True)
        return removed, freed

    def delete_project(self):
        """Erase a project directory only after the project store unpublished
        its manifest, so a listed project is never erased."""
        if self.read_only:
            raise JobError('Cannot delete a project in a read-only workspace')
        if self.result_path.is_relative_to(self.project_root):
            raise JobError('Delete result must be outside the project directory')
        if self.project_path.exists() or self.project_path.is_symlink():
            raise JobError('Project is still published; remove its manifest before deleting files')
        pending = [self.project_root]
        while pending:
            directory = pending.pop()
            try:
                info = directory.lstat()
            except FileNotFoundError:
                if directory == self.project_root:
                    return {'version': VERSION, 'status': 'complete',
                            'deleted_project_id': self.project_root.name}
                raise
            # Junctions and other Windows reparse points must never become
            # recursive deletion roots, even when their targets are local.
            if (stat.S_ISLNK(info.st_mode) or
                    getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT):
                raise JobError(f'Project deletion refuses links or reparse points: {directory}')
            if stat.S_ISDIR(info.st_mode):
                pending.extend(directory.iterdir())
        self.progress('Deleting project files', 0.1)
        shutil.rmtree(self.project_root)
        return {'version': VERSION, 'status': 'complete', 'deleted_project_id': self.project_root.name}

    def delete_scene(self):
        """Erase scene-owned files only after the project store removed membership."""
        if self.read_only:
            raise JobError('Cannot delete a scene in a read-only workspace')
        expected = self.final / 'scene.json'
        raw_path = self.request.get('scene_path')
        if not isinstance(raw_path, str) or not Path(raw_path).is_absolute() or Path(raw_path) != expected:
            raise JobError('Delete scene path must be the exact project-owned scene manifest')
        if self.result_path.is_relative_to(self.final):
            raise JobError('Delete result must be outside the scene directory')

        def checked_stat(path):
            info = path.lstat()
            # Junctions and other Windows reparse points must never become
            # recursive deletion roots, even when their targets are local.
            if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                raise JobError(f'Scene deletion refuses links or reparse points: {path}')
            return info

        # Validate ancestors before resolving or traversing the deletion tree.
        for ancestor in (self.project_root, self.project_root / 'scenes'):
            try:
                checked_stat(ancestor)
            except FileNotFoundError:
                pass
        if expected.resolve() != expected:
            raise JobError('Delete scene path escapes its project-owned directory')
        project = load_json(self.project_path)
        scenes = project.get('scenes')
        if project.get('version') != VERSION or not isinstance(scenes, list):
            raise JobError('Invalid project manifest for scene deletion')
        for scene in scenes:
            if not isinstance(scene, dict) or not isinstance(scene.get('path'), str):
                raise JobError('Invalid project scene membership')
            member_path = (self.project_root / scene['path']).resolve()
            if scene.get('id') == self.scene_id or member_path.is_relative_to(self.final):
                raise JobError('Scene is still referenced by the project; remove membership before deleting files')
        try:
            info = checked_stat(self.final)
        except FileNotFoundError:
            return {'version': VERSION, 'status': 'complete', 'deleted_scene_id': self.scene_id}
        if not stat.S_ISDIR(info.st_mode):
            raise JobError('Scene deletion root is not a directory')
        pending = [self.final]
        while pending:
            directory = pending.pop()
            for child in directory.iterdir():
                info = checked_stat(child)
                if stat.S_ISDIR(info.st_mode):
                    pending.append(child)
        self.progress('Deleting scene files', 0.1)
        shutil.rmtree(self.final)
        return {'version': VERSION, 'status': 'complete', 'deleted_scene_id': self.scene_id}

    def execute(self):
        try:
            if self.request.get('operation') in ('delete_scene', 'delete_project'):
                deleting_scene = self.request['operation'] == 'delete_scene'
                result = self.delete_scene() if deleting_scene else self.delete_project()
                atomic_json(self.result_path, result)
                atomic_json(self.progress_path, {'version': VERSION, 'status': 'complete',
                                                'stage': 'Scene deleted' if deleting_scene else 'Project deleted',
                                                'progress': 1.0})
                self.cleanup_after_success()
                return 0
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
                result = self.prepare_unbuilt(path, migrate_source_references(read_managed_scene(path), path))
            elif self.request.get('operation') == 'prepare_scene':
                result = self.prepare(self.request['scene_path'])
            elif self.request.get('operation') in ('add_entities', 'import_assets', 'reimport_asset', 'rebuild_asset', 'rename_asset'):
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
            self.cleanup_after_success()
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
