#!/usr/bin/env python3
"""Test client for `vkr_bakery project`, the managed project job runner.

Checks run each job as a `vkr_bakery project --request --result` child and
read the documents it publishes with the reference helpers below. The helpers
restate the scene-document contract (ADR-069, ADR-076) independently of the C
implementation so a check can build fixtures and inspect results.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import uuid

VERSION = 1
MAX_JSON_BYTES = 16 * 1024 * 1024
MAX_MANAGED_DOCUMENT_BYTES = 1024 * 1024
MANAGED_SCENE_VERSION = 5
MANAGED_SCENE_VERSIONS = (3, 4, MANAGED_SCENE_VERSION)
DOCUMENT_BLOCKS = ('transform', 'mesh', 'shape', 'text3d')
MAX_INVENTORY_BYTES = 16 * 1024 * 1024
FACES = ('r', 'l', 'u', 'd', 'f', 'b')
REPOSITORY = Path(__file__).resolve().parents[2]


class JobError(Exception):
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
    if path.name == 'scene.json' and value.get('version') in MANAGED_SCENE_VERSIONS:
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


def document_entity_ids(entities):
    """Index of each entity's document-stable UUID (ADR-076)."""
    ids = {}
    for index, entity in enumerate(entities):
        value = entity.get('id') if isinstance(entity, dict) else None
        try:
            canonical = isinstance(value, str) and str(uuid.UUID(value)) == value
        except ValueError:
            canonical = False
        if not canonical or value in ids:
            raise JobError('Every entity needs a unique canonical UUID id')
        ids[value] = index
    return ids


def document_to_internal(scene):
    """Version 5 keeps entity blocks in `components` and names parents by
    id. Jobs edit the internal form: blocks beside the components map and
    parents by index."""
    entities = scene.get('entities', [])
    if not isinstance(entities, list):
        raise JobError('Invalid entity array')
    ids = document_entity_ids(entities)
    for entity in entities:
        components = entity.get('components')
        if isinstance(components, dict):
            for block in DOCUMENT_BLOCKS:
                if block in components:
                    if block in entity:
                        raise JobError(f'An entity has both a {block} block and component')
                    entity[block] = components.pop(block)
            if not components:
                entity.pop('components')
        parent = entity.get('parent')
        if parent is not None:
            if parent not in ids:
                raise JobError('An entity parent names a missing entity id')
            entity['parent'] = ids[parent]
    return scene


def document_from_internal(document):
    """The version 5 form of an internal document: every entity keeps or
    gains a UUID, parents name ids and blocks move into components."""
    entities = document.get('entities', [])
    for entity in entities:
        if not isinstance(entity.get('id'), str):
            entity['id'] = str(uuid.uuid4())
    ids = [entity['id'] for entity in entities]
    document_entity_ids(entities)
    for entity in entities:
        parent = entity.get('parent')
        if parent is not None:
            if type(parent) is not int or not 0 <= parent < len(entities):
                raise JobError('Invalid entity parent index')
            entity['parent'] = ids[parent]
        moved = {block: entity.pop(block) for block in DOCUMENT_BLOCKS if block in entity}
        if moved:
            components = entity.setdefault('components', {})
            if set(moved) & set(components):
                raise JobError('An entity has both a block and a component of one kind')
            components.update(moved)
    return document


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


def remap_overlay_indices(overlay, entities):
    """A version 5 overlay names document entities by saved index plus its
    document_ids list; rebind those indices to the entities' current order,
    or -1 for entities the document no longer has. Documents without ids
    bind saved indices directly, as the runtime does."""
    saved = overlay.get('document_ids')
    if saved is None:
        return overlay
    if not isinstance(saved, list) or any(not isinstance(value, str) for value in saved):
        raise JobError('Invalid authored override document ids')
    try:
        current = document_entity_ids(entities)
    except JobError:
        return overlay

    def rebind(reference):
        index = reference.get('scene_entity')
        if type(index) is int:
            reference['scene_entity'] = current.get(saved[index], -1) if 0 <= index < len(saved) else -1

    for record in overlay['overrides'] + overlay.get('created', []):
        if not isinstance(record, dict):
            raise JobError('Invalid authored override record')
        rebind(record)
        if isinstance(record.get('parent'), dict):
            rebind(record['parent'])
        for reference, _, _ in overlay_references(record):
            if reference is not record:
                rebind(reference)
    overlay['document_ids'] = [entity['id'] for entity in entities]
    return overlay


def read_managed_scene(path):
    """Loads a managed scene with its asset records inline, in the internal
    form jobs edit. Version 3 stores assets in scene.json; later versions
    name an inventory revision, and version 5 is converted from its document
    form."""
    path = Path(path)
    scene = load_json(path, MAX_MANAGED_DOCUMENT_BYTES)
    if scene.get('version') not in (4, MANAGED_SCENE_VERSION):
        return scene
    reference = scene.pop('inventory', None)
    if not isinstance(reference, str) or 'assets' in scene:
        raise JobError('Managed scene needs exactly one inventory reference')
    inventory = load_json(contained(path.parent, reference), MAX_INVENTORY_BYTES)
    if inventory.get('version') != 1 or not isinstance(inventory.get('assets'), list):
        raise JobError('Managed scene inventory is invalid')
    scene['assets'] = inventory['assets']
    if scene['version'] == MANAGED_SCENE_VERSION:
        document_to_internal(scene)
    return scene


def validate_managed_scene(scene, reference='inventory/' + '0' * 36 + '.json'):
    """Splits an in-memory scene into its version 5 manifest and inventory
    revision, checking each against its own durable limit."""
    document = copy.deepcopy({key: value for key, value in scene.items() if key != 'assets'})
    document.update(version=MANAGED_SCENE_VERSION, inventory=reference)
    document_from_internal(document)
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


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def ready_records(log_path, materials_directory):
    """Ready-log records of the materials in `materials_directory`, checked
    against the published files: each material appears once, with the same
    lines apart from its texture references, which resolve (against the
    record's path) to the bytes the published file's references name, with
    the same queries."""
    records = {}
    for line in Path(log_path).read_text().splitlines():
        record = json.loads(line)
        assert record['material'] not in records, record['material']
        records[record['material']] = record

    def split(text, owner):
        plain, textures = [], []
        for line in text.splitlines():
            key, _, value = line.partition('=')
            if key.endswith('_texture') and value:
                reference, _, query = value.partition('?')
                target = Path(reference) if Path(reference).is_absolute() else \
                    Path(owner).parent / reference
                textures.append((key, query, digest(target)))
            else:
                plain.append(line)
        return plain, textures

    for material in sorted(Path(materials_directory).glob('*.mt')):
        record = records.get(material.stem)
        assert record, f'{material.stem} has no ready record'
        assert split(record['definition'], record['path']) == \
            split(material.read_text(), material), f'{material.stem} record differs'
    return records


def source_fingerprint(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{value:016x}'


def mesh_identity(seed, mesh_hash):
        value = int(seed, 16)
        for byte in struct.pack('<Q', int(mesh_hash, 16)):
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


def default_bakery():
    """The Release vkr_bakery of this checkout, or $VKR_BAKERY."""
    if os.environ.get('VKR_BAKERY'):
        return Path(os.environ['VKR_BAKERY'])
    suffix = '.exe' if os.name == 'nt' else ''
    path = REPOSITORY / 'build_release' / 'tools' / 'bakery' / ('vkr_bakery' + suffix)
    return path if path.is_file() else path.parent / 'Release' / path.name


class Job:
    """One `vkr_bakery project` transaction; execute() returns its exit code."""

    def __init__(self, request, result_path, bakery=None, environment=None):
        self.request = request
        self.result_path = Path(result_path)
        tools = request.get('tools') or {}
        self.bakery = Path(bakery or tools.get('mesh') or default_bakery())
        self.environment = environment or {}
        self.output = ''

    def execute(self):
        with tempfile.TemporaryDirectory(prefix='vkr-project-request-') as temporary:
            request_path = Path(temporary) / 'request.json'
            request_path.write_text(json.dumps(self.request), encoding='utf-8')
            completed = subprocess.run(
                [str(self.bakery), 'project', '--request', str(request_path),
                 '--result', str(self.result_path)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env={**os.environ, **self.environment})
        self.output = completed.stdout
        return completed.returncode

    def collect_garbage(self):
        """Runs workspace cleanup alone; returns (paths removed, bytes freed)."""
        request = dict(self.request, operation='collect_garbage')
        job = Job(request, self.result_path, self.bakery, self.environment)
        code = job.execute()
        result = load_json(self.result_path)
        if code != 0:
            raise JobError(result.get('error', job.output))
        return result['removed'], result['freed_bytes']

    def inspect_mesh(self, mesh):
        """The mesh cooker's --inspect report of a cooked .vkb."""
        with tempfile.TemporaryDirectory(prefix='vkr-inspect-') as temporary:
            report = Path(temporary) / 'report.json'
            subprocess.run([str(self.bakery), 'tool', 'mesh', '--inspect', '--input', str(mesh),
                            '--output', str(report)], check=True, capture_output=True)
            return json.loads(report.read_text(encoding='utf-8'))

    def effective_bake_runtime(self, scene, root):
        """Publishes `scene` at root/scene.json, then asks the runner for the
        runtime a bake observes with the scene's authored overlay applied."""
        scene_path = Path(root) / 'scene.json'
        original = scene_path.read_bytes() if scene_path.is_file() else None
        write_managed_scene(scene_path, copy.deepcopy(scene))
        inventory = Path(root) / load_json(scene_path)['inventory']
        request = dict(self.request, operation='effective_bake_runtime',
                       scene_path=str(scene_path))
        request.pop('source_scene', None)
        try:
            with tempfile.TemporaryDirectory(prefix='vkr-effective-') as temporary:
                result_path = Path(temporary) / 'result.json'
                job = Job(request, result_path, self.bakery, self.environment)
                code = job.execute()
                result = load_json(result_path)
        finally:
            # The fixture document existed only for this request.
            if original is not None:
                scene_path.write_bytes(original)
                inventory.unlink(missing_ok=True)
        if code != 0:
            raise JobError(result.get('error', job.output))
        return result
