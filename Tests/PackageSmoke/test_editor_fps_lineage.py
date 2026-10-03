"""Compiler-free file mutations of the actual producer layout; no engine runs."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest

spec = importlib.util.spec_from_file_location('lineage', Path(__file__).with_name('ValidateEditorFPSLineage.py'))
lineage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lineage)


class LineageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lineage-', dir=Path(__file__).parent)
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'evidence'
        self.host = self.base / 'prefix/bin/SparkEngine.exe'
        self.module = self.host.with_name('SparkGameFPS.dll')
        self.junit = self.base / 'lineage-junit.xml'
        totals = 'tests="4" failures="0" skipped="0" flaky="0" empty="0" time="1"'
        self.write(self.junit, (f'<testsuites {totals}><testsuite {totals}>' +
                   ''.join(f'<testcase name="{name}" time="0.1"/>' for name in lineage.TESTS) +
                   '</testsuite></testsuites>').encode())
        self.write(self.host, b'synthetic host fixture, never executed')
        self.write(self.module, b'synthetic module fixture, never executed')
        self.write(Path(str(self.module)+'.sparkabi'), b'synthetic ABI fixture')
        asset = b'v 0 0 0\nf 1 1 1\n'
        assets = {'Meshes/crate.obj': asset, 'Models/pistol.obj': b'installed pistol fixture',
                  'Models/rifle.obj': b'installed rifle fixture'}
        records = [{'path': path, 'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)}
                   for path, data in assets.items()]
        aggregate = hashlib.sha256(''.join(f"{r['path']}\0{r['sha256']}\0{r['size']}\n" for r in records).encode()).hexdigest()
        manifest = {'schemaVersion': 1, 'manifestSha256': aggregate, 'assets': records}
        for path, data in assets.items():
            if path.startswith('Models/'):
                self.write(self.host.parent / 'Assets' / path, data)
        scene = json.dumps({'version': 1, 'entities': [
            {'id': 0, 'name': 'Crate', 'parent': -1, 'components': [
                {'type': 'Transform', 'fields': {'position': '3.000000,2.000000,5.000000',
                 'rotation': '0.000000,0.000000,0.000000', 'scale': '1.000000,1.000000,1.000000'}},
                {'type': 'MeshRenderer', 'fields': {'meshPath': 'Assets/Meshes/crate.obj',
                                                   'visible': 'true', 'materialPath': ''}}]},
            {'id': 1, 'name': 'AuthoredCamera', 'parent': -1, 'components': [
                {'type': 'Transform', 'fields': {'position': '7.000000,11.000000,-13.000000',
                 'rotation': '0.000000,0.000000,0.000000', 'scale': '1.000000,1.000000,1.000000'}},
                {'type': 'Camera', 'fields': {'isMainCamera': 'true', 'fov': '70.000000',
                                            'nearPlane': '0.100000', 'farPlane': '1000.000000'}}]}]}).encode()
        for name in lineage.CASES:
            case = self.root / name
            project, package = case / 'CrateProject', case / 'Package'
            for path in (project / 'Scenes/Default.sparkscene', case / 'reopened.sparkscene',
                         project / 'Cooked/Scenes/Default.sparkscene', package / 'Scenes/Startup.sparkscene'):
                self.write(path, scene)
            self.write(package / 'Startup.sparkscene', b'not a reflected scene\n' if name == 'malformed' else scene)
            self.write(package / 'Crate Game.exe', self.host.read_bytes())
            if name != 'missing-module':
                self.write(package / self.module.name, self.module.read_bytes())
            self.write(package / (self.module.name+'.sparkabi'), Path(str(self.module)+'.sparkabi').read_bytes())
            self.write_json(package / 'spark.modules.json', {'modules': [
                {'name': 'SparkGameFPS', 'path': 'SparkGameFPS.dll', 'loadOrder': 1000}]})
            for prefix in (project, project / 'Cooked'):
                self.write_json(prefix / 'CrateProject.sparkproject', {'defaultScene': 'Scenes/Default.sparkscene'})
                for path, data in assets.items():
                    self.write(prefix / 'Assets' / path, data)
            for path, data in assets.items():
                if name != 'missing-asset' or path != 'Meshes/crate.obj':
                    self.write(package / 'Assets' / path, data)
            for prefix in (project / 'Cooked', package):
                self.write_json(prefix / 'Assets/spark-cook-manifest.json', manifest)
            logs = {
                'positive': 'SPARK_FPS_STARTUP scene=Startup.sparkscene nodes=2 rendering=1\n'
                            'SPARK_FPS_STARTUP_NODE index=0 type=model position=3,2,5 rotation=0,0,0 scale=1,1,1\n'
                            'SPARK_FPS_STARTUP_NODE index=1 type=Camera position=7,11,-13 rotation=0,0,0 scale=1,1,1\n'
                            'SPARK_FPS_STARTUP_MESH index=0 position=3,2,5 scale=1,1,1\n'
                            'SPARK_FPS_STARTUP_CAMERA position=7,11,-13 fov=70 near=0.100000001 far=1000\n'
                            'SPARK_FPS_STARTUP_PLAYER position=7,11,-13\n',
                'malformed': 'Reflected gameplay scene rejected: primary scene is invalid or has unsupported reflected fields\n'
                             'FPS packaged startup rejected: selected reflected scene failed to load\n',
                'missing-asset': 'Reflected gameplay scene rejected: mesh must be an existing project-confined OBJ\n'
                                 'FPS packaged startup rejected: selected reflected scene failed to load\n',
                'missing-module': 'Explicit game module not found: '+str(package/'SparkGameFPS.dll')+'\n'}
            self.write(case / 'runtime.log', logs[name].encode())
            self.write(case / 'exit-code.txt', b'0\n' if name == 'positive' else b'2\n')

    @staticmethod
    def write(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def write_json(self, path, value):
        self.write(path, json.dumps(value).encode())

    def validate(self):
        return lineage.validate(self.root, self.host, self.module, 'a'*40, 'MinSizeRel', self.junit)

    def test_complete_payload_and_scope(self):
        result = self.validate()
        self.assertEqual(set(result['cases']), set(lineage.CASES))
        self.assertEqual(result['scope'], 'payload-lineage-only')

    def test_actual_cli_exit_and_receipt(self):
        command = [sys.executable, '-B', str(Path(lineage.__file__)),
                   '--evidence-root', str(self.root), '--installed-host', str(self.host),
                   '--installed-module', str(self.module), '--source-sha', 'a'*40, '--config', 'MinSizeRel',
                   '--framework-junit', str(self.junit)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(result.stdout)
        self.assertEqual(receipt['sourceSha'], 'a'*40)
        self.assertEqual(receipt['hashBasis'], 'runner-local-files')
        self.write(self.root / 'malformed/runtime.log', b'unrelated failure\n')
        result = subprocess.run(command, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, '')
        self.assertIn('negative case reason mismatch', result.stderr)

    def test_scene_module_host_sidecar_mutations(self):
        for relative in ('positive/reopened.sparkscene', 'positive/Package/Startup.sparkscene',
                         'positive/Package/Crate Game.exe', 'positive/Package/SparkGameFPS.dll',
                         'positive/Package/SparkGameFPS.dll.sparkabi'):
            with self.subTest(relative=relative):
                path = self.root / relative
                old = path.read_bytes()
                self.write(path, b'{}\n')
                with self.assertRaises(ValueError):
                    self.validate()
                self.write(path, old)

    def test_negative_controls_cannot_disappear(self):
        paths = ('missing-asset/Package/Assets/Meshes/crate.obj',
                 'missing-module/Package/SparkGameFPS.dll',
                 'malformed/Package/Startup.sparkscene.bak')
        for relative in paths:
            with self.subTest(relative=relative):
                path = self.root / relative
                self.write(path, b'unexpected fallback')
                with self.assertRaises(ValueError):
                    self.validate()
                path.unlink()

    def test_bad_cook_records_and_aggregate(self):
        paths = [self.root/'positive'/prefix/'Assets/spark-cook-manifest.json'
                 for prefix in ('CrateProject/Cooked', 'Package')]
        old = paths[0].read_bytes()
        mutations = [lambda d: d.update(manifestSha256='0'*64),
                     lambda d: d['assets'][0].update(size=True),
                     lambda d: d['assets'][0].update(sha256='0'*64),
                     lambda d: d['assets'][0].update(path='../crate.obj'),
                     lambda d: d.update(assets=[])]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                value = json.loads(old)
                mutation(value)
                for path in paths:
                    self.write_json(path, value)
                with self.assertRaises(ValueError):
                    self.validate()
        for path in paths:
            self.write(path, old)

    def test_strict_text(self):
        path = self.root / 'positive/runtime.log'
        for bad in (b'', b'\xff', b'a\0b', b'a'*(lineage.TEXT_CAP+1),
                    b'... Output removed since output was too large'):
            with self.subTest(data=bad[:30]):
                self.write(path, bad)
                with self.assertRaises((ValueError, UnicodeError)):
                    self.validate()

    def test_wrong_outcome_and_missing_case(self):
        path = self.root / 'missing-module/exit-code.txt'
        self.write(path, b'0\n')
        with self.assertRaises(ValueError):
            self.validate()
        for code in (b'-1\n', b'1\n', b'3\n', b'124\n', b'3221225477\n'):
            self.write(path, code)
            with self.assertRaises(ValueError):
                self.validate()
        self.write(path, b'2\n')
        (self.root / 'extra-case').mkdir()
        with self.assertRaises(ValueError):
            self.validate()

    def test_consistent_hashes_do_not_bless_wrong_scene(self):
        case = self.root / 'positive'
        paths = [case / p for p in ('CrateProject/Scenes/Default.sparkscene', 'reopened.sparkscene',
                 'CrateProject/Cooked/Scenes/Default.sparkscene', 'Package/Scenes/Startup.sparkscene',
                 'Package/Startup.sparkscene')]
        original = paths[0].read_bytes()
        for field, value in (('name', 'WrongCrate'), ('parent', 1)):
            scene = json.loads(original)
            scene['entities'][0][field] = value
            for path in paths:
                self.write_json(path, scene)
            with self.assertRaises(ValueError):
                self.validate()
        scene = json.loads(original)
        scene['entities'][0]['components'][0]['fields']['position'] = '0,0,0'
        for path in paths:
            self.write_json(path, scene)
        with self.assertRaises(ValueError):
            self.validate()

    def test_duplicate_json_and_wrong_module(self):
        path = self.root / 'positive/Package/spark.modules.json'
        self.write(path, b'{"modules":[],"modules":[]}')
        with self.assertRaises(ValueError):
            self.validate()
        self.write_json(path, {'modules': [{'name': 'CrateGame', 'path': 'CrateGame.dll', 'loadOrder': 1000}]})
        with self.assertRaises(ValueError):
            self.validate()

    def test_wrong_runtime_reason_and_state(self):
        for name, replacement in (('missing-asset', 'unrelated graphics failure\n'),
                                  ('malformed', 'Reflected gameplay scene rejected: mesh must be an existing project-confined OBJ\n')):
            path = self.root / name / 'runtime.log'
            old = path.read_bytes()
            self.write(path, replacement.encode())
            with self.assertRaises(ValueError):
                self.validate()
            self.write(path, old)
        path = self.root/'positive/runtime.log'
        old = path.read_bytes()
        for mutation in (old.replace(b'position=3,2,5', b'position=0,0,0'),
                         old.replace(b'scene=Startup.sparkscene', b'scene=Scenes/Startup.sparkscene'),
                         old.replace(b'fov=70', b'fov=nan'), old + old.splitlines()[0] + b'\n',
                         old.replace(b'rendering=1', b'rendering=1 rendering=1'),
                         old.replace(b'STARTUP_MESH index=0', b'STARTUP_MESH index=1'),
                         old + b'Warning: Failed to load pistol model\n'):
            self.write(path, mutation)
            with self.assertRaises(ValueError):
                self.validate()
        self.write(path, old)

    def test_installed_weapon_asset_binding(self):
        self.write(self.host.parent/'Assets/Models/pistol.obj', b'wrong installed source')
        with self.assertRaises(ValueError):
            self.validate()

    def test_exact_framework_completion(self):
        good = self.junit.read_bytes()
        for bad in (good.replace(lineage.TESTS[0].encode(), lineage.TESTS[1].encode()),
                    good.replace(b'failures="0"', b'failures="1"'),
                    good.replace(b'time="0.1"/>', b'time="0.1"><skipped/></testcase>', 1),
                    b'<!DOCTYPE testsuites>'+good,
                    good.replace(b'time="1"', b'time="nan"')):
            self.write(self.junit, bad)
            with self.assertRaises(ValueError):
                self.validate()


if __name__ == '__main__':
    unittest.main()
