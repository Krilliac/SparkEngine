/**
 * @file PasswordHash.h
 * @brief Portable PBKDF2-HMAC-SHA256 password hashing helpers.
 */
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Spark::PasswordHash
{
    using Sha256Digest = std::array<uint8_t, 32>;

    /** Compute HMAC-SHA256 using the engine's portable, self-contained implementation. */
    [[nodiscard]] Sha256Digest ComputeHmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> data);

    /** Create a self-describing PBKDF2-SHA256 password hash with a fresh 128-bit salt. */
    [[nodiscard]] std::string Create(std::string_view password);

    /** Verify a self-describing hash in constant time. Unknown and legacy formats fail closed. */
    [[nodiscard]] bool Verify(std::string_view password, std::string_view encodedHash);
} // namespace Spark::PasswordHash
