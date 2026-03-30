/**
 * @file BinarySceneSerializer.cpp
 * @brief Binary serialization and deserialization for scene files
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains binary-format Save/Load, transform and component serialization,
 * file I/O helpers, and compression stubs.  Split from SceneSerializer.cpp.
 */

#include "SceneSerializer.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <fstream>
#include <cstring>
#include <filesystem>

using namespace DirectX;
namespace SparkEditor
{

    SerializationResult SceneSerializer::SaveBinary(const SceneFile& scene, const std::string& filePath)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Saving scene (binary) to: %s (%zu objects)", filePath.c_str(),
                       scene.objects.size());
        SerializationResult result;
        std::vector<uint8_t> buffer;

        // Write header
        size_t headerSize = sizeof(SceneHeader);
        buffer.resize(headerSize);
        memcpy(buffer.data(), &scene.header, headerSize);

        // Write objects
        for (const auto& obj : scene.objects)
        {
            // Object ID
            size_t offset = buffer.size();
            buffer.resize(offset + sizeof(ObjectID));
            memcpy(buffer.data() + offset, &obj.id, sizeof(ObjectID));

            // Name length + name
            uint32_t nameLen = static_cast<uint32_t>(obj.name.size());
            offset = buffer.size();
            buffer.resize(offset + sizeof(uint32_t) + nameLen);
            memcpy(buffer.data() + offset, &nameLen, sizeof(uint32_t));
            memcpy(buffer.data() + offset + sizeof(uint32_t), obj.name.data(), nameLen);

            // Transform
            SerializeTransform(obj.transform, buffer);

            // Active flag
            offset = buffer.size();
            buffer.resize(offset + 1);
            buffer[offset] = obj.active ? 1 : 0;
        }

        if (!WriteToFile(filePath, buffer))
        {
            result.success = false;
            result.errorMessage = "Failed to write file: " + filePath;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to write binary scene: %s", filePath.c_str());
            return result;
        }

        result.success = true;
        result.bytesProcessed = buffer.size();
        m_totalBytesWritten += buffer.size();
        return result;
    }

    SerializationResult SceneSerializer::LoadBinary(const std::string& filePath, SceneFile& outScene)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading scene (binary) from: %s", filePath.c_str());
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
            result.errorMessage = "File too small for scene header";
            return result;
        }

        memcpy(&outScene.header, data.data(), sizeof(SceneHeader));

        if (outScene.header.magic != SCENE_FILE_MAGIC)
        {
            result.success = false;
            result.errorMessage = "Invalid scene file magic number";
            return result;
        }

        HandleVersionCompatibility(outScene.header.version, outScene, result);

        result.success = true;
        result.bytesProcessed = data.size();
        m_totalBytesRead += data.size();
        return result;
    }

    void SceneSerializer::SerializeTransform(const Transform& transform, std::vector<uint8_t>& buffer)
    {
        size_t offset = buffer.size();
        size_t transformSize = sizeof(float) * 10 + sizeof(ObjectID); // pos(3) + rot(4) + scale(3) + parentID
        buffer.resize(offset + transformSize);

        memcpy(buffer.data() + offset, &transform.position, sizeof(XMFLOAT3));
        offset += sizeof(XMFLOAT3);
        memcpy(buffer.data() + offset, &transform.rotation, sizeof(XMFLOAT4));
        offset += sizeof(XMFLOAT4);
        memcpy(buffer.data() + offset, &transform.scale, sizeof(XMFLOAT3));
        offset += sizeof(XMFLOAT3);
        memcpy(buffer.data() + offset, &transform.parentID, sizeof(ObjectID));
    }

    bool SceneSerializer::DeserializeTransform(const std::vector<uint8_t>& buffer, size_t& offset, Transform& transform)
    {
        size_t needed = sizeof(XMFLOAT3) + sizeof(XMFLOAT4) + sizeof(XMFLOAT3) + sizeof(ObjectID);
        if (offset + needed > buffer.size())
            return false;

        memcpy(&transform.position, buffer.data() + offset, sizeof(XMFLOAT3));
        offset += sizeof(XMFLOAT3);
        memcpy(&transform.rotation, buffer.data() + offset, sizeof(XMFLOAT4));
        offset += sizeof(XMFLOAT4);
        memcpy(&transform.scale, buffer.data() + offset, sizeof(XMFLOAT3));
        offset += sizeof(XMFLOAT3);
        memcpy(&transform.parentID, buffer.data() + offset, sizeof(ObjectID));
        offset += sizeof(ObjectID);
        return true;
    }

    void SceneSerializer::SerializeComponent(const Component& component, std::vector<uint8_t>& buffer)
    {
        size_t offset = buffer.size();
        buffer.resize(offset + sizeof(ComponentType) + sizeof(ObjectID) + sizeof(uint32_t) + component.data.size());

        memcpy(buffer.data() + offset, &component.type, sizeof(ComponentType));
        offset += sizeof(ComponentType);
        memcpy(buffer.data() + offset, &component.objectID, sizeof(ObjectID));
        offset += sizeof(ObjectID);
        uint32_t dataSize = static_cast<uint32_t>(component.data.size());
        memcpy(buffer.data() + offset, &dataSize, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        if (dataSize > 0)
        {
            memcpy(buffer.data() + offset, component.data.data(), dataSize);
        }
    }

    bool SceneSerializer::DeserializeComponent(const std::vector<uint8_t>& buffer, size_t& offset, Component& component)
    {
        if (offset + sizeof(ComponentType) + sizeof(ObjectID) + sizeof(uint32_t) > buffer.size())
            return false;

        memcpy(&component.type, buffer.data() + offset, sizeof(ComponentType));
        offset += sizeof(ComponentType);
        memcpy(&component.objectID, buffer.data() + offset, sizeof(ObjectID));
        offset += sizeof(ObjectID);
        uint32_t dataSize = 0;
        memcpy(&dataSize, buffer.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        // Sanity check: component data should not exceed 16 MB
        if (dataSize > 16 * 1024 * 1024)
            return false;
        if (offset + dataSize > buffer.size())
            return false;
        component.data.resize(dataSize);
        if (dataSize > 0)
        {
            memcpy(component.data.data(), buffer.data() + offset, dataSize);
        }
        offset += dataSize;
        return true;
    }

    bool SceneSerializer::WriteToFile(const std::string& filePath, const std::vector<uint8_t>& data)
    {
        std::ofstream file(filePath, std::ios::binary);
        if (!file.is_open())
            return false;
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        return file.good();
    }

    bool SceneSerializer::ReadFromFile(const std::string& filePath, std::vector<uint8_t>& data)
    {
        if (filePath.empty())
            return false;

        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return false;

        auto tellResult = file.tellg();
        if (tellResult < 0)
            return false;

        size_t fileSize = static_cast<size_t>(tellResult);
        if (fileSize == 0 || fileSize > m_maxFileSize)
            return false;

        file.seekg(0);
        data.resize(fileSize);
        file.read(reinterpret_cast<char*>(data.data()), fileSize);
        return file.good();
    }

    bool SceneSerializer::CompressData(const std::vector<uint8_t>& input, std::vector<uint8_t>& output)
    {
        // No compression without a compression library - just copy
        output = input;
        return true;
    }

    bool SceneSerializer::DecompressData(const std::vector<uint8_t>& input, std::vector<uint8_t>& output)
    {
        // No decompression without a compression library - just copy
        output = input;
        return true;
    }

} // namespace SparkEditor
