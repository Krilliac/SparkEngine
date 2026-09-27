/**
 * @file TFCrypto.h
 * @brief Self-contained SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 primitives
 *        for TERRAFRONT account password hashing.
 *
 * Deliberately self-contained (stdlib only, no engine dependency) so it stays
 * linkable standalone into SparkTests, matching TFAccountSystem's existing
 * minimal-dependency convention. It serves account storage, not the network
 * transport (whose primitives are libsodium's, NET-100).
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

namespace Terrafront::Crypto
{

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
    std::vector<uint8_t> FromHex(
        const std::string& hex); // empty vector on malformed input (odd length / non-hex chars)

    // Constant-time (no early-exit-on-mismatch) comparisons for secret material.
    // A length mismatch returns false immediately -- lengths here are derived
    // from fixed public parameters (dkLen), never secret themselves.
    bool ConstantTimeEquals(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
    bool ConstantTimeEquals(const std::string& a, const std::string& b);

    // ---------------------------------------------------------------------------
    // SCRAM-SHA-256 (RFC 5802 / RFC 7677) building blocks. The server stores only
    // StoredKey and ServerKey, neither of which lets anyone log in, and a login
    // carries a one-time ClientProof bound to fresh nonces instead of the password.
    // ---------------------------------------------------------------------------

    // Byte-wise XOR (ClientProof = ClientKey XOR ClientSignature, and back).
    Sha256Digest XorBytes(const Sha256Digest& a, const Sha256Digest& b);
    // Constant-time digest equality.
    bool ConstantTimeEquals(const Sha256Digest& a, const Sha256Digest& b);
    // RFC 4648 base64 with padding (the SCRAM "s=" salt attribute).
    std::string Base64Encode(const std::vector<uint8_t>& data);

    // Keys derived from SaltedPassword = PBKDF2-HMAC-SHA256(password, salt, i).
    // All three are secrets and are wiped on destruction; StoredKey and ServerKey
    // are what the account row keeps.
    struct ScramKeys
    {
        Sha256Digest clientKey{}; // HMAC(SaltedPassword, "Client Key")
        Sha256Digest storedKey{}; // SHA-256(ClientKey)
        Sha256Digest serverKey{}; // HMAC(SaltedPassword, "Server Key")
        ~ScramKeys();
    };

    ScramKeys DeriveScramKeys(const std::string& password, const std::vector<uint8_t>& salt, uint32_t iterations);
    // Key schedule from an existing SaltedPassword (the dk of a legacy pbkdf2-sha256 row).
    ScramKeys DeriveScramKeysFromSaltedPassword(const std::vector<uint8_t>& saltedPassword);
    // Client side: ClientProof = ClientKey XOR HMAC(StoredKey, AuthMessage).
    Sha256Digest ScramClientProof(const ScramKeys& keys, const std::string& authMessage);

} // namespace Terrafront::Crypto
