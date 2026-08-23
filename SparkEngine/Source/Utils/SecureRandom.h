/**
 * @file SecureRandom.h
 * @brief Operating-system-backed cryptographic random byte generation.
 */
#pragma once

#include <cstddef>
#include <string>

namespace Spark::SecureRandom
{
    /** Fill a caller-owned buffer from the operating system CSPRNG. */
    [[nodiscard]] bool Fill(void* buffer, size_t size) noexcept;

    /** Return a lowercase hexadecimal token containing byteCount random bytes. */
    [[nodiscard]] std::string HexToken(size_t byteCount);
} // namespace Spark::SecureRandom
