"""Content-only decoder caller for the owned F2/F3 driver; never opens user saves.

The driver supplies run_child, its existing process-owner callback. No implicit
subprocess fallback exists. The callback must terminate/close its owned process
tree before returning and include cleanup in the supplied timeout.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import tempfile
import time
import xml.etree.ElementTree as ET

CASE = 'FPSInputDispatch_DecodeOwnedPrimary'
PREFIX = 'SPARK_FPS_SAVE_DECODE '
INPUT_CAP = 16 * 1024 * 1024
TEXT_CAP = 128 * 1024


def require(value, reason):
    if not value:
        raise ValueError(reason)


def plain(path):
    path = Path(path)
    require(path.is_absolute(), 'absolute owned path required')
    require('..' not in path.parts and str(path).isascii(), 'plain ASCII path required')
    for item in (path, *path.parents):
        info = item.lstat()
        require(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                'link/reparse path refused')
    return path.resolve(strict=True)


def file_identity(path, cap):
    path = plain(path)
    info = path.stat()
    require(stat.S_ISREG(info.st_mode) and 0 < info.st_size <= cap, 'regular bounded file required')
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    after = path.stat()
    require((info.st_size, info.st_mtime_ns, info.st_ctime_ns) ==
            (after.st_size, after.st_mtime_ns, after.st_ctime_ns), 'file changed while hashing')
    return {'sha256': digest, 'bytes': info.st_size}


def unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'duplicate receipt key')
        result[key] = value
    return result


def parse_receipt(stdout, stderr, expected):
    for text in (stdout, stderr):
        require(isinstance(text, str) and '\0' not in text and len(text.encode('utf-8')) <= TEXT_CAP,
                'complete decoder output cap/encoding')
    lines = [line for text in (stdout, stderr) for line in text.splitlines()
             if 'SPARK_FPS_SAVE_DECODE' in line]
    require(len(lines) == 1 and lines[0].startswith(PREFIX), 'one standalone decoder receipt required')
    raw = lines[0][len(PREFIX):]
    require(0 < len(raw.encode('utf-8')) <= 16384, 'decoder JSON cap')
    receipt = json.loads(raw, object_pairs_hook=unique,
                         parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    require(set(receipt) == {'inputSha256', 'inputBytes', 'canonicalProfile'}, 'decoder receipt shape')
    require(type(receipt['inputBytes']) is int and receipt['inputBytes'] == expected['bytes'] and
            receipt['inputSha256'] == expected['sha256'], 'decoded primary identity mismatch')
    profile = receipt['canonicalProfile']
    # This is the production WriteTo map. Its schema validation belongs to the
    # native ReadFrom path; do not implement another persisted-profile parser.
    require(isinstance(profile, dict) and profile and
            all(isinstance(k, str) and isinstance(v, str) and '\n' not in k+v and '\r' not in k+v and
                k.startswith('fps.profile.') for k, v in profile.items()), 'canonical profile map')
    return receipt


def framework(path):
    require(plain(path).stat().st_size <= TEXT_CAP, 'framework XML cap')
    raw = path.read_bytes()
    require(raw and b'\0' not in raw and b'<!DOCTYPE' not in raw, 'framework XML encoding')
    root = ET.fromstring(raw.decode('utf-8'))
    require(root.tag == 'testsuites', 'framework root')
    for node in [root, *root.findall('testsuite')]:
        require(node.get('tests') == '1' and all(node.get(key, '0') == '0'
                for key in ('failures', 'errors', 'skipped', 'flaky', 'empty')), 'framework totals')
    cases = root.findall('.//testcase')
    require(len(root.findall('testsuite')) == 1 and len(cases) == 1 and cases[0].get('name') == CASE,
            'exact decoder framework case')
    require(not any(cases[0].find(tag) is not None for tag in ('failure', 'error', 'skipped')),
            'decoder case did not pass')
    duration = float(cases[0].get('time', 'nan'))
    require(math.isfinite(duration) and duration >= 0, 'decoder duration')


def decode_owned_primary(*, input_path, owned_root, tests_executable, deadline, run_child):
    require(callable(run_child), 'owned runner callback required')
    root, original, executable = plain(owned_root), plain(input_path), plain(tests_executable)
    require(root.is_dir() and original.is_relative_to(root) and original != root,
            'input must be inside explicit owned evidence root')
    require(not root.is_relative_to(executable) and original != executable, 'input/executable separation')
    for suffix in ('.bak', '.tmp'):
        require(not original.with_name(original.name+suffix).exists(), 'copied input backup/staging refused')
    before = file_identity(original, INPUT_CAP)
    executable_before = file_identity(executable, 1024*1024*1024)
    work = Path(tempfile.mkdtemp(prefix='save-decode-', dir=root))
    inputs = work/'input'
    inputs.mkdir()
    primary = inputs/'fps_quicksave.spark_save'
    with original.open('rb') as source, primary.open('xb') as target:
        remaining = before['bytes']
        while remaining:
            block = source.read(min(65536, remaining))
            require(block, 'short input copy')
            target.write(block)
            remaining -= len(block)
        require(not source.read(1), 'input grew during copy')
    require(file_identity(original, INPUT_CAP) == before and file_identity(primary, INPUT_CAP) == before,
            'input copy changed')
    timeout = min(20.0, deadline-time.monotonic())
    require(timeout > 0, 'owner deadline exhausted before decoder')
    environment = dict(os.environ)
    for selector in ('SPARK_TEST_NAME', 'SPARK_TEST_FILE', 'SPARK_TEST_EXCLUDE'):
        environment.pop(selector, None)
    environment.update(SPARK_FPS_DECODE_DIRECTORY=str(inputs), SPARK_TEST_NAME_PREFIX=CASE,
                       SPARK_TEST_EXPECT_COUNT='1')
    junit = work/'framework.xml'
    result = run_child([str(executable), '--warn-is-error', '--empty-is-error', '--junit-xml', str(junit)],
                       cwd=work, environment=environment, timeout=timeout)
    for name, text in (('stdout.log', result.stdout), ('stderr.log', result.stderr)):
        require(isinstance(text, str) and '\0' not in text and len(text.encode('utf-8')) <= TEXT_CAP,
                'complete decoder output cap/encoding')
        with (work/name).open('x', encoding='utf-8', newline='') as output:
            output.write(text)
    require(time.monotonic() <= deadline, 'owner deadline exceeded')
    require(result.returncode == 0, 'native decoder failed')
    receipt = parse_receipt(result.stdout, result.stderr, before)
    framework(junit)
    require({p.name for p in inputs.iterdir()} == {'fps_quicksave.spark_save'}, 'decoder backup/file substitution')
    require(file_identity(original, INPUT_CAP) == before and file_identity(primary, INPUT_CAP) == before and
            file_identity(executable, 1024*1024*1024) == executable_before, 'decoder/input identity changed')
    report = {'scope': 'copied-primary-content-only', 'input': before, 'decoderExecutable': executable_before,
              'canonicalProfile': receipt['canonicalProfile'], 'frameworkJUnit': file_identity(junit, TEXT_CAP)}
    require(time.monotonic() <= deadline, 'owner deadline exceeded during final verification')
    with (work/'content-receipt.json').open('x', encoding='utf-8') as output:
        output.write(json.dumps(report, sort_keys=True)+'\n')
    return work, report
