/**
 * @file GoldenImageManifest.h
 * @brief Reviewed-threshold manifest for golden image baselines.
 *
 * `Tests/GoldenImages/manifest.json` pins, for every scene on every backend
 * row, the reviewed comparison thresholds, the reviewer, and the SHA-256 of
 * the committed baseline PNG. GoldenImageTestRunner reads thresholds only
 * from here, so a comparison without a reviewed entry cannot pass.
 *
 * Schema (every field required, unknown keys rejected, any invalid entry
 * rejects the whole manifest):
 * @code
 *   {
 *     "schemaVersion": 1,
 *     "entries": [
 *       {
 *         "scene": "TriangleClear",
 *         "backendRow": "vulkan-lavapipe",
 *         "software": true,
 *         "perPixelThreshold": 10,
 *         "tolerancePercent": 0.5,
 *         "reviewer": "name",
 *         "baselineSha256": "<64 lowercase hex>"
 *       }
 *     ]
 *   }
 * @endcode
 *
 * @see GoldenImageTest.h, Tests/GoldenImages/README.md
 */

#pragma once

#include "JsonUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Spark
{

    /**
     * @brief One reviewed baseline: a scene rendered on one backend row.
     */
    struct GoldenManifestEntry
    {
        std::string scene;           ///< Scene id; the baseline is <backendRow>/<scene>.png.
        std::string backendRow;      ///< d3d11-warp, d3d11-hw, opengl-llvmpipe or vulkan-lavapipe.
        bool software = false;       ///< True for software rasterizer rows; must agree with the row.
        float perPixelThreshold = 0; ///< Euclidean RGB distance below which a pixel matches.
        float tolerancePercent = 0;  ///< Maximum percent of differing pixels.
        std::string reviewer;        ///< Who reviewed the baseline and thresholds.
        std::string baselineSha256;  ///< Lowercase hex SHA-256 of the committed baseline PNG.
    };

    namespace GoldenManifest
    {
        /// Largest possible Euclidean RGB distance, sqrt(3 * 255^2).
        inline constexpr double kMaxPixelDistance = 441.68;

        /**
         * @brief Resolve a backend row that golden baselines may target.
         * @param row      Row id.
         * @param software [out] True for a software rasterizer row.
         * @return False for an unknown row.
         */
        [[nodiscard]] inline bool ResolveBackendRow(std::string_view row, bool& software)
        {
            if (row == "d3d11-warp" || row == "opengl-llvmpipe" || row == "vulkan-lavapipe")
            {
                software = true;
                return true;
            }
            if (row == "d3d11-hw")
            {
                software = false;
                return true;
            }
            return false;
        }

        /** @brief True for a backend row that golden baselines may target. */
        [[nodiscard]] inline bool IsKnownBackendRow(std::string_view row)
        {
            bool software = false;
            return ResolveBackendRow(row, software);
        }

        /** @brief Scene ids are 1-128 chars of [A-Za-z0-9_-]; they become file names. */
        [[nodiscard]] inline bool IsValidSceneId(std::string_view scene)
        {
            if (scene.empty() || scene.size() > 128)
            {
                return false;
            }
            return std::all_of(scene.begin(), scene.end(),
                               [](char c) {
                                   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                          c == '_' || c == '-';
                               });
        }

        namespace Detail
        {
            /** @brief True when every key of @p object is in @p allowed. */
            [[nodiscard]] inline bool HasOnlyKeys(const Json::Value& object, std::span<const std::string_view> allowed)
            {
                const auto keys = object.GetKeys();
                return std::all_of(keys.begin(), keys.end(), [&](const std::string& key)
                                   { return std::find(allowed.begin(), allowed.end(), key) != allowed.end(); });
            }

            /** @brief Validate one manifest entry; see the file comment for the schema. */
            [[nodiscard]] inline bool ParseEntry(const Json::Value& value, GoldenManifestEntry& entry,
                                                 std::string& error)
            {
                static constexpr std::array<std::string_view, 7> kFields = {
                    "scene",    "backendRow",    "software", "perPixelThreshold", "tolerancePercent",
                    "reviewer", "baselineSha256"};
                if (!value.IsObject() || !HasOnlyKeys(value, kFields))
                {
                    error = "entry must be an object with only the documented fields";
                    return false;
                }
                for (const auto field : kFields)
                {
                    if (!value.HasKey(std::string(field)))
                    {
                        error = "missing field '" + std::string(field) + "'";
                        return false;
                    }
                }

                const Json::Value& perPixel = value["perPixelThreshold"];
                const Json::Value& tolerance = value["tolerancePercent"];
                if (!value["scene"].IsString() || !value["backendRow"].IsString() || !value["reviewer"].IsString() ||
                    !value["baselineSha256"].IsString() || !value["software"].IsBool() || !perPixel.IsNumber() ||
                    !tolerance.IsNumber())
                {
                    error = "field has the wrong type";
                    return false;
                }

                entry.scene = value["scene"].AsString();
                entry.backendRow = value["backendRow"].AsString();
                entry.software = value["software"].AsBool();
                entry.reviewer = value["reviewer"].AsString();
                entry.baselineSha256 = value["baselineSha256"].AsString();
                const double perPixelValue = perPixel.AsNumber();
                const double toleranceValue = tolerance.AsNumber();

                bool rowIsSoftware = false;
                if (!IsValidSceneId(entry.scene))
                {
                    error = "scene id must be 1-128 chars of [A-Za-z0-9_-]";
                    return false;
                }
                if (!ResolveBackendRow(entry.backendRow, rowIsSoftware))
                {
                    error = "unknown backendRow '" + entry.backendRow + "'";
                    return false;
                }
                if (rowIsSoftware != entry.software)
                {
                    error = "software flag disagrees with backendRow '" + entry.backendRow + "'";
                    return false;
                }
                if (!std::isfinite(perPixelValue) || perPixelValue < 0.0 || perPixelValue > kMaxPixelDistance)
                {
                    error = "perPixelThreshold must be finite and within [0, 441.68]";
                    return false;
                }
                if (!std::isfinite(toleranceValue) || toleranceValue < 0.0 || toleranceValue > 100.0)
                {
                    error = "tolerancePercent must be finite and within [0, 100]";
                    return false;
                }
                if (entry.reviewer.find_first_not_of(" \t") == std::string::npos)
                {
                    error = "reviewer must be non-empty";
                    return false;
                }
                if (entry.baselineSha256.size() != 64 ||
                    !std::all_of(entry.baselineSha256.begin(), entry.baselineSha256.end(),
                                 [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
                {
                    error = "baselineSha256 must be 64 lowercase hex characters";
                    return false;
                }

                entry.perPixelThreshold = static_cast<float>(perPixelValue);
                entry.tolerancePercent = static_cast<float>(toleranceValue);
                return true;
            }
        } // namespace Detail

        /**
         * @brief Parse and validate a reviewed-threshold manifest file.
         * @param path    Manifest file path.
         * @param entries [out] Validated entries (empty on failure).
         * @param error   [out] Reason on failure.
         * @return True when the manifest is valid. An empty entry list is valid.
         */
        [[nodiscard]] inline bool Load(const std::filesystem::path& path, std::vector<GoldenManifestEntry>& entries,
                                       std::string& error)
        {
            entries.clear();
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                error = "cannot open " + path.string();
                return false;
            }
            const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

            Json::JsonLimits limits;
            limits.maxBytes = 1024u * 1024u;
            limits.maxDepth = 8u;
            Json::Value root;
            std::string parseError;
            if (!Json::ParseBounded(text, limits, &root, &parseError))
            {
                error = "invalid JSON: " + parseError;
                return false;
            }

            // Read through a const reference: the mutable operator[] inserts missing keys.
            const Json::Value& doc = root;
            static constexpr std::array<std::string_view, 2> kRootFields = {"schemaVersion", "entries"};
            if (!doc.IsObject() || !Detail::HasOnlyKeys(doc, kRootFields) || !doc["schemaVersion"].IsNumber() ||
                doc["schemaVersion"].AsNumber() != 1.0 || !doc["entries"].IsArray())
            {
                error = "root must be {\"schemaVersion\": 1, \"entries\": [...]}";
                return false;
            }

            const Json::Value& list = doc["entries"];
            std::vector<GoldenManifestEntry> parsed;
            parsed.reserve(list.Size());
            for (size_t i = 0; i < list.Size(); ++i)
            {
                GoldenManifestEntry entry;
                std::string entryError;
                if (!Detail::ParseEntry(list[i], entry, entryError))
                {
                    error = "entries[" + std::to_string(i) + "]: " + entryError;
                    return false;
                }
                const bool duplicate =
                    std::any_of(parsed.begin(), parsed.end(), [&](const GoldenManifestEntry& previous)
                                { return previous.scene == entry.scene && previous.backendRow == entry.backendRow; });
                if (duplicate)
                {
                    error = "entries[" + std::to_string(i) + "]: duplicate scene/backendRow";
                    return false;
                }
                parsed.push_back(std::move(entry));
            }

            entries = std::move(parsed);
            return true;
        }
    } // namespace GoldenManifest

} // namespace Spark
