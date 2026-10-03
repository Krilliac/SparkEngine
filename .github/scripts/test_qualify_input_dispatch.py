from pathlib import Path
import importlib.util, tempfile, unittest, json, hashlib
from unittest import mock
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('input_qualification',HERE/'qualify-installed-native.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class InputQualificationTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='input-qualification-')
        self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.source=self.root/'source';self.evidence=self.root/'evidence'
        self.source.mkdir();self.evidence.mkdir()
        self.identity={'source_sha':'a'*40,'workflow_sha':'b'*40,'run_id':'1','run_attempt':'1','helper_sha256':'c'*64}
    def write(self,path,data=b'x'):
        path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data);return path
    def sources(self):
        for name in m.INPUT_SOURCES:self.write(self.source/name)
        m.input_source_identity(self.identity,self.source,capture=True)
    def test_source_changes_are_rejected(self):
        self.sources();m.input_source_identity(self.identity,self.source)
        self.write(self.source/m.INPUT_SOURCES[1],b'changed')
        with self.assertRaisesRegex(ValueError,'identity changed'):m.input_source_identity(self.identity,self.source)
    def test_untracked_input_source_rejected(self):
        self.sources()
        with self.assertRaisesRegex(ValueError,'not tracked'):
            m.input_source_identity(self.identity,self.source,git=lambda *args:'\n'.join(m.INPUT_SOURCES[:-1]))
    def test_missing_source_rejected(self):
        self.sources();(self.source/m.INPUT_SOURCES[0]).unlink()
        with self.assertRaises(FileNotFoundError):m.input_source_identity(self.identity,self.source)
    def test_complete_text_cap_and_binary_rejected(self):
        p=self.write(self.root/'proof.log',b'a'*131072);self.assertEqual(len(m.input_text(p)),131072)
        for raw in (b'a'*131073,b'a\0b',b'\xff'):
            p.write_bytes(raw)
            with self.assertRaises((ValueError,UnicodeError)):m.input_text(p)
    def test_installed_images_bound_to_built_images(self):
        binary=self.source/'build/windows-shipping/bin/MinSizeRel';installed=self.evidence/'sdk/prefix/bin'
        self.identity['images']={}
        for name in ('SparkEngine.exe','SparkGameFPS.dll','SparkGameFPS.dll.sparkabi','SparkTests.exe'):
            p=self.write(binary/name,name.encode());self.identity['images'][str(p)]=m.digest(p)
            if name!='SparkTests.exe':self.write(installed/name,name.encode())
        self.assertEqual(len(m.input_images(self.identity,self.evidence,self.source)),4)
        self.write(installed/'SparkGameFPS.dll',b'substitution')
        with self.assertRaisesRegex(ValueError,'differs'):m.input_images(self.identity,self.evidence,self.source)
    def test_package_requires_real_fps_assets_and_detects_mutation(self):
        prefix=self.evidence/'sdk/prefix'
        for name in ('bin/SparkEngine.exe','bin/SparkGameFPS.dll','bin/SparkGameFPS.dll.sparkabi','bin/Assets/Models/pistol.obj','bin/Assets/Models/rifle.obj'):
            self.write(prefix/name)
        self.write(prefix/'bin/Assets/assets.integrity.json',b'{"entries":[]}')
        with mock.patch.object(m,'remaining',return_value=1):
            before=m.input_package(self.evidence,1)
            self.write(prefix/'bin/Assets/Models/rifle.obj',b'changed')
            self.assertNotEqual(before,m.input_package(self.evidence,1))
            (prefix/'bin/Assets/Models/pistol.obj').unlink()
            with self.assertRaisesRegex(ValueError,'content'):m.input_package(self.evidence,1)
    def test_input_requires_weather_mode(self):
        with self.assertRaisesRegex(ValueError,'weather'):m.run_phase('closure',self.source,self.evidence,input_dispatch=True)
    def test_no_input_launch_without_full_reserve(self):
        run=mock.Mock()
        with mock.patch.object(m,'remaining',return_value=119):
            with self.assertRaises(TimeoutError):m.run_input_phase(self.evidence,self.identity,self.source,1,run)
        run.assert_not_called()
    def test_no_input_launch_without_successful_same_source_abi(self):
        run=mock.Mock();self.write(self.evidence/'abi-binding.json',json.dumps({'passed':True,'source_sha':'wrong'}).encode())
        with mock.patch.object(m,'remaining',return_value=150):
            with self.assertRaisesRegex(ValueError,'ABI'):m.run_input_phase(self.evidence,self.identity,self.source,1,run)
        run.assert_not_called()
    def test_exact_owned_caller_and_binding(self):
        self.sources();self.write(self.evidence/'abi-binding.json',json.dumps({'passed':True,'source_sha':'a'*40}).encode())
        self.write(self.evidence/'input-assets.log',b'verified assets\n')
        run=mock.Mock();members={'input/game.log':{'bytes':3,'sha256':'d'*64}}
        with mock.patch.object(m,'remaining',return_value=150), mock.patch.object(m,'input_images',return_value={'host':'sha'}), mock.patch.object(m,'input_package',return_value={'files':5,'sha256':'e'*64}), mock.patch.object(m,'input_proof',return_value=members):
            m.run_input_phase(self.evidence,self.identity,self.source,1,run)
        label,argv,timeout=run.call_args.args
        self.assertEqual((label,timeout),('input-dispatch',90))
        self.assertEqual(argv[argv.index('--decoder-helper-sha256')+1],self.identity['input_sources'][m.INPUT_SOURCES[1]])
        self.assertEqual(argv[argv.index('--decoder')+1],self.source/'build/windows-shipping/bin/MinSizeRel/SparkTests.exe')
        binding=m.proof_json(self.evidence/'input-binding.json')
        self.assertEqual(binding['members']['input/game.log'],members['input/game.log']);self.assertEqual(binding['run_attempt'],'1')
    def test_native_failure_cannot_publish_binding(self):
        self.sources();self.write(self.evidence/'abi-binding.json',json.dumps({'passed':True,'source_sha':'a'*40}).encode())
        self.write(self.evidence/'input-assets.log',b'verified assets\n')
        with mock.patch.object(m,'remaining',return_value=150),mock.patch.object(m,'input_images',return_value={}),mock.patch.object(m,'input_package',return_value={}),mock.patch.object(m,'input_proof') as proof:
            with self.assertRaises(RuntimeError):m.run_input_phase(self.evidence,self.identity,self.source,1,mock.Mock(side_effect=RuntimeError('owned child failed')))
            proof.assert_not_called()
        self.assertFalse((self.evidence/'input-binding.json').exists())
    def test_package_mutation_cannot_publish_binding(self):
        self.sources();self.write(self.evidence/'abi-binding.json',json.dumps({'passed':True,'source_sha':'a'*40}).encode())
        self.write(self.evidence/'input-assets.log',b'verified assets\n')
        with mock.patch.object(m,'remaining',return_value=150),mock.patch.object(m,'input_images',return_value={}),mock.patch.object(m,'input_package',side_effect=[{'sha':'old'},{'sha':'new'}]),mock.patch.object(m,'input_proof',return_value={}):
            with self.assertRaisesRegex(ValueError,'closure changed'):m.run_input_phase(self.evidence,self.identity,self.source,1,mock.Mock())
        self.assertFalse((self.evidence/'input-binding.json').exists())

    def proof_fixture(self):
        self.sources()
        for name in m.INPUT_SOURCES[:2]:
            self.write(self.source/name,(HERE.parents[1]/name).read_bytes())
        m.input_source_identity(self.identity,self.source,capture=True)
        binary=self.source/'build/windows-shipping/bin/MinSizeRel';installed=self.evidence/'sdk/prefix/bin'
        self.identity['images']={}
        for name in ('SparkEngine.exe','SparkGameFPS.dll','SparkGameFPS.dll.sparkabi','SparkTests.exe'):
            path=self.write(binary/name,name.encode());self.identity['images'][str(path)]=m.digest(path)
            if name!='SparkTests.exe':self.write(installed/name,name.encode())
        driver=m.input_module('_test_real_driver',self.source/m.INPUT_SOURCES[0])
        decoder=m.input_module('_test_real_decoder',self.source/m.INPUT_SOURCES[1])
        profile={'fps.profile.class':'0','fps.profile.health':'100.000000','fps.profile.playTime':'0.000000'}
        encoded=''.join(k+'='+v+'\n' for k,v in sorted(profile.items())).encode().hex()
        base=self.evidence/'input';negative=base/'missing-slot'
        def trace(directory,missing=False):
            replay=driver.Replay(directory/'local/SparkEngine/Saves',missing_only=missing);rows=[]
            masks=[0,2,0] if missing else [0,4,0,1,1,1,0,8,0,2,2,2,0,2,2,2,0]
            previous=serial=0;live=dict(profile)
            for index,mask in enumerate(masks,1):
                pressed=mask & ~previous;released=previous & ~mask
                action=(1 if pressed==1 else 2 if pressed==2 else 0)
                encode=lambda p:''.join(k+'='+v+'\n' for k,v in sorted(p.items())).encode().hex()
                fields=dict(v=1,phase='before',input=index,update=index,mask=mask,pressed=pressed,released=released,paused=0,action=0,result=-1,reason=0,operation=serial,faults=0,profile=encode(live),transfer='-',saves=str(directory/'local/SparkEngine/Saves').encode().hex())
                phases=['before']+(['operation'] if action else [])+['dispatch','complete']
                for phase in phases:
                    fields['phase']=phase
                    if phase=='operation':
                        serial+=1
                        if not missing:live=dict(profile)
                        fields.update(action=action,result=0 if missing else 1,reason=int(missing),operation=serial,transfer='-' if missing else encoded,profile=encode(live))
                    if phase=='dispatch':
                        if pressed==4:live['fps.profile.class']=driver.SCOUT
                        if pressed==8:live['fps.profile.class']=driver.VANGUARD
                        fields['profile']=encode(live)
                    row='SPARK_FPS_INPUT '+' '.join(k+'='+str(fields[k]) for k in driver.TRACE_FIELDS)+'\n'
                    replay.feed(row);rows.append(row)
                previous=mask
            rows.append(f'SPARK_FPS_INPUT_END v=1 records={replay.records} bytes={replay.byte_count} failed=0\n')
            rows.append('FPS scene identity: authored scene "FPS Arena" (2 nodes) from '+str(self.evidence/'sdk/prefix/bin/Assets/Scenes/level1.scene')+'\n')
            rows.append('SPARK_D3D11_DEVICE driver=warp certification=software-only\n')
            rows.append('SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=1 fixed=1 render=1 unload=1 destroy=1 faults=0\n')
            self.write(directory/'game.log',''.join(rows).encode());return replay
        trace(base);neg=trace(negative,True)
        neg_report={'passed':True,'case':'missing-slot','operation':neg.operations[0]}
        self.write(negative/'receipt.json',json.dumps(neg_report).encode())
        work=base/'save-decode-fixture'
        attrs='tests="1" failures="0" errors="0" skipped="0" flaky="0" empty="0"'
        xml=f'<testsuites {attrs}><testsuite {attrs}><testcase name="{decoder.CASE}" time="0.01"/></testsuite></testsuites>'
        junit=self.write(work/'framework.xml',xml.encode())
        native={'inputSha256':'d'*64,'inputBytes':12,'canonicalProfile':profile}
        self.write(work/'stdout.log',(decoder.PREFIX+json.dumps(native)+'\n').encode());self.write(work/'stderr.log',b'')
        decoded={'scope':'copied-primary-content-only','input':{'sha256':'d'*64,'bytes':12},'decoderExecutable':{'sha256':m.digest(binary/'SparkTests.exe'),'bytes':14},'canonicalProfile':profile,'frameworkJUnit':{'sha256':m.digest(junit),'bytes':junit.stat().st_size}}
        self.write(work/'content-receipt.json',json.dumps(decoded).encode())
        report={'passed':True,'fullShippingQualification':False,'source':'a'*40,'identities':{k:v for k,v in m.input_images(self.identity,self.evidence,self.source).items() if not k.endswith('.sparkabi')},'decoderHelperSha256':self.identity['input_sources'][m.INPUT_SOURCES[1]],'missingSlot':neg_report,'elapsedSeconds':4.0,'savedProfile':profile,'decoderWork':str(work),'decoded':decoded,'slotSha256':'d'*64}
        self.write(base/'receipt.json',json.dumps(report).encode());return base,work
    def test_real_replayers_accept_complete_fixture_and_exclude_binary_saves(self):
        base,work=self.proof_fixture();self.write(base/'saved-primary.spark_save',b'never executable')
        members=m.input_proof(self.evidence,self.identity,self.source)
        self.assertEqual(len(members),8);self.assertTrue(all(not p.endswith('.spark_save') for p in members))
    def test_real_replayers_reject_fault_and_missing_terminal(self):
        base,work=self.proof_fixture();p=base/'game.log';original=p.read_bytes()
        for changed in (original.replace(b'faults=0',b'faults=1',1),b'\n'.join(x for x in original.split(b'\n') if not x.startswith(b'SPARK_FPS_INPUT_END'))):
            p.write_bytes(changed)
            with self.assertRaises(ValueError):m.input_proof(self.evidence,self.identity,self.source)
        p.write_bytes(original)
    def test_reject_decoder_path_escape_and_image_substitution(self):
        base,work=self.proof_fixture();p=base/'receipt.json';report=json.loads(p.read_text())
        report['decoderWork']=str(self.root/'outside');p.write_text(json.dumps(report))
        with self.assertRaisesRegex(ValueError,'unowned'):m.input_proof(self.evidence,self.identity,self.source)
        report['decoderWork']=str(work);report['identities']['wrong']='a'*64;p.write_text(json.dumps(report))
        with self.assertRaisesRegex(ValueError,'binding'):m.input_proof(self.evidence,self.identity,self.source)
    def test_reject_decoder_junit_and_content_substitution(self):
        base,work=self.proof_fixture();p=work/'framework.xml';p.write_text(p.read_text().replace('failures="0"','failures="1"'))
        with self.assertRaises(ValueError):m.input_proof(self.evidence,self.identity,self.source)

    def mutate_complete_trace(self,path,mutate):
        rows=path.read_text().splitlines(keepends=True)
        rows=mutate(rows)
        trace=[x for x in rows if x.startswith('SPARK_FPS_INPUT ')]
        terminal=f'SPARK_FPS_INPUT_END v=1 records={len(trace)} bytes={sum(len(x.encode()) for x in trace)} failed=0\n'
        path.write_text(''.join(terminal if x.startswith('SPARK_FPS_INPUT_END ') else x for x in rows),encoding='utf-8',newline='\n')
    def test_reject_incomplete_full_sequence_with_valid_terminal_counts(self):
        base,work=self.proof_fixture();path=base/'game.log';original=path.read_bytes()
        # Missing F9 mutation, held-save frame, and release before repress respectively.
        for update in (8,5,13):
            path.write_bytes(original)
            self.mutate_complete_trace(path,lambda rows:[x for x in rows if f' update={update} ' not in x])
            with self.subTest(update=update),self.assertRaises(ValueError):m.input_proof(self.evidence,self.identity,self.source)
    def test_reject_later_nonclass_state_change(self):
        base,work=self.proof_fixture();path=base/'game.log'
        def change(rows):
            result=[]
            for row in rows:
                if ' update=11 ' in row:
                    start=row.index('profile=')+8;end=row.index(' ',start)
                    values=bytes.fromhex(row[start:end]).decode().replace('fps.profile.health=100.000000','fps.profile.health=1.000000').encode().hex()
                    row=row[:start]+values+row[end:]
                result.append(row)
            return result
        self.mutate_complete_trace(path,change)
        with self.assertRaises(ValueError):m.input_proof(self.evidence,self.identity,self.source)
    def test_reject_fallback_arena(self):
        base,work=self.proof_fixture();path=base/'game.log';path.write_text(path.read_text().replace('authored scene "FPS Arena"','procedural fallback arena'))
        with self.assertRaisesRegex(ValueError,'fallback'):m.input_proof(self.evidence,self.identity,self.source)
    def test_preidentity_build_failure_still_collects_diagnostics(self):
        self.write(self.evidence/'build-failure.json',b'{"phase":"build","error":"wrong source"}')
        m.compact(self.evidence,weather_consumer=True,source_sha='a'*40,input_dispatch=True,source=self.source)
        self.assertTrue((self.evidence/'diagnostics-text/build-failure.json').is_file())
        self.assertFalse((self.evidence/'diagnostics-text/input-binding.json').exists())

if __name__=='__main__':unittest.main()
