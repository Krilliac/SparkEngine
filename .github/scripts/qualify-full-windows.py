"""One-build Windows Release diagnostic. Source/workflow revisions are separate."""
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
import zipfile

MIB = 1024 * 1024
REQUIRED = {'SparkEngineTests', 'SparkEngineLoadTests', 'SparkSaveInterruptionTests',
            'CpuFloor_IsaBaseline', 'CpuFloor_BelowFloorStartupRefused',
            'ModuleProfileLifecycle_SparkGameFPS_D3D11'}
BUILD_PROOF = ('SparkTests-junit.xml', 'SparkTests-load-junit.xml',
               'SparkTests-output.log', 'SparkTests-load-output.log',
               'test-registration-count.json')
ATOMIC_PROOF = ('SparkTests-atomicwrite-output.log', 'SparkTests-atomicwrite-junit.xml')
BUILD_DIAGNOSTIC = ('SparkTests.log', 'SparkTests-load.log')


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def save(path, obj):
    path.write_text(json.dumps(obj, indent=2) + '\n', encoding='utf-8')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inventory(document):
    tests = document['tests']
    result = {}
    for test in tests:
        name = test['name']
        if name in result:
            raise ValueError('Duplicate discovered test: ' + name)
        result[name] = {p['name']: p['value'] for p in test.get('properties', [])}
    if not REQUIRED <= result.keys():
        raise ValueError('Missing mandatory tests: ' + str(sorted(REQUIRED - result.keys())))
    if any(result[n].get('DISABLED', False) for n in REQUIRED):
        raise ValueError('Mandatory test disabled')
    if result['CpuFloor_IsaBaseline'].get('TIMEOUT') != 600 or result['CpuFloor_BelowFloorStartupRefused'].get('TIMEOUT') != 240:
        raise ValueError('CPU-floor test bounds changed')
    return result


def validate_postbuild(before, after):
    """Commands may resolve after building; registration policy must not change."""
    expected, actual = inventory(before), inventory(after)
    if expected.keys() != actual.keys():
        raise ValueError('Postbuild registration names changed')
    for name in expected:
        if expected[name] != actual[name]:
            raise ValueError('Postbuild registration properties changed: ' + name)
    for test in after['tests']:
        if not actual[test['name']].get('DISABLED', False):
            command = test.get('command')
            if (not isinstance(command, list) or not command
                    or any(not isinstance(part, str) or not part.strip() for part in command)):
                raise ValueError('Postbuild enabled command unresolved: ' + test['name'])
    return actual


def account(discovered, xml):
    expected = inventory(discovered)
    cases = ET.fromstring(xml).findall('.//testcase')
    by_name = {}
    for case in cases:
        name = case.get('name')
        if name in by_name:
            raise ValueError('Duplicate CTest result: ' + str(name))
        by_name[name] = case
    if expected.keys() != by_name.keys():
        raise ValueError('Incomplete CTest accounting: ' + json.dumps({
            'missing': sorted(expected.keys() - by_name.keys()),
            'unexpected': sorted(by_name.keys() - expected.keys())}))
    failed = []
    disabled = []
    declared_skips = []
    for name, props in expected.items():
        case = by_name[name]
        skipped = case.find('skipped') is not None
        if props.get('DISABLED', False):
            if (case.get('status') != 'disabled' or skipped
                    or case.find('failure') is not None or case.find('error') is not None):
                raise ValueError('Disabled registration has contradictory result: ' + name)
            disabled.append(name)
        elif case.find('failure') is not None or case.find('error') is not None:
            failed.append(name)
        elif skipped:
            if (case.get('status') != 'notrun'
                    or ('SKIP_RETURN_CODE' not in props and 'SKIP_REGULAR_EXPRESSION' not in props)):
                failed.append(name)
            else:
                declared_skips.append(name)
        elif case.get('status') not in (None, 'run'):
            failed.append(name)
    return {'complete': True, 'expected': len(expected), 'results': len(by_name),
            'failed': failed, 'disabled': disabled, 'declared_skips': declared_skips}


def bundle(payloads):
    if sum(len(v) for v in payloads.values()) > 12 * MIB:
        raise ValueError('Complete text evidence exceeds 12 MiB')
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, value in sorted(payloads.items()):
            archive.writestr(name, value)
    if len(stream.getvalue()) > MIB:
        raise ValueError('Evidence ZIP exceeds 1 MiB')
    return stream.getvalue()


def adapter(source):
    spec = importlib.util.spec_from_file_location('installed_lifetime', source / '.github/scripts/qualify-installed-native.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('phase', choices=('sde', 'graphics', 'configure', 'build', 'test', 'pack'))
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--source-sha', required=True)
    args = parser.parse_args()
    source, root = args.source.resolve(), args.root.resolve()
    temp = Path(os.environ['RUNNER_TEMP']).resolve()
    if root == temp or not root.is_relative_to(temp) or root.is_relative_to(source) or source.is_relative_to(root):
        raise ValueError('Evidence root must be an owned temporary child disjoint from source')
    root.mkdir(exist_ok=True)
    life = adapter(source)
    build = source / 'build'
    identity = root / 'identity.json'
    deadline = time.monotonic() + {'sde': 280, 'graphics': 580, 'configure': 870, 'build': 7170, 'test': 4770, 'pack': 570}[args.phase]
    # The workflow initializes this before prerequisites: all work shares the same clock.
    total_deadline = read_json(root / 'clock.json')['deadline']
    reserve = {'sde': 225 * 60, 'graphics': 225 * 60, 'configure': 210 * 60, 'build': 90 * 60, 'test': 10 * 60, 'pack': 30}[args.phase]
    deadline = min(deadline, total_deadline - reserve)

    def run(name, command, cap, check=True):
        with (root / (name + '.log')).open('wb') as output:
            return life.owned_run([str(x) for x in command], cwd=source, stdout=output,
                stderr=subprocess.STDOUT, timeout=life.remaining(deadline, cap), check=check).returncode

    def git(*command):
        run('git-query', ['git', '-C', source, *command], 30)
        return (root / 'git-query.log').read_text(encoding='utf-8').strip()

    def immutable(record):
        for name, value in record['images'].items():
            if digest(Path(name)) != value:
                raise ValueError('Qualification binary changed: ' + name)

    try:
        if args.phase in ('sde', 'graphics'):
            run(args.phase, ['pwsh', '-NoProfile', '-File', Path(__file__).with_name('full-windows-' + args.phase + '.ps1')],
                280 if args.phase == 'sde' else 580)
        elif args.phase == 'configure':
            if git('rev-parse', 'HEAD') != args.source_sha or git('status', '--porcelain', '--untracked-files=no'):
                raise ValueError('Expected clean exact product source')
            modules = git('submodule', 'status', '--recursive')
            if any(row.startswith(('-', '+', 'U')) for row in modules.splitlines()):
                raise ValueError('Submodule mismatch')
            tools = {}
            for name in ('cl', 'rc', 'fxc', 'cmake', 'ninja', 'llvm-objdump', 'llvm-pdbutil'):
                path = shutil.which(name)
                if not path:
                    raise ValueError('Missing prerequisite: ' + name)
                tools[name] = {'path': path, 'sha256': digest(Path(path))}
            sde = Path(os.environ['SPARK_SDE_EXECUTABLE'])
            if not sde.is_file():
                raise ValueError('Missing pinned SDE executable')
            save(identity, {'source_sha': args.source_sha, 'source_tree': git('rev-parse', 'HEAD^{tree}'),
                'submodules': modules, 'workflow_sha': os.environ['GITHUB_SHA'],
                'run_id': os.environ['GITHUB_RUN_ID'], 'run_attempt': os.environ['GITHUB_RUN_ATTEMPT'],
                'helper_sha256': digest(Path(__file__)),
                'lifetime_helper_sha256': digest(source / '.github/scripts/qualify-installed-native.py'), 'tools': tools,
                'sde_sha256': digest(sde), 'configuration': 'Release'})
            for script in ('test_verify_windows_package_signatures.py', 'test_write_shipping_package_manifest.py',
                           'test_provision_previous_windows_msi.py', 'test_qualify_windows_msi.py'):
                run(script[:-3], [sys.executable, source / '.github/scripts' / script], 180)
            run('configure', ['cmake', '--fresh', '-B', build, '-G', 'Ninja Multi-Config',
                '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
                '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded', '-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON',
                '-DCMAKE_CXX_SCAN_FOR_MODULES=OFF', '-DGENERATE_DEBUG_SYMBOLS=OFF',
                '-DSPARK_NATIVE_ARCH=OFF', '-DBUILD_TESTS=ON', '-DBUILD_GAME_MODULES=ON',
                '-DSPARK_SDE_EXECUTABLE=' + str(sde)], 850)
            run('prebuild-discovery', ['ctest', '--test-dir', build, '-C', 'Release', '--no-tests=error', '--show-only=json-v1'], 30)
            discovered = read_json(root / 'prebuild-discovery.log')
            inventory(discovered)
            save(root / 'prebuild-discovery.json', discovered)
            registration = next(t for t in discovered['tests'] if t['name'] == 'ReleaseApproval_Record')
            python = registration['command'][0]
            run('test-python-dependencies', [python, '-m', 'pip', 'install', '--disable-pip-version-check',
                '--require-hashes', '--only-binary=:all:', '--no-deps', '-r', source / '.github/ci-python-requirements.txt'], 120)
        elif args.phase == 'build':
            run('build', ['cmake', '--build', build, '--config', 'Release', '--parallel'], 7170)
            run('discovery', ['ctest', '--test-dir', build, '-C', 'Release', '--no-tests=error', '--show-only=json-v1'], 30)
            discovered = read_json(root / 'discovery.log')
            validate_postbuild(read_json(root / 'prebuild-discovery.json'), discovered)
            save(root / 'discovery.json', discovered)
            run('ctest-policy', [sys.executable, source / 'Tools/validate_ctest_policy.py',
                '--ctest-json', root / 'discovery.json', '--build-dir', build], 120)
            record = read_json(identity)
            images = sorted((build / 'bin/Release').glob('*.exe')) + sorted((build / 'bin/Release').glob('*.dll'))
            for name in ('SparkTests.exe', 'SparkEngine.exe'):
                if not (build / 'bin/Release' / name).is_file():
                    raise ValueError('Missing required image: ' + name)
            record['images'] = {str(p): digest(p) for p in images}
            save(identity, record)
        elif args.phase == 'test':
            record = read_json(identity)
            immutable(record)
            errors = []
            # Reserve four 120-second Primary invocations and source-census gates.
            suite_cap = life.remaining(deadline, 4770) - 770
            if suite_cap <= 0:
                raise TimeoutError('No remaining full-suite budget')
            try:
                code = run('ctest', ['ctest', '--test-dir', build, '-C', 'Release', '--parallel', '2',
                    '--output-on-failure', '--no-tests=error', '--output-junit', root / 'ctest-junit.xml'], suite_cap, False)
                if code:
                    errors.append('CTest exit ' + str(code))
            except Exception as error:
                errors.append('CTest: ' + str(error))
            try:
                junit = root / 'ctest-junit.xml'
                if junit.stat().st_size > 4 * MIB:
                    raise ValueError('Complete CTest JUnit exceeds 4 MiB')
                report = account(read_json(root / 'discovery.json'), junit.read_bytes())
                save(root / 'ctest-accounting.json', report)
                if report['failed']:
                    errors.append('CTest failures: ' + str(report['failed']))
                tree = ET.parse(root / 'ctest-junit.xml')
                cpu = ET.Element('testsuite', name='CPU-floor exact CTest results')
                for case in tree.getroot().iter('testcase'):
                    if case.get('name', '').startswith('CpuFloor_'):
                        cpu.append(case)
                ET.ElementTree(cpu).write(root / 'cpu-floor.xml', encoding='utf-8', xml_declaration=True)
                run('summary', [sys.executable, source / '.github/scripts/summarize-test-results.py',
                    build / 'SparkTests-junit.xml', build / 'SparkTests-load-junit.xml', '--min-tests', '6800',
                    '--registration-count', build / 'test-registration-count.json', '--json', root / 'test-stats.json'], 30)
            except Exception as error:
                errors.append('Accounting/summary: ' + str(error))
            try:
                minimum = read_json(source / '.github/test-count-ratchet.json')['minimumProductionSourceTests']
                run('source-census', [sys.executable, source / 'Tools/test_source_census.py', '--check',
                    '--json', root / 'test-source-census.json', '--minimum-production-tests', minimum], 120)
                run('profile-census', [sys.executable, source / 'Tools/test_source_census.py',
                    '--profile-selectors', root / 'discovery.json'], 120)
            except Exception as error:
                errors.append('Production/profile census: ' + str(error))
            immutable(record)
            try:
                run('primary', [sys.executable, source / '.github/scripts/verify-approved-primary.py',
                    '--exe', build / 'bin/Release/SparkTests.exe', '--output', root / 'primary'], 500)
            except Exception as error:
                errors.append('Primary: ' + str(error))
            immutable(record)
            for name in (*BUILD_PROOF, *ATOMIC_PROOF, *BUILD_DIAGNOSTIC):
                if not (build / name).is_file() or (build / name).stat().st_size == 0:
                    errors.append('Missing or empty required native file: ' + name)
            save(root / 'terminal.json', {'passed': not errors, 'errors': errors,
                 'spark_tests_sha256': record['images'][str(build / 'bin/Release/SparkTests.exe')]})
            if errors:
                raise ValueError('; '.join(errors))
        else:
            payloads, manifest = {}, []
            def retain(path, name, cap, tail=False, strict_utf8=False):
                if not path.is_file():
                    return
                size = path.stat().st_size
                if size > cap and not tail:
                    raise ValueError('Complete proof exceeds file cap: ' + name)
                with path.open('rb') as stream:
                    if tail:
                        stream.seek(max(0, size - cap))
                    data = stream.read(cap + 1)
                if strict_utf8:
                    if not data or b'\0' in data:
                        raise ValueError('Empty or NUL-containing complete proof: ' + name)
                    data.decode('utf-8')  # Complete framework proof must not be rewritten.
                payloads[name] = data
                manifest.append({'path': name, 'bytes': size, 'sha256': digest(path), 'tail_only': tail and size > cap})
            for path in root.glob('*.json'):
                retain(path, path.name, MIB)
            for path in root.glob('*.xml'):
                retain(path, path.name, 4 * MIB)
            for path in root.glob('*.log'):
                retain(path, 'diagnostics/' + path.name, 16384, True)
            for name in BUILD_PROOF:
                retain(build / name, 'build/' + name, 6 * MIB)
            for name in ATOMIC_PROOF:
                retain(build / name, 'build/' + name, 128 * 1024, strict_utf8=True)
            # RunSparkTests keeps framework assertions in output-file/JUnit; raw
            # stdout/stderr logger/crash streams are diagnostic tails, explicitly marked.
            for name in BUILD_DIAGNOSTIC:
                retain(build / name, 'build/' + name, 16384, True)
            for path in (root / 'primary').rglob('*'):
                if path.suffix in ('.json', '.xml', '.log'):
                    retain(path, path.relative_to(root).as_posix(), MIB)
            if (root / 'terminal.json').exists() and read_json(root / 'terminal.json').get('passed'):
                required = {'build/' + n for n in (*BUILD_PROOF, *ATOMIC_PROOF, *BUILD_DIAGNOSTIC)} | {'ctest-junit.xml', 'cpu-floor.xml',
                    'ctest-accounting.json', 'test-stats.json', 'test-source-census.json', 'primary/results.json',
                    'host.json', 'identity.json', 'prebuild-discovery.json', 'discovery.json'}
                if not required <= payloads.keys():
                    raise ValueError('Missing complete proof: ' + str(required - payloads.keys()))
            payloads['manifest.json'] = json.dumps(manifest, indent=2).encode()
            data = bundle(payloads)
            (root / 'qualification-text.zip').write_bytes(data)
            frozen = root / 'qualification-text'
            frozen.mkdir(exist_ok=False)
            for name, value in payloads.items():
                dest = frozen / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(value)
            save(root / 'archive.json', {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    except Exception as error:
        save(root / (args.phase + '-failure.json'), {'phase': args.phase, 'error': str(error)})
        raise

if __name__ == '__main__':
    main()
