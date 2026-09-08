#!/usr/bin/env python3
"""One bounded, fail-closed secret policy for OPS-100 artifacts."""

from __future__ import annotations

import hashlib
import io
import json
import re
import zipfile
from dataclasses import dataclass
from pathlib import PurePosixPath, PureWindowsPath
from typing import Any, Iterable


MAX_ARCHIVE_MEMBERS = 128
MAX_ARCHIVE_MEMBER_BYTES = 16 * 1024 * 1024
MAX_ARCHIVE_TOTAL_BYTES = 32 * 1024 * 1024
MAX_COMPRESSION_RATIO = 200
MAX_ARCHIVE_NESTING = 0
_UNSUPPORTED_CONTAINER_SUFFIXES = {".7z", ".rar", ".tar", ".gz", ".bz2", ".xz", ".cab"}
_OPAQUE_BINARY_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".pdf"}

_ZIP_LOCAL_MAGIC = b"PK\x03\x04"
_ZIP_EMPTY_MAGIC = b"PK\x05\x06"

_MINIDUMP_MAGIC = b"MDMP"
_MINIDUMP_MIN_SIZE = 32

_SAFE_PLACEHOLDERS = {
    "<redacted>",
    "redacted",
    "none",
    "null",
    "not-set",
    "not_set",
    "unset",
}
_SENSITIVE_KEY = re.compile(
    r"(?:password|passwd|pwd|secret|api[_-]?key|auth[_-]?token|access[_-]?token|"
    r"refresh[_-]?token|client[_-]?secret|private[_-]?key|smtp[_-]?pass|connection[_-]?string)",
    re.IGNORECASE,
)

# --- Redaction ceilings ---
MAX_REDACT_LINE_LENGTH = 1_048_576
MAX_REDACT_LINES = 100_000
MAX_REDACT_SECRETS = 10_000
MAX_REDACT_VALUE_BYTES = 4096
MAX_JSON_REDACT_DEPTH = 64
MAX_JSON_REDACT_ENTRIES = 50_000
MAX_JSON_REDACT_STRING = 4096


@dataclass(frozen=True)
class SecretFinding:
    rule: str
    location: str


@dataclass(frozen=True)
class ScanResult:
    findings: tuple[SecretFinding, ...]
    errors: tuple[str, ...]


@dataclass(frozen=True)
class _Rule:
    name: str
    expression: re.Pattern[str]
    value_group: str | None = None


_RULES = (
    _Rule(
        "github-token",
        re.compile(
            r"(?<![A-Za-z0-9_])(?:gh[pousr]_[A-Za-z0-9]{36,255}|"
            r"github_pat_[A-Za-z0-9_]{20,255})(?![A-Za-z0-9_])"
        ),
    ),
    _Rule(
        "openai-api-key",
        re.compile(
            r"(?<![A-Za-z0-9_-])(?:sk-(?:proj|svcacct)-[A-Za-z0-9_-]{20,512}|"
            r"sk-[A-Za-z0-9]{20,512})(?![A-Za-z0-9_-])"
        ),
    ),
    _Rule(
        "anthropic-api-key",
        re.compile(r"(?<![A-Za-z0-9_-])sk-ant-[A-Za-z0-9_-]{20,512}(?![A-Za-z0-9_-])"),
    ),
    _Rule("aws-access-key-id", re.compile(r"(?<![A-Z0-9])(?:AKIA|ASIA)[A-Z0-9]{16}(?![A-Z0-9])")),
    _Rule(
        "authorization-bearer",
        re.compile(r"\bbearer[ \t]+[A-Za-z0-9._~+/-]{16,1024}={0,2}", re.IGNORECASE),
    ),
    _Rule(
        "credential-in-url",
        re.compile(r"\b(?:https?|smtp)://[^\s/@:]{1,128}:[^\s/@]{1,512}@", re.IGNORECASE),
    ),
    _Rule(
        "private-key",
        re.compile(
            r"-----BEGIN[ \t]+(?:RSA[ \t]+|EC[ \t]+|DSA[ \t]+|OPENSSH[ \t]+|ENCRYPTED[ \t]+)?PRIVATE[ \t]+KEY-----",
            re.IGNORECASE,
        ),
    ),
    _Rule(
        "structured-credential",
        re.compile(
            r"(?P<prefix>\b[A-Za-z0-9_.-]{0,48}"
            r"(?:password|passwd|pwd|secret|api[_-]?key|auth[_-]?token|access[_-]?token|"
            r"refresh[_-]?token|client[_-]?secret|smtp[_-]?pass|connection[_-]?string)"
            r"[A-Za-z0-9_.-]{0,48}[\"']?[ \t]*[:=][ \t]*[\"']?)"
            r"(?P<value>[^\s,;\"'}]{1,1024})",
            re.IGNORECASE,
        ),
        "value",
    ),
)


def _is_placeholder(value: object) -> bool:
    return isinstance(value, str) and value.strip().lower() in _SAFE_PLACEHOLDERS


def _safe_location_hash(raw_path: str) -> str:
    """Return a keyed hash of a location path, suppressing any secret-bearing content."""
    digest = hashlib.sha256(raw_path.encode("utf-8", errors="replace")).hexdigest()[:16]
    return f"loc:{digest}"


def _sanitize_location(location: str) -> str:
    """Strip raw secret values from finding locations/key paths."""
    if _SENSITIVE_KEY.search(location):
        return _safe_location_hash(location)
    return location


def _text_views(data: bytes, *, include_utf16: bool = False) -> Iterable[tuple[str, str]]:
    yield "bytes", data.decode("latin-1")
    if include_utf16 or data.count(b"\x00") >= max(2, len(data) // 10):
        yield "utf16le", data.decode("utf-16-le", errors="ignore")
        yield "utf16be", data.decode("utf-16-be", errors="ignore")


def scan_text(text: str, *, location: str) -> list[SecretFinding]:
    findings: list[SecretFinding] = []
    seen: set[tuple[str, int]] = set()
    safe_loc = _sanitize_location(location)
    for rule in _RULES:
        for match in rule.expression.finditer(text):
            if rule.value_group and _is_placeholder(match.group(rule.value_group)):
                continue
            marker = (rule.name, match.start())
            if marker not in seen:
                seen.add(marker)
                findings.append(SecretFinding(rule.name, safe_loc))
    return findings


def scan_json_values(value: Any, *, location: str) -> list[SecretFinding]:
    """Find named credentials and token-shaped values in an already strict JSON tree."""
    findings: list[SecretFinding] = []
    pending: list[tuple[Any, str, int]] = [(value, location, 0)]
    visited = 0
    while pending:
        current, current_location, depth = pending.pop()
        visited += 1
        if visited > MAX_JSON_REDACT_ENTRIES:
            break
        if depth > MAX_JSON_REDACT_DEPTH:
            continue
        safe_loc = _sanitize_location(current_location)
        if isinstance(current, dict):
            for key, child in current.items():
                child_loc = f"{current_location}.{key}"
                findings.extend(scan_text(key, location=f"{current_location} (object key)"))
                if _SENSITIVE_KEY.search(key) and child not in (None, False) and not _is_placeholder(child):
                    findings.append(SecretFinding("structured-credential", _sanitize_location(child_loc)))
                pending.append((child, child_loc, depth + 1))
        elif isinstance(current, list):
            for index, child in enumerate(current):
                pending.append((child, f"{current_location}[{index}]", depth + 1))
        elif isinstance(current, str):
            findings.extend(scan_text(current, location=safe_loc))
    return findings


def _portable_archive_name(name: str) -> bool:
    posix = PurePosixPath(name)
    windows = PureWindowsPath(name)
    return bool(
        name
        and "\x00" not in name
        and not posix.is_absolute()
        and not windows.is_absolute()
        and not windows.drive
        and ".." not in posix.parts
        and ".." not in windows.parts
    )


def _is_zip_magic(data: bytes) -> bool:
    """Detect ZIP archives by magic bytes, not suffix."""
    return data[:4] == _ZIP_LOCAL_MAGIC or data[:4] == _ZIP_EMPTY_MAGIC


def _is_valid_minidump(data: bytes) -> bool:
    """Validate actual minidump structure: MDMP signature + version + stream directory."""
    if len(data) < _MINIDUMP_MIN_SIZE:
        return False
    if data[:4] != _MINIDUMP_MAGIC:
        return False
    version = int.from_bytes(data[4:8], "little")
    internal = version & 0xFFFF
    if internal == 0:
        return False
    stream_count = int.from_bytes(data[8:12], "little")
    if stream_count > 10000:
        return False
    return True


def _scan_zip(data: bytes, *, location: str) -> ScanResult:
    findings: list[SecretFinding] = []
    errors: list[str] = []
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            members = archive.infolist()
            if len(members) > MAX_ARCHIVE_MEMBERS:
                return ScanResult((), (f"{location}: archive exceeds {MAX_ARCHIVE_MEMBERS} members",))
            aggregate = 0
            for member in members:
                member_location = f"{location}!{member.filename}"
                if member.is_dir():
                    continue
                if not _portable_archive_name(member.filename):
                    errors.append(f"{member_location}: unsafe archive member name")
                    continue
                if member.flag_bits & 0x1:
                    errors.append(f"{member_location}: encrypted archive member cannot be inspected")
                    continue
                aggregate += member.file_size
                if member.file_size > MAX_ARCHIVE_MEMBER_BYTES:
                    errors.append(f"{member_location}: archive member exceeds {MAX_ARCHIVE_MEMBER_BYTES} bytes")
                    continue
                if aggregate > MAX_ARCHIVE_TOTAL_BYTES:
                    errors.append(f"{location}: expanded archive exceeds {MAX_ARCHIVE_TOTAL_BYTES} bytes")
                    break
                if member.file_size and member.compress_size == 0:
                    errors.append(f"{member_location}: invalid zero compressed size")
                    continue
                if member.compress_size and member.file_size > member.compress_size * MAX_COMPRESSION_RATIO:
                    errors.append(f"{member_location}: compression ratio exceeds {MAX_COMPRESSION_RATIO}:1")
                    continue
                suffix = PurePosixPath(member.filename).suffix.lower()
                if suffix in {".zip", ".7z", ".rar", ".tar", ".gz", ".bz2", ".xz"}:
                    errors.append(f"{member_location}: nested archive is not allowed")
                    continue
                try:
                    with archive.open(member) as source:
                        payload = source.read(MAX_ARCHIVE_MEMBER_BYTES + 1)
                except (OSError, RuntimeError, zipfile.BadZipFile) as exc:
                    errors.append(f"{member_location}: cannot inspect archive member: {exc}")
                    continue
                if len(payload) != member.file_size or len(payload) > MAX_ARCHIVE_MEMBER_BYTES:
                    errors.append(f"{member_location}: archive member size changed or exceeded its bound")
                    continue
                if _is_zip_magic(payload):
                    errors.append(f"{member_location}: nested archive detected by magic bytes")
                    continue
                nested = scan_payload(payload, location=member_location, suffix=suffix, allow_archive=False)
                findings.extend(nested.findings)
                errors.extend(nested.errors)
    except (OSError, zipfile.BadZipFile, zipfile.LargeZipFile) as exc:
        errors.append(f"{location}: invalid or unreadable ZIP archive: {exc}")
    return ScanResult(tuple(findings), tuple(errors))


def scan_payload(
    data: bytes,
    *,
    location: str,
    suffix: str = "",
    allow_archive: bool = True,
) -> ScanResult:
    normalized_suffix = suffix.lower()

    if _is_zip_magic(data):
        if not allow_archive:
            return ScanResult((), (f"{location}: nested archive is not allowed",))
        return _scan_zip(data, location=location)

    if normalized_suffix == ".zip":
        if not allow_archive:
            return ScanResult((), (f"{location}: nested archive is not allowed",))
        return _scan_zip(data, location=location)

    if normalized_suffix in _UNSUPPORTED_CONTAINER_SUFFIXES:
        return ScanResult((), (f"{location}: unsupported compressed/container format {normalized_suffix}",))
    if normalized_suffix in _OPAQUE_BINARY_SUFFIXES:
        return ScanResult(
            (),
            (f"{location}: opaque binary {normalized_suffix} requires a format-aware secret inspection policy",),
        )

    is_dmp = normalized_suffix == ".dmp"
    has_nul = b"\x00" in data
    if has_nul and not is_dmp:
        return ScanResult(
            (),
            (f"{location}: opaque binary content is allowed only for bounded .dmp byte/UTF-16 scanning",),
        )

    if is_dmp and has_nul:
        if not _is_valid_minidump(data):
            findings: list[SecretFinding] = []
            errors: list[str] = [
                f"{location}: binary .dmp does not have valid minidump structure; "
                f"scanning as binary but flagging invalid structure"
            ]
            for view_name, text in _text_views(data, include_utf16=True):
                findings.extend(scan_text(text, location=f"{location} ({view_name})"))
            unique = {(item.rule, item.location): item for item in findings}
            return ScanResult(tuple(unique.values()), tuple(errors))

    findings_list: list[SecretFinding] = []
    for view_name, text in _text_views(data, include_utf16=is_dmp):
        findings_list.extend(scan_text(text, location=f"{location} ({view_name})"))
    unique2 = {(item.rule, item.location): item for item in findings_list}
    return ScanResult(tuple(unique2.values()), ())


# ---------------------------------------------------------------------------
# PEM: single-pass bounded parser
# ---------------------------------------------------------------------------

_PEM_BEGIN = re.compile(
    r"-----BEGIN[ \t]+"
    r"((?:RSA|EC|DSA|OPENSSH|ENCRYPTED|)[ \t]*PRIVATE[ \t]+KEY)"
    r"-----",
    re.IGNORECASE,
)
_PEM_END_PREFIX = "-----END "
_PEM_END_SUFFIX = "-----"
_PEM_BASE64_LINE = re.compile(r"^[A-Za-z0-9+/=\s]*$")
_PEM_MAX_BODY_BYTES = 32 * 1024 * 1024


def _pem_redact_pass(text: str) -> str:
    """Single-pass bounded PEM parser: match BEGIN/END labels, reject mismatches.

    On mismatch or unmatched BEGIN, redact everything from BEGIN to the next
    END PRIVATE KEY (any label) or to end-of-text — never emit body bytes.
    """
    result: list[str] = []
    i = 0
    length = len(text)
    any_end_re = re.compile(
        r"-----END[ \t]+(?:RSA[ \t]+|EC[ \t]+|DSA[ \t]+|OPENSSH[ \t]+|ENCRYPTED[ \t]+)?PRIVATE[ \t]+KEY-----",
        re.IGNORECASE,
    )
    while i < length:
        match = _PEM_BEGIN.search(text, i)
        if match is None:
            result.append(text[i:])
            break
        result.append(text[i:match.start()])
        body_start = match.end()
        end_match = any_end_re.search(text, body_start)
        if end_match is not None:
            i = end_match.end()
            result.append("<redacted:private-key>")
        else:
            result.append("<redacted:private-key>")
            i = length
    return "".join(result)


# ---------------------------------------------------------------------------
# JSON-aware redaction
# ---------------------------------------------------------------------------

def _redact_json_value(value: Any, depth: int) -> Any:
    """Recursively redact sensitive values in a parsed JSON tree."""
    if depth > MAX_JSON_REDACT_DEPTH:
        return "<redacted:depth-exceeded>"
    if isinstance(value, dict):
        result = {}
        for key, child in value.items():
            if _SENSITIVE_KEY.search(key) and child not in (None, False) and not _is_placeholder(child):
                if isinstance(child, str):
                    result[key] = "<redacted>"
                else:
                    result[key] = "<redacted>"
            else:
                result[key] = _redact_json_value(child, depth + 1)
        return result
    if isinstance(value, list):
        return [_redact_json_value(item, depth + 1) for item in value]
    if isinstance(value, str):
        redacted, _ = _redact_text_inner(value)
        return redacted
    return value


def _try_redact_json(text: str) -> tuple[str | None, list[SecretFinding]]:
    """Attempt to parse as JSON, recursively redact, and re-serialize."""
    stripped = text.strip()
    if not stripped or stripped[0] not in ('{', '['):
        return None, []
    try:
        parsed = json.loads(text)
    except (json.JSONDecodeError, RecursionError):
        return None, []

    findings = scan_json_values(parsed, location="input")
    redacted_tree = _redact_json_value(parsed, 0)

    try:
        if text.startswith('{') or text.startswith('['):
            use_indent = None
            if '\n' in text[:200]:
                use_indent = 2
            output = json.dumps(redacted_tree, indent=use_indent, ensure_ascii=False)
        else:
            output = json.dumps(redacted_tree, ensure_ascii=False)
    except (ValueError, TypeError):
        return None, findings

    return output, findings


# ---------------------------------------------------------------------------
# Bounded linear lexer for structured credentials in generic text
# ---------------------------------------------------------------------------

_CREDENTIAL_KEYWORD = re.compile(
    r"\b[A-Za-z0-9_.-]{0,48}"
    r"(?:password|passwd|pwd|secret|api[_-]?key|auth[_-]?token|access[_-]?token|"
    r"refresh[_-]?token|client[_-]?secret|smtp[_-]?pass|connection[_-]?string)"
    r"[A-Za-z0-9_.-]{0,48}",
    re.IGNORECASE,
)


def _redact_structured_line(line: str) -> tuple[str, int]:
    """Bounded lexer for one line: find credential keywords, consume entire values."""
    result: list[str] = []
    secrets_found = 0
    pos = 0
    length = len(line)

    while pos < length:
        match = _CREDENTIAL_KEYWORD.search(line, pos)
        if match is None:
            result.append(line[pos:])
            break

        result.append(line[pos:match.start()])
        key_end = match.end()
        result.append(line[match.start():key_end])

        sep_pos = key_end
        while sep_pos < length and line[sep_pos] in "\"' \t":
            result.append(line[sep_pos])
            sep_pos += 1

        if sep_pos >= length or line[sep_pos] not in ':=':
            pos = key_end
            continue

        result.append(line[sep_pos])
        val_start = sep_pos + 1

        while val_start < length and line[val_start] in " \t":
            result.append(line[val_start])
            val_start += 1

        if val_start >= length:
            pos = val_start
            continue

        quote_char = line[val_start] if line[val_start] in ('"', "'") else None

        if quote_char:
            result.append(quote_char)
            val_pos = val_start + 1
            escaped = False
            value_chars: list[str] = []
            consumed = 0
            while val_pos < length and consumed < MAX_REDACT_VALUE_BYTES:
                ch = line[val_pos]
                if escaped:
                    value_chars.append(ch)
                    escaped = False
                    val_pos += 1
                    consumed += 1
                    continue
                if ch == '\\':
                    escaped = True
                    value_chars.append(ch)
                    val_pos += 1
                    consumed += 1
                    continue
                if ch == quote_char:
                    break
                value_chars.append(ch)
                val_pos += 1
                consumed += 1
            raw_value = "".join(value_chars)
            if _is_placeholder(raw_value):
                result.append(raw_value)
                if val_pos < length and line[val_pos] == quote_char:
                    result.append(quote_char)
                    val_pos += 1
            else:
                result.append("<redacted>")
                if val_pos < length and line[val_pos] == quote_char:
                    result.append(quote_char)
                    val_pos += 1
                secrets_found += 1
            pos = val_pos
        else:
            val_pos = val_start
            consumed = 0
            value_chars_uq: list[str] = []
            while val_pos < length and consumed < MAX_REDACT_VALUE_BYTES:
                ch = line[val_pos]
                if ch in ',;\n\r}{':
                    break
                value_chars_uq.append(ch)
                val_pos += 1
                consumed += 1
            raw_value_uq = "".join(value_chars_uq)
            if _is_placeholder(raw_value_uq.strip()):
                result.append(raw_value_uq)
            else:
                result.append("<redacted>")
                secrets_found += 1
            pos = val_pos

    return "".join(result), secrets_found


def _redact_text_inner(text: str) -> tuple[str, list[SecretFinding]]:
    """Core redaction: PEM pass, then per-line lexer, then token rules."""
    findings = scan_text(text, location="input")

    redacted = _pem_redact_pass(text)

    lines = redacted.split('\n')
    redacted_lines: list[str] = []
    total_secrets = 0
    for i, line in enumerate(lines):
        if i >= MAX_REDACT_LINES:
            redacted_lines.append(line)
            continue
        if len(line) > MAX_REDACT_LINE_LENGTH:
            redacted_lines.append(line[:MAX_REDACT_LINE_LENGTH])
            continue
        if total_secrets < MAX_REDACT_SECRETS:
            new_line, count = _redact_structured_line(line)
            total_secrets += count
            redacted_lines.append(new_line)
        else:
            redacted_lines.append(line)

    redacted = '\n'.join(redacted_lines)

    for rule in _RULES:
        if rule.name in ("structured-credential", "private-key"):
            continue
        redacted = rule.expression.sub(f"<redacted:{rule.name}>", redacted)

    return redacted, findings


def redact_text(text: str) -> tuple[str, list[SecretFinding]]:
    """Redact secrets from text. Attempts JSON-aware redaction first."""
    json_result, json_findings = _try_redact_json(text)
    if json_result is not None:
        text_findings = scan_text(text, location="input")
        all_findings = text_findings + json_findings
        unique = {(f.rule, f.location): f for f in all_findings}
        out = json_result
        for rule in _RULES:
            if rule.name in ("structured-credential", "private-key"):
                continue
            out = rule.expression.sub(f"<redacted:{rule.name}>", out)
        return out, list(unique.values())

    return _redact_text_inner(text)
