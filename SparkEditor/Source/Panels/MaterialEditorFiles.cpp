/**
 * @file MaterialEditorFiles.cpp
 * @brief Read and write the material editor's .spkmat assets.
 */
#include "MaterialEditorPanel.h"
#include "MaterialEditorMaterialParser.h"
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
        constexpr uintmax_t maximumBytes = kMaxMaterialTextBytes;
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
        MaterialDefinition material;
        material.filePath = materialPath;
        if (!ParseMaterialText(contents, material))
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
                for (float value : param.floatValues)
                {
                    file << " " << value;
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
