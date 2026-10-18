#pragma once

/**
 * @file PluginMetadata.h
 * @brief The mandatory `<plugin>.sparkplugin.json` gate run before a dynamic plugin is mapped
 *
 * DynamicPluginHost::Load refuses a plugin unless its metadata sidecar is schema-1 JSON that
 * names this host's ABI, the standard entry point, the plugin's own file name and the SHA-256
 * of its image. The check runs before the OS loader maps the image.
 *
 * Thread affinity: none; the function touches only the files it is given.
 * Ownership: no state is kept between calls.
 * Allocation: the metadata read is capped at 64 KiB; hashing streams the image.
 */

#include <filesystem>
#include <string>

namespace Spark
{
    /// Identity a plugin's metadata declares; DynamicPluginHost re-checks it against the image.
    struct PluginMetadata
    {
        std::string id;
        std::string version;
        std::string expectedHash; ///< Lower-case hex SHA-256 of the plugin image.
    };

    /**
     * @brief Validate `<pluginPath>.sparkplugin.json` against this host and the plugin image.
     *
     * The metadata must be 1..65536 bytes of strict JSON holding exactly the nine schema-1
     * fields: schema (1), id, version, type, abi_major/abi_minor (this host's major, minor
     * not above this host's), entry_point (SPARK_PLUGIN_ENTRY_POINT), binary (the file name
     * of @p pluginPath) and sha256 (64 hex digits equal to the SHA-256 of the image).
     *
     * @param pluginPath Plugin image whose metadata is read; the image itself is hashed.
     * @param metadata   Receives id, version and the lower-cased hash.
     * @param error      Receives the reason on failure.
     * @return true when the plugin may be handed to the OS loader.
     */
    [[nodiscard]] bool ValidatePluginMetadata(const std::filesystem::path& pluginPath, PluginMetadata& metadata,
                                              std::string& error);
} // namespace Spark
