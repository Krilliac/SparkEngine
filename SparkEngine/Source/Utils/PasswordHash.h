/**
 * @file PasswordHash.h
 * @brief Portable PBKDF2-HMAC-SHA256 password hashing helpers.
 */
#pragma once

#include <cstdint>
#include <string>

namespace Spark::PasswordHash
{
    /** Create a self-describing PBKDF2-SHA256 password hash with a fresh 128-bit salt. */
    [[nodiscard]] std::string Create(const std::string& password);

    /** Verify a self-describing hash in constant time. Unknown and legacy formats fail closed. */
    [[nodiscard]] bool Verify(const std::string& password, const std::string& encodedHash);
} // namespace Spark::PasswordHash
