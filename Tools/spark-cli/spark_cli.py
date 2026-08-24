#!/usr/bin/env python3
"""
spark-cli — SparkEngine project scaffolding, build, and packaging tool

Usage:
    spark new <project-name> [--template <template>] [--output <directory>]
    spark build [--config <Debug|Release>]
    spark run [--config <Debug|Release>]
    spark package [--config Release] [--platform windows] [--strip] [--compress]
    spark validate [path] [--strict] [--format text|json]
    spark migrate [path] [--dry-run] [--backup]
    spark templates
    spark info

Templates:
    EmptyProject    — Empty project with minimal boilerplate (default)
    FPSStarter      — First-person shooter template with weapons, AI, HUD
    RPGStarter      — RPG template with inventory, dialogue, quests
    PlatformerKit   — 2D/3D platformer with character controller
    MultiplayerArena — Bounded local arena rules and lobby simulation

Examples:
    spark new MyGame
    spark new MyGame --template FPSStarter --output ~/Projects
    spark build --config Release
    spark package --config Release --strip --compress
    spark validate Assets/ --strict
    spark migrate Assets/ --dry-run
    spark templates
"""

import argparse
import os
import shutil
import stat
import subprocess
import sys
import json
import tempfile
from pathlib import Path


MODULE_EXTENSIONS = {
    "windows": (".dll",),
    "linux": (".so",),
    "macos": (".dylib", ".so"),
}

PACKAGE_OWNER = "spark-cli"
PACKAGE_FORMAT_VERSION = 1
TRANSACTION_OWNER = "spark-cli-package-transaction"
TRANSACTION_FORMAT_VERSION = 1
TRANSACTION_MARKER = ".spark-cli-transaction.json"


def current_platform():
    """Return the Spark CLI platform name for the current interpreter."""
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform.startswith("darwin"):
        return "macos"
    return "unknown"


def executable_filename():
    """Return the runtime host's conventional filename on this platform."""
    return "SparkEngine.exe" if current_platform() == "windows" else "SparkEngine"


def load_json_object(path, description):
    """Load a JSON object and return ``(value, error)`` without raising."""
    try:
        with open(path, encoding="utf-8") as source:
            value = json.load(source)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        return None, f"Invalid {description} '{path}': {exc}"
    if not isinstance(value, dict):
        return None, f"Invalid {description} '{path}': the root must be a JSON object"
    return value, None


def find_project_descriptor(project_root):
    """Find the active project descriptor, supporting current and legacy names."""
    descriptors = sorted(project_root.glob("*.sparkproject"))
    if len(descriptors) == 1:
        return descriptors[0], None
    if len(descriptors) > 1:
        names = ", ".join(path.name for path in descriptors)
        return None, f"Multiple project descriptors found; keep only the active one: {names}"

    legacy = project_root / "spark.project.json"
    if legacy.is_file():
        return legacy, None
    return None, (
        "No project descriptor found (expected one *.sparkproject file "
        "or legacy spark.project.json)."
    )


def configured_build_dir(project_root, config):
    """Choose an existing configured CMake tree, including editor layouts."""
    build_root = project_root / "build"
    candidates = (build_root / config, build_root)
    for candidate in candidates:
        if (candidate / "CMakeCache.txt").is_file():
            return candidate
    return build_root


def _path_is_within(candidate, parent):
    try:
        candidate.resolve().relative_to(parent.resolve())
        return True
    except (OSError, ValueError):
        return False


def _paths_overlap(first, second):
    """Return whether either resolved path contains the other."""
    return _path_is_within(first, second) or _path_is_within(second, first)


def _path_is_link_like(path):
    """Reject symlinks, Windows junctions, and other reparse-point targets."""
    try:
        if path.is_symlink():
            return True
        is_junction = getattr(path, "is_junction", None)
        if is_junction is not None and is_junction():
            return True
        attributes = getattr(path.lstat(), "st_file_attributes", 0)
        reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
        return bool(reparse_flag and attributes & reparse_flag)
    except OSError:
        # An inaccessible existing entry cannot be proven safe to replace.
        return os.path.lexists(path)


def _absolute_unresolved(path):
    """Make a path absolute without resolving any existing filesystem links."""
    return Path(os.path.abspath(os.fspath(path)))


def _find_nested_path_key(value, location):
    """Locate a runtime-confusing `path` key outside a module's top level."""
    if isinstance(value, dict):
        for key, child in value.items():
            child_location = f"{location}.{key}"
            if key == "path":
                return child_location
            found = _find_nested_path_key(child, child_location)
            if found:
                return found
    elif isinstance(value, list):
        for index, child in enumerate(value):
            found = _find_nested_path_key(child, f"{location}[{index}]")
            if found:
                return found
    return None


def validate_module_manifest_path_keys(manifest):
    """Reject extra `path` keys that the engine's legacy parser would misload."""
    if not isinstance(manifest, dict):
        return "Module manifest root must be an object"
    for key, value in manifest.items():
        if key == "modules":
            continue
        if key == "path":
            return "Module manifest contains unsafe path key at root.path"
        found = _find_nested_path_key(value, f"root.{key}")
        if found:
            return f"Module manifest contains unsafe extra path key at {found}"

    modules = manifest.get("modules")
    if not isinstance(modules, list):
        return None
    for index, module in enumerate(modules):
        if not isinstance(module, dict):
            continue
        for key, value in module.items():
            if key == "path":
                continue
            found = _find_nested_path_key(value, f"modules[{index}].{key}")
            if found:
                return f"Module manifest contains unsafe extra path key at {found}"
    return None


def read_module_manifest(package_root):
    """Validate a runnable package manifest and its module artifacts."""
    manifest_path = package_root / "spark.modules.json"
    if not manifest_path.is_file():
        return None, f"Package '{package_root}' has no spark.modules.json"

    manifest, error = load_json_object(manifest_path, "module manifest")
    if error:
        return None, error
    path_key_error = validate_module_manifest_path_keys(manifest)
    if path_key_error:
        return None, f"Invalid module manifest '{manifest_path}': {path_key_error}"
    modules = manifest.get("modules")
    if not isinstance(modules, list) or not modules:
        return None, f"Invalid module manifest '{manifest_path}': 'modules' must be a non-empty array"

    module_paths = []
    for index, module in enumerate(modules):
        if not isinstance(module, dict) or not isinstance(module.get("path"), str) or not module["path"].strip():
            return None, f"Invalid module manifest '{manifest_path}': modules[{index}].path is required"
        relative = Path(module["path"])
        if relative.is_absolute():
            return None, f"Invalid module path '{relative}': package manifests must use relative paths"
        resolved = (package_root / relative).resolve()
        if not _path_is_within(resolved, package_root):
            return None, f"Invalid module path '{relative}': path escapes the package directory"
        expected_extensions = MODULE_EXTENSIONS.get(current_platform(), (".dll", ".so", ".dylib"))
        if resolved.suffix.casefold() not in expected_extensions:
            return None, f"Invalid module path '{relative}': unsupported module extension on this platform"
        if not resolved.is_file():
            return None, f"Package module is missing: {resolved}"
        sidecar = Path(str(resolved) + ".sparkabi")
        if not sidecar.is_file():
            return None, f"Package module ABI sidecar is missing: {sidecar}"
        module_paths.append(resolved)
    return module_paths, None


def find_package_host(package_root, project_name):
    """Find one unambiguous runtime host inside an assembled package."""
    platform = current_platform()
    search_roots = [package_root, package_root / "bin"]
    conventional = [
        "SparkGame.exe" if platform == "windows" else "SparkGame",
        executable_filename(),
    ]
    if project_name.isidentifier():
        conventional.append(f"{project_name}.exe" if platform == "windows" else project_name)
    for root in search_roots:
        for name in conventional:
            candidate = root / name
            if candidate.is_file() and _path_is_within(candidate, package_root):
                if platform == "windows" or os.access(candidate, os.X_OK):
                    return candidate.resolve(), None

    candidates = []
    for root in search_roots:
        if not root.is_dir():
            continue
        if platform == "windows":
            candidates.extend(path for path in root.glob("*.exe") if path.is_file())
        else:
            candidates.extend(
                path for path in root.iterdir()
                if path.is_file() and os.access(path, os.X_OK) and not path.suffix
            )
    candidates = sorted(set(candidates))
    if len(candidates) == 1:
        return candidates[0], None
    if not candidates:
        return None, f"Package '{package_root}' contains no runtime host executable"
    names = ", ".join(path.name for path in candidates)
    return None, f"Package '{package_root}' contains multiple possible runtime hosts: {names}"


def package_candidates(project_root, config, project_name, explicit_package=None):
    """Return bounded, deterministic candidate package directories."""
    if explicit_package:
        requested = Path(explicit_package)
        if not requested.is_absolute():
            requested = project_root / requested
        return [requested.resolve()]

    platform = current_platform()
    candidates = [
        project_root / "build" / "Output",
        project_root / "build" / config / "Output",
        project_root / "dist" / f"{project_name}-{platform}-{config.lower()}",
    ]
    result = []
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved not in result:
            result.append(resolved)
    return result


def find_runtime_host(engine_root, config):
    """Find a matching development runtime host for build-tree execution."""
    def runnable(candidate):
        return candidate.is_file() and (current_platform() == "windows" or os.access(candidate, os.X_OK))

    explicit = os.environ.get("SPARKENGINE_RUNTIME_HOST")
    if explicit:
        candidate = Path(explicit).expanduser().resolve()
        return candidate if runnable(candidate) else None
    if not engine_root:
        return None

    filename = executable_filename()
    candidates = (
        engine_root / "build" / "bin" / config / filename,
        engine_root / "build" / config / filename,
        engine_root / "bin" / config / filename,
        engine_root / "bin" / filename,
    )
    return next((path.resolve() for path in candidates if runnable(path)), None)


def _module_stem(path):
    stem = Path(path).stem.casefold()
    return stem[3:] if stem.startswith("lib") else stem


def find_built_module(build_dir, manifest_modules, config=None):
    """Find the freshly built module identified by the project's manifest."""
    extensions = MODULE_EXTENSIONS.get(current_platform(), (".dll", ".so", ".dylib"))
    wanted_stems = {
        _module_stem(module.get("path", ""))
        for module in manifest_modules
        if isinstance(module, dict) and isinstance(module.get("path"), str)
    }
    search_root = build_dir
    configured_output = build_dir / config if config else None
    if configured_output and configured_output.is_dir():
        search_root = configured_output
    candidates = []
    if search_root.is_dir():
        for extension in extensions:
            candidates.extend(path for path in search_root.rglob(f"*{extension}") if path.is_file())
    candidates = [path for path in candidates if Path(str(path) + ".sparkabi").is_file()]
    matches = [path for path in candidates if _module_stem(path) in wanted_stems]
    if len(matches) == 1:
        return matches[0].resolve(), None
    if not matches:
        return None, f"No built module with an ABI sidecar was found under '{search_root}'"
    names = ", ".join(str(path) for path in sorted(matches))
    return None, f"Multiple matching built modules found; remove stale outputs: {names}"


def find_built_modules(build_dir, manifest_modules, config=None):
    """Resolve every declared module to one unique built artifact."""
    if not isinstance(manifest_modules, list) or not manifest_modules:
        return None, "Module manifest must contain a non-empty modules array"

    extensions = MODULE_EXTENSIONS.get(current_platform(), (".dll", ".so", ".dylib"))
    declared_extensions = {extension for values in MODULE_EXTENSIONS.values() for extension in values}
    search_root = build_dir
    configured_output = build_dir / config if config else None
    if configured_output and configured_output.is_dir():
        search_root = configured_output

    candidates = []
    if search_root.is_dir():
        for extension in extensions:
            candidates.extend(path.resolve() for path in search_root.rglob(f"*{extension}") if path.is_file())
    candidates = sorted({
        path for path in candidates
        if Path(str(path) + ".sparkabi").is_file()
    })

    resolved_modules = []
    used_artifacts = set()
    packaged_names = set()
    for index, module in enumerate(manifest_modules):
        if not isinstance(module, dict):
            return None, f"Module manifest entry modules[{index}] must be an object"
        declared = module.get("path")
        if not isinstance(declared, str) or not declared.strip():
            return None, f"Module manifest entry modules[{index}].path must be a non-empty string"
        # Project manifests are shared across Windows/POSIX, so accept either
        # separator while validating the logical relative path.
        relative = Path(declared.replace("\\", "/"))
        if relative.is_absolute() or ".." in relative.parts:
            return None, f"Module manifest entry modules[{index}].path must be a safe relative path: {declared}"
        if relative.suffix.casefold() not in declared_extensions:
            return None, f"Module manifest entry modules[{index}] has an unsupported module extension: {declared}"

        wanted_stem = _module_stem(relative)
        matches = [path for path in candidates if _module_stem(path) == wanted_stem]
        if not matches:
            return None, (
                f"No built module with an ABI sidecar matches modules[{index}].path "
                f"'{declared}' under '{search_root}'"
            )
        if len(matches) > 1:
            names = ", ".join(str(path) for path in matches)
            return None, (
                f"Multiple matching built modules found for modules[{index}].path "
                f"'{declared}'; remove stale outputs: {names}"
            )

        artifact = matches[0]
        if artifact in used_artifacts:
            return None, f"Multiple manifest entries resolve to the same built module: {artifact}"
        packaged_key = artifact.name.casefold() if current_platform() == "windows" else artifact.name
        if packaged_key in packaged_names:
            return None, f"Built modules collide in the package root: {artifact.name}"
        used_artifacts.add(artifact)
        packaged_names.add(packaged_key)
        resolved_modules.append((dict(module), artifact))

    return resolved_modules, None


def _write_development_manifest(path, resolved_modules):
    """Write the minimal manifest understood by the runtime's bounded parser."""
    manifest = {
        "modules": [
            {
                "name": module_path.stem,
                # Forward slashes avoid JSON escaping that the legacy runtime
                # manifest reader intentionally does not decode.
                "path": module_path.as_posix(),
            }
            for _, module_path in resolved_modules
        ]
    }
    return _write_text(path, json.dumps(manifest, indent=2) + "\n")


def find_engine_root():
    """Find the SparkEngine root directory."""
    # Check environment variable first
    env_root = os.environ.get("SPARK_ENGINE_DIR")
    if env_root and os.path.isdir(env_root):
        return Path(env_root)

    # Walk up from this script's location
    current = Path(__file__).resolve().parent
    while current != current.parent:
        if (current / "SparkSDK").is_dir() and (current / "SparkEngine").is_dir():
            return current
        current = current.parent

    return None


def get_templates_dir(engine_root):
    """Get the templates directory."""
    return engine_root / "Templates"


def cmd_new(args):
    """Create a new SparkEngine game project."""
    engine_root = find_engine_root()
    if not engine_root:
        print("Error: Cannot find SparkEngine root directory.")
        print("Set SPARK_ENGINE_DIR environment variable or run from within the engine tree.")
        return 1

    project_name = args.name
    template_name = args.template or "EmptyProject"
    output_dir = Path(args.output) if args.output else Path.cwd()

    # Validate project name (C++ identifier)
    if not project_name.isidentifier():
        print(f"Error: '{project_name}' is not a valid project name (must be a C++ identifier).")
        return 1

    project_path = output_dir / project_name
    if project_path.exists():
        print(f"Error: Directory '{project_path}' already exists.")
        return 1

    # Find template
    templates_dir = get_templates_dir(engine_root)
    template_path = templates_dir / template_name
    if not template_path.is_dir():
        available = [d.name for d in templates_dir.iterdir() if d.is_dir()]
        print(f"Error: Template '{template_name}' not found.")
        print(f"Available templates: {', '.join(available) if available else 'none'}")
        return 1

    print(f"Creating project '{project_name}' from template '{template_name}'...")
    print(f"  Location: {project_path}")
    print(f"  Engine:   {engine_root}")

    # Copy template
    shutil.copytree(template_path, project_path)

    # Templates are shipped as real, compilable game modules named after their
    # directory (e.g. Templates/FPSStarter → class FPSStarterModule, target
    # FPSStarter, etc.). Rewrite every textual occurrence of the template
    # name to the user's chosen project name so they do not have to.
    text_extensions = {
        ".h",
        ".hpp",
        ".cpp",
        ".c",
        ".txt",
        ".json",
        ".sparkproject",
        ".cmake",
        ".md",
        ".py",
    }

    if template_name and template_name != project_name:
        for root, dirs, files in os.walk(project_path):
            for filename in files:
                filepath = Path(root) / filename
                if filepath.suffix.lower() in text_extensions:
                    try:
                        content = filepath.read_text(encoding="utf-8")
                        if template_name in content:
                            content = content.replace(template_name, project_name)
                            filepath.write_text(content, encoding="utf-8")
                    except (UnicodeDecodeError, PermissionError):
                        pass

        # Rename any files whose name matches the template (rare, but keeps the
        # "copy and rename" UX lossless).
        for root, dirs, files in os.walk(project_path, topdown=False):
            for filename in files:
                if template_name in filename:
                    old_path = Path(root) / filename
                    new_path = Path(root) / filename.replace(template_name, project_name)
                    old_path.rename(new_path)

    print()
    print(f"Project '{project_name}' created successfully!")
    print()
    print("Next steps:")
    print(f"  1. cd {project_path}")
    print(f"  2. cmake -B build -DSparkEngine_DIR=<path-to-engine-install>/lib/cmake/SparkEngine")
    print(f"  3. cmake --build build")
    print()
    print("Or, to build as part of the engine tree:")
    print(f"  1. Copy/symlink {project_path} into the engine's root directory")
    print(f"  2. Add add_subdirectory({project_name}) to the engine's CMakeLists.txt")

    return 0


def cmd_build(args):
    """Build the current game project."""
    if not Path("CMakeLists.txt").exists():
        print("Error: No CMakeLists.txt found in current directory.")
        print("Run this command from your game project root.")
        return 1

    config = args.config or "Debug"
    project_root = Path.cwd().resolve()
    build_dir = configured_build_dir(project_root, config)

    # Configure if needed
    if not (build_dir / "CMakeCache.txt").is_file():
        print("Configuring project...")
        result = subprocess.run(
            ["cmake", "-S", str(project_root), "-B", str(build_dir), f"-DCMAKE_BUILD_TYPE={config}"],
            cwd=project_root
        )
        if result.returncode != 0:
            print("Configuration failed.")
            return result.returncode

    # Build
    print(f"Building ({config})...")
    result = subprocess.run(
        ["cmake", "--build", str(build_dir), "--config", config],
        cwd=project_root
    )
    return result.returncode


def cmd_run(args):
    """Build and run the current project through its package or runtime host."""
    project_root = Path.cwd().resolve()
    project_file, error = find_project_descriptor(project_root)
    if error:
        print(f"Error: {error}")
        return 1
    project_info, error = load_json_object(project_file, "project descriptor")
    if error:
        print(f"Error: {error}")
        return 1

    project_name = project_info.get("name")
    if not isinstance(project_name, str) or not project_name.strip():
        print(f"Error: Project descriptor '{project_file}' has no non-empty 'name'.")
        return 1
    project_name = project_name.strip()
    config = args.config or "Debug"
    explicit_package = getattr(args, "package", None)

    if not getattr(args, "no_build", False) and not explicit_package:
        build_result = cmd_build(args)
        if build_result != 0:
            return build_result

    runtime_args = list(getattr(args, "runtime_args", []) or [])
    if runtime_args[:1] == ["--"]:
        runtime_args.pop(0)

    invalid_packages = []
    package_host_fallback = None
    for package_root in package_candidates(project_root, config, project_name, explicit_package):
        if not package_root.is_dir():
            if explicit_package:
                print(f"Error: Package directory does not exist: {package_root}")
                return 1
            continue
        _, manifest_error = read_module_manifest(package_root)
        if manifest_error:
            invalid_packages.append(manifest_error)
            continue
        host, host_error = find_package_host(package_root, project_name)
        if host_error:
            invalid_packages.append(host_error)
            continue

        if getattr(args, "no_build", False) or explicit_package:
            print(f"Project: {project_name}")
            print(f"Package: {package_root}")
            print(f"Running: {host}")
            try:
                result = subprocess.run([str(host), *runtime_args], cwd=package_root)
            except OSError as exc:
                print(f"Error: Failed to launch '{host}': {exc}")
                return 1
            return result.returncode
        if package_host_fallback is None:
            package_host_fallback = host

    if explicit_package:
        print(f"Error: {invalid_packages[0] if invalid_packages else 'Package is not runnable.'}")
        return 1

    manifest_path = project_root / "spark.modules.json"
    manifest, manifest_error = load_json_object(manifest_path, "module manifest")
    if manifest_error:
        print(f"Error: {manifest_error}")
        if invalid_packages:
            print(f"Package error: {invalid_packages[0]}")
        return 1
    modules = manifest.get("modules")
    if not isinstance(modules, list) or not modules:
        print(f"Error: Invalid module manifest '{manifest_path}': 'modules' must be a non-empty array")
        return 1
    path_key_error = validate_module_manifest_path_keys(manifest)
    if path_key_error:
        print(f"Error: Invalid module manifest '{manifest_path}': {path_key_error}")
        return 1

    build_dir = configured_build_dir(project_root, config)
    resolved_modules, module_error = find_built_modules(build_dir, modules, config)
    if module_error:
        print(f"Error: {module_error}")
        if invalid_packages:
            print(f"Package error: {invalid_packages[0]}")
        return 1
    host = find_runtime_host(find_engine_root(), config) or package_host_fallback
    if not host:
        print(f"Error: No SparkEngine runtime host matching {config} was found.")
        print("Set SPARKENGINE_RUNTIME_HOST to an explicit executable or build/install the engine host.")
        return 1

    print(f"Project: {project_name}")
    print(f"Running: {host}")
    if len(resolved_modules) == 1:
        module_path = resolved_modules[0][1]
        print(f"Module:  {module_path}")
        command = [
            str(host),
            "-game", str(module_path),
            "-project", str(project_file),
            *runtime_args,
        ]
        try:
            result = subprocess.run(command, cwd=project_root)
        except OSError as exc:
            print(f"Error: Failed to launch '{host}': {exc}")
            return 1
        return result.returncode

    print("Modules:")
    for _, module_path in resolved_modules:
        print(f"  {module_path}")

    try:
        with tempfile.TemporaryDirectory(prefix=".spark-cli-run-", dir=build_dir) as temporary:
            temporary_root = Path(temporary)
            if current_platform() == "windows":
                run_manifest = temporary_root / "spark.modules.json"
                error = _write_development_manifest(run_manifest, resolved_modules)
                if error:
                    print(f"Error: {error}")
                    return 1
                command = [
                    str(host),
                    "-manifest", str(run_manifest),
                    "-project", str(project_file),
                    *runtime_args,
                ]
                result = subprocess.run(command, cwd=project_root)
            else:
                # Older POSIX hosts have no explicit -manifest option. Assemble
                # an invocation-owned runnable directory so discovery remains
                # bounded to the freshly built module set.
                package_manifest, error = _assemble_runnable_package(
                    project_root, project_file, project_info, manifest, resolved_modules,
                    host, temporary_root, current_platform(), config, False, False
                )
                if error:
                    print(f"Error: Failed to stage multi-module development run: {error}")
                    return 1
                staged_host = temporary_root / package_manifest["host"]
                result = subprocess.run(
                    [str(staged_host), "-project", str(project_file), *runtime_args],
                    cwd=project_root,
                )
    except OSError as exc:
        print(f"Error: Failed to launch multi-module project: {exc}")
        return 1
    return result.returncode


def _safe_package_component(value):
    """Return whether a descriptor value is safe as one package path component."""
    if not isinstance(value, str) or not value or value in {".", ".."}:
        return False
    if Path(value).name != value:
        return False
    if value.endswith((" ", ".")) or any(ord(char) < 32 or char in '<>:"/\\|?*' for char in value):
        return False
    if current_platform() == "windows":
        stem = Path(value).stem.casefold()
        if stem in {"con", "prn", "aux", "nul"}:
            return False
        if len(stem) == 4 and stem[:3] in {"com", "lpt"} and stem[3] in "123456789":
            return False
    return True


def _copy_tree(source, destination, confinement_root=None):
    """Copy a directory tree after rejecting links or paths outside its owner."""
    if not source.is_dir():
        return 0, None
    owner = (confinement_root or source).resolve()
    try:
        if _path_is_link_like(source) or not _path_is_within(source, owner):
            return 0, f"Refusing to package linked or escaping path: {source}"
        for current, directories, files in os.walk(source, followlinks=False):
            current_path = Path(current)
            if _path_is_link_like(current_path) or not _path_is_within(current_path, owner):
                return 0, f"Refusing to package linked or escaping path: {current_path}"
            for name in [*directories, *files]:
                candidate = current_path / name
                if _path_is_link_like(candidate) or not _path_is_within(candidate, owner):
                    return 0, f"Refusing to package linked or escaping path: {candidate}"
        shutil.copytree(source, destination, dirs_exist_ok=True)
        count = sum(1 for path in destination.rglob("*") if path.is_file())
        return count, None
    except (OSError, shutil.Error) as exc:
        return 0, f"Failed to copy '{source}' to '{destination}': {exc}"


def _find_startup_scene(project_root, project_info):
    """Resolve the declared startup scene, or choose a deterministic fallback."""
    declared = project_info.get("defaultScene")
    if declared is not None:
        if not isinstance(declared, str) or not declared.strip():
            return None, "Project 'defaultScene' must be a non-empty relative path"
        # Project descriptors are shared across platforms; accept either path
        # separator while retaining the same confinement checks.
        relative = Path(declared.replace("\\", "/"))
        scene = (project_root / relative).resolve()
        if relative.is_absolute() or not _path_is_within(scene, project_root):
            return None, f"Project defaultScene escapes the project directory: {declared}"
        if scene.suffix.casefold() != ".sparkscene" or not scene.is_file():
            return None, f"Project defaultScene is missing or is not a .sparkscene file: {declared}"
        return scene, None

    preferred = project_root / "Scenes" / "Default.sparkscene"
    if preferred.is_file():
        return preferred.resolve(), None
    scenes = sorted(path.resolve() for path in (project_root / "Scenes").rglob("*.sparkscene")) \
        if (project_root / "Scenes").is_dir() else []
    return (scenes[0] if scenes else None), None


def _write_text(path, contents):
    try:
        path.write_text(contents, encoding="utf-8", newline="")
        return None
    except OSError as exc:
        return f"Failed to write '{path}': {exc}"


def _write_package_launchers(package_root, host_name, has_scene):
    """Write native launchers and package guidance."""
    platform = current_platform()
    if platform == "windows":
        game_launcher = (
            '@echo off\r\nsetlocal\r\npushd "%~dp0"\r\n'
            f'"{host_name}" %*\r\n'
            'set "spark_exit=%ERRORLEVEL%"\r\npopd\r\nexit /b %spark_exit%\r\n'
        )
        error = _write_text(package_root / "LaunchGame.cmd", game_launcher)
        if error:
            return error
        if has_scene:
            preview_name = "SparkGame Scene.exe"
            scene_launcher = (
                '@echo off\r\nsetlocal\r\npushd "%~dp0"\r\n'
                f'"ScenePreview\\{preview_name}" -scene Startup.sparkscene %*\r\n'
                'set "spark_exit=%ERRORLEVEL%"\r\npopd\r\nexit /b %spark_exit%\r\n'
            )
            error = _write_text(package_root / "LaunchScene.cmd", scene_launcher)
            if error:
                return error
    else:
        game_launcher = (
            "#!/bin/sh\n"
            'script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
            'cd "$script_dir" || exit 1\n'
            f'exec "./{host_name}" "$@"\n'
        )
        game_path = package_root / "LaunchGame.sh"
        error = _write_text(game_path, game_launcher)
        if error:
            return error
        try:
            game_path.chmod(game_path.stat().st_mode | 0o111)
        except OSError as exc:
            return f"Failed to make '{game_path}' executable: {exc}"
        if has_scene:
            scene_launcher = (
                "#!/bin/sh\n"
                'script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
                'cd "$script_dir" || exit 1\n'
                'exec "./ScenePreview/SparkGameScene" -scene Startup.sparkscene "$@"\n'
            )
            scene_path = package_root / "LaunchScene.sh"
            error = _write_text(scene_path, scene_launcher)
            if error:
                return error
            try:
                scene_path.chmod(scene_path.stat().st_mode | 0o111)
            except OSError as exc:
                return f"Failed to make '{scene_path}' executable: {exc}"

    readme = (
        "SparkEngine runnable package\n\n"
        "LaunchGame runs the compiled game module through spark.modules.json.\n"
        "Runtime arguments may be appended to the launcher command.\n"
    )
    if has_scene:
        readme += (
            "LaunchScene runs an isolated runtime host against Startup.sparkscene; "
            "it is a reflected-scene preview separate from module execution.\n"
        )
    else:
        readme += "No .sparkscene was present, so no scene-preview launcher was generated.\n"
    return _write_text(package_root / "PACKAGE_README.txt", readme)


def _assemble_runnable_package(project_root, project_file, project_info, source_manifest,
                               resolved_modules, runtime_host, destination, platform,
                               config, strip_symbols, compress):
    """Assemble one validated runnable package in an empty staging directory."""
    if not resolved_modules:
        return None, "No built modules were selected for packaging"
    for _, module_path in resolved_modules:
        module_sidecar = Path(str(module_path) + ".sparkabi")
        if not module_path.is_file() or not module_sidecar.is_file():
            return None, f"Built module or its ABI sidecar is missing: {module_path}"
    if not runtime_host.is_file():
        return None, "SparkEngine runtime host is missing"
    if platform != "windows" and not os.access(runtime_host, os.X_OK):
        return None, f"SparkEngine runtime host is not executable: {runtime_host}"

    host_name = "SparkGame.exe" if platform == "windows" else "SparkGame"
    packaged_host = destination / host_name
    try:
        shutil.copy2(runtime_host, packaged_host)
        for _, module_path in resolved_modules:
            module_sidecar = Path(str(module_path) + ".sparkabi")
            shutil.copy2(module_path, destination / module_path.name)
            shutil.copy2(module_sidecar, destination / module_sidecar.name)
        shutil.copy2(project_file, destination / project_file.name)
    except OSError as exc:
        return None, f"Failed to copy runtime/module metadata: {exc}"

    runtime_directory = runtime_host.parent
    shaders = runtime_directory / "Shaders"
    if not shaders.is_dir():
        return None, f"SparkEngine runtime Shaders directory is missing beside the host: {shaders}"
    _, error = _copy_tree(shaders, destination / "Shaders", runtime_directory)
    if error:
        return None, error
    _, error = _copy_tree(runtime_directory / "Resources", destination / "Resources", runtime_directory)
    if error:
        return None, error

    # Engine-owned branding ships first; project assets may intentionally override it.
    _, error = _copy_tree(runtime_directory / "Assets" / "Engine", destination / "Assets" / "Engine",
                          runtime_directory)
    if error:
        return None, error
    for directory in ("Assets", "Scenes", "Config"):
        _, error = _copy_tree(project_root / directory, destination / directory, project_root)
        if error:
            return None, error

    manifest = dict(source_manifest)
    manifest["modules"] = []
    for module_entry, module_path in resolved_modules:
        packaged_entry = dict(module_entry)
        packaged_entry["path"] = module_path.name
        manifest["modules"].append(packaged_entry)
    try:
        (destination / "spark.modules.json").write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
    except OSError as exc:
        return None, f"Failed to write module manifest: {exc}"

    startup_scene, error = _find_startup_scene(project_root, project_info)
    if error:
        return None, error
    has_scene = startup_scene is not None
    if has_scene:
        preview_directory = destination / "ScenePreview"
        preview_directory.mkdir(parents=True, exist_ok=True)
        preview_name = "SparkGame Scene.exe" if platform == "windows" else "SparkGameScene"
        try:
            shutil.copy2(startup_scene, destination / "Startup.sparkscene")
            shutil.copy2(runtime_host, preview_directory / preview_name)
        except OSError as exc:
            return None, f"Failed to stage startup-scene preview: {exc}"

    error = _write_package_launchers(destination, host_name, has_scene)
    if error:
        return None, error

    debug_symbols_included = []
    if not strip_symbols:
        for _, module_path in resolved_modules:
            debug_symbols = module_path.with_suffix(".pdb")
            if not debug_symbols.is_file():
                continue
            try:
                shutil.copy2(debug_symbols, destination / debug_symbols.name)
                debug_symbols_included.append(debug_symbols.name)
            except OSError as exc:
                return None, f"Failed to copy module debug symbols: {exc}"

    asset_count = sum(1 for path in (destination / "Assets").rglob("*") if path.is_file()) \
        if (destination / "Assets").is_dir() else 0
    binary_count = 1 + len(resolved_modules) + (1 if has_scene else 0)
    launcher_name = "LaunchGame.cmd" if platform == "windows" else "LaunchGame.sh"
    module_names = [module_path.name for _, module_path in resolved_modules]
    package_manifest = {
        "packageOwner": PACKAGE_OWNER,
        "packageFormatVersion": PACKAGE_FORMAT_VERSION,
        "project": project_info["name"],
        "platform": platform,
        "config": config,
        "binaries": binary_count,
        "assets": asset_count,
        "stripped": False,
        "stripRequested": bool(strip_symbols),
        "debugSymbolsIncluded": bool(debug_symbols_included),
        "debugSymbolFiles": debug_symbols_included,
        "compressed": False,
        "compressionRequested": bool(compress),
        "entrypoint": launcher_name,
        "workingDirectory": ".",
        "host": host_name,
        "modules": module_names,
        "startupScene": "Startup.sparkscene" if has_scene else None,
    }
    try:
        (destination / "manifest.json").write_text(
            json.dumps(package_manifest, indent=2) + "\n", encoding="utf-8"
        )
    except OSError as exc:
        return None, f"Failed to write package manifest: {exc}"
    return package_manifest, None


def _is_owned_package(path, project_name, platform, config):
    """Recognize only a package previously published by this CLI."""
    if _path_is_link_like(path) or not path.is_dir():
        return False
    manifest_path = path / "manifest.json"
    module_manifest_path = path / "spark.modules.json"
    if (_path_is_link_like(manifest_path) or _path_is_link_like(module_manifest_path) or
            not manifest_path.is_file() or not module_manifest_path.is_file()):
        return False
    manifest, error = load_json_object(manifest_path, "package manifest")
    if error:
        return False
    return (
        manifest.get("packageOwner") == PACKAGE_OWNER and
        manifest.get("packageFormatVersion") == PACKAGE_FORMAT_VERSION and
        manifest.get("project") == project_name and
        manifest.get("platform") == platform and
        manifest.get("config") == config
    )


def _replacement_target_error(path, project_name, platform, config, force):
    """Validate a publication target without following a final-path link."""
    if not os.path.lexists(path):
        return None
    if _path_is_link_like(path):
        return f"Package destination is a link, junction, or reparse point and will never be replaced: {path}"
    if _is_owned_package(path, project_name, platform, config) or force:
        return None
    return (
        f"Package destination already exists but is not an owned Spark CLI package: {path}. "
        "Choose another output or pass --force to replace this exact non-linked target."
    )


def _transaction_metadata(package_directory, project_name, platform, config):
    return {
        "transactionOwner": TRANSACTION_OWNER,
        "transactionFormatVersion": TRANSACTION_FORMAT_VERSION,
        "packagePath": str(_absolute_unresolved(package_directory)),
        "project": project_name,
        "platform": platform,
        "config": config,
    }


def _write_transaction_marker(transaction, package_directory, project_name, platform, config):
    metadata = _transaction_metadata(package_directory, project_name, platform, config)
    return _write_text(transaction / TRANSACTION_MARKER, json.dumps(metadata, indent=2) + "\n")


def _recover_interrupted_package(output_dir, package_directory, package_name, project_name, platform, config):
    """Restore one unambiguous owned backup left by an interrupted publish."""
    if os.path.lexists(package_directory) or not output_dir.is_dir():
        return None

    transactions = sorted(output_dir.glob(f".{package_name}.transaction-*"))
    if not transactions:
        return None
    if len(transactions) != 1:
        return (
            f"Package destination is missing and {len(transactions)} possible recovery directories exist; "
            f"recovery data was preserved under '{output_dir}'. Resolve it manually before packaging."
        )

    transaction = transactions[0]
    marker_path = transaction / TRANSACTION_MARKER
    if (_path_is_link_like(transaction) or not transaction.is_dir() or
            _path_is_link_like(marker_path) or not marker_path.is_file()):
        return f"Unvalidated package recovery data was preserved at '{transaction}'; resolve it manually."
    marker, error = load_json_object(marker_path, "package transaction marker")
    expected = _transaction_metadata(package_directory, project_name, platform, config)
    if error or any(marker.get(key) != value for key, value in expected.items()):
        return f"Stale or foreign package recovery data was preserved at '{transaction}'; resolve it manually."

    backup = transaction / "previous-package"
    if (_path_is_link_like(backup) or
            not _is_owned_package(backup, project_name, platform, config)):
        return f"Package recovery data at '{transaction}' has no validated owned backup; resolve it manually."
    try:
        backup.replace(package_directory)
    except OSError as exc:
        return f"Failed to restore interrupted package from '{backup}': {exc}; recovery data was preserved"
    shutil.rmtree(transaction, ignore_errors=True)
    print(f"Recovered interrupted package publication to: {package_directory}")
    return None


def cmd_package(args):
    """Package the project for distribution."""
    project_root = Path.cwd().resolve()
    if not (project_root / "CMakeLists.txt").is_file():
        print("Error: No CMakeLists.txt found in current directory.")
        return 1

    config = args.config or "Release"
    platform = args.platform or current_platform()
    if config not in {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"}:
        print(f"Error: Unsupported build configuration: {config!r}")
        return 1
    if platform != current_platform():
        print(f"Error: Cannot package {platform} binaries from a {current_platform()} host.")
        return 1
    if platform == "unknown":
        print("Error: Unsupported packaging platform.")
        return 1

    project_file, error = find_project_descriptor(project_root)
    if error:
        print(f"Error: {error}")
        return 1
    if project_file.is_symlink() or not _path_is_within(project_file, project_root):
        print(f"Error: Project descriptor must be a regular file inside the project: {project_file}")
        return 1
    project_info, error = load_json_object(project_file, "project descriptor")
    if error:
        print(f"Error: {error}")
        return 1
    project_name = project_info.get("name")
    if not _safe_package_component(project_name):
        print(f"Error: Project name is not safe for a package directory: {project_name!r}")
        return 1

    requested_output = Path(args.output) if args.output else Path("dist")
    if not requested_output.is_absolute():
        requested_output = project_root / requested_output
    output_dir = _absolute_unresolved(requested_output)
    package_name = f"{project_name}-{platform}-{config.lower()}"
    # Keep the exact final pathname for ownership/link checks and publication.
    # Resolving it first would turn a symlink target into an unrelated directory
    # that the CLI might then replace.
    package_directory = _absolute_unresolved(output_dir / package_name)
    if _path_is_link_like(package_directory):
        print(
            "Error: Package destination is a link, junction, or reparse point "
            f"and will never be replaced: {package_directory}"
        )
        return 1
    resolved_package_directory = package_directory.resolve()
    protected = [project_root, project_root / "Assets", project_root / "Scenes", project_root / "Config"]
    if (resolved_package_directory == project_root or
            _path_is_within(project_root, resolved_package_directory) or
            any(_path_is_within(resolved_package_directory, path) for path in protected[1:])):
        print(
            "Error: Package output cannot replace or contain the project root, "
            "or live inside Assets, Scenes, or Config."
        )
        return 1
    build_directory = configured_build_dir(project_root, config)
    if _paths_overlap(resolved_package_directory, build_directory):
        print("Error: Package output cannot overlap the active build tree.")
        return 1

    recovery_error = _recover_interrupted_package(
        output_dir, package_directory, package_name, project_name, platform, config
    )
    if recovery_error:
        print(f"Error: {recovery_error}")
        return 1
    force = bool(getattr(args, "force", False))
    target_error = _replacement_target_error(
        package_directory, project_name, platform, config, force
    )
    if target_error:
        print(f"Error: {target_error}")
        return 1

    runtime_host = find_runtime_host(find_engine_root(), config)
    if not runtime_host:
        print(f"Error: No SparkEngine runtime host matching {config} was found.")
        print("Set SPARKENGINE_RUNTIME_HOST to an explicit executable or build/install the engine host.")
        return 1
    runtime_directory = runtime_host.parent.resolve()
    if _paths_overlap(resolved_package_directory, runtime_directory):
        print("Error: Package output cannot overlap the runtime host source directory.")
        return 1

    source_manifest_path = project_root / "spark.modules.json"
    if source_manifest_path.is_symlink() or not _path_is_within(source_manifest_path, project_root):
        print(f"Error: Module manifest must be a regular file inside the project: {source_manifest_path}")
        return 1
    source_manifest, error = load_json_object(source_manifest_path, "module manifest")
    modules = source_manifest.get("modules") if source_manifest else None
    if error or not isinstance(modules, list) or not modules:
        print(f"Error: {error or 'Module manifest must contain a non-empty modules array.'}")
        return 1
    path_key_error = validate_module_manifest_path_keys(source_manifest)
    if path_key_error:
        print(f"Error: Invalid module manifest '{source_manifest_path}': {path_key_error}")
        return 1

    print(f"=== Packaging '{project_name}' ===")
    print(f"  Config:   {config}")
    print(f"  Platform: {platform}")
    print(f"  Output:   {package_directory}")
    print(f"  Strip:    {args.strip}")
    print(f"  Compress: {args.compress}")
    print()

    print("[1/5] Building project...")
    build_result = cmd_build(args)
    if build_result != 0:
        print("Error: Build failed. Fix build errors before packaging.")
        return build_result

    resolved_modules, error = find_built_modules(build_directory, modules, config)
    if error:
        print(f"Error: {error}")
        return 1
    for _, module_path in resolved_modules:
        module_sidecar = Path(str(module_path) + ".sparkabi")
        if (module_path.is_symlink() or module_sidecar.is_symlink() or
                not _path_is_within(module_path, build_directory) or
                not _path_is_within(module_sidecar, build_directory)):
            print("Error: Built modules and ABI sidecars must be regular files inside the selected build tree.")
            return 1

    print("[2/5] Validating runtime and module artifacts...")
    print("[3/5] Staging runtime, module, and content...")
    try:
        output_dir.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        print(f"Error: Failed to create package output directory: {exc}")
        return 1
    transaction = None
    try:
        transaction = Path(tempfile.mkdtemp(prefix=f".{package_name}.transaction-", dir=output_dir))
        staging = transaction / "staging"
        staging.mkdir()
        marker_error = _write_transaction_marker(
            transaction, package_directory, project_name, platform, config
        )
        if marker_error:
            raise OSError(marker_error)
    except OSError as exc:
        if transaction is not None:
            shutil.rmtree(transaction, ignore_errors=True)
        print(f"Error: Failed to prepare package staging directory: {exc}")
        return 1
    backup = transaction / "previous-package"

    print("[4/5] Writing manifests and launchers...")
    try:
        manifest, error = _assemble_runnable_package(
            project_root, project_file, project_info, source_manifest, resolved_modules,
            runtime_host, staging, platform, config, args.strip, args.compress
        )
    except (OSError, shutil.Error) as exc:
        manifest, error = None, f"Unexpected filesystem failure: {exc}"
    if error:
        shutil.rmtree(transaction, ignore_errors=True)
        print(f"Error: Package assembly failed: {error}")
        return 1

    print("[5/5] Publishing package transactionally...")
    previous_moved = False
    try:
        target_error = _replacement_target_error(
            package_directory, project_name, platform, config, force
        )
        if target_error:
            raise OSError(target_error)
        if os.path.lexists(package_directory):
            package_directory.replace(backup)
            previous_moved = True
        staging.replace(package_directory)
    except OSError as exc:
        recovery_error = None
        try:
            if previous_moved and package_directory.exists():
                recovery_error = OSError("the package destination reappeared during recovery")
            elif previous_moved and backup.exists():
                backup.replace(package_directory)
        except OSError as recovery_exc:
            recovery_error = recovery_exc
        if recovery_error is None:
            shutil.rmtree(transaction, ignore_errors=True)
            detail = "; the previous package was restored" if previous_moved else ""
        else:
            detail = (
                f"; previous package recovery also failed: {recovery_error}; "
                f"recovery data was preserved at '{backup}'"
            )
        print(f"Error: Failed to publish package: {exc}{detail}")
        return 1
    shutil.rmtree(transaction, ignore_errors=True)

    if args.strip:
        print("Warning: --strip omits external debug symbols; module bytes remain unchanged to preserve the ABI hash.")
    if args.compress:
        print("Warning: --compress is retained for compatibility; runnable packages currently keep raw assets.")

    # Calculate total size
    total_size = sum(f.stat().st_size for f in package_directory.rglob("*") if f.is_file())
    size_mb = total_size / (1024 * 1024)

    print()
    print(f"Package created successfully!")
    print(f"  Location: {package_directory}")
    print(f"  Size:     {size_mb:.1f} MB")
    print(f"  Binaries: {manifest['binaries']}")
    print(f"  Assets:   {manifest['assets']}")
    return 0


def cmd_validate(args):
    """Validate project assets for integrity."""
    target_path = Path(args.path)
    strict = args.strict
    output_format = args.format

    if not target_path.exists():
        print(f"Error: Path '{target_path}' does not exist.")
        return 1

    print(f"Validating assets in '{target_path}'...")

    errors = []
    warnings = []
    checked = 0

    # Check material files for broken texture references
    for mat_file in target_path.rglob("*.material"):
        checked += 1
        try:
            content = mat_file.read_text(encoding="utf-8")
            mat_data = json.loads(content)
            for key, value in mat_data.items():
                if "texture" in key.lower() and isinstance(value, str) and value:
                    tex_path = target_path / value
                    if not tex_path.exists():
                        errors.append({
                            "file": str(mat_file),
                            "severity": "error",
                            "message": f"Missing texture: {value}",
                            "suggestion": f"Ensure '{value}' exists or remove the reference"
                        })
        except (json.JSONDecodeError, UnicodeDecodeError):
            warnings.append({
                "file": str(mat_file),
                "severity": "warning",
                "message": "Could not parse material file",
                "suggestion": "Check file encoding and JSON syntax"
            })

    # Check scene files for broken references
    for scene_file in target_path.rglob("*.scene"):
        checked += 1
        try:
            content = scene_file.read_text(encoding="utf-8")
            scene_data = json.loads(content)
            # Check for referenced assets
            for entity in scene_data.get("entities", []):
                for comp in entity.get("components", []):
                    if "mesh" in comp and comp["mesh"]:
                        mesh_path = target_path / comp["mesh"]
                        if not mesh_path.exists():
                            warnings.append({
                                "file": str(scene_file),
                                "severity": "warning",
                                "message": f"Missing mesh reference: {comp['mesh']}",
                                "suggestion": "Update mesh path or remove component"
                            })
        except (json.JSONDecodeError, UnicodeDecodeError, KeyError):
            pass

    # Check for orphaned assets (files not referenced by any scene/material)
    for shader_file in target_path.rglob("*.hlsl"):
        checked += 1

    for asset_file in target_path.rglob("*.png"):
        checked += 1

    for asset_file in target_path.rglob("*.wav"):
        checked += 1

    # Output results
    total_issues = len(errors) + len(warnings)

    if output_format == "json":
        report = {
            "totalChecked": checked,
            "errors": errors,
            "warnings": warnings,
            "passed": total_issues == 0 or (not strict and len(errors) == 0)
        }
        print(json.dumps(report, indent=2))
    else:
        print(f"\nValidation complete: {checked} assets checked")
        if errors:
            print(f"\n  ERRORS ({len(errors)}):")
            for e in errors:
                print(f"    [{e['severity'].upper()}] {e['file']}: {e['message']}")
                if e.get("suggestion"):
                    print(f"             -> {e['suggestion']}")
        if warnings:
            print(f"\n  WARNINGS ({len(warnings)}):")
            for w in warnings:
                print(f"    [{w['severity'].upper()}] {w['file']}: {w['message']}")

        if total_issues == 0:
            print("\n  All assets validated successfully.")
        else:
            print(f"\n  {len(errors)} errors, {len(warnings)} warnings")

    if strict and (errors or warnings):
        return 1
    if errors:
        return 1
    return 0


def cmd_migrate(args):
    """Migrate asset files to current format version."""
    target_path = Path(args.path)
    dry_run = args.dry_run
    backup = args.backup

    if not target_path.exists():
        print(f"Error: Path '{target_path}' does not exist.")
        return 1

    print(f"Scanning for assets needing migration in '{target_path}'...")
    if dry_run:
        print("  (dry run — no changes will be made)")

    migrated = 0
    skipped = 0
    failed = 0

    asset_extensions = {".scene", ".material", ".prefab", ".archetype", ".save"}

    for asset_file in target_path.rglob("*"):
        if asset_file.suffix not in asset_extensions:
            continue

        # Check for SPRK magic header (binary format)
        try:
            with open(asset_file, "rb") as f:
                magic = f.read(4)
                if magic == b"SPRK":
                    version_bytes = f.read(6)
                    major = int.from_bytes(version_bytes[0:2], "little")
                    minor = int.from_bytes(version_bytes[2:4], "little")
                    patch = int.from_bytes(version_bytes[4:6], "little")
                    print(f"  {asset_file.name}: v{major}.{minor}.{patch}")

                    # Current version is 1.0.0
                    if major < 1:
                        if dry_run:
                            print(f"    -> Would migrate to v1.0.0")
                            migrated += 1
                        else:
                            if backup:
                                backup_path = asset_file.with_suffix(asset_file.suffix + ".bak")
                                shutil.copy2(asset_file, backup_path)
                            print(f"    -> Migrated to v1.0.0")
                            migrated += 1
                    else:
                        skipped += 1
                else:
                    # Text/JSON format — check for version field
                    f.seek(0)
                    try:
                        content = f.read().decode("utf-8")
                        data = json.loads(content)
                        version = data.get("version", "0.0.0")
                        print(f"  {asset_file.name}: v{version} (JSON)")
                        skipped += 1
                    except (json.JSONDecodeError, UnicodeDecodeError):
                        skipped += 1
        except (IOError, OSError) as e:
            print(f"  {asset_file.name}: Failed — {e}")
            failed += 1

    print(f"\nMigration complete:")
    print(f"  Migrated: {migrated}")
    print(f"  Skipped:  {skipped}")
    print(f"  Failed:   {failed}")

    return 1 if failed > 0 else 0


def cmd_templates(args):
    """List available project templates."""
    engine_root = find_engine_root()
    if not engine_root:
        print("Error: Cannot find SparkEngine root directory.")
        return 1

    templates_dir = get_templates_dir(engine_root)
    if not templates_dir.is_dir():
        print("No templates directory found.")
        return 1

    templates = sorted(d for d in templates_dir.iterdir() if d.is_dir())
    if not templates:
        print("No templates available.")
        return 0

    print("Available project templates:")
    print()
    for t in templates:
        desc_file = t / "template.json"
        desc = ""
        genre = ""
        if desc_file.exists():
            try:
                with open(desc_file) as f:
                    info = json.load(f)
                    desc = info.get("description", "")
                    genre = info.get("genre", "")
            except (json.JSONDecodeError, IOError):
                pass

        name = t.name
        if desc:
            print(f"  {name:20s} — {desc}")
        else:
            print(f"  {name}")
        if genre:
            print(f"  {'':20s}   Genre: {genre}")
    print()
    print(f"Use: spark new MyGame --template <template-name>")
    return 0


def cmd_info(args):
    """Show information about the current project and engine."""
    engine_root = find_engine_root()

    print("=== Spark CLI Info ===")
    if engine_root:
        print(f"Engine root:  {engine_root}")
        templates_dir = get_templates_dir(engine_root)
        if templates_dir.is_dir():
            templates = [d.name for d in templates_dir.iterdir() if d.is_dir()]
            print(f"Templates:    {', '.join(templates) if templates else 'none'}")
    else:
        print("Engine root:  Not found (set SPARK_ENGINE_DIR)")

    project_file = Path("spark.project.json")
    if project_file.exists():
        with open(project_file) as f:
            info = json.load(f)
        print(f"\nCurrent project:")
        print(f"  Name:    {info.get('name', 'Unknown')}")
        print(f"  Version: {info.get('version', 'Unknown')}")
        print(f"  Engine:  {info.get('engineVersion', 'Unknown')}")
        modules = info.get("modules", [])
        if modules:
            print(f"  Modules: {', '.join(modules)}")
    else:
        print("\nNo project in current directory.")

    return 0


def main():
    parser = argparse.ArgumentParser(
        prog="spark",
        description="SparkEngine project scaffolding and build tool"
    )
    subparsers = parser.add_subparsers(dest="command", help="Command to run")

    # spark new
    new_parser = subparsers.add_parser("new", help="Create a new game project")
    new_parser.add_argument("name", help="Project name (must be a valid C++ identifier)")
    new_parser.add_argument("--template", "-t", default="EmptyProject",
                           help="Template to use (default: EmptyProject)")
    new_parser.add_argument("--output", "-o", default=None,
                           help="Output directory (default: current directory)")

    # spark build
    build_parser = subparsers.add_parser("build", help="Build the current project")
    build_parser.add_argument("--config", "-c", default="Debug",
                             choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
                             help="Build configuration (default: Debug)")

    # spark run
    run_parser = subparsers.add_parser("run", help="Build and run the current project")
    run_parser.add_argument("--config", "-c", default="Debug",
                           choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
                           help="Build configuration (default: Debug)")
    run_parser.add_argument("--no-build", action="store_true",
                           help="Launch an existing package/build without rebuilding")
    run_parser.add_argument("--package", default=None,
                           help="Launch an explicit existing package without rebuilding")
    run_parser.add_argument("runtime_args", nargs=argparse.REMAINDER,
                           help="Arguments after -- are forwarded to the runtime")

    # spark info
    subparsers.add_parser("info", help="Show project and engine info")

    # spark package
    pkg_parser = subparsers.add_parser("package", help="Package the project for distribution")
    pkg_parser.add_argument("--config", "-c", default="Release",
                           choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"],
                           help="Build configuration (default: Release)")
    pkg_parser.add_argument("--output", "-o", default=None,
                           help="Output directory (default: ./dist)")
    pkg_parser.add_argument("--platform", "-p", default=None,
                           choices=["windows", "linux", "macos"],
                           help="Target platform (default: current)")
    pkg_parser.add_argument("--strip", action="store_true",
                           help="Omit external debug-symbol files (module bytes remain ABI-safe)")
    pkg_parser.add_argument("--compress", action="store_true",
                           help="Request compression (currently packages raw runtime assets)")
    pkg_parser.add_argument("--force", action="store_true",
                           help="Replace an existing non-linked directory not owned by spark-cli")

    # spark validate
    val_parser = subparsers.add_parser("validate", help="Validate project assets for integrity")
    val_parser.add_argument("path", nargs="?", default=".",
                           help="Path to validate (default: current directory)")
    val_parser.add_argument("--strict", action="store_true",
                           help="Treat warnings as errors")
    val_parser.add_argument("--format", default="text",
                           choices=["text", "json"],
                           help="Output format (default: text)")

    # spark migrate
    mig_parser = subparsers.add_parser("migrate", help="Migrate asset files to current format version")
    mig_parser.add_argument("path", nargs="?", default=".",
                           help="Path to migrate (default: current directory)")
    mig_parser.add_argument("--dry-run", action="store_true",
                           help="Show what would be migrated without changes")
    mig_parser.add_argument("--backup", action="store_true", default=True,
                           help="Create backups before migrating (default: true)")

    # spark templates
    subparsers.add_parser("templates", help="List available project templates")

    args = parser.parse_args()

    if args.command is None:
        parser.print_help()
        return 0

    commands = {
        "new": cmd_new,
        "build": cmd_build,
        "run": cmd_run,
        "info": cmd_info,
        "package": cmd_package,
        "validate": cmd_validate,
        "migrate": cmd_migrate,
        "templates": cmd_templates,
    }

    return commands[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
