#!/usr/bin/env python3
"""SDK-240: extract and pin the binary surface game modules compile against.

A module DLL calls the host through the vtables of the SDK interfaces
(IEngineContext, IModule, ILogger, the service interfaces) and exchanges
ModuleInfo and the pre-load compatibility descriptor by value. None of that can
be checked by the C++ compiler across two separately built binaries, and
IsSDKCompatible() is exact SPARK_SDK_VERSION equality, so the version number is
the only thing that keeps an old host from calling a new module (or the reverse)
through a mismatched layout.

This tool extracts, from the headers under SparkSDK/Include/Spark:

* every class with virtual functions: its virtual declarations in declaration
  (= vtable) order, destructor included;
* the ABI structs ModuleInfo and SparkModuleCompatibilityDescriptor: field
  order, types and their LP64 offsets/sizes;
* the ModuleKind enumerators, the module-ABI macros and the factory typedefs;

and compares the result with the committed golden SparkSDK/ABI/sdk-abi-surface.json.

``check`` (default) fails when:

* the surface changed while SPARK_SDK_VERSION did not;
* SPARK_SDK_VERSION changed but the golden was not re-pinned with ``update``;
* Version.h has no ``// vN:`` migration note for the current version N;
* Spark::EngineContextVirtualCount disagrees with the extracted
  IEngineContext virtual count.

``update`` rewrites the golden, and refuses to paper over an ABI change that
did not bump SPARK_SDK_VERSION or that lacks the migration note.

Exit status: 0 ok, 1 policy violation, 2 unreadable/unparseable input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

SDK_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INCLUDE = SDK_ROOT / "Include"
DEFAULT_GOLDEN = SDK_ROOT / "ABI" / "sdk-abi-surface.json"

# PluginABI.h is a separate C ABI with its own SPARK_PLUGIN_ABI_VERSION.
EXCLUDED_HEADERS = {"PluginABI.h"}

ABI_STRUCTS = ("Spark::ModuleInfo", "SparkModuleCompatibilityDescriptor")
ABI_ENUMS = ("Spark::ModuleKind",)
ABI_MACROS = (
    "SPARK_MODULE_ABI_MAGIC",
    "SPARK_MODULE_ABI_DESCRIPTOR_SIZE",
    "SPARK_MODULE_ABI_DESCRIPTOR_VERSION",
    "SPARK_MODULE_RUNTIME_ABI_VERSION",
    "SPARK_MODULE_COMPATIBILITY_EXPORT_NAME",
)
ABI_TYPEDEFS = ("CreateModuleFn", "DestroyModuleFn", "SparkGetModuleCompatibilityFn")
ENGINE_CONTEXT = "Spark::IEngineContext"

# LP64 (Linux/macOS x64/arm64) size and alignment of the scalar types the ABI
# structs use. An unknown field type is a hard error: extend this table
# deliberately rather than guess a layout.
LP64_SCALARS = {
    "bool": 1, "char": 1, "int8_t": 1, "uint8_t": 1,
    "int16_t": 2, "uint16_t": 2,
    "int": 4, "unsigned": 4, "float": 4, "int32_t": 4, "uint32_t": 4,
    "double": 8, "int64_t": 8, "uint64_t": 8, "size_t": 8,
}


class ParseError(Exception):
    pass


# ---------------------------------------------------------------------------
# Lexical cleanup
# ---------------------------------------------------------------------------

def strip_comments(text: str) -> str:
    """Drop // and /* */ comments, keeping string and character literals."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                raise ParseError("unterminated block comment")
            out.append("\n" * text.count("\n", i, end))
            i = end + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strip_preprocessor(text: str) -> str:
    """Remove directives, including backslash-continued macro bodies."""
    lines = text.split("\n")
    kept: list[str] = []
    continuing = False
    for line in lines:
        directive = continuing or line.lstrip().startswith("#")
        continuing = directive and line.rstrip().endswith("\\")
        kept.append("" if directive else line)
    return "\n".join(kept)


def collect_macros(text: str) -> dict[str, list[str]]:
    macros: dict[str, list[str]] = {}
    for match in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+([^\n]*)$", text, re.MULTILINE):
        macros.setdefault(match.group(1), []).append(match.group(2).strip())
    return macros


def normalize_signature(text: str) -> str:
    text = re.sub(r"\s+", " ", text).strip()
    text = re.sub(r"\s*([*&])\s*", r"\1 ", text)
    text = re.sub(r"([*&]) (?=[*&)>,])", r"\1", text)
    text = re.sub(r"\(\s+", "(", text)
    text = re.sub(r"\s+\)", ")", text)
    text = re.sub(r"\s*,\s*", ", ", text)
    text = re.sub(r"\s*::\s*", "::", text)
    return text.strip()


# ---------------------------------------------------------------------------
# Scope splitting
# ---------------------------------------------------------------------------

def match_brace(text: str, start: int) -> int:
    """Index of the '}' closing the '{' at ``start``."""
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < len(text) and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ParseError("unbalanced braces")


_BLOCK_HEAD = re.compile(r"^(?:template\s*<.*>\s*)?(?:typedef\s+)?(?:class|struct|union|enum)\b", re.DOTALL)
_ACCESS_LABEL = re.compile(r"^(?:public|protected|private)\s*:(?!:)\s*")


def split_statements(text: str) -> list[tuple[str, str | None]]:
    """Split a scope into (header, braced body or None) statements at depth 0."""
    statements: list[tuple[str, str | None]] = []
    i = 0
    start = 0
    paren = 0
    while i < len(text):
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < len(text) and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == "(":
            paren += 1
        elif c == ")":
            paren -= 1
        elif c == ";" and paren == 0:
            statements.append((text[start:i], None))
            start = i + 1
        elif c == "{" and paren == 0:
            header = text[start:i]
            end = match_brace(text, i)
            body = text[i + 1:end]
            stripped = _ACCESS_LABEL.sub("", header.strip())
            if stripped.rstrip().endswith("=") or re.search(r"\w\s*$", stripped) and "(" not in stripped \
                    and not _BLOCK_HEAD.match(stripped) and not stripped.startswith(("namespace", "extern")):
                # Brace initializer (`T x = {...};` / `T x{...};`): part of a plain statement.
                i = end + 1
                continue
            statements.append((header, body))
            i = end + 1
            if _BLOCK_HEAD.match(stripped):
                # Types end at the ';' after their closing brace.
                semi = text.find(";", i)
                i = semi + 1 if semi >= 0 else len(text)
            start = i
            continue
        i += 1
    tail = text[start:].strip()
    if tail:
        statements.append((tail, None))
    return statements


def clean_header(header: str) -> str:
    header = header.strip()
    while True:
        stripped = _ACCESS_LABEL.sub("", header)
        if stripped == header:
            return header
        header = stripped.strip()


# ---------------------------------------------------------------------------
# Surface extraction
# ---------------------------------------------------------------------------

class Surface:
    def __init__(self) -> None:
        self.classes: dict[str, dict] = {}
        self.enums: dict[str, dict] = {}
        self.typedefs: dict[str, str] = {}


def walk_scope(text: str, prefix: str, header_name: str, surface: Surface) -> None:
    for raw_header, body in split_statements(text):
        header = clean_header(raw_header)
        if body is None:
            using = re.match(r"^using\s+(\w+)\s*=\s*(.+)$", header, re.DOTALL)
            if using and using.group(1) in ABI_TYPEDEFS:
                surface.typedefs[using.group(1)] = normalize_signature(using.group(2))
            continue
        if header.startswith("namespace"):
            name = header[len("namespace"):].strip()
            walk_scope(body, f"{prefix}{name}::" if name else prefix, header_name, surface)
            continue
        if header.startswith("extern"):
            walk_scope(body, prefix, header_name, surface)
            continue
        enum = re.match(r"^enum\s+(?:class\s+|struct\s+)?(\w+)\s*(?::\s*(.+))?$", header, re.DOTALL)
        if enum:
            values = [normalize_signature(v) for v in body.split(",") if v.strip()]
            surface.enums[prefix + enum.group(1)] = {
                "underlying": normalize_signature(enum.group(2) or "int"),
                "values": values,
            }
            continue
        klass = re.match(r"^(?:typedef\s+)?(class|struct)\s+(\w+)(?:\s+final)?\s*(?::\s*(.+))?$", header, re.DOTALL)
        if klass:
            name = prefix + klass.group(2)
            if name in surface.classes:
                raise ParseError(f"{name} is defined twice ({header_name})")
            surface.classes[name] = parse_class(body, klass.group(3), header_name)


def parse_class(body: str, bases: str | None, header_name: str) -> dict:
    virtuals: list[str] = []
    fields: list[dict] = []
    for raw_header, member_body in split_statements(body):
        member = clean_header(raw_header)
        if not member:
            continue
        if member.startswith("virtual"):
            decl = member[len("virtual"):]
            decl = re.sub(r"=\s*(?:0|default|delete)\s*$", "", decl.strip())
            decl = re.sub(r"\b(?:override|final)\b", "", decl)
            virtuals.append(normalize_signature(decl))
            continue
        if member_body is not None or "(" in member or re.match(r"^(?:static|using|friend|typedef|enum)\b", member):
            continue
        try:
            fields.append(parse_field(member, header_name))
        except ParseError as exc:
            # Only the pinned ABI structs need a layout; report lazily.
            fields.append({"unparsed": str(exc)})
    return {
        "header": header_name,
        "bases": normalize_signature(bases) if bases else "",
        "virtuals": virtuals,
        "fields": fields,
    }


def parse_field(member: str, header_name: str) -> dict:
    declaration = re.split(r"=|\{", member, maxsplit=1)[0].strip()
    match = re.match(r"^(?P<type>.+?[\s*&])(?P<name>[A-Za-z_]\w*)\s*(?P<dims>(?:\[[^\]]+\]\s*)*)$", declaration,
                     re.DOTALL)
    if not match:
        raise ParseError(f"cannot parse field '{member}' in {header_name}")
    field = {"name": match.group("name"), "type": normalize_signature(match.group("type"))}
    dims = re.findall(r"\[([^\]]+)\]", match.group("dims"))
    if dims:
        field["count"] = "][".join(d.strip() for d in dims)
    return field


def lp64_layout(struct: dict, enums: dict[str, dict], name: str) -> dict:
    offset = 0
    alignment = 1
    fields = []
    for field in struct["fields"]:
        if "unparsed" in field:
            raise ParseError(f"{name}: {field['unparsed']}")
        type_name = field["type"]
        if type_name.endswith("*"):
            size = 8
        else:
            base = re.sub(r"^(?:const\s+|volatile\s+)+", "", type_name).split("::")[-1]
            enum = next((e for key, e in enums.items() if key.split("::")[-1] == base), None)
            scalar = enum["underlying"] if enum else base
            if scalar not in LP64_SCALARS:
                raise ParseError(f"{name}.{field['name']}: no LP64 size for type '{type_name}'")
            size = LP64_SCALARS[scalar]
        count = 1
        for dim in field.get("count", "1").split("]["):
            count *= int(dim.strip().rstrip("uU"), 0)
        offset = (offset + size - 1) // size * size
        fields.append({**field, "lp64_offset": offset, "lp64_size": size * count})
        offset += size * count
        alignment = max(alignment, size)
    total = (offset + alignment - 1) // alignment * alignment
    return {"fields": fields, "lp64_size": total}


def extract(include_root: Path) -> tuple[dict, dict]:
    """Return (surface, header facts) for the SDK rooted at ``include_root``."""
    spark_dir = include_root / "Spark"
    headers = sorted(p for p in spark_dir.glob("*.h") if p.name not in EXCLUDED_HEADERS)
    if not headers:
        raise ParseError(f"no SDK headers under {spark_dir}")

    surface = Surface()
    macros: dict[str, list[str]] = {}
    for path in headers:
        raw = path.read_text(encoding="utf-8")
        no_comments = strip_comments(raw)
        for key, values in collect_macros(no_comments).items():
            macros.setdefault(key, []).extend(values)
        walk_scope(strip_preprocessor(no_comments), "", path.name, surface)

    interfaces = {
        name: {"header": info["header"], "bases": info["bases"], "virtuals": info["virtuals"]}
        for name, info in sorted(surface.classes.items())
        if info["virtuals"]
    }
    if ENGINE_CONTEXT not in interfaces:
        raise ParseError(f"{ENGINE_CONTEXT} was not found")

    structs = {}
    for name in ABI_STRUCTS:
        if name not in surface.classes:
            raise ParseError(f"ABI struct {name} was not found")
        structs[name] = lp64_layout(surface.classes[name], surface.enums, name)

    enums = {}
    for name in ABI_ENUMS:
        if name not in surface.enums:
            raise ParseError(f"ABI enum {name} was not found")
        enums[name] = surface.enums[name]

    abi_macros = {}
    for name in ABI_MACROS:
        values = macros.get(name, [])
        if len(values) != 1:
            raise ParseError(f"expected exactly one #define {name}, found {len(values)}")
        abi_macros[name] = values[0]

    typedefs = {}
    for name in ABI_TYPEDEFS:
        if name not in surface.typedefs:
            raise ParseError(f"ABI typedef {name} was not found")
        typedefs[name] = surface.typedefs[name]

    sdk_versions = macros.get("SPARK_SDK_VERSION", [])
    if len(sdk_versions) != 1 or not sdk_versions[0].isdigit():
        raise ParseError("Version.h must #define SPARK_SDK_VERSION exactly once as a decimal integer")

    context_text = strip_comments((spark_dir / "IEngineContext.h").read_text(encoding="utf-8"))
    count = re.findall(r"EngineContextVirtualCount\s*=\s*(\d+)\s*;", context_text)
    version_text = (spark_dir / "Version.h").read_text(encoding="utf-8")

    facts = {
        "sdk_version": int(sdk_versions[0]),
        "pinned_engine_context_count": int(count[0]) if len(count) == 1 else None,
        "version_header": version_text,
    }
    surface_json = {
        "interfaces": interfaces,
        "structs": structs,
        "enums": enums,
        "macros": abi_macros,
        "typedefs": typedefs,
    }
    return surface_json, facts


# ---------------------------------------------------------------------------
# Policy
# ---------------------------------------------------------------------------

def surface_hash(surface: dict) -> str:
    return hashlib.sha256(json.dumps(surface, sort_keys=True).encode("utf-8")).hexdigest()


def changed_entries(old: dict, new: dict) -> list[str]:
    changed = []
    for section in sorted(set(old) | set(new)):
        before = old.get(section, {})
        after = new.get(section, {})
        for key in sorted(set(before) | set(after)):
            if before.get(key) != after.get(key):
                state = "added" if key not in before else "removed" if key not in after else "changed"
                changed.append(f"{section}.{key} ({state})")
    return changed


def invariant_errors(surface: dict, facts: dict) -> list[str]:
    errors = []
    version = facts["sdk_version"]
    if version > 1 and not re.search(rf"^\s*//\s*v{version}:", facts["version_header"], re.MULTILINE):
        errors.append(f"Spark/Version.h has no '// v{version}:' migration note for SPARK_SDK_VERSION {version}; "
                      "describe what changed in the ABI next to the version history.")
    actual = len(surface["interfaces"][ENGINE_CONTEXT]["virtuals"])
    pinned = facts["pinned_engine_context_count"]
    if pinned is None:
        errors.append("Spark/IEngineContext.h must define EngineContextVirtualCount exactly once.")
    elif pinned != actual:
        errors.append(f"Spark::EngineContextVirtualCount is stale: header says {pinned}, "
                      f"IEngineContext declares {actual} virtual functions.")
    return errors


def load_golden(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ParseError(f"cannot read golden {path}: {exc}") from exc


def check(surface: dict, facts: dict, golden: dict) -> list[str]:
    errors = invariant_errors(surface, facts)
    version = facts["sdk_version"]
    pinned_version = golden.get("sdk_version")
    pinned_surface = golden.get("surface", {})
    if surface != pinned_surface:
        changes = "; ".join(changed_entries(pinned_surface, surface))
        if version == pinned_version:
            errors.append(f"SDK ABI surface changed without a SPARK_SDK_VERSION bump (still {version}): {changes}. "
                          "Bump SPARK_SDK_VERSION in Spark/Version.h with a '// vN:' note, then run "
                          "'python3 SparkSDK/Tools/sdk_abi_surface.py update'.")
        else:
            errors.append(f"SPARK_SDK_VERSION is {version} but the golden (v{pinned_version}) was not re-pinned: "
                          f"{changes}. Run 'python3 SparkSDK/Tools/sdk_abi_surface.py update'.")
    elif version != pinned_version:
        errors.append(f"SPARK_SDK_VERSION is {version} but the golden pins v{pinned_version}: the bump was not "
                      "re-pinned (and the surface did not change -- was the bump intended?). Run "
                      "'python3 SparkSDK/Tools/sdk_abi_surface.py update' or revert the bump.")
    if golden.get("surface_sha256") != surface_hash(pinned_surface):
        errors.append("golden surface_sha256 does not match its surface; regenerate it with 'update'.")
    return errors


def update(surface: dict, facts: dict, golden: dict | None, golden_path: Path) -> list[str]:
    errors = invariant_errors(surface, facts)
    version = facts["sdk_version"]
    if golden is not None:
        pinned_version = golden.get("sdk_version", 0)
        if surface != golden.get("surface") and version == pinned_version:
            changes = "; ".join(changed_entries(golden.get("surface", {}), surface))
            errors.append(f"refusing to re-pin: SDK ABI surface changed without a SPARK_SDK_VERSION bump "
                          f"(still {version}): {changes}")
        if version < pinned_version:
            errors.append(f"refusing to re-pin: SPARK_SDK_VERSION went backwards ({pinned_version} -> {version})")
    if errors:
        return errors
    document = {
        "comment": "Generated by SparkSDK/Tools/sdk_abi_surface.py update. Do not edit by hand.",
        "sdk_version": version,
        "surface_sha256": surface_hash(surface),
        "surface": surface,
    }
    golden_path.parent.mkdir(parents=True, exist_ok=True)
    golden_path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return []


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("mode", nargs="?", choices=("check", "update", "print"), default="check")
    parser.add_argument("--sdk-include", type=Path, default=DEFAULT_INCLUDE,
                        help="include root containing Spark/*.h (default: SparkSDK/Include)")
    parser.add_argument("--golden", type=Path, default=DEFAULT_GOLDEN)
    args = parser.parse_args(argv)

    try:
        surface, facts = extract(args.sdk_include)
        if args.mode == "print":
            print(json.dumps(surface, indent=2, sort_keys=True))
            return 0
        if args.mode == "update":
            golden = load_golden(args.golden) if args.golden.exists() else None
            errors = update(surface, facts, golden, args.golden)
        else:
            errors = check(surface, facts, load_golden(args.golden))
    except (OSError, ParseError) as exc:
        print(f"sdk_abi_surface: error: {exc}", file=sys.stderr)
        return 2

    for error in errors:
        print(f"sdk_abi_surface: FAIL: {error}", file=sys.stderr)
    if errors:
        return 1
    interfaces = surface["interfaces"]
    print(f"sdk_abi_surface: OK ({args.mode}) -- SPARK_SDK_VERSION {facts['sdk_version']}, "
          f"{len(interfaces)} interfaces, {len(interfaces[ENGINE_CONTEXT]['virtuals'])} IEngineContext virtuals")
    return 0


if __name__ == "__main__":
    sys.exit(main())
