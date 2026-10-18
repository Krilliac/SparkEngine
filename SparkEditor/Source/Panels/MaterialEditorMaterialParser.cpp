/**
 * @file MaterialEditorMaterialParser.cpp
 * @brief UI-free, bounded decoder for the editor's legacy .spkmat format.
 */
#include "MaterialEditorMaterialParser.h"
#include "MaterialEditorPanel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <locale>
#include <set>
#include <sstream>
#include <utility>
namespace SparkEditor
{
    namespace
    {
        void Trim(std::string& text)
        {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                text.clear();
            }
            else
            {
                text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
            }
        }

        template <typename... Values> bool ReadValues(std::istringstream& input, Values&... values)
        {
            if (!(input >> ... >> values))
            {
                return false;
            }
            input >> std::ws;
            return input.eof();
        }

        bool ReadBool(std::istringstream& input, bool& value)
        {
            std::string token;
            if (!ReadValues(input, token) || (token != "true" && token != "false"))
            {
                return false;
            }
            value = token == "true";
            return true;
        }

        bool ReadParameter(std::istringstream& input, MaterialDefinition& material, std::set<std::string>& seen)
        {
            std::string name, type;
            if (!(input >> name >> type) || !seen.insert(name).second)
            {
                return false;
            }
            static constexpr std::array<std::pair<std::string_view, ShaderParamType>, 10> types = {
                {{"float", ShaderParamType::Float},
                 {"float2", ShaderParamType::Float2},
                 {"float3", ShaderParamType::Float3},
                 {"float4", ShaderParamType::Float4},
                 {"int", ShaderParamType::Int},
                 {"bool", ShaderParamType::Bool},
                 {"texture2d", ShaderParamType::Texture2D},
                 {"texturecube", ShaderParamType::TextureCube},
                 {"color", ShaderParamType::Color},
                 {"matrix4x4", ShaderParamType::Matrix4x4}}};
            const auto found =
                std::find_if(types.begin(), types.end(), [&](const auto& entry) { return entry.first == type; });
            if (found == types.end())
            {
                return false;
            }
            ShaderParameter* existing = material.FindParameter(name);
            ShaderParameter parameter = existing ? *existing : ShaderParameter{};
            parameter.name = name;
            if (parameter.displayName.empty())
            {
                parameter.displayName = name;
            }
            if (parameter.group.empty())
            {
                parameter.group = "Parameters";
            }
            parameter.type = found->second;
            if (parameter.type == ShaderParamType::Int)
            {
                if (!ReadValues(input, parameter.intValue))
                {
                    return false;
                }
            }
            else if (parameter.type == ShaderParamType::Bool)
            {
                if (!ReadBool(input, parameter.boolValue))
                {
                    return false;
                }
            }
            else if (parameter.type == ShaderParamType::Texture2D || parameter.type == ShaderParamType::TextureCube)
            {
                std::getline(input, parameter.texturePath);
                Trim(parameter.texturePath);
                if (parameter.texturePath.empty())
                {
                    return false;
                }
                if (parameter.texturePath == "none")
                {
                    parameter.texturePath.clear();
                }
            }
            else
            {
                const size_t count = parameter.type == ShaderParamType::Float       ? 1
                                     : parameter.type == ShaderParamType::Float2    ? 2
                                     : parameter.type == ShaderParamType::Float3    ? 3
                                     : parameter.type == ShaderParamType::Matrix4x4 ? 16
                                                                                    : 4;
                for (size_t index = 0; index < count; ++index)
                {
                    if (!(input >> parameter.floatValues[index]) || !std::isfinite(parameter.floatValues[index]))
                    {
                        return false;
                    }
                }
                input >> std::ws;
                if (!input.eof())
                {
                    return false;
                }
            }
            if (existing)
            {
                *existing = std::move(parameter);
            }
            else
            {
                material.parameters.push_back(std::move(parameter));
            }
            return true;
        }

        bool ReadTextureSlot(std::istringstream& input, MaterialDefinition& material, std::set<std::string>& seen)
        {
            std::string name, tail;
            std::string record;
            std::getline(input, record);
            Trim(record);
            // The legacy writer emits default names such as "Ambient Occlusion"
            // without quotes. Prefer the longest known name before its binding.
            for (const auto& candidate : material.textureSlots)
            {
                const size_t length = candidate.name.size();
                if (length > name.size() && record.size() > length && record.starts_with(candidate.name) &&
                    (record[length] == ' ' || record[length] == '\t'))
                {
                    name = candidate.name;
                }
            }
            if (!name.empty())
            {
                record.erase(0, name.size());
            }
            input.clear();
            input.str(record);
            if (name.empty() && !(input >> name))
            {
                return false;
            }
            int binding = 0;
            if (!(input >> binding) || binding < 0 || !seen.insert(name).second)
            {
                return false;
            }
            TextureSlot* existing = material.FindTextureSlot(name);
            TextureSlot slot = existing ? *existing : TextureSlot{};
            slot.name = name;
            slot.bindSlot = binding;
            std::getline(input, tail);
            Trim(tail);
            std::array<float, 4> coordinates{};
            for (size_t remaining = coordinates.size(); remaining > 0; --remaining)
            {
                const auto separator = tail.find_last_of(" \t");
                if (separator == std::string::npos)
                {
                    return false;
                }
                std::istringstream value(tail.substr(separator + 1));
                value.imbue(std::locale::classic());
                if (!ReadValues(value, coordinates[remaining - 1]) || !std::isfinite(coordinates[remaining - 1]))
                {
                    return false;
                }
                tail.resize(separator);
                Trim(tail);
            }
            if (tail.empty())
            {
                return false;
            }
            slot.texturePath = tail == "none" ? "" : tail;
            slot.isAssigned = !slot.texturePath.empty();
            slot.tilingU = coordinates[0];
            slot.tilingV = coordinates[1];
            slot.offsetU = coordinates[2];
            slot.offsetV = coordinates[3];
            if (existing)
            {
                *existing = std::move(slot);
            }
            else
            {
                material.textureSlots.push_back(std::move(slot));
            }
            return true;
        }

        bool ReadMaterial(std::istream& file, MaterialDefinition& material)
        {
            bool hasName = false, hasShader = false;
            std::set<std::string> parameters, textures, fields;
            std::string line;
            while (std::getline(file, line))
            {
                Trim(line);
                if (line.empty() || line.front() == '#')
                {
                    continue;
                }
                std::istringstream input(line);
                input.imbue(std::locale::classic());
                std::string key;
                input >> key;
                if (key == "param")
                {
                    if (!ReadParameter(input, material, parameters))
                    {
                        return false;
                    }
                    continue;
                }
                if (key == "texture_slot")
                {
                    if (!ReadTextureSlot(input, material, textures))
                    {
                        return false;
                    }
                    continue;
                }
                if (!fields.insert(key).second)
                {
                    return false;
                }
                if (key == "name:" || key == "shader:")
                {
                    auto& text = key == "name:" ? material.name : material.shaderPath;
                    std::getline(input, text);
                    Trim(text);
                    if (text.empty())
                    {
                        return false;
                    }
                    (key == "name:" ? hasName : hasShader) = true;
                }
                else if (key == "blend_mode" || key == "cull_mode")
                {
                    int value = 0;
                    if (!ReadValues(input, value) || value < 0 || value > (key == "blend_mode" ? 4 : 2))
                    {
                        return false;
                    }
                    if (key == "blend_mode")
                    {
                        material.renderState.blendMode = static_cast<MaterialRenderState::BlendMode>(value);
                    }
                    else
                    {
                        material.renderState.cullMode = static_cast<MaterialRenderState::CullMode>(value);
                    }
                }
                else if (key == "depth_write" || key == "depth_test" || key == "cast_shadows" ||
                         key == "receive_shadows")
                {
                    bool value = false;
                    if (!ReadBool(input, value))
                    {
                        return false;
                    }
                    if (key == "depth_write")
                    {
                        material.renderState.depthWrite = value;
                    }
                    else if (key == "depth_test")
                    {
                        material.renderState.depthTest = value;
                    }
                    else if (key == "cast_shadows")
                    {
                        material.renderState.castShadows = value;
                    }
                    else
                    {
                        material.renderState.receiveShadows = value;
                    }
                }
                else if (key == "alpha_clip")
                {
                    if (!ReadValues(input, material.renderState.alphaClipThreshold) ||
                        !std::isfinite(material.renderState.alphaClipThreshold))
                    {
                        return false;
                    }
                }
                else if (key == "render_queue")
                {
                    if (!ReadValues(input, material.renderState.renderQueue))
                    {
                        return false;
                    }
                }
                else
                {
                    return false;
                }
            }
            return file.eof() && hasName && hasShader;
        }
    } // namespace

    bool ParseMaterialText(std::string_view text, MaterialDefinition& material)
    {
        if (text.size() > kMaxMaterialTextBytes || text.find('\0') != std::string_view::npos)
        {
            return false;
        }
        MaterialDefinition parsed;
        parsed.filePath = material.filePath;
        InitializeStandardPBRParameters(parsed);
        std::istringstream input{std::string(text)};
        input.imbue(std::locale::classic());
        if (!ReadMaterial(input, parsed))
        {
            return false;
        }
        material = std::move(parsed);
        return true;
    }
} // namespace SparkEditor
