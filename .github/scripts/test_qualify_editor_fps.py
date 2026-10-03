"""Compiler-free contracts; real CMake checks use LANGUAGES NONE, native execution stays mocked."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch

SPEC = importlib.util.spec_from_file_location('qualification', Path(__file__).with_name('qualify-editor-fps.py'))
Q = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(Q)
PINS = {'source_sha': 'a'*40, 'workflow_sha': 'b'*40, 'run_id': '123', 'run_attempt': '1'}


def doc(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding='utf-8')


def selected():
    return {'tests': [{'name': Q.CTEST, 'config': Q.CONFIG, 'command': ['cmake'], 'properties': [
        {'name': 'TIMEOUT', 'value': 900}, {'name': 'RUN_SERIAL', 'value': True}]}]}


class Contracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source = self.base/'source'
        self.root = self.base/'evidence'
        self.source.mkdir()
        self.root.mkdir()
        self.build = self.source/'build/windows-shipping'
        self.args = argparse.Namespace(phase='test', source=self.source, root=self.root, **PINS)
        self.env = {'GITHUB_SHA': PINS['workflow_sha'], 'GITHUB_RUN_ID': '123', 'GITHUB_RUN_ATTEMPT': '1'}
        doc(self.root/'clock.json', {'deadline': Q.time.monotonic()+14400})
        self.life = Mock()
        self.life.remaining.side_effect = lambda deadline, cap: cap
        self.life.owned_run.return_value = subprocess.CompletedProcess([], 0)
        with patch.dict(Q.os.environ, self.env):
            self.runner = Q.Runner(self.args, self.life)

    def test_owned_run_receives_exact_command_and_timeout(self):
        self.runner.run('proof', ['cmake', '-E', 'echo', 'benign'], 12)
        call = self.life.owned_run.call_args
        self.assertEqual(call.args[0], ['cmake', '-E', 'echo', 'benign'])
        self.assertEqual(call.kwargs['cwd'], self.source)
        self.assertEqual(call.kwargs['timeout'], 12)
        self.assertIs(call.kwargs['check'], True)
        self.assertEqual(Q.read_json(self.root/'test-commands.json')[0]['exitCode'], 0)

    def test_timeout_is_propagated_and_recorded(self):
        self.life.owned_run.side_effect = subprocess.TimeoutExpired(['fake'], 5)
        with self.assertRaises(subprocess.TimeoutExpired):
            self.runner.run('proof', ['fake'], 5)
        self.assertIn('error', Q.read_json(self.root/'test-commands.json')[0])
        self.assertEqual(self.life.owned_run.call_count, 1)

    def test_configure_is_shipping_multiconfig_with_no_dev_override(self):
        command = Q.configure_command(self.source, self.build, PINS['source_sha'])
        self.assertEqual(command[command.index('--preset')+1], 'windows-shipping')
        self.assertEqual(command[command.index('-A')+1], '')
        self.assertEqual(command[command.index('-T')+1], '')
        self.assertIn('-DCMAKE_CONFIGURATION_TYPES=MinSizeRel', command)
        self.assertIn('-DBUILD_TESTS=ON', command)
        self.assertFalse(any('CONSOLE_IN_SHIPPING=ON' in part or 'DEVCOMMANDS_IN_SHIPPING=ON' in part for part in command))

    def test_build_is_only_requested_target_closure_and_fails_first(self):
        self.runner.exact_source = Mock()
        self.runner.identity = Mock(return_value={})
        self.runner.run = Mock(side_effect=subprocess.CalledProcessError(1, ['build']))
        with self.assertRaises(subprocess.CalledProcessError):
            self.runner.build_phase()
        command = self.runner.run.call_args.args[1]
        self.assertEqual(command[command.index('--target')+1:], Q.TARGETS)
        self.assertEqual(command[command.index('--config')+1], 'MinSizeRel')
        self.assertEqual(self.runner.run.call_count, 1)

    def test_reflected_failure_prevents_installed_test_and_uses_owned_profile(self):
        src = self.source/'Tests/TestSceneManagerReflectedReal.cpp'
        src.parent.mkdir()
        src.write_text('TEST(SceneManager_ReflectedGameplay_First)\nTEST(SceneManager_ReflectedGameplay_Second)\n')
        self.runner.exact_source = Mock()
        self.runner.identity = Mock(return_value={'images': {'not-read': 'not-read'}})
        self.runner.run = Mock(side_effect=subprocess.CalledProcessError(1, ['reflected']))
        with self.assertRaises(subprocess.CalledProcessError):
            self.runner.test()
        self.assertEqual(self.runner.run.call_count, 1)
        command = [str(v) for v in self.runner.run.call_args.args[1]]
        self.assertIn('SPARK_TEST_EXPECT_COUNT=2', command)
        self.assertIn('SPARK_TEST_NAME_PREFIX='+Q.PREFIX, command)
        self.assertIn('LOCALAPPDATA='+str(self.root/'tmp/localappdata'), command)
        self.assertIn('APPDATA='+str(self.root/'tmp/appdata'), command)
        self.assertNotIn('ctest', command)

    def test_installed_failure_never_writes_success_terminal(self):
        src = self.source/'Tests/TestSceneManagerReflectedReal.cpp'
        src.parent.mkdir()
        name = 'SceneManager_ReflectedGameplay_One'
        src.write_text('TEST('+name+')\n')
        self.runner.exact_source = Mock()
        self.runner.identity = Mock(return_value={'images': {'not-read': 'not-read'}})
        def command(label, argv, cap):
            if label=='reflected':
                (self.root/'reflected-junit.xml').write_text('<testsuite><testcase name="'+name+'" time="0.1"/></testsuite>')
            else:
                self.assertEqual(label, 'ctest')
                self.assertIn('^'+Q.CTEST+'$', argv)
                self.assertEqual(cap, 910)
                raise subprocess.CalledProcessError(2, argv)
        self.runner.run = Mock(side_effect=command)
        with patch.object(Q, 'immutable'):
            with self.assertRaises(subprocess.CalledProcessError):
                self.runner.test()
        self.assertFalse((self.root/'terminal.json').exists())

    def prepare_successful_native_mock(self, corrupt=False):
        src = self.source/'Tests/TestSceneManagerReflectedReal.cpp'
        src.parent.mkdir()
        name = 'SceneManager_ReflectedGameplay_One'
        src.write_text('TEST('+name+')\n')
        binary = self.build/'bin'/Q.CONFIG
        binary.mkdir(parents=True)
        images = {}
        for filename in ('SparkEngine.exe', 'SparkGameFPS.dll', 'SparkGameFPS.dll.sparkabi'):
            path = binary/filename
            path.write_bytes(b'benign toy image')
            images[str(path)] = Q.digest(path)
        self.runner.exact_source = Mock()
        self.runner.identity = Mock(return_value={'images': images})
        def command(label, argv, cap):
            if label=='reflected':
                (self.root/'reflected-junit.xml').write_text('<testsuite><testcase name="'+name+'" time="0.1"/></testsuite>')
                return
            self.assertEqual(label, 'ctest')
            (self.root/'ctest-junit.xml').write_text('<testsuite><testcase name="'+Q.CTEST+'" status="run" time="1"/></testsuite>')
            staged = Q.scratch_root(self.root, self.build)
            staged.mkdir(parents=True)
            (staged/'.spark-editor-fps-installed-lineage').write_text('owned')
            (staged/'lineage-junit.xml').write_text('<testsuite/>')
            receipt = {'sourceSha': PINS['source_sha'], 'configuration': Q.CONFIG,
                'frameworkJUnit': {'sha256': Q.digest(staged/'lineage-junit.xml')},
                'cases': dict.fromkeys(Q.CASES, {})}
            for field, filename in [('installedHost','SparkEngine.exe'), ('installedModule','SparkGameFPS.dll'),
                                    ('installedSidecar','SparkGameFPS.dll.sparkabi')]:
                receipt[field] = {'sha256': images[str(binary/filename)]}
            if corrupt: receipt['installedModule']['sha256']='0'*64
            doc(staged/'lineage-validation.json', receipt)
        self.runner.run = Mock(side_effect=command)

    def test_success_terminal_requires_both_native_stages_and_identity(self):
        self.prepare_successful_native_mock()
        self.runner.test()
        result = Q.read_json(self.root/'terminal.json')
        self.assertIs(result['passed'], True)
        self.assertEqual(result['run_id'], PINS['run_id'])
        self.assertIs(result['full_suite_qualified'], False)
        self.assertIs(result['physical_shipping_qualified'], False)
        self.assertEqual(self.runner.run.call_count, 2)

    def test_wrong_installed_module_binding_cannot_write_success(self):
        self.prepare_successful_native_mock(corrupt=True)
        with self.assertRaisesRegex(ValueError, 'binding mismatch'):
            self.runner.test()
        self.assertFalse((self.root/'terminal.json').exists())

    def test_input_mutation_stops_before_native(self):
        image = self.base/'image.exe'
        image.write_bytes(b'original')
        record = {'images': {str(image): Q.digest(image)}}
        image.write_bytes(b'mutated')
        with self.assertRaisesRegex(ValueError, 'changed'):
            Q.immutable(record)
        self.life.owned_run.assert_not_called()

    def test_identity_pins_reject_stale_run(self):
        with self.assertRaises(ValueError):
            Q.pins(self.args, dict(self.env, GITHUB_RUN_ID='124'))

    def test_ctest_accounting_requires_run_complete_bounded(self):
        path = self.root/'ctest.xml'
        for status, elapsed, extra in [('run','0.1',''), ('notrun','0',''), ('run','nan',''), ('run','901',''), ('run','1','<skipped/>')]:
            path.write_text('<testsuite><testcase name="'+Q.CTEST+'" status="'+status+'" time="'+elapsed+'">'+extra+'</testcase></testsuite>')
            if status=='run' and elapsed=='0.1':
                self.assertEqual(Q.validate_xml(path, [Q.CTEST], ctest=True)['executed'], 1)
            else:
                with self.assertRaises(ValueError):
                    Q.validate_xml(path, [Q.CTEST], ctest=True)

    def test_json_is_bounded_duplicate_safe(self):
        path = self.root/'proof.json'
        for data in ('{"passed":false,"passed":true}', '{"x":Infinity}', '"'+'x'*Q.MIB+'"'):
            path.write_text(data)
            with self.assertRaises(ValueError):
                Q.read_json(path)

    def test_full_runtime_longer_than_old_tail_is_preserved(self):
        staged = Q.scratch_root(self.root, self.build)
        path = staged/'evidence/positive/runtime.log'
        path.parent.mkdir(parents=True)
        raw = b'complete record\n'*2000
        path.write_bytes(raw)
        Q.collect(self.root, self.build, PINS)
        retained = self.root/'qualification-text/installed/evidence/positive/runtime.log'
        self.assertEqual(retained.read_bytes(), raw)
        manifest = Q.read_json(self.root/'qualification-text/manifest.json')
        row = next(row for row in manifest if row['path'].endswith('positive/runtime.log'))
        self.assertIs(row['tail_only'], False)
        self.assertIs(Q.read_json(self.root/'qualification-text/collection.json')['completeSuccessProof'], False)

    def test_full_runtime_cap_fails_before_publish(self):
        path = Q.scratch_root(self.root,self.build)/'evidence/positive/runtime.log'
        path.parent.mkdir(parents=True)
        path.write_bytes(b'x'*(128*1024+1))
        with self.assertRaisesRegex(ValueError, 'cap'):
            Q.collect(self.root, self.build, PINS)
        self.assertFalse((self.root/'qualification-text').exists())

    def test_diagnostic_reencoding_is_bounded_and_labeled(self):
        (self.root/'build.log').write_bytes(b'\xff'*16384)
        Q.collect(self.root, self.build, PINS)
        retained = self.root/'qualification-text/diagnostics/build.log'
        self.assertLessEqual(retained.stat().st_size,16384)
        retained.read_text(encoding='utf-8')
        row = next(row for row in Q.read_json(self.root/'qualification-text/manifest.json') if row['path']=='diagnostics/build.log')
        self.assertIs(row['tail_only'], True)
        self.assertIs(row['diagnostic_reencoded'], True)

    def test_stale_or_incomplete_success_is_not_packaged(self):
        doc(self.root/'terminal.json', dict(PINS, passed=True, run_id='124'))
        with self.assertRaisesRegex(ValueError, 'different'):
            Q.collect(self.root, self.build, PINS)
        doc(self.root/'terminal.json', dict(PINS, passed=True))
        with self.assertRaisesRegex(ValueError, 'Missing proof'):
            Q.collect(self.root, self.build, PINS)
        self.assertFalse((self.root/'qualification-text').exists())

    def test_discovery_rejects_wrong_config_or_policy(self):
        good = selected()
        Q.discovery(good)
        for change in ('config', 'timeout', 'extra'):
            value = selected()
            if change=='config': value['tests'][0]['config']='Release'
            if change=='timeout': value['tests'][0]['properties'][0]['value']=901
            if change=='extra': value['tests'].append(value['tests'][0])
            with self.assertRaises(ValueError): Q.discovery(value)


class ConfigureOptionGuardContracts(unittest.TestCase):
    def test_real_guard_accepts_helper_options_and_rejects_old_sha_cache_option(self):
        guard = Path(__file__).resolve().parents[2]/'cmake/SparkOptionGuard.cmake'
        self.assertTrue(guard.is_file(), 'Missing real product option guard')
        with tempfile.TemporaryDirectory(prefix='editor-fps-options-') as temporary:
            root = Path(temporary)
            source = root/'source'
            source.mkdir()
            (source/'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                'project(EditorFPSOptionContract LANGUAGES NONE)\n'
                'include("'+guard.as_posix()+'")\n'
                'spark_capture_cli_options()\n'
                'option(BUILD_TESTS "Real declared test option" OFF)\n'
                'option(GENERATE_DEBUG_SYMBOLS "Real declared symbol option" ON)\n'
                'spark_reject_undeclared_options()\n', encoding='utf-8')
            command = Q.configure_command(source, root/'unused', PINS['source_sha'])
            defines = [part for part in command if part.startswith('-D')]
            # Execute the actual guard over actual helper options. LANGUAGES NONE
            # never runs the supplied compiler paths or links any product code.
            def configure(extra, name):
                return subprocess.run([Q.shutil.which('cmake'), '-S', str(source), '-B', str(root/name),
                    '-G', 'Ninja', *defines, *extra], text=True, capture_output=True, timeout=30)
            good = configure([], 'good')
            self.assertEqual(good.returncode, 0, (good.stdout+good.stderr)[-4000:])
            self.assertNotIn('SPARK_EXPECTED_SOURCE_SHA:', (root/'good/CMakeCache.txt').read_text())
            for spelling in ('SPARK_EXPECTED_SOURCE_SHA', 'SPARK_EXPECTED_SOURCE_SHA:STRING'):
                with self.subTest(spelling=spelling):
                    bad = configure(['-D'+spelling+'='+PINS['source_sha']], 'bad-'+str(len(spelling)))
                    self.assertNotEqual(bad.returncode, 0)
                    self.assertIn('SparkOptionGuard: undeclared build option', bad.stdout+bad.stderr)
                    self.assertIn('SPARK_EXPECTED_SOURCE_SHA='+PINS['source_sha'], bad.stdout+bad.stderr)

    def test_source_sha_still_required_in_exact_ctest_command(self):
        with tempfile.TemporaryDirectory(prefix='editor-fps-source-pin-') as temporary:
            source = Path(temporary)/'source'
            build = source/'build/windows-shipping'
            row = selected()
            row['tests'][0]['command'] = [Q.shutil.which('cmake'),
                '-DSPARK_ENGINE_BUILD_DIR='+build.as_posix(), '-DSPARK_SOURCE_ROOT='+source.as_posix(),
                '-DSPARK_CONFIG=MinSizeRel', '-DSPARK_TESTS_EXECUTABLE='+(build/'bin/MinSizeRel/SparkTests.exe').as_posix(),
                '-DSPARK_ENGINE_EXECUTABLE_NAME=SparkEngine.exe', '-DSPARK_EXPECTED_SOURCE_SHA='+PINS['source_sha'],
                '-DSPARK_PYTHON_EXECUTABLE='+Q.sys.executable.replace('\\', '/'),
                '-P', (source/'cmake/RunEditorFPSInstalledLineage.cmake').as_posix()]
            Q.discovery(row, True, source, build, PINS['source_sha'])
            row['tests'][0]['command'][6] = '-DSPARK_EXPECTED_SOURCE_SHA='+'c'*40
            with self.assertRaisesRegex(ValueError, 'exact command binding'):
                Q.discovery(row, True, source, build, PINS['source_sha'])

if __name__ == '__main__':
    unittest.main()
