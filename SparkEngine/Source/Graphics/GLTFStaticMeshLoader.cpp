/**
 * @file GLTFStaticMeshLoader.cpp
 * @brief Validated CPU-side glTF 2.0 static-mesh loading via cgltf.
 */

#include "GLTFStaticMeshLoader.h"
#include "GLTFValidation.h"

#include <cmath>
#include <new>

namespace Spark::Graphics::Detail
{
#if SPARK_HAS_CGLTF
    namespace
    {
        using GLTF::kMaxIndices;
        using GLTF::kMaxVertices;
        using GLTF::UnpackFloats;

        bool ValidateStaticDocument(const cgltf_data& data, std::string& error)
        {
            // Required extensions keep their own diagnostic ahead of the static-subset check.
            if (data.extensions_required_count == 0 && (data.skins_count != 0 || data.animations_count != 0))
            {
                error = "skins and animations are outside the static-mesh subset";
                return false;
            }
            return GLTF::ValidateDocumentStructure(data, error);
        }

        void GenerateNormals(GLTFStaticMeshData& meshData, size_t vertexStart, size_t vertexCount, size_t indexStart,
                             size_t indexCount)
        {
            std::vector<std::array<float, 3>> accumulated(vertexCount, {0.0f, 0.0f, 0.0f});
            for (size_t i = indexStart; i < indexStart + indexCount; i += 3)
            {
                const size_t i0 = meshData.indices[i] - vertexStart;
                const size_t i1 = meshData.indices[i + 1] - vertexStart;
                const size_t i2 = meshData.indices[i + 2] - vertexStart;
                const auto& p0 = meshData.vertices[vertexStart + i0].position;
                const auto& p1 = meshData.vertices[vertexStart + i1].position;
                const auto& p2 = meshData.vertices[vertexStart + i2].position;

                const float e1x = p1[0] - p0[0];
                const float e1y = p1[1] - p0[1];
                const float e1z = p1[2] - p0[2];
                const float e2x = p2[0] - p0[0];
                const float e2y = p2[1] - p0[1];
                const float e2z = p2[2] - p0[2];
                const std::array<float, 3> normal = {e1y * e2z - e1z * e2y, e1z * e2x - e1x * e2z,
                                                     e1x * e2y - e1y * e2x};
                for (size_t localIndex : {i0, i1, i2})
                {
                    accumulated[localIndex][0] += normal[0];
                    accumulated[localIndex][1] += normal[1];
                    accumulated[localIndex][2] += normal[2];
                }
            }

            for (size_t i = 0; i < vertexCount; ++i)
            {
                auto& normal = accumulated[i];
                const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                meshData.vertices[vertexStart + i].normal =
                    length > 1.0e-6f ? std::array<float, 3>{normal[0] / length, normal[1] / length, normal[2] / length}
                                     : std::array<float, 3>{0.0f, 1.0f, 0.0f};
            }
        }

        bool AppendPrimitive(const cgltf_primitive& primitive, GLTFStaticMeshData& meshData, std::string& error)
        {
            if (primitive.type != cgltf_primitive_type_triangles)
            {
                error = "only triangle-list glTF primitives are supported";
                return false;
            }
            if (primitive.targets_count != 0 || primitive.has_draco_mesh_compression || primitive.extensions_count != 0)
            {
                error = "morph targets and compressed or extended primitives are unsupported";
                return false;
            }

            const cgltf_accessor* positions = nullptr;
            const cgltf_accessor* normals = nullptr;
            const cgltf_accessor* texCoords = nullptr;
            for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
            {
                const cgltf_attribute& attribute = primitive.attributes[i];
                if (!attribute.data)
                {
                    error = "mesh attribute has no accessor";
                    return false;
                }

                switch (attribute.type)
                {
                case cgltf_attribute_type_position:
                    if (attribute.index != 0 || positions)
                    {
                        error = "duplicate or nonzero POSITION attribute";
                        return false;
                    }
                    positions = attribute.data;
                    break;
                case cgltf_attribute_type_normal:
                    if (attribute.index != 0 || normals)
                    {
                        error = "duplicate or nonzero NORMAL attribute";
                        return false;
                    }
                    normals = attribute.data;
                    break;
                case cgltf_attribute_type_texcoord:
                    if (attribute.index != 0 || texCoords)
                    {
                        error = "only one TEXCOORD_0 attribute is supported";
                        return false;
                    }
                    texCoords = attribute.data;
                    break;
                default:
                    error = "mesh contains attributes outside POSITION/NORMAL/TEXCOORD_0";
                    return false;
                }
            }

            if (!positions || positions->type != cgltf_type_vec3 ||
                positions->component_type != cgltf_component_type_r_32f || positions->normalized)
            {
                error = "POSITION must be a non-normalized float VEC3 accessor";
                return false;
            }
            if (normals && (normals->type != cgltf_type_vec3 || normals->component_type != cgltf_component_type_r_32f ||
                            normals->normalized || normals->count != positions->count))
            {
                error = "NORMAL must be a matching non-normalized float VEC3 accessor";
                return false;
            }
            if (texCoords && (texCoords->type != cgltf_type_vec2 || texCoords->count != positions->count ||
                              (texCoords->component_type != cgltf_component_type_r_32f &&
                               !((texCoords->component_type == cgltf_component_type_r_8u ||
                                  texCoords->component_type == cgltf_component_type_r_16u) &&
                                 texCoords->normalized))))
            {
                error = "TEXCOORD_0 must be a matching float or normalized unsigned VEC2 accessor";
                return false;
            }

            const size_t vertexCount = static_cast<size_t>(positions->count);
            const size_t indexCount = primitive.indices ? static_cast<size_t>(primitive.indices->count) : vertexCount;
            if (vertexCount > kMaxVertices - meshData.vertices.size() ||
                indexCount > kMaxIndices - meshData.indices.size())
            {
                error = "static mesh exceeds import limits";
                return false;
            }
            if (indexCount == 0 || indexCount % 3 != 0)
            {
                error = "triangle primitive index count must be nonzero and divisible by three";
                return false;
            }
            if (primitive.indices && (primitive.indices->type != cgltf_type_scalar || primitive.indices->normalized ||
                                      (primitive.indices->component_type != cgltf_component_type_r_8u &&
                                       primitive.indices->component_type != cgltf_component_type_r_16u &&
                                       primitive.indices->component_type != cgltf_component_type_r_32u)))
            {
                error = "indices must be an unsigned scalar accessor";
                return false;
            }

            std::vector<float> positionValues;
            std::vector<float> normalValues;
            std::vector<float> texCoordValues;
            if (!UnpackFloats(*positions, 3, positionValues, error) ||
                (normals && !UnpackFloats(*normals, 3, normalValues, error)) ||
                (texCoords && !UnpackFloats(*texCoords, 2, texCoordValues, error)))
            {
                return false;
            }

            const size_t vertexStart = meshData.vertices.size();
            const size_t indexStart = meshData.indices.size();
            meshData.vertices.reserve(vertexStart + vertexCount);
            meshData.indices.reserve(indexStart + indexCount);
            for (size_t i = 0; i < vertexCount; ++i)
            {
                GLTFStaticVertex vertex{};
                vertex.position = {positionValues[i * 3], positionValues[i * 3 + 1], positionValues[i * 3 + 2]};
                if (normals)
                {
                    vertex.normal = {normalValues[i * 3], normalValues[i * 3 + 1], normalValues[i * 3 + 2]};
                }
                if (texCoords)
                {
                    vertex.texCoord = {texCoordValues[i * 2], texCoordValues[i * 2 + 1]};
                }
                meshData.vertices.push_back(vertex);
            }

            if (primitive.indices)
            {
                for (cgltf_size i = 0; i < primitive.indices->count; ++i)
                {
                    cgltf_uint localIndex = 0;
                    if (!cgltf_accessor_read_uint(primitive.indices, i, &localIndex, 1) || localIndex >= vertexCount)
                    {
                        error = "index accessor contains an invalid vertex index";
                        return false;
                    }
                    meshData.indices.push_back(static_cast<uint32_t>(vertexStart + localIndex));
                }
            }
            else
            {
                for (size_t i = 0; i < vertexCount; ++i)
                {
                    meshData.indices.push_back(static_cast<uint32_t>(vertexStart + i));
                }
            }

            if (!normals)
            {
                GenerateNormals(meshData, vertexStart, vertexCount, indexStart, indexCount);
            }
            meshData.primitives.push_back({static_cast<uint32_t>(indexStart), static_cast<uint32_t>(indexCount)});
            return true;
        }
    } // namespace
#endif

    bool LoadGLTFStaticMesh(const std::filesystem::path& path, GLTFStaticMeshData& meshData, std::string& error)
    {
        meshData = {};
        error.clear();

#if !SPARK_HAS_CGLTF
        (void)path;
        error = "cgltf support is not available in this build";
        return false;
#else
        GLTF::Document document;
        if (!GLTF::ParseDocument(path, document, error))
        {
            return false;
        }
        const cgltf_data& data = *document.data;

        try
        {
            if (!ValidateStaticDocument(data, error) || !GLTF::LoadAndValidateBuffers(document, error))
            {
                return false;
            }

            for (cgltf_size meshIndex = 0; meshIndex < data.meshes_count; ++meshIndex)
            {
                const cgltf_mesh& mesh = data.meshes[meshIndex];
                for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh.primitives_count; ++primitiveIndex)
                {
                    if (!AppendPrimitive(mesh.primitives[primitiveIndex], meshData, error))
                    {
                        meshData = {};
                        return false;
                    }
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            meshData = {};
            error = "out of memory while loading glTF static mesh";
            return false;
        }

        if (meshData.vertices.empty() || meshData.indices.empty() || meshData.primitives.empty())
        {
            meshData = {};
            error = "glTF contains no supported static triangle geometry";
            return false;
        }
        return true;
#endif
    }
} // namespace Spark::Graphics::Detail
