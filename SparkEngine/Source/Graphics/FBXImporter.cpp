/**
 * @file FBXImporter.cpp
 * @brief FBX binary format parser implementation
 */

#include "FBXImporter.h"

#include "Utils/LogMacros.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace Spark::Graphics
{

    // ============================================================================
    // Magic byte detection
    // ============================================================================

    bool FBXImporter::CanImport(const std::string& filePath) const
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
            return false;

        char magic[FBXConstants::MAGIC_LENGTH + 2];
        file.read(magic, FBXConstants::MAGIC_LENGTH);
        if (!file.good())
            return false;

        return std::memcmp(magic, FBXConstants::MAGIC, FBXConstants::MAGIC_LENGTH) == 0;
    }

    bool FBXImporter::CanImportFromMemory(const uint8_t* data, size_t size) const
    {
        if (!data || size < FBXConstants::HEADER_SIZE)
            return false;

        return std::memcmp(data, FBXConstants::MAGIC, FBXConstants::MAGIC_LENGTH) == 0;
    }

    // ============================================================================
    // Header parsing
    // ============================================================================

    bool FBXImporter::ParseHeader(FBXBinaryReader& reader, uint32_t& outVersion) const
    {
        if (!reader.HasRemaining(FBXConstants::HEADER_SIZE))
            return false;

        reader.Skip(23);
        outVersion = reader.Read<uint32_t>();
        return outVersion >= 7000 && outVersion <= 9999;
    }

    // ============================================================================
    // Property parsing
    // ============================================================================

    FBXProperty FBXImporter::ParseProperty(FBXBinaryReader& reader) const
    {
        FBXProperty prop;
        prop.type = static_cast<char>(reader.Read<uint8_t>());

        switch (prop.type)
        {
        case FBXConstants::PROP_INT16:
            prop.intValue = reader.Read<int16_t>();
            break;
        case FBXConstants::PROP_BOOL:
            prop.intValue = reader.Read<uint8_t>();
            break;
        case FBXConstants::PROP_INT32:
            prop.intValue = reader.Read<int32_t>();
            break;
        case FBXConstants::PROP_FLOAT:
            prop.floatValue = reader.Read<float>();
            break;
        case FBXConstants::PROP_DOUBLE:
            prop.doubleValue = reader.Read<double>();
            break;
        case FBXConstants::PROP_INT64:
            prop.longValue = reader.Read<int64_t>();
            break;
        case FBXConstants::PROP_STRING:
        case FBXConstants::PROP_RAW:
        {
            uint32_t len = reader.Read<uint32_t>();
            constexpr uint32_t kMaxStringLen = 16 * 1024 * 1024; // 16 MB sanity limit
            if (len > kMaxStringLen)
                break;
            prop.stringValue = reader.ReadString(len);
            break;
        }
        case FBXConstants::PROP_FLOAT_ARRAY:
        {
            uint32_t count = reader.Read<uint32_t>();
            uint32_t encoding = reader.Read<uint32_t>();
            uint32_t compressedLen = reader.Read<uint32_t>();
            constexpr uint32_t kMaxArrayCount = 1 << 26; // ~64M elements
            if (encoding == 0 && count > 0 && count <= kMaxArrayCount)
            {
                prop.floatArray.resize(count);
                reader.ReadBytes(prop.floatArray.data(), static_cast<size_t>(count) * sizeof(float));
            }
            else
            {
                reader.Skip(compressedLen);
            }
            break;
        }
        case FBXConstants::PROP_DOUBLE_ARRAY:
        {
            uint32_t count = reader.Read<uint32_t>();
            uint32_t encoding = reader.Read<uint32_t>();
            uint32_t compressedLen = reader.Read<uint32_t>();
            constexpr uint32_t kMaxArrayCount = 1 << 26;
            if (encoding == 0 && count > 0 && count <= kMaxArrayCount)
            {
                prop.doubleArray.resize(count);
                reader.ReadBytes(prop.doubleArray.data(), static_cast<size_t>(count) * sizeof(double));
            }
            else
            {
                reader.Skip(compressedLen);
            }
            break;
        }
        case FBXConstants::PROP_INT32_ARRAY:
        {
            uint32_t count = reader.Read<uint32_t>();
            uint32_t encoding = reader.Read<uint32_t>();
            uint32_t compressedLen = reader.Read<uint32_t>();
            constexpr uint32_t kMaxArrayCount = 1 << 26;
            if (encoding == 0 && count > 0 && count <= kMaxArrayCount)
            {
                prop.intArray.resize(count);
                reader.ReadBytes(prop.intArray.data(), static_cast<size_t>(count) * sizeof(int32_t));
            }
            else
            {
                reader.Skip(compressedLen);
            }
            break;
        }
        case FBXConstants::PROP_INT64_ARRAY:
        {
            uint32_t count = reader.Read<uint32_t>();
            uint32_t encoding = reader.Read<uint32_t>();
            uint32_t compressedLen = reader.Read<uint32_t>();
            constexpr uint32_t kMaxArrayCount = 1 << 26;
            if (encoding == 0 && count > 0 && count <= kMaxArrayCount)
            {
                prop.longArray.resize(count);
                reader.ReadBytes(prop.longArray.data(), static_cast<size_t>(count) * sizeof(int64_t));
            }
            else
            {
                reader.Skip(compressedLen);
            }
            break;
        }
        default:
            break;
        }
        return prop;
    }

    // ============================================================================
    // Node parsing
    // ============================================================================

    FBXNode FBXImporter::ParseNode(FBXBinaryReader& reader, uint32_t version) const
    {
        FBXNode node;

        uint64_t endOffset = 0;
        uint64_t numProperties = 0;
        uint64_t propertyListLen = 0;
        uint8_t nameLen = 0;

        if (version >= 7500)
        {
            endOffset = reader.Read<uint64_t>();
            numProperties = reader.Read<uint64_t>();
            propertyListLen = reader.Read<uint64_t>();
            nameLen = reader.Read<uint8_t>();
        }
        else
        {
            endOffset = reader.Read<uint32_t>();
            numProperties = reader.Read<uint32_t>();
            propertyListLen = reader.Read<uint32_t>();
            nameLen = reader.Read<uint8_t>();
        }

        (void)propertyListLen;

        if (endOffset == 0)
            return node;

        node.name = reader.ReadString(nameLen);

        for (uint64_t i = 0; i < numProperties; ++i)
        {
            node.properties.push_back(ParseProperty(reader));
        }

        while (reader.Position() < static_cast<size_t>(endOffset))
        {
            size_t before = reader.Position();
            auto child = ParseNode(reader, version);
            if (child.name.empty() && reader.Position() == before)
                break;
            if (!child.name.empty())
                node.children.push_back(std::move(child));
            if (reader.Position() <= before)
                break;
        }

        reader.Seek(static_cast<size_t>(endOffset));
        return node;
    }

    std::vector<FBXNode> FBXImporter::ParseNodeTree(FBXBinaryReader& reader, uint32_t version) const
    {
        std::vector<FBXNode> nodes;
        while (reader.HasRemaining(13))
        {
            size_t before = reader.Position();
            auto node = ParseNode(reader, version);
            if (node.name.empty() && reader.Position() <= before + 13)
                break;
            if (!node.name.empty())
                nodes.push_back(std::move(node));
        }
        return nodes;
    }

    // ============================================================================
    // Data extraction
    // ============================================================================

    void FBXImporter::TriangulatePolygon(const std::vector<uint32_t>& polygon, std::vector<uint32_t>& outIndices) const
    {
        if (polygon.size() < 3)
            return;
        for (size_t i = 1; i + 1 < polygon.size(); ++i)
        {
            outIndices.push_back(polygon[0]);
            outIndices.push_back(polygon[i]);
            outIndices.push_back(polygon[i + 1]);
        }
    }

    void FBXImporter::ExtractGeometry(const std::vector<FBXNode>& nodes, FBXImportResult& result,
                                      const FBXImportOptions& options) const
    {
        for (const auto& node : nodes)
        {
            if (node.name == "Geometry" && node.properties.size() >= 3 && node.properties[2].stringValue == "Mesh")
            {
                FBXMeshData mesh;

                if (auto* vertNode = node.FindChild("Vertices"))
                {
                    if (!vertNode->properties.empty() && !vertNode->properties[0].doubleArray.empty())
                    {
                        for (double v : vertNode->properties[0].doubleArray)
                            mesh.vertices.push_back(static_cast<float>(v));
                    }
                }

                if (auto* idxNode = node.FindChild("PolygonVertexIndex"))
                {
                    if (!idxNode->properties.empty() && !idxNode->properties[0].intArray.empty())
                    {
                        std::vector<uint32_t> polygon;
                        for (int32_t idx : idxNode->properties[0].intArray)
                        {
                            if (idx < 0)
                            {
                                polygon.push_back(static_cast<uint32_t>(~idx));
                                if (options.triangulate)
                                    TriangulatePolygon(polygon, mesh.indices);
                                else
                                    for (auto i : polygon)
                                        mesh.indices.push_back(i);
                                polygon.clear();
                            }
                            else
                            {
                                polygon.push_back(static_cast<uint32_t>(idx));
                            }
                        }
                    }
                }

                result.meshes.push_back(std::move(mesh));
            }

            ExtractGeometry(node.children, result, options);
        }
    }

    void FBXImporter::ExtractSkeleton(const std::vector<FBXNode>& nodes, FBXImportResult& result) const
    {
        for (const auto& node : nodes)
        {
            if (node.name == "Deformer" && node.properties.size() >= 3 && node.properties[2].stringValue == "Cluster")
            {
                FBXBoneData bone;
                if (node.properties.size() >= 2)
                    bone.name = node.properties[1].stringValue;

                if (auto* transformNode = node.FindChild("Transform"))
                {
                    if (!transformNode->properties.empty() && transformNode->properties[0].doubleArray.size() >= 16)
                    {
                        for (int i = 0; i < 16; ++i)
                            bone.bindPose[i] = static_cast<float>(transformNode->properties[0].doubleArray[i]);
                    }
                }

                result.bones.push_back(std::move(bone));
            }
            ExtractSkeleton(node.children, result);
        }
    }

    void FBXImporter::ExtractAnimations(const std::vector<FBXNode>& nodes, FBXImportResult& result) const
    {
        for (const auto& node : nodes)
        {
            if (node.name == "AnimationStack")
            {
                FBXAnimationData anim;
                if (!node.properties.empty())
                    anim.name = node.properties[0].stringValue;
                result.animations.push_back(std::move(anim));
            }
            ExtractAnimations(node.children, result);
        }
    }

    // ============================================================================
    // Public API
    // ============================================================================

    FBXImportResult FBXImporter::ImportFromMemory(const uint8_t* data, size_t size, const FBXImportOptions& options)
    {
        FBXImportResult result;

        if (!data)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "FBXImporter::ImportFromMemory — null data pointer");
            result.warnings.push_back("Null data pointer");
            return result;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "FBXImporter::ImportFromMemory — %zu bytes", size);

        if (!CanImportFromMemory(data, size))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "FBXImporter: invalid FBX binary format (%zu bytes)", size);
            result.warnings.push_back("Invalid FBX binary format");
            return result;
        }

        FBXBinaryReader reader(data, size);
        uint32_t version = 0;
        if (!ParseHeader(reader, version))
        {
            result.warnings.push_back("Invalid FBX version");
            return result;
        }

        auto nodes = ParseNodeTree(reader, version);

        ExtractGeometry(nodes, result, options);
        ExtractSkeleton(nodes, result);
        if (options.importAnimations)
            ExtractAnimations(nodes, result);

        if (options.targetCoordSystem == CoordinateSystem::LeftHanded)
        {
            for (auto& mesh : result.meshes)
            {
                for (size_t i = 2; i < mesh.vertices.size(); i += 3)
                    mesh.vertices[i] = -mesh.vertices[i];
                for (size_t i = 2; i < mesh.normals.size(); i += 3)
                    mesh.normals[i] = -mesh.normals[i];
            }
        }

        if (std::abs(options.scaleFactor - 1.0f) > 0.001f)
        {
            for (auto& mesh : result.meshes)
                for (auto& v : mesh.vertices)
                    v *= options.scaleFactor;
        }

        if (options.flipUVs)
        {
            for (auto& mesh : result.meshes)
                for (size_t i = 1; i < mesh.uvs.size(); i += 2)
                    mesh.uvs[i] = 1.0f - mesh.uvs[i];
        }

        result.success = true;
        result.warnings.push_back("FBX v" + std::to_string(version / 1000) + "." + std::to_string(version % 1000));
        return result;
    }

    FBXImportResult FBXImporter::Import(const std::string& filePath, const FBXImportOptions& options)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "FBXImporter::Import — file='%s'", filePath.c_str());
        FBXImportResult result;

        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "FBXImporter: failed to open '%s'", filePath.c_str());
            result.warnings.push_back("Failed to open file: " + filePath);
            return result;
        }

        auto fileSize = file.tellg();
        file.seekg(0);

        std::vector<uint8_t> data(static_cast<size_t>(fileSize));
        file.read(reinterpret_cast<char*>(data.data()), fileSize);

        return ImportFromMemory(data.data(), data.size(), options);
    }

    bool FBXImporter::ValidateForPipeline(const FBXImportResult& result, bool requireSkeleton, bool requireAnimation,
                                          std::string* outError) const
    {
        auto fail = [&outError](const std::string& message)
        {
            if (outError)
            {
                *outError = message;
            }
            return false;
        };

        if (!result.success)
        {
            return fail("FBX import did not succeed");
        }

        if (result.meshes.empty())
        {
            return fail("No meshes were imported");
        }

        bool hasRenderableMesh = false;
        for (const auto& mesh : result.meshes)
        {
            if (!mesh.vertices.empty() && !mesh.indices.empty())
            {
                hasRenderableMesh = true;
                break;
            }
        }
        if (!hasRenderableMesh)
        {
            return fail("Imported meshes are missing vertex/index data");
        }

        if (requireSkeleton && result.bones.empty())
        {
            return fail("Skeletal import requested but no bones were imported");
        }

        if (requireAnimation)
        {
            if (result.animations.empty())
            {
                return fail("Animation import requested but no animation clips were imported");
            }

            bool hasValidClip = false;
            for (const auto& clip : result.animations)
            {
                if (!clip.boneTracks.empty())
                {
                    for (const auto& track : clip.boneTracks)
                    {
                        if (!track.keyframes.empty())
                        {
                            hasValidClip = true;
                            break;
                        }
                    }
                }
                if (hasValidClip)
                {
                    break;
                }
            }

            if (!hasValidClip)
            {
                return fail("Animation clips are present but contain no keyframes");
            }
        }

        if (outError)
        {
            outError->clear();
        }
        return true;
    }

} // namespace Spark::Graphics
