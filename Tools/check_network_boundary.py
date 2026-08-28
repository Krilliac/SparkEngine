#!/usr/bin/env python3
"""Fail CI when first-party network listeners or shipped configs reopen NET-100."""

from __future__ import annotations

import ipaddress
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = (
    ROOT / "SparkEngine" / "Source",
    ROOT / "SparkEditor" / "Source",
    ROOT / "SparkServer" / "src",
    ROOT / "GameModules",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
FORBIDDEN_SOURCE = (
    (re.compile(r"\bINADDR_ANY\b"), "wildcard IPv4 bind"),
    (re.compile(r"\bINADDR_BROADCAST\b"), "limited IPv4 broadcast destination"),
    (re.compile(r"\bNetworkBindScope\s*::\s*AllInterfaces\b"), "retired all-interface bind scope"),
    (re.compile(r"\bUseLoopbackNetworkBind\b"), "retired mutable bind helper"),
    (re.compile(r"\bShouldUseLoopbackForUnauthenticatedTool\b"), "retired boolean tool-bind helper"),
)
RFC1918 = tuple(
    ipaddress.ip_network(value)
    for value in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")
)


def report(path: Path, line: int, message: str) -> None:
    print(f"{path.relative_to(ROOT)}:{line}: {message}")


def check_sources() -> int:
    failures = 0
    for source_root in SOURCE_ROOTS:
        for path in source_root.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            if "ThirdParty" in path.parts or "third_party" in path.parts:
                continue
            for line_number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                for pattern, description in FORBIDDEN_SOURCE:
                    if pattern.search(line):
                        report(path, line_number, f"forbidden {description}")
                        failures += 1
    return failures


def check_admission_order() -> int:
    failures = 0
    checks = (
        (
            ROOT / "SparkEngine" / "Source" / "Engine" / "Networking" / "NetworkConnection.cpp",
            "if (!IsEndpointAllowed(senderAddr))",
            "DeserializeMessage(rawData.data()",
            "gameplay peer admission must precede message deserialization",
        ),
        (
            ROOT / "SparkEngine" / "Source" / "Engine" / "Networking" / "NetworkManager.cpp",
            "if (!IsEndpointAllowed(addr))",
            "sendto(m_socket",
            "endpoint admission must precede the final gameplay send",
        ),
        (
            ROOT / "GameModules" / "SparkGameMMOFPS" / "Source" / "Game" / "TFLanDiscoveryScan.cpp",
            "!m_endpointPolicy.AllowsPeerAddress",
            "std::memcpy(&beacon",
            "discovery peer admission must precede beacon deserialization",
        ),
        (
            ROOT / "SparkEditor" / "Source" / "Communication" / "CollaborativeEditSession.cpp",
            "!endpointPolicy.AllowsPeerAddress(remoteAddress)",
            "m_clientSocket = ToStoredSocket",
            "collaboration destination admission must precede client socket creation",
        ),
        (
            ROOT / "SparkEditor" / "Source" / "Communication" / "LiveEditBridge.cpp",
            "!endpointPolicy.AllowsPeerAddress(serverAddress)",
            "m_socket = m_socketFactory",
            "live-edit destination admission must precede socket creation",
        ),
        (
            ROOT / "GameModules" / "SparkGameMMOFPS" / "Source" / "Net" / "TFClientNet.cpp",
            "GetTFMessageSecurityMetadata(id)",
            "msg.payload.resize(size)",
            "Terrafront credential metadata must be applied before payload allocation",
        ),
    )
    for path, guard, boundary, message in checks:
        text = path.read_text(encoding="utf-8", errors="replace")
        guard_offset = text.find(guard)
        boundary_offset = text.find(boundary)
        if guard_offset < 0 or boundary_offset < 0 or guard_offset > boundary_offset:
            report(path, 1, message)
            failures += 1
    return failures


def check_required_wiring() -> int:
    failures = 0
    checks = (
        (
            ROOT / "SparkEditor" / "Source" / "Communication" / "LiveEditBridge.cpp",
            (
                "m_endpointPolicy.AllowsPeerAddress(m_serverAddressNumeric)",
                "::bind(ToNativeSocket(m_socket)",
                "SO_ERROR",
            ),
            "live-edit endpoint policy/bind/connect-result wiring is incomplete",
        ),
        (
            ROOT / "GameModules" / "SparkGameMMOFPS" / "Source" / "Game" / "TFLanDiscovery.cpp",
            (
                "GetDiscoveryConfiguration()",
                "configuration.allowAdvertisement",
                "m_endpointPolicy.BroadcastAddress()",
            ),
            "Terrafront discovery must consume the authoritative active policy and broadcast option",
        ),
    )
    for path, fragments, message in checks:
        text = path.read_text(encoding="utf-8", errors="replace")
        if any(fragment not in text for fragment in fragments):
            report(path, 1, message)
            failures += 1
    return failures


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
    failures = check_sources() + check_admission_order() + check_required_wiring()
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
