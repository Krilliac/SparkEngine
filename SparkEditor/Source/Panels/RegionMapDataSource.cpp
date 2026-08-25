/**
 * @file RegionMapDataSource.cpp
 * @brief Safe Terrafront continent-to-region-map discovery for SparkEditor.
 */

#include "RegionMapDataSource.h"

#include "Utils/JsonUtils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace SparkEditor
{
    namespace
    {
        fs::path TemporarySibling(const fs::path& destination)
        {
            static std::atomic<uint64_t> sequence{0};
            const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
            fs::path temporary = destination;
            temporary += ".spark-save-tmp-" + std::to_string(tick) + "-" +
                         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
            return temporary;
        }

        bool ReadBytes(const fs::path& path, std::string& out)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
                return false;
            std::ostringstream bytes;
            bytes << input.rdbuf();
            if (input.bad())
                return false;
            out = bytes.str();
            return true;
        }

        bool WriteBytes(const fs::path& path, std::string_view bytes, std::error_code& ec)
        {
            ec.clear();
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
            {
                ec = std::make_error_code(std::errc::io_error);
                return false;
            }
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output.good())
            {
                ec = std::make_error_code(std::errc::io_error);
                output.close();
                std::error_code removeError;
                fs::remove(path, removeError);
                return false;
            }
            output.close();
            if (!output.good())
            {
                ec = std::make_error_code(std::errc::io_error);
                std::error_code removeError;
                fs::remove(path, removeError);
                return false;
            }
            return true;
        }

        bool AtomicReplace(const fs::path& temporary, const fs::path& destination, std::error_code& ec)
        {
#ifdef _WIN32
            if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                ec.clear();
                return true;
            }
            ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            return false;
#else
            fs::rename(temporary, destination, ec);
            return !ec;
#endif
        }

        bool StrictJson(std::string_view document, std::string& parseError)
        {
            Spark::Json::Value root;
            return Spark::Json::ParseStrict(std::string(document), &root, &parseError);
        }
    } // namespace

    bool IsSafeRegionMapFileName(std::string_view value)
    {
        if (value.empty() || value == "." || value == ".." || value.size() > 128)
            return false;
        if (value.find('/') != std::string_view::npos || value.find('\\') != std::string_view::npos ||
            value.find(':') != std::string_view::npos)
            return false;
        const fs::path path{std::string(value)};
        if (path.is_absolute() || path.has_root_path() || path.filename() != path || path.extension() != ".json")
            return false;
        return std::all_of(value.begin(), value.end(),
                           [](char c)
                           {
                               const auto byte = static_cast<unsigned char>(c);
                               return std::isalnum(byte) != 0 || c == '_' || c == '-' || c == '.';
                           });
    }

    bool LoadRegionMapDataSources(const fs::path& dataDirectory, std::vector<RegionMapDataSource>& outSources,
                                  std::string& outError)
    {
        outSources.clear();
        outError.clear();

        std::error_code ec;
        const fs::path dataRoot = fs::absolute(dataDirectory, ec).lexically_normal();
        if (ec || !fs::is_directory(dataRoot, ec))
        {
            outError = "Terrafront data directory is unavailable: " + dataDirectory.generic_string();
            return false;
        }

        const fs::path registryPath = dataRoot / "continents.json";
        std::ifstream registry(registryPath, std::ios::binary);
        if (!registry.is_open())
        {
            outError = "cannot open continent registry '" + registryPath.generic_string() + "'";
            return false;
        }
        std::ostringstream bytes;
        bytes << registry.rdbuf();

        Spark::Json::Value root;
        std::string parseError;
        if (!Spark::Json::ParseStrict(bytes.str(), &root, &parseError) || !root.IsObject() ||
            !root.HasKey("continents") || !root["continents"].IsArray())
        {
            outError = "invalid continent registry: " + (parseError.empty() ? "missing continents array" : parseError);
            return false;
        }

        std::unordered_set<int> mapIds;
        std::unordered_set<std::string> keys;
        std::unordered_set<std::string> files;
        const Spark::Json::Value& entries = root["continents"];
        for (size_t index = 0; index < entries.Size(); ++index)
        {
            const Spark::Json::Value& entry = entries[index];
            if (!entry.IsObject() || !entry.HasKey("regions"))
                continue;
            if (!entry["regions"].IsString())
            {
                outError = "continent entry " + std::to_string(index) + " has a non-string regions field";
                return false;
            }

            RegionMapDataSource source;
            source.mapId = entry.HasKey("mapId") ? entry["mapId"].AsInt(-1) : -1;
            source.key = entry.HasKey("key") && entry["key"].IsString() ? entry["key"].AsString() : std::string{};
            source.name = entry.HasKey("name") && entry["name"].IsString() ? entry["name"].AsString() : source.key;
            source.regionsFile = entry["regions"].AsString();

            if (source.mapId < 0 || source.key.empty() || source.name.empty())
            {
                outError = "continent entry " + std::to_string(index) + " is missing mapId, key, or name";
                return false;
            }
            if (!IsSafeRegionMapFileName(source.regionsFile))
            {
                outError = "continent '" + source.key + "' has an unsafe regions filename: " + source.regionsFile;
                return false;
            }
            if (!mapIds.insert(source.mapId).second || !keys.insert(source.key).second ||
                !files.insert(source.regionsFile).second)
            {
                outError = "duplicate mapId, key, or regions filename in continent registry";
                return false;
            }

            source.dataPath = (dataRoot / source.regionsFile).lexically_normal();
            if (source.dataPath.parent_path() != dataRoot || !fs::is_regular_file(source.dataPath, ec))
            {
                outError = "continent '" + source.key +
                           "' references a missing region map: " + source.dataPath.generic_string();
                return false;
            }
            outSources.push_back(std::move(source));
        }

        if (outSources.empty())
        {
            outError = "continent registry contains no editable region maps";
            return false;
        }
        std::sort(outSources.begin(), outSources.end(),
                  [](const RegionMapDataSource& a, const RegionMapDataSource& b) { return a.mapId < b.mapId; });
        return true;
    }

    bool WriteRegionMapDocumentAtomically(const fs::path& destination, std::string_view document, std::string& outError)
    {
        outError.clear();
        if (destination.empty())
        {
            outError = "region map destination is empty";
            return false;
        }

        std::string parseError;
        if (!StrictJson(document, parseError))
        {
            outError = "refusing to save invalid JSON (" + parseError + ")";
            return false;
        }

        std::error_code ec;
        const fs::path parent = destination.parent_path();
        if (!parent.empty() && !fs::is_directory(parent, ec))
        {
            outError = "region map directory is unavailable: " + parent.generic_string();
            return false;
        }

        std::string oldBytes;
        const bool destinationExists = fs::exists(destination, ec);
        if (ec)
        {
            outError = "cannot inspect existing region map: " + ec.message();
            return false;
        }
        bool hadOld = false;
        if (destinationExists)
        {
            hadOld = fs::is_regular_file(destination, ec);
            if (ec || !hadOld)
            {
                outError = "region map destination is not a regular file: " + destination.generic_string();
                return false;
            }
        }
        if (hadOld && !ReadBytes(destination, oldBytes))
        {
            outError = "cannot read existing region map before backup: " + destination.generic_string();
            return false;
        }

        const fs::path documentTemporary = TemporarySibling(destination);
        if (!WriteBytes(documentTemporary, document, ec))
        {
            outError = "cannot write temporary region map: " + ec.message();
            return false;
        }

        std::string temporaryBytes;
        if (!ReadBytes(documentTemporary, temporaryBytes) || temporaryBytes != document ||
            !StrictJson(temporaryBytes, parseError))
        {
            std::error_code removeError;
            fs::remove(documentTemporary, removeError);
            outError = "temporary region map verification failed";
            if (!parseError.empty())
                outError += " (" + parseError + ")";
            return false;
        }

        if (hadOld)
        {
            fs::path backup = destination;
            backup += ".bak";
            const fs::path backupTemporary = TemporarySibling(backup);
            if (!WriteBytes(backupTemporary, oldBytes, ec) || !AtomicReplace(backupTemporary, backup, ec))
            {
                std::error_code removeError;
                fs::remove(backupTemporary, removeError);
                fs::remove(documentTemporary, removeError);
                outError = "cannot commit region map backup '" + backup.generic_string() + "': " + ec.message();
                return false;
            }
        }

        if (!AtomicReplace(documentTemporary, destination, ec))
        {
            std::error_code removeError;
            fs::remove(documentTemporary, removeError);
            outError = "cannot atomically replace region map '" + destination.generic_string() + "': " + ec.message();
            return false;
        }

        std::string committedBytes;
        parseError.clear();
        if (!ReadBytes(destination, committedBytes) || committedBytes != document ||
            !StrictJson(committedBytes, parseError))
        {
            outError = "committed region map verification failed";
            if (hadOld)
            {
                const fs::path restoreTemporary = TemporarySibling(destination);
                if (WriteBytes(restoreTemporary, oldBytes, ec) && AtomicReplace(restoreTemporary, destination, ec))
                    outError += "; original restored from backup bytes";
                else
                    outError += "; original restore failed: " + ec.message();
            }
            return false;
        }
        return true;
    }
} // namespace SparkEditor
