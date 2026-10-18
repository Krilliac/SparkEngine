"""Same-build installed lineage, using the existing product validators unchanged."""
import importlib.util
import math
from pathlib import Path
import shutil
import subprocess
import sys

RESERVE = 970
CAP = 131072
CASES = ('positive', 'malformed', 'missing-asset', 'missing-module')
CASE_FILES = ('runtime.log', 'exit-code.txt', 'reopened.sparkscene',
    'CrateProject/Scenes/Default.sparkscene', 'CrateProject/Cooked/Scenes/Default.sparkscene',
    'CrateProject/CrateProject.sparkproject', 'CrateProject/Cooked/CrateProject.sparkproject',
    'CrateProject/Cooked/Assets/spark-cook-manifest.json', 'Package/Startup.sparkscene',
    'Package/Scenes/Startup.sparkscene', 'Package/spark.modules.json', 'Package/Assets/spark-cook-manifest.json')
INSTALLED_FILES = ('lineage-validation.json', 'lineage-junit.xml', 'lineage-output.log')
KEYS = ('source_sha', 'workflow_sha', 'run_id', 'run_attempt', 'helper_sha256', 'configuration')
SOURCES = ('Tests/TestEditorCookPackageReal.cpp', 'cmake/RunEditorFPSInstalledLineage.cmake',
           'cmake/ValidateEditorFPSLineageRuntime.cmake', 'cmake/RunSparkModuleProfileLifecycle.cmake',
           'Tests/PackageSmoke/ValidateEditorFPSLineage.py')


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validators():
    return load(Path(__file__).with_name('qualify-editor-fps.py'), '_same_build_editor_contract')


def names(source):
    import re
    text = (source/'Tests/TestEditorCookPackageReal.cpp').read_text()
    result = re.findall(r'\bTEST\s*\(\s*(EditorFPSLineage_\w+)\s*\)', text)
    if len(result) != 4 or len(set(result)) != 4:
        raise ValueError('Lineage source membership changed')
    return result


def members(root):
    base = root/'lineage'
    return [base/'discovery.json', base/'ctest.xml',
            base/'commands.json',
            *[base/'installed'/name for name in INSTALLED_FILES],
            *[base/'installed/evidence'/case/name for case in CASES for name in CASE_FILES]]


def checked_text(H, path):
    H.input_plain(path)
    if not 0 < path.stat().st_size <= CAP:
        raise ValueError('Missing, empty or oversized complete lineage proof: '+str(path))
    with path.open('rb') as stream:
        data = stream.read(CAP+1)
    if not 0<len(data)<=CAP:
        raise ValueError('Lineage proof changed size during bounded read')
    text = data.decode('utf-8')
    if '\0' in text or '... Output removed since' in text:
        raise ValueError('Invalid or truncated lineage proof')
    return data


def commands_for(root, source, E):
    base=root/'lineage';build=source/'build/windows-shipping';tmp=base/'tmp'
    discovery=['ctest','--test-dir',build,'-C','MinSizeRel','-R','^'+E.CTEST+'$','--show-only=json-v1']
    env=['cmake','-E','env',*[f'--unset={key}' for key in ('SPARK_TEST_FILE','SPARK_TEST_NAME','SPARK_TEST_NAME_PREFIX','SPARK_TEST_EXPECT_COUNT','SPARK_TEST_EXCLUDE','SPARK_TEST_LIMIT','SPARK_FPS_INPUT_TRACE','SPARK_SHADOW_DEVICE_TRACE')],
         'TEMP='+str(tmp),'TMP='+str(tmp),'LOCALAPPDATA='+str(tmp/'localappdata'),'APPDATA='+str(tmp/'appdata')]
    ctest=[*env,'ctest','--test-dir',build,'-C','MinSizeRel','-R','^'+E.CTEST+'$','--parallel','1','--output-on-failure','--no-tests=error','--output-junit',base/'ctest.xml']
    return [[str(value) for value in command] for command in (discovery,ctest)]


def proof(H, root, source, identity):
    base = root/'lineage'
    binding = H.proof_json(base/'binding.json')
    if binding.get('passed') is not True or any(binding.get(k) != identity.get(k) for k in KEYS):
        raise ValueError('Lineage identity differs from current qualification')
    if binding.get('validator_sha256') != identity.get('lineage_validator_sha256') or identity.get('lineage_validator_sha256') != H.digest(Path(__file__).with_name('qualify-editor-fps.py')):
        raise ValueError('Lineage harness validator changed')
    if identity.get('lineage_adapter_sha256') != H.digest(__file__):
        raise ValueError('Lineage adapter identity changed')
    if binding.get('source_files') != {name:H.digest(source/name) for name in SOURCES}:
        raise ValueError('Lineage product validation source changed')
    E = validators()
    build = source/'build/windows-shipping'
    E.discovery(H.proof_json(base/'discovery.json'), True, source, build, identity['source_sha'])
    if E.discovery(H.proof_json(root/'lineage-prebuild.json')) != E.discovery(H.proof_json(base/'discovery.json')):
        raise ValueError('Lineage configured policy changed across build')
    commands=H.proof_json(base/'commands.json')
    if not isinstance(commands,list) or len(commands)!=2:
        raise ValueError('Incomplete lineage command accounting')
    for row,cap,argv in zip(commands,(30,910),commands_for(root,source,E)):
        if row.get('argv')!=argv or row.get('cwd')!=str(source) or type(row.get('exitCode')) is not int or row['exitCode']!=0 or any(type(row.get(k)) not in (int,float) or not math.isfinite(row[k]) or not 0<=row[k]<=cap for k in ('timeout','elapsed')) or row['timeout']<=0:
            raise ValueError('Lineage command did not complete within its bound')
    E.validate_xml(base/'ctest.xml', [E.CTEST], cap=CAP, ctest=True)
    E.validate_xml(base/'installed/lineage-junit.xml', names(source), cap=CAP)
    report = H.proof_json(base/'installed/lineage-validation.json')
    if report.get('sourceSha') != identity['source_sha'] or report.get('configuration') != 'MinSizeRel' or set(report.get('cases', {})) != set(CASES):
        raise ValueError('Lineage report source/configuration/cases changed')
    for field, name in [('installedHost','SparkEngine.exe'), ('installedModule','SparkGameFPS.dll'), ('installedSidecar','SparkGameFPS.dll.sparkabi')]:
        if report[field]['sha256'] != identity['images'][str(build/'bin/MinSizeRel'/name)]:
            raise ValueError('Lineage installed image differs from shared build')
    if binding.get('test_image_sha256') != identity['images'][str(build/'bin/MinSizeRel/SparkTests.exe')]:
        raise ValueError('Lineage framework image differs from shared build')
    if report['frameworkJUnit']['sha256'] != H.digest(base/'installed/lineage-junit.xml'):
        raise ValueError('Lineage framework hash mismatch')
    actual = {path.relative_to(root).as_posix(): {'sha256':H.digest(path), 'bytes':len(checked_text(H,path))} for path in members(root)}
    for case in CASES:
        text=(base/'installed/evidence'/case/'runtime.log').read_text()
        if any(token in text for token in ('SPARK_FPS_INPUT','[shadow-probe]','Arena autopilot on')):
            raise ValueError('Unexpected diagnostic input/autoplay in ordinary lineage')
    if binding.get('members') != actual:
        raise ValueError('Lineage complete proof changed')
    return members(root)+[base/'binding.json']


def run(H, root, source, identity, deadline):
    if H.remaining(deadline, RESERVE) < RESERVE:
        raise TimeoutError('Insufficient complete lineage reserve')
    base = root/'lineage';base.mkdir(exist_ok=False)
    E = validators();build = source/'build/windows-shipping'
    if identity.get('lineage_adapter_sha256')!=H.digest(__file__) or identity.get('lineage_validator_sha256')!=H.digest(Path(__file__).with_name('qualify-editor-fps.py')):
        raise ValueError('Lineage Q scripts changed before execution')
    expected_commands=commands_for(root,source,E)
    commands=[]
    def child(label, command, ceiling):
        timeout=H.remaining(deadline,ceiling)
        commands.append({'argv':[str(x) for x in command],'timeout':timeout,'cwd':str(source)})
        H.save(base/'commands.json',commands)
        start=H.time.monotonic()
        with (base/(label+'.log')).open('wb') as output:
            H.owned_run([str(x) for x in command],cwd=source,stdout=output,stderr=subprocess.STDOUT,timeout=timeout,check=True)
        commands[-1].update(exitCode=0,elapsed=H.time.monotonic()-start)
        H.save(base/'commands.json',commands)
    staged = E.scratch_root(base,build)
    try:
        child('discovery',expected_commands[0],30)
        discovery=H.proof_json(base/'discovery.log')
        E.discovery(discovery,True,source,build,identity['source_sha'])
        H.save(base/'discovery.json',discovery)
        tmp=base/'tmp';tmp.mkdir(exist_ok=False)
        (tmp/'localappdata').mkdir();(tmp/'appdata').mkdir()
        child('ctest',expected_commands[1],910)
        H.remaining(deadline,30)
        E.validate_xml(base/'ctest.xml',[E.CTEST],cap=CAP,ctest=True)
        E.validate_xml(staged/'lineage-junit.xml',names(source),cap=CAP)
        if not (staged/'.spark-editor-fps-installed-lineage').is_file():
            raise ValueError('Installed lineage scratch ownership marker missing')
        # Replay the actual product payload validator; the CTest wrapper also ran
        # its unchanged strict CMake lifecycle parser before producing this report.
        V=load(source/'Tests/PackageSmoke/ValidateEditorFPSLineage.py','_same_build_payload_validator')
        replay=V.validate(staged/'evidence',staged/'prefix/bin/SparkEngine.exe',staged/'prefix/bin/SparkGameFPS.dll',identity['source_sha'],'MinSizeRel',staged/'lineage-junit.xml')
        if replay != H.proof_json(staged/'lineage-validation.json'):
            raise ValueError('Lineage payload replay differs from wrapper report')
        for old,new in [(staged/name,base/'installed'/name) for name in INSTALLED_FILES]+[(staged/'evidence'/case/name,base/'installed/evidence'/case/name) for case in CASES for name in CASE_FILES]:
            data=checked_text(H,old);new.parent.mkdir(parents=True,exist_ok=True);new.write_bytes(data)
        binding={k:identity[k] for k in KEYS}
        binding.update(passed=True,staged_root=str(staged),test_image_sha256=identity['images'][str(build/'bin/MinSizeRel/SparkTests.exe')],validator_sha256=H.digest(Path(__file__).with_name('qualify-editor-fps.py')),source_files={name:H.digest(source/name) for name in SOURCES},
            members={p.relative_to(root).as_posix():{'sha256':H.digest(p),'bytes':len(checked_text(H,p))} for p in members(root)})
        H.save(base/'binding.json',binding)
        proof(H,root,source,identity)
        H.remaining(deadline,30)
        return H.digest(base/'binding.json')
    except Exception as error:
        H.save(base/'failure.json',{'passed':False,'error':str(error)})
        raise
    finally:
        # Bounded named diagnostics only. Never copy the installed prefix or saves.
        diagnostics=[]
        for path in [staged/name for name in ('install-runtime.log','install-samples.log','install-redist.log','sparktests.log','lineage-validation-error.log')]+[staged/'evidence'/case/'runtime.log' for case in CASES]:
            if path.is_file():
                H.input_plain(path)
                target=base/'diagnostics'/path.relative_to(staged)
                target.parent.mkdir(parents=True,exist_ok=True)
                with path.open('rb') as stream:
                    stream.seek(max(0,path.stat().st_size-16384));raw=stream.read(16384)
                text=raw.decode('utf-8',errors='replace').encode('utf-8')
                if len(text)>16384:text=text[-16384:].decode('utf-8',errors='ignore').encode('utf-8')
                target.write_bytes(text)
                diagnostics.append({'path':target.relative_to(root).as_posix(),'originalBytes':path.stat().st_size,'originalSha256':H.digest(path),'retainedBytes':target.stat().st_size,'retainedSha256':H.digest(target),'tail_only':path.stat().st_size!=target.stat().st_size or raw!=text,'diagnostic_reencoded':raw!=text})
        H.save(base/'diagnostics.json',diagnostics)
        if (base/'failure.json').exists():
            for path,name in ((base/'ctest.xml','ctest.xml'),(staged/'lineage-junit.xml','framework.xml')):
                if path.is_file():
                    data=checked_text(H,path);target=base/'failure-proof'/name
                    target.parent.mkdir(exist_ok=True);target.write_bytes(data)
