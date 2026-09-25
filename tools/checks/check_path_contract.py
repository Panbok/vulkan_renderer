#!/usr/bin/env python3
"""Exercise portable grammar, source identity and cubemap preparation (no GPU)."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('editor_project_jobs', ROOT / 'tools/editor_project_jobs.py')
jobs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(jobs)


def fixture_job(root):
    job = jobs.Job.__new__(jobs.Job)
    job.stage = root
    root.mkdir(parents=True, exist_ok=True)
    job.sources = {}
    job.warnings = []
    job.files_copied = 0
    job.bytes_copied = 0
    job.source_display_names = {}
    job.texture_seeds = {}
    return job


def rejected(operation):
    try:
        operation()
    except jobs.JobError:
        return
    raise AssertionError('Expected path rejection')


def main():
    corpus = json.loads((ROOT / 'tests/fixtures/paths/managed.json').read_text(encoding='utf-8'))
    with tempfile.TemporaryDirectory(prefix='vkr-paths-') as temporary:
        root = Path(temporary).resolve()
        for case in corpus['cases']:
            if case['valid']:
                target = jobs.contained(root, case['value'], must_exist=False)
                assert jobs.managed_reference(target, root) == case['value']
            else:
                rejected(lambda: jobs.contained(root, case['value'], must_exist=False))
        incoming = root / 'incoming'
        incoming.mkdir()
        (incoming / 'payload').write_bytes(b'new revision')
        published = root / 'published'
        published.mkdir()
        try:
            jobs.publish_directory(incoming, published)
        except FileExistsError:
            pass
        else:
            raise AssertionError("Directory publication replaced another writer's empty reservation")
        assert published.is_dir() and not list(published.iterdir())
        (published / 'payload').write_bytes(b'previous revision')
        try:
            jobs.publish_directory(incoming, published)
        except FileExistsError:
            pass
        else:
            raise AssertionError('Directory publication replaced an existing revision')
        assert (published / 'payload').read_bytes() == b'previous revision'
        assert (incoming / 'payload').read_bytes() == b'new revision'
        jobs.publish_directory(incoming, root / 'new-published')
        assert (root / 'new-published/payload').read_bytes() == b'new revision'
        outside = root / 'outside'
        outside.mkdir()
        owner = root / 'owner'
        owner.mkdir()
        link = owner / 'escape'
        if os.name == 'nt':
            subprocess.run(['powershell', '-NoProfile', '-Command',
                'New-Item -ItemType Junction -Path $env:VKR_TEST_LINK -Target $env:VKR_TEST_TARGET -ErrorAction Stop | Out-Null'],
                env={**os.environ, 'VKR_TEST_LINK': str(link), 'VKR_TEST_TARGET': str(outside)},
                check=True, capture_output=True)
        else:
            link.symlink_to(outside, target_is_directory=True)
        try:
            rejected(lambda: jobs.contained(owner, 'escape'))
        finally:
            if os.name == 'nt':
                link.rmdir()
            else:
                link.unlink()
        legacy = {'version': 3, 'assets': [{'source': 'sources\\id\\mesh.obj', 'name': 'literal\\name', 'unknown': 'literal\\data'}]}
        jobs.migrate_source_references(legacy, 'scene.json')
        assert legacy['assets'][0] == {'source': 'sources/id/mesh.obj', 'name': 'literal\\name', 'unknown': 'literal\\data'}
        rejected(lambda: jobs.migrate_source_references({'assets': [{'source': 'sources\\..\\escape'}]}, 'scene.json'))
        imports = {'version': 1, 'id': 'fixture', 'source': 'sources\\id\\mesh.obj', 'dependencies': [], 'asset': {'source': 'sources\\id\\mesh.obj'}}
        jobs.migrate_source_references(imports, 'import.json')
        assert imports['source'] == imports['asset']['source'] == 'sources/id/mesh.obj'
        assert jobs.model_tokens("mtllib materials/O'Brien.mtl") == ['mtllib', "materials/O'Brien.mtl"]
        for name, separator in [('forward', '/'), ('native', os.sep)]:
            source = root / name
            (source / 'materials').mkdir(parents=True)
            (source / 'source.obj').write_text('mtllib "materials' + separator + 'surface.mtl" # comment\n', encoding='utf-8')
            (source / 'materials/surface.mtl').write_text('newmtl surface\nmap_Kd "tile%20name.png"\n', encoding='utf-8')
            (source / 'materials/tile%20name.png').write_bytes(b'literal percent')
            (source / 'materials/tile name.png').write_bytes(b'incorrect decoded name')
            job = fixture_job(root / (name + '-stage'))
            snapshot = job.snapshot_model(source / 'source.obj', 'fixture')
            assert snapshot.is_file()
            assert [path.read_bytes() for path in (snapshot.parent / 'dependencies').iterdir()] == [b'literal percent']
        if os.name != 'nt':
            source = root / 'literal-backslash'
            source.mkdir()
            (source / 'source.obj').write_text('mtllib materials\\surface.mtl\n', encoding='utf-8')
            (source / 'materials\\surface.mtl').write_text('newmtl surface\nmap_Kd tile\\name.png\n', encoding='utf-8')
            (source / 'tile\\name.png').write_bytes(b'literal POSIX backslash')
            job = fixture_job(root / 'literal-stage')
            snapshot = job.snapshot_model(source / 'source.obj', 'fixture')
            assert [path.read_bytes() for path in (snapshot.parent / 'dependencies').iterdir()] == [b'literal POSIX backslash']
        source = root / 'gltf'
        source.mkdir()
        (source / 'buffer%20name.bin').write_bytes(b'decode exactly once')
        (source / 'source.gltf').write_text(json.dumps({'asset': {'version': '2.0'}, 'buffers': [{'uri': 'buffer%2520name.bin', 'byteLength': 19}]}), encoding='utf-8')
        job = fixture_job(root / 'gltf-stage')
        snapshot = job.snapshot_model(source / 'source.gltf', 'fixture')
        uri = jobs.load_json(snapshot)['buffers'][0]['uri']
        assert (snapshot.parent / uri).read_bytes() == b'decode exactly once'
        rejected(lambda: jobs.gltf_uri_path('https://example.com/file.bin'))
        workspace = root / 'workspace'
        project = workspace / 'projects' / str(uuid.uuid4())
        scene_id = str(uuid.uuid4())
        scene_root = project / 'scenes' / scene_id
        (scene_root / 'sources').mkdir(parents=True)
        (scene_root / 'sources/source.bin').write_bytes(b'preserved source')
        project_path = project / 'project.json'
        jobs.atomic_json(project_path, {'version': 1, 'assets': [], 'scenes': [], 'default_font': None})
        scene_path = scene_root / 'scene.json'
        jobs.atomic_json(scene_path, {'version': 3, 'id': scene_id, 'assets': [
            {'id': 'source', 'kind': 'texture', 'name': 'literal\\name',
             'source': 'sources\\source.bin', 'unknown': 'literal\\data',
             'artifacts': [{'role': 'texture', 'path': 'sources/source.bin'}]}]})
        jobs.atomic_json(workspace / 'editor/bundles/1/manifest.json', {'version': 1, 'files': [], 'assets': []})
        before = scene_path.read_bytes(), project_path.read_bytes()
        request = {'version': 1, 'operation': 'prepare_scene', 'workspace_root': str(workspace),
            'project_path': str(project_path), 'scene_id': scene_id, 'scene_path': str(scene_path),
            'read_only': True, 'runtime_directory': str(root / 'local-runtime')}
        assert jobs.Job(request, root / 'readonly-result.json').execute() == 0
        assert before == (scene_path.read_bytes(), project_path.read_bytes())
        job = fixture_job(root / 'cube')
        job.scene_id = 'fixture'
        job.workspace = job.stage
        job.project_root = job.stage
        job.read_only = False
        job.project_document = lambda: {'assets': [], 'default_font': None}
        directory = job.stage / 'builds/fixture'
        directory.mkdir(parents=True)
        for face in jobs.FACES:
            (directory / f'cube_{face}.png').write_bytes(b'fixture')
        scene = {'version': 3, 'id': 'fixture', 'assets': [{'id': 'cube', 'name': 'Cube', 'source_kind': 'faces', 'base_path': 'builds/fixture/cube', 'extension': 'png', 'artifacts': [{'role': 'probe-cube', 'path': 'builds/fixture/cube_r.png'}]}], 'reflection_probes': [{'enabled': True, 'asset': {'scope': 'scene', 'id': 'cube', 'role': 'probe-cube'}}]}
        result = job.lower(scene, job.stage)
        runtime = jobs.load_json(result['runtime_path'])
        assert runtime['reflection_probes'][0]['cubemap']['base_path'] == str(directory / 'cube')
    print(f"Path contract passed: {len(corpus['cases'])} shared cases, containment, migration, OBJ/MTL, URI and six-face preparation")


if __name__ == '__main__':
    main()
