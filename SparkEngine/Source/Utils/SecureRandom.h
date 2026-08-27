/**
 * @file SecureRandom.h
 * @brief Operating-system-backed cryptographic random byte generation.
 */
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace Spark::SecureRandom
{
    /** Fill a caller-owned buffer from the operating system CSPRNG. */
    [[nodiscard]] bool Fill(void* buffer, size_t size) noexcept;

    /** Return a lowercase hexadecimal token containing byteCount random bytes. */
    [[nodiscard]] std::string HexToken(size_t byteCount);

    /**
     * Create a new owner-only file without following an existing target link.
     * The operation fails rather than replacing an existing file.
     */
    [[nodiscard]] bool CreatePrivateFile(const std::filesystem::path& path, std::string_view contents,
                                         std::string* error = nullptr) noexcept;
} // namespace Spark::SecureRandom
