"""Extract the already hash-verified SDE archive in one streaming XZ pass."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import tarfile
import time

_SPEC = importlib.util.spec_from_file_location('signature_archive_names',
    Path(__file__).with_name('extract_release_signature_bundle.py'))
_NAMES = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_NAMES)


def linked(path):
    return path.is_symlink() or path.is_junction()


def member_path(member):
    name = member.name
    while name.startswith('./'):
        name = name[2:]
    if name in ('', '.') and member.isdir():
        return None
    if not name or name.startswith('/') or '\\' in name:
        raise ValueError('Unsafe SDE member path: ' + repr(member.name))
    path = PurePosixPath(name)
    if '..' in path.parts or '/'.join(path.parts) != name.rstrip('/'):
        raise ValueError('Non-normal SDE member path: ' + repr(member.name))
    for part in path.parts:
        _NAMES._member_name(tarfile.TarInfo(part))
        if any(ord(c) < 32 or c in '<>"|?*' for c in part):
            raise ValueError('Invalid Windows component: ' + repr(part))
    return path


def extract_archive(archive, destination):
    archive, destination = Path(archive).absolute(), Path(destination).absolute()
    if not archive.is_file() or linked(archive):
        raise ValueError('Missing or linked SDE archive')
    if archive.parent != destination.parent or not destination.parent.is_dir():
        raise ValueError('Extraction destination must be a fresh sibling of archive')
    if destination.exists() or linked(destination):
        raise ValueError('Extraction destination already exists')
    for parent in (destination.parent, *destination.parent.parents):
        if linked(parent):
            raise ValueError('Linked extraction parent')
    destination.mkdir()
    started = time.monotonic()
    headers, spelling = set(), {}
    count, expanded = 0, 0
    executables = []
    # Fresh directory, no links/special members, data filter as an additional guard.
    # r|xz avoids seeking/re-decompressing the XZ archive for every member.
    with tarfile.open(archive, mode='r|xz') as stream:
        for member in stream:
            if not (member.isdir() or member.isfile()) or member.issparse():
                raise ValueError('SDE archive contains link/special/sparse member')
            path = member_path(member)
            count += 1
            if path is None:
                if '.' in headers:
                    raise ValueError('Repeated SDE root directory')
                headers.add('.')
                continue
            key = path.as_posix().casefold()
            if key in headers:
                raise ValueError('Repeated SDE archive member')
            headers.add(key)
            for i in range(1, len(path.parts) + 1):
                prefix = '/'.join(path.parts[:i])
                previous = spelling.setdefault(prefix.casefold(), prefix)
                if previous != prefix:
                    raise ValueError('Case-colliding SDE path component')
            target = destination.joinpath(*path.parts)
            if not target.resolve().is_relative_to(destination.resolve()):
                raise ValueError('Escaping SDE extraction target')
            # Names are canonicalized above; retain all regular runtime files and dirs.
            member.name = path.as_posix()
            stream.extract(member, path=destination, filter='data')
            if member.isfile():
                expanded += member.size
                if path.name.casefold() == 'sde.exe':
                    executables.append(target)
    if len(executables) != 1:
        raise ValueError('Expected exactly one sde.exe')
    with executables[0].open('rb') as image:
        image_hash = hashlib.file_digest(image, 'sha256').hexdigest()
    return {'archiveBytes': archive.stat().st_size, 'memberCount': count,
            'declaredExpandedBytes': expanded, 'extractionSeconds': time.monotonic() - started,
            'executableRelativePath': executables[0].relative_to(destination).as_posix(),
            'executableSha256': image_hash}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--metadata', type=Path, required=True)
    args = parser.parse_args()
    result = extract_archive(args.archive, args.destination)
    args.metadata.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, sort_keys=True))

if __name__ == '__main__':
    main()
