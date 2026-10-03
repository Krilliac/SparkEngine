import copy
import importlib.util
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET
import yaml
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('full', ROOT / '.github/scripts/qualify-full-windows.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
class Contracts(unittest.TestCase):
    def discovery(self):
        return {'tests': [{'name': n, 'properties': [{'name':'TIMEOUT','value':600 if n=='CpuFloor_IsaBaseline' else 240}]} for n in sorted(m.REQUIRED)]}
    def xml(self, names):
        root = ET.Element('testsuite')
        for n in names: ET.SubElement(root,'testcase',name=n,status='run')
        return root
    def test_complete(self):
        report=m.account(self.discovery(),ET.tostring(self.xml(sorted(m.REQUIRED))))
        self.assertTrue(report['complete']);self.assertFalse(report['failed'])
    def test_missing_extra_duplicate_rejected(self):
        for names in [sorted(m.REQUIRED)[:-1],[*m.REQUIRED,'extra'],[*m.REQUIRED,'SparkEngineTests']]:
            with self.assertRaises(ValueError):m.account(self.discovery(),ET.tostring(self.xml(names)))
    def test_cpu_gate_missing_disabled_or_timeout_changed(self):
        for mutation in ('missing','disabled','timeout'):
            doc=self.discovery();row=next(t for t in doc['tests'] if t['name']=='CpuFloor_IsaBaseline')
            if mutation=='missing':doc['tests'].remove(row)
            elif mutation=='disabled':row['properties'].append({'name':'DISABLED','value':True})
            else:row['properties'][0]['value']=601
            with self.assertRaises(ValueError):m.inventory(doc)
    def test_failures_and_unexpected_skips_fail(self):
        for marker in ('failure','error','skipped'):
            root=self.xml(sorted(m.REQUIRED));ET.SubElement(root[0],marker)
            self.assertEqual(len(m.account(self.discovery(),ET.tostring(root))['failed']),1)
    def test_declared_disabled_accounted(self):
        doc=self.discovery();doc['tests'].append({'name':'disabled','properties':[{'name':'DISABLED','value':True}]})
        root=self.xml([*sorted(m.REQUIRED),'disabled']);root[-1].set('status','disabled')
        self.assertEqual(m.account(doc,ET.tostring(root))['disabled'],['disabled'])
    def postbuild(self):
        doc=self.discovery()
        for row in doc['tests']:row['command']=['C:/fixture/test.exe','--quiet']
        return doc
    def test_postbuild_command_resolution_preserves_registration(self):
        m.validate_postbuild(self.discovery(),self.postbuild())
    def test_postbuild_missing_extra_or_duplicate_name_rejected(self):
        for change in ('missing','extra','duplicate'):
            doc=self.postbuild()
            if change=='missing':doc['tests'].pop()
            elif change=='extra':doc['tests'].append({'name':'extra','command':['extra'],'properties':[]})
            else:doc['tests'].append(copy.deepcopy(doc['tests'][0]))
            with self.assertRaises(ValueError):m.validate_postbuild(self.discovery(),doc)
    def test_postbuild_policy_mutations_rejected(self):
        for key,value in [('DISABLED',True),('TIMEOUT',241),('ENVIRONMENT',['SPARK_TEST_EXCLUDE=all']),
                          ('SKIP_RETURN_CODE',0),('SKIP_REGULAR_EXPRESSION',['.*']),('LABELS',['other']),
                          ('ENVIRONMENT_MODIFICATION',['PATH=reset:']),('WILL_FAIL',True)]:
            before=self.discovery();after=self.postbuild()
            row=next(t for t in after['tests'] if t['name']=='SparkEngineTests')
            row['properties']=[p for p in row['properties'] if p['name']!=key]+[{'name':key,'value':value}]
            with self.subTest(key=key),self.assertRaises(ValueError):m.validate_postbuild(before,after)
    def test_postbuild_enabled_command_required(self):
        for command in (None,[],[''],['exe',' '],'not-a-list'):
            doc=self.postbuild();doc['tests'][0]['command']=command
            with self.assertRaises(ValueError):m.validate_postbuild(self.discovery(),doc)
    def test_postbuild_disabled_command_can_remain_absent(self):
        before=self.discovery();before['tests'].append({'name':'disabled','properties':[{'name':'DISABLED','value':True}]})
        after=self.postbuild();after['tests'].append(copy.deepcopy(before['tests'][-1]))
        m.validate_postbuild(before,after)
    def test_disabled_status_contradictions_rejected(self):
        doc=self.discovery();doc['tests'].append({'name':'disabled','properties':[{'name':'DISABLED','value':True}]})
        for status,child in [('run',None),('notrun','skipped'),('disabled','failure'),('disabled','error'),('disabled','skipped')]:
            root=self.xml([*sorted(m.REQUIRED),'disabled']);root[-1].set('status',status)
            if child:ET.SubElement(root[-1],child)
            with self.subTest(status=status,child=child),self.assertRaises(ValueError):m.account(doc,ET.tostring(root))
    def test_active_disabled_and_malformed_declared_skip_fail(self):
        doc=self.discovery();doc['tests'].append({'name':'skip','properties':[{'name':'SKIP_RETURN_CODE','value':0}]})
        for status,child,failed in [('notrun','skipped',False),('run','skipped',True),('disabled',None,True),('notrun',None,True)]:
            root=self.xml([*sorted(m.REQUIRED),'skip']);root[-1].set('status',status)
            if child:ET.SubElement(root[-1],child)
            report=m.account(doc,ET.tostring(root))
            self.assertEqual('skip' in report['failed'],failed)
            self.assertEqual('skip' in report['declared_skips'],not failed)
    def test_postbuild_policy_order_and_reserve(self):
        text=(ROOT/'.github/scripts/qualify-full-windows.py').read_text()
        self.assertIn("suite_cap = life.remaining(deadline, 4770) - 770",text)
        self.assertLess(text.index("run('build',"),text.index("run('ctest-policy',"))
        self.assertLess(text.index('validate_postbuild(read_json'),text.index("run('ctest-policy',"))
        self.assertIn("save(root / 'prebuild-discovery.json', discovered)",text)
    def test_text_total_budget(self):
        self.assertTrue(m.bundle({'proof.json':b'{}'}).startswith(b'PK'))
        with self.assertRaisesRegex(ValueError,'12 MiB'):m.bundle({'proof':b'x'*(12*m.MIB+1)})
    def test_workflow_bound_and_source_gate(self):
        w=yaml.safe_load((ROOT/'.github/workflows/full-windows-qualification.yml').read_text());j=w['jobs']['windows']
        self.assertEqual(j['needs'],'source-contracts');self.assertEqual(j['timeout-minutes'],240)
        self.assertEqual(j['runs-on'],'windows-2022');self.assertEqual(sum(s['name']=='build' for s in j['steps']),1)
        helper=(ROOT/'.github/scripts/qualify-full-windows.py').read_text()
        self.assertNotIn("'--target'",helper);self.assertNotIn("'--stop-on-failure'",helper)
        self.assertIn('life.owned_run',helper);self.assertNotIn('subprocess.run(',helper)
    def test_framework_proof_complete_raw_streams_explicitly_diagnostic(self):
        self.assertIn('SparkTests-output.log',m.BUILD_PROOF)
        self.assertIn('SparkTests-load-output.log',m.BUILD_PROOF)
        self.assertIn('SparkTests-junit.xml',m.BUILD_PROOF)
        self.assertEqual(m.BUILD_DIAGNOSTIC,('SparkTests.log','SparkTests-load.log'))
        self.assertFalse(set(m.BUILD_PROOF)&set(m.BUILD_DIAGNOSTIC))
        text=(ROOT/'.github/scripts/qualify-full-windows.py').read_text()
        self.assertIn("'--profile-selectors', root / 'discovery.json'",text)
        self.assertIn("['minimumProductionSourceTests']",text)
        self.assertIn("'test-source-census.json'",text)
        self.assertIn("'--config', 'Release', '--parallel'], 7170)",text)
    def test_sde_bound_and_pins(self):
        s=(ROOT/'.github/scripts/full-windows-sde.ps1').read_text()
        self.assertIn('-TimeoutSec 240',s);self.assertEqual(s.count('Write-Host "Intel SDE:'),4)
        self.assertIn('74e626ede09b0baa5011fc9e51b58627ea92c3fc0bae5fd7db34b490f335f651',s)
        self.assertIn('$executables.Count -ne 1',s);self.assertNotIn('Retry',s)
if __name__=='__main__':unittest.main()
