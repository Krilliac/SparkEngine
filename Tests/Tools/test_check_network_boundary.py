#!/usr/bin/env python3
"""Mutation tests for the NET-100 first-party raw-socket inventory."""

from __future__ import annotations

import importlib.util
import sys
import unittest
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
                  required_order: tuple[str, ...] = ()) -> object:
        return boundary.RawSocketAllowance("Src/Network.cpp", "Added", operations, required_order)

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
        )
        self.assert_finding(self.scan(source, (allowance,)), "zero-initialized bind address")

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


if __name__ == "__main__":
    unittest.main()
