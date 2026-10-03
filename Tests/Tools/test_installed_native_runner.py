import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
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
            self.assertEqual(kwargs['timeout'], 1780)
            self.assertTrue(kwargs['check'])
            self.assertIn('-DSPARK_CONSUMER_GENERATOR=Ninja Multi-Config', command)
            self.assertIn('-DSPARK_CONSUMER_COMPILER=C:/tool/cl', command)
            self.assertIn(f'-DSPARK_REFERENCE_SIDECAR={self.binary / "SparkGameFPS.dll.sparkabi"}', command)
            self.assertTrue(command[-1].endswith('RunInstalledSDKTemplate.cmake'))
            installed.parent.mkdir(parents=True)
            installed.write_bytes(self.built.read_bytes())
        with patch.object(runner.shutil, 'which', side_effect=lambda name: 'C:/tool/' + name), patch.object(runner.subprocess, 'run', side_effect=simulated) as process:
            runner.run_phase('sdk', self.source, self.root)
        self.assertEqual(process.call_count, 1)
        self.assertTrue(json.loads((self.root / 'sdk-binding.json').read_text())['passed'])

    def test_modified_build_prevents_any_subprocess(self):
        self.built.write_bytes(b'changed')
        with patch.object(runner.subprocess, 'run') as process:
            with self.assertRaisesRegex(ValueError, 'Build input changed'):
                runner.run_phase('sdk', self.source, self.root)
        process.assert_not_called()

    def test_wrong_installed_host_cannot_pass_sdk(self):
        def simulated(*args, **kwargs):
            target = self.root / 'sdk/prefix/bin/SparkEngine.exe'
            target.parent.mkdir(parents=True)
            target.write_bytes(b'wrong host')
        with patch.object(runner.subprocess, 'run', side_effect=simulated):
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
        with patch.object(runner.subprocess, 'run', side_effect=simulated):
            runner.run_phase('abi', self.source, self.root)
        self.assertEqual(len(commands), 3)
        self.assertTrue((self.root / 'abi-binding.json').exists())

    def test_closure_missing_second_graph_is_failure(self):
        def simulated(command, **kwargs):
            self.assertEqual(kwargs['timeout'], 880)
            graph = self.root / 'closure/run-test/one.import-graph.json'
            graph.parent.mkdir(parents=True)
            runner.save(graph, {'images': [{'path': 'SparkEngine.exe', 'sha256': self.expected}]})
        with patch.object(runner.subprocess, 'run', side_effect=simulated):
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
        with patch.object(runner.subprocess, 'run', side_effect=runner.subprocess.TimeoutExpired('cmake', 1780)):
            with self.assertRaises(runner.subprocess.TimeoutExpired):
                runner.run_phase('sdk', self.source, self.root)
        self.assertFalse((self.root / 'sdk-binding.json').exists())


if __name__ == '__main__':
    unittest.main()
