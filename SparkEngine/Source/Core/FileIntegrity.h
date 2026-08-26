#pragma once

/**
 * @file FileIntegrity.h
 * @brief Bounded-memory integrity helpers for native modules and plugins.
 */

#include <filesystem>
#include <string>

namespace Spark::FileIntegrity
{
    /** [any thread] Stream a regular file and return its lowercase SHA-256 digest. */
    [[nodiscard]] bool ComputeSha256(const std::filesystem::path& path, std::string& digest, std::string& error);
} // namespace Spark::FileIntegrity
