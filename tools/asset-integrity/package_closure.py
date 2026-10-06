#!/usr/bin/env python3
"""Profile-scoped runtime asset closure for SparkEngine package profiles (RDY-020).

A package profile such as stable-v1 ships only the assets its in-profile game
modules and the engine actually load, not every root under ``Assets/``. The
closure is derived, never hand-listed:

1. The in-profile modules come from ``GameModules/module-content-inventory.json``
   (``profileApplicability[<profile>] == "required"``). The reviewed profile
   definition (``tools/asset-integrity/package-profiles.json``) must name the
   same modules, so adding a module to the profile forces a review here.
2. Every asset-rooted string literal compiled into those modules' sources and
   into the reviewed engine source directories (``engineSources``: the engine
   code every module runs on, such as the primitive objects whose default OBJ
   models SceneManager instantiates) is a reference. Comments and preprocessor
   lines are excluded.
3. The profile definition adds reviewed seeds (startup branding, the asset
   README), each with a reason.
4. References are expanded transitively: ``.scene`` files through their
   ``key=value`` properties (``model=<name>`` resolves below ``Models/``) and
   material/data ``.json`` files through their string values.

A literal is a reference when its first path component names a top-level
manifest directory ignoring case, after backslashes become ``/`` and a leading
``./`` or ``Assets/`` is removed. Every reference must then name a declared
manifest entry with its exact case: a reference that resolves to nothing or
differs only in case, or a closure entry whose license is ``NOASSERTION``
(owner decision OD-09), fails the derivation; neither is silently dropped.
The only exception is a reviewed ``unshippedReferences`` entry: a literal the
runtime provably never opens, listed with its reason. Such an entry must still
occur in a scanned source and must not name a declared manifest entry, so a
stale exemption also fails.

The closure covers the literal asset paths the scanned code names; paths the
runtime composes at run time from non-literal parts are outside it until a
runtime smoke on the shipping backend confirms the set.

OBJ ``mtllib`` directives are not followed: the engine and FPS OBJ loaders
never open MTL files, so an MTL file is not a runtime dependency.
"""
from __future__ import annotations

import functools
import json
import re
from pathlib import Path
from typing import Any, Iterable


PROFILE_DEFINITIONS_RELATIVE = Path("tools/asset-integrity/package-profiles.json")
MODULE_INVENTORY_RELATIVE = Path("GameModules/module-content-inventory.json")
PROFILE_DEFINITIONS_VERSION = 1
PROFILE_KEYS = frozenset({"modules", "engineSources", "seeds", "unshippedReferences"})
NOASSERTION = "NOASSERTION"
# Module source files scanned for literals; a literal with one of these suffixes names code, not an asset.
SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"})
MAX_SOURCE_BYTES = 8 * 1024 * 1024
MAX_REFERENCE_FILE_BYTES = 16 * 1024 * 1024
ASSET_PREFIX = "assets/"
BACKSLASHES_RE = re.compile(r"\\+")
REFERENCE_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]*(?:/[A-Za-z0-9_][A-Za-z0-9_ .()+-]*)+\Z")
# Asset formats whose content names further assets.
REFERENCE_SUFFIXES = frozenset({".scene", ".json"})
CHAR_LITERAL_PREFIXES = frozenset({"L", "u", "U", "u8"})


class ClosureError(ValueError):
    """The profile closure could not be derived; the message names the cause."""


def _read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ClosureError(f"cannot read {path}: {exc}") from exc


def _skip_line(text: str, index: int) -> int:
    """Return the index after the logical line starting at ``index`` (backslash continuations join lines)."""
    length = len(text)
    while index < length:
        if text[index] == "\\" and text.startswith("\n", index + 1):
            index += 2
            continue
        if text[index] == "\n":
            return index + 1
        index += 1
    return index


def _quoted_end(text: str, index: int, quote: str) -> int:
    """Return the index of the closing ``quote`` for a literal whose body starts at ``index``."""
    length = len(text)
    while index < length:
        char = text[index]
        if char == "\\":
            index += 2
            continue
        if char == quote or char == "\n":
            return index
        index += 1
    return length


def cpp_string_literals(text: str) -> list[tuple[int, str]]:
    """Return ``(line, value)`` for every C/C++ string literal outside comments and preprocessor lines.

    Raw strings are returned verbatim; ordinary strings keep their escape
    sequences unexpanded, which is sufficient for asset paths.
    """
    literals: list[tuple[int, str]] = []
    index = 0
    length = len(text)
    at_line_start = True
    while index < length:
        char = text[index]
        if char == "\n":
            at_line_start = True
            index += 1
            continue
        if char in " \t\r\f\v":
            index += 1
            continue
        if at_line_start and char == "#":
            index = _skip_line(text, index)
            continue
        at_line_start = False
        if text.startswith("//", index):
            index = _skip_line(text, index)
            at_line_start = True
            continue
        if text.startswith("/*", index):
            close = text.find("*/", index + 2)
            index = length if close < 0 else close + 2
            continue
        if char.isalpha() or char == "_":
            start = index
            while index < length and (text[index].isalnum() or text[index] == "_"):
                index += 1
            identifier = text[start:index]
            if index < length and text[index] == "'" and identifier in CHAR_LITERAL_PREFIXES:
                index = _quoted_end(text, index + 1, "'") + 1
            elif index < length and text[index] == '"' and identifier.endswith("R") and \
                    identifier[:-1] in ("", *CHAR_LITERAL_PREFIXES):
                open_paren = text.find("(", index + 1)
                if open_paren < 0:
                    return literals
                delimiter = text[index + 1:open_paren]
                terminator = f"){delimiter}\""
                close = text.find(terminator, open_paren + 1)
                end = length if close < 0 else close
                literals.append((text.count("\n", 0, index) + 1, text[open_paren + 1:end]))
                index = length if close < 0 else close + len(terminator)
            continue
        if char.isdigit():
            # Numbers may carry digit separators (1'000), which are not char literals.
            while index < length and (text[index].isalnum() or text[index] in "'._"):
                index += 1
            continue
        if char == "'":
            index = _quoted_end(text, index + 1, "'") + 1
            continue
        if char == '"':
            end = _quoted_end(text, index + 1, '"')
            literals.append((text.count("\n", 0, index) + 1, text[index + 1:end]))
            index = end + 1
            continue
        index += 1
    return literals


@functools.lru_cache(maxsize=8)
def _folded(names: frozenset[str]) -> frozenset[str]:
    return frozenset(name.casefold() for name in names)


def asset_reference(value: str, top_level: frozenset[str]) -> str | None:
    """Return the asset-root-relative path ``value`` names, or ``None`` when it names no asset file.

    Backslashes (escaped in C++ source or literal in data files) become ``/``
    and a leading ``./`` or ``Assets/`` is removed. The first component is
    matched against ``top_level`` ignoring case, so a case typo that a
    case-insensitive filesystem would tolerate is still returned (and then
    fails the exact-case manifest lookup) instead of being dropped.
    """
    candidate = BACKSLASHES_RE.sub("/", value)
    while candidate.startswith("./"):
        candidate = candidate[2:]
    if candidate.casefold().startswith(ASSET_PREFIX):
        candidate = candidate[len(ASSET_PREFIX):]
    if not REFERENCE_RE.match(candidate):
        return None
    parts = candidate.split("/")
    if parts[0].casefold() not in _folded(top_level) or "." not in parts[-1] or \
            any(part in (".", "..") for part in parts):
        return None
    if Path(parts[-1]).suffix.lower() in SOURCE_SUFFIXES:
        return None
    return candidate


def load_profile_definition(repo_root: Path, profile: str) -> dict[str, Any]:
    """Load and validate one profile from the reviewed profile definitions."""
    path = repo_root / PROFILE_DEFINITIONS_RELATIVE
    document = _read_json(path)
    if not isinstance(document, dict) or document.get("version") != PROFILE_DEFINITIONS_VERSION:
        raise ClosureError(f"{path}: version must be {PROFILE_DEFINITIONS_VERSION}")
    profiles = document.get("profiles")
    if not isinstance(profiles, dict) or profile not in profiles:
        raise ClosureError(f"{path}: profile {profile!r} is not defined")
    definition = profiles[profile]
    if not isinstance(definition, dict) or set(definition) != PROFILE_KEYS:
        raise ClosureError(f"{path}: profile {profile!r} must have exactly {sorted(PROFILE_KEYS)}")
    modules = definition["modules"]
    if not isinstance(modules, list) or not modules or not all(isinstance(name, str) and name for name in modules):
        raise ClosureError(f"{path}: profile {profile!r} modules must be a non-empty list of names")
    for key in ("engineSources", "seeds", "unshippedReferences"):
        items = definition[key]
        if not isinstance(items, list):
            raise ClosureError(f"{path}: profile {profile!r} {key} must be a list")
        for index, item in enumerate(items):
            if (not isinstance(item, dict) or set(item) != {"path", "reason"}
                    or not isinstance(item["path"], str) or not item["path"]
                    or not isinstance(item["reason"], str) or not item["reason"].strip()):
                raise ClosureError(f"{path}: profile {profile!r} {key} {index} needs a non-empty path and reason")
    for index, source in enumerate(definition["engineSources"]):
        parts = source["path"].rstrip("/").split("/")
        if not source["path"].endswith("/") or source["path"].startswith("/") or any(
                part in ("", ".", "..") for part in parts):
            raise ClosureError(
                f"{path}: profile {profile!r} engineSources {index} must be a repository-relative directory ending in '/'")
    return definition


def _profile_modules(repo_root: Path, profile: str, declared: list[str]) -> list[dict[str, Any]]:
    path = repo_root / MODULE_INVENTORY_RELATIVE
    inventory = _read_json(path)
    modules = inventory.get("modules") if isinstance(inventory, dict) else None
    if not isinstance(modules, list):
        raise ClosureError(f"{path}: modules must be a list")
    required = [
        module for module in modules
        if isinstance(module, dict)
        and isinstance(module.get("profileApplicability"), dict)
        and module["profileApplicability"].get(profile) == "required"
    ]
    names = sorted(str(module.get("name")) for module in required)
    if names != sorted(declared):
        raise ClosureError(
            f"profile {profile!r} modules {sorted(declared)} do not match the module inventory's "
            f"required modules {names}; review {PROFILE_DEFINITIONS_RELATIVE.as_posix()}")
    return required


def _module_source_directory(module: dict[str, Any]) -> str:
    relative = module.get("sourceDirectory")
    if not isinstance(relative, str) or not relative:
        raise ClosureError(f"module {module.get('name')!r} has no sourceDirectory in the inventory")
    return relative


def _directory_sources(repo_root: Path, relative: str, owner: str) -> list[Path]:
    directory = repo_root / relative
    if not directory.is_dir() or directory.is_symlink():
        raise ClosureError(f"{owner} source directory is missing or a link: {relative}")
    return sorted(
        path for path in directory.rglob("*")
        if path.suffix.lower() in SOURCE_SUFFIXES and path.is_file() and not path.is_symlink())


def _bounded_text(path: Path, limit: int) -> str:
    try:
        size = path.stat().st_size
        if size > limit:
            raise ClosureError(f"{path}: {size} bytes exceeds the {limit}-byte reference-scan limit")
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise ClosureError(f"cannot read {path}: {exc}") from exc


def _scene_references(text: str, top_level: frozenset[str]) -> Iterable[tuple[int, str]]:
    for line_number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", ";", "[")) or "=" not in stripped:
            continue
        key, value = (part.strip() for part in stripped.split("=", 1))
        if key == "model" and value and "/" not in value and "\\" not in value:
            # SceneManager resolves a bare model name below Models/.
            value = f"Models/{value}"
        reference = asset_reference(value, top_level)
        if reference is not None:
            yield line_number, reference


def _json_references(value: Any, top_level: frozenset[str]) -> Iterable[str]:
    if isinstance(value, str):
        reference = asset_reference(value, top_level)
        if reference is not None:
            yield reference
    elif isinstance(value, dict):
        for item in value.values():
            yield from _json_references(item, top_level)
    elif isinstance(value, list):
        for item in value:
            yield from _json_references(item, top_level)


def _file_references(asset_root: Path, relative: str, top_level: frozenset[str]) -> list[tuple[str, str]]:
    """Return ``(location, reference)`` pairs for an asset whose format carries references."""
    suffix = Path(relative).suffix.lower()
    if suffix not in REFERENCE_SUFFIXES:
        return []
    text = _bounded_text(asset_root / relative, MAX_REFERENCE_FILE_BYTES)
    if suffix == ".scene":
        return [(f"{relative}:{line}", reference) for line, reference in _scene_references(text, top_level)]
    try:
        document = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ClosureError(f"{relative}: invalid JSON while following references: {exc}") from exc
    return [(relative, reference) for reference in _json_references(document, top_level)]


def derive_closure(
    repo_root: Path, manifest: dict[str, Any], profile: str
) -> tuple[dict[str, list[str]], list[Path]]:
    """Return ``({asset path: [why it is included]}, inputs)`` for ``profile``.

    ``manifest`` is the loaded reviewed source manifest whose root is
    ``<repo_root>/<manifest["root"]>``. ``inputs`` lists the files read other
    than C/C++ sources (profile definition, module inventory, scenes,
    materials) plus every scanned module and engine source *directory*, so a
    build system can re-derive the closure when an input changes or a source
    file is added or removed without treating every engine source edit as a
    configure input. Raises ``ClosureError`` with every unresolved reference
    and every NOASSERTION closure entry listed.
    """
    definition = load_profile_definition(repo_root, profile)
    inputs: list[Path] = [repo_root / PROFILE_DEFINITIONS_RELATIVE, repo_root / MODULE_INVENTORY_RELATIVE]
    entries = {entry["path"]: entry for entry in manifest["entries"]}
    folded_entries = {path.casefold(): path for path in entries}
    top_level = frozenset(path.split("/", 1)[0] for path in entries if "/" in path)
    asset_root = repo_root / manifest["root"]
    unshipped = {item["path"]: item["reason"] for item in definition["unshippedReferences"]}
    unshipped_seen: set[str] = set()
    reasons: dict[str, list[str]] = {}
    problems: list[str] = []
    pending: list[str] = []

    def include(reference: str, why: str) -> None:
        if reference in unshipped and reference not in entries:
            unshipped_seen.add(reference)
            return
        if reference not in entries:
            declared = folded_entries.get(reference.casefold())
            detail = (f"which differs in case from the declared {declared!r}" if declared
                      else "which the source manifest does not declare")
            problems.append(f"{why}: references {reference!r}, {detail}")
            return
        if reference not in reasons:
            reasons[reference] = []
            pending.append(reference)
        if why not in reasons[reference]:
            reasons[reference].append(why)

    scanned = [(_module_source_directory(module), f"module {module.get('name')!r}")
               for module in _profile_modules(repo_root, profile, definition["modules"])]
    scanned += [(source["path"], "engine") for source in definition["engineSources"]]
    for relative, owner in scanned:
        inputs.append(repo_root / relative.rstrip("/"))
        for source in _directory_sources(repo_root, relative, owner):
            location = source.relative_to(repo_root).as_posix()
            for line, value in cpp_string_literals(_bounded_text(source, MAX_SOURCE_BYTES)):
                reference = asset_reference(value, top_level)
                if reference is not None:
                    include(reference, f"{location}:{line}")

    for seed in definition["seeds"]:
        seed_path = seed["path"]
        why = f"seed: {seed['reason'].strip()}"
        if seed_path.endswith("/"):
            matched = [path for path in entries if path.startswith(seed_path)]
            if not matched:
                problems.append(f"seed directory {seed_path!r} matches no source manifest entry")
            for path in matched:
                include(path, why)
        else:
            include(seed_path, why)

    while pending:
        current = pending.pop()
        if Path(current).suffix.lower() in REFERENCE_SUFFIXES:
            inputs.append(asset_root / current)
        for location, reference in _file_references(asset_root, current, top_level):
            include(reference, location)

    for path in sorted(unshipped):
        if path in entries:
            problems.append(f"unshippedReferences {path!r} names a declared manifest entry; remove the exemption")
        elif path not in unshipped_seen:
            problems.append(f"unshippedReferences {path!r} occurs in no scanned source; remove the exemption")
    for path in sorted(reasons):
        if entries[path].get("license") == NOASSERTION:
            problems.append(
                f"{path}: needed by {reasons[path][0]} but its license is NOASSERTION; "
                f"package profile {profile!r} cannot ship it (OD-09)")
    if problems:
        raise ClosureError("\n".join(sorted(set(problems))))
    return {path: reasons[path] for path in sorted(reasons)}, sorted(set(inputs))
