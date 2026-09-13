#!/usr/bin/env python3
"""Check append publication, overlay identity and failed-add preservation."""
import argparse
import copy
import importlib.util
from pathlib import Path
import tempfile
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesh-cooker', required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / 'editor_project_jobs.py'
    spec = importlib.util.spec_from_file_location('editor_project_jobs', source)
    jobs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(jobs)
    with tempfile.TemporaryDirectory(prefix='vkr-add-entities-') as temporary:
        root = Path(temporary)
        workspace = root / '.vkreditor'
        project = workspace / 'projects' / str(uuid.uuid4())
        project.mkdir(parents=True)
        manifest = project / 'project.json'
        jobs.atomic_json(manifest, {'version': 1, 'assets': [], 'scenes': []})
        transform = {'pos': [2, 3, 4], 'rot': [0, 0, 0, 1], 'scale': [1, 1, 1]}
        light = {'name': 'Original', 'parent': None, 'transform': transform,
                 'point_light': {'color': [1, 1, 1], 'intensity': 5, 'range': 10}}
        request = {'version': 1, 'operation': 'create_scene',
                   'workspace_root': str(workspace), 'project_path': str(manifest),
                   'scene_id': str(uuid.uuid4()), 'scene_name': 'Entity publication fixture',
                   'lights': [light], 'bakes': {},
                   'tools': {'mesh': str(Path(args.mesh_cooker).resolve())}}
        result_path = root / 'result.json'
        assert jobs.Job(request, result_path).execute() == 0
        initial = jobs.load_json(result_path)
        scene_path = Path(initial['scene_path'])
        scene = jobs.load_json(scene_path)
        original_entities = copy.deepcopy(scene['entities'])
        overlay_path = scene_path.parent / 'edits' / 'original.json'
        jobs.atomic_json(overlay_path, {'version': 1, 'overrides': [{
            'scene_entity': 0, 'gltf_node': -1,
            'source_fingerprint': jobs.source_fingerprint(request['scene_id'].encode()),
            'fields': 2, 'name': 'Saved original'}]})
        scene['edit_overlay'] = 'edits/original.json'
        jobs.atomic_json(scene_path, scene)
        overlay_bytes = overlay_path.read_bytes()
        additions = [
            {'name': 'Point', 'transform': transform, 'point_light': light['point_light']},
            {'name': 'Spot', 'transform': transform,
             'point_light': {**light['point_light'], 'kind': 2,
                             'direction_local': [0, -1, 0],
                             'inner_cone_angle': .35, 'outer_cone_angle': .6}},
            {'name': 'Sun', 'directional_light': {'color': [1, 1, 1], 'intensity': 1}},
            {'name': 'Rectangle', 'transform': transform,
             'rectangle_light': {'color': [1, 1, 1], 'radiance': 2, 'size': [1, 2]}},
        ]
        add_request = dict(request, operation='add_entities', scene_path=str(scene_path), lights=additions)
        assert jobs.Job(add_request, result_path).execute() == 0
        result = jobs.load_json(result_path)
        scene = jobs.load_json(scene_path)
        assert result['added_scene_entity'] == 1
        assert scene['entities'][:1] == original_entities
        assert len(scene['entities']) == 5
        assert scene['entities'][2]['point_light']['kind'] == 2
        assert len({entity['id'] for entity in scene['entities']}) == 5
        assert overlay_path.read_bytes() == overlay_bytes
        assert scene['edit_overlay'] == 'edits/original.json'
        runtime = jobs.load_json(result['runtime_path'])
        assert runtime['source_identity'] == request['scene_id']
        assert runtime['entities'][2]['point_light']['kind'] == 2
        # The existing overlay interpreter independently checks identity and index.
        effective = jobs.Job(add_request, result_path).effective_bake_runtime(scene, scene_path.parent)
        assert jobs.load_json(effective['runtime_path'])['entities'][0]['name'] == 'Saved original'

        # A small CPU-only triangle isolates source/cooked model append behavior.
        model = root / 'triangle.obj'
        model.write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n', encoding='utf-8')
        model_request = dict(add_request, lights=[], models=[str(model)])
        assert jobs.Job(model_request, result_path).execute() == 0
        result = jobs.load_json(result_path)
        assert result['added_scene_entity'] == 5
        model_runtime = jobs.load_json(result['runtime_path'])
        cooked = Path(model_runtime['entities'][5]['mesh']['path'])
        assert cooked.is_file()
        cooked_bytes = cooked.read_bytes()
        cooked_request = dict(model_request, models=[str(cooked)])
        assert jobs.Job(cooked_request, result_path).execute() == 0
        result = jobs.load_json(result_path)
        assert result['added_scene_entity'] == 6
        runtime = jobs.load_json(result['runtime_path'])
        assert Path(runtime['entities'][6]['mesh']['path']).read_bytes() == cooked_bytes
        assert jobs.load_json(scene_path)['entities'][:5] == scene['entities']
        assert overlay_path.read_bytes() == overlay_bytes
        reopen = dict(model_request, operation='prepare_scene')
        assert jobs.Job(reopen, result_path).execute() == 0
        assert len(jobs.load_json(jobs.load_json(result_path)['runtime_path'])['entities']) == 7

        published = scene_path.read_bytes()
        for invalid in (dict(add_request, lights=[]),
                        dict(add_request, lights=[{'point_light': {}, 'directional_light': {}}]),
                        dict(model_request, models=[str(root / 'missing.obj')])):
            assert jobs.Job(invalid, result_path).execute() == 1
            assert scene_path.read_bytes() == published
            assert overlay_path.read_bytes() == overlay_bytes
        assert not list((project / '.staging').iterdir())
    print('Entity append, four light kinds, source/cooked models, overlay identity, reopen and failure preservation passed')


if __name__ == '__main__':
    main()
