#!/usr/bin/env python3
"""CPU checks for permanent scene cleanup without deleting unrelated files."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch
import uuid


def main():
    module_path = Path(__file__).resolve().parents[1] / 'editor_project_jobs.py'
    spec = importlib.util.spec_from_file_location('editor_project_jobs', module_path)
    jobs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(jobs)
    with tempfile.TemporaryDirectory(prefix='vkr-scene-delete-') as temporary:
        root = Path(temporary).resolve()
        workspace = root / 'workspace'
        project = workspace / 'projects' / str(uuid.uuid4())
        scene_id = str(uuid.uuid4())
        scene = project / 'scenes' / scene_id
        scene.mkdir(parents=True)
        manifest = project / 'project.json'
        result_path = root / 'result.json'
        request = {'version': 1, 'operation': 'delete_scene',
                   'workspace_root': str(workspace), 'project_path': str(manifest),
                   'scene_id': scene_id, 'scene_path': str(scene / 'scene.json'),
                   'bootstrap_directory': str(root / 'missing-bootstrap'),
                   'project_font_source': str(root / 'missing-font')}
        protected = [project / 'shared' / 'asset.bin',
                     project / 'scenes' / str(uuid.uuid4()) / 'scene.json',
                     root / 'external' / 'precious.bin']
        for path in protected:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'preserve me')

        def write_project(scenes):
            jobs.atomic_json(manifest, {'version': 1, 'scenes': scenes, 'assets': []})

        def execute(changes=None, success=True):
            before = manifest.read_bytes()
            with contextlib.redirect_stderr(io.StringIO()):
                code = jobs.Job({**request, **(changes or {})}, result_path).execute()
            result = json.loads(result_path.read_text())
            assert code == (0 if success else 1), result
            assert result['status'] == ('complete' if success else 'failed'), result
            assert manifest.read_bytes() == before, 'Cleanup modified project membership'
            assert all(path.read_bytes() == b'preserve me' for path in protected)
            if success:
                assert result['deleted_scene_id'] == scene_id
                assert 'project_assets' not in result and 'default_font' not in result
            return result

        (scene / 'scene.json').write_text('{}')
        write_project([{'id': scene_id, 'path': f'scenes/{scene_id}/scene.json'}])
        execute(success=False)
        assert (scene / 'scene.json').read_text() == '{}'
        write_project([{'id': str(uuid.uuid4()), 'path': f'scenes/{scene_id}/scene.json'}])
        execute(success=False)
        assert (scene / 'scene.json').is_file()
        write_project([])
        for invalid in (str(protected[2]), str(scene / '..' / scene_id / 'scene.json')):
            execute({'scene_path': invalid}, success=False)
            assert (scene / 'scene.json').is_file()
        try:
            jobs.Job({**request, 'read_only': True}, result_path)
        except jobs.JobError:
            pass
        else:
            raise AssertionError('Read-only deletion was accepted')
        try:
            jobs.Job({**request, 'scene_id': '../external'}, result_path)
        except jobs.JobError:
            pass
        else:
            raise AssertionError('Invalid scene UUID was accepted')

        def make_link(link, target):
            # Windows junctions do not require the symlink creation privilege.
            if os.name == 'nt':
                subprocess.run(['powershell', '-NoProfile', '-Command',
                                'New-Item -ItemType Junction -Path $env:VKR_TEST_LINK -Target $env:VKR_TEST_TARGET -ErrorAction Stop | Out-Null'],
                               env={**os.environ, 'VKR_TEST_LINK': str(link), 'VKR_TEST_TARGET': str(target)},
                               check=True, capture_output=True)
            else:
                link.symlink_to(target, target_is_directory=True)

        def remove_link(link):
            if os.name == 'nt':
                link.rmdir()
            else:
                link.unlink()

        link = scene / 'external-link'
        make_link(link, protected[2].parent)
        try:
            execute(success=False)
            assert (scene / 'scene.json').is_file(), 'Link rejection partially erased the scene'
        finally:
            remove_link(link)

        linked_project = workspace / 'projects' / str(uuid.uuid4())
        linked_project.mkdir()
        jobs.atomic_json(linked_project / 'project.json', {'version': 1, 'scenes': []})
        make_link(linked_project / 'scenes', protected[2].parent)
        try:
            execute({'project_path': str(linked_project / 'project.json'),
                     'scene_path': str(linked_project / 'scenes' / scene_id / 'scene.json')}, success=False)
        finally:
            remove_link(linked_project / 'scenes')

        (scene / 'builds' / 'revision').mkdir(parents=True)
        (scene / 'builds' / 'revision' / 'mesh.bin').write_bytes(b'owned')
        original_rmtree = jobs.shutil.rmtree

        def partial_failure(path, *args, **kwargs):
            if Path(path) == scene:
                (scene / 'builds' / 'revision' / 'mesh.bin').unlink()
                raise PermissionError('injected locked scene file')
            return original_rmtree(path, *args, **kwargs)

        with patch.object(jobs.shutil, 'rmtree', side_effect=partial_failure):
            result = execute(success=False)
        assert 'injected locked scene file' in result['error']
        assert scene.is_dir() and not (scene / 'builds' / 'revision' / 'mesh.bin').exists()
        execute()
        assert not scene.exists()
        execute()
        assert not scene.exists(), 'Retry recreated the scene directory'
        make_link(scene, protected[2].parent)
        try:
            execute(success=False)
        finally:
            remove_link(scene)
        for relative in ('scene.json', 'sources/model.bin', 'builds/revision/cooked.bin', 'runtime/scene.json'):
            path = scene / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'scene-owned bytes')
        execute()
        assert not scene.exists(), 'Successful cleanup left scene-owned files'
    print('Scene deletion checks passed: membership, containment, links, read-only, cleanup and retries')


if __name__ == '__main__':
    main()
