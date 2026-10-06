#!/usr/bin/env python3
"""Check unchanged approved Primary images and their explicit mutation dependencies."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('capture', ROOT / 'Tools/rhi210_capture.py')
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)
APPROVED = {
    'Primary_DeferredGeometry': '6e25c22bd2c2d929bf5e851292b333e5bd3df31d4236b826697ad16a7db420ac',
    'Primary_DeferredLighting': '6f88f8c2a024afe456a6d1923493a4baa70cd11a90f46eea65f4b46dd63680ae',
    'Primary_ShadowDepth': 'b083f6cec0dab16b593b127b3ee43c7ab3828a9665762fb2da1ce2fb3b7451b3',
}
AFFECTED = {
    None: set(),
    'Primary_DeferredGeometry': {'Primary_DeferredGeometry', 'Primary_DeferredLighting'},
    'Primary_DeferredLighting': {'Primary_DeferredLighting'},
    'Primary_ShadowDepth': {'Primary_ShadowDepth'},
}
FILE_NAME = 'TestRHI210D3D11PrimaryGoldenReal.cpp'


def validate(result, scene):
    # Keep strict per-test assertion bodies, test identities, process status and
    # debug validation checks. Never reconstruct missing JUnit from aggregate stderr.
    capture._validate_capture_result(result, scene or 'normal', FILE_NAME, 4, mutant_scene=scene)
    affected = AFFECTED[scene]
    verdicts = [line for line in result['stdout'].splitlines() if '[RHI-210 GOLDEN] scene=' in line]
    observed = {}
    for line in verdicts:
        match = re.search(r'scene=(\S+) matched=(yes|no) differing=(\d+)/(\d+) '
                          r'\([\d.]+%, tolerance ([\d.]+)%\).* threshold=([\d.]+)$', line)
        if not match:
            raise capture.CaptureError(f'malformed golden verdict: {line}')
        name, status, differing, total, tolerance, threshold = match.groups()
        if name not in APPROVED or name in observed:
            raise capture.CaptureError(f'unexpected or duplicate scene: {name}')
        differing, total = int(differing), int(total)
        if float(tolerance) != 0 or float(threshold) != 0 or not 0 <= differing <= total or total == 0:
            raise capture.CaptureError(f'invalid comparison policy or pixel counts: {line}')
        expected_mismatch = name in affected
        if (status == 'no') != expected_mismatch or (differing > 0) != expected_mismatch:
            raise capture.CaptureError(f'unexpected mutation effect: {line}')
        observed[name] = line
    if set(observed) != set(APPROVED):
        raise capture.CaptureError('missing Primary scene verdict')
    cases = ET.parse(result['junitPath']).getroot().findall('.//testcase')
    failed_names = {case.get('name') for case in cases if case.find('failure') is not None}
    expected_failed = {capture.TEST_NAME_BY_SCENE[name] for name in affected}
    if failed_names != expected_failed or result['returncode'] != (1 if affected else 0):
        raise capture.CaptureError('JUnit failures/exit disagree with explicit mutation dependencies')
    return verdicts


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    capture.TIMEOUT_SECONDS = 120
    for scene, digest in APPROVED.items():
        path = ROOT / 'Tests/GoldenImages/d3d11-warp' / f'{scene}.png'
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise capture.CaptureError(f'approved baseline hash changed: {scene}')
    exe, out = args.exe.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    records = []
    for scene in [None, *APPROVED]:
        token = None if scene is None else capture.MUTANTS[scene][0]
        folder = out / ('normal' if scene is None else token)
        folder.mkdir()
        result = capture._run_one(exe, folder, FILE_NAME, 4, disable=token, junit_path=folder / 'tests.xml')
        capture._write_result(folder / 'tests.log', result)
        verdicts = validate(result, scene)
        records.append({'scene': scene, 'exitCode': result['returncode'], 'verdicts': verdicts})
        print(*verdicts, sep='\n', flush=True)
    (out / 'results.json').write_text(json.dumps({
        'binarySha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'runs': records}, indent=2) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
