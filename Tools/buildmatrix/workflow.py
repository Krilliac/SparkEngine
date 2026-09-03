#!/usr/bin/env python3
"""Fail-closed extraction of material GitHub Actions workflow semantics.

A build-matrix inventory that records only the text of ``cmake`` configure lines
cannot tell a required Windows Debug+Release lane that builds every target from
an Ubuntu-only, Debug-only, ``if: false``, ``continue-on-error`` lane that builds
one target behind a ``paths-ignore`` filter. Both produce the same configure
text. This module binds the surrounding semantics -- triggers and path filters,
runner OS, job and step ``if``, ``continue-on-error``, matrix axes and their
expanded combinations, the effective shell, and every cmake/ctest invocation
including builds, configurations, and targets -- so that weakening any of them
changes the evidence.

Everything here is deliberately intolerant: a construct this module does not
understand raises ``WorkflowError`` or is recorded as an explicit ``unresolved``
entry. Nothing is ever skipped silently.
"""

from __future__ import annotations

import itertools
import re
import shlex
from typing import Any, Iterable


class WorkflowError(RuntimeError):
    """The workflow cannot be interpreted safely."""


# --------------------------------------------------------------------------- #
# A strict YAML subset, sufficient for GitHub workflows and nothing more.
# --------------------------------------------------------------------------- #

# Resource bounds. A workflow is a small, human-authored file; anything beyond
# these limits is malformed or hostile input, and either way must not be parsed
# into an unbounded amount of work.
MAX_DOCUMENT_BYTES = 4 * 1024 * 1024
MAX_FLOW_DEPTH = 32

_BLOCK_MARKERS = {"|", "|-", "|+", ">", ">-", ">+"}
_REJECTED_PREFIXES = {
    "&": "YAML anchors",
    "*": "YAML aliases",
    "!": "YAML tags",
}


def _strip_comment(text: str) -> str:
    """Remove a trailing comment, honouring quotes and ``${{ }}`` expressions."""
    result: list[str] = []
    quote: str | None = None
    index = 0
    while index < len(text):
        char = text[index]
        if quote:
            result.append(char)
            if char == quote:
                quote = None
            index += 1
            continue
        if char in "'\"":
            quote = char
            result.append(char)
            index += 1
            continue
        if char == "#" and (not result or result[-1].isspace()):
            break
        result.append(char)
        index += 1
    return "".join(result).rstrip()


def _scalar(text: str) -> Any:
    """Convert a plain/quoted YAML scalar. Keys never come through here."""
    value = text.strip()
    if not value:
        return None
    if value[0] == '"' and value.endswith('"') and len(value) >= 2:
        return _unescape_double(value[1:-1])
    if value[0] == "'" and value.endswith("'") and len(value) >= 2:
        return value[1:-1].replace("''", "'")
    if value[0] in _REJECTED_PREFIXES and not value.startswith("${{"):
        raise WorkflowError(f"{_REJECTED_PREFIXES[value[0]]} are not supported: {text!r}")
    lowered = value.lower()
    if lowered in {"null", "~"}:
        return None
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    if re.fullmatch(r"-?\d+", value):
        return int(value)
    return value


def _unescape_double(value: str) -> str:
    out: list[str] = []
    index = 0
    while index < len(value):
        if value[index] == "\\" and index + 1 < len(value):
            following = value[index + 1]
            out.append({"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\"}.get(following, following))
            index += 2
            continue
        out.append(value[index])
        index += 1
    return "".join(out)


def _split_flow(body: str) -> list[str]:
    """Split a flow collection body on top-level commas."""
    parts: list[str] = []
    depth = 0
    quote: str | None = None
    current: list[str] = []
    for char in body:
        if quote:
            current.append(char)
            if char == quote:
                quote = None
            continue
        if char in "'\"":
            quote = char
            current.append(char)
            continue
        if char in "[{":
            depth += 1
        elif char in "]}":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append("".join(current))
            current = []
            continue
        current.append(char)
    if quote:
        raise WorkflowError(f"unterminated quote in flow collection {body!r}")
    tail = "".join(current).strip()
    if tail:
        parts.append(tail)
    return [part.strip() for part in parts if part.strip()]


def _parse_flow(text: str, depth: int = 0) -> Any:
    if depth > MAX_FLOW_DEPTH:
        raise WorkflowError(f"flow collection nested deeper than {MAX_FLOW_DEPTH} levels")
    value = text.strip()
    if value.startswith("[") and value.endswith("]"):
        return [_parse_flow(item, depth + 1) for item in _split_flow(value[1:-1])]
    if value.startswith("{") and value.endswith("}"):
        result: dict[str, Any] = {}
        for item in _split_flow(value[1:-1]):
            if ":" not in item:
                raise WorkflowError(f"flow mapping entry lacks ':': {item!r}")
            key, _, raw = item.partition(":")
            name = _key(key)
            if name in result:
                raise WorkflowError(f"duplicate YAML key {name!r} in flow mapping")
            result[name] = _parse_flow(raw, depth + 1)
        return result
    return _scalar(value)


def _key(text: str) -> str:
    key = text.strip()
    if key.startswith(("'", '"')) and len(key) >= 2 and key[0] == key[-1]:
        return key[1:-1]
    return key


class _Lines:
    """Indentation-aware cursor over the significant lines of a document."""

    def __init__(self, text: str) -> None:
        if len(text.encode("utf-8")) > MAX_DOCUMENT_BYTES:
            raise WorkflowError(f"workflow document exceeds the {MAX_DOCUMENT_BYTES} byte bound")
        if "\t" in text:
            for number, raw in enumerate(text.splitlines(), start=1):
                if "\t" in raw[: len(raw) - len(raw.lstrip())]:
                    raise WorkflowError(f"line {number}: tab in YAML indentation")
        self.raw = text.splitlines()
        self.index = 0

    def peek(self) -> tuple[int, str, int] | None:
        """Return (indent, content, line-number) of the next significant line."""
        while self.index < len(self.raw):
            raw = self.raw[self.index]
            stripped = raw.strip()
            if not stripped or stripped.startswith("#"):
                self.index += 1
                continue
            if stripped in {"---", "..."}:
                raise WorkflowError(f"line {self.index + 1}: multi-document YAML is not supported")
            content = _strip_comment(raw)
            if not content.strip():
                self.index += 1
                continue
            return len(raw) - len(raw.lstrip()), content.strip(), self.index + 1
        return None


def _read_block_scalar(lines: _Lines, marker: str, parent_indent: int) -> str:
    """Consume a ``|`` / ``>`` block, preserving comments inside the body."""
    body: list[str] = []
    indent: int | None = None
    while lines.index < len(lines.raw):
        raw = lines.raw[lines.index]
        if not raw.strip():
            body.append("")
            lines.index += 1
            continue
        current = len(raw) - len(raw.lstrip())
        if current <= parent_indent:
            break
        if indent is None:
            indent = current
        body.append(raw[indent:] if len(raw) >= indent else "")
        lines.index += 1
    while body and not body[-1].strip():
        body.pop()
    if marker.startswith(">"):
        folded: list[str] = []
        for line in body:
            if not line.strip():
                folded.append("\n")
            elif folded and folded[-1] not in {"\n", ""}:
                folded[-1] = folded[-1] + " " + line.strip()
            else:
                folded.append(line.strip())
        text = "".join(part if part == "\n" else part for part in folded)
        text = " ".join(value for value in text.split("\n") if value).strip() if "\n" not in text else text
    else:
        text = "\n".join(body)
    if marker.endswith("+"):
        return text + "\n"
    if marker.endswith("-"):
        return text
    return text + "\n" if text else text


def _parse_value(lines: _Lines, inline: str, parent_indent: int) -> Any:
    """Parse the value that follows ``key:`` -- inline, or the indented block."""
    inline = inline.strip()
    if inline in _BLOCK_MARKERS:
        return _read_block_scalar(lines, inline, parent_indent)
    if inline:
        return _parse_flow(inline)
    following = lines.peek()
    if following is None:
        return None
    if following[0] > parent_indent:
        return _parse_node(lines, following[0])
    # YAML permits a block sequence at the same indentation as its parent key,
    # which is the form GitHub's own workflow templates use for `steps:`.
    if following[0] == parent_indent and (following[1] == "-" or following[1].startswith("- ")):
        return _parse_sequence(lines, following[0])
    return None


def _parse_node(lines: _Lines, indent: int) -> Any:
    head = lines.peek()
    if head is None:
        return None
    if head[1].startswith("- "):
        return _parse_sequence(lines, indent)
    if head[1] == "-":
        return _parse_sequence(lines, indent)
    return _parse_mapping(lines, indent)


def _parse_sequence(lines: _Lines, indent: int) -> list[Any]:
    items: list[Any] = []
    while True:
        head = lines.peek()
        if head is None or head[0] != indent or not (head[1] == "-" or head[1].startswith("- ")):
            return items
        _, content, number = head
        lines.index += 1
        rest = content[2:] if content.startswith("- ") else ""
        if not rest.strip():
            following = lines.peek()
            if following is not None and following[0] > indent:
                items.append(_parse_node(lines, following[0]))
            else:
                items.append(None)
            continue
        if _looks_like_mapping(rest):
            items.append(_parse_inline_mapping_item(lines, rest, indent + 2, number))
            continue
        if rest.strip() in _BLOCK_MARKERS:
            items.append(_read_block_scalar(lines, rest.strip(), indent))
            continue
        items.append(_parse_flow(rest))


def _looks_like_mapping(text: str) -> bool:
    value = text.strip()
    if value.startswith(("[", "{", "'", '"')):
        return False
    return _split_key(value) is not None


def _split_key(text: str) -> tuple[str, str] | None:
    """Split ``key: rest`` outside quotes and ``${{ }}``; None when not a mapping."""
    quote: str | None = None
    index = 0
    while index < len(text):
        char = text[index]
        if quote:
            if char == quote:
                quote = None
            index += 1
            continue
        if char in "'\"":
            quote = char
            index += 1
            continue
        if text.startswith("${{", index):
            end = text.find("}}", index)
            if end < 0:
                raise WorkflowError(f"unterminated expression in {text!r}")
            index = end + 2
            continue
        if char == ":" and (index + 1 == len(text) or text[index + 1] in " \t"):
            return text[:index], text[index + 1 :]
        index += 1
    return None


def _parse_inline_mapping_item(lines: _Lines, rest: str, indent: int, number: int) -> dict[str, Any]:
    """A ``- key: value`` sequence item, whose siblings are indented under it."""
    result: dict[str, Any] = {}
    split = _split_key(rest)
    if split is None:
        raise WorkflowError(f"line {number}: expected 'key: value' in sequence item")
    key, inline = split
    result[_key(key)] = _parse_value(lines, inline, indent - 1)
    while True:
        head = lines.peek()
        if head is None or head[0] != indent or head[1].startswith("- "):
            return result
        split = _split_key(head[1])
        if split is None:
            return result
        lines.index += 1
        key, inline = split
        result[_key(key)] = _parse_value(lines, inline, indent)


def _parse_mapping(lines: _Lines, indent: int) -> dict[str, Any]:
    result: dict[str, Any] = {}
    while True:
        head = lines.peek()
        if head is None or head[0] != indent:
            return result
        current_indent, content, number = head
        if content.startswith("- "):
            return result
        split = _split_key(content)
        if split is None:
            raise WorkflowError(f"line {number}: unsupported YAML construct {content!r}")
        lines.index += 1
        key, inline = split
        name = _key(key)
        if name in result:
            raise WorkflowError(f"line {number}: duplicate YAML key {name!r}")
        result[name] = _parse_value(lines, inline, current_indent)


def parse_workflow_yaml(text: str) -> dict[str, Any]:
    """Parse a GitHub workflow document, refusing anything ambiguous."""
    lines = _Lines(text)
    head = lines.peek()
    if head is None:
        raise WorkflowError("workflow document is empty")
    document = _parse_mapping(lines, head[0])
    if lines.peek() is not None:
        remainder = lines.peek()
        raise WorkflowError(f"line {remainder[2]}: trailing YAML content {remainder[1]!r}")
    if not isinstance(document, dict):
        raise WorkflowError("workflow document must be a mapping")
    return document


# --------------------------------------------------------------------------- #
# Semantic model
# --------------------------------------------------------------------------- #

_PATH_FILTER_KEYS = ("paths", "paths-ignore")
_TRIGGER_FILTER_KEYS = ("branches", "branches-ignore", "tags", "tags-ignore", "types", *_PATH_FILTER_KEYS)


def runner_os_class(label: Any) -> str:
    """Classify a ``runs-on`` label. Unknown labels are never assumed benign."""
    if not isinstance(label, str) or not label:
        return "unresolved"
    if "${{" in label:
        return "unresolved"
    lowered = label.lower()
    if "windows" in lowered:
        return "windows"
    if "macos" in lowered or "darwin" in lowered:
        return "macos"
    if "ubuntu" in lowered or "linux" in lowered:
        return "linux"
    return "unknown"


def _as_list(value: Any) -> list[Any]:
    if value is None:
        return []
    return value if isinstance(value, list) else [value]


def _expand_matrix(matrix: Any) -> tuple[list[dict[str, Any]], list[str], bool]:
    """Expand a strategy matrix into its concrete combinations.

    Returns (combinations, axis names, resolved). ``resolved`` is False when the
    matrix is driven by an expression, which must become a blocking unknown
    rather than an empty -- and therefore silently harmless -- expansion.
    """
    if matrix is None:
        return [{}], [], True
    if not isinstance(matrix, dict):
        return [{}], [], False
    if isinstance(matrix, str) or any("${{" in str(key) for key in matrix):
        return [{}], [], False
    axes = {key: value for key, value in matrix.items() if key not in {"include", "exclude"}}
    for values in axes.values():
        if not isinstance(values, list):
            return [{}], sorted(axes), False
    names = sorted(axes)
    combinations = [
        dict(zip(names, values)) for values in itertools.product(*(axes[name] for name in names))
    ] if names else []

    for excluded in _as_list(matrix.get("exclude")):
        if not isinstance(excluded, dict):
            return combinations or [{}], names, False
        combinations = [
            combination
            for combination in combinations
            if not all(combination.get(key) == value for key, value in excluded.items())
        ]

    for included in _as_list(matrix.get("include")):
        if not isinstance(included, dict):
            return combinations or [{}], names, False
        matched = False
        for combination in combinations:
            if all(combination.get(key) == value for key, value in included.items() if key in combination):
                combination.update(included)
                matched = True
        if not matched:
            combinations.append(dict(included))
    return combinations or [{}], names, True


def _tri_state(value: Any) -> Any:
    """Preserve the difference between absent, false, true, and an expression."""
    if value is None:
        return "absent"
    return value


def _default_shell(section: Any) -> str | None:
    if isinstance(section, dict):
        run = section.get("run")
        if isinstance(run, dict) and isinstance(run.get("shell"), str):
            return run["shell"]
    return None


def analyze_workflow(text: str, source: str) -> dict[str, Any]:
    """Build the semantic record of one workflow file."""
    document = parse_workflow_yaml(text)
    triggers_raw = document.get("on")
    if triggers_raw is None and "on" not in document:
        raise WorkflowError(f"{source}: workflow declares no 'on' trigger block")

    events: list[dict[str, Any]] = []
    if isinstance(triggers_raw, str):
        events.append({"event": triggers_raw, "filters": {}, "pathFiltered": False})
    elif isinstance(triggers_raw, list):
        for name in triggers_raw:
            events.append({"event": str(name), "filters": {}, "pathFiltered": False})
    elif isinstance(triggers_raw, dict):
        for name, body in sorted(triggers_raw.items(), key=lambda item: str(item[0])):
            filters = {
                key: _as_list(body.get(key))
                for key in _TRIGGER_FILTER_KEYS
                if isinstance(body, dict) and body.get(key) is not None
            }
            entry: dict[str, Any] = {
                "event": str(name),
                "filters": filters,
                "pathFiltered": any(key in filters for key in _PATH_FILTER_KEYS),
            }
            if isinstance(body, dict) and body.get("cron") is not None:
                entry["cron"] = _as_list(body.get("cron"))
            if isinstance(body, dict) and isinstance(body.get("inputs"), dict):
                entry["inputs"] = sorted(body["inputs"])
            events.append(entry)
    else:
        raise WorkflowError(f"{source}: unsupported 'on' trigger form {type(triggers_raw).__name__}")

    workflow_env = document.get("env") if isinstance(document.get("env"), dict) else {}
    workflow_shell = _default_shell(document.get("defaults"))

    jobs_raw = document.get("jobs")
    if not isinstance(jobs_raw, dict) or not jobs_raw:
        raise WorkflowError(f"{source}: workflow declares no jobs")

    jobs: list[dict[str, Any]] = []
    for job_id, body in jobs_raw.items():
        if not isinstance(body, dict):
            raise WorkflowError(f"{source}: job {job_id!r} is not a mapping")
        strategy = body.get("strategy") if isinstance(body.get("strategy"), dict) else {}
        combinations, axes, matrix_resolved = _expand_matrix(strategy.get("matrix"))
        runs_on = body.get("runs-on")
        job_env = body.get("env") if isinstance(body.get("env"), dict) else {}
        job_shell = _default_shell(body.get("defaults")) or workflow_shell
        raw_permissions = body.get("permissions")
        if raw_permissions is None:
            permissions: dict[str, str] = {}
        elif isinstance(raw_permissions, dict):
            permissions = {
                str(name): str(value)
                for name, value in raw_permissions.items()
                if isinstance(name, str) and isinstance(value, str)
            }
            if len(permissions) != len(raw_permissions):
                permissions = {"__unresolved__": "true"}
        else:
            permissions = {"__unresolved__": str(raw_permissions)}

        runner_labels: list[str] = []
        for combination in combinations:
            resolved = _substitute_matrix(runs_on, combination) if isinstance(runs_on, str) else runs_on
            runner_labels.append(resolved if isinstance(resolved, str) else str(resolved))
        os_classes = sorted({runner_os_class(label) for label in runner_labels})

        steps: list[dict[str, Any]] = []
        for index, step in enumerate(_as_list(body.get("steps"))):
            if not isinstance(step, dict):
                raise WorkflowError(f"{source}: job {job_id!r} step {index} is not a mapping")
            steps.append(
                {
                    "index": index,
                    "name": step.get("name") if isinstance(step.get("name"), str) else "",
                    "id": step.get("id") if isinstance(step.get("id"), str) else "",
                    "uses": step.get("uses") if isinstance(step.get("uses"), str) else "",
                    "shell": step.get("shell") if isinstance(step.get("shell"), str) else "",
                    "if": _tri_state(step.get("if")),
                    "continueOnError": _tri_state(step.get("continue-on-error")),
                    "hasRun": isinstance(step.get("run"), str),
                    "env": sorted(step["env"]) if isinstance(step.get("env"), dict) else [],
                }
            )

        jobs.append(
            {
                "id": str(job_id),
                "name": body.get("name") if isinstance(body.get("name"), str) else "",
                "runsOn": runs_on if isinstance(runs_on, (str, list)) else None,
                "runnerLabels": sorted(set(runner_labels)),
                "runnerOsClasses": os_classes,
                "if": _tri_state(body.get("if")),
                "continueOnError": _tri_state(body.get("continue-on-error")),
                "timeoutMinutes": _tri_state(body.get("timeout-minutes")),
                "needs": [str(item) for item in _as_list(body.get("needs"))],
                "failFast": _tri_state(strategy.get("fail-fast")),
                "maxParallel": _tri_state(strategy.get("max-parallel")),
                "matrixAxes": axes,
                "matrixCombinations": combinations if combinations != [{}] else [],
                "matrixResolved": matrix_resolved,
                "defaultShell": job_shell or "",
                "permissions": dict(sorted(permissions.items())),
                "stepCount": len(steps),
                "steps": steps,
                "envNames": sorted(job_env),
            }
        )

    return {
        "source": source,
        "name": document.get("name") if isinstance(document.get("name"), str) else "",
        "events": events,
        "pathFilteredEvents": sorted(entry["event"] for entry in events if entry["pathFiltered"]),
        "defaultShell": workflow_shell or "",
        "envNames": sorted(workflow_env),
        "jobCount": len(jobs),
        "jobs": jobs,
        "_document": document,
        "_workflowEnv": workflow_env,
    }


def _substitute_matrix(text: str, combination: dict[str, Any]) -> str:
    def replace(match: re.Match[str]) -> str:
        key = match.group(1).strip()
        if key in combination:
            return str(combination[key])
        return match.group(0)

    return re.sub(r"\$\{\{\s*matrix\.([A-Za-z0-9_-]+)\s*\}\}", replace, text)


def _substitute_env(text: str, env: dict[str, Any]) -> str:
    def replace(match: re.Match[str]) -> str:
        key = match.group(1).strip()
        if key in env and isinstance(env[key], (str, int, bool)):
            return str(env[key])
        return match.group(0)

    return re.sub(r"\$\{\{\s*env\.([A-Za-z0-9_-]+)\s*\}\}", replace, text)


# --------------------------------------------------------------------------- #
# Shell command extraction
# --------------------------------------------------------------------------- #

_POWERSHELL_SHELLS = {"pwsh", "powershell"}
_BASH_SHELLS = {"bash", "sh", "zsh"}


def effective_shell(step_shell: str, job_shell: str, os_classes: Iterable[str]) -> str:
    """Resolve the shell a run block actually executes under.

    GitHub defaults to ``pwsh`` on Windows runners and ``bash`` elsewhere; a run
    block's continuation syntax depends on this, so guessing is not an option.
    """
    if step_shell:
        return step_shell
    if job_shell:
        return job_shell
    classes = set(os_classes)
    if classes == {"windows"}:
        return "pwsh"
    if classes and classes <= {"linux", "macos"}:
        return "bash"
    return "unresolved"


def logical_shell_lines(script: str, shell: str) -> list[str]:
    """Join continued lines using the continuation character of ``shell``.

    Bash continues on a trailing backslash, PowerShell on a trailing backtick,
    and cmd on a trailing caret. Applying the wrong one silently truncates a
    command's arguments, which is how a full configure line can shrink to its
    first fragment without anyone noticing.
    """
    base = shell.split()[0].lower() if shell else ""
    if base in _POWERSHELL_SHELLS:
        marker = "`"
    elif base in {"cmd"}:
        marker = "^"
    elif base in _BASH_SHELLS or base in {"", "unresolved"}:
        marker = "\\"
    else:
        raise WorkflowError(f"unsupported run shell {shell!r}")

    lines: list[str] = []
    pending = ""
    for raw in script.splitlines():
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped.endswith(marker) and not stripped.endswith(marker * 2):
            pending += stripped[: -len(marker)] + " "
            continue
        lines.append(pending + stripped)
        pending = ""
    if pending:
        raise WorkflowError("shell run block ends with an unterminated line continuation")
    return lines


_OPERATORS = {"|", "||", "&&", ";", "&"}
_PREFIX_NOISE = {"then", "do", "else", "elif", "if", "!", "time", "exec", "sudo", "command", "&"}


def shell_tokens(command: str) -> list[str]:
    protected = re.sub(r"\$\{\{.*?\}\}", lambda match: match.group(0).replace(" ", "__GH_SPACE__"), command)
    try:
        lexer = shlex.shlex(protected, posix=True, punctuation_chars="|&;")
        lexer.whitespace_split = True
        lexer.commenters = "#"
        return [token.replace("__GH_SPACE__", " ") for token in lexer]
    except ValueError as error:
        raise WorkflowError(f"cannot lex workflow command {command!r}: {error}") from error


def split_segments(tokens: list[str]) -> list[list[str]]:
    """Split a token stream into individual commands on shell operators."""
    segments: list[list[str]] = []
    current: list[str] = []
    for token in tokens:
        if token in _OPERATORS:
            if current:
                segments.append(current)
            current = []
            continue
        current.append(token)
    if current:
        segments.append(current)
    return segments


_ENV_REF = re.compile(r"^(?:\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?|\$env:([A-Za-z_][A-Za-z0-9_]*))$")


def resolve_command_word(token: str, env: dict[str, Any]) -> tuple[str, str]:
    """Resolve the command word, reporting how it was obtained.

    Returns (value, provenance) where provenance is ``literal``, ``env`` when a
    workflow ``env:`` entry supplied it, or ``unresolved`` when it comes from a
    variable this module cannot bind. ``unresolved`` is never silently dropped.
    """
    match = _ENV_REF.match(token)
    if match:
        name = match.group(1) or match.group(2)
        if name in env and isinstance(env[name], str):
            return env[name], "env"
        return token, "unresolved"
    if "${{" in token:
        return token, "unresolved"
    if token.startswith("$"):
        return token, "unresolved"
    return token, "literal"


def _basename(command: str) -> str:
    return re.split(r"[\\/]", command)[-1].lower().removesuffix(".exe")


_CMAKE_NON_CONFIGURE = {
    "--build", "--install", "--open", "--workflow", "--find-package",
    "-P", "-E", "--version", "--help", "--list-presets",
}
_CONFIGURE_VALUE_FLAGS = {
    "-B": "buildDir", "-S": "sourceDir", "-G": "generator",
    "-A": "architecture", "-T": "toolset", "--preset": "preset",
}
_INSTALLERS = {"apt", "apt-get", "brew", "dnf", "yum", "choco", "winget", "pip", "pip3"}


def classify_cmake_segment(segment: list[str], env: dict[str, Any]) -> str:
    """Return the verb of a cmake/ctest/cpack segment, or "" when it is neither."""
    index = 0
    while index < len(segment) and segment[index] in _PREFIX_NOISE:
        index += 1
    if index >= len(segment):
        return ""
    word, _ = resolve_command_word(segment[index], env)
    base = _basename(word)
    if base in {"cmake", "ctest", "cpack"}:
        return base
    return ""


def parse_commands(
    script: str,
    shell: str,
    env: dict[str, Any],
    combination: dict[str, Any],
    context: dict[str, Any],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Extract every cmake/ctest invocation from one run block.

    Returns (commands, unresolved). Anything that looks like a build-system
    invocation but cannot be bound goes into ``unresolved`` so the parity checker
    can raise it; nothing is discarded.
    """
    commands: list[dict[str, Any]] = []
    unresolved: list[dict[str, Any]] = []
    command_index = 0
    for line in logical_shell_lines(script, shell):
        resolved_line = _substitute_env(_substitute_matrix(line, combination), env)
        tokens = shell_tokens(resolved_line)
        for segment in split_segments(tokens):
            if not segment:
                continue
            index = 0
            while index < len(segment) and segment[index] in _PREFIX_NOISE:
                index += 1
            if index >= len(segment):
                continue
            word, provenance = resolve_command_word(segment[index], env)
            base = _basename(word)
            args = segment[index + 1 :]
            if provenance == "unresolved":
                # A variable command word could be cmake. Refusing to decide is
                # the only safe answer, and it must be visible.
                if _looks_like_build_tool(args):
                    unresolved.append(
                        {
                            **context,
                            "reason": "command word is a variable this inventory cannot resolve",
                            "commandWord": segment[index],
                            "command": resolved_line,
                        }
                    )
                continue
            if base not in {"cmake", "ctest", "cpack"}:
                # A build-tool invocation hidden behind another command word --
                # `echo cmake -B build`, a wrapper script, an unrecognised
                # launcher -- must not read as "no configure here".
                if (
                    base not in _INSTALLERS
                    and any(_basename(token) in {"cmake", "ctest"} for token in segment[index:])
                    and _looks_like_build_tool(args)
                ):
                    raise WorkflowError(
                        f"{context.get('job')}/{context.get('step')}: "
                        f"unparsed command containing cmake: {resolved_line}"
                    )
                continue
            record = _parse_tool_invocation(base, args, resolved_line, context, provenance)
            if record is None:
                unresolved.append(
                    {
                        **context,
                        "reason": f"unparsed {base} invocation",
                        "commandWord": word,
                        "command": resolved_line,
                    }
                )
                continue
            record["commandIndex"] = command_index
            command_index += 1
            commands.append(record)
    return commands, unresolved


def _build_matrix_option(args: list[str], name: str) -> str:
    """Read one exact long option without guessing through an opaque shell wrapper."""
    values = _build_matrix_options(args, name)
    if len(values) > 1:
        raise WorkflowError(f"build-matrix invocation repeats {name}")
    return values[0] if values else ""


def _build_matrix_options(args: list[str], name: str) -> list[str]:
    """Read a repeatable exact long option in original command order."""
    values: list[str] = []
    for index, argument in enumerate(args):
        if argument == name:
            if index + 1 >= len(args) or args[index + 1].startswith("-"):
                raise WorkflowError(f"build-matrix invocation {name} lacks a value")
            values.append(args[index + 1])
        elif argument.startswith(name + "="):
            if not argument[len(name) + 1 :]:
                raise WorkflowError(f"build-matrix invocation has an invalid {name}")
            values.append(argument[len(name) + 1 :])
    return values


def parse_build_matrix_invocations(
    script: str,
    shell: str,
    env: dict[str, Any],
    combination: dict[str, Any],
    context: dict[str, Any],
) -> list[dict[str, Any]]:
    """Extract the producer/validator commands that make build-matrix evidence trusted.

    These commands are intentionally parsed separately from CMake commands: a
    textual mention of ``capture_provenance.py`` must not be mistaken for the
    CI producer that ran before the validator.
    """
    records: list[dict[str, Any]] = []
    scripts = {
        "tools/buildmatrix/capture_provenance.py": "producer",
        "tools/buildmatrix/inventory.py": "inventory",
        "tools/buildmatrix/check_parity.py": "parity",
        "tools/buildmatrix/validate_pending_authority.py": "pending-authority",
    }
    for line in logical_shell_lines(script, shell):
        resolved_line = _substitute_env(_substitute_matrix(line, combination), env)
        for segment in split_segments(shell_tokens(resolved_line)):
            if not segment:
                continue
            index = 0
            while index < len(segment) and segment[index] in _PREFIX_NOISE:
                index += 1
            if index >= len(segment):
                continue
            word, provenance = resolve_command_word(segment[index], env)
            launcher = _basename(word)
            arguments = segment[index + 1 :]
            for script_index, argument in enumerate(arguments):
                normalized = argument.replace("\\", "/").removeprefix("./").casefold()
                kind = scripts.get(normalized)
                if kind is None:
                    continue
                # A wrapper/echo can print the script name without executing it.
                # Treat it as an explicit untrusted invocation, never as proof.
                executable = launcher in {"python", "python3", "py"}
                tool_args = arguments[script_index + 1 :]
                codemodels = _build_matrix_options(tool_args, "--codemodel")
                records.append(
                    {
                        **context,
                        "kind": kind,
                        "launcher": launcher,
                        "launcherProvenance": provenance,
                        "executable": executable,
                        "profile": _build_matrix_option(tool_args, "--profile"),
                        "buildDir": _build_matrix_option(tool_args, "--build-dir"),
                        "codemodel": codemodels[0] if len(codemodels) == 1 else "",
                        "codemodels": codemodels,
                        "inventory": _build_matrix_option(tool_args, "--inventory"),
                        "baseline": _build_matrix_option(tool_args, "--baseline"),
                        "report": _build_matrix_option(tool_args, "--report"),
                        "output": _build_matrix_option(tool_args, "--output"),
                        "build": "--build" in tool_args,
                        "command": resolved_line,
                    }
                )
    return records


def _looks_like_build_tool(args: list[str]) -> bool:
    markers = {"-B", "-S", "--build", "--preset", "--test-dir", "--config", "--install"}
    return any(argument in markers or argument.startswith(("-B", "-S", "-D", "--preset=")) for argument in args)


def _parse_tool_invocation(
    tool: str, args: list[str], command: str, context: dict[str, Any], provenance: str
) -> dict[str, Any] | None:
    if not args:
        return None
    if tool == "ctest":
        return _parse_ctest(args, command, context, provenance)
    if tool == "cpack":
        return {**context, "tool": "cpack", "kind": "package", "command": command, "commandProvenance": provenance}
    if any(argument in _CMAKE_NON_CONFIGURE for argument in args):
        if "--build" in args:
            return _parse_cmake_build(args, command, context, provenance)
        if "--install" in args:
            return _parse_cmake_install(args, command, context, provenance)
        return {
            **context, "tool": "cmake", "kind": "other", "command": command,
            "commandProvenance": provenance,
        }
    if not any(
        argument in {"-B", "--preset"} or argument.startswith(("-B", "--preset="))
        for argument in args
    ):
        return None
    return _parse_cmake_configure(args, command, context, provenance)


def _parse_cmake_configure(
    args: list[str], command: str, context: dict[str, Any], provenance: str
) -> dict[str, Any]:
    result: dict[str, Any] = {
        **context, "tool": "cmake", "kind": "configure", "command": command,
        "commandProvenance": provenance, "options": {},
    }
    index = 0
    while index < len(args):
        argument = args[index]
        if argument in _CONFIGURE_VALUE_FLAGS:
            if index + 1 >= len(args):
                raise WorkflowError(f"{context.get('job')}/{context.get('step')}: {argument} lacks a value")
            result[_CONFIGURE_VALUE_FLAGS[argument]] = args[index + 1]
            index += 2
            continue
        if argument.startswith("--preset="):
            result["preset"] = argument.split("=", 1)[1]
        elif argument == "--fresh":
            result["fresh"] = True
        elif argument.startswith("-B") and len(argument) > 2:
            result["buildDir"] = argument[2:]
        elif argument.startswith("-S") and len(argument) > 2:
            result["sourceDir"] = argument[2:]
        elif argument == "-D":
            if index + 1 >= len(args):
                raise WorkflowError(f"{context.get('job')}/{context.get('step')}: -D lacks a cache assignment")
            _record_cache(result, args[index + 1], context)
            index += 2
            continue
        elif argument.startswith("-D"):
            _record_cache(result, argument[2:], context)
        index += 1
    return result


def _record_cache(result: dict[str, Any], assignment: str, context: dict[str, Any]) -> None:
    if "=" not in assignment:
        raise WorkflowError(f"{context.get('job')}/{context.get('step')}: CMake -D lacks '=': {assignment}")
    name, value = assignment.split("=", 1)
    name = name.split(":", 1)[0]
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise WorkflowError(f"{context.get('job')}/{context.get('step')}: unsupported cache variable {name!r}")
    result["options"][name] = value


def _parse_cmake_build(
    args: list[str], command: str, context: dict[str, Any], provenance: str
) -> dict[str, Any]:
    result: dict[str, Any] = {
        **context, "tool": "cmake", "kind": "build", "command": command,
        "commandProvenance": provenance, "targets": [], "configuration": "",
        "buildDir": "", "preset": "", "parallel": "",
    }
    index = 0
    saw_build = False
    while index < len(args):
        argument = args[index]
        if argument == "--build":
            saw_build = True
            if index + 1 < len(args) and not args[index + 1].startswith("-"):
                result["buildDir"] = args[index + 1]
                index += 2
                continue
            index += 1
            continue
        if argument in {"--config", "-C"}:
            if index + 1 >= len(args):
                raise WorkflowError(f"{context.get('job')}/{context.get('step')}: --config lacks a value")
            result["configuration"] = args[index + 1]
            index += 2
            continue
        if argument == "--preset":
            if index + 1 >= len(args):
                raise WorkflowError(f"{context.get('job')}/{context.get('step')}: --preset lacks a value")
            result["preset"] = args[index + 1]
            index += 2
            continue
        if argument == "--target" or argument == "-t":
            index += 1
            while index < len(args) and not args[index].startswith("-"):
                result["targets"].append(args[index])
                index += 1
            continue
        if argument in {"--parallel", "-j"}:
            following = args[index + 1] if index + 1 < len(args) else ""
            result["parallel"] = following if following and not following.startswith("-") else "auto"
            index += 2 if following and not following.startswith("-") else 1
            continue
        if argument == "--":
            break
        index += 1
    if not saw_build or not (result["buildDir"] or result["preset"]):
        raise WorkflowError(
            f"{context.get('job')}/{context.get('step')}: --build lacks a directory or preset"
        )
    # An empty --target list means CMake builds the default `all` target. Record
    # that explicitly: absence and "everything" must never look the same.
    result["targets"] = sorted(result["targets"]) if result["targets"] else ["all"]
    result["buildsAllTargets"] = result["targets"] == ["all"]
    return result


def _parse_cmake_install(
    args: list[str], command: str, context: dict[str, Any], provenance: str
) -> dict[str, Any]:
    result: dict[str, Any] = {
        **context, "tool": "cmake", "kind": "install", "command": command,
        "commandProvenance": provenance, "configuration": "", "prefix": "", "component": "",
    }
    for index, argument in enumerate(args):
        following = args[index + 1] if index + 1 < len(args) else ""
        if argument == "--config":
            result["configuration"] = following
        elif argument == "--prefix":
            result["prefix"] = following
        elif argument == "--component":
            result["component"] = following
    return result


def _parse_ctest(
    args: list[str], command: str, context: dict[str, Any], provenance: str
) -> dict[str, Any]:
    result: dict[str, Any] = {
        **context, "tool": "ctest", "kind": "test", "command": command,
        "commandProvenance": provenance, "configuration": "", "testDir": "",
        "noTestsAction": "", "outputJUnit": "",
    }
    for index, argument in enumerate(args):
        following = args[index + 1] if index + 1 < len(args) else ""
        if argument in {"-C", "--build-config"}:
            result["configuration"] = following
        elif argument == "--test-dir":
            result["testDir"] = following
        elif argument.startswith("--no-tests="):
            result["noTestsAction"] = argument.split("=", 1)[1]
        elif argument == "--output-junit":
            result["outputJUnit"] = following
    return result


# --------------------------------------------------------------------------- #
# Whole-workflow invocation extraction
# --------------------------------------------------------------------------- #


def _job_gating(job: dict[str, Any]) -> str:
    """How strongly a job's failure is felt: blocking, advisory, or conditional."""
    if job["continueOnError"] is True:
        return "advisory"
    if job["if"] != "absent":
        return "conditional"
    return "blocking"


def extract_invocations(analysis: dict[str, Any]) -> dict[str, Any]:
    """Attach every cmake/ctest invocation, expanded per matrix combination.

    Recording one entry per (job, matrix combination, step) is what makes
    ``config: [Debug, Release]`` narrowed to ``[Debug]`` a visible diff: the
    configure text is unchanged, but a whole combination disappears.
    """
    document = analysis["_document"]
    workflow_env = analysis["_workflowEnv"]
    jobs_raw = document.get("jobs", {})
    commands: list[dict[str, Any]] = []
    unresolved: list[dict[str, Any]] = []
    build_matrix_commands: list[dict[str, Any]] = []

    for job in analysis["jobs"]:
        raw_job = jobs_raw.get(job["id"], {})
        job_env = raw_job.get("env") if isinstance(raw_job.get("env"), dict) else {}
        raw_steps = _as_list(raw_job.get("steps"))
        combinations = job["matrixCombinations"] or [{}]
        gating = _job_gating(job)
        for combination in combinations:
            for step in job["steps"]:
                raw_step = raw_steps[step["index"]] if step["index"] < len(raw_steps) else {}
                script = raw_step.get("run") if isinstance(raw_step, dict) else None
                if not isinstance(script, str):
                    continue
                step_env = raw_step.get("env") if isinstance(raw_step.get("env"), dict) else {}
                env = {**workflow_env, **job_env, **step_env}
                os_classes = (
                    [runner_os_class(_substitute_matrix(job["runsOn"], combination))]
                    if isinstance(job["runsOn"], str)
                    else job["runnerOsClasses"]
                )
                shell = effective_shell(step["shell"], job["defaultShell"], os_classes)
                context = {
                    "job": job["id"],
                    "step": step["name"] or f"step[{step['index']}]",
                    "stepIndex": step["index"],
                    "matrix": dict(sorted(combination.items())),
                    "matrixResolved": job["matrixResolved"],
                    "runnerOs": os_classes[0] if len(os_classes) == 1 else "multiple",
                    "shell": shell,
                    "jobIf": job["if"],
                    "jobContinueOnError": job["continueOnError"],
                    "stepIf": step["if"],
                    "stepContinueOnError": step["continueOnError"],
                    "gating": gating,
                }
                if shell == "unresolved":
                    unresolved.append(
                        {**context, "reason": "cannot resolve the shell for this run block", "command": ""}
                    )
                    continue
                found, missed = parse_commands(script, shell, env, combination, context)
                commands.extend(found)
                unresolved.extend(missed)
                build_matrix_commands.extend(parse_build_matrix_invocations(script, shell, env, combination, context))

    analysis["cmakeInvocations"] = commands
    analysis["unresolvedInvocations"] = unresolved
    analysis["configureInvocations"] = [item for item in commands if item["kind"] == "configure"]
    analysis["buildInvocations"] = [item for item in commands if item["kind"] == "build"]
    analysis["testInvocations"] = [item for item in commands if item["kind"] == "test"]
    analysis["buildMatrixInvocations"] = build_matrix_commands
    return analysis


def _digest(text: str) -> str:
    import hashlib

    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


def build_workflow_record(text: str, source: str) -> dict[str, Any]:
    """The serializable, fully bound semantic record for one workflow."""
    analysis = extract_invocations(analyze_workflow(text, source))
    document = analysis.pop("_document")
    analysis.pop("_workflowEnv")
    jobs_raw = document.get("jobs", {})
    for job in analysis["jobs"]:
        raw_steps = _as_list(jobs_raw.get(job["id"], {}).get("steps"))
        for step in job["steps"]:
            raw = raw_steps[step["index"]] if step["index"] < len(raw_steps) else {}
            script = raw.get("run") if isinstance(raw, dict) else None
            step["runDigest"] = _digest(script) if isinstance(script, str) else ""
        job["gating"] = _job_gating(job)

    matrix_legs = sum(len(job["matrixCombinations"]) or 1 for job in analysis["jobs"])
    analysis["summary"] = {
        "jobCount": analysis["jobCount"],
        "matrixLegCount": matrix_legs,
        "pathFilteredEventCount": len(analysis["pathFilteredEvents"]),
        "configureCount": len(analysis["configureInvocations"]),
        "buildCount": len(analysis["buildInvocations"]),
        "testCount": len(analysis["testInvocations"]),
        "buildMatrixInvocationCount": len(analysis["buildMatrixInvocations"]),
        "unresolvedCount": len(analysis["unresolvedInvocations"]),
        "buildAllTargetCount": sum(
            1 for item in analysis["buildInvocations"] if item.get("buildsAllTargets")
        ),
        "blockingJobCount": sum(1 for job in analysis["jobs"] if job["gating"] == "blocking"),
        "advisoryJobCount": sum(1 for job in analysis["jobs"] if job["gating"] == "advisory"),
        "conditionalJobCount": sum(1 for job in analysis["jobs"] if job["gating"] == "conditional"),
        "builtOsConfigurationPairs": sorted(
            {
                f"{item['runnerOs']}:{item.get('configuration') or 'default'}"
                for item in analysis["buildInvocations"]
            }
        ),
        "testedOsConfigurationPairs": sorted(
            {
                f"{item['runnerOs']}:{item.get('configuration') or 'default'}"
                for item in analysis["testInvocations"]
            }
        ),
    }
    return analysis
