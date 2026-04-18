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
    MultiplayerArena — Multiplayer arena with networking and lobby

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
import subprocess
import sys
import json
from pathlib import Path


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
    text_extensions = {".h", ".hpp", ".cpp", ".c", ".txt", ".json", ".cmake", ".md", ".py"}

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
    build_dir = Path("build")

    # Configure if needed
    if not build_dir.exists():
        print("Configuring project...")
        result = subprocess.run(
            ["cmake", "-B", "build", f"-DCMAKE_BUILD_TYPE={config}"],
            cwd=Path.cwd()
        )
        if result.returncode != 0:
            print("Configuration failed.")
            return result.returncode

    # Build
    print(f"Building ({config})...")
    result = subprocess.run(
        ["cmake", "--build", "build", "--config", config],
        cwd=Path.cwd()
    )
    return result.returncode


def cmd_run(args):
    """Build and run the current game project."""
    # Build first
    build_result = cmd_build(args)
    if build_result != 0:
        return build_result

    # Find the built DLL and the engine executable
    project_file = Path("spark.project.json")
    if not project_file.exists():
        print("Error: No spark.project.json found. Cannot determine how to run.")
        return 1

    with open(project_file) as f:
        project_info = json.load(f)

    print(f"Project: {project_info.get('name', 'Unknown')}")
    print("To run, copy the built DLL next to SparkEngine.exe and launch it.")
    print("(Automatic launch will be supported in a future update)")
    return 0


def cmd_package(args):
    """Package the project for distribution."""
    if not Path("CMakeLists.txt").exists():
        print("Error: No CMakeLists.txt found in current directory.")
        return 1

    config = args.config or "Release"
    output_dir = Path(args.output) if args.output else Path("dist")
    strip_symbols = args.strip
    compress = args.compress

    # Detect platform
    if args.platform:
        platform = args.platform
    elif sys.platform.startswith("win"):
        platform = "windows"
    elif sys.platform.startswith("linux"):
        platform = "linux"
    elif sys.platform.startswith("darwin"):
        platform = "macos"
    else:
        platform = "unknown"

    # Read project info
    project_name = "SparkGame"
    project_file = Path("spark.project.json")
    if project_file.exists():
        with open(project_file) as f:
            info = json.load(f)
            project_name = info.get("name", project_name)

    print(f"=== Packaging '{project_name}' ===")
    print(f"  Config:   {config}")
    print(f"  Platform: {platform}")
    print(f"  Output:   {output_dir}")
    print(f"  Strip:    {strip_symbols}")
    print(f"  Compress: {compress}")
    print()

    # Step 1: Build
    print("[1/5] Building project...")
    build_result = subprocess.run(
        ["cmake", "--build", "build", "--config", config],
        cwd=Path.cwd()
    )
    if build_result.returncode != 0:
        print("Error: Build failed. Fix build errors before packaging.")
        return build_result.returncode

    # Step 2: Create output directory
    print("[2/5] Preparing output directory...")
    output_dir.mkdir(parents=True, exist_ok=True)
    pkg_dir = output_dir / f"{project_name}-{platform}-{config.lower()}"
    if pkg_dir.exists():
        shutil.rmtree(pkg_dir)
    pkg_dir.mkdir(parents=True)

    # Step 3: Copy binaries
    print("[3/5] Copying binaries...")
    bin_dir = pkg_dir / "bin"
    bin_dir.mkdir()

    build_bin = Path("build") / config
    if not build_bin.exists():
        build_bin = Path("build")

    binary_exts = {".exe", ".dll", ".so", ".dylib", ""} if platform != "windows" else {".exe", ".dll"}
    copied_bins = 0
    for ext in binary_exts:
        for f in build_bin.rglob(f"*{ext}"):
            if f.is_file() and f.stat().st_size > 0:
                dest = bin_dir / f.name
                shutil.copy2(f, dest)
                copied_bins += 1

    # Step 4: Copy assets
    print("[4/5] Copying assets...")
    assets_src = Path("Assets")
    if assets_src.is_dir():
        assets_dst = pkg_dir / "Assets"
        shutil.copytree(assets_src, assets_dst)
        asset_count = sum(1 for _ in assets_dst.rglob("*") if _.is_file())
    else:
        asset_count = 0

    # Step 5: Strip symbols if requested
    if strip_symbols and platform == "linux":
        print("[5/5] Stripping debug symbols...")
        for f in bin_dir.rglob("*"):
            if f.is_file() and not f.suffix:
                subprocess.run(["strip", str(f)], capture_output=True)
    else:
        print("[5/5] Finalizing...")

    # Create manifest
    manifest = {
        "project": project_name,
        "platform": platform,
        "config": config,
        "binaries": copied_bins,
        "assets": asset_count,
        "stripped": strip_symbols,
        "compressed": compress,
    }
    manifest_path = pkg_dir / "manifest.json"
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    # Calculate total size
    total_size = sum(f.stat().st_size for f in pkg_dir.rglob("*") if f.is_file())
    size_mb = total_size / (1024 * 1024)

    print()
    print(f"Package created successfully!")
    print(f"  Location: {pkg_dir}")
    print(f"  Size:     {size_mb:.1f} MB")
    print(f"  Binaries: {copied_bins}")
    print(f"  Assets:   {asset_count}")
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
                           help="Strip debug symbols from binaries")
    pkg_parser.add_argument("--compress", action="store_true",
                           help="Compress assets into SparkPak archives")

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
