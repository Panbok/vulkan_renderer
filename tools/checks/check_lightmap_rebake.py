#!/usr/bin/env python3
"""GPU check of project lightmap rebakes (ADR-088): unchanged skips, full
rebakes and build pruning.

Through `vkr_bakery project` on a small managed scene of box brushes, four
static lamps and an atmosphere sun, this checks that:

- a second bake of an unchanged scene keeps the set, its record and its build
  revision and writes no new revision;
- the request's `full` bakes again although nothing changed;
- after a lamp edit the bake is incremental: it builds on the previous set,
  which its record names as `previous_build`, and the revision before that
  goes while the current and previous revisions stay;
- every record carries the set's input index as its `lightmap_index`
  artifact and says how the set was produced.

It bakes on the host's GPU (Metal ray tracing or Vulkan ray queries) in a few
seconds per bake and needs a Release vkr_bakery.

Example:
  python3 tools/checks/check_lightmap_rebake.py --bakery build_release/tools/bakery/vkr_bakery
"""
import argparse
from pathlib import Path
import sys
import tempfile
import time
import uuid

import project_jobs as jobs


def street_entities(lamp_intensity):
    """Internal-form entities: a floor, two walls and three props as box
    brushes, and four static point lamps."""
    entities = []

    def box(name, pos, scale):
        first = len(entities)
        entities.append({'id': str(uuid.uuid5(uuid.NAMESPACE_URL, 'rebake/' + name)), 'name': name,
                         'parent': None,
                         'transform': {'pos': pos, 'rot': [0, 0, 0, 1], 'scale': scale},
                         'brush': {'role': 'solid'}})
        for i, normal in enumerate(([1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0],
                                    [0, 0, 1], [0, 0, -1])):
            entities.append({'id': str(uuid.uuid5(uuid.NAMESPACE_URL, f'rebake/{name}/{i}')),
                             'name': f'{name} face {i}', 'parent': first,
                             'brush_face': {'normal': normal, 'distance': 0.5}})

    box('floor', [0, -0.5, 0], [12, 1, 24])
    box('wall west', [-6.5, 2, 0], [1, 4, 24])
    box('wall east', [6.5, 1.5, 0], [1, 3, 24])
    for i, (x, z) in enumerate(((-3, -8), (2, 0), (-2, 8))):
        box(f'prop {i}', [x, 0.75, z], [1.5, 1.5, 1.5])
    for i, (x, z) in enumerate(((-4, -9), (4, -3), (-4, 3), (4, 9))):
        entities.append({'id': str(uuid.uuid5(uuid.NAMESPACE_URL, f'rebake/lamp {i}')),
                         'name': f'lamp {i}', 'parent': None,
                         'transform': {'pos': [x, 3, z], 'rot': [0, 0, 0, 1], 'scale': [1, 1, 1]},
                         'point_light': {'enabled': True, 'color': [1.0, 0.8, 0.6],
                                         'intensity': lamp_intensity[i], 'range': 8.0,
                                         'mobility': 'static'}})
    return entities


def lightmap_record(scene_path):
    scene = jobs.read_managed_scene(scene_path)
    reference = scene['lightmaps'][host_class()]['asset']
    return next(record for record in scene['assets'] if record['id'] == reference['id'])


def host_class():
    return 'tiled' if sys.platform == 'darwin' else 'desktop'


def revision(record):
    return record['artifacts'][0]['path'].split('/')[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bakery', default=str(jobs.default_bakery()))
    args = parser.parse_args()
    bakery = Path(args.bakery).resolve()
    with tempfile.TemporaryDirectory(prefix='vkr-lightmap-rebake-') as temporary:
        root = Path(temporary)
        workspace = root / 'workspace' / '.vkreditor'
        project = workspace / 'projects' / str(uuid.uuid4())
        project.mkdir(parents=True)
        manifest = project / 'project.json'
        jobs.atomic_json(manifest, {'version': 1, 'assets': [], 'scenes': [], 'default_font': None})
        result_path = root / 'result.json'
        create = {'version': 1, 'operation': 'create_scene', 'workspace_root': str(workspace),
                  'project_path': str(manifest), 'scene_id': str(uuid.uuid4()),
                  'scene_name': 'Lightmap rebake', 'models': [], 'bakes': {},
                  'tools': {'mesh': str(bakery), 'texture': str(bakery)}}
        assert jobs.Job(create, result_path).execute() == 0, result_path.read_text()
        scene_path = Path(jobs.load_json(result_path)['scene_path'])
        builds = scene_path.parent / 'builds'

        def edit(lamps):
            scene = jobs.read_managed_scene(scene_path)
            scene['entities'] = street_entities(lamps)
            scene['atmosphere'] = {'enabled': True, 'sun_direction': [0.3, 0.6, 0.5]}
            jobs.write_managed_scene(scene_path, scene)

        def bake(**extra):
            request = dict(create, operation='bake_scene', scene_path=str(scene_path),
                           bakes={'lightmap': True}, **extra)
            started = time.monotonic()
            job = jobs.Job(request, result_path)
            assert job.execute() == 0, result_path.read_text() + job.output
            return job.output, time.monotonic() - started

        edit([40.0, 40.0, 40.0, 40.0])
        _, first_seconds = bake()
        first = lightmap_record(scene_path)
        assert 'previous_build' not in first, first
        assert [artifact['role'] for artifact in first['artifacts']] == ['lightmap', 'lightmap_index']
        assert first['production']['kind'] == 'full', first['production']
        assert sorted(path.name for path in builds.iterdir()) == [revision(first)]

        # Nothing changed: the set, its record and its revision stay.
        output, unchanged_seconds = bake()
        assert 'Lightmaps unchanged' in output, output
        assert lightmap_record(scene_path) == first
        assert sorted(path.name for path in builds.iterdir()) == [revision(first)]

        # A full rebake bakes again though nothing changed; it names the set
        # it replaced as its previous build.
        output, full_seconds = bake(full=True)
        assert 'Lightmaps unchanged' not in output, output
        second = lightmap_record(scene_path)
        assert second['id'] == first['id'] and revision(second) != revision(first)
        assert second['previous_build'] == first['artifacts'][0]['path'], second
        assert second['production']['kind'] == 'full', second['production']

        # A lamp edit bakes a third revision; the first, two revisions back,
        # goes, and the current and previous ones stay.
        edit([40.0, 80.0, 40.0, 40.0])
        _, edited_seconds = bake()
        third = lightmap_record(scene_path)
        assert third['previous_build'] == second['artifacts'][0]['path'], third
        production = third['production']
        assert production['kind'] == 'incremental', production
        assert production['base_sha256'] == second['fingerprint'], production
        assert production['gathered_texels'] < production['total_texels'], production
        kept = sorted(path.name for path in builds.iterdir())
        assert kept == sorted([revision(second), revision(third)]), kept
        print(f'check_lightmap_rebake: passed ({host_class()} host; first bake '
              f'{first_seconds:.1f} s, unchanged {unchanged_seconds:.1f} s, full '
              f'{full_seconds:.1f} s, after a lamp edit {edited_seconds:.1f} s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
