"""Caller contracts only: native decoder callback is always synthetic."""
import json
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from unittest import mock
import fps_owned_save_decode as decoder


class DecodeContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.primary = self.root/'owned-primary.spark_save'
        self.primary.write_bytes(b'synthetic container; never native decoded')
        self.exe = self.root/'SparkTests.exe'
        self.exe.write_bytes(b'synthetic executable; never executed')
        self.variant = ''

    def child(self, command, *, cwd, environment, timeout):
        self.assertEqual(environment['SPARK_TEST_NAME_PREFIX'], decoder.CASE)
        self.assertEqual(environment['SPARK_TEST_EXPECT_COUNT'], '1')
        self.assertLessEqual(timeout, 20)
        primary = Path(environment['SPARK_FPS_DECODE_DIRECTORY'])/'fps_quicksave.spark_save'
        identity = decoder.file_identity(primary, decoder.INPUT_CAP)
        receipt = dict(inputSha256=identity['sha256'], inputBytes=identity['bytes'],
                       canonicalProfile={'fps.profile.version': '1', 'fps.profile.class': '1'})
        if self.variant == 'wrong-hash':
            receipt['inputSha256'] = '0'*64
        if self.variant == 'input-mutation':
            self.primary.write_bytes(b'changed')
        if self.variant == 'backup':
            primary.with_name(primary.name+'.bak').write_bytes(b'backup')
        totals = 'tests="1" failures="0" skipped="0" flaky="0" empty="0"'
        case = '<testcase name="'+decoder.CASE+'" time="0.01"'+('><skipped/></testcase>' if self.variant == 'skipped' else '/>')
        Path(command[-1]).write_text('<testsuites '+totals+'><testsuite '+totals+'>'+case+'</testsuite></testsuites>')
        line = decoder.PREFIX+json.dumps(receipt)+'\n'
        if self.variant == 'duplicate':
            line += line
        if self.variant == 'oversize':
            line += 'x'*decoder.TEXT_CAP
        return subprocess.CompletedProcess(command, 2 if self.variant == 'exit' else 0, line, '')

    def invoke(self, **extra):
        return decoder.decode_owned_primary(input_path=self.primary, owned_root=self.root,
            tests_executable=self.exe, deadline=time.monotonic()+30, run_child=self.child, **extra)

    def test_complete_content_scope(self):
        work, report = self.invoke()
        self.assertEqual(report['scope'], 'copied-primary-content-only')
        self.assertEqual(report['input'], decoder.file_identity(self.primary, decoder.INPUT_CAP))
        self.assertTrue((work/'content-receipt.json').is_file())

    def test_bad_native_receipts_never_publish_success(self):
        for variant in ('wrong-hash', 'input-mutation', 'backup', 'skipped', 'duplicate', 'oversize', 'exit'):
            with self.subTest(variant=variant):
                self.primary.write_bytes(b'synthetic container; never native decoded')
                self.variant = variant
                with self.assertRaises(ValueError):
                    self.invoke()
        self.assertFalse(list(self.root.glob('save-decode-*/content-receipt.json')))

    def test_outside_input_and_backup_rejected_before_child(self):
        owned = self.root/'inner'
        owned.mkdir()
        with self.assertRaisesRegex(ValueError, 'inside explicit owned'):
            decoder.decode_owned_primary(input_path=self.primary, owned_root=owned,
                tests_executable=self.exe, deadline=time.monotonic()+30,
                run_child=lambda *a, **k: self.fail('child must not run'))
        self.primary.with_name(self.primary.name+'.bak').write_bytes(b'old')
        with self.assertRaisesRegex(ValueError, 'backup'):
            self.invoke()

    def test_final_verification_must_fit_owner_deadline(self):
        with mock.patch.object(decoder.time, 'monotonic', side_effect=[0, 0, 99]):
            with self.assertRaisesRegex(ValueError, 'during final verification'):
                decoder.decode_owned_primary(input_path=self.primary, owned_root=self.root,
                    tests_executable=self.exe, deadline=50, run_child=self.child)
        self.assertFalse(list(self.root.glob('save-decode-*/content-receipt.json')))


if __name__ == '__main__':
    unittest.main()
