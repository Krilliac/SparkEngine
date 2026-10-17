/**
 * @file ModSystemIO.cpp
 * @brief Mod system persistence and console output — config save/load, mod.json parsing, console status
 */

#include "ModSystem.h"
#include "../../Utils/FileUtils.h"
#include "../../Utils/JsonUtils.h"
#include "../../Utils/LogMacros.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>

namespace Spark
{

    namespace
    {
        /// A mod manifest / mod config is a hand-written document, never a data
        /// dump (see ModSystem::kMaxManifestBytes); depth 16 is far above the two
        /// levels these documents actually use.
        constexpr size_t MAX_MOD_JSON_BYTES = ModSystem::kMaxManifestBytes;
        constexpr Json::JsonLimits MOD_JSON_LIMITS{.maxBytes = MAX_MOD_JSON_BYTES, .maxDepth = 16u, .maxNodes = 4096u};

        /// A mod id is a map key, a log argument, a SaveConfig field and a UI label, so it
        /// is a short printable token: no separators, control bytes, NUL or spaces.
        constexpr size_t MAX_MOD_ID_BYTES = 128;

        bool IsValidModId(std::string_view id)
        {
            if (id.empty() || id.size() > MAX_MOD_ID_BYTES || id == "." || id == "..")
            {
                return false;
            }
            return std::all_of(id.begin(), id.end(),
                               [](char c)
                               {
                                   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                          c == '.' || c == '_' || c == '-';
                               });
        }

        /// Read the mod config file whole, refusing an oversized file from its
        /// directory entry BEFORE any of its bytes are pulled into memory. The config
        /// path is engine configuration, not the untrusted mods tree, so it is opened
        /// by path; mod manifests are read by ScanForMods through the held mods-root
        /// handle instead (ModSystem.cpp).
        bool ReadModConfigFile(const std::string& path, std::string& outContent)
        {
            // Engine path strings are UTF-8; the narrow std::filesystem / fstream constructors
            // would decode them in the Windows ANSI code page and miss a non-ASCII mod folder.
            const std::filesystem::path nativePath = FileUtils::PathFromUtf8(path);
            std::error_code ec;
            if (nativePath.empty() || !std::filesystem::is_regular_file(nativePath, ec) || ec)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: '%s' is not a regular file", path.c_str());
                return false;
            }
            const auto fileSize = std::filesystem::file_size(nativePath, ec);
            if (ec)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: cannot stat '%s' (%s)", path.c_str(),
                                ec.message().c_str());
                return false;
            }
            if (fileSize > MAX_MOD_JSON_BYTES)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: '%s' is %llu bytes, above the %zu byte limit",
                                path.c_str(), static_cast<unsigned long long>(fileSize), MAX_MOD_JSON_BYTES);
                return false;
            }

            std::ifstream file(nativePath, std::ios::binary);
            if (!file.is_open())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: cannot open '%s' (errno=%d)", path.c_str(),
                                errno);
                return false;
            }

            // The stat above is only a fast reject: the file can be swapped or keep growing
            // between it and this read (or report a size that lies, as procfs files do).
            // The read itself is therefore bounded to one byte past the limit, which is
            // what actually caps memory; seeing that extra byte means the file is too big.
            outContent.assign(MAX_MOD_JSON_BYTES + 1, '\0');
            file.read(outContent.data(), static_cast<std::streamsize>(outContent.size()));
            outContent.resize(static_cast<size_t>(file.gcount()));
            if (outContent.size() > MAX_MOD_JSON_BYTES)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: '%s' grew past the %zu byte limit while reading",
                                path.c_str(), MAX_MOD_JSON_BYTES);
                outContent.clear();
                return false;
            }
            return true;
        }
    } // namespace

    bool ModSystem::SaveConfig(const std::string& filePath) const
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "ModSystem::SaveConfig to '%s'", filePath.c_str());
        std::ofstream file(FileUtils::PathFromUtf8(filePath));
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem::SaveConfig failed to open '%s'", filePath.c_str());
            return false;
        }

        auto modsArray = Json::Value::MakeArray();
        for (const auto& [id, info] : m_mods)
        {
            Json::Value entry;
            entry["id"] = Json::Value(id);
            entry["enabled"] = Json::Value(info.enabled);
            entry["loadOrder"] = Json::Value(info.loadOrder);
            modsArray.PushBack(std::move(entry));
        }

        Json::Value root;
        root["mods"] = std::move(modsArray);
        file << Json::StringifyPretty(root) << "\n";
        file.close();
        if (file.fail())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem::SaveConfig write failed for '%s'", filePath.c_str());
            return false;
        }
        return true;
    }

    bool ModSystem::LoadConfig(const std::string& filePath)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "ModSystem::LoadConfig from '%s'", filePath.c_str());

        std::string content;
        if (!ReadModConfigFile(filePath, content))
        {
            return false;
        }

        Json::Value root;
        std::string parseError;
        if (!Json::ParseBounded(content, MOD_JSON_LIMITS, &root, &parseError))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "ModSystem::LoadConfig rejected '%s': %s", filePath.c_str(),
                           parseError.c_str());
            return false;
        }

        if (!root.IsObject() || !root.HasKey("mods") || !root["mods"].IsArray())
        {
            return false;
        }

        const auto& modsArray = root["mods"];
        for (size_t i = 0; i < modsArray.Size(); ++i)
        {
            const auto& entry = modsArray[i];
            if (!entry.IsObject() || !entry.HasKey("id") || !entry["id"].IsString())
            {
                continue;
            }

            std::string modId = entry["id"].AsString();
            auto modIt = m_mods.find(modId);
            if (modIt != m_mods.end())
            {
                modIt->second.enabled =
                    entry.HasKey("enabled") && entry["enabled"].IsBool() ? entry["enabled"].AsBool() : false;
                // A missing, non-integer or out-of-int-range loadOrder falls back
                // to the default priority instead of an undefined conversion.
                std::optional<int> loadOrder;
                if (entry.HasKey("loadOrder"))
                {
                    loadOrder = entry["loadOrder"].TryAsInt();
                    if (!loadOrder)
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Game,
                                       "ModSystem::LoadConfig: mod '%s' loadOrder is not an integer in int range; "
                                       "using 0",
                                       modId.c_str());
                    }
                }
                modIt->second.loadOrder = loadOrder.value_or(0);
                m_modStates[modId] = modIt->second.enabled ? ModState::Available : ModState::Disabled;
            }
        }
        return true;
    }

    bool ModSystem::ParseModJson(const std::string& content, const std::string& path, ModInfo& info)
    {
        Json::Value root;
        std::string parseError;
        if (!Json::ParseBounded(content, MOD_JSON_LIMITS, &root, &parseError))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: mod manifest '%s' rejected: %s", path.c_str(),
                            parseError.c_str());
            return false;
        }

        if (!root.IsObject())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "ModSystem: mod manifest '%s' is not a JSON object",
                            path.c_str());
            return false;
        }

        // Extract string fields
        auto getString = [&](const std::string& key) -> std::string
        {
            if (root.HasKey(key) && root[key].IsString())
            {
                return root[key].AsString();
            }
            return "";
        };

        info.id = getString("id");
        if (!IsValidModId(info.id))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "ModSystem: mod manifest '%s' rejected: id must be 1-%zu characters of [A-Za-z0-9._-] "
                            "and not '.' or '..'",
                            path.c_str(), MAX_MOD_ID_BYTES);
            return false;
        }
        info.name = getString("name");
        info.author = getString("author");
        info.version = getString("version");
        info.description = getString("description");
        info.previewImage = getString("previewImage");

        // Extract dependencies array. A dependency names another mod, so it follows the
        // same id policy; a repeat is recorded once and a mod may not depend on itself.
        if (root.HasKey("dependencies") && root["dependencies"].IsArray())
        {
            const auto& deps = root["dependencies"];
            for (size_t i = 0; i < deps.Size(); ++i)
            {
                if (!deps[i].IsString())
                {
                    continue;
                }
                const std::string& dependency = deps[i].AsString();
                if (!IsValidModId(dependency) || dependency == info.id)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                    "ModSystem: mod manifest '%s' rejected: dependency %zu is not a valid mod id "
                                    "other than the mod's own",
                                    path.c_str(), i);
                    return false;
                }
                if (std::find(info.dependencies.begin(), info.dependencies.end(), dependency) ==
                    info.dependencies.end())
                {
                    info.dependencies.push_back(dependency);
                }
            }
        }

        // Extract load order if present. A manifest that declares one must declare
        // an exact int: a fraction or out-of-range number rejects the manifest.
        if (root.HasKey("loadOrder"))
        {
            const std::optional<int> loadOrder = root["loadOrder"].TryAsInt();
            if (!loadOrder)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                "ModSystem: mod manifest '%s' rejected: loadOrder must be an integer in %d..%d",
                                path.c_str(), std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
                return false;
            }
            info.loadOrder = *loadOrder;
        }

        return true;
    }

    std::string ModSystem::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== Mod System ===\n";
        oss << "Mods directory: " << m_modsDirectory << "\n";
        oss << "Total mods: " << m_mods.size() << "\n";
        size_t active = 0;
        for (const auto& [id, state] : m_modStates)
        {
            if (state == ModState::Active)
            {
                ++active;
            }
        }
        oss << "Active mods: " << active << "\n";
        return oss.str();
    }

    std::string ModSystem::Console_ListMods() const
    {
        std::ostringstream oss;
        oss << "=== Installed Mods ===\n";
        const char* stateNames[] = {"Available", "Loading", "Active", "Error", "Disabled"};
        for (const auto& [id, info] : m_mods)
        {
            auto stateIt = m_modStates.find(id);
            const char* state =
                (stateIt != m_modStates.end()) ? stateNames[static_cast<int>(stateIt->second)] : "Unknown";
            oss << "  " << info.name << " v" << info.version << " by " << info.author << " [" << state << "]\n";
        }
        return oss.str();
    }

} // namespace Spark
