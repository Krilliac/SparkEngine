/**
 * @file PluginMetadata.cpp
 * @brief The `<plugin>.sparkplugin.json` gate DynamicPluginHost::Load runs before mapping a plugin
 *
 * Split out of DynamicPluginHost.cpp so the reader links without the plugin lifecycle; the
 * SEC-120 fuzz target (FuzzerTests/FuzzPluginMetadata.cpp) drives ValidatePluginMetadata
 * through real files.
 */

#include "PluginMetadata.h"

#include "FileIntegrity.h"
#include "Utils/JsonUtils.h"

#include <Spark/PluginABI.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>

namespace Spark
{
    namespace
    {
        std::string PathToUtf8(const std::filesystem::path& path)
        {
            const std::u8string utf8 = path.generic_u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }

        std::filesystem::path MetadataPath(const std::filesystem::path& pluginPath)
        {
            std::filesystem::path metadata = pluginPath;
            metadata += ".sparkplugin.json";
            return metadata;
        }

        bool ReadRequiredString(const Json::Value& root, std::string_view name, std::string& value, std::string& error)
        {
            const Json::Value& field = root[std::string(name)];
            if (!field.IsString() || field.AsString().empty())
            {
                error = "missing or invalid string field '" + std::string(name) + "'";
                return false;
            }
            value = field.AsString();
            return true;
        }

        bool ReadRequiredUInt(const Json::Value& root, std::string_view name, uint32_t& value, std::string& error)
        {
            const Json::Value& field = root[std::string(name)];
            if (!field.IsNumber())
            {
                error = "missing or invalid integer field '" + std::string(name) + "'";
                return false;
            }
            const double number = field.AsNumber();
            if (!std::isfinite(number) || number < 0.0 || number > static_cast<double>(UINT32_MAX) ||
                std::floor(number) != number)
            {
                error = "missing or invalid integer field '" + std::string(name) + "'";
                return false;
            }
            value = static_cast<uint32_t>(number);
            return true;
        }
    } // namespace

    bool ValidatePluginMetadata(const std::filesystem::path& pluginPath, PluginMetadata& metadata, std::string& error)
    {
        constexpr uintmax_t kMaximumMetadataBytes = uintmax_t{64} * 1024;
        const std::filesystem::path metadataPath = MetadataPath(pluginPath);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(metadataPath, ec) || ec)
        {
            error = "missing mandatory plugin metadata '" + PathToUtf8(metadataPath) + "'";
            return false;
        }
        const uintmax_t metadataSize = std::filesystem::file_size(metadataPath, ec);
        if (ec || metadataSize == 0 || metadataSize > kMaximumMetadataBytes)
        {
            error = "plugin metadata has an invalid size";
            return false;
        }

        std::ifstream stream(metadataPath, std::ios::binary);
        if (!stream)
        {
            error = "failed to open plugin metadata";
            return false;
        }
        std::ostringstream contents;
        contents << stream.rdbuf();
        if (!stream.eof() && stream.fail())
        {
            error = "failed to read plugin metadata";
            return false;
        }

        Json::Value root;
        std::string parseError;
        const std::string json = contents.str();
        if (!Json::ParseStrict(json, &root, &parseError) || !root.IsObject())
        {
            error = "malformed plugin metadata: " + parseError;
            return false;
        }
        if (root.Size() != 9)
        {
            error = "plugin metadata must contain exactly the schema 1 fields";
            return false;
        }

        uint32_t schema = 0;
        uint32_t abiMajor = 0;
        uint32_t abiMinor = 0;
        std::string type;
        std::string entryPoint;
        std::string binary;
        if (!ReadRequiredUInt(root, "schema", schema, error) || !ReadRequiredString(root, "id", metadata.id, error) ||
            !ReadRequiredString(root, "version", metadata.version, error) ||
            !ReadRequiredString(root, "type", type, error) || !ReadRequiredUInt(root, "abi_major", abiMajor, error) ||
            !ReadRequiredUInt(root, "abi_minor", abiMinor, error) ||
            !ReadRequiredString(root, "entry_point", entryPoint, error) ||
            !ReadRequiredString(root, "binary", binary, error) ||
            !ReadRequiredString(root, "sha256", metadata.expectedHash, error))
        {
            return false;
        }

        if (schema != 1)
        {
            error = "unsupported plugin metadata schema";
            return false;
        }
        if (abiMajor != SPARK_PLUGIN_ABI_MAJOR || abiMinor > SPARK_PLUGIN_ABI_MINOR)
        {
            error = "plugin metadata ABI is incompatible with this host";
            return false;
        }
        if (entryPoint != SPARK_PLUGIN_ENTRY_POINT)
        {
            error = "plugin metadata entry point mismatch";
            return false;
        }
        if (binary != PathToUtf8(pluginPath.filename()))
        {
            error = "plugin metadata binary name mismatch";
            return false;
        }
        if (metadata.expectedHash.size() != 64 ||
            !std::all_of(metadata.expectedHash.begin(), metadata.expectedHash.end(),
                         [](unsigned char c) { return std::isxdigit(c); }))
        {
            error = "plugin metadata has an invalid SHA-256 digest";
            return false;
        }
        std::transform(metadata.expectedHash.begin(), metadata.expectedHash.end(), metadata.expectedHash.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        std::string actualHash;
        std::string hashError;
        if (!FileIntegrity::ComputeSha256(pluginPath, actualHash, hashError))
        {
            error = "failed to hash plugin binary: " + hashError;
            return false;
        }
        if (actualHash != metadata.expectedHash)
        {
            error = "plugin metadata binary SHA-256 mismatch";
            return false;
        }
        return true;
    }
} // namespace Spark
