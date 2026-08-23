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
        (void)scene;
        SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Refusing unsupported binary scene save: %s", filePath.c_str());
        SerializationResult result;
        result.errorMessage =
            "Binary scene serialization is unavailable because the legacy format is incomplete; use JSON/.sparkscene";
        return result;
    }

    SerializationResult SceneSerializer::LoadBinary(const std::string& filePath, SceneFile& outScene)
    {
        (void)outScene;
        SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Refusing unsupported binary scene load: %s", filePath.c_str());
        SerializationResult result;
        result.errorMessage =
            "Binary scene deserialization is unavailable because the legacy format is incomplete; use JSON/.sparkscene";
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
        (void)component;
        (void)buffer;
        // Retired: serializing C++ object images is not portable or safe.
    }

    bool SceneSerializer::DeserializeComponent(const std::vector<uint8_t>& buffer, size_t& offset, Component& component)
    {
        (void)buffer;
        (void)offset;
        (void)component;
        // Retired: legacy raw payloads fail closed.
        return false;
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
