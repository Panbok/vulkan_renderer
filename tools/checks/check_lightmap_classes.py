#!/usr/bin/env python3
"""CPU check of per-pipeline-class lightmap references (ADR-088).

A managed scene names one lightmap set per pipeline class, `lightmaps.tiled`
and `lightmaps.desktop`. Through `vkr_bakery project` on a one-triangle
scene this checks that lowering keeps only this host's set, that a lightmap
bake with nothing to bake for this host drops only this host's reference and
record and keeps the other class's, and that a scene naming one set for every
class fails until it is ported. The triangle has no lightmap UVs, so the
lightmap bake stops at the baker's CPU inspection; no GPU runs.

Example:
  python tools/checks/check_lightmap_classes.py --bakery build_debug/tools/bakery/vkr_bakery.exe
"""
import argparse
from pathlib import Path
import sys
import tempfile
import uuid

import project_jobs as jobs


def lightmap_record(scene_root, pipeline):
    """A recorded set of one class; lowering checks only its bytes."""
    revision = str(uuid.uuid4())
    artifact = scene_root / 'builds' / revision / 'lightmaps.vklm'
    artifact.parent.mkdir(parents=True)
    artifact.write_bytes(pipeline.encode())
    return {'id': str(uuid.uuid4()), 'kind': 'lightmap', 'name': pipeline.title() + ' lightmaps',
            'import_id': revision, 'source': None,
            'artifacts': [{'role': 'lightmap', 'path': f'builds/{revision}/lightmaps.vklm', 'version': 1}],
            'fingerprint': 'sha256:' + jobs.digest(artifact),
            'recipe': {'tool': 'lightmap', 'version': 1, 'settings': {}}}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bakery', default=str(jobs.default_bakery()))
    args = parser.parse_args()
    bakery = Path(args.bakery).resolve()
    # The class this host's bakery bakes and lowers, as it picks it.
    host_class = 'tiled' if sys.platform == 'darwin' else 'desktop'
    other_class = 'desktop' if host_class == 'tiled' else 'tiled'
    with tempfile.TemporaryDirectory(prefix='vkr-lightmap-classes-') as temporary:
        root = Path(temporary)
        workspace = root / 'workspace' / '.vkreditor'
        project = workspace / 'projects' / str(uuid.uuid4())
        project.mkdir(parents=True)
        manifest = project / 'project.json'
        jobs.atomic_json(manifest, {'version': 1, 'assets': [], 'scenes': [], 'default_font': None})
        source = root / 'sources'
        source.mkdir()
        (source / 'triangle.mtl').write_text('newmtl flat\nKd 0.5 0.5 0.5\n')
        model = source / 'triangle.obj'
        model.write_text('mtllib triangle.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\n'
                         'vt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\n'
                         'usemtl flat\nf 1/1/1 2/2/1 3/3/1\n')
        scene_id = str(uuid.uuid4())
        result_path = root / 'result.json'
        create = {'version': 1, 'operation': 'create_scene', 'workspace_root': str(workspace),
                  'project_path': str(manifest), 'scene_id': scene_id, 'scene_name': 'Lightmap classes',
                  'models': [str(model)], 'bakes': {},
                  'tools': {'mesh': str(bakery), 'texture': str(bakery)}}
        assert jobs.Job(create, result_path).execute() == 0, result_path.read_text()
        scene_path = Path(jobs.load_json(result_path)['scene_path'])

        # Each class names its own set; lowering keeps this host's alone.
        scene = jobs.read_managed_scene(scene_path)
        records = {pipeline: lightmap_record(scene_path.parent, pipeline)
                   for pipeline in (host_class, other_class)}
        scene['assets'].extend(records.values())
        scene['lightmaps'] = {pipeline: {'asset': {'scope': 'scene', 'id': record['id'], 'role': 'lightmap'}}
                              for pipeline, record in records.items()}
        jobs.write_managed_scene(scene_path, scene)
        prepare = dict(create, operation='prepare_scene', scene_path=str(scene_path))
        assert jobs.Job(prepare, result_path).execute() == 0, result_path.read_text()
        lowered = jobs.load_json(jobs.load_json(result_path)['runtime_path'])['lightmaps']
        assert list(lowered) == [host_class], lowered
        assert Path(lowered[host_class]['path']).read_bytes() == host_class.encode()

        # Nothing to bake for this host drops its set and keeps the other's.
        bake = dict(prepare, operation='bake_scene', bakes={'lightmap': True})
        assert jobs.Job(bake, result_path).execute() == 0, result_path.read_text()
        baked = jobs.load_json(result_path)
        assert any('Lightmaps skipped' in warning for warning in baked['warnings']), baked['warnings']
        assert 'lightmaps' not in jobs.load_json(baked['runtime_path'])
        kept = jobs.read_managed_scene(scene_path)
        assert kept['lightmaps'] == {other_class: scene['lightmaps'][other_class]}, kept['lightmaps']
        lightmap_ids = [record['id'] for record in kept['assets'] if record['kind'] == 'lightmap']
        assert lightmap_ids == [records[other_class]['id']], lightmap_ids

        # One set holding both classes' planes (VKLM v4) is one record that
        # both classes name. Nothing to bake for this host drops this host's
        # reference only; the record and its file stay for the other class.
        shared_scene = jobs.read_managed_scene(scene_path)
        shared = lightmap_record(scene_path.parent, 'shared')
        shared_scene['assets'].append(shared)
        shared_reference = {'asset': {'scope': 'scene', 'id': shared['id'], 'role': 'lightmap'}}
        shared_scene['lightmaps'] = {host_class: shared_reference, other_class: shared_reference}
        jobs.write_managed_scene(scene_path, shared_scene)
        assert jobs.Job(bake, result_path).execute() == 0, result_path.read_text()
        kept = jobs.read_managed_scene(scene_path)
        assert kept['lightmaps'] == {other_class: shared_reference}, kept['lightmaps']
        assert any(record['id'] == shared['id'] for record in kept['assets']), kept['assets']
        assert (scene_path.parent / shared['artifacts'][0]['path']).is_file()

        # A scene that names one set for every class fails until ported.
        single = jobs.read_managed_scene(scene_path)
        single['lightmaps'] = {'asset': single['lightmaps'][other_class]['asset']}
        jobs.write_managed_scene(scene_path, single)
        assert jobs.Job(prepare, result_path).execute() == 1
        assert 'one set per pipeline class' in jobs.load_json(result_path)['error']
    print('check_lightmap_classes: passed (' + host_class + ' host)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
