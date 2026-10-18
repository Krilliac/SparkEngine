/**
 * @file FuzzPluginMetadataProduction.cpp
 * @brief libc++-compiled production adapter for the plugin metadata libFuzzer harness.
 *
 * DynamicPluginHost::Load calls Spark::ValidatePluginMetadata on `<plugin>.sparkplugin.json`
 * before the OS loader maps the plugin, and later re-checks the declared id/version and
 * SHA-256 against the mapped image. The adapter keeps a fixed plugin image in a private
 * directory and writes the fuzz bytes as its metadata file, so the gate reads, parses and
 * hashes real files. A violated contract aborts so libFuzzer records a crash:
 *  - the gate accepts the adapter's own correct metadata (a gate that rejects everything
 *    would otherwise pass every other check),
 *  - a rejection always carries a reason, and the verdict is the same on a second call,
 *  - inputs outside 1..65536 bytes are rejected,
 *  - an accepted document names a non-empty id and version and declares the plugin
 *    image's own SHA-256, in lower case.
 */

#include "FuzzPluginMetadataProduction.h"

#include "Core/PluginMetadata.h"

#include <Spark/PluginABI.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace
{
    constexpr std::size_t kMaxInputBytes = 70000;
    constexpr std::size_t kMaxMetadataBytes = 64u * 1024u;

    // The plugin image the metadata describes, and its SHA-256 (generate_plugin_metadata_corpus.py
    // writes the same digest into the seeds).
    constexpr std::string_view kPluginImage = "SparkFuzzPluginMetadata image: not a loadable plugin.\n";
    constexpr std::string_view kPluginImageSha256 = "30733e1a836a3d81bf4b097e4568a78d9f25caea08854af251d20905ed41c68f";
    constexpr std::string_view kPluginFileName = "fuzz-plugin.so";

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzPluginMetadata: ValidatePluginMetadata violated: %s\n", what);
        std::abort();
    }

    void WriteFile(const std::filesystem::path& path, std::string_view bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            InvariantFailure("could not write a fixture file");
    }

    std::string CorrectMetadata()
    {
        return std::string(R"({"schema":1,"id":"spark.fuzz.plugin","version":"1.0.0","type":"gameplay",)") +
               "\"abi_major\":" + std::to_string(SPARK_PLUGIN_ABI_MAJOR) +
               ",\"abi_minor\":" + std::to_string(SPARK_PLUGIN_ABI_MINOR) + ",\"entry_point\":\"" +
               SPARK_PLUGIN_ENTRY_POINT + "\",\"binary\":\"" + std::string(kPluginFileName) + "\",\"sha256\":\"" +
               std::string(kPluginImageSha256) + "\"}";
    }

    struct Fixture
    {
        std::filesystem::path plugin;
        std::filesystem::path metadata;
    };

    const Fixture& GetFixture()
    {
        static const Fixture fixture = []
        {
            std::string pattern = (std::filesystem::temp_directory_path() / "spark-fuzz-plugin-XXXXXX").string();
            if (::mkdtemp(pattern.data()) == nullptr)
                InvariantFailure("could not create the fixture directory");
            const std::filesystem::path plugin = std::filesystem::path(pattern) / std::string(kPluginFileName);
            WriteFile(plugin, kPluginImage);
            std::filesystem::path metadata = plugin;
            metadata += ".sparkplugin.json";
            WriteFile(metadata, CorrectMetadata());
            Spark::PluginMetadata parsed;
            std::string error;
            if (!Spark::ValidatePluginMetadata(plugin, parsed, error))
            {
                std::fprintf(stderr, "SparkFuzzPluginMetadata: correct metadata rejected: %s\n", error.c_str());
                InvariantFailure("the gate rejects correct metadata");
            }
            return Fixture{plugin, metadata};
        }();
        return fixture;
    }
} // namespace

extern "C" int SparkFuzzValidatePluginMetadata(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
        return 0;
    const Fixture& fixture = GetFixture();
    WriteFile(fixture.metadata,
              size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size));

    Spark::PluginMetadata metadata;
    std::string error;
    const bool accepted = Spark::ValidatePluginMetadata(fixture.plugin, metadata, error);
    Spark::PluginMetadata second;
    std::string secondError;
    if (Spark::ValidatePluginMetadata(fixture.plugin, second, secondError) != accepted || secondError != error)
        InvariantFailure("the verdict changed on a second call");
    if (!accepted)
    {
        if (error.empty())
            InvariantFailure("rejected metadata carries no reason");
        return 0;
    }

    if (size == 0 || size > kMaxMetadataBytes)
        InvariantFailure("metadata outside 1..65536 bytes was accepted");
    if (metadata.id.empty() || metadata.version.empty())
        InvariantFailure("accepted metadata has an empty id or version");
    if (metadata.expectedHash != kPluginImageSha256)
        InvariantFailure("accepted metadata does not declare the plugin image's lower-case SHA-256");
    if (second.id != metadata.id || second.version != metadata.version)
        InvariantFailure("the declared identity changed on a second call");
    return 0;
}
