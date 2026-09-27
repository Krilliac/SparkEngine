#!/usr/bin/env python3
"""Source contract for network crypto (NET-100).

The transport's randomness, AEAD, KDF, comparison and wiping all come from
libsodium (owner decision OD-06). This contract fails if the deleted XOR
prototype returns, or if an in-house cryptographic primitive reappears anywhere
under SparkEngine/Source/Engine/Networking.
"""

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

# Fingerprints of a hand-written ChaCha20, Poly1305 or HMAC. Any of them in the
# networking code means a primitive is being implemented instead of called.
IN_HOUSE_PRIMITIVE_MARKERS = (
    ("ChaCha quarter round", re.compile(r"QuarterRound", re.IGNORECASE)),
    ("ChaCha 'expand 32-byte k' constant", re.compile(r"0x61707865", re.IGNORECASE)),
    ("Poly1305 block function", re.compile(r"poly1305_block", re.IGNORECASE)),
    ("Poly1305 accumulator type", re.compile(r"\b(?:class|struct)\s+Poly1305\b")),
    ("in-house HMAC", re.compile(r"ComputeHmacSha256")),
)

# libsodium entry points NetworkEncryption.cpp must use for each job.
REQUIRED_LIBSODIUM_CALLS = (
    "sodium_init",
    "crypto_aead_chacha20poly1305_ietf_encrypt",
    "crypto_aead_chacha20poly1305_ietf_decrypt",
    "crypto_kdf_hkdf_sha256_extract",
    "crypto_kdf_hkdf_sha256_expand",
    "randombytes_buf",
    "sodium_memcmp",
    "sodium_memzero",
)


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", source)


def code_without_comments(path: Path) -> str:
    return strip_comments(path.read_text(encoding="utf-8"))


def in_house_primitive_findings(source: str) -> list[str]:
    """Names of the in-house primitive markers present in C++ source code."""
    code = strip_comments(source)
    return [name for name, pattern in IN_HOUSE_PRIMITIVE_MARKERS if pattern.search(code)]


def networking_sources() -> list[Path]:
    return sorted(p for p in NETWORKING.rglob("*") if p.suffix in {".h", ".hpp", ".cpp"})


class NetworkSecurityCsprngContractTests(unittest.TestCase):
    def test_token_registry_delegates_to_fail_closed_generator(self) -> None:
        source = code_without_comments(TOKEN_REGISTRY_HEADER)

        self.assertIn('#include "NetworkEncryption.h"', source)
        self.assertIn("Spark::Net::GenerateConnectionToken(outToken)", source)
        self.assertIn("ValidateToken(", source)
        self.assertNotIn("std::mt19937", source)
        self.assertNotIn("std::random_device", source)

    def test_encryption_uses_libsodium_for_every_primitive(self) -> None:
        source = code_without_comments(ENCRYPTION_SOURCE)

        self.assertIn("#include <sodium.h>", source)
        for call in REQUIRED_LIBSODIUM_CALLS:
            with self.subTest(call=call):
                self.assertIn(call, source)
        self.assertNotIn("std::mt19937", source)
        self.assertNotIn("std::random_device", source)
        self.assertNotIn("volatile", source)  # hand-rolled wipe loops

    def test_no_in_house_primitive_remains_in_networking(self) -> None:
        sources = networking_sources()
        self.assertTrue(sources, f"no networking sources found under {NETWORKING}")
        self.assertIn(ENCRYPTION_SOURCE, sources)
        for path in sources:
            with self.subTest(path=path.relative_to(ROOT).as_posix()):
                self.assertEqual(in_house_primitive_findings(path.read_text(encoding="utf-8")), [])

    def test_scanner_flags_a_reintroduced_quarter_round(self) -> None:
        # Mutation case: the scanner itself must catch a primitive coming back,
        # otherwise the check above passes vacuously.
        mutated = (
            "void QuarterRound(uint32_t* s, int a, int b, int c, int d)\n"
            "{ s[a] += s[b]; s[d] ^= s[a]; }\n"
            "constexpr uint32_t kSigma0 = 0x61707865;\n"
            "class Poly1305 { void Block(); };\n"
        )
        self.assertEqual(
            in_house_primitive_findings(mutated),
            ["ChaCha quarter round", "ChaCha 'expand 32-byte k' constant", "Poly1305 accumulator type"],
        )
        self.assertEqual(in_house_primitive_findings("// QuarterRound was deleted\n"), [])

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
