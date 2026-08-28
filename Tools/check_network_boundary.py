#!/usr/bin/env python3
"""Fail CI when first-party network listeners or shipped configs reopen NET-100."""

from __future__ import annotations

import ipaddress
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional, Sequence


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = (
    ROOT / "SparkEngine" / "Source",
    ROOT / "SparkEditor" / "Source",
    ROOT / "SparkServer" / "src",
    ROOT / "GameModules",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
RAW_PRIMITIVE = re.compile(
    r"(?<![A-Za-z0-9_:.])(?:::)?(socket|bind|connect|sendto|recvfrom|accept|listen)\s*\("
)
FORBIDDEN_SOURCE = (
    (re.compile(r"\bINADDR_ANY\b"), "wildcard IPv4 bind"),
    (re.compile(r"\bINADDR_BROADCAST\b"), "limited IPv4 broadcast destination"),
    (re.compile(
        r"\b(?:AF_INET6|PF_INET6|AF_UNSPEC|PF_UNSPEC|sockaddr_in6|sockaddr_storage|in6addr_any|"
        r"IN6ADDR_ANY(?:_INIT)?|getaddrinfo|GetAddrInfoW|freeaddrinfo)\b",
        re.I,
    ), "unreviewed IPv6 endpoint or address-family fallback"),
    (re.compile(r"\bNetworkBindScope\s*::\s*AllInterfaces\b"), "retired all-interface bind scope"),
    (re.compile(r"\bUseLoopbackNetworkBind\b"), "retired mutable bind helper"),
    (re.compile(r"\bShouldUseLoopbackForUnauthenticatedTool\b"), "retired boolean tool-bind helper"),
)
WILDCARD_INET_ADDR = re.compile(r"\binet_addr\s*\(\s*([\"'])0\.0\.0\.0\1\s*\)")
RFC1918 = tuple(
    ipaddress.ip_network(value)
    for value in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")
)


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    message: str


@dataclass(frozen=True)
class RawSocketAllowance:
    """One reviewed function and its exact raw-socket surface."""

    path: str
    function: str
    operations: tuple[tuple[str, int], ...]
    required_order: tuple[str, ...] = ()
    reviewed_exception: str = ""


@dataclass(frozen=True)
class ControlPathCheck:
    path: str
    function: str
    required_order: tuple[str, ...]
    message: str


RAW_SOCKET_ALLOWLIST = (
    RawSocketAllowance(
        "SparkEditor/Source/Communication/LiveEditBridge.cpp",
        "LiveEditBridge::LiveEditBridge",
        (("socket", 1),),
        reviewed_exception="deferred factory; invocation is guarded in LiveEditBridge::Connect",
    ),
    RawSocketAllowance(
        "SparkEditor/Source/Communication/LiveEditBridge.cpp",
        "LiveEditBridge::Connect",
        (("bind", 1), ("connect", 2)),
        (
            r"endpointPolicy\.IsValid\s*\(",
            r"endpointPolicy\.AllowsPeerAddress\s*\(",
            r"m_socketFactory\s*\(",
            r"endpointPolicy\.BindAddress\s*\(",
            r"::bind\s*\(",
            r"::connect\s*\(",
            r"SO_ERROR",
        ),
    ),
    RawSocketAllowance(
        "SparkEditor/Source/Communication/CollaborativeEditSession.cpp",
        "ConnectWithTimeout",
        (("connect", 2),),
        (r"endpointPolicy\.IsValid\s*\(", r"endpointPolicy\.AllowsPeerAddress\s*\(", r"::connect\s*\("),
    ),
    RawSocketAllowance(
        "SparkEditor/Source/Communication/CollaborativeEditSession.cpp",
        "CollaborativeEditSession::Host",
        (("socket", 1), ("bind", 1), ("listen", 1)),
        (
            r"endpointPolicy\.IsValid\s*\(",
            r"::socket\s*\(",
            r"m_endpointPolicy\.BindAddress\s*\(",
            r"::bind\s*\(",
            r"::listen\s*\(",
        ),
    ),
    RawSocketAllowance(
        "SparkEditor/Source/Communication/CollaborativeEditSession.cpp",
        "CollaborativeEditSession::Connect",
        (("socket", 1), ("bind", 1)),
        (
            r"endpointPolicy\.IsValid\s*\(",
            r"endpointPolicy\.AllowsPeerAddress\s*\(",
            r"::socket\s*\(",
            r"m_endpointPolicy\.BindAddress\s*\(",
            r"::bind\s*\(",
            r"ConnectWithTimeout\s*\(",
        ),
    ),
    RawSocketAllowance(
        "SparkEditor/Source/Communication/CollaborativeEditSession.cpp",
        "CollaborativeEditSession::NetworkThreadHost",
        (("accept", 1),),
        (r"::accept\s*\(", r"m_endpointPolicy\.AllowsPeerAddress\s*\("),
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Utils/DaemonClient.cpp",
        "DaemonClient::Connect",
        (("socket", 1), ("connect", 1)),
        (r"socketPath\.empty\s*\(", r"socketPath\.size\s*\(", r"::socket\s*\(\s*AF_UNIX", r"::connect\s*\("),
    ),
    RawSocketAllowance(
        "GameModules/SparkGameMMOFPS/Source/Game/TFLanDiscoveryScan.cpp",
        "TFLanDiscovery::StartScanning",
        (("socket", 1), ("bind", 1)),
        (r"m_endpointPolicy\.IsValid\s*\(", r"::socket\s*\(", r"m_endpointPolicy\.BindAddress\s*\(", r"bind\s*\("),
    ),
    RawSocketAllowance(
        "GameModules/SparkGameMMOFPS/Source/Game/TFLanDiscoveryScan.cpp",
        "TFLanDiscovery::UpdateScanner",
        (("recvfrom", 2),),
        (r"recvfrom\s*\(", r"m_endpointPolicy\.AllowsPeerAddress\s*\(", r"std::memcpy\s*\(\s*&beacon"),
    ),
    RawSocketAllowance(
        "GameModules/SparkGameMMOFPS/Source/Game/TFLanDiscovery.cpp",
        "TFLanDiscovery::StartBeacon",
        (("socket", 1), ("bind", 1)),
        (
            r"m_allowAdvertisement",
            r"m_endpointPolicy\.IsValid\s*\(",
            r"::socket\s*\(",
            r"m_endpointPolicy\.BindAddress\s*\(",
            r"bind\s*\(",
        ),
    ),
    RawSocketAllowance(
        "GameModules/SparkGameMMOFPS/Source/Game/TFLanDiscovery.cpp",
        "TFLanDiscovery::BroadcastBeacon",
        (("sendto", 1),),
        (r"m_allowAdvertisement", r"m_endpointPolicy\.IsValid\s*\(", r"m_bcastTargets", r"sendto\s*\("),
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/UDPTransport.h",
        "Initialize",
        (("socket", 1), ("bind", 1)),
        (r"m_endpointPolicy\.IsValid\s*\(", r"::socket\s*\(", r"m_endpointPolicy\.BindAddress\s*\(", r"::bind\s*\("),
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/UDPTransport.h",
        "Send",
        (("sendto", 1),),
        (r"m_endpointPolicy\.AllowsPeerAddress\s*\(", r"sendto\s*\("),
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/UDPTransport.h",
        "Receive",
        (("recvfrom", 1),),
        (r"recvfrom\s*\(", r"m_endpointPolicy\.AllowsPeerAddress\s*\("),
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/NetworkManager.cpp",
        "NetworkManager::CreateSocket",
        (("socket", 1), ("bind", 1)),
        (r"endpointPolicy\.IsValid\s*\(", r"::socket\s*\(", r"endpointPolicy\.BindAddress\s*\(", r"::bind\s*\("),
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/NetworkManager.cpp",
        "NetworkManager::SendRawTo",
        (("sendto", 1),),
        (r"IsEndpointAllowed\s*\(", r"sendto\s*\("),
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/NetworkManager.cpp",
        "NetworkManager::ReceiveRaw",
        (("recvfrom", 1),),
        reviewed_exception="peer admission is enforced in ProcessIncoming before deserialization",
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/DedicatedServer.cpp",
        "DedicatedServer::DedicatedServer",
        (("socket", 2),),
        reviewed_exception="deferred factories; invocation is guarded in StartLanBroadcast",
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/DedicatedServer.cpp",
        "DedicatedServer::StartLanBroadcast",
        (("bind", 1), ("sendto", 1)),
        (
            r"m_config\.enableLanBroadcast",
            r"m_config\.endpointPolicy\.IsValid\s*\(",
            r"m_lanSocketFactory\s*\(",
            r"m_config\.endpointPolicy\.BindAddress\s*\(",
            r"::bind\s*\(",
            r"m_config\.endpointPolicy\.BroadcastAddress\s*\(",
            r"sendto\s*\(",
        ),
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/DedicatedServer.cpp",
        "DedicatedServer::DiscoverLanServers",
        (("socket", 1), ("bind", 1), ("recvfrom", 1)),
        (
            r"endpointPolicy\.IsValid\s*\(",
            r"::socket\s*\(",
            r"endpointPolicy\.BindAddress\s*\(",
            r"::bind\s*\(",
            r"recvfrom\s*\(",
            r"endpointPolicy\.AllowsPeerAddress\s*\(",
        ),
    ),
)

CONTROL_PATH_CHECKS = (
    ControlPathCheck(
        "SparkEngine/Source/Engine/Networking/NetworkConnection.cpp",
        "NetworkManager::ProcessIncoming",
        (r"ReceiveRaw\s*\(", r"IsEndpointAllowed\s*\(", r"DeserializeMessage\s*\("),
        "gameplay peer admission must precede message deserialization",
    ),
    ControlPathCheck(
        "GameModules/SparkGameMMOFPS/Source/Net/TFClientNet.cpp",
        "TFClientNet::SendMsg",
        (r"GetTFMessageSecurityMetadata\s*\(", r"msg\.sensitive", r"msg\.localOnly", r"msg\.payload\.resize\s*\("),
        "Terrafront credential metadata must precede payload allocation",
    ),
    ControlPathCheck(
        "GameModules/SparkGameMMOFPS/Source/Game/TFLanDiscovery.cpp",
        "TFLanDiscovery::RefreshEndpointConfiguration",
        (r"GetDiscoveryConfiguration\s*\(", r"configuration\.allowAdvertisement", r"m_allowAdvertisement\s*="),
        "Terrafront discovery must consume the authoritative active advertisement policy",
    ),
    ControlPathCheck(
        "GameModules/SparkGameMMOFPS/Source/Game/TFLanDiscovery.cpp",
        "TFLanDiscovery::RefreshBroadcastTargets",
        (r"m_endpointPolicy\.BindAddress\s*\(", r"m_endpointPolicy\.BroadcastAddress\s*\("),
        "Terrafront discovery targets must derive from the captured endpoint policy",
    ),
)


def report(path: Path, line: int, message: str, root: Path = ROOT) -> None:
    try:
        display_path = path.relative_to(root)
    except ValueError:
        display_path = path
    print(f"{display_path}:{line}: {message}")


def _mask_cpp(text: str, *, strings: bool) -> str:
    """Mask C/C++ comments and optionally literals while preserving offsets/newlines."""

    output = list(text)
    index = 0
    while index < len(text):
        if text.startswith("//", index):
            end = text.find("\n", index + 2)
            if end < 0:
                end = len(text)
            for cursor in range(index, end):
                output[cursor] = " "
            index = end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = len(text) if end < 0 else end + 2
            for cursor in range(index, end):
                if output[cursor] != "\n":
                    output[cursor] = " "
            index = end
            continue
        if text[index] in {'"', "'"}:
            quote = text[index]
            end = index + 1
            while end < len(text):
                if text[end] == "\\":
                    end += 2
                    continue
                end += 1
                if text[end - 1] == quote:
                    break
            if strings:
                for cursor in range(index, min(end, len(text))):
                    if output[cursor] != "\n":
                        output[cursor] = " "
            index = end
            continue
        index += 1
    return "".join(output)


def _matching_delimiter(text: str, opening: int, left: str, right: str) -> Optional[int]:
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == left:
            depth += 1
        elif text[index] == right:
            depth -= 1
            if depth == 0:
                return index
    return None


def _mask_named_call(text: str, name: str) -> str:
    output = list(text)
    for match in list(re.finditer(rf"\b{re.escape(name)}\s*\(", text)):
        opening = text.find("(", match.start())
        closing = _matching_delimiter(text, opening, "(", ")")
        if closing is None:
            continue
        for index in range(match.start(), closing + 1):
            if output[index] != "\n":
                output[index] = " "
    return "".join(output)


def _function_spans(code: str, function: str) -> list[tuple[int, int]]:
    spans: list[tuple[int, int]] = []
    pattern = re.compile(rf"(?<![A-Za-z0-9_:]){re.escape(function)}\s*\(")
    for match in pattern.finditer(code):
        opening = code.find("(", match.start())
        closing = _matching_delimiter(code, opening, "(", ")")
        if closing is None:
            continue
        paren_depth = 0
        bracket_depth = 0
        body_start: Optional[int] = None
        cursor = closing + 1
        while cursor < len(code):
            character = code[cursor]
            if character == "(":
                paren_depth += 1
            elif character == ")" and paren_depth:
                paren_depth -= 1
            elif character == "[":
                bracket_depth += 1
            elif character == "]" and bracket_depth:
                bracket_depth -= 1
            elif character == ";" and paren_depth == 0 and bracket_depth == 0:
                break
            elif character == "{" and paren_depth == 0 and bracket_depth == 0:
                body_start = cursor
                break
            cursor += 1
        if body_start is None:
            continue
        body_end = _matching_delimiter(code, body_start, "{", "}")
        if body_end is not None:
            # Include constructor initializer expressions: deferred socket
            # factories live there even though their lambda bodies precede the
            # constructor's compound statement.
            spans.append((match.start(), body_end + 1))
    return spans


def _ordered_markers_present(text: str, markers: Sequence[str]) -> bool:
    cursor = 0
    for marker in markers:
        match = re.search(marker, text[cursor:], re.MULTILINE)
        if match is None:
            return False
        cursor += match.end()
    return True


def _source_files(source_roots: Iterable[Path]) -> Iterable[Path]:
    for source_root in source_roots:
        if not source_root.is_dir():
            continue
        for path in sorted(source_root.rglob("*")):
            lowered_parts = {part.lower() for part in path.parts}
            if (path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES and "thirdparty" not in lowered_parts and
                    "third_party" not in lowered_parts):
                yield path


def _line_at(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _bind_address_findings(path: Path, code: str, occurrences: Sequence[tuple[str, int, int]],
                           containing_spans: dict[int, tuple[int, int]]) -> list[Finding]:
    findings: list[Finding] = []
    for operation, offset, line in occurrences:
        if operation != "bind":
            continue
        opening = code.find("(", offset)
        closing = _matching_delimiter(code, opening, "(", ")")
        if closing is None:
            findings.append(Finding(path, line, "cannot parse reviewed bind call"))
            continue
        address_match = re.search(r"&\s*([A-Za-z_]\w*)", code[opening:closing + 1])
        if address_match is None:
            findings.append(Finding(path, line, "bind must use a named, explicitly addressed sockaddr"))
            continue
        variable = address_match.group(1)
        span = containing_spans.get(offset)
        if span is None:
            continue
        prefix = code[span[0]:offset]
        declaration_matches = list(re.finditer(rf"\bsockaddr_in\s+{re.escape(variable)}\s*[^;]*;", prefix))
        if not declaration_matches:
            findings.append(Finding(path, line, f"bind address {variable} is not a local IPv4 sockaddr"))
            continue
        after_declaration = prefix[declaration_matches[-1].end():]
        assignments = list(re.finditer(
            rf"\b{re.escape(variable)}\s*\.\s*sin_addr\s*\.\s*s_addr\s*=\s*([^;]+);",
            after_declaration,
        ))
        if not assignments:
            findings.append(Finding(path, line, f"zero-initialized bind address {variable} has no explicit IPv4 address"))
            continue
        expression = re.sub(r"\s+", "", assignments[-1].group(1))
        if re.fullmatch(r"(?:htonl\()?(?:0[uUlL]*|INADDR_ANY)\)?", expression):
            findings.append(Finding(path, line, f"bind address {variable} resolves to the IPv4 wildcard"))
    return findings


def scan_source_text(
    source: str,
    relative_path: str,
    allowances: Sequence[RawSocketAllowance] = (),
    display_path: Optional[Path] = None,
) -> list[Finding]:
    """Scan one in-memory source file; used by production inventory and mutation tests."""

    path = display_path or Path(relative_path)
    relative = Path(relative_path).as_posix()
    path_allowances = [allowance for allowance in allowances if Path(allowance.path).as_posix() == relative]
    findings: list[Finding] = []
    comments_removed = _mask_cpp(source, strings=False)
    code = _mask_named_call(_mask_cpp(source, strings=True), "decltype")

    for pattern, description in FORBIDDEN_SOURCE:
        for match in pattern.finditer(code):
            findings.append(Finding(path, _line_at(source, match.start()), f"forbidden {description}"))
    for match in WILDCARD_INET_ADDR.finditer(comments_removed):
        findings.append(Finding(path, _line_at(source, match.start()), "forbidden inet_addr IPv4 wildcard"))

    spans_by_allowance: dict[RawSocketAllowance, list[tuple[int, int]]] = {}
    for allowance in path_allowances:
        spans = _function_spans(code, allowance.function)
        spans_by_allowance[allowance] = spans
        if not allowance.required_order and not allowance.reviewed_exception:
            findings.append(Finding(path, 1, f"raw-socket allowance lacks policy evidence: {allowance.function}"))
        if not spans:
            findings.append(Finding(path, 1, f"reviewed raw-socket function missing: {allowance.function}"))

    occurrences: list[tuple[str, int, int]] = []
    containing_span: dict[int, tuple[int, int]] = {}
    observed: dict[RawSocketAllowance, Counter[str]] = {allowance: Counter() for allowance in path_allowances}
    for match in RAW_PRIMITIVE.finditer(code):
        operation = match.group(1)
        occurrence = (operation, match.start(), _line_at(source, match.start()))
        occurrences.append(occurrence)
        owners = [
            (allowance, span)
            for allowance, spans in spans_by_allowance.items()
            for span in spans
            if span[0] <= match.start() < span[1]
        ]
        if len(owners) != 1:
            findings.append(Finding(path, occurrence[2], f"unreviewed raw {operation} site"))
            continue
        allowance, span = owners[0]
        containing_span[match.start()] = span
        observed[allowance][operation] += 1

    findings.extend(_bind_address_findings(path, code, occurrences, containing_span))
    for allowance in path_allowances:
        spans = spans_by_allowance[allowance]
        if not spans:
            continue
        expected = Counter(dict(allowance.operations))
        if observed[allowance] != expected:
            findings.append(Finding(
                path,
                _line_at(source, spans[0][0]),
                f"raw-socket inventory changed in {allowance.function}: expected {dict(expected)}, "
                f"observed {dict(observed[allowance])}",
            ))
        bodies = "\n".join(code[start:end] for start, end in spans)
        if allowance.required_order and not _ordered_markers_present(bodies, allowance.required_order):
            findings.append(Finding(
                path,
                _line_at(source, spans[0][0]),
                f"policy/control-path markers incomplete in {allowance.function}",
            ))
    return findings


def scan_raw_socket_inventory(
    root: Path = ROOT,
    source_roots: Optional[Iterable[Path]] = None,
    allowances: Sequence[RawSocketAllowance] = RAW_SOCKET_ALLOWLIST,
) -> list[Finding]:
    """Return all unreviewed or policy-incomplete raw socket sites under explicit roots."""

    roots = tuple(source_roots) if source_roots is not None else SOURCE_ROOTS
    findings: list[Finding] = []
    for path in _source_files(roots):
        relative = path.relative_to(root).as_posix()
        findings.extend(scan_source_text(
            path.read_text(encoding="utf-8", errors="replace"),
            relative,
            allowances,
            path,
        ))
    for allowance in allowances:
        expected_path = root / allowance.path
        if not expected_path.exists():
            findings.append(Finding(expected_path, 1, f"reviewed raw-socket file missing: {allowance.function}"))
    return findings


def check_control_paths(root: Path = ROOT, checks: Sequence[ControlPathCheck] = CONTROL_PATH_CHECKS) -> list[Finding]:
    findings: list[Finding] = []
    for check in checks:
        path = root / check.path
        if not path.is_file():
            findings.append(Finding(path, 1, check.message))
            continue
        original = path.read_text(encoding="utf-8", errors="replace")
        code = _mask_cpp(original, strings=True)
        spans = _function_spans(code, check.function)
        bodies = "\n".join(code[start:end] for start, end in spans)
        if not spans or not _ordered_markers_present(bodies, check.required_order):
            findings.append(Finding(path, _line_at(original, spans[0][0]) if spans else 1, check.message))
    return findings


def is_allowed_bind(value: str) -> bool:
    if value in {"local", "localhost", "loopback"}:
        return True
    if "/" not in value:
        try:
            address = ipaddress.ip_address(value)
        except ValueError:
            return False
        return (
            isinstance(address, ipaddress.IPv4Address)
            and str(address) == value
            and address.is_loopback
            and address not in {ipaddress.IPv4Address("127.0.0.0"), ipaddress.IPv4Address("127.255.255.255")}
        )
    try:
        address_text, prefix_text = value.split("/", 1)
        interface = ipaddress.ip_interface(value)
    except ValueError:
        return False
    if not isinstance(interface, ipaddress.IPv4Interface):
        return False
    if str(interface.ip) != address_text or str(interface.network.prefixlen) != prefix_text:
        return False
    if interface.network.prefixlen > 30:
        return False
    if not any(interface.network.subnet_of(network) for network in RFC1918):
        return False
    return interface.ip not in {interface.network.network_address, interface.network.broadcast_address}


def check_config(path: Path) -> int:
    failures = 0
    settings: dict[str, tuple[str, int]] = {}
    section = ""
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if section != "network" or "=" not in line:
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        settings[key.lower()] = (value, line_number)

    if "lan_only" in settings:
        report(path, settings["lan_only"][1], "legacy lan_only is forbidden in shipped configuration")
        failures += 1

    bind = settings.get("bind_address")
    if bind is None:
        report(path, 1, "shipped server config must declare bind_address")
        failures += 1
    elif not is_allowed_bind(bind[0]):
        report(path, bind[1], f"disallowed bind_address={bind[0]!r}")
        failures += 1

    broadcast = settings.get("lan_broadcast")
    if broadcast is None or broadcast[0].lower() != "false":
        report(path, broadcast[1] if broadcast else 1, "shipped lan_broadcast must default to false")
        failures += 1
    return failures


def shipped_configs() -> list[Path]:
    paths = list((ROOT / "SparkServer" / "config").glob("server*.ini"))
    paths.extend((ROOT / "Templates").glob("*/Config/server.ini"))
    return sorted(paths)


def main() -> int:
    findings = scan_raw_socket_inventory() + check_control_paths()
    for finding in findings:
        report(finding.path, finding.line, finding.message)
    failures = len(findings)
    configs = shipped_configs()
    if not configs:
        print("No shipped server configurations found", file=sys.stderr)
        return 1
    for path in configs:
        failures += check_config(path)
    if failures:
        print(f"Network boundary guard failed with {failures} finding(s).")
        return 1
    print(f"Network boundary guard passed ({len(configs)} shipped configs checked).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
