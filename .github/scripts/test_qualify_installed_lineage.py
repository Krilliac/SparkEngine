"""Compiler-free checks of real lineage proof parsing and owned orchestration."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE=Path(__file__).resolve().parent
def load(name,file):
    spec=importlib.util.spec_from_file_location(name,HERE/file)
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module);return module
H=load('lineage_test_host','qualify-installed-native.py')
L=load('lineage_test_adapter','qualify_installed_lineage.py')
E=L.validators()


class LineageContracts(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name)/'evidence';self.root.mkdir()
        self.source=Path(self.tmp.name)/'source';self.source.mkdir()
        self.build=self.source/'build/windows-shipping'
        self.identity=dict(source_sha='a'*40,workflow_sha='b'*40,run_id='1',run_attempt='1',helper_sha256=H.digest(H.__file__),configuration='MinSizeRel',editor_lineage=True,weather_consumer=True,
            lineage_adapter_sha256=H.digest(L.__file__),lineage_validator_sha256=H.digest(HERE/'qualify-editor-fps.py'))
        self.case_names=['EditorFPSLineage_'+s for s in ('AuthoredSceneLoadedByInstalledFPS','MalformedStartupFailsClosed','MissingCookedAssetFailsClosed','MissingRealModuleFailsClosed')]
        for name in L.SOURCES:
            path=self.source/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('source fixture')
        (self.source/L.SOURCES[0]).write_text('\n'.join('TEST('+n+') {}' for n in self.case_names))
        self.identity['images']={}
        for name in ('SparkEngine.exe','SparkGameFPS.dll','SparkGameFPS.dll.sparkabi','SparkTests.exe','SparkCooker.exe','SparkCrashReporter.exe'):
            path=self.build/'bin/MinSizeRel'/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(name.encode());self.identity['images'][str(path)]=H.digest(path)
        argv=[str(shutil.which('cmake')),'-DSPARK_ENGINE_BUILD_DIR='+self.build.as_posix(),'-DSPARK_SOURCE_ROOT='+self.source.as_posix(),'-DSPARK_CONFIG=MinSizeRel','-DSPARK_TESTS_EXECUTABLE='+(self.build/'bin/MinSizeRel/SparkTests.exe').as_posix(),'-DSPARK_ENGINE_EXECUTABLE_NAME=SparkEngine.exe','-DSPARK_EXPECTED_SOURCE_SHA='+'a'*40,'-DSPARK_PYTHON_EXECUTABLE='+sys.executable.replace('\\','/'),'-P',(self.source/'cmake/RunEditorFPSInstalledLineage.cmake').as_posix()]
        self.discovery={'tests':[{'name':E.CTEST,'config':'MinSizeRel','command':argv,'properties':[{'name':'TIMEOUT','value':900},{'name':'RUN_SERIAL','value':True}]}]}
        H.save(self.root/'lineage-prebuild.json',self.discovery)

    def fixture(self):
        for path in L.members(self.root):
            path.parent.mkdir(parents=True,exist_ok=True);path.write_text('complete text\n')
        base=self.root/'lineage'
        H.save(base/'discovery.json',self.discovery)
        (base/'ctest.xml').write_text('<testsuite><testcase name="'+E.CTEST+'" status="run" time="2"/></testsuite>')
        xml='<testsuites><testsuite>'+''.join('<testcase name="'+n+'" time="0.1"/>' for n in self.case_names)+'</testsuite></testsuites>'
        (base/'installed/lineage-junit.xml').write_text(xml)
        report={'sourceSha':'a'*40,'configuration':'MinSizeRel','cases':dict.fromkeys(L.CASES,{}),'frameworkJUnit':{'sha256':H.digest(base/'installed/lineage-junit.xml')}}
        for field,name in [('installedHost','SparkEngine.exe'),('installedModule','SparkGameFPS.dll'),('installedSidecar','SparkGameFPS.dll.sparkabi')]:
            report[field]={'sha256':self.identity['images'][str(self.build/'bin/MinSizeRel'/name)]}
        H.save(base/'installed/lineage-validation.json',report)
        H.save(base/'commands.json',[{'argv':a,'cwd':str(self.source),'timeout':cap,'elapsed':1,'exitCode':0} for a,cap in zip(L.commands_for(self.root,self.source,E),(30,910))])
        self.rebind()

    def rebind(self):
        binding={key:self.identity[key] for key in L.KEYS}
        binding.update(passed=True,validator_sha256=self.identity['lineage_validator_sha256'],test_image_sha256=self.identity['images'][str(self.build/'bin/MinSizeRel/SparkTests.exe')],source_files={name:H.digest(self.source/name) for name in L.SOURCES},members={p.relative_to(self.root).as_posix():{'sha256':H.digest(p),'bytes':p.stat().st_size} for p in L.members(self.root)})
        H.save(self.root/'lineage/binding.json',binding)
        self.identity['lineage_binding_sha256']=H.digest(self.root/'lineage/binding.json')

    def test_complete_proof_and_exact_members(self):
        self.fixture();self.assertEqual(len(L.proof(H,self.root,self.source,self.identity)),55)

    def test_missing_and_truncated_full_proof_rejected(self):
        self.fixture();p=self.root/'lineage/installed/evidence/positive/runtime.log'
        p.unlink()
        with self.assertRaises((ValueError,OSError)):L.proof(H,self.root,self.source,self.identity)
        p.write_text('... Output removed since truncated');self.rebind()
        with self.assertRaises(ValueError):L.proof(H,self.root,self.source,self.identity)

    def test_skip_failure_and_missing_framework_case_rejected(self):
        self.fixture();p=self.root/'lineage/installed/lineage-junit.xml';original=p.read_text()
        for changed in (original.replace('/>','><skipped/></testcase>',1),original.replace('/>','><failure/></testcase>',1),original.replace(self.case_names[0],'UnrelatedCase')):
            p.write_text(changed);self.rebind()
            with self.assertRaises(ValueError):L.proof(H,self.root,self.source,self.identity)

    def test_command_environment_and_boolean_exit_rejected(self):
        self.fixture();p=self.root/'lineage/commands.json';original=H.proof_json(p)
        for change in ('env','config','boolean','zero'):
            rows=json.loads(json.dumps(original))
            if change=='env':rows[1]['argv'].remove('--unset=SPARK_FPS_INPUT_TRACE')
            if change=='config':rows[1]['argv'][rows[1]['argv'].index('MinSizeRel')]='Release'
            if change=='boolean':rows[1]['exitCode']=False
            if change=='zero':rows[1]['timeout']=0
            H.save(p,rows);self.rebind()
            with self.assertRaises(ValueError):L.proof(H,self.root,self.source,self.identity)

    def test_source_run_image_and_validator_binding_rejected(self):
        self.fixture()
        for key in ('run_attempt','source_sha','lineage_validator_sha256','lineage_adapter_sha256'):
            changed=dict(self.identity);changed[key]='changed'
            with self.assertRaises(ValueError):L.proof(H,self.root,self.source,changed)
        report_path=self.root/'lineage/installed/lineage-validation.json';report=H.proof_json(report_path);report['installedHost']['sha256']='f'*64;H.save(report_path,report);self.rebind()
        with self.assertRaises(ValueError):L.proof(H,self.root,self.source,self.identity)

    def test_ordinary_lineage_rejects_diagnostic_controls(self):
        self.fixture();p=self.root/'lineage/installed/evidence/positive/runtime.log'
        for token in ('SPARK_FPS_INPUT','[shadow-probe]','Arena autopilot on'):
            p.write_text(token);self.rebind()
            with self.assertRaises(ValueError):L.proof(H,self.root,self.source,self.identity)

    def test_reserve_exhaustion_launches_no_child(self):
        with patch.object(H.time,'monotonic',return_value=100),patch.object(H,'owned_run') as child:
            with self.assertRaises(TimeoutError):L.run(H,self.root,self.source,self.identity,1069)
            child.assert_not_called()

    def test_sole_build_reserves_300_plus_970_without_new_budget(self):
        clock=[1000.0];builds=[]
        H.save(self.root/'clock.json',{'deadline':15400})
        for name in ('SparkMismatchedModuleFixture.dll','SparkMismatchedModuleFixture.dll.sparkabi','SparkPreviousSdkModuleFixture.dll','SparkPreviousSdkModuleFixture.dll.sparkabi'):
            (self.build/'bin/MinSizeRel'/name).write_bytes(name.encode())
        def child(command,**kw):
            if command[0]=='git':
                text='a'*40 if command[3:]==['rev-parse','HEAD'] else 'b'*40 if command[3:]==['rev-parse','HEAD^{tree}'] else ''
                kw['stdout'].write((text+'\n').encode());kw['stdout'].flush()
            if '--show-only=json-v1' in command:kw['stdout'].write(json.dumps(self.discovery).encode())
            if '--preset' in command:
                self.assertIn('-DPython3_EXECUTABLE='+sys.executable,command)
                clock[0]+=900
            if '--build' in command:
                builds.append(command);self.assertEqual(kw['timeout'],8900)
                clock[0]+=kw['timeout']
            return subprocess.CompletedProcess(command,0)
        def focused(*args):
            clock[0]+=300
            return {'test_image_sha256':H.digest(self.build/'bin/MinSizeRel/SparkTests.exe')}
        def lineage(host,root,source,identity,deadline):
            self.assertEqual(deadline-clock[0],970)
            return 'c'*64
        with patch.object(H.time,'monotonic',side_effect=lambda:clock[0]),patch.object(H,'owned_run',side_effect=child),patch.object(H,'focused_native',side_effect=focused),patch.object(H,'lineage_adapter',return_value=L),patch.object(L,'run',side_effect=lineage),patch.object(H.shutil,'which',return_value=sys.executable),patch.dict(os.environ,{'GITHUB_SHA':'b'*40,'GITHUB_RUN_ID':'1','GITHUB_RUN_ATTEMPT':'1'}):
            H.run_phase('build',self.source,self.root,True,'a'*40,False,True)
        self.assertEqual(len(builds),1)

    def test_oversized_complete_proof_rejected(self):
        path=self.root/'large.log';path.write_bytes(b'x'*(L.CAP+1))
        with self.assertRaises(ValueError):L.checked_text(H,path)

    def test_failed_ctest_preserves_complete_failure_xml(self):
        def child(command,**kwargs):
            if '--show-only=json-v1' in command:
                kwargs['stdout'].write(json.dumps(self.discovery).encode());return subprocess.CompletedProcess(command,0)
            path=self.root/'lineage/ctest.xml';path.write_text('<testsuite><testcase name="failed"><failure/></testcase></testsuite>')
            raise subprocess.CalledProcessError(1,command)
        with patch.object(H.time,'monotonic',return_value=100),patch.object(H,'owned_run',side_effect=child):
            with self.assertRaises(subprocess.CalledProcessError):L.run(H,self.root,self.source,self.identity,1070)
        self.assertEqual((self.root/'lineage/failure-proof/ctest.xml').read_bytes(),(self.root/'lineage/ctest.xml').read_bytes())
        self.assertFalse((self.root/'lineage/binding.json').exists())

    def test_compact_retains_failure_not_success_and_preserves_caps(self):
        H.save(self.root/'build-failure.json',{'phase':'build','error':'test'})
        p=self.root/'lineage/failure-proof/ctest.xml';p.parent.mkdir(parents=True);p.write_text('<failed/>')
        H.compact(self.root,True,'a'*40,input_dispatch=False,source=self.source,editor_lineage=True)
        self.assertEqual((self.root/'diagnostics-text/lineage/failure-proof/ctest.xml').read_bytes(),b'<failed/>')


if __name__=='__main__':unittest.main(verbosity=2)
