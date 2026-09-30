"""Import real default templates, retaining isolated projects for editor checks.

Oracle: each course keeps a copied static collider and an empty playable spawn
with its authored camera mode in the published runtime document.
"""

import argparse
import hashlib
import json
from pathlib import Path
import uuid

import project_jobs as jobs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bakery', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    workspace = output/'workspace'/'.vkreditor'
    workspace.mkdir(parents=True, exist_ok=True)
    registry = workspace/'workspace.json'
    if not registry.exists():
        jobs.atomic_json(registry, {'version': 1, 'id': str(uuid.uuid4())})
    bakery = str(args.bakery.resolve())
    reports = []
    for key, name, mode in [('blank', 'Blank', 'first_person'),
                            ('fps_arena', 'FPS Arena', 'first_person'),
                            ('rpg_grounds', 'RPG Grounds', 'third_person')]:
        source = root/'assets'/'templates'/(key+'.scene.json')
        source_digest = hashlib.sha256(source.read_bytes()).hexdigest()
        project_id, scene_id = str(uuid.uuid4()), str(uuid.uuid4())
        project = workspace/'projects'/project_id
        project.mkdir(parents=True)
        manifest = project/'project.json'
        jobs.atomic_json(manifest, {
            'version': 1, 'id': project_id, 'name': name+' verification',
            'default_font': {'scope': 'editor', 'id': 'default-scene-font'},
            'editor_settings': {'version': 1, 'graphics': {}, 'layout': {},
                                'viewport': {}, 'input': {}, 'panels': {}, 'bakery': {}},
            'scene_editor_state': {}, 'assets': [], 'scenes': []})
        request = {'version': 1, 'operation': 'create_scene',
                   'workspace_root': str(workspace), 'project_path': str(manifest),
                   'scene_id': scene_id, 'scene_name': name,
                   'models': [], 'bakes': {}, 'source_scene': str(source), 'include_edits': True,
                   'legacy_root': str(root),
                   'bootstrap_directory': str(root/'build_release'/'editor'/'resources'/'editor'),
                   'tools': {tool: bakery for tool in ('mesh', 'animation', 'collision', 'texture', 'font')}}
        result_path = output/(key+'-import-result.json')
        job = jobs.Job(request, result_path, bakery=bakery)
        code = job.execute()
        (output/(key+'-import.log')).write_text(job.output, encoding='utf-8')
        assert code == 0, result_path.read_text(encoding='utf-8')
        result = jobs.load_json(result_path)
        runtime_path = Path(result['runtime_path'])
        runtime = jobs.load_json(runtime_path)
        managed_path = Path(result['scene_path'])
        managed = jobs.read_managed_scene(managed_path)
        players = [e for e in runtime['entities'] if 'player' in e]
        assert len(players) == 1
        player = players[0]
        assert player['player']['camera_mode'] == mode
        assert player['name'] == 'Player Spawn'
        assert 'mesh' not in player and 'animation' not in player
        meshes = [e for e in runtime['entities'] if 'mesh' in e]
        assert len(meshes) == 1 and Path(meshes[0]['mesh']['path']).is_file()
        assert hashlib.sha256(source.read_bytes()).hexdigest() == source_digest
        overlay = jobs.load_json(result['edit_path'])
        collisions = [c for e in overlay['overrides'] if 'physics' in e
                      for c in e['physics'].get('colliders', [])]
        assert len(collisions) == 1 and collisions[0]['shape'] == 4
        collision_path = Path(collisions[0]['asset'])
        if not collision_path.is_absolute():
            collision_path = workspace/collision_path
        assert collision_path.is_file(), collision_path
        assert collision_path.resolve().is_relative_to(workspace)
        # A fresh editor reads project membership; its UI normally publishes it.
        document = jobs.load_json(manifest)
        document['scenes'] = [{'id': scene_id, 'name': name,
                               'path': f'scenes/{scene_id}/scene.json'}]
        jobs.atomic_json(manifest, document)
        reports.append({'template': key, 'project_id': project_id, 'scene_id': scene_id,
                        'runtime': str(runtime_path), 'managed': str(managed_path),
                        'camera_mode': mode, 'collision': str(collision_path),
                        'mesh_assets': len([a for a in managed['assets'] if a['kind'] == 'mesh']),
                        'source_sha256': source_digest})
    report = {'status': 'passed', 'workspace': str(workspace.parent), 'projects': reports}
    jobs.atomic_json(output/'template-import-report.json', report)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
