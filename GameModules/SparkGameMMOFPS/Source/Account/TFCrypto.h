/**
 * @file TFCrypto.h
 * @brief Self-contained SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 primitives
 *        for TERRAFRONT account password hashing.
 *
 * Deliberately self-contained (stdlib only, no engine dependency) so it stays
 * linkable standalone into SparkTests, matching TFAccountSystem's existing
 * minimal-dependency convention.
 *
 * NOT reused from SparkEngine/Source/Engine/Networking/NetworkEncryption.h:
 * that header's cipher/HMAC are explicitly documented as "NOT cryptographically
 * secure" (an XOR stream cipher + FNV-1a keyed hash for casual packet-sniffing
 * resistance only) -- unsuitable for password-at-rest hashing.
 *
 * All functions are pure/stateless from the caller's perspective. Correctness
 * is pinned by known-answer tests in Tests/TestTFOnboarding.cpp (FIPS 180-2
 * SHA-256 vector, RFC 4231 HMAC-SHA256 vector, a published PBKDF2-HMAC-SHA256
 * vector) -- do not modify the algorithms without re-verifying against those.
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Terrafront::Crypto {

using Sha256Digest = std::array<uint8_t, 32>;

Sha256Digest Sha256(const uint8_t* data, size_t len);
Sha256Digest Sha256(const std::string& data);

Sha256Digest HmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen);
Sha256Digest HmacSha256(const std::string& key, const std::string& data);

// PBKDF2-HMAC-SHA256 (RFC 8018 5.2). dkLen is the desired derived-key length in bytes.
std::vector<uint8_t> Pbkdf2HmacSha256(const std::string& password, const std::vector<uint8_t>& salt,
                                       uint32_t iterations, size_t dkLen);

std::string ToHex(const uint8_t* data, size_t len);
std::string ToHex(const std::vector<uint8_t>& data);
std::vector<uint8_t> FromHex(const std::string& hex);   // empty vector on malformed input (odd length / non-hex chars)

// Constant-time (no early-exit-on-mismatch) comparisons for secret material.
// A length mismatch returns false immediately -- lengths here are derived
// from fixed public parameters (dkLen), never secret themselves.
bool ConstantTimeEquals(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
bool ConstantTimeEquals(const std::string& a, const std::string& b);

} // namespace Terrafront::Crypto
