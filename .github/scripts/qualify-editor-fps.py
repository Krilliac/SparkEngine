"""Bounded MinSizeRel editor/FPS slice. No full-suite or physical Shipping claim."""
import argparse
import hashlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
import zipfile

MIB = 1024 * 1024
CONFIG = 'MinSizeRel'
CTEST = 'EditorFPSLineage_InstalledRuntime'
PREFIX = 'SceneManager_ReflectedGameplay'
TARGETS = ['SparkEngine', 'SparkGameFPS', 'SparkTests', 'SparkCooker', 'SparkCrashReporter']
PHASES = {'configure': 870, 'build': 7170, 'test': 1170, 'pack': 570}
RESERVE = {'configure': 150 * 60, 'build': 30 * 60, 'test': 10 * 60, 'pack': 30}
CASES = ('positive', 'malformed', 'missing-asset', 'missing-module')
SOURCE_FILES = ('CMakePresets.json', 'Tests/CMakeLists.txt',
    'Tests/TestEditorCookPackageReal.cpp', 'Tests/TestSceneManagerReflectedReal.cpp',
    'cmake/RunEditorFPSInstalledLineage.cmake', 'cmake/ValidateEditorFPSLineageRuntime.cmake',
    'cmake/RunSparkModuleProfileLifecycle.cmake', 'Tests/PackageSmoke/ValidateEditorFPSLineage.py',
    'SparkEngine/Source/SceneManager/SceneManagerReflected.cpp',
    'GameModules/SparkGameFPS/Source/Game/Game.cpp', '.github/scripts/qualify-installed-native.py')
CAPS = {'jobSeconds': 14400, 'configureSeconds': 900, 'buildSeconds': 7200,
    'testSeconds': 1200, 'packSeconds': 600, 'zipBytes': MIB, 'expandedBytes': 12*MIB,
    'jsonBytes': MIB, 'ctestXmlBytes': 4*MIB, 'runtimeProofBytes': 128*1024}


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def bounded_bytes(path, cap):
    path = Path(path)
    for part in (path, *path.parents):
        if part.is_symlink() or (hasattr(part, 'is_junction') and part.is_junction()):
            raise ValueError('Linked proof path is not allowed')
    if path.stat().st_size > cap:
        raise ValueError('Evidence exceeds read cap')
    with path.open('rb') as stream:
        data = stream.read(cap+1)
    if len(data)>cap or b'\0' in data:
        raise ValueError('Oversize or NUL evidence')
    return data


def read_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate JSON key')
            result[key] = value
        return result
    return json.loads(bounded_bytes(path, MIB).decode('utf-8-sig'), object_pairs_hook=unique,
                      parse_constant=lambda x: (_ for _ in ()).throw(ValueError('Nonfinite JSON')))


def save(path, data):
    Path(path).write_text(json.dumps(data, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def adapter(source):
    spec = importlib.util.spec_from_file_location('_editor_fps_lifetime', source / '.github/scripts/qualify-installed-native.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def pins(args, env):
    expected = {'GITHUB_SHA': args.workflow_sha, 'GITHUB_RUN_ID': args.run_id,
                'GITHUB_RUN_ATTEMPT': args.run_attempt}
    if not re.fullmatch(r'[0-9a-f]{40}', args.source_sha) or not re.fullmatch(r'[0-9a-f]{40}', args.workflow_sha):
        raise ValueError('Explicit immutable source/workflow SHA required')
    if not all(re.fullmatch(r'[1-9][0-9]*', v) for v in (args.run_id, args.run_attempt)):
        raise ValueError('Explicit run/attempt required')
    if any(env.get(key) != value for key, value in expected.items()):
        raise ValueError('Workflow/run identity does not match hosted environment')
    return {'source_sha': args.source_sha, 'workflow_sha': args.workflow_sha,
            'run_id': args.run_id, 'run_attempt': args.run_attempt}


def configure_command(source, build, sha):
    return ['cmake', '--fresh', '--preset', 'windows-shipping', '-B', str(build),
        '-G', 'Ninja Multi-Config', '-A', '', '-T', '', '-DCMAKE_CONFIGURATION_TYPES=MinSizeRel',
        '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl', '-DBUILD_TESTS=ON',
        '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded', '-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON',
        '-DCMAKE_CXX_SCAN_FOR_MODULES=OFF', '-DGENERATE_DEBUG_SYMBOLS=OFF',
        '-DSPARK_EXPECTED_SOURCE_SHA=' + sha, '-DPython3_EXECUTABLE='+sys.executable]


def cache_contract(source, build, source_sha):
    values = {}
    for line in (build / 'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
        if line and not line.startswith(('#', '//')) and '=' in line and ':' in line.split('=', 1)[0]:
            key, value = line.split('=', 1)
            values[key.split(':', 1)[0]] = value
    preset = next(p for p in read_json(source / 'CMakePresets.json')['configurePresets'] if p['name'] == 'windows-shipping')
    expected = dict(preset['cacheVariables'], BUILD_TESTS='ON', CMAKE_CONFIGURATION_TYPES=CONFIG,
                    CMAKE_GENERATOR='Ninja Multi-Config', CMAKE_MSVC_DEBUG_INFORMATION_FORMAT='Embedded',
                    CMAKE_DISABLE_PRECOMPILE_HEADERS='ON', CMAKE_CXX_SCAN_FOR_MODULES='OFF',
                    GENERATE_DEBUG_SYMBOLS='OFF', SPARK_EXPECTED_SOURCE_SHA=source_sha,
                    Python3_EXECUTABLE=sys.executable)
    if any(values.get(key) != str(value) for key, value in expected.items()):
        raise ValueError('Shipping preset/cache contract changed')
    if values.get('ENABLE_CONSOLE_IN_SHIPPING') != 'OFF' or values.get('ENABLE_DEVCOMMANDS_IN_SHIPPING') != 'OFF':
        raise ValueError('Shipping developer facilities enabled')
    for compiler in ('CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER'):
        if Path(values.get(compiler, '')).name.casefold() not in ('cl', 'cl.exe'):
            raise ValueError('MSVC compiler selection changed')
    metadata = list((build/'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
    if len(metadata)!=1:
        raise ValueError('Ambiguous compiler metadata')
    text = bounded_bytes(metadata[0], MIB).decode('utf-8')
    if not re.search(r'set\(CMAKE_CXX_COMPILER_ID "MSVC"\)', text):
        raise ValueError('Compiler is not MSVC')
    version = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "(19\.[34][0-9]\.[0-9.]+)"\)', text)
    if not version or os.environ.get('VisualStudioVersion')!='17.0' or os.environ.get('VSCMD_ARG_TGT_ARCH')!='x64':
        raise ValueError('Expected VS2022 v143 x64 toolchain')
    if not re.fullmatch(r'14\.[34][0-9]\.[0-9.]+', os.environ.get('VCToolsVersion', '')):
        raise ValueError('Expected v143 tools version')
    return {'cache': {key: values[key] for key in expected}, 'compilerVersion': version.group(1),
            'compilerMetadataSha256': digest(metadata[0]), 'compilerMetadataPath': str(metadata[0]),
            'VisualStudioVersion': os.environ['VisualStudioVersion'], 'VCToolsVersion': os.environ['VCToolsVersion'],
            'architecture': 'x64'}


def discovery(document, require_command=False, source=None, build=None, source_sha=None):
    rows = document['tests']
    if len(rows) != 1 or rows[0]['name'] != CTEST:
        raise ValueError('Dedicated CTest inventory is not exact')
    row = rows[0]
    if row.get('config') != CONFIG:
        raise ValueError('Dedicated CTest discovery configuration changed')
    props = {p['name']: p['value'] for p in row.get('properties', [])}
    if props.get('DISABLED', False) or props.get('TIMEOUT') != 900 or props.get('RUN_SERIAL') is not True:
        raise ValueError('Dedicated CTest policy changed')
    if require_command and (not row.get('command') or not all(isinstance(v, str) and v for v in row['command'])):
        raise ValueError('Unresolved postbuild CTest command')
    if require_command and source is not None:
        command = row['command']
        normalized = [v.replace('\\', '/') for v in command]
        expected = [str(shutil.which('cmake')).replace('\\', '/'),
                    '-DSPARK_ENGINE_BUILD_DIR='+build.as_posix(), '-DSPARK_SOURCE_ROOT='+source.as_posix(),
                    '-DSPARK_CONFIG='+CONFIG, '-DSPARK_TESTS_EXECUTABLE='+(build/'bin'/CONFIG/'SparkTests.exe').as_posix(),
                    '-DSPARK_ENGINE_EXECUTABLE_NAME=SparkEngine.exe', '-DSPARK_EXPECTED_SOURCE_SHA='+source_sha,
                    '-DSPARK_PYTHON_EXECUTABLE='+sys.executable.replace('\\', '/'),
                    '-P', (source/'cmake/RunEditorFPSInstalledLineage.cmake').as_posix()]
        if normalized != expected:
            raise ValueError('Dedicated CTest exact command binding changed')
    return props


def framework_names(source):
    names = re.findall(r'^TEST\((' + PREFIX + r'[A-Za-z0-9_]*)\)',
                       (source / 'Tests/TestSceneManagerReflectedReal.cpp').read_text(encoding='utf-8'), re.M)
    if not names or len(set(names)) != len(names):
        raise ValueError('Missing/duplicate reflected source cases')
    return names


def validate_xml(path, names, cap=4*MIB, ctest=False):
    data = bounded_bytes(path, cap)
    if len(data) > cap or b'\0' in data or b'<!DOCTYPE' in data or b'<!ENTITY' in data:
        raise ValueError('Invalid/oversize XML proof')
    text = data.decode('utf-8')
    cases = ET.fromstring(text).findall('.//testcase')
    if sorted(c.get('name', '') for c in cases) != sorted(names):
        raise ValueError('Incomplete or duplicate native case accounting')
    for case in cases:
        if ctest and case.get('status') != 'run':
            raise ValueError('CTest did not report execution')
        elapsed = float(case.get('time', 'nan'))
        if not math.isfinite(elapsed) or elapsed<0 or (ctest and elapsed>900):
            raise ValueError('Invalid native case duration')
        if case.get('status') not in (None, 'run') or any(case.find(n) is not None for n in ('failure', 'error', 'skipped')):
            raise ValueError('Native case did not execute successfully')
    return {'expected': len(names), 'executed': len(cases), 'failed': 0, 'skipped': 0}


def scratch_root(root, build):
    # Matches CMake REAL_PATH then MD5 over its forward-slash absolute spelling.
    key = hashlib.md5(build.resolve().as_posix().encode('utf-8')).hexdigest()[:12]
    return root / 'tmp/spark-editor-fps-installed-lineage' / key / CONFIG


def immutable(record):
    for name, expected in record.get('images', {}).items():
        if digest(Path(name)) != expected:
            raise ValueError('Built qualification input changed: ' + name)


class Runner:
    def __init__(self, args, life):
        self.args, self.life = args, life
        self.source, self.root = args.source.resolve(), args.root.resolve()
        self.build = self.source / 'build/windows-shipping'
        self.expected = pins(args, os.environ)
        total = read_json(self.root / 'clock.json')['deadline']
        now = time.monotonic()
        if type(total) not in (int, float) or not math.isfinite(total) or total > now + CAPS['jobSeconds']:
            raise ValueError('Invalid shared job deadline')
        self.deadline = min(now + PHASES[args.phase], total - RESERVE[args.phase])
        self.commands = []

    def run(self, name, command, cap):
        command = [str(v) for v in command]
        record = {'name': name, 'argv': command, 'cwd': str(self.source), 'ceilingSeconds': cap}
        self.commands.append(record)
        try:
            with (self.root / (name + '.log')).open('wb') as output:
                result = self.life.owned_run(command, cwd=self.source, stdout=output, stderr=subprocess.STDOUT,
                    timeout=self.life.remaining(self.deadline, cap), check=True)
                record['exitCode'] = result.returncode
                return result.returncode
        except Exception as error:
            record['error'] = str(error)
            raise
        finally:
            save(self.root / (self.args.phase + '-commands.json'), self.commands)

    def git(self, *args):
        self.run('git-query', ['git', '-C', self.source, *args], 30)
        return (self.root / 'git-query.log').read_text(encoding='utf-8').strip()

    def exact_source(self):
        if self.git('rev-parse', 'HEAD') != self.args.source_sha or self.git('status', '--porcelain', '--untracked-files=no'):
            raise ValueError('Product source is not exact and clean')

    def identity(self):
        record = read_json(self.root / 'identity.json')
        if any(record.get(k) != v for k, v in self.expected.items()):
            raise ValueError('Phase identity mismatch')
        if record['helper_sha256'] != digest(Path(__file__)):
            raise ValueError('Workflow helper changed between phases')
        for name, expected in record['source_files'].items():
            if digest(self.source / name) != expected:
                raise ValueError('Trusted source parser/producer changed')
        for tool in record['tools'].values():
            if digest(tool['path'])!=tool['sha256']:
                raise ValueError('Pinned tool changed between phases')
        immutable(record)
        return record

    def configure(self):
        self.exact_source()
        modules = self.git('submodule', 'status', '--recursive')
        if any(line.startswith(('-', '+', 'U')) for line in modules.splitlines()):
            raise ValueError('Submodule mismatch')
        tools = {}
        for name in ('cl', 'rc', 'fxc', 'cmake', 'ninja'):
            path = shutil.which(name)
            if not path:
                raise ValueError('Missing preinstalled prerequisite: ' + name)
            tools[name] = {'path': path, 'sha256': digest(path)}
        tools['python'] = {'path': sys.executable, 'sha256': digest(sys.executable)}
        record = dict(self.expected, source_tree=self.git('rev-parse', 'HEAD^{tree}'), submodules=modules,
            configuration=CONFIG, tools=tools, helper_sha256=digest(Path(__file__)),
            source_files={name: digest(self.source / name) for name in SOURCE_FILES},
            scope='editor authored/cooked scene consumed by installed real FPS; hosted WARP only', caps=CAPS)
        save(self.root / 'identity.json', record)
        self.run('configure', configure_command(self.source, self.build, self.args.source_sha), 850)
        save(self.root / 'shipping-cache.json', cache_contract(self.source, self.build, self.args.source_sha))
        self.run('prebuild-discovery', ['ctest', '--test-dir', self.build, '-C', CONFIG, '-R', '^'+CTEST+'$', '--show-only=json-v1'], 30)
        document = read_json(self.root / 'prebuild-discovery.log')
        discovery(document)
        save(self.root / 'prebuild-discovery.json', document)

    def build_phase(self):
        self.exact_source()
        record = self.identity()
        self.run('build', ['cmake', '--build', self.build, '--config', CONFIG, '--parallel', '2', '--target', *TARGETS], 7070)
        self.run('discovery', ['ctest', '--test-dir', self.build, '-C', CONFIG, '-R', '^'+CTEST+'$', '--show-only=json-v1'], 30)
        document = read_json(self.root / 'discovery.log')
        if discovery(document, True, self.source, self.build, self.args.source_sha) != discovery(read_json(self.root / 'prebuild-discovery.json')):
            raise ValueError('Registration policy changed during build')
        save(self.root / 'discovery.json', document)
        binary = self.build / 'bin' / CONFIG
        names = ('SparkEngine.exe', 'SparkGameFPS.dll', 'SparkGameFPS.dll.sparkabi', 'SparkTests.exe', 'SparkCooker.exe', 'SparkCrashReporter.exe')
        record['images'] = {str(binary / name): digest(binary / name) for name in names}
        save(self.root / 'identity.json', record)

    def test(self):
        self.exact_source()
        record = self.identity()
        if not record.get('images'):
            raise ValueError('Missing built identities')
        names = framework_names(self.source)
        save(self.root / 'reflected-cases.json', {'source': 'Tests/TestSceneManagerReflectedReal.cpp', 'prefix': PREFIX, 'names': names})
        temporary = self.root / 'tmp'
        temporary.mkdir(exist_ok=False)
        (temporary/'localappdata').mkdir()
        (temporary/'appdata').mkdir()
        env = ['cmake', '-E', 'env', '--unset=SPARK_TEST_FILE', '--unset=SPARK_TEST_NAME',
            '--unset=SPARK_TEST_EXCLUDE', '--unset=SPARK_TEST_LIMIT', 'TEMP='+str(temporary), 'TMP='+str(temporary),
            'LOCALAPPDATA='+str(temporary/'localappdata'), 'APPDATA='+str(temporary/'appdata')]
        self.run('reflected', [*env, 'SPARK_TEST_NAME_PREFIX='+PREFIX, 'SPARK_TEST_EXPECT_COUNT='+str(len(names)),
            self.build / 'bin' / CONFIG / 'SparkTests.exe', '--warn-is-error', '--empty-is-error',
            '--junit-xml', self.root / 'reflected-junit.xml', '--output-file', self.root / 'reflected-output.log'], 180)
        stats = validate_xml(self.root / 'reflected-junit.xml', names)
        immutable(record)
        self.run('ctest', [*env, '--unset=SPARK_TEST_NAME_PREFIX', '--unset=SPARK_TEST_EXPECT_COUNT',
            'ctest', '--test-dir', self.build, '-C', CONFIG, '-R', '^'+CTEST+'$', '--parallel', '1',
            '--output-on-failure', '--no-tests=error', '--output-junit', self.root / 'ctest-junit.xml'], 910)
        ctest = validate_xml(self.root / 'ctest-junit.xml', [CTEST], ctest=True)
        staged = scratch_root(self.root, self.build)
        if not (staged / '.spark-editor-fps-installed-lineage').is_file():
            raise ValueError('Owned installed evidence root missing')
        report = read_json(staged / 'lineage-validation.json')
        # The exact-source wrapper invokes both strict native lifecycle and file/state validators.
        # Preserve its full report and case evidence; do not reinterpret partial output as success.
        if report.get('sourceSha')!=self.args.source_sha or report.get('configuration')!=CONFIG:
            raise ValueError('Lineage report source/configuration mismatch')
        binary = self.build/'bin'/CONFIG
        for field, name in [('installedHost', 'SparkEngine.exe'), ('installedModule', 'SparkGameFPS.dll'),
                            ('installedSidecar', 'SparkGameFPS.dll.sparkabi')]:
            if report[field]['sha256']!=record['images'][str(binary/name)]:
                raise ValueError('Built/installed image binding mismatch: '+field)
        if report['frameworkJUnit']['sha256']!=digest(staged/'lineage-junit.xml'):
            raise ValueError('Framework JUnit binding mismatch')
        if set(report['cases'])!=set(CASES):
            raise ValueError('Lineage report case set mismatch')
        immutable(record)
        save(self.root / 'terminal.json', dict(self.expected, passed=True, reflected=stats, ctest=ctest,
            staged_root=str(staged), lineage_report_sha256=digest(staged / 'lineage-validation.json'),
            images=record['images'], full_suite_qualified=False, physical_shipping_qualified=False))


def collect(root, build, expected=None):
    payloads, manifest = {}, []
    def retain(path, name, cap, *, tail=False, required=True):
        if not path.is_file():
            if required:
                raise ValueError('Missing proof: '+name)
            return
        for part in (path, *path.parents):
            if part.is_symlink() or (hasattr(part, 'is_junction') and part.is_junction()):
                raise ValueError('Linked proof path is not allowed')
        size = path.stat().st_size
        if not tail and size > cap:
            raise ValueError('Complete proof exceeds cap: '+name)
        with path.open('rb') as stream:
            if tail:
                stream.seek(max(0, size-cap))
            data = stream.read(cap+1)
        if len(data)>cap or b'\0' in data:
            raise ValueError('Oversize or NUL text proof')
        original_data = data
        data = data.decode('utf-8', errors='replace' if tail else 'strict').encode('utf-8')
        if tail and len(data)>cap:
            data = data[-cap:].decode('utf-8', errors='ignore').encode('utf-8')
        if not tail and not data:
            raise ValueError('Empty proof: '+name)
        if not tail and path.suffix == '.json':
            read_json(path)
        payloads[name] = data
        manifest.append({'path': name, 'bytes': size, 'sha256': digest(path),
            'retainedBytes': len(data), 'retainedSha256': hashlib.sha256(data).hexdigest(),
            'tail_only': tail and (size > cap or data!=original_data),
            'diagnostic_reencoded': tail and data!=original_data})
    terminal = read_json(root / 'terminal.json') if (root / 'terminal.json').is_file() else {}
    success = terminal.get('passed') is True
    if success and (expected is None or any(terminal.get(k)!=v for k,v in expected.items())):
        raise ValueError('Terminal proof belongs to a different source/workflow/run')
    if success and any((root/(phase+'-failure.json')).exists() for phase in PHASES):
        raise ValueError('Success terminal contradicts phase failure')
    for name in ('clock.json', 'host.json', 'identity.json', 'shipping-cache.json', 'prebuild-discovery.json',
                 'discovery.json', 'reflected-cases.json', 'terminal.json'):
        retain(root/name, name, MIB, required=success)
    for phase in PHASES:
        retain(root/(phase+'-commands.json'), phase+'-commands.json', MIB, required=success and phase!='pack')
        retain(root/(phase+'-failure.json'), phase+'-failure.json', MIB, required=False)
    for name in ('reflected-junit.xml', 'ctest-junit.xml'):
        retain(root/name, name, 4*MIB, required=success)
    retain(root/'reflected-output.log', 'reflected-output.log', 128*1024, required=success)
    staged = scratch_root(root, build)
    for name, cap in [('lineage-validation.json', MIB), ('lineage-junit.xml', 4*MIB), ('lineage-output.log', 128*1024)]:
        retain(staged/name, 'installed/'+name, cap, required=success)
    case_proof = ('runtime.log', 'exit-code.txt', 'reopened.sparkscene',
        'CrateProject/Scenes/Default.sparkscene', 'CrateProject/Cooked/Scenes/Default.sparkscene',
        'CrateProject/CrateProject.sparkproject', 'CrateProject/Cooked/CrateProject.sparkproject',
        'CrateProject/Cooked/Assets/spark-cook-manifest.json', 'Package/Startup.sparkscene',
        'Package/Scenes/Startup.sparkscene', 'Package/spark.modules.json', 'Package/Assets/spark-cook-manifest.json')
    for case in CASES:
        for name in case_proof:
            retain(staged/'evidence'/case/name, 'installed/evidence/'+case+'/'+name,
                   128*1024 if name=='runtime.log' else MIB, required=success)
    for name in ('configure.log', 'build.log', 'graphics.log', 'reflected.log', 'ctest.log'):
        retain(root/name, 'diagnostics/'+name, 16384, tail=True, required=False)
    for name in ('install-runtime.log', 'install-samples.log', 'install-redist.log', 'sparktests.log', 'lineage-validation-error.log'):
        retain(staged/name, 'diagnostics/installed/'+name, 16384, tail=True, required=False)
    payloads['manifest.json'] = (json.dumps(manifest, indent=2)+'\n').encode()
    payloads['collection.json'] = (json.dumps({'caps': CAPS, 'completeSuccessProof': success,
        'binaryHashBasis': 'runner local file attestation; raw images are not retained',
        'scope': 'MinSizeRel editor-FPS slice only', 'identity': expected}, indent=2)+'\n').encode()
    if sum(map(len, payloads.values())) > CAPS['expandedBytes']:
        raise ValueError('Text archive expanded cap exceeded')
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, data in sorted(payloads.items()):
            archive.writestr(name, data)
    # Reserve wrapper/header variation in upload-artifact's re-created ZIP.
    if len(stream.getvalue()) > MIB-16384:
        raise ValueError('ZIP cap including artifact wrapper reserve exceeded')
    frozen = root/'qualification-text'
    frozen.mkdir(exist_ok=False)
    for name, data in payloads.items():
        target = frozen/name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    save(root/'archive.json', {'preflightZipBytes': len(stream.getvalue()),
        'expandedBytes': sum(map(len, payloads.values())), 'caps': CAPS, 'members': len(payloads)})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('phase', choices=tuple(PHASES))
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    for name in ('source-sha', 'workflow-sha', 'run-id', 'run-attempt'):
        parser.add_argument('--'+name, required=True)
    args = parser.parse_args()
    source, root = args.source.resolve(strict=True), args.root.resolve(strict=True)
    temp = Path(os.environ['RUNNER_TEMP']).resolve(strict=True)
    if (root==temp or not root.is_relative_to(temp) or source.is_relative_to(root) or root.is_relative_to(source)
            or not str(root).isascii() or len(str(root))>120):
        raise ValueError('Owned short ASCII temporary root must be disjoint from source')
    args.source, args.root = source, root
    pins(args, os.environ)
    try:
        runner = Runner(args, adapter(source))
        if args.phase=='pack':
            runner.life.remaining(runner.deadline, PHASES['pack'])
            if (root/'terminal.json').is_file() and read_json(root/'terminal.json').get('passed') is True:
                runner.exact_source()
                runner.identity()
            collect(root, runner.build, runner.expected)
            runner.life.remaining(runner.deadline, PHASES['pack'])
        else:
            getattr(runner, 'build_phase' if args.phase=='build' else args.phase)()
    except Exception as error:
        save(root/(args.phase+'-failure.json'), {'phase': args.phase, 'error': str(error), 'passed': False})
        raise

if __name__ == '__main__':
    main()
