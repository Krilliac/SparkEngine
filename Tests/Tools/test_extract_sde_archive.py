import hashlib
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('sde_extract', ROOT / '.github/scripts/extract_sde_archive.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class Contracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.archive = self.root / 'fixture.tar.xz'
        self.dest = self.root / 'unpacked'

    def make_archive(self, rows):
        with tarfile.open(self.archive, 'w:xz') as out:
            for name, data, kind in rows:
                entry = tarfile.TarInfo(name)
                entry.type = kind
                entry.mode = 0o755 if kind == tarfile.DIRTYPE else 0o644
                if kind in (tarfile.SYMTYPE, tarfile.LNKTYPE): entry.linkname = '../outside'
                if kind == tarfile.REGTYPE: entry.size = len(data)
                out.addfile(entry, io.BytesIO(data) if kind == tarfile.REGTYPE else None)

    def test_full_nested_layout_runtime_files_and_metadata(self):
        files = {'sde-kit/sde.exe': b'benign-executable-fixture',
                 'sde-kit/intel64/runtime.dll': b'64-bit-runtime-fixture',
                 'sde-kit/ia32/runtime.dll': b'32-bit-runtime-fixture',
                 'sde-kit/misc/config.txt': b'configuration', 'sde-kit/LICENSE': b'license'}
        self.make_archive([('./', b'', tarfile.DIRTYPE), ('./sde-kit/', b'', tarfile.DIRTYPE)] +
                          [(name, data, tarfile.REGTYPE) for name, data in files.items()])
        result = m.extract_archive(self.archive, self.dest)
        self.assertEqual({p.relative_to(self.dest).as_posix(): p.read_bytes() for p in self.dest.rglob('*') if p.is_file()}, files)
        self.assertEqual(result['memberCount'], len(files) + 2)
        self.assertEqual(result['declaredExpandedBytes'], sum(map(len, files.values())))
        self.assertEqual(result['executableSha256'], hashlib.sha256(files['sde-kit/sde.exe']).hexdigest())
        self.assertEqual(result['archiveBytes'], self.archive.stat().st_size)
        self.assertGreaterEqual(result['extractionSeconds'], 0)

    def test_existing_destination_is_not_reused(self):
        self.make_archive([('sde.exe', b'fixture', tarfile.REGTYPE)])
        self.dest.mkdir()
        with self.assertRaisesRegex(ValueError, 'already exists'): m.extract_archive(self.archive, self.dest)

    def test_paths_outside_root_are_rejected(self):
        self.make_archive([('../outside', b'bad', tarfile.REGTYPE)])
        with self.assertRaises(ValueError): m.extract_archive(self.archive, self.dest)
        self.assertFalse((self.root / 'outside').exists())

    def test_links_and_special_members_are_rejected(self):
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE, tarfile.CHRTYPE):
            with self.subTest(kind=kind):
                self.make_archive([('linked', b'', kind)])
                target = self.root / ('unpacked-' + kind.hex())
                with self.assertRaisesRegex(ValueError, 'link/special'): m.extract_archive(self.archive, target)

    def test_duplicate_and_case_colliding_components_rejected(self):
        for first, second in [('kit/sde.exe', 'kit/sde.exe'), ('Kit/sde.exe', 'kit/runtime.dll')]:
            self.make_archive([(first, b'a', tarfile.REGTYPE), (second, b'b', tarfile.REGTYPE)])
            target = self.root / ('collision-' + str(len(list(self.root.iterdir()))))
            with self.assertRaises(ValueError): m.extract_archive(self.archive, target)

    def test_missing_or_multiple_executable_rejected(self):
        for names in [('kit/runtime.dll',), ('a/sde.exe', 'b/sde.exe')]:
            self.make_archive([(name, b'fixture', tarfile.REGTYPE) for name in names])
            target = self.root / ('exe-' + str(len(names)))
            with self.assertRaisesRegex(ValueError, 'exactly one'): m.extract_archive(self.archive, target)

class NameContracts(unittest.TestCase):
    def test_unsafe_windows_names_rejected(self):
        for name in ('/absolute', 'C:/absolute', 'kit\\file', 'kit/../escape',
                     'kit/file:stream', 'kit/NUL.txt', 'kit/COM1', 'kit/name.',
                     'kit/name ', 'kit/a?b', 'kit/a//b', 'kit/./b'):
            with self.subTest(name=name), self.assertRaises(ValueError): m.member_path(tarfile.TarInfo(name))

    def test_hash_pin_and_deadlines_unchanged(self):
        script = (ROOT / '.github/scripts/full-windows-sde.ps1').read_text()
        self.assertIn('74e626ede09b0baa5011fc9e51b58627ea92c3fc0bae5fd7db34b490f335f651', script)
        self.assertIn('https://downloadmirror.intel.com/924984/sde-external-10.13.1-2026-07-28-win.tar.xz', script)
        self.assertIn('-TimeoutSec 240', script)
        self.assertEqual(script.count('Write-Host "Intel SDE:'), 4)
        self.assertEqual(script.count("UtcNow.ToString('o')"), 4)
        self.assertLess(script.index('$actualSha256 -ne $expectedSha256'), script.index('extract_sde_archive.py'))
        self.assertIn('if ($LASTEXITCODE -ne 0)', script)
        self.assertIn('$executables.Count -ne 1', script)

if __name__ == '__main__': unittest.main()
