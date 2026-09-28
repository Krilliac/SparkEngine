/**
 * @file TFWorldSave.h
 * @brief Fail-closed, continent-qualified JSON persistence helpers.
 */
#pragma once

#include "Data/TFDataTables.h"
#include "Persistence/TFJsonStrict.h"
#include "Persistence/TFSavePaths.h"
#include "Utils/JsonUtils.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Terrafront::WorldSave
{
    enum class ReadStatus
    {
        Missing,
        Loaded,
        Unreadable,
        Corrupt,
        WrongContinent,
    };

    inline bool ReadUint32(const Spark::Json::Value& value, uint32_t& out) noexcept
    {
        if (!value.IsNumber())
            return false;
        const double number = value.AsNumber(-1.0);
        if (!std::isfinite(number) || number < 0.0 ||
            number > static_cast<double>(std::numeric_limits<uint32_t>::max()) || std::trunc(number) != number)
            return false;
        out = static_cast<uint32_t>(number);
        return true;
    }

    struct DominionState
    {
        bool active = false;
        uint32_t faction = 0;
        double remainingSec = 0.0;
    };

    /**
     * Validate the complete persisted dominion object before exposing it.
     * Versioned saves use a canonical three-field schema even while inactive,
     * so malformed values cannot hide behind `active: false`. Pre-versioned
     * saves may omit the inactive fields, but an active legacy hold must still
     * provide valid values.
     */
    inline bool ReadDominionState(const Spark::Json::Value& value, bool requireCanonical, uint32_t factionCount,
                                  DominionState& out) noexcept
    {
        if (!value.IsObject() || factionCount <= 1)
            return false;

        const bool hasActive = value.HasKey("active");
        const bool hasFaction = value.HasKey("faction");
        const bool hasRemaining = value.HasKey("remainingSec");
        if (requireCanonical && (!hasActive || !hasFaction || !hasRemaining))
            return false;
        if (hasActive && !value["active"].IsBool())
            return false;

        DominionState parsed;
        parsed.active = hasActive && value["active"].AsBool(false);

        if (hasFaction)
        {
            if (!ReadUint32(value["faction"], parsed.faction) || parsed.faction >= factionCount)
                return false;
        }
        if (hasRemaining)
        {
            if (!value["remainingSec"].IsNumber())
                return false;
            parsed.remainingSec = value["remainingSec"].AsNumber(-1.0);
            if (!std::isfinite(parsed.remainingSec) || parsed.remainingSec < 0.0 || parsed.remainingSec > 600.0)
                return false;
        }

        if (parsed.active)
        {
            if (!hasFaction || !hasRemaining || parsed.faction == 0)
                return false;
        }
        else if (requireCanonical && (parsed.faction != 0 || parsed.remainingSec != 0.0))
        {
            return false;
        }

        out = parsed;
        return true;
    }

    /// Territory file schema (terrafront_territory.<continent-key>.json). v0 is the pre-versioned layout.
    inline constexpr uint32_t kTerritorySchemaVersion = 1;

    enum class TerritoryDecodeResult
    {
        Loaded,      ///< current schema, every field valid; adopt `out` as is
        Migrate,     ///< valid v0 or legacy-location document; adopt `out`, then rewrite as the current schema
        NewerSchema, ///< written by a newer build; must not be loaded or rewritten (rollback guard)
        Invalid,     ///< malformed or inconsistent; `detail` says which field
    };

    struct TerritoryDecode
    {
        std::vector<FactionId> owners; ///< one validated owner per region, skyanchors at their home faction
        DominionState dominion;
        uint32_t sourceVersion = 0;
    };

    /**
     * Pure decode of a territory document that ReadJson already matched to the continent.
     *
     * Reads schema N (kTerritorySchemaVersion) and N-1 (v0, no "version"), refuses a newer schema, and
     * validates the region lattice against `regions`: the owner count, each owner id, and that a skyanchor
     * is held by its home faction. A v0 or legacy-location (`legacySource`) document may carry a skyanchor
     * owner or omit "dominion"; the owner is coerced to the home faction and the dominion defaults to
     * inactive. `out` is written only on Loaded or Migrate. Game thread only (no I/O, no allocation beyond
     * `out` and `detail`).
     */
    inline TerritoryDecodeResult DecodeTerritory(const Spark::Json::Value& root, std::span<const RegionDef> regions,
                                                 bool legacySource, TerritoryDecode& out, std::string& detail)
    {
        detail.clear();
        uint32_t version = 0;
        if (root.HasKey("version") && !ReadUint32(root["version"], version))
        {
            detail = "invalid schema version";
            return TerritoryDecodeResult::Invalid;
        }
        if (version > kTerritorySchemaVersion)
        {
            detail = "schema version " + std::to_string(version) + " is newer than supported " +
                     std::to_string(kTerritorySchemaVersion);
            return TerritoryDecodeResult::NewerSchema;
        }
        // v0 is the sole older schema: it has the fields validated below and is rewritten only after all of
        // them pass. No other downgrade is inferred.
        const bool olderSchema = version == 0;

        const size_t count = regions.size();
        const Spark::Json::Value& owners = root["owners"];
        uint32_t persistedCount = 0;
        if (!ReadUint32(root["regionCount"], persistedCount) || persistedCount != count || !owners.IsArray() ||
            owners.Size() != count)
        {
            detail = "invalid region lattice";
            return TerritoryDecodeResult::Invalid;
        }

        TerritoryDecode decoded;
        decoded.sourceVersion = version;
        decoded.owners.resize(count);
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t raw = 0;
            if (!ReadUint32(owners[i], raw) || raw >= static_cast<uint32_t>(FactionId::COUNT))
            {
                detail = "owner " + std::to_string(i) + " is malformed";
                return TerritoryDecodeResult::Invalid;
            }
            FactionId owner = static_cast<FactionId>(raw);
            if (regions[i].tier == "skyanchor")
            {
                if (owner != regions[i].homeFaction && !legacySource && !olderSchema)
                {
                    detail = "skyanchor owner " + std::to_string(i) + " conflicts with its home faction";
                    return TerritoryDecodeResult::Invalid;
                }
                owner = regions[i].homeFaction;
            }
            decoded.owners[i] = owner;
        }

        if (!root.HasKey("dominion"))
        {
            if (!olderSchema)
            {
                detail = "missing dominion";
                return TerritoryDecodeResult::Invalid;
            }
        }
        else if (!ReadDominionState(root["dominion"], !olderSchema, static_cast<uint32_t>(FactionId::COUNT),
                                    decoded.dominion))
        {
            detail = "invalid dominion";
            return TerritoryDecodeResult::Invalid;
        }

        out = std::move(decoded);
        return legacySource || olderSchema ? TerritoryDecodeResult::Migrate : TerritoryDecodeResult::Loaded;
    }

    inline ReadStatus ReadJson(const std::filesystem::path& path, std::string_view expectedKey,
                               std::string_view expectedLegacyName, bool allowLegacyName, Spark::Json::Value& out,
                               std::string& detail)
    {
        out = Spark::Json::Value{};
        detail.clear();
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec)
        {
            detail = ec.message();
            return ReadStatus::Unreadable;
        }
        if (!exists)
            return ReadStatus::Missing;

        const bool isRegularFile = std::filesystem::is_regular_file(path, ec);
        if (ec || !isRegularFile)
        {
            detail = ec ? ec.message() : "path is not a regular file";
            return ReadStatus::Unreadable;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            detail = "open failed";
            return ReadStatus::Unreadable;
        }
        std::ostringstream stream;
        stream << input.rdbuf();
        if (input.bad())
        {
            detail = "read failed";
            return ReadStatus::Unreadable;
        }

        const std::string text = stream.str();
        if (!JsonStrict::ValidateLexemes(text, {"remainingSec"}, detail))
            return ReadStatus::Corrupt;

        Spark::Json::Value root;
        if (!Spark::Json::ParseStrict(text, &root, &detail) || !root.IsObject())
        {
            if (detail.empty())
                detail = "root is not an object";
            return ReadStatus::Corrupt;
        }

        if (root.HasKey("continentKey"))
        {
            if (!root["continentKey"].IsString())
            {
                detail = "continentKey is not a string";
                return ReadStatus::Corrupt;
            }
            if (root["continentKey"].AsString() != expectedKey)
            {
                detail = "continentKey mismatch";
                return ReadStatus::WrongContinent;
            }
        }
        else if (allowLegacyName && root["continent"].IsString())
        {
            if (root["continent"].AsString() != expectedLegacyName)
            {
                detail = "legacy continent mismatch";
                return ReadStatus::WrongContinent;
            }
        }
        else
        {
            detail = "missing continent identity";
            return ReadStatus::WrongContinent;
        }

        out = std::move(root);
        return ReadStatus::Loaded;
    }

    inline bool WriteJson(const std::filesystem::path& path, const Spark::Json::Value& root, std::string& detail)
    {
        detail.clear();
        if (path.empty())
        {
            detail = "empty destination";
            return false;
        }

        std::error_code ec;
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                detail = ec.message();
                return false;
            }
        }

        if (!SavePaths::WriteDurableReplace(path, Spark::Json::StringifyPretty(root), ec))
        {
            detail = ec.message();
            return false;
        }
        return true;
    }
} // namespace Terrafront::WorldSave
