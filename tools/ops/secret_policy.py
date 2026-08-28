#!/usr/bin/env python3
"""One bounded, fail-closed secret policy for OPS-100 artifacts."""

from __future__ import annotations

import io
import re
import zipfile
from dataclasses import dataclass
from pathlib import PurePosixPath, PureWindowsPath
from typing import Any, Iterable


MAX_ARCHIVE_MEMBERS = 128
MAX_ARCHIVE_MEMBER_BYTES = 16 * 1024 * 1024
MAX_ARCHIVE_TOTAL_BYTES = 32 * 1024 * 1024
MAX_COMPRESSION_RATIO = 200
_UNSUPPORTED_CONTAINER_SUFFIXES = {".7z", ".rar", ".tar", ".gz", ".bz2", ".xz", ".cab"}
_OPAQUE_BINARY_SUFFIXES = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".pdf"}

_SAFE_PLACEHOLDERS = {
    "",
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


# Expressions are deliberately bounded and avoid nested/unbounded repetition.
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
            r"-----BEGIN[ \t]+(?:RSA[ \t]+|EC[ \t]+|OPENSSH[ \t]+|ENCRYPTED[ \t]+)?PRIVATE[ \t]+KEY-----",
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


def _text_views(data: bytes, *, include_utf16: bool = False) -> Iterable[tuple[str, str]]:
    # latin-1 preserves every ASCII byte in arbitrary binary artifacts.
    yield "bytes", data.decode("latin-1")
    if include_utf16 or data.count(b"\x00") >= max(2, len(data) // 10):
        yield "utf16le", data.decode("utf-16-le", errors="ignore")
        yield "utf16be", data.decode("utf-16-be", errors="ignore")


def scan_text(text: str, *, location: str) -> list[SecretFinding]:
    findings: list[SecretFinding] = []
    seen: set[tuple[str, int]] = set()
    for rule in _RULES:
        for match in rule.expression.finditer(text):
            if rule.value_group and _is_placeholder(match.group(rule.value_group)):
                continue
            marker = (rule.name, match.start())
            if marker not in seen:
                seen.add(marker)
                findings.append(SecretFinding(rule.name, location))
    return findings


def scan_json_values(value: Any, *, location: str) -> list[SecretFinding]:
    """Find named credentials and token-shaped values in an already strict JSON tree."""

    findings: list[SecretFinding] = []
    pending: list[tuple[Any, str]] = [(value, location)]
    while pending:
        current, current_location = pending.pop()
        if isinstance(current, dict):
            for key, child in current.items():
                child_location = f"{current_location}.{key}"
                findings.extend(scan_text(key, location=f"{current_location} (object key)"))
                if _SENSITIVE_KEY.search(key) and child not in (None, False) and not _is_placeholder(child):
                    findings.append(SecretFinding("structured-credential", child_location))
                pending.append((child, child_location))
        elif isinstance(current, list):
            pending.extend((child, f"{current_location}[{index}]") for index, child in enumerate(current))
        elif isinstance(current, str):
            findings.extend(scan_text(current, location=current_location))
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
    if b"\x00" in data and normalized_suffix != ".dmp":
        return ScanResult(
            (),
            (f"{location}: opaque binary content is allowed only for bounded .dmp byte/UTF-16 scanning",),
        )

    findings: list[SecretFinding] = []
    for view_name, text in _text_views(data, include_utf16=normalized_suffix == ".dmp"):
        findings.extend(scan_text(text, location=f"{location} ({view_name})"))
    unique = {(item.rule, item.location): item for item in findings}
    return ScanResult(tuple(unique.values()), ())


_REDACT_PEM_BLOCK = re.compile(
    r"-----BEGIN[ \t]+(?:RSA[ \t]+|EC[ \t]+|OPENSSH[ \t]+|ENCRYPTED[ \t]+)?PRIVATE[ \t]+KEY-----"
    r"[\s\S]*?"
    r"-----END[ \t]+(?:RSA[ \t]+|EC[ \t]+|OPENSSH[ \t]+|ENCRYPTED[ \t]+)?PRIVATE[ \t]+KEY-----",
    re.IGNORECASE,
)

_REDACT_PEM_BODY = re.compile(
    r"-----BEGIN[ \t]+(?:RSA[ \t]+|EC[ \t]+|OPENSSH[ \t]+|ENCRYPTED[ \t]+)?PRIVATE[ \t]+KEY-----"
    r"(?:[ \t\r\n]*[A-Za-z0-9+/=]{4,})*",
    re.IGNORECASE,
)

_REDACT_STRUCTURED = re.compile(
    r"(\b[A-Za-z0-9_.-]{0,48}"
    r"(?:password|passwd|pwd|secret|api[_-]?key|auth[_-]?token|access[_-]?token|"
    r"refresh[_-]?token|client[_-]?secret|smtp[_-]?pass|connection[_-]?string)"
    r"[A-Za-z0-9_.-]{0,48}[\"']?[ \t]*[:=][ \t]*)"
    r"(?:"
    r"\"[^\"]{0,4096}\""
    r"|'[^']{0,4096}'"
    r"|[^\n,;\"'}{]{1,4096}"
    r")",
    re.IGNORECASE,
)


def _structured_replacement(match: re.Match[str]) -> str:
    prefix = match.group(1)
    value_part = match.group(0)[len(prefix):]
    if value_part.startswith('"') and value_part.endswith('"'):
        return prefix + '"<redacted>"'
    if value_part.startswith("'") and value_part.endswith("'"):
        return prefix + "'<redacted>'"
    return prefix + "<redacted>"


def redact_text(text: str) -> tuple[str, list[SecretFinding]]:
    findings = scan_text(text, location="input")
    redacted = text
    redacted = _REDACT_PEM_BLOCK.sub("<redacted:private-key>", redacted)
    redacted = _REDACT_PEM_BODY.sub("<redacted:private-key>", redacted)
    redacted = _REDACT_STRUCTURED.sub(_structured_replacement, redacted)
    for rule in _RULES:
        if rule.name == "structured-credential":
            continue
        if rule.value_group:
            redacted = rule.expression.sub(lambda match: match.group("prefix") + "<redacted>", redacted)
        else:
            redacted = rule.expression.sub(f"<redacted:{rule.name}>", redacted)
    return redacted, findings
