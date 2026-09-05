#!/usr/bin/env python3
"""Fail CI when first-party network listeners or shipped configs reopen NET-100."""

from __future__ import annotations

import ipaddress
import re
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional, Sequence


ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = (
    # Keep this inventory explicit.  These are the first-party source trees
    # that produce shipped executables, libraries, SDK headers, and project
    # templates.  Tests, build output, generated code, and third-party code are
    # intentionally not roots (and are also filtered defensively below).
    ROOT / "SparkAssetPipelineCore" / "include",
    ROOT / "SparkAssetPipelineCore" / "src",
    ROOT / "SparkAutomation" / "src",
    ROOT / "SparkBuild" / "resources",
    ROOT / "SparkBuild" / "src",
    ROOT / "cmake",
    ROOT / "SparkConsole" / "src",
    ROOT / "SparkCooker" / "src",
    ROOT / "SparkCrashReporter" / "src",
    ROOT / "SparkDaemon" / "src",
    ROOT / "SparkEditor" / "Source",
    ROOT / "SparkEngine" / "Source",
    ROOT / "SparkGateway" / "src",
    ROOT / "SparkInstaller" / "src",
    ROOT / "SparkLauncher" / "src",
    ROOT / "SparkSDK" / "Include",
    ROOT / "SparkServer" / "src",
    ROOT / "SparkShaderCompiler" / "src",
    ROOT / "SparkWorker" / "src",
    ROOT / "GameModules",
    ROOT / "Templates",
    ROOT / "Tools",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".mm"}
RAW_PRIMITIVE = re.compile(
    r"(?<![A-Za-z0-9_:.])(?:::)?"
    r"(WSASocketA|WSASocketW|WSASocket|WSAIoctl|WSAConnect|WSAAccept|WSASendTo|WSARecvFrom|"
    r"WSASendMsg|WSARecvMsg|WSASend|WSARecv|AcceptEx|ConnectEx|DisconnectEx|TransmitFile|"
    r"TransmitPackets|accept4|sendmmsg|recvmmsg|sendmsg|recvmsg|socket|bind|connect|listen|accept|"
    r"sendto|recvfrom|send|recv)\s*\("
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
    (re.compile(
        r"\bWSAID_(?:ACCEPTEX|CONNECTEX|DISCONNECTEX|TRANSMITFILE|TRANSMITPACKETS|WSARECVMSG|WSASENDMSG)\b"
    ), "unreviewed Winsock extension-function acquisition"),
)
WILDCARD_INET_ADDR = re.compile(r"\binet_addr\s*\(\s*([\"'])0\.0\.0\.0\1\s*\)")
HARDCODED_IPV4_CALLS = (
    ("inet_addr", re.compile(r"\binet_addr\s*\(\s*([\"'])(\d{1,3}(?:\.\d{1,3}){3})\1")),
    ("inet_pton", re.compile(
        r"\binet_pton\s*\(\s*AF_INET\s*,\s*([\"'])(\d{1,3}(?:\.\d{1,3}){3})\1"
    )),
)
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
    ordinal: int = 1
    family: str = "inet"
    address_rules: tuple["SocketAddressRule", ...] = ()


@dataclass(frozen=True)
class SocketAddressRule:
    """Tie one address-bearing primitive to its reviewed address expression."""

    operation: str
    occurrence: int
    argument: str
    source_variable: str = ""
    source_expression: str = ""
    provenance: str = ""
    guard_pattern: str = ""


@dataclass(frozen=True)
class ControlPathCheck:
    path: str
    function: str
    required_order: tuple[str, ...]
    message: str
    ordinal: int = 1
    exact_counts: tuple[tuple[str, int], ...] = ()


RAW_SOCKET_ALLOWLIST = (
    RawSocketAllowance(
        "SparkEditor/Source/Communication/LiveEditBridge.cpp",
        "SendAll",
        (("send", 1),),
        reviewed_exception="file-local TCP framing helper used only after LiveEditBridge::Connect admits the peer",
    ),
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
        ordinal=2,
        address_rules=(
            SocketAddressRule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&localAddress)", "localAddress",
                r"htonl\(endpointPolicy\.BindAddress\(\)\)",
            ),
            SocketAddressRule(
                "connect", 1, "reinterpret_cast<sockaddr*>(&addr)", "addr",
                provenance=r"inet_pton\s*\(\s*AF_INET\s*,\s*address\.c_str\s*\(\s*\)\s*,\s*&addr\.sin_addr\s*\)",
                guard_pattern=r"if\s*\(\s*!Spark::Net::ParseIPv4Address\s*\(\s*address\s*,\s*serverAddress\s*\)\s*\|\|\s*"
                              r"!endpointPolicy\.AllowsPeerAddress\s*\(\s*serverAddress\s*\)\s*\)\s*\{[^{}]*?"
                              r"return\s+false\s*;\s*\}",
            ),
            SocketAddressRule(
                "connect", 2, "reinterpret_cast<sockaddr*>(&addr)", "addr",
                provenance=r"inet_pton\s*\(\s*AF_INET\s*,\s*address\.c_str\s*\(\s*\)\s*,\s*&addr\.sin_addr\s*\)",
                guard_pattern=r"if\s*\(\s*!Spark::Net::ParseIPv4Address\s*\(\s*address\s*,\s*serverAddress\s*\)\s*\|\|\s*"
                              r"!endpointPolicy\.AllowsPeerAddress\s*\(\s*serverAddress\s*\)\s*\)\s*\{[^{}]*?"
                              r"return\s+false\s*;\s*\}",
            ),
        ),
    ),
    RawSocketAllowance(
        "SparkEditor/Source/Communication/CollaborativeEditSession.cpp",
        "SendFramed",
        (("send", 1),),
        reviewed_exception="file-local TCP framing helper used only on policy-admitted collaboration sockets",
    ),
    RawSocketAllowance(
        "SparkEditor/Source/Communication/CollaborativeEditSession.cpp",
        "RecvFramed",
        (("recv", 1),),
        reviewed_exception="file-local TCP framing helper used only on policy-admitted collaboration sockets",
    ),
    RawSocketAllowance(
        "SparkEditor/Source/Communication/CollaborativeEditSession.cpp",
        "ConnectWithTimeout",
        (("connect", 2),),
        (r"endpointPolicy\.IsValid\s*\(", r"endpointPolicy\.AllowsPeerAddress\s*\(", r"::connect\s*\("),
        address_rules=(
            SocketAddressRule(
                "connect", 1, "addr",
                provenance=r"addr\s*->\s*sa_family\s*!=\s*AF_INET[\s\S]*endpointPolicy\.AllowsPeerAddress",
                guard_pattern=r"if\s*\(\s*!endpointPolicy\.AllowsPeerAddress\s*\(\s*ntohl\s*\(\s*"
                              r"ipv4Address->sin_addr\.s_addr\s*\)\s*\)\s*\)\s*return\s+false\s*;",
            ),
            SocketAddressRule(
                "connect", 2, "addr",
                provenance=r"addr\s*->\s*sa_family\s*!=\s*AF_INET[\s\S]*endpointPolicy\.AllowsPeerAddress",
                guard_pattern=r"if\s*\(\s*!endpointPolicy\.AllowsPeerAddress\s*\(\s*ntohl\s*\(\s*"
                              r"ipv4Address->sin_addr\.s_addr\s*\)\s*\)\s*\)\s*return\s+false\s*;",
            ),
        ),
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
        ordinal=2,
        address_rules=(
            SocketAddressRule(
                "bind", 1, "reinterpret_cast<sockaddr*>(&addr)", "addr",
                r"htonl\(m_endpointPolicy\.BindAddress\(\)\)",
            ),
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
        ordinal=2,
        address_rules=(
            SocketAddressRule(
                "bind", 1, "reinterpret_cast<sockaddr*>(&localAddr)", "localAddr",
                r"htonl\(m_endpointPolicy\.BindAddress\(\)\)",
            ),
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
        family="unix",
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Utils/DaemonFraming.h",
        "SendAll",
        (("send", 1),),
        reviewed_exception="POSIX branch of the named-pipe/AF_UNIX daemon framing adapter",
        family="unix",
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Utils/DaemonFraming.h",
        "RecvAll",
        (("recv", 1),),
        reviewed_exception="POSIX branch of the named-pipe/AF_UNIX daemon framing adapter",
        family="unix",
    ),
    RawSocketAllowance(
        "SparkDaemon/src/DaemonServer.cpp",
        "EndpointIsActive",
        (("socket", 1), ("connect", 1)),
        (r"::socket\s*\(\s*AF_UNIX", r"address\.sun_family\s*=\s*AF_UNIX", r"::connect\s*\("),
        family="unix",
    ),
    RawSocketAllowance(
        "SparkDaemon/src/DaemonServer.cpp",
        "DaemonServer::Run",
        (("socket", 1), ("bind", 1), ("listen", 1), ("accept", 1)),
        (
            r"socketPath\.empty\s*\(",
            r"::socket\s*\(\s*AF_UNIX",
            r"addr\.sun_family\s*=\s*AF_UNIX",
            r"::bind\s*\(",
            r"::listen\s*\(",
            r"::accept\s*\(",
            r"IsSameUserPeer\s*\(",
        ),
        family="unix",
    ),
    RawSocketAllowance(
        "SparkGateway/src/GatewayAreaControl.cpp",
        "ReceiveExactUntil",
        (("recv", 1),),
        reviewed_exception="POSIX branch of the local named-pipe/AF_UNIX framing adapter",
        family="unix",
    ),
    RawSocketAllowance(
        "SparkGateway/src/GatewayAreaControl.cpp",
        "SendExactUntil",
        (("send", 1),),
        reviewed_exception="POSIX branch of the local named-pipe/AF_UNIX framing adapter",
        family="unix",
    ),
    RawSocketAllowance(
        "SparkGateway/src/GatewayAreaControl.cpp",
        "EndpointIsActive",
        (("socket", 1), ("connect", 1)),
        (r"::socket\s*\(\s*AF_UNIX", r"address\.sun_family\s*=\s*AF_UNIX", r"::connect\s*\("),
        family="unix",
    ),
    RawSocketAllowance(
        "SparkGateway/src/GatewayAreaControl.cpp",
        "CreateLocalListener",
        (("socket", 1), ("bind", 1), ("listen", 1)),
        (
            r"NormalizeLocalEndpoint\s*\(",
            r"::socket\s*\(\s*AF_UNIX",
            r"address\.sun_family\s*=\s*AF_UNIX",
            r"::bind\s*\(",
            r"::listen\s*\(",
        ),
        family="unix",
    ),
    RawSocketAllowance(
        "SparkGateway/src/GatewayAreaControl.cpp",
        "LocalAreaControlService::Run",
        (("accept", 1),),
        (r"CreateLocalListener\s*\(", r"::accept\s*\(", r"SameUserPeer\s*\("),
        family="unix",
    ),
    RawSocketAllowance(
        "SparkGateway/src/GatewayAreaControl.cpp",
        "LocalGatewayIngressService::Run",
        (("accept", 1),),
        (r"CreateLocalListener\s*\(", r"::accept\s*\(", r"SameUserPeer\s*\("),
        family="unix",
    ),
    RawSocketAllowance(
        "GameModules/SparkGameMMOFPS/Source/Game/TFLanDiscoveryScan.cpp",
        "TFLanDiscovery::StartScanning",
        (("socket", 1), ("bind", 1)),
        (r"m_endpointPolicy\.IsValid\s*\(", r"::socket\s*\(", r"m_endpointPolicy\.BindAddress\s*\(", r"bind\s*\("),
        address_rules=(
            SocketAddressRule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&bindAddr)", "bindAddr",
                r"htonl\(m_endpointPolicy\.BindAddress\(\)\)",
            ),
        ),
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
        address_rules=(
            SocketAddressRule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&localAddress)", "localAddress",
                r"htonl\(m_endpointPolicy\.BindAddress\(\)\)",
            ),
        ),
    ),
    RawSocketAllowance(
        "GameModules/SparkGameMMOFPS/Source/Game/TFLanDiscovery.cpp",
        "TFLanDiscovery::BroadcastBeacon",
        (("sendto", 1),),
        (r"m_allowAdvertisement", r"m_endpointPolicy\.IsValid\s*\(", r"m_bcastTargets", r"sendto\s*\("),
        address_rules=(
            SocketAddressRule(
                "sendto", 1, "reinterpret_cast<const sockaddr*>(&to)", "to", r"target",
                r"for\s*\(\s*const\s+uint32_t\s+target\s*:\s*m_bcastTargets\s*\)",
            ),
        ),
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/UDPTransport.h",
        "Initialize",
        (("socket", 1), ("bind", 1)),
        (r"m_endpointPolicy\.IsValid\s*\(", r"::socket\s*\(", r"m_endpointPolicy\.BindAddress\s*\(", r"::bind\s*\("),
        address_rules=(
            SocketAddressRule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&localAddr)", "localAddr",
                r"htonl\(m_endpointPolicy\.BindAddress\(\)\)",
            ),
        ),
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/UDPTransport.h",
        "Send",
        (("sendto", 1),),
        (r"m_endpointPolicy\.AllowsPeerAddress\s*\(", r"sendto\s*\("),
        address_rules=(
            SocketAddressRule(
                "sendto", 1, "reinterpret_cast<const sockaddr*>(&dest)", "dest",
                provenance=r"inet_pton\s*\(\s*AF_INET\s*,\s*address\.c_str\s*\(\s*\)\s*,\s*&dest\.sin_addr\s*\)",
                guard_pattern=r"if\s*\(\s*!m_endpointPolicy\.AllowsPeerAddress\s*\(\s*ntohl\s*\(\s*"
                              r"dest\.sin_addr\.s_addr\s*\)\s*\)\s*\)\s*return\s+false\s*;",
            ),
        ),
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
        address_rules=(
            SocketAddressRule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&localAddr)", "localAddr",
                r"htonl\(endpointPolicy\.BindAddress\(\)\)",
            ),
        ),
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/NetworkManager.cpp",
        "NetworkManager::SendRawTo",
        (("sendto", 1),),
        (r"IsEndpointAllowed\s*\(", r"sendto\s*\("),
        address_rules=(
            SocketAddressRule(
                "sendto", 1, "reinterpret_cast<const sockaddr*>(&addr)", "addr",
                provenance=r"IsEndpointAllowed\s*\(\s*addr\s*\)",
                guard_pattern=r"if\s*\(\s*!IsEndpointAllowed\s*\(\s*addr\s*\)\s*\)\s*\{[^{}]*?"
                              r"return\s+false\s*;\s*\}",
            ),
        ),
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
        (("socket", 1),),
        reviewed_exception="deferred factories; invocation is guarded in StartLanBroadcast",
        ordinal=2,
    ),
    RawSocketAllowance(
        "SparkEngine/Source/Engine/Networking/DedicatedServer.cpp",
        "DedicatedServer::DedicatedServer",
        (("socket", 1),),
        reviewed_exception="fallback deferred factory; invocation is guarded in StartLanBroadcast",
        ordinal=3,
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
        address_rules=(
            SocketAddressRule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&localAddress)", "localAddress",
                r"htonl\(m_config\.endpointPolicy\.BindAddress\(\)\)",
            ),
            SocketAddressRule(
                "sendto", 1, "reinterpret_cast<const sockaddr*>(&broadcastAddr)", "broadcastAddr",
                r"m_config\.endpointPolicy\.PeerScope\(\)==NetworkPeerScope::LoopbackOnly\?"
                r"htonl\(m_config\.endpointPolicy\.BindAddress\(\)\):"
                r"htonl\(m_config\.endpointPolicy\.BroadcastAddress\(\)\)",
            ),
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
        address_rules=(
            SocketAddressRule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&bindAddr)", "bindAddr",
                r"htonl\(endpointPolicy\.BindAddress\(\)\)",
            ),
        ),
    ),
)

CONTROL_PATH_CHECKS = (
    ControlPathCheck(
        "SparkEditor/Source/Panels/DedicatedServerPanel.cpp",
        "DedicatedServerPanel::LaunchPIEServer",
        (r"request\.bindAddress\s*=", r"request\.lanBroadcast\s*=\s*false", r"m_serverProcess->Launch\s*\("),
        "editor PIE must explicitly disable LAN broadcast before launching SparkServer",
        exact_counts=(
            (r"\brequest\.lanBroadcast\b", 1),
            (r"\brequest\.lanBroadcast\s*=\s*false\s*;", 1),
        ),
    ),
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


def _inside_executable_scope(text: str, offset: int) -> bool:
    """Return whether offset is nested in a function/control/lambda body.

    C++ does not permit a function definition inside an executable body.  This
    keeps a call followed by its caller's block from being mistaken for a
    definition while still allowing namespace- and class-scoped definitions.
    Comments and strings must already be masked by the caller.
    """

    scopes: list[bool] = []
    for index, character in enumerate(text[:offset]):
        if character == "{":
            boundary = max(text.rfind(";", 0, index), text.rfind("{", 0, index), text.rfind("}", 0, index))
            prefix = text[boundary + 1:index].strip()
            container = bool(
                re.search(r"\bnamespace(?:\s+[A-Za-z_]\w*(?:::\w+)*)?\s*$", prefix)
                or re.search(r"\b(?:class|struct|union|enum)(?:\s+class)?\b[^;{}()]*$", prefix)
                or re.search(r"\bextern\s*$", prefix)
            )
            scopes.append(not container)
        elif character == "}" and scopes:
            scopes.pop()
    return any(scopes)


def _delimiter_depth_at(text: str, offset: int, left: str, right: str) -> int:
    depth = 0
    for character in text[:offset]:
        if character == left:
            depth += 1
        elif character == right and depth:
            depth -= 1
    return depth


def _brace_scope_path(text: str, offset: int) -> tuple[int, ...]:
    stack: list[int] = []
    for index, character in enumerate(text[:offset]):
        if character == "{":
            stack.append(index)
        elif character == "}" and stack:
            stack.pop()
    return tuple(stack)


def _preprocessor_branch_path(text: str, offset: int) -> tuple[tuple[int, int], ...]:
    """Identify the active textual #if/#elif/#else branch at an offset."""

    stack: list[tuple[int, int]] = []
    directive = re.compile(r"(?m)^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b")
    for match in directive.finditer(text, 0, offset):
        kind = match.group(1)
        if kind in {"if", "ifdef", "ifndef"}:
            stack.append((match.start(), 0))
        elif kind in {"elif", "else"} and stack:
            opening, branch = stack[-1]
            stack[-1] = (opening, branch + 1)
        elif kind == "endif" and stack:
            stack.pop()
    return tuple(stack)


def _evidence_dominates_call(text: str, evidence_offset: int, call_offset: int) -> bool:
    evidence_path = _preprocessor_branch_path(text, evidence_offset)
    call_path = _preprocessor_branch_path(text, call_offset)
    return call_path[:len(evidence_path)] == evidence_path


def _function_spans(code: str, function: str) -> list[tuple[int, int]]:
    spans: list[tuple[int, int]] = []
    pattern = re.compile(rf"(?<![A-Za-z0-9_:]){re.escape(function)}\s*\(")
    for match in pattern.finditer(code):
        if (_inside_executable_scope(code, match.start())
                or _delimiter_depth_at(code, match.start(), "(", ")")
                or _delimiter_depth_at(code, match.start(), "[", "]")):
            continue
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


def _selected_function_span(code: str, allowance: RawSocketAllowance) -> Optional[tuple[int, int]]:
    spans = _function_spans(code, allowance.function)
    if allowance.ordinal < 1 or allowance.ordinal > len(spans):
        return None
    return spans[allowance.ordinal - 1]


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
            if (path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES and
                    not lowered_parts.intersection({"thirdparty", "third_party", "build", "generated", "tests"})):
                yield path


NON_SHIPPED_TOP_LEVEL = {
    ".claude", ".codex", ".git", ".github", "assets", "build", "docs", "resources", "scripts",
    "shaders", "tests", "thirdparty", "wiki",
}

# Exact, reviewed first-party implementation files that support developer or CI
# tooling but are not part of any shipped source payload.  Keep this list at
# file granularity so future implementation files remain fail-closed.
NON_SHIPPED_FIRST_PARTY_SOURCES = frozenset({"tools/gvisor-wine-shim.c"})


def _is_inventory_candidate(root: Path, path: Path) -> bool:
    """True when *path* is shipped first-party source the inventory must account for."""

    try:
        relative = path.relative_to(root)
    except ValueError:
        return False
    parts = relative.parts
    if len(parts) < 2:
        # Only files inside a top-level directory can belong to a shipped tree.
        return False
    if parts[0].lower() in NON_SHIPPED_TOP_LEVEL:
        return False
    if path.suffix.lower() not in SOURCE_SUFFIXES:
        return False
    lowered = {part.lower() for part in parts}
    return not lowered.intersection({"thirdparty", "third_party", "build", "generated", "tests"})


def _tracked_inventory_candidates(root: Path) -> Optional[list[Path]]:
    """Shipped first-party sources known to git, or None when the tree is not a checkout.

    The inventory answers "does the repository ship source the guard has not
    reviewed?", so it must judge the committed tree.  Walking the working
    directory instead made every stray local build or playtest directory a
    finding, which is why the guard was red on an otherwise clean baseline.
    """

    if not (root / ".git").exists():
        return None
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--cached"],
            capture_output=True,
            check=True,
            timeout=120,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    entries = completed.stdout.decode("utf-8", errors="replace").split("\0")
    # An empty list is returned as an empty list, never as None: "git listed
    # nothing" and "this is not a checkout" are different facts and the caller
    # has to be able to tell them apart.
    return [root / entry for entry in entries if entry]


def _inventory_coverage_findings(root: Path, roots: Sequence[Path]) -> list[Finding]:
    """Reject shipped first-party source that falls outside the explicit roots."""

    findings: list[Finding] = []
    resolved_roots = tuple(path.resolve() for path in roots if path.is_dir())
    candidates = _tracked_inventory_candidates(root)
    if candidates is None:
        # Not a git checkout (exported tarball, vendored copy): fall back to the
        # working-directory walk, which is the stricter of the two.
        candidates = [path for path in root.rglob("*") if path.is_file()]
    elif not candidates:
        # git succeeded and named no tracked file. There is then no population
        # to judge, and "no findings" would be manufactured rather than
        # measured - exactly the shape a sparse checkout, a wrong --work-tree,
        # or a git that stops enumerating would take. Fail loudly instead.
        return [
            Finding(
                root,
                1,
                "git listed no tracked files: the network inventory has no population to judge",
            )
        ]
    for path in sorted(candidates):
        if not _is_inventory_candidate(root, path):
            continue
        relative = path.relative_to(root).as_posix()
        if relative in NON_SHIPPED_FIRST_PARTY_SOURCES:
            continue
        resolved = path.resolve()
        if not any(resolved.is_relative_to(source_root) for source_root in resolved_roots):
            findings.append(Finding(path, 1, "first-party source is outside the explicit network inventory"))
    return findings


def _line_at(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _call_arguments(code: str, offset: int) -> Optional[list[str]]:
    opening = code.find("(", offset)
    closing = _matching_delimiter(code, opening, "(", ")")
    if opening < 0 or closing is None:
        return None
    arguments: list[str] = []
    start = opening + 1
    paren_depth = 0
    bracket_depth = 0
    brace_depth = 0
    angle_depth = 0
    cursor = start
    while cursor < closing:
        character = code[cursor]
        if character == "(":
            paren_depth += 1
        elif character == ")" and paren_depth:
            paren_depth -= 1
        elif character == "[":
            bracket_depth += 1
        elif character == "]" and bracket_depth:
            bracket_depth -= 1
        elif character == "{":
            brace_depth += 1
        elif character == "}" and brace_depth:
            brace_depth -= 1
        elif character == "<":
            angle_depth += 1
        elif character == ">" and angle_depth:
            angle_depth -= 1
        elif character == "," and not (paren_depth or bracket_depth or brace_depth or angle_depth):
            arguments.append(code[start:cursor].strip())
            start = cursor + 1
        cursor += 1
    arguments.append(code[start:closing].strip())
    return arguments


def _compact_cpp(text: str) -> str:
    return re.sub(r"\s+", "", text)


def _is_direct_statement(text: str, offset: int) -> bool:
    """Reject conditionally-prefixed single-line assignments.

    A reviewed address assignment must begin a statement in the same lexical
    block as the primitive that consumes it.  Braced conditionals are caught
    by the block-depth check; this catches `if (condition) address = ...;`.
    """

    boundary = max(text.rfind(";", 0, offset), text.rfind("{", 0, offset), text.rfind("}", 0, offset))
    return not text[boundary + 1:offset].strip()


ADDRESS_ARGUMENT_INDEX = {"bind": 1, "connect": 1, "sendto": 4}


def _address_rule_findings(path: Path, code: str, allowance: RawSocketAllowance, span: tuple[int, int],
                           occurrences: Sequence[tuple[str, int, int]]) -> list[Finding]:
    findings: list[Finding] = []
    address_occurrences = [item for item in occurrences if item[0] in ADDRESS_ARGUMENT_INDEX]
    rules_by_site = {(rule.operation, rule.occurrence): rule for rule in allowance.address_rules}
    counters: Counter[str] = Counter()
    for operation, offset, line in address_occurrences:
        counters[operation] += 1
        site = (operation, counters[operation])
        rule = rules_by_site.get(site)
        if allowance.family == "inet" and rule is None:
            findings.append(Finding(path, line, f"reviewed raw {operation} site lacks an exact address rule"))
            continue
        if rule is None:
            continue
        arguments = _call_arguments(code, offset)
        argument_index = ADDRESS_ARGUMENT_INDEX[operation]
        if arguments is None or len(arguments) <= argument_index:
            findings.append(Finding(path, line, f"cannot parse reviewed {operation} call"))
            continue
        observed_argument = _compact_cpp(arguments[argument_index])
        if observed_argument != _compact_cpp(rule.argument):
            findings.append(Finding(
                path,
                line,
                f"reviewed {operation} address changed: expected {rule.argument!r}, observed {arguments[argument_index]!r}",
            ))
            continue
        prefix = code[span[0]:offset]
        evidence_end: Optional[int] = None
        if rule.source_variable:
            if rule.source_expression:
                assignments = list(re.finditer(
                    rf"\b{re.escape(rule.source_variable)}\s*\.\s*sin_addr\s*\.\s*s_addr\s*=\s*([^;]+);",
                    prefix,
                ))
                if not assignments:
                    findings.append(Finding(
                        path, line, f"reviewed {operation} address {rule.source_variable} has no explicit IPv4 source",
                    ))
                    continue
                if len(assignments) != 1:
                    findings.append(Finding(
                        path, line, f"reviewed {operation} address {rule.source_variable} has multiple explicit IPv4 sources",
                    ))
                    continue
                assignment = assignments[0]
                expression = _compact_cpp(assignment.group(1))
                if not re.fullmatch(rule.source_expression, expression):
                    findings.append(Finding(
                        path,
                        line,
                        f"reviewed {operation} address {rule.source_variable} is not policy-derived",
                    ))
                else:
                    evidence_end = assignment.end()
                    assignment_scope = _brace_scope_path(prefix, assignment.start())
                    call_scope = _brace_scope_path(prefix, len(prefix))
                    if (not _is_direct_statement(prefix, assignment.start())
                            or call_scope[:len(assignment_scope)] != assignment_scope
                            or not _evidence_dominates_call(prefix, assignment.start(), len(prefix))):
                        findings.append(Finding(
                            path, line,
                            f"reviewed {operation} address {rule.source_variable} source does not dominate the raw call",
                        ))
            elif not rule.provenance:
                findings.append(Finding(
                    path, line, f"reviewed {operation} address rule has no source evidence",
                ))
        if rule.provenance:
            provenance_matches = list(re.finditer(rule.provenance, prefix, re.MULTILINE))
            if not provenance_matches:
                findings.append(Finding(path, line, f"reviewed {operation} address provenance is incomplete"))
            elif len(provenance_matches) != 1:
                findings.append(Finding(path, line, f"reviewed {operation} address provenance is ambiguous"))
            else:
                provenance_match = provenance_matches[0]
                if not _evidence_dominates_call(prefix, provenance_match.start(), len(prefix)):
                    findings.append(Finding(
                        path, line, f"reviewed {operation} address provenance is preprocessor-conditional",
                    ))
                if evidence_end is None:
                    evidence_end = provenance_match.end()
        if rule.guard_pattern:
            guard_matches = list(re.finditer(rule.guard_pattern, prefix, re.MULTILINE))
            if len(guard_matches) != 1:
                findings.append(Finding(path, line, f"reviewed {operation} policy result is not fail-closed"))
            elif not _evidence_dominates_call(prefix, guard_matches[0].start(), len(prefix)):
                findings.append(Finding(path, line, f"reviewed {operation} policy guard is preprocessor-conditional"))
        if rule.source_variable and evidence_end is not None:
            tail = prefix[evidence_end:]
            variable = re.escape(rule.source_variable)
            target = (rf"\b{variable}(?:\s*\.\s*sin_addr(?:\s*\.\s*(?:s_addr|"
                      rf"S_un\s*\.\s*S_addr))?)?")
            assignment_operator = r"(?:<<|>>|[+\-*/%&|^])?=(?!=)"
            later_write = re.search(
                rf"(?:{target}\s*{assignment_operator}|(?:\+\+|--)\s*{target}|{target}\s*(?:\+\+|--)|"
                rf"\b(?:memcpy|memmove|memset)\s*\(\s*&\s*{variable}\b|"
                rf"\binet_pton\s*\([^;]*&\s*{variable}\s*\.\s*sin_addr|"
                rf"\b(?:auto|[A-Za-z_]\w*(?:::\w+)*)\s*(?:\*|&)\s*\w+\s*=\s*(?:&\s*)?{target}|"
                rf"\b(?:auto|[A-Za-z_]\w*(?:::\w+)*)\s*(?:\*|&)\s*\w+\s*=\s*"
                rf"(?:reinterpret_cast|static_cast|const_cast)\s*<[^;>]+>\s*\(\s*&?\s*{target}\s*\)|"
                rf"\b(?:auto|[A-Za-z_]\w*(?:::\w+)*)\s*(?:\*|&)\s*\w+\s*=\s*"
                rf"\([^;()]*[&*][^;()]*\)\s*&?\s*{target})",
                tail,
            )
            if later_write is not None:
                findings.append(Finding(
                    path, line, f"reviewed {operation} address changed after policy derivation",
                ))
            safe_read_helpers = {
                "if", "while", "for", "switch", "ntohl", "IsEndpointAllowed", "IsIPv4LoopbackAddress",
                "AllowsPeerAddress", "bind", "connect", "sendto", "recvfrom", "send", "recv",
            }
            for helper in re.finditer(
                rf"\b(?P<name>(?:[A-Za-z_]\w*::)*[A-Za-z_]\w*)\s*\([^;]*\b{variable}\b[^;]*\)", tail
            ):
                if helper.group("name").split("::")[-1] not in safe_read_helpers:
                    findings.append(Finding(
                        path, line,
                        f"reviewed {operation} address passed to an unreviewed helper after derivation: "
                        f"{helper.group('name')}",
                    ))
                    break

    for site in rules_by_site:
        if counters[site[0]] < site[1]:
            findings.append(Finding(
                path,
                _line_at(code, span[0]),
                f"reviewed address site missing: {site[0]} occurrence {site[1]} in {allowance.function}",
            ))
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
        if re.match(r"\binet_addr\b", code[match.start():]) is None:
            continue
        findings.append(Finding(path, _line_at(source, match.start()), "forbidden inet_addr IPv4 wildcard"))
    for function_name, pattern in HARDCODED_IPV4_CALLS:
        for match in pattern.finditer(comments_removed):
            # The comments-only mask retains literals so their value can be
            # validated.  Cross-check the strings mask at the same offset so a
            # sentence containing `inet_addr("8.8.8.8")` is not code evidence.
            if re.match(rf"\b{function_name}\b", code[match.start():]) is None:
                continue
            try:
                address = ipaddress.IPv4Address(match.group(2))
            except ipaddress.AddressValueError:
                continue
            if not (address.is_loopback or any(address in network for network in RFC1918)):
                findings.append(Finding(
                    path, _line_at(source, match.start()), "forbidden hard-coded non-private IPv4 endpoint literal",
                ))

    spans_by_allowance: dict[RawSocketAllowance, Optional[tuple[int, int]]] = {}
    selectors: Counter[tuple[str, int]] = Counter()
    for allowance in path_allowances:
        selectors[(allowance.function, allowance.ordinal)] += 1
        span = _selected_function_span(code, allowance)
        spans_by_allowance[allowance] = span
        if not allowance.required_order and not allowance.reviewed_exception:
            findings.append(Finding(path, 1, f"raw-socket allowance lacks policy evidence: {allowance.function}"))
        if span is None:
            findings.append(Finding(
                path, 1, f"reviewed raw-socket function missing: {allowance.function} ordinal {allowance.ordinal}",
            ))
    for (function, ordinal), count in selectors.items():
        if count != 1:
            findings.append(Finding(path, 1, f"duplicate raw-socket selector: {function} ordinal {ordinal}"))

    occurrences: list[tuple[str, int, int]] = []
    observed: dict[RawSocketAllowance, Counter[str]] = {allowance: Counter() for allowance in path_allowances}
    owned_occurrences: dict[RawSocketAllowance, list[tuple[str, int, int]]] = {
        allowance: [] for allowance in path_allowances
    }
    for match in RAW_PRIMITIVE.finditer(code):
        operation = match.group(1)
        occurrence = (operation, match.start(), _line_at(source, match.start()))
        occurrences.append(occurrence)
        owners = [
            (allowance, span)
            for allowance, span in spans_by_allowance.items()
            if span is not None and span[0] <= match.start() < span[1]
        ]
        if len(owners) != 1:
            findings.append(Finding(path, occurrence[2], f"unreviewed raw {operation} site"))
            continue
        allowance, span = owners[0]
        observed[allowance][operation] += 1
        owned_occurrences[allowance].append(occurrence)

    for allowance in path_allowances:
        span = spans_by_allowance[allowance]
        if span is None:
            continue
        expected = Counter(dict(allowance.operations))
        if observed[allowance] != expected:
            findings.append(Finding(
                path,
                _line_at(source, span[0]),
                f"raw-socket inventory changed in {allowance.function} ordinal {allowance.ordinal}: "
                f"expected {dict(expected)}, "
                f"observed {dict(observed[allowance])}",
            ))
        body = code[span[0]:span[1]]
        if re.search(r"\bgoto\b", body):
            findings.append(Finding(
                path, _line_at(source, span[0]), f"reviewed raw-socket function contains unsupported goto: {allowance.function}",
            ))
        if allowance.family == "unix":
            if re.search(r"\b(?:AF_INET|AF_INET6|AF_UNSPEC|PF_INET|PF_INET6|PF_UNSPEC)\b", body):
                findings.append(Finding(
                    path, _line_at(source, span[0]), f"local-IPC allowance widened beyond AF_UNIX: {allowance.function}",
                ))
            if any(operation in {"socket", "WSASocket", "WSASocketA", "WSASocketW"}
                   for operation in observed[allowance]) and not re.search(r"\bAF_UNIX\b", body):
                findings.append(Finding(
                    path, _line_at(source, span[0]), f"local-IPC socket lacks AF_UNIX evidence: {allowance.function}",
                ))
        elif re.search(r"\bAF_UNIX\b", body):
            findings.append(Finding(
                path, _line_at(source, span[0]), f"INET allowance unexpectedly contains AF_UNIX: {allowance.function}",
            ))
        findings.extend(_address_rule_findings(path, code, allowance, span, owned_occurrences[allowance]))
        if allowance.required_order and not _ordered_markers_present(body, allowance.required_order):
            findings.append(Finding(
                path,
                _line_at(source, span[0]),
                f"policy/control-path markers incomplete in {allowance.function} ordinal {allowance.ordinal}",
            ))
    return findings


def scan_raw_socket_inventory(
    root: Path = ROOT,
    source_roots: Optional[Iterable[Path]] = None,
    allowances: Sequence[RawSocketAllowance] = RAW_SOCKET_ALLOWLIST,
) -> list[Finding]:
    """Return all unreviewed or policy-incomplete raw socket sites under explicit roots."""

    explicit_roots = source_roots is None
    roots = tuple(source_roots) if source_roots is not None else SOURCE_ROOTS
    findings: list[Finding] = []
    for source_root in roots:
        if not source_root.is_dir():
            findings.append(Finding(source_root, 1, "explicit first-party source root is missing"))
    for path in _source_files(roots):
        relative = path.relative_to(root).as_posix()
        findings.extend(scan_source_text(
            path.read_text(encoding="utf-8", errors="replace"),
            relative,
            allowances,
            path,
        ))
    if explicit_roots:
        findings.extend(_inventory_coverage_findings(root, roots))
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
        span = spans[check.ordinal - 1] if 1 <= check.ordinal <= len(spans) else None
        body = code[span[0]:span[1]] if span is not None else ""
        exact_counts_match = all(
            len(re.findall(pattern, body, re.MULTILINE)) == expected
            for pattern, expected in check.exact_counts
        )
        if span is None or not _ordered_markers_present(body, check.required_order) or not exact_counts_match:
            findings.append(Finding(path, _line_at(original, span[0]) if span is not None else 1, check.message))
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
        line = raw_line
        if line_number == 1 and line.startswith("\ufeff"):
            line = line[1:]
        line = line.strip()
        if not line:
            continue
        if line[0] in "#;":
            continue
        if line.startswith("["):
            if not line.endswith("]") or not line[1:-1].strip():
                report(path, line_number, "malformed server configuration section")
                failures += 1
                section = ""
                continue
            section = line[1:-1].strip()
            continue
        if "=" not in line:
            report(path, line_number, "malformed server configuration entry")
            failures += 1
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        if not key:
            report(path, line_number, "empty server configuration key")
            failures += 1
            continue
        if section.lower() != "network":
            continue
        lowered_key = key.lower()
        if lowered_key in {"lan_only", "bind_address", "lan_broadcast"}:
            if section != "Network" or key != lowered_key:
                report(path, line_number, f"security-sensitive key must use canonical spelling Network.{lowered_key}")
                failures += 1
            if lowered_key in settings:
                report(path, line_number, f"duplicate security-sensitive Network.{lowered_key}")
                failures += 1
        settings[lowered_key] = (value, line_number)

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
