#!/usr/bin/env python3
"""Check `vkr_bakery bundle <project>` on a small synthetic managed project.

The project is made by the job runner itself: `create_project` installs the
editor bundle and `create_scene` imports a textured triangle. Its World
places a collider that names a cooked collision file by a workspace path. The
package is read with the independent `.vkpak` parser of
check_bakery_bundle.py. The check asserts that no archived document names an
absolute or workspace path, the World and startup scene ship, the World's
bytes are unchanged and its overlay is rewritten to content identities, the
archives validate, a failed build keeps the earlier package, a refused output
folder is left alone, and a cancelled build leaves no package.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import time
import uuid

import project_jobs as jobs
from check_bakery_bundle import read_pack

REPOSITORY = Path(__file__).resolve().parents[2]


def triangle_gltf(path):
    positions = struct.pack('<9f', 0, 0, 0, 1, 0, 0, 0, 1, 0)
    indices = struct.pack('<3H', 0, 1, 2) + b'\0\0'
    binary = positions + indices
    path.write_text(json.dumps({
        'asset': {'version': '2.0'},
        'buffers': [{'byteLength': len(binary), 'uri': 'data:application/octet-stream;base64,' +
                     base64.b64encode(binary).decode()}],
        'bufferViews': [{'buffer': 0, 'byteLength': 36},
                        {'buffer': 0, 'byteOffset': 36, 'byteLength': 6}],
        'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3',
                       'min': [0, 0, 0], 'max': [1, 1, 0]},
                      {'bufferView': 1, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0}, 'indices': 1}]}],
        'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}))


def tree_digest(directory):
    return {path.relative_to(directory).as_posix(): jobs.digest(path)
            for path in sorted(directory.rglob('*')) if path.is_file()}


def latest_report(workspace):
    """The newest build report; names order by second only."""
    return max((workspace / 'logs' / 'builds').glob('*.json'), key=lambda path: path.stat().st_mtime_ns)


def strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for item in value.values():
            yield from strings(item)
    elif isinstance(value, list):
        for item in value:
            yield from strings(item)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bakery', default=str(jobs.default_bakery()))
    args = parser.parse_args()
    bakery = Path(args.bakery).resolve()
    with tempfile.TemporaryDirectory(prefix='vkr-package-') as temporary:
        root = Path(temporary).resolve()
        workspace = root / 'workspace' / '.vkreditor'
        project_id = str(uuid.uuid4())
        project = workspace / 'projects' / project_id
        project.mkdir(parents=True)
        (workspace / 'workspace.json').write_text(json.dumps({'version': 1, 'id': str(uuid.uuid4())}))
        manifest = project / 'project.json'
        manifest.write_text(json.dumps({'version': 1, 'id': project_id, 'name': 'Fixture Game',
                                        'assets': [], 'scenes': [], 'default_font': None}))
        bootstrap = root / 'bootstrap'
        (bootstrap / 'fonts').mkdir(parents=True)
        for name in ('UbuntuMono-cooked.fontcfg', 'UbuntuMono-cooked.vkfa', 'UbuntuMono-R.ttf'):
            (bootstrap / 'fonts' / name).write_bytes((REPOSITORY / 'assets' / 'fonts' / name).read_bytes())
        base = {'version': 1, 'workspace_root': str(workspace), 'project_path': str(manifest),
                'bootstrap_directory': str(bootstrap)}
        result_path = root / 'result.json'
        job = jobs.Job(dict(base, operation='create_project'), result_path, bakery)
        assert job.execute() == 0, job.output

        # One textured triangle scene, published into the project by hand as
        # the editor store publishes membership.
        source = root / 'sources'
        source.mkdir()
        (source / 'pixel.png').write_bytes(base64.b64decode(
            'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aTWQAAAAASUVORK5CYII='))
        (source / 'test.mtl').write_text('newmtl test\nKd 1 0 0\nmap_Kd pixel.png\n')
        (source / 'test.obj').write_text('mtllib test.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\n'
                                         'vt 0 1\nvn 0 0 1\nusemtl test\nf 1/1/1 2/2/1 3/3/1\n')
        scene_id = str(uuid.uuid4())
        job = jobs.Job(dict(base, operation='create_scene', scene_id=scene_id, scene_name='Arena',
                            models=[str(source / 'test.obj')], bakes={}), result_path, bakery)
        assert job.execute() == 0, job.output
        document = json.loads(manifest.read_text())
        document['scenes'] = [{'id': scene_id, 'name': 'Arena', 'path': f'scenes/{scene_id}/scene.json'}]
        manifest.write_text(json.dumps(document))

        # The World: one sun, and an overlay collider naming a cooked shape by
        # its workspace path.
        triangle_gltf(root / 'shape.gltf')
        (project / 'collision').mkdir()
        cooked = project / 'collision' / 'shape.vkc'
        cooking = subprocess.run([str(bakery), 'tool', 'collision', '--input', str(root / 'shape.gltf'),
                                  '--output', str(cooked), '--kind', 'mesh'], capture_output=True, text=True)
        assert cooking.returncode == 0, cooking.stdout + cooking.stderr
        world = project / 'world.scene.json'
        world.write_text(json.dumps({'version': 2, 'entities': [{
            'name': 'Directional Light', 'parent': None,
            'transform': {'pos': [0, 3, 0], 'rot': [0, 0, 0, 1], 'scale': [1, 1, 1]},
            'directional_light': {'enabled': True, 'intensity': 3}}]}))
        workspace_shape = f'projects/{project_id}/collision/shape.vkc'
        (project / 'world.editor.json').write_text(json.dumps({'version': 4, 'overrides': [{
            'scene_entity': 0, 'gltf_node': -1, 'source_fingerprint': '0' * 16, 'fields': 0,
            'physics': {'colliders': [{'shape': 4, 'asset': workspace_shape}]}}]}))

        # game.json: the scene starts, a shipping profile names the output.
        out = root / 'builds' / 'Fixture'
        game = {'version': 1,
                'game': {'name': 'Fixture Game', 'version': '1.2.3', 'company': 'VKR Checks',
                         'executable': 'Fixture Game', 'startup_scene': scene_id, 'scenes': [scene_id],
                         'window': {'mode': 'windowed', 'width': 1280, 'height': 720},
                         'graphics': {'version': 1, 'vsync': False}},
                'profiles': [{'name': 'Host Shipping', 'platform': 'host', 'config': 'shipping',
                              'output': str(out), 'include': [], 'bake_lighting': False}]}
        (project / 'game.json').write_text(json.dumps(game))
        command = [str(bakery), 'bundle', str(project), '--profile', 'Host Shipping', '--json',
                   '--cache', str(root / 'cache')]
        workspace_before = tree_digest(project)
        built = subprocess.run(command, capture_output=True, text=True, timeout=300)
        assert built.returncode == 0, built.stdout + built.stderr
        assert not Path(str(out) + '.staging').exists()

        description = json.loads((out / 'bundle.json').read_text())
        assert description['version'] == 2 and description['config'] == 'shipping'
        suffix = '.exe' if description['platform'].startswith('windows') else ''
        assert (out / ('Fixture Game' + suffix)).is_file()
        assert (out / 'shaders' / description['backend'] / 'shader_manifest.json').is_file()
        assert not (out / 'shaders' / ('vulkan' if description['backend'] == 'metal' else 'metal')).exists()
        assert description['packs'] == ['content/game.vkpak', 'content/engine.vkpak']
        entries = {}
        for pack in description['packs']:
            pack_entries, _ = read_pack(out / pack)
            assert not set(entries) & set(pack_entries)
            entries.update(pack_entries)
        assert set(entries) == set(description['products'])
        assert all(name.startswith('assets/') for name in read_pack(out / 'content/engine.vkpak')[0])
        for identity, (data, _) in entries.items():
            assert hashlib.sha256(data).hexdigest() == description['products'][identity], identity

        # No archived document names the workspace, the repository or an
        # absolute path.
        for identity, (data, _) in entries.items():
            if identity.endswith(('.json', '.mt', '.fontcfg')):
                text = data.decode('utf-8')
                assert str(root) not in text and str(REPOSITORY) not in text, identity
                if identity.endswith('.json'):
                    for value in strings(json.loads(text)):
                        assert not value.startswith('/') and value[1:3] != ':\\', (identity, value)

        game_object = description['game']
        assert game_object['name'] == 'Fixture Game' and game_object['company'] == 'VKR Checks'
        assert game_object['window'] == game['game']['window']
        assert game_object['graphics'] == game['game']['graphics']
        assert game_object['world'] == 'project/world.scene.json'
        assert entries['project/world.scene.json'][0] == world.read_bytes(), 'the World ships byte-identical'
        overlay = json.loads(entries[game_object['world_overlay']][0])
        collider = overlay['overrides'][0]['physics']['colliders'][0]['asset']
        assert collider == 'project/collision/shape.vkc'
        assert entries[collider][0] == cooked.read_bytes()
        startup = game_object['startup_scene']
        assert startup == f'project/scenes/{scene_id}/runtime.scene.json'
        assert game_object['scenes'][0]['scene'] == startup
        assert game_object['startup_overlay'] in entries
        runtime = json.loads(entries[startup][0])
        mesh = runtime['entities'][0]['mesh']['path']
        assert mesh.startswith(f'project/scenes/{scene_id}/builds/') and mesh in entries
        materials = [name for name in entries if name.endswith('.mt')]
        textures = [name for name in entries if name.endswith('.vkt')]
        assert materials and textures, sorted(entries)
        fonts = {font['name']: font['config'] for font in game_object['fonts']}
        assert fonts['default-scene-font'] in entries
        assert 'assets/render_graphs/main.rendergraph.json' in entries
        report_events = [json.loads(line) for line in built.stdout.splitlines() if line.startswith('{')]
        stages = [event['source'] for event in report_events if event.get('ev') == 'start']
        assert sum(event.get('ev') == 'done' and event.get('status') == 'ok'
                   for event in report_events) == len(stages), report_events
        assert stages == ['Validate', 'Finalize', 'Bake', 'Lower', 'Pack', 'Stage runtime',
                          'Verify and report'], stages
        report = json.loads(latest_report(workspace).read_text())
        assert report['status'] == 'complete' and report['output'] == str(out)
        assert report['package']['files'] == len(entries)
        assert tree_digest(project) == workspace_before, 'a package build must not write the project'

        # An unchanged rebuild reuses both archives byte for byte.
        archives_before = {name: jobs.digest(out / 'content' / name)
                           for name in ('game.vkpak', 'engine.vkpak')}
        rebuilt = subprocess.run(command, capture_output=True, text=True, timeout=300)
        assert rebuilt.returncode == 0, rebuilt.stdout + rebuilt.stderr
        report = json.loads(latest_report(workspace).read_text())
        assert all(archive['reused'] for archive in report['package']['archives']), report
        assert {name: jobs.digest(out / 'content' / name) for name in archives_before} == archives_before

        # A failed stage keeps the earlier package.
        package_before = tree_digest(out)
        world.write_text(json.dumps({'version': 2, 'entities': [{'name': 'Bad', 'mesh': {'path': str(cooked)}}]}))
        failed = subprocess.run(command, capture_output=True, text=True, timeout=300)
        assert failed.returncode == 1, failed.stdout + failed.stderr
        assert any(event.get('ev') == 'diag' and 'absolute path' in event.get('message', '')
                   for event in (json.loads(line) for line in failed.stdout.splitlines()
                                 if line.startswith('{'))), failed.stdout
        assert tree_digest(out) == package_before
        assert not Path(str(out) + '.staging').exists()
        world.write_text(json.dumps({'version': 2, 'entities': []}))

        # A window mode the player does not know is refused before packaging.
        modes = json.loads((project / 'game.json').read_text())
        modes['game']['window']['mode'] = 'exclusive'
        (project / 'game.json').write_text(json.dumps(modes))
        refused_mode = subprocess.run(command, capture_output=True, text=True, timeout=300)
        assert refused_mode.returncode == 1 and 'window mode' in refused_mode.stdout
        assert tree_digest(out) == package_before
        modes['game']['window']['mode'] = 'borderless'
        (project / 'game.json').write_text(json.dumps(modes))

        # An existing folder that is not a package is refused and untouched.
        foreign = root / 'builds' / 'Documents'
        foreign.mkdir()
        (foreign / 'notes.txt').write_text('keep me')
        refused = subprocess.run(command + ['--out', str(foreign)], capture_output=True, text=True,
                                 timeout=300)
        assert refused.returncode == 1 and tree_digest(foreign) == {'notes.txt': jobs.digest(foreign / 'notes.txt')}

        # A project asset imported at the preview tier is finalized for the
        # package; the report returns the final inventory, which the editor
        # publishes, and project.json stays as it was.
        job = jobs.Job(dict(base, operation='import_project_assets', texture_tier='preview',
                            sources=[str(source / 'test.obj')]), result_path, bakery)
        assert job.execute() == 0, job.output
        document = json.loads(manifest.read_text())
        document['assets'] = jobs.load_json(result_path)['project_assets']
        assert any(asset.get('texture_tier') == 'preview' for asset in document['assets'])
        manifest.write_text(json.dumps(document))
        manifest_before = manifest.read_bytes()
        finalized = subprocess.run(command, capture_output=True, text=True, timeout=300)
        assert finalized.returncode == 0, finalized.stdout + finalized.stderr
        assert manifest.read_bytes() == manifest_before, 'the editor publishes project.json'
        report = json.loads(latest_report(workspace).read_text())
        finalize = next(stage for stage in report['stages'] if stage['name'] == 'Finalize')
        assert 'detail' not in finalize, finalize
        assert report['project_assets'] and not any(
            asset.get('texture_tier') in ('preview', 'deferred') for asset in report['project_assets'])
        # Changed game content rewrites only the game archive.
        reused = {archive['path']: archive['reused'] for archive in report['package']['archives']}
        assert reused == {'content/game.vkpak': False, 'content/engine.vkpak': True}, reused

        # Cancellation once staging exists leaves no package.
        cancelled_out = root / 'builds' / 'Cancelled'
        staging = Path(str(cancelled_out) + '.staging')
        process = subprocess.Popen(command + ['--out', str(cancelled_out)], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 60
        while not staging.exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.002)
        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=120)
        assert process.returncode == 3, (process.returncode, stdout[-2000:], stderr[-2000:])
        assert any(json.loads(line).get('status') == 'cancelled' for line in stdout.splitlines()
                   if line.startswith('{')), stdout
        assert not cancelled_out.exists() and not staging.exists()
    print('Bakery package: layout, two archives, portable documents, byte-identical World, rewritten '
          'overlay, startup scene, stage events, report, kept package on failure, refused folder, '
          'finalized project inventory, archive reuse and cancellation passed')


if __name__ == '__main__':
    main()
