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

    /**
     * @brief Whether @p child resolves inside @p parent.
     *
     * Containment is decided on NORMALIZED paths: lexically_relative does not
     * normalize, so for child = <root>/a/../../etc/x the relative form is
     * "a/../../etc/x", whose FIRST component is "a" — an escape that a check
     * inspecting only the front accepts. On Windows the comparison is also
     * case-folded, because path equality is case-sensitive there while NTFS is
     * not: "C:/Build/Out" and "C:/build/out/manifest.json" name the same tree and
     * a case-sensitive compare would reject the legitimate target.
     *
     * Exposed (rather than left in an anonymous namespace) so the predicate can be
     * tested with the un-normalized and differently-cased inputs that every
     * internal call site has already canonicalized away.
     */
    [[nodiscard]] bool IsPathContained(const std::filesystem::path& child, const std::filesystem::path& parent);

    [[nodiscard]] bool ComputeFileSha256(const std::filesystem::path& path, std::string& digest, std::string& error);
    [[nodiscard]] bool CookFile(const std::filesystem::path& source, const std::filesystem::path& output,
                                const std::string& expectedSha256, bool dryRun, bool& updated, std::string& error);
    [[nodiscard]] CookResult CookAssets(const CookRequest& request);
} // namespace Spark::AssetPipeline
