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
MIN_SMOKE_LINES = 3
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


_PASS_RE = re.compile(
    r"(?:exit_code\s*=\s*0|(?:^|\s)PASS(?:\s|$)|(?:^|\s)OK(?:\s|$))",
    re.IGNORECASE,
)


def validate_package_smoke(path: Path, module_name: str) -> list[str]:
    """Validate a package-smoke log carries real smoke-test evidence."""
    errors: list[str] = []
    try:
        size = path.stat().st_size
    except OSError as exc:
        return [f"package-smoke artifact {path.name} is unreadable: {exc}"]
    if size == 0:
        return [
            f"package-smoke artifact {path.name} is zero bytes — an empty "
            f"file is not package evidence"
        ]
    if size > MAX_ARTIFACT_BYTES:
        return [
            f"package-smoke artifact {path.name} is {size} bytes, exceeding "
            f"the {MAX_ARTIFACT_BYTES} byte limit"
        ]
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [f"package-smoke artifact {path.name} is unreadable: {exc}"]

    non_empty = [line for line in text.splitlines() if line.strip()]
    if len(non_empty) < MIN_SMOKE_LINES:
        errors.append(
            f"package-smoke log has only {len(non_empty)} non-empty line(s), "
            f"minimum is {MIN_SMOKE_LINES} — a trivially short log is not "
            f"meaningful evidence"
        )

    has_module_ref = any(module_name in line for line in non_empty)
    if not has_module_ref:
        errors.append(
            f"package-smoke log does not mention module {module_name!r} — "
            f"evidence must identify the module it covers"
        )

    has_pass = any(_PASS_RE.search(line) for line in non_empty)
    if not has_pass:
        errors.append(
            "package-smoke log contains no pass indicator (exit_code=0, "
            "PASS, or OK) — evidence must record a successful outcome"
        )

    return errors


def validate_package_smoke_bytes(data: bytes, leaf_name: str, module_name: str) -> list[str]:
    """Validate package-smoke evidence from exact already-held bytes."""
    errors: list[str] = []
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
    text = data.decode("utf-8", errors="replace")
    non_empty = [line for line in text.splitlines() if line.strip()]
    if len(non_empty) < MIN_SMOKE_LINES:
        errors.append(
            f"package-smoke log has only {len(non_empty)} non-empty line(s), "
            f"minimum is {MIN_SMOKE_LINES} — a trivially short log is not meaningful evidence"
        )
    if not any(module_name in line for line in non_empty):
        errors.append(
            f"package-smoke log does not mention module {module_name!r} — evidence must identify the module it covers"
        )
    if not any(_PASS_RE.search(line) for line in non_empty):
        errors.append(
            "package-smoke log contains no pass indicator (exit_code=0, PASS, or OK) — evidence must record a successful outcome"
        )
    return errors


def validate_artifact_bytes(
    data: bytes, leaf_name: str, evidence_type: str, module_name: str,
) -> list[str]:
    """Dispatch semantic validation for exact already-held artifact bytes."""
    validators = {
        "junit-xml": validate_junit_xml_bytes,
        "package-smoke-log": validate_package_smoke_bytes,
    }
    validator = validators.get(evidence_type)
    if validator is None:
        return []
    return validator(data, leaf_name, module_name)


def validate_artifact(
    path: Path, evidence_type: str, module_name: str
) -> list[str]:
    """Dispatch semantic validation for one evidence artifact."""
    validators = {
        "junit-xml": validate_junit_xml,
        "package-smoke-log": validate_package_smoke,
    }
    validator = validators.get(evidence_type)
    if validator is None:
        return []
    return validator(path, module_name)
