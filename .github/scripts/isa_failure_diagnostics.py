"""Bounded diagnostic wrapper; run only inside the qualification owned-process job."""
import argparse
from dataclasses import asdict
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

SOURCE = 'd90b86e42fd586a56d53f077bbca6a457200d9bd'
ORIGINAL_IMAGE = 'a78b3d3c6563a15a92af289e9da67e795a884e56098fe08f92be76b31fbfab3b'
CAP = 1024 * 1024
MAX_REGION = 64 * 1024
MAX_RAW = 128 * 1024
MAX_REGIONS = 8


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def encoded(record):
    data = (json.dumps(record, indent=2) + '\n').encode('utf-8')
    if len(data) > CAP:
        raise ValueError('Complete diagnostic JSON exceeds 1 MiB')
    return data


def region_plan(findings, procedures, image_base=0, sections=()):
    """Retain extents or explicitly unclassified section windows; never guess a function."""
    selected, windows = {}, {}
    for finding in findings:
        address = int(finding['address'], 16)
        matches = [p for p in procedures if p.start <= address < p.end]
        if matches:
            for p in matches:
                selected[(p.start, p.end, p.name, p.ambiguous)] = p
            continue
        mappings = [(image_base+rva, image_base+rva+min(virtual, raw))
                    for rva, virtual, raw, offset, flags in sections
                    if flags & 0x20000000 and image_base+rva <= address < image_base+rva+min(virtual, raw)]
        if len(mappings) != 1:
            raise ValueError('Uncovered finding lacks unique executable section')
        lo, hi = mappings[0]
        start, end = max(lo, address-1024), min(hi, address+1025)
        before = max((p.end for p in procedures if p.end <= address), default=None)
        after = min((p.start for p in procedures if p.start > address), default=None)
        adjacent = [asdict(p) for p in procedures if (p.end == before or p.start == after
                    or (p.start < end and start < p.end))]
        windows[address] = {'findingAddress': address, 'start': start, 'end': end,
                            'sectionStart': lo, 'sectionEnd': hi,
                            'classification': 'unclassified context; no matching PDB extent',
                            'adjacentParsedExtents': adjacent}
    candidates = [{'start':p.start,'end':p.end,'pdbExtents':[asdict(p)],'unclassifiedWindows':[]}
                  for p in selected.values()]
    candidates += [{'start':w['start'],'end':w['end'],'pdbExtents':[], 'unclassifiedWindows':[w]}
                   for w in windows.values()]
    regions = []
    for item in sorted(candidates, key=lambda r:(r['start'],r['end'])):
        if regions and item['start'] < regions[-1]['end']:
            regions[-1]['end'] = max(regions[-1]['end'], item['end'])
            regions[-1]['pdbExtents'].extend(item['pdbExtents'])
            regions[-1]['unclassifiedWindows'].extend(item['unclassifiedWindows'])
        else:
            regions.append(item)
    sizes = [r['end'] - r['start'] for r in regions]
    if len(regions) > MAX_REGIONS or any(n <= 0 or n > MAX_REGION for n in sizes) or sum(sizes) > MAX_RAW:
        raise ValueError('Complete failing regions exceed diagnostic bound')
    return regions


def collect_regions(findings, procedures, image_base, sections, read_at, disassemble):
    regions = region_plan(findings, procedures, image_base, sections)
    for region in regions:
        start, end = region['start'], region['end']
        mappings = [(image_base + rva, min(virtual, raw), offset) for rva, virtual, raw, offset, flags in sections
                    if flags & 0x20000000 and image_base + rva <= start and end <= image_base + rva + min(virtual, raw)]
        if len(mappings) != 1:
            raise ValueError('Region lacks unique executable file mapping')
        section_start, _, section_offset = mappings[0]
        offset = section_offset + start - section_start
        raw = read_at(offset, end - start)
        if len(raw) != end - start:
            raise ValueError('Truncated procedure bytes')
        text = disassemble(start, end)
        if not text.strip() or len(text.encode('utf-8')) > 256 * 1024:
            raise ValueError('Missing or oversized complete region disassembly')
        region.update(imageBase=image_base, startRva=start-image_base, endRva=end-image_base,
                      fileOffset=offset, byteCount=len(raw), rawHex=raw.hex(),
                      rawSha256=hashlib.sha256(raw).hexdigest(), disassembly=text)
    return regions


def finish_diagnostics(record, regions, commands):
    """Evidence completeness must never replace the independently computed scan exit."""
    record.update(regions=regions, disassemblyCommands=commands, diagnosticComplete=True)
    encoded(record)
    return record['scanExitCode']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'image', 'pdb', 'objdump', 'pdbutil', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError('Diagnostic output already exists')
    record = {'diagnosticComplete': False, 'scanExitCode': None, 'sourceSha': SOURCE,
              'workflowSha': os.environ.get('GITHUB_SHA'), 'run': os.environ.get('GITHUB_RUN_ID'),
              'attempt': os.environ.get('GITHUB_RUN_ATTEMPT'), 'originalImageSha256': ORIGINAL_IMAGE,
              'scope': 'Target-only static diagnostic; not full-suite qualification. Original PDB was not retained.'}
    def save():
        data = encoded(record)
        temporary = args.output.with_suffix(args.output.suffix+'.pending')
        with temporary.open('xb') as stream:
            stream.write(data)
        temporary.replace(args.output)
    save()
    try:
        def git(*cmd):
            return subprocess.check_output(['git','-C',str(args.source),*cmd],text=True,timeout=30).strip()
        if git('rev-parse','HEAD') != SOURCE or git('status','--porcelain','--untracked-files=no'):
            raise ValueError('Expected clean exact d90 product')
        source_file = args.source / 'tools/check_isa_baseline.py'
        spec = importlib.util.spec_from_file_location('diagnostic_exact_isa', source_file)
        scanner = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = scanner
        spec.loader.exec_module(scanner)
        paths = {'image': args.image, 'pdb': args.pdb, 'objdump': args.objdump, 'pdbutil': args.pdbutil,
                 'scanner': source_file, 'codeMap': args.source/'tools/isa_code_map.py',
                 'symbolIdentityParser': args.source/'tools/shipping_symbol_manifest.py'}
        hashes = {name: sha(path) for name,path in paths.items()}
        record.update(hashes=hashes, paths={n:str(p.resolve()) for n,p in paths.items()},
                      originalImageMatch=hashes['image']==ORIGINAL_IMAGE)
        save()
        tool = [str(args.objdump.resolve())]
        if not scanner.image_is_x86_64(tool, str(args.image)):
            raise ValueError('Expected x86-64 image')
        pdb = scanner._pdb_info(str(args.pdb), str(args.image), str(args.pdbutil.resolve()))
        identity_parser = sys.modules['spark_shipping_symbol_manifest']
        with args.image.open('rb') as image_stream, args.pdb.open('rb') as pdb_stream:
            record['peCodeView'] = asdict(identity_parser.inspect_pe(image_stream))
            record['pdbIdentity'] = asdict(identity_parser.inspect_pdb(pdb_stream))
        record['matchedPePdbIdentity'] = True  # exact scanner validated CodeView GUID and age
        record['pdbToolsets'] = sorted(pdb.toolsets)
        save()
        result = scanner.scan(tool, str(args.image), [], pdb)
        findings = [asdict(f) for f in result.violations]
        record.update(scanExitCode=1 if findings else 0, findings=findings,
                      instructionCount=result.instructions, findingCount=len(findings))
        save()  # preserve strict scan result even if later diagnostic collection times out
        image_base, sections = scanner._pe_section_table(str(args.image))
        def read_at(offset, size):
            with args.image.open('rb') as image:
                image.seek(offset)
                return image.read(size)
        commands = []
        def disassemble(start, end):
            command = scanner._disassemble_command(tool, str(args.image),
                        '--start-address='+hex(start), '--stop-address='+hex(end))
            commands.append(command)
            with tempfile.TemporaryFile() as stream:
                subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=30)
                stream.seek(0)
                data = stream.read(256*1024+1)
            if len(data) > 256*1024:
                raise ValueError('Complete disassembly exceeds bound')
            return data.decode('utf-8', errors='strict')
        regions = collect_regions(findings, pdb.procedures, image_base, sections, read_at, disassemble)
        if hashes != {name:sha(path) for name,path in paths.items()}:
            raise ValueError('Image/PDB/tool/source changed during diagnostic')
        code = finish_diagnostics(record, regions, commands)
        save()
        return code
    except Exception as error:
        record['diagnosticComplete'] = False
        record['diagnosticError'] = str(error)
        # Never silently truncate complete proof to fit a bound.
        record.pop('regions', None)
        record.pop('disassemblyCommands', None)
        try:
            save()
        except ValueError:
            record.pop('findings', None)
            record['findingsComplete'] = False
            save()
        return 2

if __name__ == '__main__':
    sys.exit(main())
