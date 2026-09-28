#!/usr/bin/env python3
"""TagReleaseContract_PackagedVersions: every packaged product embeds the tag version (REL-100).

Fixtures are synthetic PE32+ images whose .rsrc section holds a
VS_VERSIONINFO laid out as rc.exe emits cmake/SparkWindowsVersionInfo.rc.in.
The parser was also checked against a real windows-shipping CPack ZIP.
"""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import tempfile
import unittest
import zipfile

import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import verify_package_versions as vpv  # noqa: E402

VERSION = "1.2.3"
PACKAGE = f"SparkEngine-{VERSION}-Windows-AMD64.zip"
CONFIG_VERSION = (
    'set(PACKAGE_VERSION "{version}")\n'
    'if(NOT CMAKE_SIZEOF_VOID_P STREQUAL "8")\n'
    '  set(PACKAGE_VERSION "${{PACKAGE_VERSION}} (${{installedBits}}bit)")\n'
    'endif()\n'
)


def _pad(buffer: bytearray) -> None:
    buffer.extend(b"\0" * (-len(buffer) % 4))


def _block(key: str, value: bytes = b"", children: tuple = (), *, text: bool = False) -> bytes:
    out = bytearray(6) + key.encode("utf-16-le") + b"\0\0"
    _pad(out)
    out += value
    for child in children:
        _pad(out)
        out += child
    value_length = len(value) // 2 if text else len(value)
    struct.pack_into("<HHH", out, 0, len(out), value_length, 1 if text else 0)
    return bytes(out)


def _string(key: str, value: str) -> bytes:
    return _block(key, (value + "\0").encode("utf-16-le"), text=True)


def version_resource(fixed=(1, 2, 3, 0), product=(1, 2, 3, 0), file_text="1.2.3.0", product_text="1.2.3.0",
                     signature=vpv.VS_FIXEDFILEINFO_SIGNATURE) -> bytes:
    def split(parts):
        return (parts[0] << 16) | parts[1], (parts[2] << 16) | parts[3]

    fixed_info = struct.pack("<13I", signature, 0x10000, *split(fixed), *split(product), 0x3F, 0, 0x40004, 1, 0, 0, 0)
    strings = _block("040904b0", children=(_string("CompanyName", "SparkEngine"),
                                           _string("FileVersion", file_text),
                                           _string("ProductVersion", product_text)), text=True)
    string_info = _block("StringFileInfo", children=(strings,), text=True)
    var_info = _block("VarFileInfo", children=(_block("Translation", struct.pack("<HH", 0x0409, 1200)),), text=True)
    return _block("VS_VERSION_INFO", fixed_info, (string_info, var_info))


def pe_image(resource: bytes | None) -> bytes:
    """A minimal PE32+ image, with an RT_VERSION leaf when ``resource`` is given."""
    section_rva, section_offset = 0x1000, 0x200
    rsrc = bytearray()
    if resource is not None:
        rsrc += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + struct.pack("<II", vpv.RT_VERSION, 0x80000000 | 0x18)
        rsrc += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + struct.pack("<II", 1, 0x80000000 | 0x30)
        rsrc += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + struct.pack("<II", 0x409, 0x48)
        rsrc += struct.pack("<IIII", section_rva + 0x58, len(resource), 0, 0)
        rsrc += b"\0" * (0x58 - len(rsrc)) + resource
    else:
        rsrc += b"\0" * 16
    _pad(rsrc)

    image = bytearray(section_offset)
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x40)
    image[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, 0x44, 0x8664, 1, 0, 0, 0, 240, 0x22)
    optional = 0x58
    struct.pack_into("<H", image, optional, 0x20B)
    struct.pack_into("<I", image, optional + 108, 16)
    if resource is not None:
        struct.pack_into("<II", image, optional + 112 + 8 * 2, section_rva, len(rsrc))
    section = optional + 240
    image[section:section + 8] = b".rsrc\0\0\0"
    struct.pack_into("<IIII", image, section + 8, len(rsrc), section_rva, len(rsrc), section_offset)
    return bytes(image + rsrc)


class PackagedVersionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.temp, ignore_errors=True)

    def package(self, name: str = PACKAGE, members: dict[str, bytes] | None = None) -> Path:
        root = name.rsplit(".zip", 1)[0].rsplit(".tar.gz", 1)[0]
        if members is None:
            members = self.members()
        path = self.temp / name
        if name.endswith(".zip"):
            with zipfile.ZipFile(path, "w") as archive:
                for member, data in members.items():
                    archive.writestr(f"{root}/{member}", data)
        else:
            with tarfile.open(path, "w:gz") as archive:
                for member, data in members.items():
                    info = tarfile.TarInfo(f"{root}/{member}")
                    info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
        return path

    def members(self, **overrides: bytes) -> dict[str, bytes]:
        members = {
            "bin/SparkEngine.exe": pe_image(version_resource()),
            "bin/SparkConsole.exe": pe_image(version_resource()),
            "bin/SparkGameFPS.dll": pe_image(None),
            "bin/SDL2.dll": b"not inspected",
            vpv.CONFIG_VERSION_SUFFIX: CONFIG_VERSION.format(version=VERSION).encode(),
        }
        members.update(overrides)
        return members

    def errors(self, path: Path) -> list[str]:
        return vpv.package_errors(path, VERSION, vpv.shipped_executables())

    def run_main(self, *paths: Path) -> tuple[int, str]:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
            status = vpv.main(["--version", VERSION, *map(str, paths)])
        return status, stderr.getvalue()

    def test_matching_zip_and_tgz_packages_pass(self) -> None:
        zip_package = self.package()
        tgz_package = self.package(f"SparkEngine-{VERSION}-Windows-AMD64.tar.gz")
        self.assertEqual(self.errors(zip_package), [])
        self.assertEqual(self.errors(tgz_package), [])
        self.assertEqual(self.run_main(zip_package, tgz_package), (0, ""))

    def test_wrong_fixed_file_version_fails(self) -> None:
        path = self.package(members=self.members(**{"bin/SparkEngine.exe": pe_image(version_resource(fixed=(1, 2, 4, 0)))}))
        self.assertIn(f"{PACKAGE[:-4]}/bin/SparkEngine.exe: fixed FileVersion is 1.2.4.0, expected 1.2.3.0",
                      self.errors(path))
        self.assertEqual(self.run_main(path)[0], 1)

    def test_wrong_string_product_version_fails(self) -> None:
        image = pe_image(version_resource(product_text="1.2.3"))
        errors = self.errors(self.package(members=self.members(**{"bin/SparkConsole.exe": image})))
        self.assertEqual(errors, [f"{PACKAGE[:-4]}/bin/SparkConsole.exe: string table 040904b0 ProductVersion is "
                                  "'1.2.3', expected '1.2.3.0'"])

    def test_bad_fixed_info_signature_fails(self) -> None:
        image = pe_image(version_resource(signature=0))
        errors = self.errors(self.package(members=self.members(**{"bin/SparkConsole.exe": image})))
        self.assertIn(f"{PACKAGE[:-4]}/bin/SparkConsole.exe: VS_FIXEDFILEINFO signature is wrong", errors)

    def test_missing_version_resource_fails(self) -> None:
        errors = self.errors(self.package(members=self.members(**{"bin/SparkEngine.exe": pe_image(None)})))
        self.assertEqual(errors, [f"{PACKAGE[:-4]}/bin/SparkEngine.exe: no version resource"])

    def test_unlisted_spark_executable_is_checked_too(self) -> None:
        errors = self.errors(self.package(members=self.members(**{"bin/SparkNewTool.exe": pe_image(None)})))
        self.assertEqual(errors, [f"{PACKAGE[:-4]}/bin/SparkNewTool.exe: no version resource"])

    def test_package_without_a_shipped_executable_fails(self) -> None:
        members = self.members()
        del members["bin/SparkEngine.exe"], members["bin/SparkConsole.exe"]
        self.assertEqual(self.errors(self.package(members=members)), ["no shipped Spark executable was found"])

    def test_mismatched_or_missing_config_version_fails(self) -> None:
        wrong = CONFIG_VERSION.format(version="1.2.4").encode()
        errors = self.errors(self.package(members=self.members(**{vpv.CONFIG_VERSION_SUFFIX: wrong})))
        self.assertEqual(errors, ["SparkEngineConfigVersion.cmake sets PACKAGE_VERSION ['1.2.4'], expected ['1.2.3']"])
        members = self.members()
        del members[vpv.CONFIG_VERSION_SUFFIX]
        self.assertEqual(self.errors(self.package(members=members)),
                         [f"expected one {vpv.CONFIG_VERSION_SUFFIX}, found 0"])

    def test_wrong_archive_name_fails(self) -> None:
        path = self.package(name="SparkEngine-1.2.30-Windows-AMD64.zip")
        self.assertEqual(self.errors(path), [f"file name does not carry -{VERSION}-"])

    def test_unreadable_package_and_bad_version_argument_fail(self) -> None:
        corrupt = self.temp / PACKAGE
        corrupt.write_bytes(b"not a zip")
        status, stderr = self.run_main(corrupt)
        self.assertEqual(status, 1)
        self.assertIn("unreadable package", stderr)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(vpv.main(["--version", "1.2", str(self.package())]), 1)

    def test_shipped_set_comes_from_the_cmake_version_module(self) -> None:
        shipped = vpv.shipped_executables()
        for name in ("SparkEngine.exe", "SparkConsole.exe", "SparkEditor.exe", "SparkInstaller.exe"):
            self.assertIn(name, shipped)
        module = self.temp / "SparkWindowsVersionInfo.cmake"
        module.write_text("function(x)\nendfunction()\n", encoding="utf-8")
        with self.assertRaises(vpv.PackageVersionError):
            vpv.shipped_executables(module)


class WorkflowWiringTests(unittest.TestCase):
    def test_windows_package_job_checks_the_archive_before_extracting_it(self) -> None:
        workflow = yaml.safe_load((ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8"))
        step = next(step for step in workflow["jobs"]["build-windows"]["steps"]
                    if step.get("name") == "Extract and smoke-test portable package")
        self.assertNotIn("if", step)
        self.assertNotIn("continue-on-error", step)
        command = ('python tools/verify_package_versions.py --version "${{ needs.prepare.outputs.cmake_version }}" '
                   '"$($archives[0].FullName)"\nif ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }')
        self.assertIn(command, step["run"])
        self.assertLess(step["run"].index(command), step["run"].index("Expand-Archive"))


if __name__ == "__main__":
    unittest.main()
