"""Compiler-free orchestration contracts; fixtures do not qualify native code."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('isa_runner', ROOT / '.github/scripts/qualify-isa-diagnostic.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value), encoding='utf-8')


class Contracts(unittest.TestCase):
    def test_only_requested_target(self):
        self.assertEqual(runner.build_command(Path('build')), [
            'cmake', '--build', 'build', '--config', 'Release', '--target', 'SparkGameMMOFPS', '--parallel'])

    def test_successful_collection_preserves_scan_failure(self):
        result = runner.terminal(1, {'scanExitCode': 1, 'diagnosticComplete': True})
        self.assertTrue(result['consistent'])
        self.assertTrue(result['diagnosticComplete'])
        self.assertFalse(result['passed'])

    def test_original_toolchain_drift_rejected(self):
        tools = {name: {'sha256': name} for name in ('cl', 'rc', 'fxc', 'cmake', 'ninja', 'llvm-objdump', 'llvm-pdbutil')}
        identity = {'source_sha': runner.SOURCE, 'source_tree': 'tree', 'tools': tools}
        reference = json.loads(json.dumps(identity))
        reference['image_sha256'] = runner.ORIGINAL_IMAGE
        runner.validate_reference(identity, reference)
        reference['tools']['cl']['sha256'] = 'changed'
        with self.assertRaisesRegex(ValueError, 'tool mismatch: cl'):
            runner.validate_reference(identity, reference)

    def test_original_source_drift_rejected(self):
        with self.assertRaisesRegex(ValueError, 'identity mismatch: source_sha'):
            runner.validate_reference({'source_sha': 'other'}, {'source_sha': runner.SOURCE})

    def test_only_consistent_complete_zero_passes(self):
        self.assertTrue(runner.terminal(0, {'scanExitCode': 0, 'diagnosticComplete': True})['passed'])
        for code, scan, complete in [(0, 1, True), (1, 0, True), (2, 0, True),
                                      (0, 0, False), (0, None, True), (0, False, True)]:
            with self.subTest(code=code, scan=scan, complete=complete):
                self.assertFalse(runner.terminal(code, {'scanExitCode': scan, 'diagnosticComplete': complete})['passed'])

    def test_text_allowlist_and_explicit_tail(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'module.dll').write_bytes(b'not text\xff')
            (root / 'module.pdb').write_bytes(b'not text\xff')
            text = ('\u00e9' * 9000).encode()
            (root / 'build.log').write_bytes(text)
            (root / 'proof.json').write_text('{}', encoding='utf-8')
            retained = runner.payloads(root, digest)
            self.assertEqual(set(retained), {'build.log', 'proof.json', 'manifest.json'})
            retained['build.log'].decode('utf-8')
            record = next(x for x in json.loads(retained['manifest.json']) if x['path'] == 'build.log')
            self.assertTrue(record['tailOnly'])
            self.assertEqual(record['originalSha256'], hashlib.sha256(text).hexdigest())
            self.assertEqual(record['retainedSha256'], hashlib.sha256(retained['build.log']).hexdigest())

    def test_invalid_utf8_is_not_replaced(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'proof.json').write_bytes(b'\xff')
            with self.assertRaises(UnicodeDecodeError):
                runner.payloads(root, digest)

    def test_nul_in_json_or_log_is_rejected(self):
        for suffix in ('.json', '.log'):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / ('proof' + suffix)).write_bytes(b'valid UTF-8\x00binary content')
                with self.assertRaisesRegex(ValueError, 'NUL byte in text evidence'):
                    runner.payloads(root, digest)

    def test_complete_proof_overflow_is_not_truncated(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'isa-diagnostics.json').write_bytes(b'a' * (runner.MIB + 1))
            with self.assertRaisesRegex(ValueError, 'exceeds cap'):
                runner.payloads(root, digest)

    def test_ab_pass_requires_original_baseline_and_fixed_zero(self):
        self._ab_run('success')

    def test_unexpected_baseline_stops_fixed_scan(self):
        self._ab_run('baseline-unexpected')

    def test_fixed_nonzero_preserves_baseline_failure(self):
        self._ab_run('fixed-failure')

    def test_fixed_timeout_preserves_both_checkpoints(self):
        self._ab_run('fixed-timeout')

    def test_wrong_checker_identity_stops_before_scans(self):
        self._ab_run('wrong-checker')

    def test_different_image_or_pdb_cannot_verify_correction(self):
        record = {'image_sha256': runner.ORIGINAL_IMAGE, 'pdb_sha256': 'pdb'}
        report = {'scanExitCode': 0, 'diagnosticComplete': True, 'sourceSha': runner.SOURCE,
                  'checkerSha': 'a' * 40, 'originalImageMatch': True, 'findingCount': 0, 'findings': [],
                  'hashes': {'image': runner.ORIGINAL_IMAGE, 'pdb': 'pdb'}}
        self.assertTrue(runner.scan_matches(0, report, 'a' * 40, record, 0))
        for name in ('image', 'pdb'):
            changed = json.loads(json.dumps(report))
            changed['hashes'][name] = 'different'
            with self.subTest(name=name):
                self.assertFalse(runner.scan_matches(0, changed, 'a' * 40, record, 0))

    def _ab_run(self, mode):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            source, checker, evidence = base / 'source', base / 'checker', base / 'evidence'
            source.mkdir()
            checker.mkdir()
            evidence.mkdir()
            tool = base / 'tool'
            tool.write_bytes(b'tool')
            image, pdb = base / 'target.dll', base / 'target.pdb'
            image.write_bytes(b'image')
            pdb.write_bytes(b'pdb')
            checker_sha = 'a' * 40
            now = [1000.0]
            save(evidence / 'identity.json', {'source_sha': runner.SOURCE, 'configuration': 'Release',
                 'tools': {name: {'path': str(tool), 'sha256': digest(tool)} for name in ('llvm-objdump', 'llvm-pdbutil')}})
            save(evidence / 'clock.json', {'deadline': now[0] + 240 * 60})
            save(evidence / 'target-identity.json', {'image': str(image), 'pdb': str(pdb),
                 'image_sha256': runner.ORIGINAL_IMAGE, 'pdb_sha256': digest(pdb)})
            calls = []

            def owned(command, **kwargs):
                calls.append((command, kwargs['timeout']))
                self.assertLessEqual(kwargs['timeout'], 570)
                if 'rev-parse' in command:
                    sha = runner.SOURCE if command[2] == str(source) else checker_sha
                    if mode == 'wrong-checker' and command[2] == str(checker):
                        sha = 'b' * 40
                    kwargs['stdout'].write((sha + '\n').encode())
                if '--output' in command:
                    output = Path(command[command.index('--output') + 1])
                    baseline = output.name.startswith('baseline')
                    findings = 74 if baseline else (1 if mode == 'fixed-failure' else 0)
                    if baseline and mode == 'baseline-unexpected':
                        findings = 73
                    code = 1 if findings else 0
                    report = {'scanExitCode': code, 'diagnosticComplete': not (mode == 'fixed-timeout' and not baseline),
                              'sourceSha': runner.SOURCE, 'checkerSha': runner.SOURCE if baseline else checker_sha,
                              'originalImageMatch': True, 'findingCount': findings, 'findings': [{}] * findings,
                              'hashes': {'image': runner.ORIGINAL_IMAGE, 'pdb': digest(pdb), 'objdump': digest(tool), 'pdbutil': digest(tool)}}
                    save(output, report)
                    now[0] += 300 if baseline else 10
                    if mode == 'fixed-timeout' and not baseline:
                        raise subprocess.TimeoutExpired(command, kwargs['timeout'])
                    return SimpleNamespace(returncode=code)
                return SimpleNamespace(returncode=0)

            def fixture_digest(path):
                return runner.ORIGINAL_IMAGE if path == image else digest(path) if path.exists() else 'fixture-collector-hash'
            life = SimpleNamespace(owned_run=owned, remaining=lambda deadline, cap: min(cap, deadline - now[0]))
            full = SimpleNamespace(adapter=lambda source: life, read_json=lambda p: json.loads(p.read_text()),
                                   save=save, digest=fixture_digest)
            argv = ['runner', 'diagnose', '--source', str(source), '--root', str(evidence), '--source-sha', runner.SOURCE,
                    '--checker-source', str(checker), '--checker-sha', checker_sha]
            with patch.object(runner, 'load', return_value=full), patch.object(sys, 'argv', argv), patch.dict(os.environ, RUNNER_TEMP=str(base)), patch.object(runner.time, 'monotonic', side_effect=lambda: now[0]):
                if mode == 'success':
                    runner.main()
                else:
                    with self.assertRaises(subprocess.TimeoutExpired if mode == 'fixed-timeout' else ValueError):
                        runner.main()
            scans = [(cmd, timeout) for cmd, timeout in calls if '--output' in cmd]
            if mode == 'wrong-checker':
                self.assertEqual(scans, [])
                return
            self.assertEqual(len(scans), 1 if mode == 'baseline-unexpected' else 2)
            if len(scans) == 2:
                self.assertEqual(scans[0][1], 570)
                self.assertEqual(scans[1][1], 270)  # one shared deadline, not 570 seconds each
                for arg in ('--image', '--pdb'):
                    self.assertEqual(scans[0][0][scans[0][0].index(arg) + 1], scans[1][0][scans[1][0].index(arg) + 1])
            result = json.loads((evidence / 'target-terminal.json').read_text())
            self.assertEqual(result['baselineScanExitCode'], 1)
            self.assertEqual(result['targetedCorrectionVerified'], mode == 'success')
            self.assertTrue((evidence / 'baseline-isa-diagnostics.json').is_file())
            self.assertEqual(json.loads((evidence / 'baseline-terminal.json').read_text())['passed'], False)
            if mode == 'fixed-timeout':
                self.assertEqual(result['fixedScanExitCode'], 0)
                self.assertIsNone(result['fixedCollectorExitCode'])
                self.assertFalse(result['fixedCheckerPassed'])


if __name__ == '__main__':
    unittest.main()
