#!/usr/bin/env python3
"""Source contract for network crypto (NET-100).

The transport's randomness, AEAD, KDF, comparison and wiping all come from
libsodium (owner decision OD-06). This contract fails if the deleted XOR
prototype returns, if an in-house cryptographic primitive reappears anywhere
under SparkEngine/Source/Engine/Networking, if the GCC/Clang libsodium build
loses the barrier and hardening macros upstream always defines, or if the
Windows build script stops initializing the libsodium submodule.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
NETWORKING = ROOT / "SparkEngine" / "Source" / "Engine" / "Networking"
TOKEN_REGISTRY_HEADER = NETWORKING / "NetworkSecurity.h"
NETWORK_STACK_HEADER = NETWORKING / "NetworkIntegration.h"
ENCRYPTION_SOURCE = NETWORKING / "NetworkEncryption.cpp"
LIBSODIUM_CMAKE = ROOT / "cmake" / "SparkLibsodium.cmake"
BUILD_SH = ROOT / "build.sh"
BUILD_PS1 = ROOT / "build.ps1"

# Macros upstream build.zig (initLibConfig) defines and configure.ac probes. The
# memory fences make ACQUIRE_FENCE a real barrier in the AEAD open path, and
# HAVE_INLINE_ASM keeps the compiler barriers in sodium_memzero and friends.
LIBSODIUM_HARDENING_MACROS = (
    "HAVE_INLINE_ASM",
    "HAVE_WEAK_SYMBOLS",
    "HAVE_ATOMIC_OPS",
    "HAVE_C11_MEMORY_FENCES",
    "HAVE_GCC_MEMORY_FENCES",
)

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


def non_msvc_branch(cmake_text: str) -> str:
    """The else() branch of SparkLibsodium.cmake's top-level if(MSVC), with comment lines removed."""
    code = "\n".join(line for line in cmake_text.splitlines() if not line.lstrip().startswith("#"))
    match = re.search(r"^if\(MSVC\)\n.*?^else\(\)\n(.*?)^endif\(\)", code, flags=re.DOTALL | re.MULTILINE)
    return match.group(1) if match else ""


def missing_hardening_macros(cmake_text: str) -> list[str]:
    """Hardening macros the non-MSVC branch does not both probe and define."""
    branch = non_msvc_branch(cmake_text)
    definitions = re.search(r"foreach\(_macro ([^)]*)\)\s*if\(SPARK_SODIUM_\$\{_macro\}\)", branch)
    defined = set(definitions.group(1).split()) if definitions else set()
    probed = set(re.findall(r'"\s*SPARK_SODIUM_(HAVE_[A-Z0-9_]+)\)', branch))
    return [macro for macro in LIBSODIUM_HARDENING_MACROS if macro not in defined or macro not in probed]


def submodule_paths_sh(text: str) -> set[str]:
    block = re.search(r"^SUBMODULES=\((.*?)^\)", text, flags=re.DOTALL | re.MULTILINE)
    return set(re.findall(r'^\s*"([^"|]+)\|', block.group(1), flags=re.MULTILINE)) if block else set()


def submodule_paths_ps1(text: str) -> set[str]:
    block = re.search(r"^\$submodules = @\((.*?)^\)", text, flags=re.DOTALL | re.MULTILINE)
    paths = re.findall(r'Path\s*=\s*"([^"]+)"', block.group(1)) if block else []
    return {path.replace("\\", "/") for path in paths}


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

    def test_posix_libsodium_build_defines_upstream_hardening_macros(self) -> None:
        text = LIBSODIUM_CMAKE.read_text(encoding="utf-8")
        branch = non_msvc_branch(text)
        self.assertIn("check_c_source_compiles", branch, "SparkLibsodium.cmake lost its if(MSVC)/else() split")
        self.assertEqual(missing_hardening_macros(text), [])
        # Neither fence form available must fail configuration instead of
        # building a libsodium whose ACQUIRE_FENCE is (void) 0.
        self.assertRegex(
            branch,
            r"if\(NOT SPARK_SODIUM_HAVE_C11_MEMORY_FENCES AND NOT SPARK_SODIUM_HAVE_GCC_MEMORY_FENCES\)\s*"
            r"message\(FATAL_ERROR",
        )

    def test_hardening_scanner_flags_a_dropped_macro(self) -> None:
        # Mutation cases: the scanner must notice a macro leaving the definition
        # list or losing its probe, otherwise the check above passes vacuously.
        text = LIBSODIUM_CMAKE.read_text(encoding="utf-8")
        dropped_define = text.replace(" HAVE_GCC_MEMORY_FENCES)", ")", 1)
        self.assertNotEqual(dropped_define, text)
        self.assertEqual(missing_hardening_macros(dropped_define), ["HAVE_GCC_MEMORY_FENCES"])
        dropped_probe = text.replace("SPARK_SODIUM_HAVE_INLINE_ASM)", "SPARK_SODIUM_HAVE_ASM_UNUSED)", 1)
        self.assertNotEqual(dropped_probe, text)
        self.assertEqual(missing_hardening_macros(dropped_probe), ["HAVE_INLINE_ASM"])

    def test_windows_build_script_initializes_every_build_sh_submodule(self) -> None:
        # build.ps1 runs `git submodule update --init` only when a listed
        # submodule is missing; an unlisted libsodium stays empty and the
        # configure-time FATAL_ERROR in SparkLibsodium.cmake stops the build.
        sh_paths = submodule_paths_sh(BUILD_SH.read_text(encoding="utf-8"))
        ps1_paths = submodule_paths_ps1(BUILD_PS1.read_text(encoding="utf-8"))
        self.assertIn("ThirdParty/Security/libsodium", sh_paths)
        self.assertIn("ThirdParty/Networking/curl", ps1_paths)  # the parser reads the whole list
        self.assertEqual(sorted(sh_paths - ps1_paths), [])

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
