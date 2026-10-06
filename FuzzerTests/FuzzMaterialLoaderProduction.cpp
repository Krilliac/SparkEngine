/**
 * @file FuzzMaterialLoaderProduction.cpp
 * @brief libc++-compiled production adapter for the .sparkmat material libFuzzer harness.
 *
 * MaterialLoader::LoadMaterial opens every .sparkmat file (shipped content and mods alike)
 * and hands the stream to Spark::Graphics::ParseSparkMatDefinition; RegisterMaterial then
 * clamps the factors into the material's PBR constants. The adapter wraps the fuzz bytes in
 * the same kind of stream and calls the shipped reader. A violated contract aborts so
 * libFuzzer records a crash:
 *  - the result is true exactly when the definition names a material,
 *  - no accepted string field holds a line break or keeps leading or trailing whitespace
 *    (the reader trims both, so a survivor means a line was split or joined),
 *  - every accepted numeric factor is finite (std::clamp in RegisterMaterial passes NaN
 *    through to the GPU constant buffers),
 *  - an accepted definition written back as key = value lines parses to the identical
 *    definition, so the reader has one meaning for what it accepts,
 *  - parsing the same bytes twice gives the same definition.
 */

#include "FuzzMaterialLoaderProduction.h"

#include "Graphics/MaterialLoader.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzMaterialLoader: ParseSparkMatDefinition violated: %s\n", what);
        std::abort();
    }

    using Definition = Spark::Graphics::SparkMatDefinition;

    std::array<const std::string*, 8> Strings(const Definition& def)
    {
        return {&def.name,
                &def.blendMode,
                &def.albedoTexture,
                &def.normalTexture,
                &def.metallicTexture,
                &def.roughnessTexture,
                &def.emissiveTexture,
                &def.occlusionTexture};
    }

    std::array<float, 6> Factors(const Definition& def)
    {
        return {def.metallic,          def.roughness,      def.normalScale,
                def.occlusionStrength, def.emissiveFactor, def.alphaCutoff};
    }

    bool SameDefinition(const Definition& a, const Definition& b)
    {
        const auto stringsA = Strings(a);
        const auto stringsB = Strings(b);
        for (std::size_t i = 0; i < stringsA.size(); ++i)
        {
            if (*stringsA[i] != *stringsB[i])
                return false;
        }
        const auto factorsA = Factors(a);
        const auto factorsB = Factors(b);
        return std::memcmp(factorsA.data(), factorsB.data(), sizeof(float) * factorsA.size()) == 0;
    }

    void CheckString(const std::string& value)
    {
        if (value.find('\n') != std::string::npos)
            InvariantFailure("a field holds a line feed");
        constexpr std::string_view kWhitespace = " \t\r\n";
        if (!value.empty() && (kWhitespace.find(value.front()) != std::string_view::npos ||
                               kWhitespace.find(value.back()) != std::string_view::npos))
            InvariantFailure("a field keeps leading or trailing whitespace");
    }

    std::string FormatFactor(float value)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
        return buffer;
    }

    std::string Write(const Definition& def)
    {
        std::string text;
        const auto append = [&text](const char* key, const std::string& value)
        { text.append(key).append(" = ").append(value).append("\n"); };
        append("name", def.name);
        append("blendMode", def.blendMode);
        append("metallic", FormatFactor(def.metallic));
        append("roughness", FormatFactor(def.roughness));
        append("normalScale", FormatFactor(def.normalScale));
        append("occlusionStrength", FormatFactor(def.occlusionStrength));
        append("emissiveFactor", FormatFactor(def.emissiveFactor));
        append("alphaCutoff", FormatFactor(def.alphaCutoff));
        append("albedoTexture", def.albedoTexture);
        append("normalTexture", def.normalTexture);
        append("metallicTexture", def.metallicTexture);
        append("roughnessTexture", def.roughnessTexture);
        append("emissiveTexture", def.emissiveTexture);
        append("occlusionTexture", def.occlusionTexture);
        return text;
    }

    bool Parse(const std::string& text, Definition& def)
    {
        std::istringstream input(text);
        return Spark::Graphics::ParseSparkMatDefinition(input, def);
    }
} // namespace

extern "C" int SparkFuzzParseSparkMat(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
        return 0;
    const std::string text = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);

    Definition def;
    def.name = "sentinel";
    const bool accepted = Parse(text, def);
    if (accepted != !def.name.empty())
        InvariantFailure("the result disagrees with whether the definition names a material");

    Definition again;
    if (Parse(text, again) != accepted || !SameDefinition(def, again))
        InvariantFailure("parsing the same bytes twice gave different definitions");
    if (!accepted)
        return 0;

    for (const std::string* value : Strings(def))
        CheckString(*value);
    for (const float factor : Factors(def))
    {
        if (!std::isfinite(factor))
            InvariantFailure("an accepted factor is not finite");
    }

    Definition reloaded;
    if (!Parse(Write(def), reloaded))
        InvariantFailure("a written definition no longer names a material");
    if (!SameDefinition(def, reloaded))
        InvariantFailure("write -> parse changed the definition");
    return 0;
}
