"""Bounded diagnostics preserve findings and never include product files."""
import hashlib
import io
import json
from pathlib import Path
import sys
import unittest
from unittest import mock
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'Tools' / 'buildmatrix'))
import bundle_diagnostics as tool


class BundleDiagnosticsTests(unittest.TestCase):
    def run_bundle(self, files, **limits):
        def opened(path, mode):
            self.assertEqual(mode, 'rb')
            if path.name not in files:
                raise FileNotFoundError(path)
            return io.BytesIO(files[path.name])
        with mock.patch.object(Path, 'open', opened), mock.patch.multiple(tool, **({'MAX_EXPANDED': tool.MAX_EXPANDED, 'MAX_ZIP': tool.MAX_ZIP} | limits)):
            return tool.bundle(Path('fixture'), {'GITHUB_SHA': 'a' * 40,
                                                'GITHUB_RUN_ID': '123', 'GITHUB_RUN_ATTEMPT': '2'})

    def test_exact_findings_inventory_receipt_and_provenance_only(self):
        files = {tool.INPUTS[0]: b'{"errorCount":5,"warningCount":3,"findings":[{"category":"extra","detail":"exact evidence"}]}\n',
                 tool.INPUTS[1]: b'{"configuredTargets":["one"]}\n',
                 tool.INPUTS[2]: b'{"accepted":false}\n', 'product.exe': b'product'}
        data = self.run_bundle(files)
        self.assertLessEqual(len(data), tool.MAX_ZIP)
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            self.assertEqual(set(archive.namelist()), set(tool.INPUTS) | {'diagnostic-provenance.json'})
            for name in tool.INPUTS:
                self.assertEqual(archive.read(name), files[name])
            provenance = json.loads(archive.read('diagnostic-provenance.json'))
            self.assertEqual(provenance['sourceSha'], 'a' * 40)
            self.assertEqual(provenance['runAttempt'], '2')
            for entry in provenance['inputs']:
                self.assertEqual(entry['sha256'], hashlib.sha256(files[entry['path']]).hexdigest())

    def test_missing_producers_are_explicit_not_empty_success_reports(self):
        with zipfile.ZipFile(io.BytesIO(self.run_bundle({}))) as archive:
            self.assertEqual(archive.namelist(), ['diagnostic-provenance.json'])
            provenance = json.loads(archive.read('diagnostic-provenance.json'))
            self.assertTrue(all(x['state'] == 'missing' for x in provenance['inputs']))

    def test_invalid_json_is_rejected(self):
        with self.assertRaises(ValueError):
            self.run_bundle({tool.INPUTS[0]: b'not JSON'})

    def test_expanded_and_compressed_limits_fail_without_truncation(self):
        for limits in ({'MAX_EXPANDED': 10}, {'MAX_ZIP': 10}):
            with self.subTest(limits=limits), self.assertRaises(ValueError):
                self.run_bundle({tool.INPUTS[0]: b'{"full": "evidence"}'}, **limits)


if __name__ == '__main__':
    unittest.main()
