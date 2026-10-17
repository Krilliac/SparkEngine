"""Reject private header and include-directory leakage in the isolated actor SDK consumer."""

import argparse
from pathlib import Path
import re
import tempfile


INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)
PRIVATE_PREFIXES = ("Core/", "Game/", "Graphics/", "Utils/", "Engine/", "SparkEngine/")


def verify(sdk: Path, usage_file: Path | None = None, system_file: Path | None = None) -> list[str]:
    sdk = sdk.resolve()
    errors = []
    for path in sorted(sdk.rglob("*.h")):
        resolved = path.resolve()
        if not resolved.is_relative_to(sdk):
            errors.append(f"Header resolves outside the isolated SDK: {path}")
            continue
        for delimiter, include in INCLUDE.findall(path.read_text(encoding="utf-8")):
            include = include.replace("\\", "/")
            if include.startswith(PRIVATE_PREFIXES) or ".." in Path(include).parts or Path(include).is_absolute():
                errors.append(f"Private include in {path.name}: {include}")
                continue
            if include.startswith("Spark/"):
                target = sdk / include
            elif delimiter == '"':
                target = path.parent / include
            else:
                continue  # Toolchain and system dependency headers.
            if not target.is_file() or not target.resolve().is_relative_to(sdk):
                errors.append(f"Unresolved public include in {path.name}: {include}")
    if usage_file:
        allowed = {sdk}
        if system_file:
            allowed.update(
                Path(value).resolve()
                for value in system_file.read_text(encoding="utf-8").replace(";", "\n").splitlines()
                if value
            )
        for value in usage_file.read_text(encoding="utf-8").replace(";", "\n").splitlines():
            if not value:
                continue
            resolved = Path(value).resolve()
            normalized = resolved.as_posix().lower()
            engine_private = "/sparkengine/source" in normalized or normalized.endswith("/include/sparkengine")
            if resolved not in allowed or engine_private:
                errors.append(f"Consumer include directory is outside the isolated public SDK: {value}")
    return errors


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="spark-actor-boundary-") as directory:
        root = Path(directory)
        sdk = root / "public-sdk"
        (sdk / "Spark").mkdir(parents=True)
        header = sdk / "Spark" / "Actor.h"
        header.write_text("#include <vector>\n", encoding="utf-8")
        assert not verify(sdk)
        for include in ("Core/Platform.h", "Game/GameObject.h", "Graphics/Mesh.h", "../private.h"):
            header.write_text(f'#include "{include}"\n', encoding="utf-8")
            assert verify(sdk), include
        header.write_text("#include <Spark/Missing.h>\n", encoding="utf-8")
        assert verify(sdk)
        header.write_text("#include <vector>\n", encoding="utf-8")
        usage = root / "usage.txt"
        usage.write_text(str(sdk) + "\n", encoding="utf-8")
        assert not verify(sdk, usage)
        usage.write_text(str(root / "include" / "SparkEngine") + "\n", encoding="utf-8")
        assert verify(sdk, usage)
        system = root / "system.txt"
        system.write_text(str(root / "sdl2" / "include") + "\n", encoding="utf-8")
        usage.write_text(str(root / "sdl2" / "include") + "\n", encoding="utf-8")
        assert not verify(sdk, usage, system)
        system.write_text(str(root / "SparkEngine" / "Source") + "\n", encoding="utf-8")
        usage.write_text(system.read_text(encoding="utf-8"), encoding="utf-8")
        assert verify(sdk, usage, system)
    print("Actor SDK boundary rejects private includes, missing closure, and foreign include directories")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--usage-file", type=Path)
    parser.add_argument("--system-includes-file", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.sdk or not (args.sdk / "Spark").is_dir():
        parser.error("--sdk must name an isolated public SDK tree containing Spark/")
    errors = verify(args.sdk, args.usage_file, args.system_includes_file)
    for error in errors:
        print(error)
    if not errors:
        print("Actor SDK boundary verified: only public header closure and public include directories")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
