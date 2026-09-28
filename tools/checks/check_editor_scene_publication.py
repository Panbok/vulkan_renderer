#!/usr/bin/env python3
"""Exercise scene directory publication and rollback without renderer assets."""
from pathlib import Path
import tempfile
import uuid


def main():
    import project_jobs as jobs
    with tempfile.TemporaryDirectory(prefix='vkr-publication-') as temporary:
        root = Path(temporary)
        workspace = root / '.vkreditor'
        project = workspace / 'projects' / str(uuid.uuid4())
        project.mkdir(parents=True)
        manifest = project / 'project.json'
        jobs.atomic_json(manifest, {'version': 1, 'assets': [], 'scenes': []})
        original = manifest.read_bytes()
        request = {'version': 1, 'operation': 'create_scene',
                   'workspace_root': str(workspace), 'project_path': str(manifest),
                   'scene_id': str(uuid.uuid4()), 'scene_name': 'Publication',
                   'models': [], 'bakes': {}}
        result = root / 'result.json'
        job = jobs.Job(request, result)
        assert job.execute() == 0
        document = jobs.load_json(result)
        scene = Path(document['scene_path'])
        assert scene.is_file() and Path(document['runtime_path']).is_file()
        assert not list((project / '.staging').iterdir())
        published = scene.read_bytes()
        assert jobs.Job(request, result).execute() == 1
        assert scene.read_bytes() == published, 'Existing scene was overwritten'

        failed_request = dict(request, scene_id=str(uuid.uuid4()))
        # Fail after the scene directory is published; the job rolls it back.
        failed = jobs.Job(failed_request, result,
                          environment={'VKR_BAKERY_FAULT_STAGE': 'Opening scene'})
        assert failed.execute() == 1
        assert 'Injected failure at Opening scene' in jobs.load_json(result)['error']
        assert not (project / 'scenes' / failed_request['scene_id']).exists(), \
            'Failed publication was not rolled back'
        assert not list((project / '.staging').iterdir())
        assert scene.read_bytes() == published
        assert manifest.read_bytes() == original
    print('Scene publication, existing-scene preservation and rollback passed')


if __name__ == '__main__':
    main()
