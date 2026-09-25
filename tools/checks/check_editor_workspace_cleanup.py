#!/usr/bin/env python3
"""CPU workspace cleanup checks: reachable data survives, garbage is removed."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import time
import uuid


def main():
    module_path = Path(__file__).resolve().parents[1] / 'editor_project_jobs.py'
    spec = importlib.util.spec_from_file_location('editor_project_jobs', module_path)
    jobs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(jobs)
    old = time.time() - 30 * 24 * 60 * 60

    def age(path):
        for item in [*sorted(Path(path).rglob('*'), reverse=True), Path(path)]:
            os.utime(item, (old, old), follow_symlinks=False)

    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return hashlib.sha256(data).hexdigest()

    with tempfile.TemporaryDirectory(prefix='vkr-cleanup-') as temporary:
        workspace = Path(temporary) / '.vkreditor'
        project_id, scene_id = str(uuid.uuid4()), str(uuid.uuid4())
        project = workspace / 'projects' / project_id
        scene_root = project / 'scenes' / scene_id
        manifest = project / 'project.json'
        jobs.atomic_json(manifest, {'version': 1, 'assets': [], 'scenes': [
            {'id': scene_id, 'name': 'Live', 'path': f'scenes/{scene_id}/scene.json'}]})

        # A live scene names one texture and one probe revision.
        used = write(scene_root / 'builds' / 'kept' / 'texture.vkt', b'used texture')
        write(scene_root / 'builds' / 'probe' / 'probe.vkt', b'probe')
        write(scene_root / 'builds' / 'replaced' / 'texture.vkt', b'replaced')
        write(scene_root / 'builds' / 'recent' / 'texture.vkt', b'recent')
        age(scene_root / 'builds' / 'replaced')
        age(scene_root / 'builds' / 'kept')
        scene = {'version': 4, 'id': scene_id, 'entities': [], 'edit_overlay': None, 'assets': [
            {'id': str(uuid.uuid4()), 'kind': 'texture', 'name': 'used',
             'artifacts': [{'role': 'texture', 'path': 'builds/kept/texture.vkt', 'version': 1}],
             'fingerprint': 'sha256:' + used},
            {'id': str(uuid.uuid4()), 'kind': 'probe-cube', 'name': 'probe',
             'artifacts': [{'role': 'probe-cube', 'path': 'builds/probe/probe.vkt', 'version': 1}]}]}
        jobs.write_managed_scene(scene_root / 'scene.json', scene)
        superseded = scene_root / jobs.load_json(scene_root / 'scene.json')['inventory']
        age(superseded)
        jobs.write_managed_scene(scene_root / 'scene.json', scene)
        current = scene_root / jobs.load_json(scene_root / 'scene.json')['inventory']

        # A recent unlisted scene may await membership; an old one is abandoned.
        pending_root = project / 'scenes' / str(uuid.uuid4())
        pending = write(pending_root / 'builds' / 'a' / 'texture.vkt', b'pending texture')
        jobs.write_managed_scene(pending_root / 'scene.json', {'version': 4, 'id': pending_root.name,
            'entities': [], 'assets': [{'id': str(uuid.uuid4()), 'kind': 'texture', 'name': 'pending',
            'artifacts': [{'role': 'texture', 'path': 'builds/a/texture.vkt', 'version': 1}],
            'fingerprint': 'sha256:' + pending}]})
        abandoned_root = project / 'scenes' / str(uuid.uuid4())
        write(abandoned_root / 'scene.json', b'{}')
        age(abandoned_root)
        write(project / '.staging' / 'crashed' / 'partial.bin', b'partial')
        age(project / '.staging' / 'crashed')
        orphan = workspace / 'projects' / str(uuid.uuid4())
        (orphan / 'scenes').mkdir(parents=True)
        age(orphan)
        reserving = workspace / 'projects' / str(uuid.uuid4())
        reserving.mkdir()

        cache = workspace / 'cache'
        kept_entry = cache / 'textures' / 'kept'
        write(kept_entry / 'texture.vkt', b'used texture')
        jobs.atomic_json(kept_entry / 'manifest.json', {'version': 1, 'sha256': used,
                                                        'recipe': {'version': 2}})
        unused_entry = cache / 'textures' / 'unused'
        write(unused_entry / 'texture.vkt', b'unused')
        jobs.atomic_json(unused_entry / 'manifest.json', {
            'version': 1, 'sha256': hashlib.sha256(b'unused').hexdigest(), 'recipe': {'version': 2}})
        legacy_entry = cache / 'textures' / 'legacy'
        write(legacy_entry / 'texture.vkt', b'used texture')
        jobs.atomic_json(legacy_entry / 'manifest.json', {'version': 1, 'sha256': used,
                                                          'recipe': {'version': 1, 'tool': '0'}})
        incomplete_entry = cache / 'textures' / 'incomplete'
        write(incomplete_entry / 'partial.vkt', b'partial')
        age(incomplete_entry)
        generated = cache / 'generated'
        write(generated / 'normalrough_v2' / 'used_normal.vkt', b'used texture')
        write(generated / 'normalrough_v2' / 'pending_normal.vkt', b'pending texture')
        write(generated / 'cutout_v1' / 'unused.vkt', b'no scene uses this')
        old_job = workspace / 'jobs' / str(uuid.uuid4())
        write(old_job / 'request.json', b'{}')
        age(old_job)
        recent_job = workspace / 'jobs' / str(uuid.uuid4())
        write(recent_job / 'request.json', b'{}')

        request = {'version': 1, 'operation': 'rename_asset', 'workspace_root': str(workspace),
                   'project_path': str(manifest), 'scene_id': scene_id,
                   'scene_path': str(scene_root / 'scene.json')}
        job = jobs.Job(request, recent_job / 'result.json')
        removed, freed = job.collect_garbage()
        assert freed > 0 and removed

        present = lambda *paths: all(Path(path).exists() for path in paths)
        absent = lambda *paths: not any(Path(path).exists() for path in paths)
        assert present(scene_root / 'builds' / 'kept', scene_root / 'builds' / 'probe',
                       scene_root / 'builds' / 'recent', current), 'Reachable or recent revision removed'
        assert absent(scene_root / 'builds' / 'replaced', superseded), 'Old unreferenced revision kept'
        assert present(pending_root) and absent(abandoned_root), 'Unlisted scene grace failed'
        assert absent(project / '.staging' / 'crashed')
        assert absent(orphan) and present(reserving), 'Orphan project grace failed'
        assert present(kept_entry) and absent(unused_entry, legacy_entry, incomplete_entry)
        assert present(generated / 'normalrough_v2' / 'used_normal.vkt',
                       generated / 'normalrough_v2' / 'pending_normal.vkt')
        assert absent(generated / 'cutout_v1'), 'Unused derived texture or its empty folder kept'
        assert absent(old_job) and present(recent_job)
        index = jobs.load_json(cache / 'generated-index.json')
        assert sorted(index) == ['normalrough_v2/pending_normal.vkt', 'normalrough_v2/used_normal.vkt']

        # A second pass finds nothing and reuses indexed digests.
        assert job.collect_garbage() == ([], 0)

        # An unreadable scene leaves every cache entry alone.
        unused_again = write(generated / 'cutout_v1' / 'unused.vkt', b'no scene uses this')
        (scene_root / 'scene.json').write_text('{')
        job.collect_garbage()
        assert (generated / 'cutout_v1' / 'unused.vkt').is_file() and unused_again
        jobs.write_managed_scene(scene_root / 'scene.json', scene)

        # Successful write jobs run cleanup; the new scene counts as live.
        age(generated / 'cutout_v1')
        create = {'version': 1, 'operation': 'create_scene', 'workspace_root': str(workspace),
                  'project_path': str(manifest), 'scene_id': str(uuid.uuid4()),
                  'scene_name': 'Created', 'models': [], 'bakes': {}}
        assert jobs.Job(create, recent_job / 'create.json').execute() == 0
        assert (project / 'scenes' / create['scene_id'] / 'scene.json').is_file()
        assert absent(generated / 'cutout_v1' / 'unused.vkt'), 'Write job skipped cleanup'
        assert present(generated / 'normalrough_v2' / 'used_normal.vkt')

    print('Workspace cleanup: reachable revisions, caches and pending scenes kept; '
          'abandoned scenes, projects, staging, stale revisions, unused caches and old jobs removed')


if __name__ == '__main__':
    main()
