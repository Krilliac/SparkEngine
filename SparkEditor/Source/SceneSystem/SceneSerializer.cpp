/**
 * @file SceneSerializer.cpp
 * @brief Implementation of the scene serialization system
 * @author Spark Engine Team
 * @date 2025
 */

#include "SceneSerializer.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstring>
#include <filesystem>

using namespace DirectX;
namespace SparkEditor {

SceneSerializer::SceneSerializer() = default;
SceneSerializer::~SceneSerializer() = default;

SerializationResult SceneSerializer::SaveScene(const SceneFile& scene,
                                                const std::string& filePath,
                                                SerializationFormat format) {
    auto startTime = std::chrono::high_resolution_clock::now();

    SerializationFormat actualFormat = format;
    if (actualFormat == SerializationFormat::AUTO) {
        actualFormat = DetectFormat(filePath);
        if (actualFormat == SerializationFormat::AUTO) {
            actualFormat = SerializationFormat::BINARY;
        }
    }

    SerializationResult result;
    if (m_createBackups) {
        CreateBackup(filePath);
    }

    if (actualFormat == SerializationFormat::BINARY) {
        result = SaveBinary(scene, filePath);
    } else {
        result = SaveJSON(scene, filePath);
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    result.processingTime = std::chrono::duration<float>(endTime - startTime).count();
    m_totalProcessingTime += result.processingTime;
    return result;
}

SerializationResult SceneSerializer::LoadScene(const std::string& filePath,
                                                SceneFile& outScene) {
    auto startTime = std::chrono::high_resolution_clock::now();

    SerializationFormat format = DetectFormat(filePath);
    SerializationResult result;

    if (format == SerializationFormat::JSON) {
        result = LoadJSON(filePath, outScene);
    } else {
        result = LoadBinary(filePath, outScene);
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    result.processingTime = std::chrono::duration<float>(endTime - startTime).count();
    m_totalProcessingTime += result.processingTime;
    return result;
}

SerializationResult SceneSerializer::ValidateSceneFile(const std::string& filePath) {
    SerializationResult result;

    std::vector<uint8_t> data;
    if (!ReadFromFile(filePath, data)) {
        result.success = false;
        result.errorMessage = "Failed to read file: " + filePath;
        return result;
    }

    if (data.size() < sizeof(SceneHeader)) {
        result.success = false;
        result.errorMessage = "File too small to be a valid scene file";
        return result;
    }

    SceneHeader header;
    memcpy(&header, data.data(), sizeof(SceneHeader));

    if (header.magic != SCENE_FILE_MAGIC) {
        result.success = false;
        result.errorMessage = "Invalid scene file magic number";
        return result;
    }

    if (header.version > SCENE_FILE_VERSION) {
        result.warnings.push_back("Scene file version is newer than supported");
    }

    result.success = true;
    result.bytesProcessed = data.size();
    return result;
}

SerializationResult SceneSerializer::ConvertSceneFormat(const std::string& inputPath,
                                                         const std::string& outputPath,
                                                         SerializationFormat outputFormat) {
    SceneFile scene;
    SerializationResult loadResult = LoadScene(inputPath, scene);
    if (!loadResult.success) {
        return loadResult;
    }
    return SaveScene(scene, outputPath, outputFormat);
}

std::vector<std::string> SceneSerializer::GetSupportedExtensions(SerializationFormat format) {
    switch (format) {
        case SerializationFormat::BINARY:
            return {".spks", ".scene"};
        case SerializationFormat::JSON:
            return {".json", ".scenejson"};
        default:
            return {".spks", ".scene", ".json", ".scenejson"};
    }
}

SerializationFormat SceneSerializer::DetectFormat(const std::string& filePath) {
    size_t dotPos = filePath.rfind('.');
    if (dotPos == std::string::npos) {
        return SerializationFormat::AUTO;
    }
    std::string ext = filePath.substr(dotPos);
    if (ext == ".json" || ext == ".scenejson") {
        return SerializationFormat::JSON;
    }
    if (ext == ".spks" || ext == ".scene") {
        return SerializationFormat::BINARY;
    }
    return SerializationFormat::AUTO;
}

bool SceneSerializer::IsSceneFile(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;

    uint32_t magic = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic == SCENE_FILE_MAGIC) return true;

    // Check for JSON format
    file.seekg(0);
    char firstChar = 0;
    file.read(&firstChar, 1);
    return firstChar == '{';
}

// Private methods

SerializationResult SceneSerializer::SaveBinary(const SceneFile& scene, const std::string& filePath) {
    SerializationResult result;
    std::vector<uint8_t> buffer;

    // Write header
    size_t headerSize = sizeof(SceneHeader);
    buffer.resize(headerSize);
    memcpy(buffer.data(), &scene.header, headerSize);

    // Write objects
    for (const auto& obj : scene.objects) {
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

    if (!WriteToFile(filePath, buffer)) {
        result.success = false;
        result.errorMessage = "Failed to write file: " + filePath;
        return result;
    }

    result.success = true;
    result.bytesProcessed = buffer.size();
    m_totalBytesWritten += buffer.size();
    return result;
}

SerializationResult SceneSerializer::SaveJSON(const SceneFile& scene, const std::string& filePath) {
    SerializationResult result;

    std::ofstream file(filePath);
    if (!file.is_open()) {
        result.success = false;
        result.errorMessage = "Failed to open file for writing: " + filePath;
        return result;
    }

    // Simple JSON output
    file << "{\n";
    file << "  \"sceneName\": \"" << scene.header.sceneName << "\",\n";
    file << "  \"version\": " << scene.header.version << ",\n";
    file << "  \"objectCount\": " << scene.objects.size() << ",\n";
    file << "  \"objects\": [\n";

    for (size_t i = 0; i < scene.objects.size(); ++i) {
        const auto& obj = scene.objects[i];
        file << "    {\n";
        file << "      \"id\": " << obj.id << ",\n";
        file << "      \"name\": \"" << obj.name << "\",\n";
        file << "      \"active\": " << (obj.active ? "true" : "false") << ",\n";
        file << "      \"position\": [" << obj.transform.position.x << ", "
             << obj.transform.position.y << ", " << obj.transform.position.z << "]\n";
        file << "    }" << (i + 1 < scene.objects.size() ? "," : "") << "\n";
    }

    file << "  ]\n";
    file << "}\n";

    result.success = true;
    result.bytesProcessed = static_cast<size_t>(file.tellp());
    return result;
}

SerializationResult SceneSerializer::LoadBinary(const std::string& filePath, SceneFile& outScene) {
    SerializationResult result;

    std::vector<uint8_t> data;
    if (!ReadFromFile(filePath, data)) {
        result.success = false;
        result.errorMessage = "Failed to read file: " + filePath;
        return result;
    }

    if (data.size() < sizeof(SceneHeader)) {
        result.success = false;
        result.errorMessage = "File too small for scene header";
        return result;
    }

    memcpy(&outScene.header, data.data(), sizeof(SceneHeader));

    if (outScene.header.magic != SCENE_FILE_MAGIC) {
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

SerializationResult SceneSerializer::LoadJSON(const std::string& filePath, SceneFile& outScene) {
    SerializationResult result;

    std::ifstream file(filePath);
    if (!file.is_open()) {
        result.success = false;
        result.errorMessage = "Failed to open file: " + filePath;
        return result;
    }

    // Basic JSON loading stub - full implementation would use a JSON parser
    result.success = true;
    result.warnings.push_back("JSON loading is a basic implementation");
    return result;
}

void SceneSerializer::SerializeTransform(const Transform& transform, std::vector<uint8_t>& buffer) {
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

bool SceneSerializer::DeserializeTransform(const std::vector<uint8_t>& buffer, size_t& offset, Transform& transform) {
    size_t needed = sizeof(XMFLOAT3) + sizeof(XMFLOAT4) + sizeof(XMFLOAT3) + sizeof(ObjectID);
    if (offset + needed > buffer.size()) return false;

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

void SceneSerializer::SerializeComponent(const Component& component, std::vector<uint8_t>& buffer) {
    size_t offset = buffer.size();
    buffer.resize(offset + sizeof(ComponentType) + sizeof(ObjectID) + sizeof(uint32_t) + component.data.size());

    memcpy(buffer.data() + offset, &component.type, sizeof(ComponentType));
    offset += sizeof(ComponentType);
    memcpy(buffer.data() + offset, &component.objectID, sizeof(ObjectID));
    offset += sizeof(ObjectID);
    uint32_t dataSize = static_cast<uint32_t>(component.data.size());
    memcpy(buffer.data() + offset, &dataSize, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    if (dataSize > 0) {
        memcpy(buffer.data() + offset, component.data.data(), dataSize);
    }
}

bool SceneSerializer::DeserializeComponent(const std::vector<uint8_t>& buffer, size_t& offset, Component& component) {
    if (offset + sizeof(ComponentType) + sizeof(ObjectID) + sizeof(uint32_t) > buffer.size()) return false;

    memcpy(&component.type, buffer.data() + offset, sizeof(ComponentType));
    offset += sizeof(ComponentType);
    memcpy(&component.objectID, buffer.data() + offset, sizeof(ObjectID));
    offset += sizeof(ObjectID);
    uint32_t dataSize = 0;
    memcpy(&dataSize, buffer.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    if (offset + dataSize > buffer.size()) return false;
    component.data.resize(dataSize);
    if (dataSize > 0) {
        memcpy(component.data.data(), buffer.data() + offset, dataSize);
    }
    offset += dataSize;
    return true;
}

void* SceneSerializer::TransformToJSON(const Transform& /*transform*/) {
    return nullptr; // Would require a JSON library
}

bool SceneSerializer::JSONToTransform(void* /*json*/, Transform& /*transform*/) {
    return false; // Would require a JSON library
}

void* SceneSerializer::ComponentToJSON(const Component& /*component*/) {
    return nullptr; // Would require a JSON library
}

bool SceneSerializer::JSONToComponent(void* /*json*/, Component& /*component*/) {
    return false; // Would require a JSON library
}

bool SceneSerializer::ValidateScene(const SceneFile& scene, SerializationResult& result) {
    std::vector<std::string> errors;
    bool valid = scene.Validate(errors);
    for (const auto& err : errors) {
        result.warnings.push_back(err);
    }
    return valid;
}

bool SceneSerializer::HandleVersionCompatibility(uint32_t fileVersion, SceneFile& /*scene*/, SerializationResult& result) {
    if (fileVersion > SCENE_FILE_VERSION) {
        result.warnings.push_back("Scene file version " + std::to_string(fileVersion) +
                                   " is newer than supported version " + std::to_string(SCENE_FILE_VERSION));
    }
    return true;
}

bool SceneSerializer::CreateBackup(const std::string& filePath) {
    std::string backupPath = filePath + ".bak";
    try {
        if (std::filesystem::exists(filePath)) {
            std::filesystem::copy_file(filePath, backupPath,
                std::filesystem::copy_options::overwrite_existing);
            return true;
        }
    } catch (const std::exception&) {
        // Backup failure is non-fatal
    }
    return false;
}

bool SceneSerializer::WriteToFile(const std::string& filePath, const std::vector<uint8_t>& data) {
    std::ofstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}

bool SceneSerializer::ReadFromFile(const std::string& filePath, std::vector<uint8_t>& data) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    size_t fileSize = static_cast<size_t>(file.tellg());
    if (fileSize > m_maxFileSize) return false;

    file.seekg(0);
    data.resize(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    return file.good();
}

bool SceneSerializer::CompressData(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    // No compression without a compression library - just copy
    output = input;
    return true;
}

bool SceneSerializer::DecompressData(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    // No decompression without a compression library - just copy
    output = input;
    return true;
}

} // namespace SparkEditor
