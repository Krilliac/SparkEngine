"""Target-only ISA diagnosis; never a substitute for full CPU qualification."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

SOURCE = 'd90b86e42fd586a56d53f077bbca6a457200d9bd'
ORIGINAL_IMAGE = 'a78b3d3c6563a15a92af289e9da67e795a884e56098fe08f92be76b31fbfab3b'
TARGET = 'SparkGameMMOFPS'
MIB = 1024 * 1024


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_command(build):
    return ['cmake', '--build', str(build), '--config', 'Release', '--target', TARGET, '--parallel']


def validate_reference(identity, reference):
    for field in ('source_sha', 'source_tree'):
        if identity[field] != reference[field]:
            raise ValueError('Original measurement identity mismatch: ' + field)
    if reference['image_sha256'] != ORIGINAL_IMAGE:
        raise ValueError('Original image identity mismatch')
    for name in ('cl', 'rc', 'fxc', 'cmake', 'ninja', 'llvm-objdump', 'llvm-pdbutil'):
        if identity['tools'][name]['sha256'] != reference['tools'][name]['sha256']:
            raise ValueError('Original measurement tool mismatch: ' + name)


def terminal(code, report):
    scan = report.get('scanExitCode')
    complete = report.get('diagnosticComplete') is True
    # A collector success can never erase the strict scanner's failure.
    consistent = type(scan) is int and scan in (0, 1) and code == scan and complete
    return {'scanExitCode': scan, 'collectorExitCode': code,
            'diagnosticComplete': complete, 'consistent': consistent,
            'passed': consistent and scan == 0,
            'scope': 'target-only new measurement; not full CPU-floor qualification'}


def payloads(root, digest):
    result, manifest = {}, []
    # Explicit text extensions only; no DLL/PDB or other binary is packaged.
    for path in sorted(root.iterdir()):
        if path.suffix not in ('.json', '.log') or path.name == 'archive.json':
            continue
        if path.is_symlink() or not path.is_file():
            raise ValueError('Non-regular evidence: ' + path.name)
        size = path.stat().st_size
        cap = MIB
        tail = path.suffix == '.log' and size > 16384
        if path.suffix == '.json' and size > cap:
            raise ValueError('Complete proof exceeds cap: ' + path.name)
        with path.open('rb') as stream:
            if tail:
                stream.seek(size - 16384)
            data = stream.read((16384 if path.suffix == '.log' else cap) + 1)
        if tail:
            # Drop only an incomplete leading UTF-8 codepoint, never invalid text.
            trim = 0
            while trim < min(3, len(data)) and data[trim] & 0xC0 == 0x80:
                trim += 1
            data = data[trim:]
        if b'\x00' in data:
            raise ValueError('NUL byte in text evidence: ' + path.name)
        data.decode('utf-8', errors='strict')
        result[path.name] = data
        manifest.append({'path': path.name, 'originalBytes': size,
                         'originalSha256': digest(path), 'retainedBytes': len(data),
                         'retainedSha256': hashlib.sha256(data).hexdigest(), 'tailOnly': tail})
    result['manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('phase', choices=('build', 'diagnose', 'pack'))
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--source-sha', choices=(SOURCE,), required=True)
    args = parser.parse_args()
    source, root = args.source.resolve(), args.root.resolve()
    temp = Path(os.environ['RUNNER_TEMP']).resolve()
    if root == temp or not root.is_relative_to(temp) or root.is_relative_to(source) or source.is_relative_to(root):
        raise ValueError('Evidence root must be a temporary child disjoint from source')
    full = load(Path(__file__).with_name('qualify-full-windows.py'), 'full_windows')
    life = full.adapter(source)
    phase_seconds = {'build': 7170, 'diagnose': 570, 'pack': 570}[args.phase]
    reserve = {'build': 5400, 'diagnose': 600, 'pack': 30}[args.phase]
    deadline = min(time.monotonic() + phase_seconds, full.read_json(root / 'clock.json')['deadline'] - reserve)
    build = source / 'build'

    def run(name, command, check=True):
        with (root / (name + '.log')).open('wb') as output:
            return life.owned_run([str(x) for x in command], cwd=source, stdout=output,
                                  stderr=subprocess.STDOUT,
                                  timeout=life.remaining(deadline, phase_seconds), check=check).returncode

    try:
        if args.phase != 'pack':
            identity = full.read_json(root / 'identity.json')
            if identity['source_sha'] != SOURCE or identity['configuration'] != 'Release':
                raise ValueError('Wrong configured source/configuration')
            run('target-source', ['git', '-C', source, 'rev-parse', 'HEAD'])
            if (root / 'target-source.log').read_text().strip() != SOURCE:
                raise ValueError('Product revision changed')
            run('target-clean', ['git', '-C', source, 'status', '--porcelain', '--untracked-files=no'])
            if (root / 'target-clean.log').read_text().strip():
                raise ValueError('Product source modified')
            for tool in identity['tools'].values():
                if full.digest(Path(tool['path'])) != tool['sha256']:
                    raise ValueError('Configured tool changed')
        if args.phase == 'build':
            reference_path = Path(__file__).with_name('isa-diagnostic-reference.json')
            reference = full.read_json(reference_path)
            validate_reference(identity, reference)
            run('target-build', build_command(build))
            image = build / 'bin/Release' / (TARGET + '.dll')
            pdbs = sorted(build.rglob(TARGET + '.pdb'))
            if not image.is_file() or len(pdbs) != 1:
                raise ValueError('Expected built DLL and exactly one matching PDB')
            full.save(root / 'target-identity.json', {
                'source_sha': SOURCE, 'image': str(image), 'pdb': str(pdbs[0]),
                'image_sha256': full.digest(image), 'pdb_sha256': full.digest(pdbs[0]),
                'original_image_sha256': ORIGINAL_IMAGE,
                'matches_original_image': full.digest(image) == ORIGINAL_IMAGE,
                'helper_sha256': full.digest(Path(__file__)),
                'reference_sha256': full.digest(reference_path),
                'measurement': 'new target-only build, not replay of original binary'})
        elif args.phase == 'diagnose':
            record = full.read_json(root / 'target-identity.json')
            def immutable():
                for name in ('image', 'pdb'):
                    if full.digest(Path(record[name])) != record[name + '_sha256']:
                        raise ValueError('Diagnostic input changed: ' + name)
            immutable()
            collector = Path(__file__).with_name('isa_failure_diagnostics.py')
            output = root / 'isa-diagnostics.json'
            if output.exists():
                raise ValueError('Diagnostic output must be fresh')
            command = [sys.executable, collector, '--source', source,
                       '--image', record['image'], '--pdb', record['pdb'],
                       '--objdump', identity['tools']['llvm-objdump']['path'],
                       '--pdbutil', identity['tools']['llvm-pdbutil']['path'], '--output', output]
            full.save(root / 'diagnostic-command.json', {'argv': [str(x) for x in command],
                       'collector_sha256': full.digest(collector), 'timeout_ceiling_seconds': 570})
            try:
                code = run('isa-collector', command, check=False)
            except Exception:
                # The collector checkpoints the strict scan before expensive
                # diagnostics. Preserve that failure even if its job times out.
                if output.is_file() and output.stat().st_size <= MIB:
                    result = terminal(None, full.read_json(output))
                    result['diagnosticComplete'] = False
                    full.save(root / 'target-terminal.json', result)
                raise
            immutable()
            if output.stat().st_size > MIB:
                raise ValueError('Collector proof exceeds 1 MiB')
            result = terminal(code, full.read_json(output))
            full.save(root / 'target-terminal.json', result)
            if not result['passed']:
                raise ValueError('Strict scan failed or diagnostic collection incomplete')
        else:
            life.remaining(deadline, phase_seconds)
            frozen = root / 'qualification-text'
            if frozen.exists():
                raise ValueError('Frozen evidence already exists')
            data = payloads(root, full.digest)
            archive = full.bundle(data)  # Existing 12 MiB text / 1 MiB ZIP bounds.
            frozen.mkdir()
            for name, value in data.items():
                (frozen / name).write_bytes(value)
            (root / 'qualification-text.zip').write_bytes(archive)
            full.save(root / 'archive.json', {'bytes': len(archive), 'sha256': hashlib.sha256(archive).hexdigest()})
    except Exception as error:
        full.save(root / (args.phase + '-failure.json'), {'phase': args.phase, 'error': str(error)})
        raise


if __name__ == '__main__':
    main()
