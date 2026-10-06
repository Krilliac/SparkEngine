"""Compiler-free focused-native orchestration and proof regression fixtures."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('focused_helper',Path(os.environ.get('SPARK_FOCUSED_HELPER',str(HERE/'qualify-installed-native.py'))))
H=importlib.util.module_from_spec(spec);spec.loader.exec_module(H)


def evidence(directory,label,names):
    plain=''.join(f'[   OK   ] {name} (1ms, 2 assertions)\r\n' for name in names)
    plain+=f'Tests:      {len(names)} passed, 0 failed, {len(names)} total\r\nAssertions: {2*len(names)} passed, 0 failed\r\n'
    attr=f'tests="{len(names)}" failures="0" skipped="0" flaky="0" empty="0" time="0.01"'
    xml=f'<testsuites {attr}><testsuite {attr}>'+''.join(f'<testcase name="{name}" time="0.001"/>' for name in names)+'</testsuite></testsuites>'
    for kind,raw in [('stdout.log',plain),('stderr.log',''),('output.log',plain),('junit.xml',xml)]:
        (directory/f'{label}-{kind}').write_bytes(raw.encode())

class FocusedContracts(unittest.TestCase):
    def fixture(self,root):
        source=root/'source';(source/'Tests').mkdir(parents=True)
        for label,filename,prefix,count in H.FOCUSED:
            (source/'Tests'/filename).write_text('\n'.join(f'TEST({prefix}{i})\n{{}}' for i in range(count)))
        image=source/'SparkTests.exe';image.write_bytes(b'fake image; never executable')
        identity=dict(source_sha='a'*40,workflow_sha='b'*40,run_id='1',run_attempt='1',weather_consumer=True)
        return source,image,identity

    def mock_run(self,source,mutation=None):
        calls=[]
        def run(argv,**kwargs):
            calls.append((argv,kwargs['timeout']))
            self.assertEqual(kwargs['cwd'],source)
            self.assertTrue(kwargs['check'])
            output=Path(argv[argv.index('--output-file')+1]);label=output.name.split('-')[0]
            prefix=next(x.split('=',1)[1] for x in argv if x.startswith('SPARK_TEST_NAME_PREFIX='))
            filename=next(x.split('=',1)[1] for x in argv if x.startswith('SPARK_TEST_FILE='))
            count=int(next(x.split('=',1)[1] for x in argv if x.startswith('SPARK_TEST_EXPECT_COUNT=')))
            names=H.focused_names(source,filename,prefix,count)
            evidence(output.parent,label,names)
            if mutation=='timeout':raise subprocess.TimeoutExpired(argv,kwargs['timeout'])
            if mutation=='image':(source/'SparkTests.exe').write_bytes(b'changed')
            if mutation=='exit':return subprocess.CompletedProcess(argv,2)
            return subprocess.CompletedProcess(argv,0)
        return calls,run

    def test_source_census_requires_exact_unique_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source,_,_=self.fixture(root)
            label,file,prefix,count=H.FOCUSED[0]
            self.assertEqual(len(H.focused_names(source,file,prefix,count)),4)
            (source/'Tests'/file).write_text('TEST('+prefix+'same)\n'*count)
            with self.assertRaises(ValueError):H.focused_names(source,file,prefix,count)

    def test_owned_five_calls_counts_isolation_and_image_receipt(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source,image,identity=self.fixture(root)
            calls,run=self.mock_run(source)
            with patch.object(H,'owned_run',side_effect=run):
                bound=H.focused_native(source,root,image,identity,time.monotonic()+300)
            self.assertEqual(len(calls),5)
            for argv,timeout in calls:
                self.assertEqual(timeout,60)
                for flag in ('--warn-is-error','--empty-is-error','--unset=SPARK_TEST_LIMIT','--unset=SPARK_TEST_EXCLUDE','--unset=SPARK_TEST_NAME'):
                    self.assertIn(flag,argv)
                self.assertTrue(any(x.startswith('LOCALAPPDATA=') for x in argv))
            identity['focused']=bound
            identity['images']={str(image):H.digest(image)}
            H.focused_proof(root,identity)
            report=json.loads((root/'focused/report.json').read_text())
            self.assertEqual(sum(len(row['names']) for row in report['families']),29)
            self.assertEqual(bound['test_image_sha256'],H.digest(image))

    def test_timeout_nonzero_and_image_mutation_never_publish_pass(self):
        for mutation in ('timeout','exit','image'):
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);source,image,identity=self.fixture(root);calls,run=self.mock_run(source,mutation)
                with patch.object(H,'owned_run',side_effect=run),self.assertRaises((ValueError,subprocess.TimeoutExpired)):
                    H.focused_native(source,root,image,identity,time.monotonic()+300)
                self.assertEqual(len(calls),1)
                self.assertFalse((root/'focused/report.json').exists())
                self.assertTrue((root/'focused/host-stdout.log').exists())

    def test_junit_rejects_faults_counts_missing_names_nonfinite_and_truncation(self):
        mutations=[('failures="0"','failures="1"'),('skipped="0"','skipped="1"'),('empty="0"','empty="1"'),('flaky="0"','flaky="1"'),('time="0.01"','time="NaN"'),('time="0.01"','time="61"'),('name="Case1"','name="Case0"'),('/>','><skipped/></testcase>')]
        for old,new in mutations:
            with self.subTest(mutation=new),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);evidence(root,'case',['Case0','Case1'])
                path=root/'case-junit.xml';path.write_text(path.read_text().replace(old,new))
                with self.assertRaises((ValueError,H.ET.ParseError)):H.validate_focused(root,'case',['Case0','Case1'])

    def test_plain_stdout_stderr_faults_and_assertion_mismatch_rejected(self):
        for kind,content in [('stdout.log','[ CRASH ] bad'),('stderr.log','[ FAILED ] bad'),('output.log','bytes of output removed')]:
            with tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);evidence(root,'case',['Case0'])
                with (root/f'case-{kind}').open('a') as stream:stream.write(content)
                with self.assertRaises(ValueError):H.validate_focused(root,'case',['Case0'])
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);evidence(root,'case',['Case0'])
            path=root/'case-output.log';path.write_text(path.read_text().replace('Assertions: 2','Assertions: 3'))
            with self.assertRaises(ValueError):H.validate_focused(root,'case',['Case0'])

    def test_all_focused_files_are_complete_capped_proof_on_failure(self):
        for size,good in ((20000,True),(131073,False)):
            with tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);(root/'focused').mkdir();(root/'focused/host-stdout.log').write_bytes(b'a'*size)
                if good:
                    H.compact(root)
                    self.assertEqual((root/'diagnostics-text/focused/host-stdout.log').stat().st_size,size)
                else:
                    with self.assertRaises(ValueError):H.compact(root)

    def test_tampered_report_or_files_and_missing_proof_rejected(self):
        for mutation in ('report','file','missing'):
            with tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);source,image,identity=self.fixture(root);_,run=self.mock_run(source)
                with patch.object(H,'owned_run',side_effect=run):identity['focused']=H.focused_native(source,root,image,identity,time.monotonic()+300)
                identity['images']={str(image):H.digest(image)}
                path=root/'focused'/('report.json' if mutation=='report' else 'fps-output.log')
                if mutation=='missing':path.unlink()
                else:path.write_text('tampered')
                with self.assertRaises((ValueError,FileNotFoundError)):H.focused_proof(root,identity)

    def test_shared_clock_preserves_pack_cleanup_and_phase_ceiling(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            now=time.monotonic();H.save(root/'clock.json',dict(deadline=now+1000))
            phase=H.weather_deadline(root,'build')
            self.assertAlmostEqual(phase,now+670,places=3)
            pack=H.weather_deadline(root,'pack')
            self.assertLessEqual(pack,time.monotonic()+270)
            self.assertGreater(pack,now+269)
            H.save(root/'clock.json',dict(deadline=now+100))
            self.assertAlmostEqual(H.weather_deadline(root,'pack'),now+70,places=3)
            H.save(root/'clock.json',dict(deadline=now+14400))
            self.assertLessEqual(H.weather_deadline(root,'sdk')-time.monotonic(),H.PHASE_SECONDS['sdk'])

    def test_actual_expired_clock_launches_no_child(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for offset in (-1,329):
                H.save(root/'clock.json',dict(deadline=time.monotonic()+offset))
                with patch.object(H,'owned_run') as child,self.assertRaises(TimeoutError):
                    H.run_phase('build',root/'source',root,True,'a'*40)
                child.assert_not_called()
            H.save(root/'clock.json',dict(deadline=time.monotonic()+29))
            with self.assertRaises(TimeoutError):H.weather_deadline(root,'pack')
            with self.assertRaises(TimeoutError):H.compact(root,deadline=time.monotonic()-1)
            self.assertFalse((root/'diagnostics-text').exists())

    def test_clock_rejects_invalid_nonfinite_future_and_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            with self.assertRaises(FileNotFoundError):H.weather_deadline(root,'pack')
            for value in (True,'later',float('inf'),float('nan'),-1,time.monotonic()+14401):
                H.save(root/'clock.json',dict(deadline=value))
                with self.assertRaises(ValueError):H.weather_deadline(root,'sdk')

    def test_weather_boundary_receipt_requires_real_integer_counts(self):
        for scanned,violations,good in ((2,0,True),(5,0,True),(None,0,False),(True,0,False),(1,0,False),(2,1,False),(2,False,False)):
            with self.subTest(scanned=scanned,violations=violations),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);(root/'sdk').mkdir()
                identity=dict(source_sha='a'*40,workflow_sha='b'*40,run_id='1',run_attempt='1',host_sha256='c'*64)
                receipt=dict(identity,module='SparkGeneratedGame',sdk_version=10,headless='unavailable',windowed='accepted',lifecycle='passed',consumer_sha256='d'*64,module_sha256='e'*64,sidecar_sha256='f'*64,boundary_scanned=scanned,boundary_violations=violations,streams={})
                for weather,available in (('',0),('-weather',1)):
                    for kind in ('stdout','stderr'):
                        name=f'runtime{weather}-{kind}.log';path=root/'sdk'/name
                        path.write_text(f'SPARK_SDK_WEATHER module=SparkGeneratedGame available={available} accepted={available} invalid_rejected={available} clear_accepted={available} callback=OnLoad\n' if kind=='stdout' else '')
                        receipt['streams'][name]=H.digest(path)
                H.save(root/'sdk/weather-identity.json',receipt)
                if good:H.weather_proof(root,identity)
                else:
                    with self.assertRaises(ValueError):H.weather_proof(root,identity)

    def test_combined_collector_requires_both_real_focused_and_consumer_proofs(self):
        for missing in (None,'focused','consumer'):
            with self.subTest(missing=missing),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp);source,image,identity=self.fixture(root);_,run=self.mock_run(source)
                with patch.object(H,'owned_run',side_effect=run):
                    identity['focused']=H.focused_native(source,root,image,identity,time.monotonic()+300)
                identity['images']={str(image):H.digest(image)}
                identity['host_sha256']='c'*64
                H.save(root/'identity.json',identity)
                sdk=root/'sdk';sdk.mkdir()
                receipt={key:identity[key] for key in ('source_sha','workflow_sha','run_id','run_attempt','host_sha256')}
                receipt.update(module='SparkGeneratedGame',sdk_version=10,headless='unavailable',windowed='accepted',lifecycle='passed',consumer_sha256='d'*64,module_sha256='e'*64,sidecar_sha256='f'*64,boundary_scanned=2,boundary_violations=0,streams={})
                for weather,available in (('',0),('-weather',1)):
                    for kind in ('stdout','stderr'):
                        name=f'runtime{weather}-{kind}.log';path=sdk/name
                        path.write_text(f'SPARK_SDK_WEATHER module=SparkGeneratedGame available={available} accepted={available} invalid_rejected={available} clear_accepted={available} callback=OnLoad\n' if kind=='stdout' else '')
                        receipt['streams'][name]=H.digest(path)
                H.save(sdk/'weather-identity.json',receipt)
                H.save(root/'sdk-binding.json',dict(passed=True,source_sha=identity['source_sha'],host_sha256=identity['host_sha256'],weather_identity_sha256=H.digest(sdk/'weather-identity.json')))
                if missing=='focused':(root/'focused/report.json').unlink()
                if missing=='consumer':(sdk/'weather-identity.json').unlink()
                if missing:
                    with self.assertRaises((ValueError,FileNotFoundError)):H.compact(root,True,identity['source_sha'])
                    self.assertFalse((root/'diagnostics-text').exists())
                else:
                    # Neither proof validator is mocked: this is the actual collector.
                    H.compact(root,True,identity['source_sha'])
                    self.assertTrue((root/'diagnostics-text/focused/report.json').is_file())
                    self.assertTrue((root/'diagnostics-text/sdk/weather-identity.json').is_file())

if __name__=='__main__':unittest.main()
