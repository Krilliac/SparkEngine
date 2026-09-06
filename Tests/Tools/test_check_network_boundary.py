#!/usr/bin/env python3
"""Mutation tests for the NET-100 first-party raw-socket inventory."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPOSITORY_ROOT / "Tools" / "check_network_boundary.py"
SPEC = importlib.util.spec_from_file_location("check_network_boundary", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
boundary = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = boundary
SPEC.loader.exec_module(boundary)


class NetworkBoundaryMutationTests(unittest.TestCase):
    def allowance(self, operations: tuple[tuple[str, int], ...],
                  required_order: tuple[str, ...] = (), *, ordinal: int = 1, family: str = "inet",
                  address_rules: tuple[object, ...] = ()) -> object:
        return boundary.RawSocketAllowance(
            "Src/Network.cpp", "Added", operations, required_order, ordinal=ordinal, family=family,
            address_rules=address_rules,
        )

    def address_rule(self, operation: str, occurrence: int, argument: str, source_variable: str = "",
                     source_expression: str = "", provenance: str = "", guard_pattern: str = "") -> object:
        return boundary.SocketAddressRule(
            operation, occurrence, argument, source_variable, source_expression, provenance, guard_pattern,
        )

    def scan(self, source: str, allowances: tuple[object, ...] = ()) -> list[object]:
        return boundary.scan_source_text(source, "Src/Network.cpp", allowances)

    def assert_finding(self, findings: list[object], fragment: str) -> None:
        self.assertTrue(
            any(fragment in finding.message for finding in findings),
            f"expected {fragment!r} in {[finding.message for finding in findings]!r}",
        )

    def test_new_file_raw_socket_site_is_rejected(self) -> None:
        source = "void Added() { auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); }\n"
        self.assert_finding(self.scan(source), "unreviewed raw socket site")

    def test_wsasocketw_constructor_is_rejected(self) -> None:
        source = "void Added() { auto handle = ::WSASocketW(AF_INET, SOCK_DGRAM, 0, nullptr, 0, 0); }\n"
        self.assert_finding(self.scan(source), "unreviewed raw WSASocketW site")

    def test_winsock_peer_and_extension_primitives_are_rejected(self) -> None:
        operations = (
            "WSAIoctl", "WSAConnect", "WSAAccept", "WSASend", "WSARecv", "WSASendTo", "WSARecvFrom",
            "WSASendMsg", "WSARecvMsg", "AcceptEx", "ConnectEx", "DisconnectEx", "TransmitFile",
            "TransmitPackets", "accept4", "sendmsg", "recvmsg", "sendmmsg", "recvmmsg",
        )
        for operation in operations:
            with self.subTest(operation=operation):
                source = f"void Added() {{ (void)::{operation}(); }}\n"
                self.assert_finding(self.scan(source), f"unreviewed raw {operation} site")

    def test_winsock_extension_pointer_acquisition_is_rejected(self) -> None:
        source = """
void Added(SOCKET handle) {
    GUID id = WSAID_CONNECTEX;
    LPFN_CONNECTEX extension = nullptr;
    DWORD bytes = 0;
    ::WSAIoctl(handle, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &extension,
               sizeof(extension), &bytes, nullptr, nullptr);
    extension(handle, nullptr, 0, nullptr, 0, nullptr, nullptr);
}
"""
        findings = self.scan(source)
        self.assert_finding(findings, "unreviewed raw WSAIoctl site")
        self.assert_finding(findings, "Winsock extension-function acquisition")

    def test_new_site_inside_reviewed_function_changes_exact_inventory(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy& endpointPolicy) {
    if (!endpointPolicy.IsValid()) return;
    auto first = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    auto second = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}
"""
        allowance = self.allowance(
            (("socket", 1),),
            (r"endpointPolicy\.IsValid\s*\(", r"::socket\s*\("),
        )
        self.assert_finding(self.scan(source, (allowance,)), "raw-socket inventory changed")

    def test_policy_marker_in_another_overload_cannot_authorize_site(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy& endpointPolicy) {
    if (!endpointPolicy.IsValid()) return;
}
void Added(int) {
    auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}
"""
        allowance = self.allowance(
            (("socket", 1),),
            (r"endpointPolicy\.IsValid\s*\(", r"::socket\s*\("),
            ordinal=1,
        )
        self.assert_finding(self.scan(source, (allowance,)), "unreviewed raw socket site")

    def test_call_before_definition_cannot_be_selected_as_function_body(self) -> None:
        source = """
bool Added();
void Caller(const NetworkEndpointPolicy& endpointPolicy) {
    if (Added()) {
        if (!endpointPolicy.IsValid()) return;
        auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }
}
bool Added() { return true; }
"""
        allowance = self.allowance(
            (("socket", 1),),
            (r"endpointPolicy\.IsValid\s*\(", r"::socket\s*\("),
        )
        self.assert_finding(self.scan(source, (allowance,)), "unreviewed raw socket site")

    def test_allowance_without_policy_evidence_is_rejected(self) -> None:
        source = "void Added() { auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); }\n"
        allowance = self.allowance((("socket", 1),))
        self.assert_finding(self.scan(source, (allowance,)), "allowance lacks policy evidence")

    def test_inet_addr_wildcard_is_rejected_even_in_reviewed_function(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy& endpointPolicy) {
    if (!endpointPolicy.IsValid()) return;
    auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("0.0.0.0");
    ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}
"""
        allowance = self.allowance(
            (("socket", 1), ("bind", 1)),
            (r"endpointPolicy\.IsValid\s*\(", r"::socket\s*\(", r"::bind\s*\("),
            address_rules=(self.address_rule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&address)", "address",
                r"htonl\(endpointPolicy\.BindAddress\(\)\)",
            ),),
        )
        self.assert_finding(self.scan(source, (allowance,)), "forbidden inet_addr IPv4 wildcard")

    def test_zero_initialized_sockaddr_bind_is_rejected(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy& endpointPolicy) {
    if (!endpointPolicy.IsValid()) return;
    auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}
"""
        allowance = self.allowance(
            (("socket", 1), ("bind", 1)),
            (r"endpointPolicy\.IsValid\s*\(", r"::socket\s*\(", r"::bind\s*\("),
            address_rules=(self.address_rule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&address)", "address",
                r"htonl\(endpointPolicy\.BindAddress\(\)\)",
            ),),
        )
        self.assert_finding(self.scan(source, (allowance,)), "has no explicit IPv4 source")

    def test_hard_coded_public_bind_cannot_hide_behind_unused_policy_marker(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy& endpointPolicy) {
    if (!endpointPolicy.IsValid()) return;
    auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    const auto unused = htonl(endpointPolicy.BindAddress());
    address.sin_addr.s_addr = inet_addr("8.8.8.8");
    ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}
"""
        allowance = self.allowance(
            (("socket", 1), ("bind", 1)),
            (r"endpointPolicy\.IsValid\s*\(", r"endpointPolicy\.BindAddress\s*\(", r"::bind\s*\("),
            address_rules=(self.address_rule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&address)", "address",
                r"htonl\(endpointPolicy\.BindAddress\(\)\)",
            ),),
        )
        self.assert_finding(self.scan(source, (allowance,)), "is not policy-derived")

    def test_same_count_address_replacement_is_rejected(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy& endpointPolicy) {
    if (!endpointPolicy.IsValid()) return;
    auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in approved{};
    approved.sin_addr.s_addr = htonl(endpointPolicy.BindAddress());
    sockaddr_in replacement{};
    replacement.sin_addr.s_addr = htonl(endpointPolicy.BindAddress());
    ::bind(handle, reinterpret_cast<const sockaddr*>(&replacement), sizeof(replacement));
}
"""
        allowance = self.allowance(
            (("socket", 1), ("bind", 1)),
            (r"endpointPolicy\.IsValid\s*\(", r"endpointPolicy\.BindAddress\s*\(", r"::bind\s*\("),
            address_rules=(self.address_rule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&approved)", "approved",
                r"htonl\(endpointPolicy\.BindAddress\(\)\)",
            ),),
        )
        self.assert_finding(self.scan(source, (allowance,)), "reviewed bind address changed")

    def test_ignored_policy_boolean_cannot_authorize_send(self) -> None:
        source = """
void Added(const sockaddr_in& address) {
    (void)IsEndpointAllowed(address);
    ::sendto(handle, payload, size, 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}
"""
        allowance = self.allowance(
            (("sendto", 1),),
            (r"IsEndpointAllowed\s*\(", r"::sendto\s*\("),
            address_rules=(self.address_rule(
                "sendto", 1, "reinterpret_cast<const sockaddr*>(&address)", "address",
                provenance=r"IsEndpointAllowed\s*\(\s*address\s*\)",
                guard_pattern=r"if\s*\(\s*!IsEndpointAllowed\s*\(\s*address\s*\)\s*\)\s*return\s*;",
            ),),
        )
        self.assert_finding(self.scan(source, (allowance,)), "policy result is not fail-closed")

    def test_policy_guard_cannot_borrow_return_from_later_block(self) -> None:
        source = """
void Added(const sockaddr_in& address, bool localOnly) {
    if (!IsEndpointAllowed(address)) {
        RecordPolicyFailure();
    }
    if (localOnly) {
        return;
    }
    ::sendto(handle, payload, size, 0, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}
"""
        allowance = self.allowance(
            (("sendto", 1),),
            (r"IsEndpointAllowed\s*\(", r"::sendto\s*\("),
            address_rules=(self.address_rule(
                "sendto", 1, "reinterpret_cast<const sockaddr*>(&address)", "address",
                provenance=r"IsEndpointAllowed\s*\(\s*address\s*\)",
                guard_pattern=r"if\s*\(\s*!IsEndpointAllowed\s*\(\s*address\s*\)\s*\)\s*\{[^{}]*"
                              r"return\s*;\s*\}",
            ),),
        )
        self.assert_finding(self.scan(source, (allowance,)), "policy result is not fail-closed")

    def test_address_overwrite_after_policy_derivation_is_rejected(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy& endpointPolicy, const std::string& text) {
    if (!endpointPolicy.IsValid()) return;
    sockaddr_in address{};
    inet_pton(AF_INET, text.c_str(), &address.sin_addr);
    if (!endpointPolicy.AllowsPeerAddress(ntohl(address.sin_addr.s_addr))) return;
    address.sin_addr.s_addr = inet_addr("8.8.8.8");
    auto handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address));
}
"""
        allowance = self.allowance(
            (("socket", 1), ("connect", 1)),
            (r"endpointPolicy\.IsValid\s*\(", r"endpointPolicy\.AllowsPeerAddress\s*\(", r"::socket\s*\(",
             r"::connect\s*\("),
            address_rules=(self.address_rule(
                "connect", 1, "reinterpret_cast<sockaddr*>(&address)", "address",
                provenance=r"inet_pton\s*\(\s*AF_INET\s*,\s*text\.c_str\s*\(\s*\)\s*,\s*&address\.sin_addr\s*\)",
            ),),
        )
        self.assert_finding(self.scan(source, (allowance,)), "changed after policy derivation")

    def test_compound_increment_and_alias_address_writes_are_rejected(self) -> None:
        mutations = (
            "address.sin_addr.s_addr &= 0u;",
            "++address.sin_addr.s_addr;",
            "address.sin_addr.s_addr--;",
            "address.sin_addr.S_un.S_addr = 0u;",
            "auto& alias = address.sin_addr.s_addr; alias = 0u;",
            "sockaddr_in* alias = reinterpret_cast<sockaddr_in*>(&address); alias->sin_addr.s_addr = 0u;",
            "MutateAddress(address);",
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                source = f"""
void Added(const NetworkEndpointPolicy& endpointPolicy) {{
    if (!endpointPolicy.IsValid()) return;
    auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{{}};
    address.sin_addr.s_addr = htonl(endpointPolicy.BindAddress());
    {mutation}
    ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}}
"""
                allowance = self.allowance(
                    (("socket", 1), ("bind", 1)),
                    (r"endpointPolicy\.IsValid\s*\(", r"endpointPolicy\.BindAddress\s*\(", r"::bind\s*\("),
                    address_rules=(self.address_rule(
                        "bind", 1, "reinterpret_cast<const sockaddr*>(&address)", "address",
                        r"htonl\(endpointPolicy\.BindAddress\(\)\)",
                    ),),
                )
                findings = self.scan(source, (allowance,))
                self.assertTrue(
                    any("changed after policy derivation" in item.message
                        or "unreviewed helper after derivation" in item.message for item in findings),
                    [item.message for item in findings],
                )

    def test_conditional_policy_assignment_cannot_hide_wildcard_path(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy& endpointPolicy, bool condition) {
    if (!endpointPolicy.IsValid()) return;
    auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
    address.sin_addr.s_addr = 0u;
    if (condition)
        address.sin_addr.s_addr = htonl(endpointPolicy.BindAddress());
    ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}
"""
        allowance = self.allowance(
            (("socket", 1), ("bind", 1)),
            (r"endpointPolicy\.IsValid\s*\(", r"endpointPolicy\.BindAddress\s*\(", r"::bind\s*\("),
            address_rules=(self.address_rule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&address)", "address",
                r"htonl\(endpointPolicy\.BindAddress\(\)\)",
            ),),
        )
        self.assert_finding(self.scan(source, (allowance,)), "multiple explicit IPv4 sources")

    def test_braced_conditional_policy_assignment_must_dominate_call(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy& endpointPolicy, bool condition) {
    if (!endpointPolicy.IsValid()) return;
    auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
    if (condition) {
        address.sin_addr.s_addr = htonl(endpointPolicy.BindAddress());
    }
    ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}
"""
        allowance = self.allowance(
            (("socket", 1), ("bind", 1)),
            (r"endpointPolicy\.IsValid\s*\(", r"endpointPolicy\.BindAddress\s*\(", r"::bind\s*\("),
            address_rules=(self.address_rule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&address)", "address",
                r"htonl\(endpointPolicy\.BindAddress\(\)\)",
            ),),
        )
        self.assert_finding(self.scan(source, (allowance,)), "source does not dominate")

    def test_preprocessor_conditional_policy_assignment_is_rejected(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy& endpointPolicy) {
    if (!endpointPolicy.IsValid()) return;
    auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
#ifdef _WIN32
    PrepareAddress();
    address.sin_addr.s_addr = htonl(endpointPolicy.BindAddress());
#endif
    ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}
"""
        allowance = self.allowance(
            (("socket", 1), ("bind", 1)),
            (r"endpointPolicy\.IsValid\s*\(", r"endpointPolicy\.BindAddress\s*\(", r"::bind\s*\("),
            address_rules=(self.address_rule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&address)", "address",
                r"htonl\(endpointPolicy\.BindAddress\(\)\)",
            ),),
        )
        self.assert_finding(self.scan(source, (allowance,)), "source does not dominate")

    def test_goto_cannot_skip_reviewed_policy_assignment(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy& endpointPolicy) {
    if (!endpointPolicy.IsValid()) return;
    auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
    goto consume;
    address.sin_addr.s_addr = htonl(endpointPolicy.BindAddress());
consume:
    ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}
"""
        allowance = self.allowance(
            (("socket", 1), ("bind", 1)),
            (r"endpointPolicy\.IsValid\s*\(", r"endpointPolicy\.BindAddress\s*\(", r"::bind\s*\("),
            address_rules=(self.address_rule(
                "bind", 1, "reinterpret_cast<const sockaddr*>(&address)", "address",
                r"htonl\(endpointPolicy\.BindAddress\(\)\)",
            ),),
        )
        self.assert_finding(self.scan(source, (allowance,)), "unsupported goto")

    def test_ipv6_and_in6addr_any_are_rejected(self) -> None:
        source = """
void Added() {
    sockaddr_in6 address{};
    address.sin6_addr = in6addr_any;
    auto handle = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
}
"""
        self.assert_finding(self.scan(source), "forbidden unreviewed IPv6 endpoint")

    def test_address_family_fallback_resolver_is_rejected(self) -> None:
        source = """
void Added(const char* host) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    addrinfo* result = nullptr;
    getaddrinfo(host, nullptr, &hints, &result);
}
"""
        self.assert_finding(self.scan(source), "address-family fallback")

    def test_comment_only_policy_guard_does_not_satisfy_control_path(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy& endpointPolicy) {
    // endpointPolicy.IsValid();
    auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}
"""
        allowance = self.allowance(
            (("socket", 1),),
            (r"endpointPolicy\.IsValid\s*\(", r"::socket\s*\("),
        )
        self.assert_finding(self.scan(source, (allowance,)), "policy/control-path markers incomplete")

    def test_string_only_policy_guard_does_not_satisfy_control_path(self) -> None:
        source = """
void Added(const NetworkEndpointPolicy&) {
    const char* claim = "endpointPolicy.IsValid()";
    auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}
"""
        allowance = self.allowance(
            (("socket", 1),),
            (r"endpointPolicy\.IsValid\s*\(", r"::socket\s*\("),
        )
        self.assert_finding(self.scan(source, (allowance,)), "policy/control-path markers incomplete")

    def test_forbidden_tokens_and_primitives_inside_strings_are_ignored(self) -> None:
        source = ('void Added() { const char* text = '
                  '"socket(AF_INET6) getaddrinfo AF_UNSPEC inet_addr(\\\"8.8.8.8\\\") '
                  'inet_addr(\\\"0.0.0.0\\\")"; }\n')
        self.assertEqual(self.scan(source), [])

    def test_af_unix_exact_allowance_passes_and_inet_substitution_fails(self) -> None:
        source = """
void Added() {
    auto handle = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}
"""
        allowance = self.allowance(
            (("socket", 1), ("bind", 1)),
            (r"::socket\s*\(\s*AF_UNIX", r"address\.sun_family\s*=\s*AF_UNIX", r"::bind\s*\("),
            family="unix",
        )
        self.assertEqual(self.scan(source, (allowance,)), [])
        widened = source.replace("AF_UNIX", "AF_INET")
        self.assert_finding(self.scan(widened, (allowance,)), "local-IPC allowance widened beyond AF_UNIX")

    def test_new_af_inet_site_in_previously_omitted_root_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source_root = root / "SparkDaemon" / "src"
            source_root.mkdir(parents=True)
            (source_root / "Added.cpp").write_text(
                "void Added() { auto handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); }\n",
                encoding="utf-8",
            )
            findings = boundary.scan_raw_socket_inventory(root, (source_root,), ())
        self.assert_finding(findings, "unreviewed raw socket site")

    def test_pie_false_then_true_assignment_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "Panel.cpp"
            path.write_text(
                "void LaunchPIEServer() { request.bindAddress = loopback; "
                "request.lanBroadcast = false; request.lanBroadcast |= true; "
                "m_serverProcess->Launch(request); }\n",
                encoding="utf-8",
            )
            check = boundary.ControlPathCheck(
                "Panel.cpp",
                "LaunchPIEServer",
                (r"request\.bindAddress\s*=", r"request\.lanBroadcast\s*=\s*false", r"m_serverProcess->Launch\s*\("),
                "PIE broadcast policy changed",
                exact_counts=(
                    (r"\brequest\.lanBroadcast\b", 1),
                    (r"\brequest\.lanBroadcast\s*=\s*false\s*;", 1),
                ),
            )
            findings = boundary.check_control_paths(root, (check,))
        self.assert_finding(findings, "PIE broadcast policy changed")

    def test_shipped_config_checker_matches_runtime_spelling_and_comment_rules(self) -> None:
        cases = {
            "canonical": ("[Network]\nbind_address=loopback\nlan_broadcast=false\n", 0),
            "noncanonical": ("[network]\nbind_address=loopback\nLAN_BROADCAST=false\n", 1),
            "inline-comment": ("[Network]\nbind_address=loopback\nlan_broadcast=false # not a runtime comment\n", 1),
            "malformed": ("[Network]\nbind_address=loopback\nlan_broadcast=false\nnot-an-entry\n", 1),
        }
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "server.ini"
            for name, (content, minimum_failures) in cases.items():
                with self.subTest(name=name):
                    path.write_text(content, encoding="utf-8")
                    failures = boundary.check_config(path)
                    if minimum_failures == 0:
                        self.assertEqual(failures, 0)
                    else:
                        self.assertGreaterEqual(failures, minimum_failures)

    def test_production_inventory_names_every_shipped_source_root(self) -> None:
        observed = {path.relative_to(REPOSITORY_ROOT).as_posix() for path in boundary.SOURCE_ROOTS}
        expected = {
            "SparkAssetPipelineCore/include", "SparkAssetPipelineCore/src", "SparkAutomation/src",
            "SparkBuild/resources", "SparkBuild/src", "cmake",
            "SparkConsole/src", "SparkCooker/src", "SparkCrashReporter/src", "SparkDaemon/src",
            "SparkEditor/Source", "SparkEngine/Source", "SparkGateway/src", "SparkInstaller/src",
            "SparkLauncher/src", "SparkSDK/Include", "SparkServer/src", "SparkShaderCompiler/src", "SparkWorker/src",
            "GameModules", "Templates", "Tools",
        }
        self.assertEqual(observed, expected)

    def test_inventory_coverage_excludes_only_exact_reviewed_nonshipping_source(self) -> None:
        self.assertEqual(
            boundary.NON_SHIPPED_FIRST_PARTY_SOURCES,
            frozenset({"tools/gvisor-wine-shim.c"}),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shipped_root = root / "Tools"
            shipped_root.mkdir()
            (shipped_root / "shipped.cpp").write_text("int shipped;\n", encoding="utf-8")
            (shipped_root / "gvisor-wine-shim.c").write_text("int uppercase;\n", encoding="utf-8")

            developer_root = root / "tools"
            try:
                developer_root.mkdir()
            except FileExistsError:
                self.skipTest("filesystem cannot distinguish uppercase Tools from lowercase tools")
            (developer_root / "gvisor-wine-shim.c").write_text("int shim;\n", encoding="utf-8")
            (developer_root / "new-helper.c").write_text("int helper;\n", encoding="utf-8")
            (developer_root / "GVisor-Wine-Shim.c").write_text("int case_variant;\n", encoding="utf-8")
            nested_root = developer_root / "nested"
            nested_root.mkdir()
            (nested_root / "gvisor-wine-shim.c").write_text("int nested;\n", encoding="utf-8")

            with mock.patch.object(boundary, "SOURCE_ROOTS", (shipped_root,)):
                findings = boundary.scan_raw_socket_inventory(root, allowances=())

        paths = {finding.path.relative_to(root).as_posix() for finding in findings}
        self.assertNotIn("tools/gvisor-wine-shim.c", paths)
        self.assertIn("tools/new-helper.c", paths)
        self.assertIn("tools/GVisor-Wine-Shim.c", paths)
        self.assertIn("tools/nested/gvisor-wine-shim.c", paths)
        self.assertNotIn("Tools/shipped.cpp", paths)
        self.assertNotIn("Tools/gvisor-wine-shim.c", paths)

    def make_git_repository(self, root: Path) -> bool:
        """Initialise a throwaway checkout at *root*; False when git is unusable."""

        commands = (
            ("git", "init", "--quiet", str(root)),
            ("git", "-C", str(root), "config", "user.email", "guard@example.invalid"),
            ("git", "-C", str(root), "config", "user.name", "Guard Test"),
        )
        for command in commands:
            try:
                completed = subprocess.run(command, capture_output=True, timeout=60, check=False)
            except (OSError, subprocess.SubprocessError):
                return False
            if completed.returncode != 0:
                return False
        return True

    def git_add(self, root: Path, relative: str) -> None:
        subprocess.run(["git", "-C", str(root), "add", "--", relative],
                       capture_output=True, timeout=60, check=True)

    def test_inventory_coverage_ignores_untracked_working_directory_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            if not self.make_git_repository(root):
                self.skipTest("git is unavailable for the tracked-inventory test")
            shipped_root = root / "Tools"
            shipped_root.mkdir()
            (shipped_root / "shipped.cpp").write_text("int shipped;\n", encoding="utf-8")
            self.git_add(root, "Tools/shipped.cpp")

            stray_root = root / "build-local-flake" / "Source"
            stray_root.mkdir(parents=True)
            (stray_root / "Stray.cpp").write_text(
                "void Added() { (void)::socket(AF_INET, SOCK_DGRAM, 0); }\n", encoding="utf-8")
            live_root = root / "LiveProjects" / "Demo" / "Source"
            live_root.mkdir(parents=True)
            (live_root / "GameModule.cpp").write_text("int live;\n", encoding="utf-8")

            with mock.patch.object(boundary, "SOURCE_ROOTS", (shipped_root,)):
                findings = boundary.scan_raw_socket_inventory(root, allowances=())

        messages = [finding.message for finding in findings]
        self.assertNotIn("first-party source is outside the explicit network inventory", messages)

    def test_inventory_coverage_still_fails_on_tracked_source_outside_the_roots(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            if not self.make_git_repository(root):
                self.skipTest("git is unavailable for the tracked-inventory test")
            shipped_root = root / "Tools"
            shipped_root.mkdir()
            (shipped_root / "shipped.cpp").write_text("int shipped;\n", encoding="utf-8")
            self.git_add(root, "Tools/shipped.cpp")

            new_root = root / "SparkNewService" / "src"
            new_root.mkdir(parents=True)
            (new_root / "Listener.cpp").write_text("int listener;\n", encoding="utf-8")
            self.git_add(root, "SparkNewService/src/Listener.cpp")

            with mock.patch.object(boundary, "SOURCE_ROOTS", (shipped_root,)):
                findings = boundary.scan_raw_socket_inventory(root, allowances=())

        paths = {
            finding.path.relative_to(root).as_posix()
            for finding in findings
            if finding.message == "first-party source is outside the explicit network inventory"
        }
        self.assertEqual(paths, {"SparkNewService/src/Listener.cpp"})

    def test_inventory_coverage_fails_when_git_lists_no_tracked_files(self) -> None:
        """An empty tracked listing must be an error, never a clean inventory.

        A sparse checkout, a wrong --work-tree, or a git that stops enumerating
        all produce `git ls-files` success with no output. Judging that as
        "nothing outside the roots" reports the reassuring answer without ever
        having looked at a file.
        """

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            if not self.make_git_repository(root):
                self.skipTest("git is unavailable for the tracked-inventory test")
            shipped_root = root / "Tools"
            shipped_root.mkdir()
            (shipped_root / "shipped.cpp").write_text("int shipped;\n", encoding="utf-8")
            # Deliberately never `git add`ed: the index stays empty while a
            # shipped-looking source sits outside the explicit roots.
            stray_root = root / "SparkNewService" / "src"
            stray_root.mkdir(parents=True)
            (stray_root / "Listener.cpp").write_text("int listener;\n", encoding="utf-8")

            self.assertEqual(boundary._tracked_inventory_candidates(root), [])
            with mock.patch.object(boundary, "SOURCE_ROOTS", (shipped_root,)):
                findings = boundary.scan_raw_socket_inventory(root, allowances=())

        self.assert_finding(findings, "git listed no tracked files")

    def test_inventory_coverage_falls_back_to_the_walk_without_a_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.assertFalse((root / ".git").exists())
            shipped_root = root / "Tools"
            shipped_root.mkdir()
            (shipped_root / "shipped.cpp").write_text("int shipped;\n", encoding="utf-8")
            stray_root = root / "SparkNewService" / "src"
            stray_root.mkdir(parents=True)
            (stray_root / "Listener.cpp").write_text("int listener;\n", encoding="utf-8")

            with mock.patch.object(boundary, "SOURCE_ROOTS", (shipped_root,)):
                findings = boundary.scan_raw_socket_inventory(root, allowances=())

        paths = {
            finding.path.relative_to(root).as_posix()
            for finding in findings
            if finding.message == "first-party source is outside the explicit network inventory"
        }
        self.assertEqual(paths, {"SparkNewService/src/Listener.cpp"})

    def test_source_inventory_scans_inl_and_objective_cxx_files(self) -> None:
        self.assertTrue({".inl", ".mm"}.issubset(boundary.SOURCE_SUFFIXES))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source_root = root / "SparkEngine" / "Source"
            source_root.mkdir(parents=True)
            for name in ("NetworkInline.inl", "NetworkMetal.mm"):
                (source_root / name).write_text(
                    "void Added() { (void)::socket(AF_INET, SOCK_DGRAM, 0); }\n",
                    encoding="utf-8",
                )

            with mock.patch.object(boundary, "SOURCE_ROOTS", (source_root,)):
                findings = boundary.scan_raw_socket_inventory(root, allowances=())

        paths = {
            finding.path.relative_to(root).as_posix()
            for finding in findings
            if "unreviewed raw socket site" in finding.message
        }
        self.assertEqual(
            paths,
            {
                "SparkEngine/Source/NetworkInline.inl",
                "SparkEngine/Source/NetworkMetal.mm",
            },
        )


if __name__ == "__main__":
    unittest.main()
