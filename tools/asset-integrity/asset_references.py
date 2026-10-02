#!/usr/bin/env python3
"""Reference extraction for the staged-package asset reference-closure check (ENG-220).

``verify_asset_integrity.py references`` proves that every asset a staged
scene or material names resolves to a file inside the staged ``Assets`` root
that ``assets.integrity.json`` lists. This module only parses; the verifier
owns every filesystem access and the manifest lookup.

Formats and the runtime convention each reference follows:

* INI scenes (``[Scene]``/``[Object]``/``[Terrain]`` sections of
  ``key=value`` lines, read by ``SceneManager``). ``model`` and ``material``
  are runtime references: ``SceneManager`` hands ``model`` to the OBJ loader
  unchanged, so it must start with ``Assets/``, and ``GameObject`` loads a
  ``material`` only when it starts with exactly ``Assets/Materials/`` (or
  ``Assets\\Materials\\``) and ends with ``.json``; anything else silently
  renders a placeholder cube or the default material, so it is an error here.
  Two keys are validator policy, not runtime resolution: ``SceneManager``
  stores ``mesh`` as an opaque node property and ignores ``[Scene] skybox``
  today, but content that names them must still be closed, so ``mesh`` is
  checked as an ``Assets/``-rooted model and a non-``default`` ``skybox`` as
  an ``Assets/``-rooted cubemap prefix whose six
  ``<prefix>_{px,nx,py,ny,pz,nz}.png`` faces must all be staged.
* JSON scenes (``{"entities": [...]}``, the reflected-world dialect).
  ``MeshRenderer.mesh`` (or the built-in ``Primitive/<Name>``),
  ``MeshRenderer.material``, ``AudioSource.sound`` and
  ``environment.skyTexture`` are ``Assets``-root relative; a leading
  ``Assets/`` is accepted. An empty value means "none".
* Material JSON (``GraphicsEngine::GetOrLoadBasicMaterial``). ``albedo``,
  ``normal`` and a string ``roughness`` name textures relative to ``Assets``
  unless already prefixed with ``Assets/``.

Scene and material dialects name references differently, so the parser fails
closed: a value that looks like an asset path (it contains a path separator or
ends with a known asset suffix) under any key not listed above is reported as
an unknown reference key instead of being skipped. A scene that is neither INI
nor JSON (the legacy space-delimited form) is rejected rather than ignored.

This proves reference closure of staged content; it does not prove that a
runtime consumer loads every JSON-dialect reference.
"""
from __future__ import annotations

import json
import ntpath
from dataclasses import dataclass
from pathlib import PurePosixPath
from typing import Any, Iterable


ASSETS_PREFIX = "Assets/"
MODEL_SUFFIXES = frozenset({".obj"})
MATERIAL_SUFFIXES = frozenset({".json"})
TEXTURE_SUFFIXES = frozenset({
    ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".hdr", ".exr", ".ktx", ".ktx2", ".basis",
})
AUDIO_SUFFIXES = frozenset({".wav", ".ogg", ".mp3", ".flac"})
# Suffixes that make a value under an unknown key reference-like.
ASSET_SUFFIXES = MODEL_SUFFIXES | MATERIAL_SUFFIXES | TEXTURE_SUFFIXES | AUDIO_SUFFIXES | frozenset({
    ".fbx", ".gltf", ".glb", ".mtl", ".scene", ".as", ".spk", ".ttf", ".otf", ".csv", ".txt", ".xml",
})
CUBEMAP_FACES = ("px", "nx", "py", "ny", "pz", "nz")
BUILTIN_SKYBOXES = frozenset({"default"})
BUILTIN_MESH_PREFIX = "Primitive/"
MATERIAL_TEXTURE_KEYS = ("albedo", "normal", "roughness")
MATERIAL_TEXT_KEYS = frozenset({"name", "shader"})
# (enclosing object key, value key) -> reference kind, for the JSON scene dialect.
JSON_SCENE_REFERENCE_KEYS = {
    ("MeshRenderer", "mesh"): "model",
    ("MeshRenderer", "material"): "material",
    ("AudioSource", "sound"): "audio",
    ("environment", "skyTexture"): "texture",
}
INI_SCENE_REFERENCE_KEYS = {"model": "model", "mesh": "model", "material": "material"}
# GameObject::PrepareBasicMaterialRender loads an INI material only under these
# exact, case-sensitive prefixes and with this exact suffix.
INI_RUNTIME_MATERIAL_PREFIXES = ("Assets/Materials/", "Assets\\Materials\\")
INI_RUNTIME_MATERIAL_SUFFIX = ".json"
KIND_SUFFIXES = {
    "model": MODEL_SUFFIXES,
    "material": MATERIAL_SUFFIXES,
    "texture": TEXTURE_SUFFIXES,
    "audio": AUDIO_SUFFIXES,
}


@dataclass(frozen=True)
class Reference:
    """One resolved reference: ``target`` is relative to the staged Assets root."""

    location: str
    key: str
    value: str
    kind: str
    target: str


@dataclass(frozen=True)
class Problem:
    """A reference that cannot be resolved lexically; ``message`` says why."""

    location: str
    key: str
    value: str
    message: str


def is_reference_like(value: str) -> bool:
    """True when ``value`` looks like an asset path rather than a name or number."""
    text = value.strip()
    if not text:
        return False
    if "/" in text or "\\" in text:
        return True
    return PurePosixPath(text.lower()).suffix in ASSET_SUFFIXES


def _assets_relative(value: str, *, require_prefix: bool) -> tuple[str | None, str | None]:
    """Return ``(target, None)`` relative to the Assets root, or ``(None, reason)``."""
    candidate = value.strip().replace("\\", "/")
    if candidate.startswith("/") or ntpath.splitdrive(candidate)[0] or ntpath.isabs(value.strip()):
        return None, "is an absolute path; references must stay inside the staged Assets root"
    if candidate.startswith(ASSETS_PREFIX):
        candidate = candidate[len(ASSETS_PREFIX):]
    elif require_prefix:
        return None, ("is not an Assets/-rooted path; the runtime resolves it outside the staged Assets root "
                      "and falls back to a placeholder")
    parts = candidate.split("/")
    if ".." in parts:
        return None, "escapes the staged Assets root through '..'"
    if any(part in ("", ".") for part in parts):
        return None, "has an empty or '.' path component"
    return "/".join(parts), None


def _resolve(location: str, key: str, value: str, kind: str, *, require_prefix: bool,
             references: list[Reference], problems: list[Problem]) -> None:
    target, reason = _assets_relative(value, require_prefix=require_prefix)
    if target is None:
        problems.append(Problem(location, key, value, reason or "cannot be resolved"))
        return
    suffixes = KIND_SUFFIXES.get(kind)
    if suffixes is not None and PurePosixPath(target.lower()).suffix not in suffixes:
        expected = ", ".join(sorted(suffixes))
        problems.append(Problem(location, key, value, f"names a {kind} that is not one of {expected}"))
        return
    references.append(Reference(location, key, value, kind, target))


def _skybox(location: str, value: str, references: list[Reference], problems: list[Problem]) -> None:
    if value in BUILTIN_SKYBOXES:
        return
    if not is_reference_like(value):
        problems.append(Problem(location, "skybox", value,
                                f"is neither a built-in skybox ({', '.join(sorted(BUILTIN_SKYBOXES))}) "
                                "nor an Assets/-rooted cubemap prefix"))
        return
    for face in CUBEMAP_FACES:
        _resolve(location, "skybox", f"{value}_{face}.png", "texture", require_prefix=True,
                 references=references, problems=problems)


def _decode(relative: str, data: bytes) -> tuple[str | None, Problem | None]:
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        return None, Problem(relative, "", "", f"is not valid UTF-8: {exc}")
    return text, None


def _strict_json(relative: str, text: str) -> tuple[Any, Problem | None]:
    def reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, item in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON object key {key!r}")
            result[key] = item
        return result

    try:
        return json.loads(text, object_pairs_hook=reject_duplicates), None
    except ValueError as exc:
        return None, Problem(relative, "", "", f"is not valid JSON: {exc}")


def _unknown(location: str, key: str, value: str, problems: list[Problem]) -> None:
    problems.append(Problem(location, key, value,
                            "is an asset-like value under a key the reference validator does not know; "
                            "add the key to tools/asset-integrity/asset_references.py or fix the content"))


def _ini_material_ignored_by_runtime(value: str) -> bool:
    """True for an Assets/-rooted INI material that GameObject would silently skip.

    Values that are not Assets/-rooted at all fall through to ``_resolve`` so
    they keep the more specific rooted/absolute diagnostics.
    """
    if value.startswith(INI_RUNTIME_MATERIAL_PREFIXES) and value.endswith(INI_RUNTIME_MATERIAL_SUFFIX):
        return False
    return value.replace("\\", "/").startswith(ASSETS_PREFIX)


def _ini_scene(relative: str, text: str) -> tuple[list[Reference], list[Problem]]:
    references: list[Reference] = []
    problems: list[Problem] = []
    section = ""
    for line_number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        location = f"{relative}:{line_number}"
        if not stripped or stripped.startswith(("#", ";")):
            continue
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1].strip()
            continue
        if "=" not in stripped:
            problems.append(Problem(location, "", stripped, "is neither a section, a comment, nor a key=value line"))
            continue
        key, value = (part.strip() for part in stripped.split("=", 1))
        kind = INI_SCENE_REFERENCE_KEYS.get(key)
        if kind is not None:
            if value and key == "material" and _ini_material_ignored_by_runtime(value):
                problems.append(Problem(location, key, value,
                                        "is not an Assets/Materials/*.json path; GameObject ignores materials "
                                        "outside Assets/Materials/ and renders the default material"))
                continue
            if value:
                _resolve(location, key, value, kind, require_prefix=True, references=references, problems=problems)
            continue
        if key == "skybox" and section == "Scene":
            _skybox(location, value, references, problems)
            continue
        if is_reference_like(value):
            _unknown(location, key, value, problems)
    return references, problems


def _json_scene_walk(value: Any, relative: str, trail: tuple[str, ...],
                     references: list[Reference], problems: list[Problem]) -> None:
    if isinstance(value, dict):
        for key, item in value.items():
            _json_scene_walk(item, relative, trail + (key,), references, problems)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            _json_scene_walk(item, relative, trail + (str(index),), references, problems)
    elif isinstance(value, str):
        key = trail[-1] if trail else ""
        parent = trail[-2] if len(trail) >= 2 else ""
        location = f"{relative}:{'.'.join(trail)}"
        kind = JSON_SCENE_REFERENCE_KEYS.get((parent, key))
        if kind is not None:
            if kind == "model" and value.startswith(BUILTIN_MESH_PREFIX) and "/" not in value[len(BUILTIN_MESH_PREFIX):]:
                return
            if value:
                _resolve(location, key, value, kind, require_prefix=False, references=references, problems=problems)
            return
        if is_reference_like(value):
            _unknown(location, key, value, problems)


def scene_references(relative: str, data: bytes) -> tuple[list[Reference], list[Problem]]:
    """Parse one staged ``.scene`` file (INI or JSON dialect) into references and problems."""
    text, problem = _decode(relative, data)
    if text is None:
        return [], [problem] if problem else []
    body = text.lstrip()
    if body.startswith("{"):
        document, problem = _strict_json(relative, text)
        if problem is not None:
            return [], [problem]
        references: list[Reference] = []
        problems: list[Problem] = []
        _json_scene_walk(document, relative, (), references, problems)
        return references, problems
    if not any(line.strip().startswith("[") for line in text.splitlines()):
        return [], [Problem(relative, "", "", "is not an INI or JSON scene; the legacy space-delimited dialect "
                                              "cannot be checked for reference closure")]
    return _ini_scene(relative, text)


def material_references(relative: str, data: bytes) -> tuple[list[Reference], list[Problem]]:
    """Parse one staged material JSON file into texture references and problems."""
    text, problem = _decode(relative, data)
    if text is None:
        return [], [problem] if problem else []
    document, problem = _strict_json(relative, text)
    if problem is not None:
        return [], [problem]
    if not isinstance(document, dict):
        return [], [Problem(relative, "", "", "material root must be a JSON object")]
    references: list[Reference] = []
    problems: list[Problem] = []
    for key in MATERIAL_TEXTURE_KEYS:
        value = document.get(key)
        if isinstance(value, str) and value:
            _resolve(f"{relative}:{key}", key, value, "texture", require_prefix=False,
                     references=references, problems=problems)
    for key, value in _strings(document, ()):
        if key[0] in MATERIAL_TEXT_KEYS and len(key) == 1:
            continue
        if len(key) == 1 and key[0] in MATERIAL_TEXTURE_KEYS:
            continue
        if is_reference_like(value):
            _unknown(f"{relative}:{'.'.join(key)}", key[-1], value, problems)
    return references, problems


def _strings(value: Any, trail: tuple[str, ...]) -> Iterable[tuple[tuple[str, ...], str]]:
    if isinstance(value, dict):
        for key, item in value.items():
            yield from _strings(item, trail + (key,))
    elif isinstance(value, list):
        for index, item in enumerate(value):
            yield from _strings(item, trail + (str(index),))
    elif isinstance(value, str):
        yield trail, value
