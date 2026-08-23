/**
 * @file PasswordHash.h
 * @brief Portable PBKDF2-HMAC-SHA256 password hashing helpers.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Spark::PasswordHash
{
    /** Create a self-describing PBKDF2-SHA256 password hash with a fresh 128-bit salt. */
    [[nodiscard]] std::string Create(std::string_view password);

    /** Verify a self-describing hash in constant time. Unknown and legacy formats fail closed. */
    [[nodiscard]] bool Verify(std::string_view password, std::string_view encodedHash);
} // namespace Spark::PasswordHash
