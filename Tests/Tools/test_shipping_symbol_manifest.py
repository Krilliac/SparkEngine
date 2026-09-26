#!/usr/bin/env python3
"""BLD-100: shipped images map to private symbols by build ID.

These tests build real fixtures and run tools/shipping_symbol_manifest.py on
them:

* gcc ELF executables and shared libraries linked through the production link
  launcher, cmake/SparkSplitDebugLink.cmake, map to their split .debug files;
  the build-id is cross-checked against readelf;
* clang/lld-link PE DLLs linked with /DEBUG /PDBALTPATH:%_PDB% map to their
  PDBs; GUID and age are cross-checked against llvm-readobj and llvm-pdbutil;
* missing, duplicate, orphaned, mismatched and CRC-mismatched symbols, images
  that still carry debug info or have no build-id, absolute PDB paths, symbols
  left in the runtime tree and malformed images all fail and write nothing;
* the manifest schema is closed;
* the root SPARK_SHIPPED_IMAGE_TARGETS list, whose symbols the "symbols"
  install component carries, names every image target any first-party CMake
  file installs to bin or lib.
"""

from __future__ import annotations

import importlib.util
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL = REPO_ROOT / "tools" / "shipping_symbol_manifest.py"
LAUNCHER = REPO_ROOT / "cmake" / "SparkSplitDebugLink.cmake"
ROOT_CMAKE = REPO_ROOT / "CMakeLists.txt"

GCC = shutil.which("gcc")
OBJCOPY = shutil.which("objcopy")
CMAKE = shutil.which("cmake")
READELF = shutil.which("readelf")
CLANG = shutil.which("clang")
LLD_LINK = shutil.which("lld-link")
LLVM_READOBJ = shutil.which("llvm-readobj")
LLVM_PDBUTIL = shutil.which("llvm-pdbutil")

ELF_TOOLS = GCC and OBJCOPY and CMAKE and sys.platform.startswith("linux")
PE_TOOLS = CLANG and LLD_LINK


def _load_tool():
    spec = importlib.util.spec_from_file_location("shipping_symbol_manifest", TOOL)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


tool = _load_tool()


def _run(*command: str | Path, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(part) for part in command], cwd=cwd, capture_output=True, text=True, check=False, timeout=120
    )


def _check(*command: str | Path, cwd: Path | None = None) -> str:
    result = _run(*command, cwd=cwd)
    if result.returncode != 0:
        raise AssertionError(f"{command} failed:\n{result.stdout}\n{result.stderr}")
    return result.stdout


class ManifestRun:
    def __init__(self, root: Path, images: list[Path], symbols: list[Path]) -> None:
        self.output = root / "manifest.json"
        self.output.unlink(missing_ok=True)
        args = [sys.executable, "-B", TOOL, "--output", self.output]
        for path in images:
            args += ["--images", path]
        for path in symbols:
            args += ["--symbols", path]
        self.result = _run(*args)

    @property
    def manifest(self) -> dict:
        return json.loads(self.output.read_text(encoding="utf-8"))


class ToolTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory(prefix="spark-symbols-")
        self.root = Path(self._temp.name)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def assert_ok(self, run: ManifestRun) -> dict:
        self.assertEqual(run.result.returncode, 0, run.result.stderr)
        manifest = run.manifest
        tool.validate_manifest(manifest)
        return manifest

    def assert_fails(self, run: ManifestRun, message: str) -> None:
        self.assertEqual(run.result.returncode, 1, run.result.stdout + run.result.stderr)
        self.assertIn(message, run.result.stderr)
        self.assertFalse(run.output.exists(), "a failing run must not write a manifest")


@unittest.skipUnless(ELF_TOOLS, "needs Linux with gcc, objcopy and cmake")
class ElfSymbolTests(ToolTestCase):
    def _object(self, name: str, source: str, *flags: str) -> Path:
        source_path = self.root / f"{name}.c"
        source_path.write_text(source, encoding="utf-8")
        object_path = self.root / f"{name}.o"
        _check(GCC, "-g", "-fPIC", *flags, "-c", source_path, "-o", object_path)
        return object_path

    def _link(self, output: Path, objects: list[Path], *flags: str, split: bool = True) -> Path:
        output.parent.mkdir(parents=True, exist_ok=True)
        link = [GCC, *flags, *objects, "-o", output]
        if split:
            _check(CMAKE, f"-DSPARK_OBJCOPY={OBJCOPY}", "-P", LAUNCHER, "--", *link)
        else:
            _check(*link)
        return output

    def _app(self, directory: Path, marker: int = 0, split: bool = True, build_id: str = "sha1") -> Path:
        obj = self._object(f"app{marker}", f"int main(void) {{ return {marker}; }}\n")
        return self._link(directory / "app", [obj], f"-Wl,--build-id={build_id}", split=split)

    def _library(self, directory: Path) -> Path:
        obj = self._object("fixture", "int Fixture(int value) { return value * 3; }\n")
        return self._link(directory / "libfixture.so", [obj], "-shared", "-Wl,--build-id=sha1")

    def _readelf_build_id(self, path: Path) -> str:
        match = re.search(r"Build ID:\s*([0-9a-f]+)", _check(READELF, "-n", path))
        assert match is not None
        return match.group(1)

    def test_split_images_map_to_their_debug_files(self) -> None:
        bin_dir = self.root / "bin"
        app = self._app(bin_dir)
        library = self._library(bin_dir)
        manifest = self.assert_ok(ManifestRun(self.root, [bin_dir], [bin_dir]))
        self.assertEqual([image["path"] for image in manifest["images"]], ["app", "libfixture.so"])
        for image, path in zip(manifest["images"], (app, library)):
            self.assertEqual(image["format"], "elf")
            self.assertEqual(image["symbols"]["path"], path.name + ".debug")
            self.assertEqual(image["symbolKey"], image["buildId"])
            crc = zlib.crc32(path.with_name(path.name + ".debug").read_bytes()) & 0xFFFFFFFF
            self.assertEqual(image["debugLinkCrc32"], f"{crc:08x}")
            if READELF:
                self.assertEqual(image["buildId"], self._readelf_build_id(path))
                self.assertEqual(len(image["buildId"]), 40)

    def test_separate_symbol_root(self) -> None:
        bin_dir, symbols = self.root / "stage" / "bin", self.root / "symbols"
        app = self._app(bin_dir)
        symbols.mkdir()
        shutil.move(str(app) + ".debug", symbols / "app.debug")
        manifest = self.assert_ok(ManifestRun(self.root, [bin_dir], [symbols]))
        # Paths are relative to the roots' common parent.
        self.assertEqual(manifest["images"][0]["path"], "stage/bin/app")
        self.assertEqual(manifest["images"][0]["symbols"]["path"], "symbols/app.debug")

    def test_nested_symbol_root_inside_image_root(self) -> None:
        prefix = self.root / "prefix"
        app = self._app(prefix / "bin")
        (prefix / "symbols").mkdir()
        shutil.move(str(app) + ".debug", prefix / "symbols" / "app.debug")
        manifest = self.assert_ok(ManifestRun(self.root, [prefix], [prefix / "symbols"]))
        self.assertEqual(manifest["images"][0]["path"], "bin/app")

    def test_symbols_left_in_runtime_tree_fail(self) -> None:
        bin_dir, symbols = self.root / "bin", self.root / "symbols"
        self._app(bin_dir)
        symbols.mkdir()
        self.assert_fails(ManifestRun(self.root, [bin_dir], [symbols]), "private symbols in the runtime tree")

    def test_missing_symbols_fail(self) -> None:
        bin_dir = self.root / "bin"
        app = self._app(bin_dir)
        Path(str(app) + ".debug").unlink()
        self.assert_fails(ManifestRun(self.root, [bin_dir], [bin_dir]), "0 symbol files for build-id")

    def test_duplicate_symbols_fail(self) -> None:
        bin_dir = self.root / "bin"
        app = self._app(bin_dir)
        shutil.copy(str(app) + ".debug", bin_dir / "copy.debug")
        self.assert_fails(ManifestRun(self.root, [bin_dir], [bin_dir]), "2 symbol files for build-id")

    def test_mismatched_and_orphaned_symbols_fail(self) -> None:
        bin_dir, other = self.root / "bin", self.root / "other"
        app = self._app(bin_dir, marker=1)
        self._app(other, marker=2)
        shutil.copy(other / "app.debug", str(app) + ".debug")
        run = ManifestRun(self.root, [bin_dir], [bin_dir])
        self.assert_fails(run, "0 symbol files for build-id")
        self.assertIn("app.debug: symbol file maps to no image", run.result.stderr)

    def test_debuglink_crc_mismatch_fails(self) -> None:
        bin_dir = self.root / "bin"
        app = self._app(bin_dir)
        with open(str(app) + ".debug", "ab") as handle:
            handle.write(b"\x00")
        self.assert_fails(ManifestRun(self.root, [bin_dir], [bin_dir]), "does not match the .gnu_debuglink CRC-32")

    def test_debuglink_name_mismatch_fails(self) -> None:
        bin_dir = self.root / "bin"
        app = self._app(bin_dir)
        shutil.move(str(app) + ".debug", bin_dir / "renamed.debug")
        self.assert_fails(ManifestRun(self.root, [bin_dir], [bin_dir]), ".gnu_debuglink names app.debug")

    def test_unstripped_image_fails(self) -> None:
        bin_dir = self.root / "bin"
        self._app(bin_dir, split=False)
        self.assert_fails(ManifestRun(self.root, [bin_dir], [bin_dir]), "still carries debug info or a symbol table")

    def test_image_without_build_id_fails(self) -> None:
        bin_dir = self.root / "bin"
        self._app(bin_dir, build_id="none")
        self.assert_fails(ManifestRun(self.root, [bin_dir], [bin_dir]), "has no GNU build-id")

    def test_identical_copies_share_one_symbol_file(self) -> None:
        bin_dir = self.root / "bin"
        app = self._app(bin_dir)
        shutil.copy(app, bin_dir / "app-copy")
        manifest = self.assert_ok(ManifestRun(self.root, [bin_dir], [bin_dir]))
        self.assertEqual({image["symbols"]["path"] for image in manifest["images"]}, {"app.debug"})
        self.assertEqual(len(manifest["images"]), 2)

    def test_equal_names_under_two_roots_stay_distinct(self) -> None:
        bin_dir, lib_dir = self.root / "bin", self.root / "lib"
        library = self._library(lib_dir)
        bin_dir.mkdir()
        shutil.copy(library, bin_dir / library.name)
        manifest = self.assert_ok(ManifestRun(self.root, [bin_dir, lib_dir], [bin_dir, lib_dir]))
        self.assertEqual([image["path"] for image in manifest["images"]], ["bin/libfixture.so", "lib/libfixture.so"])
        self.assertEqual({image["symbols"]["path"] for image in manifest["images"]}, {"lib/libfixture.so.debug"})

    def test_symlinks_are_not_artifacts(self) -> None:
        bin_dir = self.root / "bin"
        library = self._library(bin_dir)
        (bin_dir / "libfixture.so.1").symlink_to(library.name)
        manifest = self.assert_ok(ManifestRun(self.root, [bin_dir], [bin_dir]))
        self.assertEqual([image["path"] for image in manifest["images"]], ["libfixture.so"])

    def test_malformed_elf_is_an_error(self) -> None:
        bin_dir = self.root / "bin"
        app = self._app(bin_dir)
        (bin_dir / "broken").write_bytes(app.read_bytes()[:200])
        self.assert_fails(ManifestRun(self.root, [bin_dir], [bin_dir]), "broken:")

    def test_launcher_propagates_link_failure(self) -> None:
        output = self.root / "never"
        result = _run(CMAKE, f"-DSPARK_OBJCOPY={OBJCOPY}", "-P", LAUNCHER, "--", GCC, "missing.o", "-o", output)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())
        self.assertFalse(Path(str(output) + ".debug").exists())

    def test_launcher_requires_an_output(self) -> None:
        obj = self._object("app", "int main(void) { return 0; }\n")
        result = _run(CMAKE, f"-DSPARK_OBJCOPY={OBJCOPY}", "-P", LAUNCHER, "--", GCC, obj, cwd=self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no '-o <output>'", result.stderr)


@unittest.skipUnless(PE_TOOLS, "needs clang and lld-link")
class PeSymbolTests(ToolTestCase):
    def _dll(self, directory: Path, name: str, value: int = 3, pdb_alt_path: bool = True) -> Path:
        directory.mkdir(parents=True, exist_ok=True)
        source = self.root / f"{name}{value}.c"
        source.write_text(f"__declspec(dllexport) int Fixture(int v) {{ return v * {value}; }}\n", encoding="utf-8")
        obj = self.root / f"{name}{value}.obj"
        _check(CLANG, "--target=x86_64-pc-windows-msvc", "-g", "-gcodeview", "-O2", "-c", source, "-o", obj)
        dll = directory / f"{name}.dll"
        link = [LLD_LINK, "/DLL", "/NODEFAULTLIB", "/NOENTRY", "/DEBUG", obj]
        link += [f"/OUT:{dll}", f"/PDB:{dll.with_suffix('.pdb')}"]
        if pdb_alt_path:
            link.append("/PDBALTPATH:%_PDB%")
        _check(*link)
        return dll

    def test_dll_maps_to_its_pdb(self) -> None:
        bin_dir = self.root / "bin"
        dll = self._dll(bin_dir, "fixture")
        manifest = self.assert_ok(ManifestRun(self.root, [bin_dir], [bin_dir]))
        (image,) = manifest["images"]
        self.assertEqual((image["format"], image["machine"], image["pdbName"]), ("pe", "x86-64", "fixture.pdb"))
        self.assertEqual(image["symbols"]["path"], "fixture.pdb")
        self.assertEqual(image["symbolKey"], image["pdbGuid"].replace("-", "") + f"{image['pdbAge']:X}")
        if LLVM_READOBJ:
            directory = _check(LLVM_READOBJ, "--coff-debug-directory", dll)
            guid = re.search(r"PDBGUID: \(([0-9A-F ]+)\)", directory)
            assert guid is not None
            raw = bytes.fromhex(guid.group(1).replace(" ", ""))
            self.assertEqual(image["pdbGuid"], str(tool.uuid.UUID(bytes_le=raw)).upper())
            self.assertIn(f"PDBAge: {image['pdbAge']}", directory)
            self.assertIn("PDBFileName: fixture.pdb", directory)
        if LLVM_PDBUTIL:
            summary = _check(LLVM_PDBUTIL, "dump", "--summary", dll.with_suffix(".pdb"))
            self.assertIn("{" + image["pdbGuid"] + "}", summary)
            self.assertRegex(summary, rf"Age: {image['pdbAge']}\b")

    def test_absolute_pdb_path_fails(self) -> None:
        bin_dir = self.root / "bin"
        self._dll(bin_dir, "fixture", pdb_alt_path=False)
        self.assert_fails(ManifestRun(self.root, [bin_dir], [bin_dir]), "is not a bare file name")

    def test_pdb_from_another_link_fails(self) -> None:
        bin_dir, other = self.root / "bin", self.root / "other"
        self._dll(bin_dir, "fixture", value=3)
        self._dll(other, "fixture", value=5)
        shutil.copy(other / "fixture.pdb", bin_dir / "fixture.pdb")
        run = ManifestRun(self.root, [bin_dir], [bin_dir])
        self.assert_fails(run, "0 PDBs for GUID")
        self.assertIn("fixture.pdb: symbol file maps to no image", run.result.stderr)

    def test_pdb_in_runtime_tree_fails(self) -> None:
        bin_dir, symbols = self.root / "bin", self.root / "symbols"
        self._dll(bin_dir, "fixture")
        symbols.mkdir()
        self.assert_fails(ManifestRun(self.root, [bin_dir], [symbols]), "private symbols in the runtime tree")

    def test_separate_symbol_root(self) -> None:
        bin_dir, symbols = self.root / "bin", self.root / "symbols"
        self._dll(bin_dir, "fixture")
        symbols.mkdir()
        shutil.move(bin_dir / "fixture.pdb", symbols / "fixture.pdb")
        self.assert_ok(ManifestRun(self.root, [bin_dir], [symbols]))

    def test_truncated_pe_is_an_error(self) -> None:
        bin_dir = self.root / "bin"
        dll = self._dll(bin_dir, "fixture")
        (bin_dir / "broken.dll").write_bytes(dll.read_bytes()[:600])
        self.assert_fails(ManifestRun(self.root, [bin_dir], [bin_dir]), "broken.dll:")


class ManifestContractTests(ToolTestCase):
    def test_empty_roots_fail(self) -> None:
        (self.root / "bin").mkdir()
        self.assert_fails(ManifestRun(self.root, [self.root / "bin"], [self.root / "bin"]), "no ELF or PE images")

    def test_missing_root_is_a_usage_error(self) -> None:
        run = ManifestRun(self.root, [self.root / "absent"], [self.root])
        self.assertEqual(run.result.returncode, 2)

    def test_schema_is_closed(self) -> None:
        image = {
            "path": "app",
            "format": "elf",
            "machine": "x86-64",
            "sha256": "0" * 64,
            "symbolKey": "ab" * 20,
            "buildId": "ab" * 20,
            "debugLinkCrc32": "00000000",
            "symbols": {"path": "app.debug", "sha256": "1" * 64},
        }
        tool.validate_manifest({"schema": tool.SCHEMA, "images": [image]})
        rejected = [
            {"schema": tool.SCHEMA, "images": [image], "extra": 1},
            {"schema": "other", "images": [image]},
            {"schema": tool.SCHEMA, "images": []},
            {"schema": tool.SCHEMA, "images": [dict(image, extra=1)]},
            {"schema": tool.SCHEMA, "images": [dict(image, format="pe")]},
            {"schema": tool.SCHEMA, "images": [dict(image, symbols={"path": "app.debug"})]},
            {"schema": tool.SCHEMA, "images": [dict(image, path="b"), dict(image, path="a")]},
            {"schema": tool.SCHEMA, "images": [image, image]},
        ]
        for manifest in rejected:
            with self.subTest(manifest=manifest), self.assertRaises(ValueError):
                tool.validate_manifest(manifest)


def _cmake_calls(text: str, command: str) -> list[str]:
    text = re.sub(r"#[^\n]*", "", text)
    calls = []
    for match in re.finditer(rf"\b{command}\s*\(", text):
        depth, index = 1, match.end()
        while depth:
            depth += {"(": 1, ")": -1}.get(text[index], 0)
            index += 1
        calls.append(text[match.end() : index - 1])
    return calls


INSTALL_KEYWORDS = {
    "EXPORT", "RUNTIME", "LIBRARY", "ARCHIVE", "FRAMEWORK", "BUNDLE", "INCLUDES",
    "DESTINATION", "COMPONENT", "PUBLIC_HEADER", "PRIVATE_HEADER", "RESOURCE",
}  # fmt: skip


class SymbolProductionContractTests(unittest.TestCase):
    """The root CMake wiring that produces the symbols the tool maps."""

    def setUp(self) -> None:
        self.text = re.sub(r"#[^\n]*", "", ROOT_CMAKE.read_text(encoding="utf-8"))

    def test_msvc_always_links_with_a_relative_pdb_reference(self) -> None:
        link = "add_link_options(/DEBUG $<$<NOT:$<CONFIG:Debug>>:/PDBALTPATH:%_PDB%>)"
        self.assertEqual(self.text.count(link), 1)
        # /DEBUG must not be conditional on STRIP_DEBUG_SYMBOLS again.
        conditional = r"if\(STRIP_DEBUG_SYMBOLS\)[^\n]*\n[^\n]*\n\s*else\(\)\s*add_link_options\(/DEBUG"
        self.assertNotRegex(self.text, conditional)

    def test_elf_links_carry_a_build_id_and_split_symbols(self) -> None:
        self.assertIn("add_link_options(-Wl,--build-id=sha1)", self.text)
        self.assertIn('-P "${CMAKE_SOURCE_DIR}/cmake/SparkSplitDebugLink.cmake" --', self.text)
        self.assertRegex(self.text, r"if\(STRIP_DEBUG_SYMBOLS AND CMAKE_EXECUTABLE_FORMAT STREQUAL \"ELF\"\)")

    def test_only_shipped_images_link_through_the_split_launcher(self) -> None:
        # A global launcher would split (and, with -g, bloat) SparkTests and every probe too.
        self.assertNotRegex(self.text, r"set\(CMAKE_(C|CXX)_LINKER_LAUNCHER")
        self.assertIn("PROPERTY ${_spark_symbol_language}_LINKER_LAUNCHER", self.text)
        self.assertIn("set_property(TARGET ${_spark_symbol_target} PROPERTY SPARK_PRIVATE_SYMBOLS ON)", self.text)
        # Every other image drops LTO debug generation and is stripped at link.
        unshipped = "$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:$<TARGET_PROPERTY:SPARK_PRIVATE_SYMBOLS>>>>"
        self.assertIn(unshipped, self.text)
        self.assertIn('"$<${_spark_unshipped_release_link}:-g0>"', self.text)
        self.assertIn('"$<${_spark_unshipped_release_link}:-Wl,--strip-all>"', self.text)

    def test_symbols_component_is_never_packaged(self) -> None:
        # The root list and every per-generator override (cmake/SparkCPackOptions.cmake).
        component_lists = [
            (cmake_file, call.split()[1:])
            for cmake_file in [ROOT_CMAKE, *sorted((REPO_ROOT / "cmake").glob("*.cmake"))]
            for call in _cmake_calls(cmake_file.read_text(encoding="utf-8"), "set")
            if call.split() and call.split()[0] == "CPACK_COMPONENTS_ALL"
        ]
        files = {cmake_file.name for cmake_file, _ in component_lists}
        self.assertEqual(files, {"CMakeLists.txt", "SparkCPackOptions.cmake"}, "CPACK_COMPONENTS_ALL parsing drifted")
        for cmake_file, components in component_lists:
            for component in components:
                self.assertNotIn(
                    component.lower(),
                    {"symbols", "${spark_install_component_symbols}"},
                    f"{cmake_file.name} packages the private symbols component",
                )
        self.assertIn('set(SPARK_INSTALL_COMPONENT_SYMBOLS "symbols")', self.text)


class ShippedImageListTests(unittest.TestCase):
    def test_symbol_list_names_every_installed_image(self) -> None:
        installed: set[str] = set()
        first_party = [
            ROOT_CMAKE,
            *sorted(REPO_ROOT.glob("Spark*/**/CMakeLists.txt")),
            *sorted(REPO_ROOT.glob("GameModules/**/CMakeLists.txt")),
            *sorted(REPO_ROOT.glob("cmake/**/*.cmake")),
        ]
        for cmake_file in first_party:
            for call in _cmake_calls(cmake_file.read_text(encoding="utf-8"), "install"):
                tokens = call.split()
                if not tokens or tokens[0] != "TARGETS" or not {"RUNTIME", "LIBRARY"} & set(tokens):
                    continue
                for token in tokens[1:]:
                    if token in INSTALL_KEYWORDS:
                        break
                    installed.add(token)
        # SparkEngineLib and its static dependencies install as SDK archives.
        installed.discard("${_SPARK_INSTALL_TARGETS}")
        # The per-module install loop iterates _SPARK_GAME_MODULE_NAMES.
        if "${_SPARK_GAME_MODULE}" in installed:
            installed.remove("${_SPARK_GAME_MODULE}")
            installed.add("${_SPARK_GAME_MODULE_NAMES}")

        (listing,) = [
            call for call in _cmake_calls(ROOT_CMAKE.read_text(encoding="utf-8"), "set")
            if call.split()[0] == "SPARK_SHIPPED_IMAGE_TARGETS"
        ]  # fmt: skip
        shipped = set(listing.split()[1:])
        self.assertIn("SparkEngine", installed, "install(TARGETS) parsing found nothing")
        self.assertEqual(sorted(installed - shipped), [], "installed images without private symbols")
        self.assertEqual(sorted(shipped - installed), [], "symbol list names targets that are not installed")


if __name__ == "__main__":
    unittest.main()
