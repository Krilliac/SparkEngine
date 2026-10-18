#!/usr/bin/env python3
"""Bundle exact parity diagnostics, never product binaries or acceptance evidence."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import zipfile

INPUTS = ('build-matrix-parity-findings.json', 'build-matrix-inventory.json',
          'build-matrix-pending-receipt.json')
MAX_EXPANDED = 12 * 1024 * 1024
# Reserve space below the 1 MiB retrieval cap for the upload service wrapper.
MAX_ZIP = 960 * 1024


def bundle(root, environment):
    payloads = {}
    records = []
    total = 0
    for name in INPUTS:
        path = root / name
        try:
            with path.open('rb') as stream:
                data = stream.read(MAX_EXPANDED - total + 1)
        except FileNotFoundError:
            records.append({'path': name, 'state': 'missing'})
            continue
        total += len(data)
        if total > MAX_EXPANDED:
            raise ValueError('diagnostics exceed expanded byte limit')
        json.loads(data.decode('utf-8'))
        payloads[name] = data
        records.append({'path': name, 'state': 'present', 'bytes': len(data),
                        'sha256': hashlib.sha256(data).hexdigest()})
    provenance = {'schemaVersion': 1, 'purpose': 'diagnostic-only',
                  'sourceSha': environment.get('GITHUB_SHA', ''),
                  'repository': environment.get('GITHUB_REPOSITORY', ''),
                  'runId': environment.get('GITHUB_RUN_ID', ''),
                  'runAttempt': environment.get('GITHUB_RUN_ATTEMPT', ''),
                  'workflowRef': environment.get('GITHUB_WORKFLOW_REF', ''),
                  'workflowSha': environment.get('GITHUB_WORKFLOW_SHA', ''),
                  'inputs': records}
    payloads['diagnostic-provenance.json'] = (json.dumps(provenance, indent=2) + '\n').encode()
    if sum(map(len, payloads.values())) > MAX_EXPANDED:
        raise ValueError('diagnostics including provenance exceed expanded byte limit')
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in payloads.items():
            archive.writestr(name, data)
    if output.tell() > MAX_ZIP:
        raise ValueError('diagnostics exceed compressed byte limit')
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('.'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = bundle(args.root, os.environ)
    # Exclusive creation prevents a stale bundle being silently reused.
    with args.output.open('xb') as stream:
        stream.write(data)


if __name__ == '__main__':
    main()
