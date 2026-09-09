#!/usr/bin/env python3
"""Semantic validation for evidence artifacts beyond file-existence checks.

The previous validator checked only ``path.is_file()`` for JUnit XML and
package-smoke evidence.  A zero-byte file, an empty XML document, or a log
containing only whitespace all satisfied the gate.  This module requires each
artifact to carry the structured content its producer would actually emit.
"""

from __future__ import annotations

import re
import xml.etree.ElementTree as ET
from pathlib import Path


def _safe_parse_xml(path: Path) -> ET.ElementTree:
    """Parse XML rejecting DTDs and entity declarations (XXE hardening).

    Uses expat directly to install rejection handlers before parsing, which
    works across Python 3.10-3.13+.
    """
    from xml.parsers import expat as _expat

    target = ET.TreeBuilder()
    expat_parser = _expat.ParserCreate()

    def _reject_entity(*args: object) -> None:
        raise ET.ParseError("entity declarations are rejected")

    def _reject_external(*args: object) -> int:
        raise ET.ParseError("external entity references are rejected")

    expat_parser.EntityDeclHandler = _reject_entity
    expat_parser.ExternalEntityRefHandler = _reject_external
    expat_parser.UnparsedEntityDeclHandler = _reject_entity

    def _start(tag: str, attrs: dict) -> None:
        target.start(tag, attrs)

    def _end(tag: str) -> None:
        target.end(tag)

    def _data(data: str) -> None:
        target.data(data)

    expat_parser.StartElementHandler = _start
    expat_parser.EndElementHandler = _end
    expat_parser.CharacterDataHandler = _data

    with open(path, "rb") as fh:
        expat_parser.ParseFile(fh)

    root = target.close()
    tree = ET.ElementTree(root)
    return tree

MIN_JUNIT_TESTCASES = 3
MAX_ARTIFACT_BYTES = 64 * 1024 * 1024


def validate_junit_xml(path: Path, module_name: str) -> list[str]:
    """Validate a JUnit XML artifact carries real test evidence."""
    errors: list[str] = []
    try:
        size = path.stat().st_size
    except OSError as exc:
        return [f"junit-xml artifact {path.name} is unreadable: {exc}"]
    if size == 0:
        return [
            f"junit-xml artifact {path.name} is zero bytes — an empty file "
            f"is not test evidence"
        ]
    if size > MAX_ARTIFACT_BYTES:
        return [
            f"junit-xml artifact {path.name} is {size} bytes, exceeding the "
            f"{MAX_ARTIFACT_BYTES} byte limit"
        ]
    try:
        tree = _safe_parse_xml(path)
    except ET.ParseError as exc:
        return [f"junit-xml artifact {path.name} is not valid XML: {exc}"]
    except Exception as exc:
        return [f"junit-xml artifact {path.name} cannot be parsed: {exc}"]

    root = tree.getroot()
    if root.tag not in ("testsuites", "testsuite"):
        errors.append(
            f"junit-xml root element is <{root.tag}>, expected <testsuites> "
            f"or <testsuite>"
        )
        return errors

    tests_attr = root.get("tests")
    if tests_attr is None:
        suites = root.findall(".//testsuite")
        if suites:
            total = 0
            for suite in suites:
                t = suite.get("tests")
                if t is not None:
                    try:
                        total += int(t)
                    except ValueError:
                        errors.append(
                            f"junit-xml testsuite tests attribute {t!r} is "
                            f"not an integer"
                        )
            if total == 0 and not errors:
                errors.append(
                    "junit-xml declares zero tests across all testsuites — "
                    "a test run with no tests proves nothing"
                )
        else:
            errors.append(
                "junit-xml has no tests attribute and no <testsuite> children"
            )
    else:
        try:
            tests_count = int(tests_attr)
        except ValueError:
            errors.append(
                f"junit-xml tests attribute {tests_attr!r} is not an integer"
            )
            tests_count = -1
        if tests_count == 0:
            errors.append(
                "junit-xml declares tests=\"0\" — a test run with zero tests "
                "proves nothing"
            )

    for attr in ("failures", "errors"):
        val = root.get(attr)
        if val is not None:
            try:
                int(val)
            except ValueError:
                errors.append(
                    f"junit-xml {attr} attribute {val!r} is not an integer"
                )

    testcases = root.findall(".//testcase")
    if not testcases:
        errors.append(
            "junit-xml contains no <testcase> elements — a JUnit document "
            "without test cases is not test evidence"
        )
    elif len(testcases) < MIN_JUNIT_TESTCASES:
        errors.append(
            f"junit-xml contains only {len(testcases)} <testcase> element(s), "
            f"minimum is {MIN_JUNIT_TESTCASES} — a trivially small test count "
            f"does not constitute meaningful evidence"
        )
    else:
        for i, tc in enumerate(testcases):
            if tc.get("name") is None:
                errors.append(
                    f"junit-xml testcase[{i}] has no name attribute"
                )
                break
            if tc.get("classname") is None:
                errors.append(
                    f"junit-xml testcase[{i}] has no classname attribute"
                )
                break

    return errors


def _safe_parse_xml_bytes(data: bytes) -> ET.ElementTree:
    """Parse exact held XML bytes while rejecting DTDs and entity declarations."""
    from xml.parsers import expat as _expat

    target = ET.TreeBuilder()
    expat_parser = _expat.ParserCreate()

    def _reject_entity(*args: object) -> None:
        raise ET.ParseError("entity declarations are rejected")

    def _reject_external(*args: object) -> int:
        raise ET.ParseError("external entity references are rejected")

    expat_parser.EntityDeclHandler = _reject_entity
    expat_parser.ExternalEntityRefHandler = _reject_external
    expat_parser.UnparsedEntityDeclHandler = _reject_entity

    def _start(tag: str, attrs: dict) -> None:
        target.start(tag, attrs)

    def _end(tag: str) -> None:
        target.end(tag)

    def _data(value: str) -> None:
        target.data(value)

    expat_parser.StartElementHandler = _start
    expat_parser.EndElementHandler = _end
    expat_parser.CharacterDataHandler = _data
    expat_parser.Parse(data, True)
    return ET.ElementTree(target.close())


def validate_junit_xml_bytes(data: bytes, leaf_name: str, module_name: str) -> list[str]:
    """Validate JUnit evidence from exact already-held bytes."""
    errors: list[str] = []
    size = len(data)
    if size == 0:
        return [f"junit-xml artifact {leaf_name} is zero bytes — an empty file is not test evidence"]
    if size > MAX_ARTIFACT_BYTES:
        return [
            f"junit-xml artifact {leaf_name} is {size} bytes, exceeding the "
            f"{MAX_ARTIFACT_BYTES} byte limit"
        ]
    try:
        tree = _safe_parse_xml_bytes(data)
    except ET.ParseError as exc:
        return [f"junit-xml artifact {leaf_name} is not valid XML: {exc}"]
    except Exception as exc:
        return [f"junit-xml artifact {leaf_name} cannot be parsed: {exc}"]

    root = tree.getroot()
    if root.tag not in ("testsuites", "testsuite"):
        return [
            f"junit-xml root element is <{root.tag}>, expected <testsuites> "
            "or <testsuite>"
        ]
    tests_attr = root.get("tests")
    if tests_attr is None:
        suites = root.findall(".//testsuite")
        if suites:
            total = 0
            for suite in suites:
                value = suite.get("tests")
                if value is not None:
                    try:
                        total += int(value)
                    except ValueError:
                        errors.append(
                            f"junit-xml testsuite tests attribute {value!r} is not an integer"
                        )
            if total == 0 and not errors:
                errors.append(
                    "junit-xml declares zero tests across all testsuites — "
                    "a test run with no tests proves nothing"
                )
        else:
            errors.append("junit-xml has no tests attribute and no <testsuite> children")
    else:
        try:
            tests_count = int(tests_attr)
        except ValueError:
            errors.append(f"junit-xml tests attribute {tests_attr!r} is not an integer")
            tests_count = -1
        if tests_count == 0:
            errors.append(
                "junit-xml declares tests=\"0\" — a test run with zero tests proves nothing"
            )
    for attr in ("failures", "errors"):
        value = root.get(attr)
        if value is not None:
            try:
                int(value)
            except ValueError:
                errors.append(f"junit-xml {attr} attribute {value!r} is not an integer")
    testcases = root.findall(".//testcase")
    if not testcases:
        errors.append("junit-xml contains no <testcase> elements — a JUnit document without test cases is not test evidence")
    elif len(testcases) < MIN_JUNIT_TESTCASES:
        errors.append(
            f"junit-xml contains only {len(testcases)} <testcase> element(s), "
            f"minimum is {MIN_JUNIT_TESTCASES} — a trivially small test count "
            "does not constitute meaningful evidence"
        )
    else:
        for index, testcase in enumerate(testcases):
            if testcase.get("name") is None:
                errors.append(f"junit-xml testcase[{index}] has no name attribute")
                break
            if testcase.get("classname") is None:
                errors.append(f"junit-xml testcase[{index}] has no classname attribute")
                break
    return errors


PACKAGE_SMOKE_PREFIX = "[package-smoke] "
PACKAGE_SMOKE_SCHEMA = "package-smoke-v1"
_SHA1_RE = re.compile(r"[0-9a-f]{40}")
_SHA256_RE = re.compile(r"[0-9a-f]{64}")
_PACKAGE_SMOKE_FIXED_FIELDS = {
    "schema": f"schema={PACKAGE_SMOKE_SCHEMA}",
    "product": "product=SparkEngine",
    "profile": "profile=stable-v1",
    "backend:nullrhi": "backend=nullrhi result=PASS",
    "backend:d3d11-warp": "backend=d3d11-warp result=PASS",
    "exit_code": "exit_code=0",
    "PASS": "PASS",
}
_PACKAGE_SMOKE_ALLOWED_FIELDS = frozenset({
    *_PACKAGE_SMOKE_FIXED_FIELDS,
    "module",
    "commit_sha",
    "msi_sha256",
})


def _parse_package_smoke_fields(text: str, leaf_name: str) -> tuple[dict[str, str], list[str]]:
    """Decode the closed line format without accepting decorative log output."""
    fields: dict[str, str] = {}
    errors: list[str] = []
    terminal_pass_seen = False
    for number, raw_line in enumerate(text.splitlines(), start=1):
        if not raw_line.strip():
            errors.append(
                f"package-smoke artifact {leaf_name} line {number} is a blank record"
            )
            continue
        if terminal_pass_seen:
            errors.append(
                f"package-smoke artifact {leaf_name} line {number} appears after terminal PASS"
            )
        if not raw_line.startswith(PACKAGE_SMOKE_PREFIX):
            errors.append(
                f"package-smoke artifact {leaf_name} line {number} is not a canonical package-smoke record"
            )
            continue
        record = raw_line[len(PACKAGE_SMOKE_PREFIX):]
        if record == "PASS":
            key = "PASS"
        elif record.startswith("backend="):
            parts = record.split(" ")
            if len(parts) != 2 or not parts[0][len("backend="):] or not parts[1].startswith("result="):
                errors.append(
                    f"package-smoke artifact {leaf_name} line {number} has malformed backend result"
                )
                continue
            key = f"backend:{parts[0][len('backend='):]}"
        elif record.count("=") == 1 and " " not in record:
            key = record.split("=", 1)[0]
            if not key:
                errors.append(
                    f"package-smoke artifact {leaf_name} line {number} has an empty field name"
                )
                continue
        else:
            errors.append(
                f"package-smoke artifact {leaf_name} line {number} has malformed record {record!r}"
            )
            continue
        if key in fields:
            errors.append(f"package-smoke artifact {leaf_name} duplicates field {key!r}")
            continue
        fields[key] = record
        if key == "PASS":
            terminal_pass_seen = True
    return fields, errors


def _validate_package_smoke_text(
    text: str, leaf_name: str, module_name: str, *, expected_sha: str | None,
) -> list[str]:
    """Require exact package identity and both installed backend observations."""
    fields, errors = _parse_package_smoke_fields(text, leaf_name)
    for key in sorted(set(fields) - _PACKAGE_SMOKE_ALLOWED_FIELDS):
        errors.append(f"package-smoke artifact {leaf_name} has unknown field {key!r}")
    for key, expected in _PACKAGE_SMOKE_FIXED_FIELDS.items():
        actual = fields.get(key)
        if actual is None:
            errors.append(f"package-smoke artifact {leaf_name} is missing required field {key!r}")
        elif actual != expected:
            errors.append(
                f"package-smoke artifact {leaf_name} field {key!r} must be {expected!r}, got {actual!r}"
            )
    module_record = fields.get("module")
    expected_module = f"module={module_name}"
    if module_record is None:
        errors.append(f"package-smoke artifact {leaf_name} is missing required module identity")
    elif module_record != expected_module:
        errors.append(
            f"package-smoke artifact {leaf_name} module identity must be {module_name!r}, got {module_record!r}"
        )
    commit_record = fields.get("commit_sha")
    if commit_record is None:
        errors.append(f"package-smoke artifact {leaf_name} is missing required commit SHA")
    else:
        commit_sha = commit_record.removeprefix("commit_sha=")
        if not _SHA1_RE.fullmatch(commit_sha):
            errors.append(f"package-smoke artifact {leaf_name} has invalid lower-case commit SHA")
        elif expected_sha is None:
            errors.append(
                "no expected package-smoke SHA was established, so package evidence "
                "cannot be bound to the revision under test"
            )
        elif not _SHA1_RE.fullmatch(expected_sha):
            errors.append("expected package-smoke SHA is not a lower-case 40-character Git revision")
        elif commit_sha != expected_sha:
            errors.append(
                f"package-smoke artifact {leaf_name} commit SHA {commit_sha} does not match expected {expected_sha}"
            )
    digest_record = fields.get("msi_sha256")
    if digest_record is None:
        errors.append(f"package-smoke artifact {leaf_name} is missing required MSI SHA-256")
    elif not _SHA256_RE.fullmatch(digest_record.removeprefix("msi_sha256=")):
        errors.append(f"package-smoke artifact {leaf_name} has invalid lower-case MSI SHA-256")
    return errors


def validate_package_smoke_bytes(
    data: bytes, leaf_name: str, module_name: str, *, expected_sha: str | None = None,
) -> list[str]:
    """Validate package-smoke evidence from exact already-held bytes."""
    size = len(data)
    if size == 0:
        return [
            f"package-smoke artifact {leaf_name} is zero bytes — an empty file is not package evidence"
        ]
    if size > MAX_ARTIFACT_BYTES:
        return [
            f"package-smoke artifact {leaf_name} is {size} bytes, exceeding "
            f"the {MAX_ARTIFACT_BYTES} byte limit"
        ]
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        return [f"package-smoke artifact {leaf_name} is not UTF-8: {exc}"]
    return _validate_package_smoke_text(text, leaf_name, module_name, expected_sha=expected_sha)


def validate_package_smoke(
    path: Path, module_name: str, *, expected_sha: str | None = None,
) -> list[str]:
    """Validate a package-smoke log through the same closed byte contract."""
    try:
        size = path.stat().st_size
    except OSError as exc:
        return [f"package-smoke artifact {path.name} is unreadable: {exc}"]
    if size == 0:
        return [
            f"package-smoke artifact {path.name} is zero bytes — an empty file is not package evidence"
        ]
    if size > MAX_ARTIFACT_BYTES:
        return [
            f"package-smoke artifact {path.name} is {size} bytes, exceeding "
            f"the {MAX_ARTIFACT_BYTES} byte limit"
        ]
    try:
        data = path.read_bytes()
    except OSError as exc:
        return [f"package-smoke artifact {path.name} is unreadable: {exc}"]
    return validate_package_smoke_bytes(data, path.name, module_name, expected_sha=expected_sha)


def validate_artifact_bytes(
    data: bytes, leaf_name: str, evidence_type: str, module_name: str,
    *, expected_sha: str | None = None,
) -> list[str]:
    """Dispatch semantic validation for exact already-held artifact bytes."""
    if evidence_type == "junit-xml":
        return validate_junit_xml_bytes(data, leaf_name, module_name)
    if evidence_type == "package-smoke-log":
        return validate_package_smoke_bytes(
            data, leaf_name, module_name, expected_sha=expected_sha,
        )
    return []


def validate_artifact(
    path: Path, evidence_type: str, module_name: str, *, expected_sha: str | None = None,
) -> list[str]:
    """Dispatch semantic validation for one evidence artifact."""
    if evidence_type == "junit-xml":
        return validate_junit_xml(path, module_name)
    if evidence_type == "package-smoke-log":
        return validate_package_smoke(path, module_name, expected_sha=expected_sha)
    return []
