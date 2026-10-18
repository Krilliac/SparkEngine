"""Check preserved EditorFPSLineage_ payloads; caller validates native lifecycle.

Hashes are runner measurements, not offline-retained executable verification.
Source/config come from the calling qualification envelope, not binary inference.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import xml.etree.ElementTree as ET

TEXT_CAP = 128 * 1024
PAYLOAD_CAP = 1024 * 1024 * 1024
CASES = ('positive', 'malformed', 'missing-asset', 'missing-module')
TESTS = ('EditorFPSLineage_AuthoredSceneLoadedByInstalledFPS',
         'EditorFPSLineage_MalformedStartupFailsClosed',
         'EditorFPSLineage_MissingCookedAssetFailsClosed',
         'EditorFPSLineage_MissingRealModuleFailsClosed')


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def regular(path):
    path = Path(os.path.abspath(path))
    for part in (path, *path.parents):
        info = part.lstat()
        require(not stat.S_ISLNK(info.st_mode) and
                not getattr(info, 'st_file_attributes', 0) & 0x400,
                f'link/reparse path: {path}')
    require(stat.S_ISREG(path.stat().st_mode), f'not a regular file: {path}')
    return path


def read_text(path):
    path = regular(path)
    require(0 < path.stat().st_size <= TEXT_CAP, f'text size: {path}')
    with path.open('rb') as stream:
        data = stream.read(TEXT_CAP + 1)
    require(0 < len(data) <= TEXT_CAP and b'\0' not in data, f'text bytes: {path}')
    return data.decode('utf-8', errors='strict')


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        require(key not in value, f'duplicate JSON key: {key}')
        value[key] = item
    return value


def document(path):
    return json.loads(read_text(path), object_pairs_hook=unique_object,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))


def identity(path):
    path = regular(path)
    size = path.stat().st_size
    require(0 < size <= PAYLOAD_CAP, f'payload size: {path}')
    digest, count = hashlib.sha256(), 0
    with path.open('rb') as stream:
        while chunk := stream.read(1024 * 1024):
            count += len(chunk)
            require(count <= PAYLOAD_CAP, f'payload grew over cap: {path}')
            digest.update(chunk)
    require(count == size, f'payload changed size: {path}')
    return {'size': size, 'sha256': digest.hexdigest()}


def equal(paths):
    values = [identity(path) for path in paths]
    require(all(value == values[0] for value in values), f'payload mismatch: {paths}')
    return values[0]


def absent(path):
    require(not os.path.lexists(path), f'unexpected negative-case payload: {path}')


def authored_scene(path):
    scene = document(path)
    require(type(scene) is dict and type(scene.get('version')) is int and scene['version'] == 1,
            'reflected fixture version')
    entities = scene.get('entities')
    require(type(entities) is list and len(entities) == 2, 'authored entity count')
    names = {}
    ids = set()
    for entity in entities:
        require(type(entity) is dict and type(entity.get('name')) is str, 'entity name')
        require(entity['name'] not in names, 'duplicate entity name')
        require(type(entity.get('id')) is int and entity['id'] >= 0 and entity['id'] not in ids,
                'authored entity id')
        ids.add(entity['id'])
        require(type(entity.get('parent')) is int and entity['parent'] == -1, 'root fixture entity')
        components = entity.get('components')
        require(type(components) is list, 'component list')
        fields = {}
        for component in components:
            require(type(component) is dict and type(component.get('type')) is str and
                    type(component.get('fields')) is dict, 'component fields')
            require(component['type'] not in fields, 'duplicate component')
            fields[component['type']] = component['fields']
        names[entity['name']] = fields
    require(set(names) == {'Crate', 'AuthoredCamera'}, 'fixture entity identities')
    crate, camera = names['Crate'], names['AuthoredCamera']
    require(set(crate) == {'Transform', 'MeshRenderer'} and set(camera) == {'Transform', 'Camera'},
            'fixture component identities')
    require(crate['Transform'].get('position') == '3.000000,2.000000,5.000000', 'authored mesh position')
    require(crate['MeshRenderer'].get('meshPath') == 'Assets/Meshes/crate.obj', 'authored mesh reference')
    require(crate['MeshRenderer'].get('visible') == 'true' and
            crate['MeshRenderer'].get('materialPath') == '', 'authored visible material-free mesh')
    require(camera['Transform'].get('position') == '7.000000,11.000000,-13.000000' and
            camera['Camera'].get('isMainCamera') == 'true', 'authored main camera')
    for entity in (crate, camera):
        require(entity['Transform'].get('rotation') == '0.000000,0.000000,0.000000' and
                entity['Transform'].get('scale') == '1.000000,1.000000,1.000000', 'authored transform')
    require(camera['Camera'].get('fov') == '70.000000' and
            camera['Camera'].get('nearPlane') == '0.100000' and
            camera['Camera'].get('farPlane') == '1000.000000', 'authored camera projection')


def runtime_records(log, name, package):
    marker = 'SPARK_FPS_STARTUP'
    lines = [line for line in log.splitlines() if marker in line]
    reasons = {
        'malformed': 'Reflected gameplay scene rejected: primary scene is invalid or has unsupported reflected fields',
        'missing-asset': 'Reflected gameplay scene rejected: mesh must be an existing project-confined OBJ',
    }
    if name != 'positive':
        require(not lines, 'negative case committed startup')
        if name in reasons:
            require(reasons[name] in log and
                    'FPS packaged startup rejected: selected reflected scene failed to load' in log,
                    'negative case reason mismatch')
            other = reasons['missing-asset' if name == 'malformed' else 'malformed']
            require(other not in log, 'contradictory rejection reasons')
        else:
            expected = 'Explicit game module not found: ' + str(package / 'SparkGameFPS.dll')
            require(expected.replace('\\', '/') in log.replace('\\', '/'),
                    'missing-module rejection reason')
        return
    require(not any(reason in log for reason in reasons.values()) and
            'FPS packaged startup rejected:' not in log, 'positive case also rejected')
    require('Warning: Failed to load' not in log and 'Model: tinyobj error:' not in log and
            'Model: OBJ rejected' not in log, 'model loading failure')
    require(len(lines) == 6, 'exact startup record count')
    parsed = {}
    for line in lines:
        tokens = line.split(' ')
        tag = tokens[0]
        fields = {}
        for token in tokens[1:]:
            require(token.count('=') == 1, 'startup token syntax')
            key, value = token.split('=')
            require(key not in fields, 'duplicate startup key')
            fields[key] = value
        parsed.setdefault(tag, []).append(fields)
    require(set(parsed) == {marker, marker+'_NODE', marker+'_CAMERA', marker+'_PLAYER', marker+'_MESH'}, 'startup tags')
    require(parsed[marker] == [{'scene': 'Startup.sparkscene', 'nodes': '2', 'rendering': '1'}], 'startup summary')

    def numbers(text, expected):
        parts = text.split(',')
        require(len(parts) == len(expected), 'startup numeric arity')
        for value, target in zip(parts, expected):
            require(re.fullmatch(r'-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?', value), 'startup number')
            actual = float(value)
            require(math.isfinite(actual) and math.isclose(actual, target, rel_tol=1e-6, abs_tol=1e-6),
                    'startup value differs from authored fixture')

    nodes = parsed[marker+'_NODE']
    require(len(nodes) == 2 and {node.get('index') for node in nodes} == {'0', '1'} and
            {node.get('type') for node in nodes} == {'model', 'Camera'}, 'startup node identities')
    for node in nodes:
        require(set(node) == {'index', 'type', 'position', 'rotation', 'scale'}, 'startup node fields')
        numbers(node['position'], (3, 2, 5) if node['type'] == 'model' else (7, 11, -13))
        numbers(node['rotation'], (0, 0, 0))
        numbers(node['scale'], (1, 1, 1))
    mesh = parsed[marker+'_MESH']
    model_index = next(node['index'] for node in nodes if node['type'] == 'model')
    require(len(mesh) == 1 and set(mesh[0]) == {'index', 'position', 'scale'} and
            mesh[0]['index'] == model_index, 'live mesh identity')
    numbers(mesh[0]['position'], (3, 2, 5))
    numbers(mesh[0]['scale'], (1, 1, 1))
    for tag, keys in (('_CAMERA', {'position', 'fov', 'near', 'far'}), ('_PLAYER', {'position'})):
        records = parsed[marker+tag]
        require(len(records) == 1 and set(records[0]) == keys, 'startup camera/player fields')
        numbers(records[0]['position'], (7, 11, -13))
        if tag == '_CAMERA':
            for field, expected in (('fov', 70), ('near', 0.1), ('far', 1000)):
                numbers(records[0][field], (expected,))


def cook_assets(project, package, missing, installed_assets):
    cooked = project / 'Cooked/Assets'
    manifest = cooked / 'spark-cook-manifest.json'
    equal([manifest, package / 'Assets/spark-cook-manifest.json'])
    data = document(manifest)
    require(type(data) is dict and set(data) == {'schemaVersion', 'manifestSha256', 'assets'},
            'cook manifest fields')
    require(type(data['schemaVersion']) is int and data['schemaVersion'] == 1, 'cook schema')
    records = data['assets']
    expected_paths = ['Meshes/crate.obj', 'Models/pistol.obj', 'Models/rifle.obj']
    require(type(records) is list and len(records) == len(expected_paths), 'cook fixture asset count')
    body, assets = bytearray(), {}
    for record, relative in zip(records, expected_paths):
        require(type(record) is dict and set(record) == {'path', 'sha256', 'size'}, 'cook record fields')
        require(record['path'] == relative, 'cook fixture asset path/order')
        require(type(record['size']) is int and 0 < record['size'] <= PAYLOAD_CAP, 'cook asset size')
        require(type(record['sha256']) is str and re.fullmatch('[0-9a-f]{64}', record['sha256']),
                'cook asset digest')
        expected = {key: record[key] for key in ('size', 'sha256')}
        paths = [project / 'Assets' / relative, cooked / relative]
        if relative.startswith('Models/'):
            paths.append(installed_assets / relative)
        if missing and relative == 'Meshes/crate.obj':
            absent(package / 'Assets' / relative)
        else:
            paths.append(package / 'Assets' / relative)
        require(equal(paths) == expected, 'cook asset digest/size mismatch')
        body.extend(f"{relative}\0{record['sha256']}\0{record['size']}\n".encode('utf-8'))
        assets[relative] = expected
    require(data['manifestSha256'] == hashlib.sha256(body).hexdigest(), 'cook aggregate digest')
    return {'manifestFile': identity(manifest), 'aggregateSha256': data['manifestSha256'],
            'assets': assets}


def framework_proof(path):
    text = read_text(path)
    require('<!DOCTYPE' not in text and '<!ENTITY' not in text, 'XML declarations')
    tree = ET.fromstring(text)
    require(tree.tag == 'testsuites' and len(tree) == 1 and tree[0].tag == 'testsuite', 'framework suite')
    for node in (tree, tree[0]):
        require(node.get('tests') == '4' and
                all(node.get(key) == '0' for key in ('failures', 'skipped', 'flaky', 'empty')) and
                node.get('errors', '0') == '0', 'framework totals')
    cases = list(tree[0])
    require(len(cases) == 4 and {case.get('name') for case in cases} == set(TESTS), 'framework cases')
    for case in cases:
        require(case.tag == 'testcase' and not list(case), 'framework failure/skip content')
    for node in (tree, tree[0], *cases):
        duration = float(node.get('time', 'nan'))
        require(math.isfinite(duration) and duration >= 0, 'framework duration')
    return identity(path)


def validate(root, host, module, source_sha, config, framework_junit):
    framework = framework_proof(framework_junit)
    require(re.fullmatch('[0-9a-f]{40}', source_sha), 'source SHA')
    require(config in ('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel'), 'configuration')
    require(module.name == 'SparkGameFPS.dll', 'actual FPS module required')
    host_id, module_id = identity(host), identity(module)
    sidecar = Path(str(module) + '.sparkabi')
    sidecar_id = identity(sidecar)
    require(root.is_dir() and {p.name for p in root.iterdir()} == set(CASES), 'exact case set')
    result = {}
    for name in CASES:
        case = root / name
        project, package = case / 'CrateProject', case / 'Package'
        source_scene = project / 'Scenes/Default.sparkscene'
        authored_scene(source_scene)
        # Read complete bounded documents, not truncated prefixes or self-reported booleans.
        for path in (source_scene, case / 'reopened.sparkscene',
                     project / 'Cooked/Scenes/Default.sparkscene', package / 'Scenes/Startup.sparkscene'):
            document(path)
        scene = equal([source_scene, case / 'reopened.sparkscene',
                       project / 'Cooked/Scenes/Default.sparkscene', package / 'Scenes/Startup.sparkscene'])
        startup = package / 'Startup.sparkscene'
        absent(Path(str(startup) + '.bak'))
        if name == 'malformed':
            require(read_text(startup) == 'not a reflected scene\n', 'exact malformed mutation')
        else:
            require(identity(startup) == scene, 'module startup differs from authored scene')
        require(identity(package / 'Crate Game.exe') == host_id, 'installed host mismatch')
        if name == 'missing-module':
            absent(package / module.name)
        else:
            require(identity(package / module.name) == module_id, 'installed FPS mismatch')
        require(identity(package / sidecar.name) == sidecar_id, 'installed sidecar mismatch')
        modules = document(package / 'spark.modules.json')
        require(modules == {'modules': [{'name': 'SparkGameFPS', 'path': 'SparkGameFPS.dll',
                                         'loadOrder': 1000}]}, 'module manifest identity')
        for prefix in (project, project / 'Cooked'):
            project_doc = document(prefix / 'CrateProject.sparkproject')
            require(type(project_doc) is dict and
                    project_doc.get('defaultScene') == 'Scenes/Default.sparkscene', 'default scene')
        cooked = cook_assets(project, package, name == 'missing-asset', host.parent / 'Assets')
        log = read_text(case / 'runtime.log')
        require('... Output removed since' not in log, 'truncated runtime log')
        exit_text = read_text(case / 'exit-code.txt')
        require(re.fullmatch(r'-?[0-9]+\n', exit_text), 'exit status syntax')
        code = int(exit_text)
        # SparkEngineWindows.cpp returns 2 for -require-game initialization rejection.
        # Other nonzero exits can represent unrelated startup failure, abort or timeout.
        require(code == (0 if name == 'positive' else 2), 'runtime outcome: expected clean host rejection')
        runtime_records(log, name, package)
        result[name] = {'scene': scene, 'cook': cooked, 'runtimeLog': identity(case / 'runtime.log'),
                        'exitCode': code}
    return {'sourceSha': source_sha, 'configuration': config, 'scope': 'payload-lineage-only',
            'hashBasis': 'runner-local-files', 'lifecycleScope': 'separate required caller validation',
            'frameworkJUnit': framework,
            'installedHost': host_id, 'installedModule': module_id, 'installedSidecar': sidecar_id,
            'cases': result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('evidence-root', 'installed-host', 'installed-module', 'source-sha', 'config', 'framework-junit'):
        parser.add_argument('--' + option, required=True)
    args = parser.parse_args()
    try:
        result = validate(Path(args.evidence_root), Path(args.installed_host),
                          Path(args.installed_module), args.source_sha, args.config, Path(args.framework_junit))
        print(json.dumps(result, sort_keys=True))
    except (OSError, ValueError, TypeError, RecursionError, ET.ParseError) as error:
        parser.exit(1, f'Editor FPS lineage rejected: {error}\n')


if __name__ == '__main__':
    main()
