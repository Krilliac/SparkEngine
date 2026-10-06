/**
 * @file MaterialEditorFiles.cpp
 * @brief Read and write the material editor's .spkmat assets.
 */
#include "MaterialEditorPanel.h"
#include "Utils/FileUtils.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <set>
#include <sstream>
#include <string_view>

namespace SparkEditor
{
    namespace
    {
        void Trim(std::string& text)
        {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                text.clear();
            else
                text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        }

        template <typename... Values> bool ReadValues(std::istringstream& input, Values&... values)
        {
            if (!(input >> ... >> values))
                return false;
            input >> std::ws;
            return input.eof();
        }

        bool ReadBool(std::istringstream& input, bool& value)
        {
            std::string token;
            if (!ReadValues(input, token) || (token != "true" && token != "false"))
                return false;
            value = token == "true";
            return true;
        }

        bool ReadParameter(std::istringstream& input, MaterialDefinition& material, std::set<std::string>& seen)
        {
            std::string name, type;
            if (!(input >> name >> type) || !seen.insert(name).second)
                return false;
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
                return false;
            ShaderParameter* existing = material.FindParameter(name);
            ShaderParameter parameter = existing ? *existing : ShaderParameter{};
            parameter.name = name;
            if (parameter.displayName.empty())
                parameter.displayName = name;
            if (parameter.group.empty())
                parameter.group = "Parameters";
            parameter.type = found->second;
            if (parameter.type == ShaderParamType::Int)
            {
                if (!ReadValues(input, parameter.intValue))
                    return false;
            }
            else if (parameter.type == ShaderParamType::Bool)
            {
                if (!ReadBool(input, parameter.boolValue))
                    return false;
            }
            else if (parameter.type == ShaderParamType::Texture2D || parameter.type == ShaderParamType::TextureCube)
            {
                std::getline(input, parameter.texturePath);
                Trim(parameter.texturePath);
                if (parameter.texturePath.empty())
                    return false;
                if (parameter.texturePath == "none")
                    parameter.texturePath.clear();
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
                        return false;
                }
                input >> std::ws;
                if (!input.eof())
                    return false;
            }
            if (existing)
                *existing = std::move(parameter);
            else
                material.parameters.push_back(std::move(parameter));
            return true;
        }

        bool ReadTextureSlot(std::istringstream& input, MaterialDefinition& material, std::set<std::string>& seen)
        {
            std::string name, tail;
            int binding = 0;
            if (!(input >> name >> binding) || binding < 0 || !seen.insert(name).second)
                return false;
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
                    return false;
                std::istringstream value(tail.substr(separator + 1));
                value.imbue(std::locale::classic());
                if (!ReadValues(value, coordinates[remaining - 1]) || !std::isfinite(coordinates[remaining - 1]))
                    return false;
                tail.resize(separator);
                Trim(tail);
            }
            if (tail.empty())
                return false;
            slot.texturePath = tail == "none" ? "" : tail;
            slot.isAssigned = !slot.texturePath.empty();
            slot.tilingU = coordinates[0];
            slot.tilingV = coordinates[1];
            slot.offsetU = coordinates[2];
            slot.offsetV = coordinates[3];
            if (existing)
                *existing = std::move(slot);
            else
                material.textureSlots.push_back(std::move(slot));
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
                    continue;
                std::istringstream input(line);
                input.imbue(std::locale::classic());
                std::string key;
                input >> key;
                if (key == "param")
                {
                    if (!ReadParameter(input, material, parameters))
                        return false;
                    continue;
                }
                if (key == "texture_slot")
                {
                    if (!ReadTextureSlot(input, material, textures))
                        return false;
                    continue;
                }
                if (!fields.insert(key).second)
                    return false;
                if (key == "name:" || key == "shader:")
                {
                    auto& text = key == "name:" ? material.name : material.shaderPath;
                    std::getline(input, text);
                    Trim(text);
                    if (text.empty())
                        return false;
                    (key == "name:" ? hasName : hasShader) = true;
                }
                else if (key == "blend_mode" || key == "cull_mode")
                {
                    int value = 0;
                    if (!ReadValues(input, value) || value < 0 || value > (key == "blend_mode" ? 4 : 2))
                        return false;
                    if (key == "blend_mode")
                        material.renderState.blendMode = static_cast<MaterialRenderState::BlendMode>(value);
                    else
                        material.renderState.cullMode = static_cast<MaterialRenderState::CullMode>(value);
                }
                else if (key == "depth_write" || key == "depth_test" || key == "cast_shadows" ||
                         key == "receive_shadows")
                {
                    bool value = false;
                    if (!ReadBool(input, value))
                        return false;
                    if (key == "depth_write")
                        material.renderState.depthWrite = value;
                    else if (key == "depth_test")
                        material.renderState.depthTest = value;
                    else if (key == "cast_shadows")
                        material.renderState.castShadows = value;
                    else
                        material.renderState.receiveShadows = value;
                }
                else if (key == "alpha_clip")
                {
                    if (!ReadValues(input, material.renderState.alphaClipThreshold) ||
                        !std::isfinite(material.renderState.alphaClipThreshold))
                        return false;
                }
                else if (key == "render_queue")
                {
                    if (!ReadValues(input, material.renderState.renderQueue))
                        return false;
                }
                else
                    return false;
            }
            return file.eof() && hasName && hasShader;
        }
    } // namespace

    void MaterialEditorPanel::OpenMaterial(const std::string& materialPath)
    {
        for (int index = 0; index < static_cast<int>(m_materials.size()); ++index)
        {
            if (m_materials[index].filePath == materialPath)
            {
                m_selectedMaterialIndex = index;
                SetVisible(true);
                return;
            }
        }
        const auto path = Spark::FileUtils::PathFromUtf8(materialPath);
        std::error_code error;
        constexpr uintmax_t maximumBytes = 16 * 1024 * 1024;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > maximumBytes)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Material Editor: cannot read material '%s'",
                            materialPath.c_str());
            return;
        }
        std::ifstream file(path, std::ios::binary);
        std::string contents(static_cast<size_t>(size), '\0');
        if (!file.is_open() || !file.read(contents.data(), static_cast<std::streamsize>(contents.size())) ||
            file.peek() != std::char_traits<char>::eof() || contents.find('\0') != std::string::npos)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Material Editor: cannot read material '%s'",
                            materialPath.c_str());
            return;
        }
        std::istringstream input(contents);
        input.imbue(std::locale::classic());
        MaterialDefinition material;
        material.filePath = materialPath;
        PopulateDefaultPBRParameters(material);
        if (!ReadMaterial(input, material))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Material Editor: invalid material '%s'", materialPath.c_str());
            return;
        }
        m_materials.push_back(std::move(material));
        m_selectedMaterialIndex = static_cast<int>(m_materials.size()) - 1;
        SetVisible(true);
        NotifyStateChange();
    }

    void MaterialEditorPanel::CreateMaterial(const std::string& name, const std::string& shaderPath)
    {
        MaterialDefinition material;
        material.name = name;
        material.shaderPath = shaderPath;
        material.filePath = "Assets/Materials/" + name + ".spkmat";
        material.isModified = true;
        material.isBuiltIn = false;

        PopulateDefaultPBRParameters(material);

        m_materials.push_back(std::move(material));
        m_selectedMaterialIndex = static_cast<int>(m_materials.size()) - 1;
        SetModified(true);

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Material Editor: created material '%s' (shader='%s')", name.c_str(),
                       shaderPath.c_str());
    }

    bool MaterialEditorPanel::SaveMaterial()
    {
        MaterialDefinition* selected = GetSelectedMaterial();
        if (selected == nullptr)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Material Editor: no material selected to save");
            return false;
        }

        if (selected->isBuiltIn)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Material Editor: cannot save built-in material '%s'",
                           selected->name.c_str());
            return false;
        }

        const auto materialPath = Spark::FileUtils::PathFromUtf8(selected->filePath);
        if (materialPath.empty())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Material Editor: cannot save to an empty file path");
            return false;
        }
        std::error_code directoryError;
        if (!materialPath.parent_path().empty())
        {
            std::filesystem::create_directories(materialPath.parent_path(), directoryError);
        }
        if (directoryError)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Material Editor: cannot create parent directory for '%s': %s",
                            selected->filePath.c_str(), directoryError.message().c_str());
            return false;
        }

        // Serialize the existing .spkmat format through its native path on Windows.
        std::ofstream file(materialPath);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Material Editor: failed to open file '%s' for writing",
                            selected->filePath.c_str());
            return false;
        }

        file << "# SparkEngine Material\n";
        file << "name: " << selected->name << "\n";
        file << "shader: " << selected->shaderPath << "\n";
        file << "\n";

        // Write parameters
        file << "# Parameters\n";
        for (const auto& param : selected->parameters)
        {
            file << "param " << param.name << " ";
            switch (param.type)
            {
            case ShaderParamType::Float:
                file << "float " << param.floatValues[0] << "\n";
                break;
            case ShaderParamType::Float2:
                file << "float2 " << param.floatValues[0] << " " << param.floatValues[1] << "\n";
                break;
            case ShaderParamType::Float3:
                file << "float3 " << param.floatValues[0] << " " << param.floatValues[1] << " " << param.floatValues[2]
                     << "\n";
                break;
            case ShaderParamType::Float4:
                file << "float4 " << param.floatValues[0] << " " << param.floatValues[1] << " " << param.floatValues[2]
                     << " " << param.floatValues[3] << "\n";
                break;
            case ShaderParamType::Color:
                file << "color " << param.floatValues[0] << " " << param.floatValues[1] << " " << param.floatValues[2]
                     << " " << param.floatValues[3] << "\n";
                break;
            case ShaderParamType::Int:
                file << "int " << param.intValue << "\n";
                break;
            case ShaderParamType::Bool:
                file << "bool " << (param.boolValue ? "true" : "false") << "\n";
                break;
            case ShaderParamType::Texture2D:
                file << "texture2d " << (param.texturePath.empty() ? "none" : param.texturePath) << "\n";
                break;
            case ShaderParamType::TextureCube:
                file << "texturecube " << (param.texturePath.empty() ? "none" : param.texturePath) << "\n";
                break;
            case ShaderParamType::Matrix4x4:
                file << "matrix4x4";
                for (int i = 0; i < 16; ++i)
                {
                    file << " " << param.floatValues[i];
                }
                file << "\n";
                break;
            }
        }
        file << "\n";

        // Write texture slots
        file << "# Texture Slots\n";
        for (const auto& slot : selected->textureSlots)
        {
            file << "texture_slot " << slot.name << " " << slot.bindSlot << " "
                 << (slot.texturePath.empty() ? "none" : slot.texturePath) << " " << slot.tilingU << " " << slot.tilingV
                 << " " << slot.offsetU << " " << slot.offsetV << "\n";
        }
        file << "\n";

        // Write render state
        file << "# Render State\n";
        file << "blend_mode " << static_cast<int>(selected->renderState.blendMode) << "\n";
        file << "cull_mode " << static_cast<int>(selected->renderState.cullMode) << "\n";
        file << "depth_write " << (selected->renderState.depthWrite ? "true" : "false") << "\n";
        file << "depth_test " << (selected->renderState.depthTest ? "true" : "false") << "\n";
        file << "cast_shadows " << (selected->renderState.castShadows ? "true" : "false") << "\n";
        file << "receive_shadows " << (selected->renderState.receiveShadows ? "true" : "false") << "\n";
        file << "alpha_clip " << selected->renderState.alphaClipThreshold << "\n";
        file << "render_queue " << selected->renderState.renderQueue << "\n";

        file.flush();
        if (!file.good())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Material Editor: failed to write material '%s'",
                            selected->filePath.c_str());
            return false;
        }
        file.close();
        if (file.fail())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Material Editor: failed to close material '%s'",
                            selected->filePath.c_str());
            return false;
        }

        selected->isModified = false;
        SetModified(false);
        NotifyStateChange();

        std::cout << "Material Editor: saved material '" << selected->name << "' to '" << selected->filePath << "'\n";
        return true;
    }

} // namespace SparkEditor
