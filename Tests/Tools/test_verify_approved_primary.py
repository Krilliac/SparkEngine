import importlib.util
from pathlib import Path
import shutil
import uuid
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('verify_primary', ROOT / '.github/scripts/verify-approved-primary.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class PrimaryVerifierTests(unittest.TestCase):
    def setUp(self):
        self.directory = ROOT / ('.primary-test-' + uuid.uuid4().hex)
        self.directory.mkdir()
        self.path = self.directory / 'tests.xml'

    def tearDown(self):
        self.assertEqual(self.directory.resolve().parent, ROOT.resolve())
        shutil.rmtree(self.directory)

    def record(self, scene):
        affected = verify.AFFECTED[scene]
        cases = ET.Element('testsuite')
        for name in verify.capture.TESTS_BY_LANE[verify.FILE_NAME]:
            case = ET.SubElement(cases, 'testcase', name=name)
            if name in {verify.capture.TEST_NAME_BY_SCENE[s] for s in affected}:
                ET.SubElement(case, 'failure').text = 'FAIL: RHI210Golden::MatchesGolden(...) was false (Primary.cpp:228)'
        ET.ElementTree(cases).write(self.path)
        lines = []
        for name in verify.APPROVED:
            failed = name in affected
            lines.append(f'[RHI-210 GOLDEN] scene={name} matched={"no" if failed else "yes"} '
                         f'differing={1 if failed else 0}/100 (0.000%, tolerance 0.000%) '
                         'maxDist=1.00 meanDist=0.000 threshold=0.00')
        return dict(stdout='\n'.join(lines), stderr='', returncode=1 if affected else 0,
                    timedOut=False, junitPath=str(self.path))

    def test_normal_and_each_explicit_mutation_dependency(self):
        for scene in verify.AFFECTED:
            with self.subTest(scene=scene):
                self.assertEqual(len(verify.validate(self.record(scene), scene)), 3)

    def test_empty_failure_remains_rejected(self):
        record = self.record('Primary_DeferredGeometry')
        tree = ET.parse(self.path)
        tree.find('.//failure').text = ''
        tree.write(self.path)
        with self.assertRaisesRegex(verify.capture.CaptureError, 'empty failure'):
            verify.validate(record, 'Primary_DeferredGeometry')

    def test_unrelated_assertion_remains_rejected(self):
        record = self.record('Primary_DeferredGeometry')
        tree = ET.parse(self.path)
        tree.find('.//failure').text += '\nFAIL: validation->errors == 0 (1 != 0) at fixture.h:123'
        tree.write(self.path)
        with self.assertRaisesRegex(verify.capture.CaptureError, 'unexpected JUnit failure'):
            verify.validate(record, 'Primary_DeferredGeometry')

    def test_shadow_cannot_change_under_geometry_mutation(self):
        record = self.record('Primary_DeferredGeometry')
        record['stdout'] = record['stdout'].replace('scene=Primary_ShadowDepth matched=yes differing=0',
                                                   'scene=Primary_ShadowDepth matched=no differing=1')
        with self.assertRaises(verify.capture.CaptureError):
            verify.validate(record, 'Primary_DeferredGeometry')

    def test_missing_duplicate_or_nonzero_threshold_rejected(self):
        for change in ('missing', 'duplicate', 'threshold'):
            record = self.record(None)
            if change == 'missing':
                record['stdout'] = '\n'.join(record['stdout'].splitlines()[:2])
            elif change == 'duplicate':
                record['stdout'] += '\n' + record['stdout'].splitlines()[0]
            else:
                record['stdout'] = record['stdout'].replace('threshold=0.00', 'threshold=1.00')
            with self.subTest(change=change), self.assertRaises(verify.capture.CaptureError):
                verify.validate(record, None)

    def test_junit_failure_cannot_disagree_with_scene_verdict(self):
        record = self.record('Primary_DeferredGeometry')
        tree = ET.parse(self.path)
        case = tree.findall('testcase')[-1]
        ET.SubElement(case, 'failure').text = 'FAIL: RHI210Golden::MatchesGolden(...) was false (Primary.cpp:228)'
        tree.write(self.path)
        with self.assertRaisesRegex(verify.capture.CaptureError, 'JUnit failures/exit'):
            verify.validate(record, 'Primary_DeferredGeometry')

    def test_capture_requests_distinct_runner_output_for_junit_details(self):
        with mock.patch.object(verify.capture.subprocess, 'run') as run:
            run.return_value = mock.Mock(stdout='', stderr='', returncode=0)
            verify.capture._run_one(Path('SparkTests.exe'), self.path.parent, verify.FILE_NAME, 4, junit_path=self.path)
        command = run.call_args.args[0]
        self.assertEqual(command[command.index('--output-file') + 1], str(self.path.with_suffix('.runner.log')))
        self.assertIn('--junit-xml', command)
        self.assertIn('--warn-is-error', command)
        self.assertIn('--empty-is-error', command)


if __name__ == '__main__':
    unittest.main()
