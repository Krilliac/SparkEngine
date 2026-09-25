#!/usr/bin/env python3
"""Source contract for network token randomness and removal of the XOR prototype (NET-100)."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
NETWORKING = ROOT / "SparkEngine" / "Source" / "Engine" / "Networking"
TOKEN_REGISTRY_HEADER = NETWORKING / "NetworkSecurity.h"
NETWORK_STACK_HEADER = NETWORKING / "NetworkIntegration.h"
ENCRYPTION_SOURCE = NETWORKING / "NetworkEncryption.cpp"

# Identifiers that belonged to the deleted repeating-key XOR "encryption" prototype.
REMOVED_XOR_API = (
    "PacketEncrypt",
    "PacketDecrypt",
    "GetEncryptionKey",
    "SetEncryptionKey",
    "SetEncryptionEnabled",
    "IsEncryptionEnabled",
    "SECURITY_KEY_SIZE",
    "enableEncryption",
)


def code_without_comments(path: Path) -> str:
    source = path.read_text(encoding="utf-8")
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", source)


class NetworkSecurityCsprngContractTests(unittest.TestCase):
    def test_token_registry_delegates_to_fail_closed_generator(self) -> None:
        source = code_without_comments(TOKEN_REGISTRY_HEADER)

        self.assertIn('#include "NetworkEncryption.h"', source)
        self.assertIn("Spark::Net::GenerateConnectionToken(outToken)", source)
        self.assertIn("ValidateToken(", source)
        self.assertNotIn("std::mt19937", source)
        self.assertNotIn("std::random_device", source)

    def test_encryption_generator_uses_os_csprng(self) -> None:
        source = ENCRYPTION_SOURCE.read_text(encoding="utf-8")

        self.assertIn('#include "../../Utils/SecureRandom.h"', source)
        self.assertIn("SecureRandom::Fill", source)
        self.assertNotIn("std::mt19937", source)
        self.assertNotIn("std::random_device", source)

    def test_xor_prototype_is_gone_from_the_transport_surface(self) -> None:
        for header in (TOKEN_REGISTRY_HEADER, NETWORK_STACK_HEADER):
            source = code_without_comments(header)
            with self.subTest(header=header.name):
                for identifier in REMOVED_XOR_API:
                    self.assertNotIn(identifier, source)
                self.assertNotIn("^=", source)
                self.assertNotRegex(source, r"\bEncrypt\s*\(")
                self.assertNotRegex(source, r"\bDecrypt\s*\(")


if __name__ == "__main__":
    unittest.main()
