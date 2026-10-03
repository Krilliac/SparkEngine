import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, Mock
import zipfile

SCRIPT = Path(__file__).resolve().parents[2] / '.github/scripts/qualify-installed-native.py'
spec = importlib.util.spec_from_file_location('qualification', SCRIPT)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class Contracts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.source = self.base / 'source'
        self.root = self.base / 'evidence'
        self.root.mkdir()
        self.binary = self.source / 'build/windows-shipping/bin/MinSizeRel'
        self.binary.mkdir(parents=True)
        self.built = self.binary / 'SparkEngine.exe'
        self.built.write_bytes(b'benign identity fixture')
        self.expected = runner.digest(self.built)
        runner.save(self.root / 'identity.json', {'host_sha256': self.expected,
                    'images': {str(self.built): self.expected}})

    def test_sdk_uses_installed_runner_reference_compiler_and_bound(self):
        installed = self.root / 'sdk/prefix/bin/SparkEngine.exe'
        def simulated(command, **kwargs):
            self.assertEqual(kwargs['cwd'], self.source)
            self.assertGreater(kwargs['timeout'], 1760)
            self.assertLessEqual(kwargs['timeout'], 1770)
            self.assertTrue(kwargs['check'])
            self.assertIn('-DSPARK_CONSUMER_GENERATOR=Ninja Multi-Config', command)
            self.assertIn('-DSPARK_CONSUMER_COMPILER=C:/tool/cl', command)
            self.assertIn(f'-DSPARK_REFERENCE_SIDECAR={self.binary / "SparkGameFPS.dll.sparkabi"}', command)
            self.assertTrue(command[-1].endswith('RunInstalledSDKTemplate.cmake'))
            installed.parent.mkdir(parents=True)
            installed.write_bytes(self.built.read_bytes())
        with patch.object(runner.shutil, 'which', side_effect=lambda name: 'C:/tool/' + name), patch.object(runner, 'owned_run', side_effect=simulated) as process:
            runner.run_phase('sdk', self.source, self.root)
        self.assertEqual(process.call_count, 1)
        self.assertTrue(json.loads((self.root / 'sdk-binding.json').read_text())['passed'])

    def test_modified_build_prevents_any_subprocess(self):
        self.built.write_bytes(b'changed')
        with patch.object(runner, 'owned_run') as process:
            with self.assertRaisesRegex(ValueError, 'Build input changed'):
                runner.run_phase('sdk', self.source, self.root)
        process.assert_not_called()

    def test_wrong_installed_host_cannot_pass_sdk(self):
        def simulated(*args, **kwargs):
            target = self.root / 'sdk/prefix/bin/SparkEngine.exe'
            target.parent.mkdir(parents=True)
            target.write_bytes(b'wrong host')
        with patch.object(runner, 'owned_run', side_effect=simulated):
            with self.assertRaisesRegex(ValueError, 'host identity changed'):
                runner.run_phase('sdk', self.source, self.root)
        self.assertFalse((self.root / 'sdk-binding.json').exists())

    def test_graph_requires_exact_single_host(self):
        path = self.root / 'graph.json'
        row = {'path': 'SparkEngine.exe', 'sha256': self.expected}
        runner.save(path, {'images': [row]})
        runner.bind_graph(path, self.expected)
        for rows in ([], [row, row], [{'path': 'sub/SparkEngine.exe', 'sha256': self.expected}], [{'path': 'SparkEngine.exe', 'sha256': 'wrong'}]):
            runner.save(path, {'images': rows})
            with self.assertRaises(ValueError):
                runner.bind_graph(path, self.expected)

    def test_abi_installs_components_then_runs_exact_driver_and_binds_report(self):
        installed = self.root / 'sdk/prefix/bin/SparkEngine.exe'
        installed.parent.mkdir(parents=True)
        installed.write_bytes(self.built.read_bytes())
        for name in ('SparkGameFPS.dll', 'SparkGameFPS.dll.sparkabi'):
            (self.binary / name).write_bytes(b'reference fixture')
            (installed.parent / name).write_bytes(b'reference fixture')
        commands = []
        def simulated(command, **kwargs):
            commands.append(command)
            if len(commands) <= 2:
                self.assertEqual(command[-2], '--component')
                self.assertEqual(command[-1], ('samples', 'redist')[len(commands) - 1])
                self.assertEqual(kwargs['timeout'], 20)
                return
            self.assertEqual(kwargs['timeout'], 250)
            self.assertIn(str(self.binary / 'SparkMismatchedModuleFixture.dll'), command)
            self.assertIn(str(self.binary / 'SparkPreviousSdkModuleFixture.dll'), command)
            self.assertEqual(command[-4:], ['--source-sha', runner.SOURCE, '--configuration', 'MinSizeRel'])
            report = self.root / 'abi/installed-abi-test/report.json'
            report.parent.mkdir(parents=True)
            runner.save(report, {'passed': True, 'inputs': {str(installed.resolve()): self.expected}})
        with patch.object(runner, 'owned_run', side_effect=simulated):
            runner.run_phase('abi', self.source, self.root)
        self.assertEqual(len(commands), 3)
        self.assertTrue((self.root / 'abi-binding.json').exists())

    def test_closure_missing_second_graph_is_failure(self):
        def simulated(command, **kwargs):
            self.assertGreater(kwargs['timeout'], 860)
            self.assertLessEqual(kwargs['timeout'], 870)
            graph = self.root / 'closure/run-test/one.import-graph.json'
            graph.parent.mkdir(parents=True)
            runner.save(graph, {'images': [{'path': 'SparkEngine.exe', 'sha256': self.expected}]})
        with patch.object(runner, 'owned_run', side_effect=simulated):
            with self.assertRaisesRegex(ValueError, 'both closure graphs'):
                runner.run_phase('closure', self.source, self.root)
        self.assertFalse((self.root / 'closure-binding.json').exists())

    def test_archive_omits_prefix_and_keeps_bounded_log_tail(self):
        hidden = self.root / 'sdk/prefix'
        hidden.mkdir(parents=True)
        (hidden / 'secret.json').write_text('{}')
        (self.root / 'build.log').write_text('x' * 50000 + 'END')
        runner.compact(self.root)
        with zipfile.ZipFile(self.root / 'diagnostics-size-check.zip') as archive:
            self.assertNotIn('sdk/prefix/secret.json', archive.namelist())
            self.assertEqual(len(archive.read('build.log')), 16384)
            self.assertTrue(archive.read('build.log').endswith(b'END'))
            inventory = json.loads(archive.read('inventory.json'))
            self.assertTrue(next(row for row in inventory if row['path'] == 'build.log')['tail_only'])
        self.assertLess((self.root / 'diagnostics-size-check.zip').stat().st_size, 1000000)
        self.assertEqual((self.root / 'diagnostics-text/build.log').read_bytes(), b'x' * (16384 - 3) + b'END')

    def test_oversized_structured_proof_fails_closed(self):
        (self.root / 'huge.json').write_text(' ' * 131073)
        with self.assertRaisesRegex(ValueError, 'Structured evidence'):
            runner.compact(self.root)
        self.assertFalse((self.root / 'diagnostics-size-check.zip').exists())

    def test_subprocess_failure_propagates_without_binding(self):
        with patch.object(runner, 'owned_run', side_effect=runner.subprocess.TimeoutExpired('cmake', 1780)):
            with self.assertRaises(runner.subprocess.TimeoutExpired):
                runner.run_phase('sdk', self.source, self.root)
        self.assertFalse((self.root / 'sdk-binding.json').exists())

    def runtime_proof_paths(self):
        return ['sdk/runtime-stdout.log', 'sdk/runtime-stderr.log'] + [
            f'abi/installed-abi-test/{case}/{stream}.log'
            for case in ('newer', 'previous') for stream in ('stdout', 'stderr')]

    def write_proof(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def test_complete_runtime_proof_over_16k_preserves_prefix_and_hash(self):
        data = b'PROOF_PREFIX\n' + b'x' * 20000 + b'\nPROOF_END\n'
        for name in self.runtime_proof_paths():
            self.write_proof(name, data)
        runner.compact(self.root)
        inventory = json.loads((self.root / 'diagnostics-text/inventory.json').read_text())
        for name in self.runtime_proof_paths():
            with self.subTest(name=name):
                self.assertEqual((self.root / 'diagnostics-text' / name).read_bytes(), data)
                row = next(row for row in inventory if row['path'] == name)
                self.assertFalse(row['tail_only'])
                self.assertEqual(row['bytes'], len(data))
                self.assertEqual(row['sha256'], runner.digest(self.root / 'diagnostics-text' / name))

    def test_oversized_runtime_proof_fails_instead_of_truncating(self):
        self.write_proof('abi/installed-abi-test/newer/stderr.log', b'x' * 131073)
        with self.assertRaisesRegex(ValueError, 'Runtime proof exceeds compact budget'):
            runner.compact(self.root)
        self.assertFalse((self.root / 'diagnostics-text').exists())
        self.assertFalse((self.root / 'diagnostics-size-check.zip').exists())

    def test_complete_proofs_still_obey_total_budget(self):
        for name in self.runtime_proof_paths():
            self.write_proof(name, b'x' * 131072)
        (self.root / 'extra.json').write_text('{}' + ' ' * 119998)
        with self.assertRaisesRegex(ValueError, '900000-byte budget'):
            runner.compact(self.root)
        self.assertFalse((self.root / 'diagnostics-text').exists())
        self.assertFalse((self.root / 'diagnostics-size-check.zip').exists())

    def test_invalid_utf8_proof_fails_without_changing_hashed_bytes(self):
        self.write_proof('sdk/runtime-stderr.log', b'prefix\xffsuffix')
        with self.assertRaises(UnicodeDecodeError):
            runner.compact(self.root)
        self.assertFalse((self.root / 'diagnostics-text').exists())


class OwnedProcessContracts(unittest.TestCase):
    def invoke(self, assigned=True, failure=None):
        events = []
        job = Mock()
        job.assign.side_effect = lambda p: events.append('assign') or assigned
        job.close.side_effect = lambda: events.append('close-job')
        process = Mock()
        process.stdin.closed = False
        process.stdin.write.side_effect = lambda data: events.append(('release', data))
        process.stdin.close.side_effect = lambda: events.append('close-stdin')
        process.kill.side_effect = lambda: events.append('kill-wrapper')
        process.wait.side_effect = [failure or 0, 0] if assigned else [0]
        caught = None
        with patch.object(runner, 'job_for', return_value=job), patch.object(runner.subprocess, 'Popen', return_value=process):
            try:
                runner.owned_run(['cmake', '--build', 'build'], cwd=Path('source'),
                                 stdout=Mock(), stderr=-2, timeout=17)
            except (RuntimeError, runner.subprocess.TimeoutExpired, runner.subprocess.CalledProcessError) as error:
                caught = error
        if not assigned:
            self.assertIsInstance(caught, RuntimeError)
        elif isinstance(failure, Exception):
            self.assertIs(caught, failure)
        elif failure:
            self.assertIsInstance(caught, runner.subprocess.CalledProcessError)
        else:
            self.assertIsNone(caught)
        return events, process

    def test_success_assigns_before_release_and_closes_job(self):
        events, process = self.invoke()
        self.assertLess(events.index('assign'), events.index(('release', b'GO\n')))
        self.assertIn('close-job', events)
        self.assertEqual(process.wait.call_args_list[-1].kwargs['timeout'], 10)

    def test_assignment_failure_never_releases_work(self):
        events, process = self.invoke(assigned=False)
        self.assertNotIn(('release', b'GO\n'), events)
        self.assertIn('kill-wrapper', events)
        self.assertIn('close-job', events)
        self.assertEqual(process.wait.call_args.kwargs['timeout'], 10)

    def test_timeout_and_command_failure_close_owned_job(self):
        for failure in (runner.subprocess.TimeoutExpired('wrapper', 17), 2):
            with self.subTest(failure=failure):
                events, process = self.invoke(failure=failure)
                self.assertIn('close-job', events)
                self.assertEqual(process.wait.call_args_list[-1].kwargs['timeout'], 10)

    def test_shared_deadline_caps_later_commands_and_reserves_cleanup(self):
        self.assertEqual(runner.PHASE_SECONDS['build'], 11070)
        with patch.object(runner.time, 'monotonic', side_effect=[100, 10900, 11070]):
            self.assertEqual(runner.remaining(11070, 900), 900)
            self.assertEqual(runner.remaining(11070, 10800), 170)
            with self.assertRaises(TimeoutError):
                runner.remaining(11070, 15)

    def test_wrapper_requires_go_before_command(self):
        # Execute only Python text with a mocked subprocess; no native child.
        import io
        for token, expected in ((b'', 125), (b'NO\n', 125), (b'GO\n', 7)):
            fake_sys = Mock(argv=['wrapper', 'cmake'])
            fake_sys.stdin.buffer = io.BytesIO(token)
            fake_subprocess = Mock()
            fake_subprocess.call.return_value = 7
            with patch.dict('sys.modules', {'sys': fake_sys, 'subprocess': fake_subprocess}):
                exec(runner.WRAPPER, {})
            fake_sys.exit.assert_called_once_with(expected)
            self.assertEqual(fake_subprocess.call.call_count, int(token == b'GO\n'))


if __name__ == '__main__':
    unittest.main()
