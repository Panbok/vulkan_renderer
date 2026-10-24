#!/usr/bin/env python3
"""Check the depot merge drivers and the pull report of `vkr_bakery project`.

`merge_scene_unit` and `merge_project_document` get each side's member files
in `base`, `mine` and `theirs` folders, as the depot's merge writes them
(docs/proposals/depot-git-model.md, "Merge"). The check asserts that:

- inventory records that each side added, and a record one side changed,
  all reach the merged inventory, which `scene.json` names;
- entities one side appended stay, after the base entities, and the
  overlay the other side changed is the one `scene.json` names;
- both sides appending entities keeps both appends in order;
- a record both sides changed apart, and an overlay both sides changed
  that does not merge per entity, are conflicts that write nothing;
- overlays both sides changed merge per entity: entities each side moved
  or appended (numbered anew through `document_ids`), created entities
  renumbered on a clash with their parents kept, and two fields of one
  entity; one field both sides changed apart is the only conflict;
- `project.json` keeps the scenes each side added and reports a name both
  sides changed apart as a conflict;
- `pull_report` re-cooks a scene stripped of its cooked textures, as a
  pull from another host leaves it, and reports that it opens.
"""
import argparse
import json
from pathlib import Path
import tempfile
import uuid

import project_jobs as jobs
from check_bakery_recook import write_model


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2))


def record(id_, name, **fields):
    return dict({'id': id_, 'kind': 'texture', 'name': name}, **fields)


def entity(id_, name):
    return {'id': id_, 'name': name, 'parent': None, 'tags': {}, 'components': {}}


def write_side(root, entities, records, overlay, overlay_name, inventory_name):
    write_json(root / 'scene.json', {
        'version': 5, 'id': 'scene', 'entities': entities, 'reflection_probes': [],
        'default_font': None, 'edit_overlay': f'edits/{overlay_name}.json',
        'inventory': f'inventory/{inventory_name}.json'})
    write_json(root / 'inventory' / f'{inventory_name}.json', {'version': 1, 'assets': records})
    write_json(root / 'edits' / f'{overlay_name}.json', overlay)


def merge(root, bakery, name, operation, anchor, sides):
    folder = root / name
    for side, writer in sides.items():
        (folder / side).mkdir(parents=True)
        writer(folder / side)
    (folder / 'output').mkdir()
    result_path = folder / 'result.json'
    job = jobs.Job({'version': 1, 'read_only': True, 'operation': operation,
                    'workspace_root': str(root), 'anchor': anchor,
                    'base': str(folder / 'base'), 'mine': str(folder / 'mine'),
                    'theirs': str(folder / 'theirs'), 'output': str(folder / 'output')},
                   result_path, bakery=bakery)
    assert job.execute() == 0, job.output
    return jobs.load_json(result_path), folder / 'output'


def override(index, name, position, node=-1):
    """An override record as the editor writes one: every field the entity
    has, with `scene_entity` indexing the file's `document_ids`."""
    return {'scene_entity': index, 'gltf_node': node,
            'source_fingerprint': '0000000000000000', 'parent': None, 'fields': 3,
            'name': name, 'position': position, 'rotation': [0.0, 0.0, 0.0, 1.0],
            'scale': [1.0, 1.0, 1.0], 'components': {}}


def created(id_, uuid_, name, parent=None):
    return {'id': id_, 'uuid': uuid_, 'parent': parent, 'fields': 3, 'name': name,
            'position': [0.0, 0.0, 0.0], 'rotation': [0.0, 0.0, 0.0, 1.0],
            'scale': [1.0, 1.0, 1.0], 'components': {'tags': {'tags': name}}}


def overlay(ids, overrides, created_records):
    return {'version': 5, 'document_ids': ids, 'overrides': overrides,
            'created': created_records, 'scene_settings': {'inherit_world': True}}


def check_overlay_per_entity(root, bakery, anchor, records):
    """Overlays both sides changed merge per entity: an entity each side
    moved, entities each side appended and edited (whose indices differ
    between the sides' files), created entities with the same id on both
    sides and a parent naming one, and two fields of one entity; one field
    both sides changed apart is the only conflict."""
    base_entities = [entity('e1', 'floor'), entity('e2', 'wall')]
    floor = [0.0, 0.0, 0.0]
    base_overlay = overlay(['e1', 'e2'], [override(0, 'floor', floor)],
                           [created(1, 'c1', 'box')])

    def side(entities, edits, name):
        def writer(folder):
            write_side(folder, entities, records, edits, name, 'i0')
        return writer

    mine_entities = base_entities + [entity('e3', 'lamp')]
    theirs_entities = base_entities + [entity('e4', 'door')]
    mine_overlay = overlay(
        ['e1', 'e2', 'e3'],
        [override(0, 'floor', [1.0, 0.0, 0.0]), override(2, 'lamp', [0.0, 3.0, 0.0])],
        [created(1, 'c1', 'box'), created(2, 'c2', 'lid', {'created': 1})])
    theirs_overlay = overlay(
        ['e1', 'e2', 'e4'],
        [override(0, 'Floor', floor), override(1, 'wall', [0.0, 0.0, 2.0]),
         override(2, 'door', [5.0, 0.0, 0.0])],
        [created(1, 'c1', 'crate'), created(2, 'c3', 'barrel')])
    result, output = merge(root, bakery, 'per-entity', 'merge_scene_unit', anchor,
                           {'base': side(base_entities, base_overlay, 'o0'),
                            'mine': side(mine_entities, mine_overlay, 'o1'),
                            'theirs': side(theirs_entities, theirs_overlay, 'o2')})
    assert result['merged'] is True, result
    scene = json.loads((output / 'scene.json').read_text())
    assert [e['id'] for e in scene['entities']] == ['e1', 'e2', 'e3', 'e4']
    assert scene['edit_overlay'] not in ('edits/o1.json', 'edits/o2.json'), scene
    merged = json.loads((output / scene['edit_overlay']).read_text())
    ids = merged['document_ids']
    by_entity = {ids[r['scene_entity']]: r for r in merged['overrides']}
    assert set(by_entity) == {'e1', 'e2', 'e3', 'e4'}, merged['overrides']
    # e1: mine moved it, theirs renamed it; both reach the record.
    assert by_entity['e1']['position'] == [1.0, 0.0, 0.0] and \
        by_entity['e1']['name'] == 'Floor', by_entity['e1']
    assert by_entity['e2']['position'] == [0.0, 0.0, 2.0]
    assert by_entity['e3']['name'] == 'lamp' and by_entity['e4']['name'] == 'door'
    # Theirs' index 2 named e4; in the merged file e4 has its own index.
    assert ids.index('e4') == 3 and ids.index('e3') == 2, ids
    by_uuid = {r['uuid']: r for r in merged['created']}
    assert by_uuid['c1']['name'] == 'crate' and by_uuid['c1']['id'] == 1, by_uuid
    assert by_uuid['c2']['id'] == 2 and by_uuid['c2']['parent'] == {'created': 1}
    # c3 had id 2 on theirs, which mine's c2 holds: it gets a new one.
    assert by_uuid['c3']['id'] == 3, by_uuid
    assert merged['scene_settings'] == {'inherit_world': True}
    assert list(merged)[:3] == ['version', 'document_ids', 'overrides'], list(merged)

    apart = overlay(['e1', 'e2'], [override(0, 'floor', [9.0, 0.0, 0.0])],
                    [created(1, 'c1', 'box')])
    result, output = merge(root, bakery, 'per-entity-apart', 'merge_scene_unit', anchor,
                           {'base': side(base_entities, base_overlay, 'o0'),
                            'mine': side(base_entities, mine_overlay, 'o1'),
                            'theirs': side(base_entities, apart, 'o3')})
    assert result['merged'] is False, result
    conflicts = [(c['path'], c['id']) for c in result['conflicts']]
    assert conflicts == [('edits floor (e1/-1)', 'position')], result
    assert not any(output.iterdir()), 'A conflict writes nothing'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bakery', default=str(jobs.default_bakery()))
    args = parser.parse_args()
    anchor = '.vkreditor/projects/p/scenes/s/scene.json'
    with tempfile.TemporaryDirectory(prefix='vkr-merge-') as temporary:
        root = Path(temporary).resolve()
        base_entities = [entity('e1', 'floor')]
        base_records = [record('r1', 'stone', size=1), record('r2', 'brick')]
        base_overlay = {'version': 5, 'overrides': []}

        def base(side):
            write_side(side, base_entities, base_records, base_overlay, 'o0', 'i0')

        def mine(side):
            write_side(side, base_entities + [entity('e2', 'crate')],
                       base_records + [record('r3', 'wood')], base_overlay, 'o0', 'i1')

        def theirs(side):
            write_side(side, base_entities,
                       [record('r1', 'stone', size=2), record('r2', 'brick'),
                        record('r4', 'glass')],
                       {'version': 5, 'overrides': [{'scene_entity': 0, 'name': 'Floor'}]},
                       'o1', 'i2')

        result, output = merge(root, args.bakery, 'scene', 'merge_scene_unit', anchor,
                               {'base': base, 'mine': mine, 'theirs': theirs})
        assert result['merged'] is True, result
        scene = json.loads((output / 'scene.json').read_text())
        assert 'scene.json' in result['write'] and scene['inventory'] in result['write'], result
        inventory = json.loads((output / scene['inventory']).read_text())
        assert [(r['id'], r.get('size')) for r in inventory['assets']] == \
            [('r1', 2), ('r2', None), ('r3', None), ('r4', None)], inventory
        assert [e['id'] for e in scene['entities']] == ['e1', 'e2'], scene['entities']
        assert scene['edit_overlay'] == 'edits/o1.json', scene

        def mine_appends(side):
            write_side(side, base_entities + [entity('e2', 'crate')], base_records,
                       base_overlay, 'o0', 'i0')

        def theirs_appends(side):
            write_side(side, base_entities + [entity('e3', 'lamp')], base_records,
                       base_overlay, 'o0', 'i0')

        result, output = merge(root, args.bakery, 'append', 'merge_scene_unit', anchor,
                               {'base': base, 'mine': mine_appends, 'theirs': theirs_appends})
        scene = json.loads((output / 'scene.json').read_text())
        assert result['merged'] and [e['id'] for e in scene['entities']] == ['e1', 'e2', 'e3']
        assert scene['inventory'] == 'inventory/i0.json', 'Unchanged records keep their file'

        def mine_changes_r2(side):
            write_side(side, base_entities, [record('r1', 'stone', size=1),
                                             record('r2', 'brick', size=5)],
                       {'version': 5, 'overrides': [{'scene_entity': 0, 'hidden': True}]},
                       'o2', 'i3')

        def theirs_changes_r2(side):
            write_side(side, base_entities, [record('r1', 'stone', size=1),
                                             record('r2', 'brick', size=6)],
                       {'version': 5, 'overrides': [{'scene_entity': 0, 'name': 'Floor'}]},
                       'o1', 'i4')

        result, output = merge(root, args.bakery, 'conflict', 'merge_scene_unit', anchor,
                               {'base': base, 'mine': mine_changes_r2,
                                'theirs': theirs_changes_r2})
        assert result['merged'] is False, result
        reasons = {(c['path'], c['id']) for c in result['conflicts']}
        assert ('inventory', 'r2') in reasons and ('edits', None) in reasons, result
        assert not any(output.iterdir()), 'A conflict writes nothing'

        check_overlay_per_entity(root, args.bakery, anchor, base_records)

        scenes = [{'id': 's1', 'name': 'One', 'path': 'scenes/s1/scene.json'}]

        def project(name, scene_list):
            def writer(side):
                write_json(side / 'project.json', {'version': 1, 'id': 'p', 'name': name,
                                                   'default_font': None, 'assets': [],
                                                   'scenes': scene_list})
            return writer

        two = {'id': 's2', 'name': 'Two', 'path': 'scenes/s2/scene.json'}
        three = {'id': 's3', 'name': 'Three', 'path': 'scenes/s3/scene.json'}
        project_anchor = '.vkreditor/projects/p/project.json'
        result, output = merge(root, args.bakery, 'project', 'merge_project_document',
                               project_anchor,
                               {'base': project('Arena', scenes),
                                'mine': project('Arena', scenes + [two]),
                                'theirs': project('Arena', scenes + [three])})
        merged = json.loads((output / 'project.json').read_text())
        assert result['merged'] and [s['id'] for s in merged['scenes']] == ['s1', 's2', 's3']
        result, _ = merge(root, args.bakery, 'renamed', 'merge_project_document',
                          project_anchor,
                          {'base': project('Arena', scenes), 'mine': project('Arena Mine', scenes),
                           'theirs': project('Arena Theirs', scenes)})
        assert result['merged'] is False and result['conflicts'][0]['id'] == 'name', result

        # The pull report: a scene stripped of its cooked textures, as a pull
        # from another host leaves it, is re-cooked and opens.
        workspace = root / 'workspace' / '.vkreditor'
        project_id = str(uuid.uuid4())
        project_dir = workspace / 'projects' / project_id
        project_dir.mkdir(parents=True)
        manifest = project_dir / 'project.json'
        manifest.write_text(json.dumps({'version': 1, 'id': project_id, 'name': 'Pull',
                                        'assets': [], 'scenes': [], 'default_font': None}))
        sources = root / 'sources'
        sources.mkdir()
        write_model(sources)
        environment = {'VKR_BAKERY_CACHE': str(root / 'cache')}
        scene_id = str(uuid.uuid4())
        created_path = root / 'create.json'
        job = jobs.Job({'version': 1, 'operation': 'create_scene', 'workspace_root': str(workspace),
                        'project_path': str(manifest), 'scene_id': scene_id,
                        'scene_name': 'Pulled', 'models': [str(sources / 'model.gltf')],
                        'bakes': {}}, created_path, bakery=args.bakery, environment=environment)
        assert job.execute() == 0, job.output
        document = json.loads(manifest.read_text())
        document['scenes'] = [{'id': scene_id, 'name': 'Pulled',
                               'path': f'scenes/{scene_id}/scene.json'}]
        manifest.write_text(json.dumps(document))
        # The build textures the recipes name; a depot copy lacks them, as
        # it lacks every .vkt. The builds' `dependencies/` copies are not
        # needed to open the scene and are not re-cooked.
        stripped = [path for path in (project_dir / 'scenes' / scene_id).rglob('*.vkt')
                    if path.parent.name == 'textures']
        for path in stripped:
            path.unlink()
        assert stripped, 'The created scene has cooked textures to strip'
        report_path = root / 'pull.json'
        job = jobs.Job({'version': 1, 'operation': 'pull_report',
                        'workspace_root': str(workspace), 'project_path': str(manifest),
                        'scenes': [scene_id]}, report_path, bakery=args.bakery,
                       environment=environment)
        assert job.execute() == 0, job.output
        report = jobs.load_json(report_path)
        assert report['scenes_open'] == 1, report
        entry = report['scenes'][0]
        assert entry['scene_id'] == scene_id and entry['opens'] is True, entry
        assert entry['recooked'] >= 1 and entry['skipped'] == 0, entry
        missing = [str(p.relative_to(project_dir)) for p in stripped if not p.is_file()]
        assert not missing, missing
    print('Bakery merge: scene units merge records, appended entities and one-sided overlay '
          'edits, refuse two-sided changes, project documents keep both sides\' scenes, and '
          'the pull report re-cooks and opens a pulled scene')


if __name__ == '__main__':
    main()
