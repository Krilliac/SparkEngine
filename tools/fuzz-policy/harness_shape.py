#!/usr/bin/env python3
"""C++ fuzz-harness shape verification for SEC-120.

A harness proves nothing if its required tokens live in comments or string
literals. Everything here runs against source with comments and literal bodies
removed, so only real declarations, real calls, and real comparisons count.
"""

from __future__ import annotations

import re
from pathlib import Path

from policy_common import PolicyError, read_confined_file

MAX_HARNESS_BYTES = 1024 * 1024
MAX_PARSER_SOURCE_BYTES = 8 * 1024 * 1024

ENTRY_POINT = "LLVMFuzzerTestOneInput"
DEPTH_CONSTANT = "SPARK_FUZZ_MAX_DEPTH"
INPUT_CONSTANT = "SPARK_FUZZ_MAX_INPUT_BYTES"

_RAW_STRING = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(')
_SYMBOL = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*$")
_HARNESS_DEFINITION = re.compile(
    r"\bint\s+" + ENTRY_POINT + r"\s*\(\s*const\s+(?:std\s*::\s*)?(?:uint8_t|unsigned\s+char)\s*\*"
    r"[^)]{0,120}?\b(?:std\s*::\s*)?size_t\b[^)]{0,80}\)\s*\{"
)
_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]', re.MULTILINE)


def strip_cxx(text: str) -> str:
    """Blank out comments and the bodies of string/char literals, preserving offsets loosely."""
    out: list[str] = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char == "/" and index + 1 < length:
            following = text[index + 1]
            if following == "/":
                end = text.find("\n", index)
                end = length if end < 0 else end
                out.append("\n" * text.count("\n", index, end))
                index = end
                continue
            if following == "*":
                end = text.find("*/", index + 2)
                if end < 0:
                    raise PolicyError("source has an unterminated block comment")
                out.append("\n" * text.count("\n", index, end))
                index = end + 2
                continue
        raw = _RAW_STRING.match(text, index)
        if raw is not None:
            closing = ')' + raw.group(1) + '"'
            end = text.find(closing, raw.end())
            if end < 0:
                raise PolicyError("source has an unterminated raw string literal")
            out.append('""' + "\n" * text.count("\n", index, end))
            index = end + len(closing)
            continue
        if char in "\"'":
            quote = char
            cursor = index + 1
            while cursor < length:
                if text[cursor] == "\\":
                    cursor += 2
                    continue
                if text[cursor] == quote:
                    cursor += 1
                    break
                if text[cursor] == "\n":
                    break
                cursor += 1
            out.append(quote * 2 + "\n" * text.count("\n", index, cursor))
            index = cursor
            continue
        out.append(char)
        index += 1
    return "".join(out)


def _read_source(root: Path, relative: str, field: str, *, max_bytes: int) -> str:
    payload = read_confined_file(root, relative, field, max_bytes=max_bytes)
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise PolicyError(f"{field} must be strict UTF-8") from exc
    return strip_cxx(text)


def _constant_binding(code: str, name: str, value: int, field: str) -> None:
    """Require the constant to be *defined* with the declared value and then used."""
    define_form = r"#\s*define\s+" + name + r"\s+" + str(value) + r"\b"
    constant_form = (
        r"\b(?:constexpr|const|constinit)\s+[A-Za-z_][A-Za-z0-9_:<>\s*]{0,64}?\b"
        + name
        + r"\s*(?:=|\{)\s*"
        + str(value)
        + r"\b"
    )
    definition = re.compile(define_form + r"|" + constant_form)
    if definition.search(code) is None:
        raise PolicyError(f"{field} does not define {name} = {value}")
    if len(re.findall(r"\b" + name + r"\b", code)) < 2:
        raise PolicyError(f"{field} defines {name} but never uses it")


def require_entry_symbol(symbol: str, field: str) -> str:
    if not _SYMBOL.fullmatch(symbol):
        raise PolicyError(f"{field} must be a C++ identifier or qualified name: {symbol!r}")
    return symbol


def verify_harness(
    root: Path,
    *,
    harness: str,
    entry_symbol: str,
    max_depth: int,
    max_input_bytes: int,
    field: str,
) -> None:
    """Require a compilable harness shape that calls the production entry point."""
    code = _read_source(root, harness, f"{field}.harness", max_bytes=MAX_HARNESS_BYTES)
    if _HARNESS_DEFINITION.search(code) is None:
        raise PolicyError(f"{field}.harness does not define {ENTRY_POINT}(const uint8_t*, size_t)")
    if _INCLUDE.search(code) is None:
        raise PolicyError(f"{field}.harness includes no headers, so it cannot call a production parser")

    _constant_binding(code, DEPTH_CONSTANT, max_depth, f"{field}.harness")
    _constant_binding(code, INPUT_CONSTANT, max_input_bytes, f"{field}.harness")
    comparison = re.compile(
        r"(?:[<>]=?\s*" + INPUT_CONSTANT + r"\b|\b" + INPUT_CONSTANT + r"\s*[<>]=?)"
    )
    if comparison.search(code) is None:
        raise PolicyError(f"{field}.harness never compares its input size against {INPUT_CONSTANT}")

    leaf = require_entry_symbol(entry_symbol, f"{field}.entry_symbol").rsplit("::", 1)[-1]
    if re.search(r"\b" + re.escape(leaf) + r"\s*\(", code) is None:
        raise PolicyError(f"{field}.harness never calls the declared entry symbol {entry_symbol}")


def assert_entry_symbol_in_sources(root: Path, sources: tuple[str, ...], entry_symbol: str, field: str) -> None:
    """Bind the harness to the parser it claims to cover."""
    leaf = require_entry_symbol(entry_symbol, f"{field}.entry_symbol").rsplit("::", 1)[-1]
    pattern = re.compile(r"\b" + re.escape(leaf) + r"\s*\(")
    for source in sources:
        code = _read_source(root, source, f"{field}.source_files", max_bytes=MAX_PARSER_SOURCE_BYTES)
        if pattern.search(code) is not None:
            return
    raise PolicyError(f"{field}.entry_symbol {entry_symbol} is not declared by any inventoried source file")
