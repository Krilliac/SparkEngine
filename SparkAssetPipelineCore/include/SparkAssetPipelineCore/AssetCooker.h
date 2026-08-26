/** @file AssetCooker.h @brief Deterministic headless asset-cooking primitives. */
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Spark::AssetPipeline
{
    struct CookRecord
    {
        std::string path;
        std::string sha256;
        std::uintmax_t size = 0;
        bool updated = false;
    };

    struct CookRequest
    {
        std::filesystem::path sourceRoot;
        std::filesystem::path outputRoot;
        std::filesystem::path manifestPath;
        bool dryRun = false;
        std::function<void(const CookRecord&, std::size_t, std::size_t)> onProgress;
    };

    struct CookResult
    {
        std::vector<CookRecord> records;
        std::string manifestSha256;
        std::string error;
        std::size_t updatedCount = 0;
        std::size_t unchangedCount = 0;
        [[nodiscard]] bool Succeeded() const noexcept { return error.empty(); }
    };

    [[nodiscard]] bool ComputeFileSha256(const std::filesystem::path& path, std::string& digest, std::string& error);
    [[nodiscard]] bool CookFile(const std::filesystem::path& source, const std::filesystem::path& output,
                                const std::string& expectedSha256, bool dryRun, bool& updated, std::string& error);
    [[nodiscard]] CookResult CookAssets(const CookRequest& request);
} // namespace Spark::AssetPipeline
