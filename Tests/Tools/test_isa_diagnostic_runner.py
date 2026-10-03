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

    def test_diagnose_failure_runs_once_under_owned_bound(self):
        self._diagnostic_failure(False)

    def test_timeout_preserves_checkpointed_strict_failure(self):
        self._diagnostic_failure(True)

    def _diagnostic_failure(self, timed_out):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            source, evidence = base / 'source', base / 'evidence'
            source.mkdir()
            evidence.mkdir()
            tool = base / 'tool'
            tool.write_bytes(b'tool')
            image, pdb = base / 'target.dll', base / 'target.pdb'
            image.write_bytes(b'image')
            pdb.write_bytes(b'pdb')
            save(evidence / 'identity.json', {'source_sha': runner.SOURCE, 'configuration': 'Release',
                 'tools': {name: {'path': str(tool), 'sha256': digest(tool)} for name in ('llvm-objdump', 'llvm-pdbutil')}})
            save(evidence / 'clock.json', {'deadline': time.monotonic() + 240 * 60})
            save(evidence / 'target-identity.json', {'image': str(image), 'pdb': str(pdb),
                 'image_sha256': digest(image), 'pdb_sha256': digest(pdb)})
            calls = []

            def owned(command, **kwargs):
                calls.append((command, kwargs['timeout']))
                self.assertLessEqual(kwargs['timeout'], 570)
                if 'rev-parse' in command:
                    kwargs['stdout'].write((runner.SOURCE + '\n').encode())
                if '--output' in command:
                    save(Path(command[command.index('--output') + 1]), {'scanExitCode': 1, 'diagnosticComplete': not timed_out})
                    if timed_out:
                        raise subprocess.TimeoutExpired(command, 570)
                    return SimpleNamespace(returncode=1)
                return SimpleNamespace(returncode=0)

            life = SimpleNamespace(owned_run=owned, remaining=lambda deadline, cap: min(cap, deadline - time.monotonic()))
            full = SimpleNamespace(adapter=lambda source: life, read_json=lambda p: json.loads(p.read_text()),
                                   save=save, digest=lambda p: digest(p) if p.exists() else 'fixture-collector-hash')
            argv = ['runner', 'diagnose', '--source', str(source), '--root', str(evidence), '--source-sha', runner.SOURCE]
            with patch.object(runner, 'load', return_value=full), patch.object(sys, 'argv', argv), patch.dict(os.environ, RUNNER_TEMP=str(base)):
                with self.assertRaises(subprocess.TimeoutExpired if timed_out else ValueError):
                    runner.main()
            self.assertEqual(sum('--output' in cmd for cmd, _ in calls), 1)
            result = json.loads((evidence / 'target-terminal.json').read_text())
            self.assertEqual(result['scanExitCode'], 1)
            self.assertEqual(result['diagnosticComplete'], not timed_out)
            self.assertFalse(result['passed'])
            self.assertTrue((evidence / 'diagnose-failure.json').is_file())


if __name__ == '__main__':
    unittest.main()
