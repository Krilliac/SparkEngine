/**
 * @file SparkMatParser.cpp
 * @brief .sparkmat text reader, apart from MaterialLoader's registration and logging
 *
 * MaterialLoader::ParseFile opens the file and hands the stream here. Keeping the reader in
 * its own translation unit lets the SEC-120 fuzz target (FuzzerTests/FuzzMaterialLoader.cpp)
 * link the shipped parser without the MaterialSystem, console or graphics device.
 */

#include "MaterialLoader.h"

#include "../Utils/StringUtils.h"

#include <cmath>
#include <istream>
#include <optional>
#include <string>

namespace Spark::Graphics
{
    namespace
    {
        /// Trim leading and trailing whitespace from a string.
        std::string TrimWhitespace(const std::string& str)
        {
            auto start = str.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
            {
                return "";
            }
            auto end = str.find_last_not_of(" \t\r\n");
            return str.substr(start, end - start + 1);
        }

        /// Assign a whole-token finite float, keeping @p current when the value does not parse.
        /// std::stof accepts "nan" and "inf"; RegisterMaterial's std::clamp passes NaN through
        /// and does not clamp normalScale at all, so a non-finite factor reached the GPU.
        float ParseFactor(const std::string& value, float current)
        {
            const std::optional<float> parsed = Spark::StringUtils::ParseFloat(value);
            if (!parsed || !std::isfinite(*parsed))
            {
                return current;
            }
            return *parsed;
        }
    } // namespace

    bool ParseSparkMatDefinition(std::istream& input, SparkMatDefinition& outDef)
    {
        outDef = {};
        std::string line;
        while (std::getline(input, line))
        {
            // Skip empty lines and comments
            std::string trimmed = TrimWhitespace(line);
            if (trimmed.empty() || trimmed.starts_with("//"))
            {
                continue;
            }

            // Split on first '='
            auto eqPos = trimmed.find('=');
            if (eqPos == std::string::npos)
            {
                continue;
            }

            std::string key = TrimWhitespace(trimmed.substr(0, eqPos));
            std::string value = TrimWhitespace(trimmed.substr(eqPos + 1));

            if (key == "name")
            {
                outDef.name = value;
            }
            else if (key == "blendMode")
            {
                outDef.blendMode = value;
            }
            else if (key == "metallic")
            {
                outDef.metallic = ParseFactor(value, outDef.metallic);
            }
            else if (key == "roughness")
            {
                outDef.roughness = ParseFactor(value, outDef.roughness);
            }
            else if (key == "normalScale")
            {
                outDef.normalScale = ParseFactor(value, outDef.normalScale);
            }
            else if (key == "occlusionStrength")
            {
                outDef.occlusionStrength = ParseFactor(value, outDef.occlusionStrength);
            }
            else if (key == "emissiveFactor")
            {
                outDef.emissiveFactor = ParseFactor(value, outDef.emissiveFactor);
            }
            else if (key == "alphaCutoff")
            {
                outDef.alphaCutoff = ParseFactor(value, outDef.alphaCutoff);
            }
            else if (key == "albedoTexture")
            {
                outDef.albedoTexture = value;
            }
            else if (key == "normalTexture")
            {
                outDef.normalTexture = value;
            }
            else if (key == "metallicTexture")
            {
                outDef.metallicTexture = value;
            }
            else if (key == "roughnessTexture")
            {
                outDef.roughnessTexture = value;
            }
            else if (key == "emissiveTexture")
            {
                outDef.emissiveTexture = value;
            }
            else if (key == "occlusionTexture")
            {
                outDef.occlusionTexture = value;
            }
        }

        return !outDef.name.empty();
    }

} // namespace Spark::Graphics
