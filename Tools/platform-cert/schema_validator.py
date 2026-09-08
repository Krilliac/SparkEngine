#!/usr/bin/env python3
"""A fail-closed JSON Schema (Draft 2020-12) validator for PLT-200.

The certification schemas are committed to the repository, so their semantics
must be enforced exactly rather than approximated.  This module compiles a
schema and refuses, at compile time, any keyword it does not implement.  That
is the whole point: a validator that silently ignores an unknown keyword is
indistinguishable from one that has no rule at all, and it will always report
the reassuring answer.

Two deliberate strengthenings beyond a stock Draft 2020-12 implementation are
recorded here because they are not "equivalence":

  1. `format: "date-time"` is *asserted*, not treated as an annotation.  In
     2020-12 the format vocabulary is annotation-only unless declared, so a
     stock validator accepts "not-a-date".  A certification gate must not.
  2. `pattern` anchors are given ECMA-262 semantics.  Python's `$` also
     matches before a trailing newline, so `re.search` on "^[0-9a-f]{40}$"
     would accept a 40-hex string with "\\n" glued on.  `^`/`$` are compiled
     as `\\A`/`\\Z`.

Booleans are never numbers.  `isinstance(True, int)` is True in Python, which
is the single most common way a JSON validator written in Python leaks.
"""

from __future__ import annotations

import math
import re
from datetime import datetime
from typing import Any

# Every keyword this module understands.  Anything else is a hard error.
_ASSERTION_KEYWORDS = frozenset(
    {
        "type",
        "required",
        "additionalProperties",
        "properties",
        "enum",
        "const",
        "pattern",
        "format",
        "minimum",
        "maximum",
        "exclusiveMinimum",
        "exclusiveMaximum",
        "minLength",
        "maxLength",
        "minItems",
        "maxItems",
        "uniqueItems",
        "minProperties",
        "maxProperties",
        "items",
        "$ref",
        "$defs",
    }
)

# Keywords that carry no assertion and may be ignored safely.
_ANNOTATION_KEYWORDS = frozenset(
    {"$schema", "$id", "title", "description", "examples", "$comment"}
)

_VALID_TYPES = frozenset(
    {"object", "array", "string", "integer", "number", "boolean", "null"}
)


class SchemaError(Exception):
    """The schema itself is unusable -- unknown keyword, bad $ref, etc."""


def _json_type(value: Any) -> str:
    """The JSON type of a Python value, keeping bool distinct from integer."""
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    if isinstance(value, dict):
        return "object"
    raise SchemaError(f"value of unrepresentable type {type(value).__name__}")


def _type_matches(value: Any, declared: str) -> bool:
    actual = _json_type(value)
    if declared == "number":
        # An integer is a valid number; a boolean is not.
        return actual in ("integer", "number")
    if declared == "integer":
        # 1.0 is a valid integer in JSON Schema; True is not.
        if actual == "number":
            return float(value).is_integer()
        return actual == "integer"
    return actual == declared


def _json_equal(a: Any, b: Any) -> bool:
    """JSON equality: 1 == 1.0, but True != 1 and 0 != False."""
    ta, tb = _json_type(a), _json_type(b)
    if ta in ("integer", "number") and tb in ("integer", "number"):
        return float(a) == float(b)
    if ta != tb:
        return False
    if ta == "array":
        return len(a) == len(b) and all(_json_equal(x, y) for x, y in zip(a, b))
    if ta == "object":
        return a.keys() == b.keys() and all(_json_equal(a[k], b[k]) for k in a)
    return a == b


def _compile_pattern(pattern: str) -> re.Pattern[str]:
    """Compile a JSON Schema pattern with ECMA-262 anchor semantics."""
    body = pattern
    prefix = ""
    suffix = ""
    if body.startswith("^"):
        prefix, body = r"\A", body[1:]
    if body.endswith("$") and not body.endswith(r"\$"):
        body, suffix = body[:-1], r"\Z"
    # Only fully-anchored or fully-unanchored patterns are supported; an
    # interior anchor would need real ECMA translation, so refuse it.
    if "^" in body or re.search(r"(?<!\\)\$", body):
        raise SchemaError(f"unsupported interior anchor in pattern {pattern!r}")
    try:
        return re.compile(prefix + body + suffix)
    except re.error as exc:
        raise SchemaError(f"invalid pattern {pattern!r}: {exc}") from exc


def _parse_rfc3339(value: str) -> datetime | None:
    """Parse a strict RFC 3339 date-time, requiring an explicit offset."""
    text = value
    if text.endswith(("Z", "z")):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except (ValueError, TypeError):
        return None
    if parsed.tzinfo is None:
        return None
    return parsed


class CompiledSchema:
    """A schema compiled once, then reused for every document."""

    def __init__(self, schema: dict[str, Any]) -> None:
        if not isinstance(schema, dict):
            raise SchemaError("schema root must be an object")
        self._root = schema
        self._defs = schema.get("$defs", {})
        if not isinstance(self._defs, dict):
            raise SchemaError("$defs must be an object")
        self._patterns: dict[str, re.Pattern[str]] = {}
        # Compile-time sweep: every subschema must be fully understood before
        # a single document is validated against it.
        self._audit(schema, "#")
        for name, sub in self._defs.items():
            self._audit(sub, f"#/$defs/{name}")

    # -- compile-time auditing --------------------------------------------

    def _audit(self, schema: Any, pointer: str) -> None:
        if not isinstance(schema, dict):
            raise SchemaError(f"{pointer}: subschema must be an object")
        for keyword in schema:
            if keyword in _ANNOTATION_KEYWORDS:
                continue
            if keyword not in _ASSERTION_KEYWORDS:
                raise SchemaError(
                    f"{pointer}: unsupported schema keyword {keyword!r} -- "
                    f"refusing to validate with an incomplete implementation"
                )

        declared = schema.get("type")
        if declared is not None:
            names = declared if isinstance(declared, list) else [declared]
            for name in names:
                if name not in _VALID_TYPES:
                    raise SchemaError(f"{pointer}: unknown type {name!r}")

        if "pattern" in schema:
            self._patterns[schema["pattern"]] = _compile_pattern(schema["pattern"])

        fmt = schema.get("format")
        if fmt is not None and fmt != "date-time":
            raise SchemaError(f"{pointer}: unsupported format {fmt!r}")

        ap = schema.get("additionalProperties")
        if ap is not None and ap is not False:
            raise SchemaError(
                f"{pointer}: additionalProperties must be false "
                f"(open objects are not certifiable)"
            )

        ref = schema.get("$ref")
        if ref is not None:
            self._resolve(ref, pointer)

        for name, sub in schema.get("properties", {}).items():
            self._audit(sub, f"{pointer}/properties/{name}")
        if "items" in schema:
            self._audit(schema["items"], f"{pointer}/items")

    def _resolve(self, ref: str, pointer: str) -> dict[str, Any]:
        if not ref.startswith("#/$defs/"):
            raise SchemaError(f"{pointer}: only local #/$defs/ refs supported: {ref!r}")
        name = ref[len("#/$defs/") :]
        if name not in self._defs:
            raise SchemaError(f"{pointer}: dangling $ref {ref!r}")
        return self._defs[name]

    # -- document validation ----------------------------------------------

    def validate(self, document: Any) -> list[str]:
        """Return every violation of this schema, deepest path first."""
        errors: list[str] = []
        self._check(document, self._root, "$", errors)
        return errors

    def _check(
        self, value: Any, schema: dict[str, Any], path: str, errors: list[str]
    ) -> None:
        if "$ref" in schema:
            self._check(value, self._resolve(schema["$ref"], path), path, errors)
            # A $ref sits alongside no other assertion in these schemas, but
            # continue anyway so a future sibling keyword is not dropped.

        declared = schema.get("type")
        if declared is not None:
            names = declared if isinstance(declared, list) else [declared]
            if not any(_type_matches(value, n) for n in names):
                errors.append(
                    f"{path}: expected type {'/'.join(names)}, "
                    f"got {_json_type(value)}"
                )
                return

        if "const" in schema and not _json_equal(value, schema["const"]):
            errors.append(f"{path}: must equal {schema['const']!r}, got {value!r}")
        if "enum" in schema and not any(
            _json_equal(value, option) for option in schema["enum"]
        ):
            errors.append(
                f"{path}: {value!r} is not one of {sorted(map(repr, schema['enum']))}"
            )

        kind = _json_type(value)
        if kind == "string":
            self._check_string(value, schema, path, errors)
        elif kind in ("integer", "number"):
            self._check_number(value, schema, path, errors)
        elif kind == "array":
            self._check_array(value, schema, path, errors)
        elif kind == "object":
            self._check_object(value, schema, path, errors)

    def _check_string(
        self, value: str, schema: dict[str, Any], path: str, errors: list[str]
    ) -> None:
        if "minLength" in schema and len(value) < schema["minLength"]:
            errors.append(
                f"{path}: string shorter than {schema['minLength']} ({len(value)})"
            )
        if "maxLength" in schema and len(value) > schema["maxLength"]:
            errors.append(
                f"{path}: string longer than {schema['maxLength']} ({len(value)})"
            )
        if "pattern" in schema:
            if not self._patterns[schema["pattern"]].search(value):
                errors.append(
                    f"{path}: {value!r} does not match {schema['pattern']!r}"
                )
        if schema.get("format") == "date-time" and _parse_rfc3339(value) is None:
            errors.append(
                f"{path}: {value!r} is not an RFC 3339 date-time with an "
                f"explicit UTC offset"
            )

    def _check_number(
        self, value: Any, schema: dict[str, Any], path: str, errors: list[str]
    ) -> None:
        if isinstance(value, float) and not math.isfinite(value):
            errors.append(f"{path}: non-finite number")
            return
        for keyword, ok in (
            ("minimum", lambda v, b: v >= b),
            ("maximum", lambda v, b: v <= b),
            ("exclusiveMinimum", lambda v, b: v > b),
            ("exclusiveMaximum", lambda v, b: v < b),
        ):
            if keyword in schema and not ok(value, schema[keyword]):
                errors.append(f"{path}: fails {keyword} {schema[keyword]}")

    def _check_array(
        self, value: list[Any], schema: dict[str, Any], path: str, errors: list[str]
    ) -> None:
        if "minItems" in schema and len(value) < schema["minItems"]:
            errors.append(f"{path}: fewer than {schema['minItems']} items")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            errors.append(f"{path}: more than {schema['maxItems']} items")
        if schema.get("uniqueItems"):
            for i in range(len(value)):
                for j in range(i + 1, len(value)):
                    if _json_equal(value[i], value[j]):
                        errors.append(f"{path}: duplicate items at [{i}] and [{j}]")
                        break
        item_schema = schema.get("items")
        if item_schema is not None:
            for i, item in enumerate(value):
                self._check(item, item_schema, f"{path}[{i}]", errors)

    def _check_object(
        self, value: dict[str, Any], schema: dict[str, Any], path: str, errors: list[str]
    ) -> None:
        props = schema.get("properties", {})
        for name in schema.get("required", []):
            if name not in value:
                errors.append(f"{path}: missing required property {name!r}")
        if "minProperties" in schema and len(value) < schema["minProperties"]:
            errors.append(f"{path}: fewer than {schema['minProperties']} properties")
        if "maxProperties" in schema and len(value) > schema["maxProperties"]:
            errors.append(f"{path}: more than {schema['maxProperties']} properties")
        if schema.get("additionalProperties") is False:
            extra = sorted(set(value) - set(props))
            if extra:
                errors.append(f"{path}: unknown properties {extra}")
        for name, sub in props.items():
            if name in value:
                self._check(value[name], sub, f"{path}.{name}", errors)


def cross_check_with_jsonschema(
    schema: dict[str, Any], document: Any, our_errors: list[str]
) -> list[str]:
    """Second-opinion pass using the `jsonschema` package when installed.

    Only an asymmetric disagreement matters.  If the reference implementation
    rejects a document that we accepted, our engine has a hole and that is a
    hard error.  The reverse -- we reject what it accepts -- is expected, and
    is exactly the two documented strengthenings above.
    """
    try:
        import jsonschema  # type: ignore[import-not-found]
    except ImportError:
        return []

    try:
        validator_cls = jsonschema.validators.validator_for(schema)
        validator = validator_cls(schema)
        reference_errors = [e.message for e in validator.iter_errors(document)]
    except Exception as exc:  # a broken reference run must not pass silently
        return [f"jsonschema cross-check failed to run: {exc}"]

    if reference_errors and not our_errors:
        return [
            "VALIDATOR HOLE: jsonschema rejected a document this validator "
            f"accepted: {reference_errors[:3]}"
        ]
    return []
