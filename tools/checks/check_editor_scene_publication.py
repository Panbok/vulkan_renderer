#!/usr/bin/env python3
"""Exercise scene directory publication and rollback without renderer assets."""
import importlib.util
from pathlib import Path
import tempfile
import uuid


def main():
    source = Path(__file__).resolve().parents[1] / 'editor_project_jobs.py'
    spec = importlib.util.spec_from_file_location('editor_project_jobs', source)
    jobs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(jobs)
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
        failed = jobs.Job(failed_request, result)
        progress = failed.progress

        def fail_after_publication(label, *args, **kwargs):
            if label == 'Opening scene':
                assert (failed.final / 'scene.json').is_file()
                raise jobs.JobError('Injected failure after directory publication')
            return progress(label, *args, **kwargs)

        failed.progress = fail_after_publication
        assert failed.execute() == 1
        assert not failed.final.exists(), 'Failed publication was not rolled back'
        assert not list((project / '.staging').iterdir())
        assert scene.read_bytes() == published
        assert manifest.read_bytes() == original
    print('Scene publication, existing-scene preservation and rollback passed')


if __name__ == '__main__':
    main()
