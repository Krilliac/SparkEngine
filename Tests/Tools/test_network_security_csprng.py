#!/usr/bin/env python3
"""Source contract for OS-backed randomness in legacy network helpers."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
LEGACY_SECURITY_HEADER = ROOT / "SparkEngine" / "Source" / "Engine" / "Networking" / "NetworkSecurity.h"
LEGACY_ENCRYPTION_SOURCE = ROOT / "SparkEngine" / "Source" / "Engine" / "Networking" / "NetworkEncryption.cpp"


class NetworkSecurityCsprngContractTests(unittest.TestCase):
    def test_header_token_and_key_generation_uses_os_csprng(self) -> None:
        source = LEGACY_SECURITY_HEADER.read_text(encoding="utf-8")

        self.assertIn('#include "../../Utils/SecureRandom.h"', source)
        self.assertIn("SecureRandom::Fill", source)
        self.assertNotIn("std::mt19937", source)
        self.assertNotIn("std::random_device", source)

    def test_encryption_generator_uses_os_csprng(self) -> None:
        source = LEGACY_ENCRYPTION_SOURCE.read_text(encoding="utf-8")

        self.assertIn('#include "../../Utils/SecureRandom.h"', source)
        self.assertIn("SecureRandom::Fill", source)
        self.assertNotIn("std::mt19937", source)
        self.assertNotIn("std::random_device", source)


if __name__ == "__main__":
    unittest.main()
