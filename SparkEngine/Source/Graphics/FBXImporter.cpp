/**
 * @file FBXImporter.cpp
 * @brief FBX binary format parser implementation
 */

#include "FBXImporter.h"

#include "Utils/LogMacros.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

#if defined(SPARK_MINIZ_AVAILABLE) && SPARK_MINIZ_AVAILABLE
#include <miniz.h>
#endif

namespace Spark::Graphics
{
    namespace
    {
        constexpr size_t kMaxFBXFileBytes = 256ull * 1024ull * 1024ull;
        constexpr uint32_t kMaxArrayElements = 1u * 1024u * 1024u;
        constexpr uint64_t kMaxPropertiesPerNode = 100'000;
        constexpr uint32_t kMaxNodeDepth = 128;
        constexpr uint32_t kMaxNodeCount = 50'000;
        constexpr size_t kMaxMeshIndices = 3u * 1024u * 1024u;
        constexpr size_t kMaxTotalVertexFloats = 8u * 1024u * 1024u;
        constexpr size_t kMaxTotalIndices = 16u * 1024u * 1024u;

        template <typename T>
        bool ReadArray(FBXBinaryReader& reader, uint32_t count, uint32_t encoding, uint32_t storedLength,
                       std::vector<T>& output)
        {
            if (count == 0)
            {
                if (storedLength != 0)
                    reader.Invalidate();
                return reader.IsValid();
            }
            if (count > kMaxArrayElements || count > std::numeric_limits<size_t>::max() / sizeof(T))
            {
                reader.Invalidate();
                return false;
            }

            const size_t decodedBytes = static_cast<size_t>(count) * sizeof(T);
            if (encoding == 0)
            {
                if (storedLength != decodedBytes || !reader.HasRemaining(decodedBytes))
                {
                    reader.Invalidate();
                    return false;
                }
            }
            else if (encoding != 1 || storedLength == 0 || !reader.HasRemaining(storedLength))
            {
                reader.Invalidate();
                return false;
            }

            if (!reader.ReserveDecodedBytes(decodedBytes))
                return false;
            try
            {
                output.resize(count);
            }
            catch (const std::bad_alloc&)
            {
                reader.Invalidate();
                return false;
            }

            if (encoding == 0)
                return reader.ReadBytes(output.data(), decodedBytes);

#if defined(SPARK_MINIZ_AVAILABLE) && SPARK_MINIZ_AVAILABLE
            const uint8_t* compressed = reader.Data() + reader.Position();
            mz_ulong destinationLength = static_cast<mz_ulong>(decodedBytes);
            mz_ulong sourceLength = static_cast<mz_ulong>(storedLength);
            const int status = mz_uncompress2(reinterpret_cast<unsigned char*>(output.data()), &destinationLength,
                                              compressed, &sourceLength);
            if (!reader.Skip(storedLength))
                return false;
            if (status != MZ_OK || destinationLength != decodedBytes || sourceLength != storedLength)
            {
                reader.Invalidate();
                return false;
            }
            return true;
#else
            reader.Invalidate();
            return false;
#endif
        }
    } // namespace

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
        if (!reader.HasRemaining())
        {
            reader.Invalidate();
            return prop;
        }
        prop.type = static_cast<char>(reader.Read<uint8_t>());

        switch (prop.type)
        {
        case FBXConstants::PROP_INT16:
            prop.intValue = reader.Read<int16_t>();
            break;
        case FBXConstants::PROP_INT8:
        case FBXConstants::PROP_BOOL:
        case FBXConstants::PROP_CHAR:
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
            if (len > kMaxStringLen || !reader.HasRemaining(len))
            {
                reader.Invalidate();
                break;
            }
            if (prop.type == FBXConstants::PROP_STRING)
                prop.stringValue = reader.ReadString(len);
            else
            {
                if (!reader.ReserveDecodedBytes(len))
                    break;
                prop.rawData.resize(len);
                reader.ReadBytes(prop.rawData.data(), len);
            }
            break;
        }
        case FBXConstants::PROP_FLOAT_ARRAY:
        {
            uint32_t count = reader.Read<uint32_t>();
            uint32_t encoding = reader.Read<uint32_t>();
            uint32_t compressedLen = reader.Read<uint32_t>();
            ReadArray(reader, count, encoding, compressedLen, prop.floatArray);
            break;
        }
        case FBXConstants::PROP_DOUBLE_ARRAY:
        {
            uint32_t count = reader.Read<uint32_t>();
            uint32_t encoding = reader.Read<uint32_t>();
            uint32_t compressedLen = reader.Read<uint32_t>();
            ReadArray(reader, count, encoding, compressedLen, prop.doubleArray);
            break;
        }
        case FBXConstants::PROP_INT32_ARRAY:
        {
            uint32_t count = reader.Read<uint32_t>();
            uint32_t encoding = reader.Read<uint32_t>();
            uint32_t compressedLen = reader.Read<uint32_t>();
            ReadArray(reader, count, encoding, compressedLen, prop.intArray);
            break;
        }
        case FBXConstants::PROP_INT64_ARRAY:
        {
            uint32_t count = reader.Read<uint32_t>();
            uint32_t encoding = reader.Read<uint32_t>();
            uint32_t compressedLen = reader.Read<uint32_t>();
            ReadArray(reader, count, encoding, compressedLen, prop.longArray);
            break;
        }
        case FBXConstants::PROP_BOOL_ARRAY:
        case FBXConstants::PROP_BYTE_ARRAY:
        {
            const uint32_t count = reader.Read<uint32_t>();
            const uint32_t encoding = reader.Read<uint32_t>();
            const uint32_t compressedLen = reader.Read<uint32_t>();
            ReadArray(reader, count, encoding, compressedLen, prop.rawData);
            break;
        }
        default:
            reader.Invalidate();
            break;
        }
        return prop;
    }

    // ============================================================================
    // Node parsing
    // ============================================================================

    FBXNode FBXImporter::ParseNode(FBXBinaryReader& reader, uint32_t version, size_t parentEnd, uint32_t depth,
                                   uint32_t& nodeCount) const
    {
        FBXNode node;

        const size_t recordSize = version >= 7500 ? 25u : 13u;
        if (depth > kMaxNodeDepth || nodeCount >= kMaxNodeCount || parentEnd > reader.Size() ||
            reader.Position() > parentEnd || recordSize > parentEnd - reader.Position())
        {
            reader.Invalidate();
            return node;
        }

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

        if (endOffset == 0)
        {
            if (numProperties != 0 || propertyListLen != 0 || nameLen != 0)
                reader.Invalidate();
            return node;
        }

        ++nodeCount;
        if (!reader.IsValid() || endOffset > parentEnd || endOffset > reader.Size() || endOffset <= reader.Position() ||
            (depth > 0 && (parentEnd < recordSize || endOffset > parentEnd - recordSize)) ||
            numProperties > kMaxPropertiesPerNode || nameLen == 0 || nameLen > endOffset - reader.Position())
        {
            reader.Invalidate();
            return {};
        }

        node.name = reader.ReadString(nameLen);
        if (!reader.IsValid() || !reader.ReserveProperties(static_cast<size_t>(numProperties)) ||
            propertyListLen > endOffset - reader.Position())
        {
            reader.Invalidate();
            return {};
        }
        const size_t propertyEnd = reader.Position() + static_cast<size_t>(propertyListLen);

        if (recordSize > static_cast<size_t>(endOffset) - propertyEnd)
        {
            reader.Invalidate();
            return {};
        }

        try
        {
            node.properties.reserve(static_cast<size_t>(numProperties));
        }
        catch (const std::bad_alloc&)
        {
            reader.Invalidate();
            return {};
        }

        const size_t nodeLimit = reader.Limit();
        if (!reader.SetLimit(propertyEnd))
            return {};

        for (uint64_t i = 0; i < numProperties; ++i)
        {
            node.properties.push_back(ParseProperty(reader));
            if (!reader.IsValid() || reader.Position() > propertyEnd)
            {
                reader.Invalidate();
                return {};
            }
        }
        if (reader.Position() != propertyEnd)
        {
            reader.Invalidate();
            return {};
        }
        if (!reader.SetLimit(nodeLimit))
            return {};

        while (reader.Position() < endOffset)
        {
            size_t before = reader.Position();
            auto child = ParseNode(reader, version, static_cast<size_t>(endOffset), depth + 1, nodeCount);
            if (!reader.IsValid())
                return {};
            if (child.name.empty())
            {
                if (reader.Position() != endOffset)
                {
                    reader.Invalidate();
                    return {};
                }
                break;
            }
            node.children.push_back(std::move(child));
            if (reader.Position() <= before)
            {
                reader.Invalidate();
                return {};
            }
        }

        if (reader.Position() != endOffset)
        {
            reader.Invalidate();
            return {};
        }
        return node;
    }

    std::vector<FBXNode> FBXImporter::ParseNodeTree(FBXBinaryReader& reader, uint32_t version) const
    {
        std::vector<FBXNode> nodes;
        const size_t recordSize = version >= 7500 ? 25u : 13u;
        uint32_t nodeCount = 0;
        bool sawTerminator = false;
        while (reader.HasRemaining(recordSize))
        {
            auto node = ParseNode(reader, version, reader.Size(), 0, nodeCount);
            if (!reader.IsValid())
                return {};
            if (node.name.empty())
            {
                sawTerminator = true;
                break;
            }
            nodes.push_back(std::move(node));
        }
        if (!nodes.empty() && !sawTerminator)
        {
            reader.Invalidate();
            return {};
        }
        return nodes;
    }

    // ============================================================================
    // Data extraction
    // ============================================================================

    bool FBXImporter::TriangulatePolygon(const std::vector<uint32_t>& polygon, std::vector<uint32_t>& outIndices) const
    {
        if (polygon.size() < 3)
            return true;
        const size_t triangleIndices = (polygon.size() - 2) * 3;
        if (triangleIndices > kMaxMeshIndices - outIndices.size())
            return false;
        outIndices.reserve(outIndices.size() + triangleIndices);
        for (size_t i = 1; i + 1 < polygon.size(); ++i)
        {
            outIndices.push_back(polygon[0]);
            outIndices.push_back(polygon[i]);
            outIndices.push_back(polygon[i + 1]);
        }
        return true;
    }

    void FBXImporter::ExtractGeometry(const std::vector<FBXNode>& nodes, FBXImportResult& result,
                                      const FBXImportOptions& options, size_t& totalVertexFloats,
                                      size_t& totalIndices) const
    {
        for (const auto& node : nodes)
        {
            if (node.name == "Geometry" && node.properties.size() >= 3 && node.properties[2].stringValue == "Mesh")
            {
                FBXMeshData mesh;
                bool exceededOutputBudget = false;

                if (auto* vertNode = node.FindChild("Vertices"))
                {
                    if (!vertNode->properties.empty() && !vertNode->properties[0].doubleArray.empty())
                    {
                        for (double v : vertNode->properties[0].doubleArray)
                        {
                            if (!std::isfinite(v) || v > std::numeric_limits<float>::max() ||
                                v < -std::numeric_limits<float>::max())
                            {
                                exceededOutputBudget = true;
                                break;
                            }
                            mesh.vertices.push_back(static_cast<float>(v));
                        }
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
                                    exceededOutputBudget = !TriangulatePolygon(polygon, mesh.indices);
                                else
                                {
                                    if (polygon.size() > kMaxMeshIndices - mesh.indices.size())
                                        exceededOutputBudget = true;
                                    for (auto i : polygon)
                                    {
                                        if (!exceededOutputBudget)
                                            mesh.indices.push_back(i);
                                    }
                                }
                                polygon.clear();
                                if (exceededOutputBudget)
                                    break;
                            }
                            else
                            {
                                polygon.push_back(static_cast<uint32_t>(idx));
                            }
                        }
                    }
                }

                if (mesh.vertices.size() % 3 != 0)
                {
                    result.warnings.push_back("Rejected Geometry node with a non-triplet vertex array");
                    continue;
                }
                if (exceededOutputBudget)
                {
                    result.warnings.push_back("Rejected Geometry node that exceeds the index output budget");
                    continue;
                }
                mesh.vertexCount = static_cast<uint32_t>(mesh.vertices.size() / 3);
                if (mesh.vertexCount == 0 || mesh.indices.empty() ||
                    std::any_of(mesh.indices.begin(), mesh.indices.end(),
                                [&](uint32_t index) { return index >= mesh.vertexCount; }))
                {
                    result.warnings.push_back("Rejected Geometry node with missing or out-of-range indices");
                    continue;
                }

                if (mesh.vertices.size() > kMaxTotalVertexFloats - totalVertexFloats ||
                    mesh.indices.size() > kMaxTotalIndices - totalIndices)
                {
                    result.warnings.push_back("Rejected Geometry node that exceeds the aggregate mesh output budget");
                    continue;
                }

                totalVertexFloats += mesh.vertices.size();
                totalIndices += mesh.indices.size();
                result.meshes.push_back(std::move(mesh));
            }

            ExtractGeometry(node.children, result, options, totalVertexFloats, totalIndices);
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
                        bool validBindPose = true;
                        for (int i = 0; i < 16; ++i)
                        {
                            const double value = transformNode->properties[0].doubleArray[i];
                            if (!std::isfinite(value) || value > std::numeric_limits<float>::max() ||
                                value < -std::numeric_limits<float>::max())
                            {
                                validBindPose = false;
                                break;
                            }
                            bone.bindPose[i] = static_cast<float>(value);
                        }
                        if (!validBindPose)
                        {
                            result.warnings.push_back("Rejected Cluster node with a non-finite bind pose");
                            continue;
                        }
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

        if (size > kMaxFBXFileBytes)
        {
            result.errorMessage = "FBX input exceeds the 256 MiB import budget";
            result.warnings.push_back(result.errorMessage);
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

        std::vector<FBXNode> nodes;
        try
        {
            nodes = ParseNodeTree(reader, version);
            if (reader.IsValid() && !nodes.empty())
            {
                size_t totalVertexFloats = 0;
                size_t totalIndices = 0;
                ExtractGeometry(nodes, result, options, totalVertexFloats, totalIndices);
                ExtractSkeleton(nodes, result);
                if (options.importAnimations)
                    ExtractAnimations(nodes, result);
            }
        }
        catch (const std::bad_alloc&)
        {
            reader.Invalidate();
            result.errorMessage = "FBX import exceeded available memory";
        }
        catch (const std::exception& e)
        {
            reader.Invalidate();
            result.errorMessage = std::string("FBX import failed: ") + e.what();
        }

        if (!reader.IsValid() || nodes.empty())
        {
            if (result.errorMessage.empty())
                result.errorMessage = reader.IsValid() ? "FBX contains no document nodes" : "Malformed FBX node tree";
            result.warnings.push_back(result.errorMessage);
            return result;
        }

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

        result.fbxVersion = version;
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
        if (fileSize <= 0 || static_cast<uint64_t>(fileSize) > kMaxFBXFileBytes)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "FBXImporter: empty or unreadable file '%s'",
                            filePath.c_str());
            result.warnings.push_back("Empty or unreadable file: " + filePath);
            return result;
        }
        file.seekg(0);

        try
        {
            std::vector<uint8_t> data(static_cast<size_t>(fileSize));
            if (!file.read(reinterpret_cast<char*>(data.data()), fileSize))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "FBXImporter: read failed for '%s'", filePath.c_str());
                result.warnings.push_back("Read failed: " + filePath);
                return result;
            }

            return ImportFromMemory(data.data(), data.size(), options);
        }
        catch (const std::bad_alloc&)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "FBXImporter: allocation failed for '%s'", filePath.c_str());
            result.warnings.push_back("Insufficient memory to read: " + filePath);
            return result;
        }
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
