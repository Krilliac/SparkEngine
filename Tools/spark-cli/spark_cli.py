#!/usr/bin/env python3
"""
spark-cli — SparkEngine project scaffolding tool

Usage:
    spark new <project-name> [--template <template>] [--output <directory>]
    spark build [--config <Debug|Release>]
    spark run [--config <Debug|Release>]
    spark info

Templates:
    empty   — Empty project with minimal boilerplate (default)

Examples:
    spark new MyGame
    spark new MyGame --template empty --output ~/Projects
    spark build --config Release
    spark run
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
        if (current / "SparkSDK").is_dir() and (current / "Spark Engine").is_dir():
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

    # Replace {{PROJECT_NAME}} placeholders in all text files
    placeholder = "{{PROJECT_NAME}}"
    text_extensions = {".h", ".hpp", ".cpp", ".c", ".txt", ".json", ".cmake", ".md", ".py"}

    for root, dirs, files in os.walk(project_path):
        for filename in files:
            filepath = Path(root) / filename
            if filepath.suffix.lower() in text_extensions:
                try:
                    content = filepath.read_text(encoding="utf-8")
                    if placeholder in content:
                        content = content.replace(placeholder, project_name)
                        filepath.write_text(content, encoding="utf-8")
                except (UnicodeDecodeError, PermissionError):
                    pass

    # Rename template files that contain the placeholder in their name
    for root, dirs, files in os.walk(project_path, topdown=False):
        for filename in files:
            if placeholder in filename:
                old_path = Path(root) / filename
                new_path = Path(root) / filename.replace(placeholder, project_name)
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

    args = parser.parse_args()

    if args.command is None:
        parser.print_help()
        return 0

    commands = {
        "new": cmd_new,
        "build": cmd_build,
        "run": cmd_run,
        "info": cmd_info,
    }

    return commands[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
