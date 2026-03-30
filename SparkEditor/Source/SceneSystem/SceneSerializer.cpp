/**
 * @file SceneSerializer.cpp
 * @brief Scene serialization coordinator and format utilities
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains the public API (SaveScene, LoadScene), format detection,
 * validation, version compatibility, and backup helpers.
 * Binary I/O lives in BinarySceneSerializer.cpp; JSON I/O lives in
 * JSONSceneSerializer.cpp.
 */

#include "SceneSerializer.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <fstream>
#include <chrono>
#include <cstring>
#include <filesystem>

using namespace DirectX;
namespace SparkEditor
{

    SceneSerializer::SceneSerializer() = default;
    SceneSerializer::~SceneSerializer() = default;

    SerializationResult SceneSerializer::SaveScene(const SceneFile& scene, const std::string& filePath,
                                                   SerializationFormat format)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        SPARK_VALIDATE_RET(Spark::LogCategory::Scene, !filePath.empty(), SerializationResult{});
        auto startTime = std::chrono::high_resolution_clock::now();

        SerializationFormat actualFormat = format;
        if (actualFormat == SerializationFormat::AUTO)
        {
            actualFormat = DetectFormat(filePath);
            if (actualFormat == SerializationFormat::AUTO)
            {
                actualFormat = SerializationFormat::BINARY;
            }
        }
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Saving scene to '%s' (format=%s)", filePath.c_str(),
                       actualFormat == SerializationFormat::BINARY ? "binary" : "json");

        SerializationResult result;
        if (m_createBackups)
        {
            CreateBackup(filePath);
        }

        if (actualFormat == SerializationFormat::BINARY)
        {
            result = SaveBinary(scene, filePath);
        }
        else
        {
            result = SaveJSON(scene, filePath);
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        result.processingTime = std::chrono::duration<float>(endTime - startTime).count();
        m_totalProcessingTime += result.processingTime;
        return result;
    }

    SerializationResult SceneSerializer::LoadScene(const std::string& filePath, SceneFile& outScene)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        SPARK_VALIDATE_RET(Spark::LogCategory::Scene, !filePath.empty(), SerializationResult{});
        auto startTime = std::chrono::high_resolution_clock::now();

        SerializationFormat format = DetectFormat(filePath);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading scene from '%s' (detected format=%s)", filePath.c_str(),
                       format == SerializationFormat::JSON ? "json" : "binary");
        SerializationResult result;

        if (format == SerializationFormat::JSON)
        {
            result = LoadJSON(filePath, outScene);
        }
        else
        {
            result = LoadBinary(filePath, outScene);
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        result.processingTime = std::chrono::duration<float>(endTime - startTime).count();
        m_totalProcessingTime += result.processingTime;
        return result;
    }

    SerializationResult SceneSerializer::ValidateSceneFile(const std::string& filePath)
    {
        SerializationResult result;

        std::vector<uint8_t> data;
        if (!ReadFromFile(filePath, data))
        {
            result.success = false;
            result.errorMessage = "Failed to read file: " + filePath;
            return result;
        }

        if (data.size() < sizeof(SceneHeader))
        {
            result.success = false;
            result.errorMessage = "File too small to be a valid scene file";
            return result;
        }

        SceneHeader header;
        memcpy(&header, data.data(), sizeof(SceneHeader));

        if (header.magic != SCENE_FILE_MAGIC)
        {
            result.success = false;
            result.errorMessage = "Invalid scene file magic number";
            return result;
        }

        if (header.version > SCENE_FILE_VERSION)
        {
            result.warnings.push_back("Scene file version is newer than supported");
        }

        result.success = true;
        result.bytesProcessed = data.size();
        return result;
    }

    SerializationResult SceneSerializer::ConvertSceneFormat(const std::string& inputPath, const std::string& outputPath,
                                                            SerializationFormat outputFormat)
    {
        SceneFile scene;
        SerializationResult loadResult = LoadScene(inputPath, scene);
        if (!loadResult.success)
        {
            return loadResult;
        }
        return SaveScene(scene, outputPath, outputFormat);
    }

    std::vector<std::string> SceneSerializer::GetSupportedExtensions(SerializationFormat format)
    {
        switch (format)
        {
        case SerializationFormat::BINARY:
            return {".spks", ".scene"};
        case SerializationFormat::JSON:
            return {".json", ".scenejson"};
        default:
            return {".spks", ".scene", ".json", ".scenejson"};
        }
    }

    SerializationFormat SceneSerializer::DetectFormat(const std::string& filePath)
    {
        size_t dotPos = filePath.rfind('.');
        if (dotPos == std::string::npos)
        {
            return SerializationFormat::AUTO;
        }
        std::string ext = filePath.substr(dotPos);
        if (ext == ".json" || ext == ".scenejson")
        {
            return SerializationFormat::JSON;
        }
        if (ext == ".spks" || ext == ".scene")
        {
            return SerializationFormat::BINARY;
        }
        return SerializationFormat::AUTO;
    }

    bool SceneSerializer::IsSceneFile(const std::string& filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
            return false;

        uint32_t magic = 0;
        if (!file.read(reinterpret_cast<char*>(&magic), sizeof(magic)))
            return false;
        if (magic == SCENE_FILE_MAGIC)
            return true;

        // Check for JSON format
        file.seekg(0);
        char firstChar = 0;
        if (!file.read(&firstChar, 1))
            return false;
        return firstChar == '{';
    }

    bool SceneSerializer::ValidateScene(const SceneFile& scene, SerializationResult& result)
    {
        std::vector<std::string> errors;
        bool valid = scene.Validate(errors);
        for (const auto& err : errors)
        {
            result.warnings.push_back(err);
        }
        return valid;
    }

    bool SceneSerializer::HandleVersionCompatibility(uint32_t fileVersion, SceneFile& /*scene*/,
                                                     SerializationResult& result)
    {
        if (fileVersion > SCENE_FILE_VERSION)
        {
            result.warnings.push_back("Scene file version " + std::to_string(fileVersion) +
                                      " is newer than supported version " + std::to_string(SCENE_FILE_VERSION));
        }
        return true;
    }

    bool SceneSerializer::CreateBackup(const std::string& filePath)
    {
        std::string backupPath = filePath + ".bak";
        try
        {
            if (std::filesystem::exists(filePath))
            {
                std::filesystem::copy_file(filePath, backupPath, std::filesystem::copy_options::overwrite_existing);
                return true;
            }
        }
        catch (const std::exception&)
        {
            // Backup failure is non-fatal
        }
        return false;
    }

} // namespace SparkEditor
