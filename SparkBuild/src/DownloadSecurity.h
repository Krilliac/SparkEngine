#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace SparkBuild::DownloadSecurity
{
    // Restrict curl redirects to HTTPS when the original request was HTTPS.
    // HTTP callers may redirect only within the HTTP(S) protocol family.
    [[nodiscard]] std::string RedirectProtocolPolicy(std::string_view sourceUrl);

    // Compute and verify lowercase hexadecimal SHA-256 digests using bounded memory.
    [[nodiscard]] bool ComputeSha256(const std::filesystem::path& path, std::string& digest, std::string& error);
    [[nodiscard]] bool VerifySha256(const std::filesystem::path& path, std::string_view expectedDigest,
                                    std::string& error);
} // namespace SparkBuild::DownloadSecurity
