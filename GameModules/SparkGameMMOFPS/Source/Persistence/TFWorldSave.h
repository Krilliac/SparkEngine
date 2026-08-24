/**
 * @file TFWorldSave.h
 * @brief Fail-closed, continent-qualified JSON persistence helpers.
 */
#pragma once

#include "Persistence/TFJsonStrict.h"
#include "Persistence/TFSavePaths.h"
#include "Utils/JsonUtils.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

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

        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
            {
                detail = "temporary open failed";
                return false;
            }
            output << Spark::Json::StringifyPretty(root);
            if (!output.good())
            {
                detail = "temporary write failed";
                output.close();
                std::filesystem::remove(temporary, ec);
                return false;
            }
        }

        if (!SavePaths::AtomicReplace(temporary, path, ec))
        {
            detail = ec.message();
            std::error_code removeEc;
            std::filesystem::remove(temporary, removeEc);
            return false;
        }
        return true;
    }
} // namespace Terrafront::WorldSave
