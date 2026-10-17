/**
 * @file GLTFSkinnedMeshLoader.cpp
 * @brief Validated CPU-side glTF 2.0 skin import (JOINTS_0/WEIGHTS_0, inverse binds, joint tree) via cgltf.
 */

#include "GLTFSkinnedMeshLoader.h"
#include "GLTFSkinSkeleton.h"
#include "GLTFValidation.h"

#include <cmath>
#include <new>

namespace Spark::Graphics::Detail
{
#if SPARK_HAS_CGLTF
    namespace
    {
        struct SkinAttributes
        {
            const cgltf_accessor* positions = nullptr;
            const cgltf_accessor* normals = nullptr;
            const cgltf_accessor* texCoords = nullptr;
            const cgltf_accessor* joints = nullptr;
            const cgltf_accessor* weights = nullptr;
        };

        bool CollectAttributes(const cgltf_primitive& primitive, SkinAttributes& attributes, std::string& error)
        {
            for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
            {
                const cgltf_attribute& attribute = primitive.attributes[i];
                const cgltf_accessor** slot = nullptr;
                switch (attribute.type)
                {
                case cgltf_attribute_type_position:
                    slot = &attributes.positions;
                    break;
                case cgltf_attribute_type_normal:
                    slot = &attributes.normals;
                    break;
                case cgltf_attribute_type_texcoord:
                    slot = &attributes.texCoords;
                    break;
                case cgltf_attribute_type_joints:
                    slot = &attributes.joints;
                    break;
                case cgltf_attribute_type_weights:
                    slot = &attributes.weights;
                    break;
                default:
                    break;
                }

                const std::string name = attribute.name ? attribute.name : "unnamed";
                if ((attribute.type == cgltf_attribute_type_joints || attribute.type == cgltf_attribute_type_weights) &&
                    attribute.index != 0)
                {
                    error = "attribute " + name +
                            " would exceed 4 influences per vertex; only JOINTS_0/WEIGHTS_0 "
                            "are supported";
                    return false;
                }
                if (!slot || attribute.index != 0)
                {
                    error = "attribute " + name + " is outside POSITION/NORMAL/TEXCOORD_0/JOINTS_0/WEIGHTS_0";
                    return false;
                }
                if (!attribute.data || *slot)
                {
                    error = "attribute " + name + " is duplicated or has no accessor";
                    return false;
                }
                *slot = attribute.data;
            }

            const cgltf_accessor* positions = attributes.positions;
            if (!positions || positions->type != cgltf_type_vec3 ||
                positions->component_type != cgltf_component_type_r_32f || positions->normalized)
            {
                error = "POSITION must be a non-normalized float VEC3 accessor";
                return false;
            }
            const cgltf_accessor* normals = attributes.normals;
            if (!normals || normals->type != cgltf_type_vec3 || normals->component_type != cgltf_component_type_r_32f ||
                normals->normalized || normals->count != positions->count)
            {
                error = "skinned primitives require a matching non-normalized float VEC3 NORMAL accessor";
                return false;
            }
            const cgltf_accessor* texCoords = attributes.texCoords;
            if (texCoords && (texCoords->type != cgltf_type_vec2 || texCoords->count != positions->count ||
                              (texCoords->component_type != cgltf_component_type_r_32f &&
                               !((texCoords->component_type == cgltf_component_type_r_8u ||
                                  texCoords->component_type == cgltf_component_type_r_16u) &&
                                 texCoords->normalized))))
            {
                error = "TEXCOORD_0 must be a matching float or normalized unsigned VEC2 accessor";
                return false;
            }
            const cgltf_accessor* joints = attributes.joints;
            if (!joints || joints->type != cgltf_type_vec4 || joints->normalized ||
                (joints->component_type != cgltf_component_type_r_8u &&
                 joints->component_type != cgltf_component_type_r_16u) ||
                joints->count != positions->count)
            {
                error = "JOINTS_0 must be a matching non-normalized UNSIGNED_BYTE or UNSIGNED_SHORT VEC4 accessor";
                return false;
            }
            const cgltf_accessor* weights = attributes.weights;
            if (!weights || weights->type != cgltf_type_vec4 || weights->count != positions->count ||
                !((weights->component_type == cgltf_component_type_r_32f && !weights->normalized) ||
                  ((weights->component_type == cgltf_component_type_r_8u ||
                    weights->component_type == cgltf_component_type_r_16u) &&
                   weights->normalized)))
            {
                error = "WEIGHTS_0 must be a matching FLOAT or normalized UNSIGNED_BYTE/UNSIGNED_SHORT VEC4 accessor";
                return false;
            }
            return true;
        }

        bool ReadInfluences(const cgltf_accessor& joints, const std::vector<float>& weightValues, size_t vertex,
                            const std::vector<uint32_t>& boneOfJoint, GLTFSkinnedVertex& output, std::string& error)
        {
            cgltf_uint jointValues[4] = {};
            if (!cgltf_accessor_read_uint(&joints, vertex, jointValues, 4))
            {
                error = "failed to read JOINTS_0";
                return false;
            }

            float sum = 0.0f;
            for (size_t k = 0; k < 4; ++k)
            {
                if (jointValues[k] >= boneOfJoint.size())
                {
                    error = "references joint " + std::to_string(jointValues[k]) + " but the skin has " +
                            std::to_string(boneOfJoint.size()) + " joints";
                    return false;
                }
                const float weight = weightValues[vertex * 4 + k];
                if (weight < 0.0f)
                {
                    error = "has negative weight " + std::to_string(weight);
                    return false;
                }
                output.joints[k] = boneOfJoint[jointValues[k]];
                output.weights[k] = weight;
                sum += weight;
            }

            if (sum == 0.0f)
            {
                error = "has zero-sum weights";
                return false;
            }
            if (std::abs(sum - 1.0f) > kGLTFSkinWeightSumTolerance)
            {
                error = "has weights summing to " + std::to_string(sum) + ", outside 1 +/- " +
                        std::to_string(kGLTFSkinWeightSumTolerance);
                return false;
            }
            for (float& weight : output.weights)
            {
                weight /= sum;
            }
            return true;
        }

        bool AppendSkinnedPrimitive(const cgltf_primitive& primitive, const std::string& label,
                                    const std::vector<uint32_t>& boneOfJoint, GLTFSkinnedMeshData& meshData,
                                    std::string& error)
        {
            SkinAttributes attributes;
            if (!CollectAttributes(primitive, attributes, error))
            {
                error = label + ": " + error;
                return false;
            }

            const size_t vertexCount = static_cast<size_t>(attributes.positions->count);
            const size_t indexCount = primitive.indices ? static_cast<size_t>(primitive.indices->count) : vertexCount;
            if (vertexCount > GLTF::kMaxVertices - meshData.vertices.size() ||
                indexCount > GLTF::kMaxIndices - meshData.indices.size())
            {
                error = label + ": skinned mesh exceeds import limits";
                return false;
            }
            if (indexCount == 0 || indexCount % 3 != 0)
            {
                error = label + ": triangle primitive index count must be nonzero and divisible by three";
                return false;
            }
            if (primitive.indices && (primitive.indices->type != cgltf_type_scalar || primitive.indices->normalized ||
                                      (primitive.indices->component_type != cgltf_component_type_r_8u &&
                                       primitive.indices->component_type != cgltf_component_type_r_16u &&
                                       primitive.indices->component_type != cgltf_component_type_r_32u)))
            {
                error = label + ": indices must be an unsigned scalar accessor";
                return false;
            }

            std::vector<float> positionValues;
            std::vector<float> normalValues;
            std::vector<float> texCoordValues;
            std::vector<float> weightValues;
            if (!GLTF::UnpackFloats(*attributes.positions, 3, positionValues, error) ||
                !GLTF::UnpackFloats(*attributes.normals, 3, normalValues, error) ||
                (attributes.texCoords && !GLTF::UnpackFloats(*attributes.texCoords, 2, texCoordValues, error)) ||
                !GLTF::UnpackFloats(*attributes.weights, 4, weightValues, error))
            {
                error = label + ": " + error;
                return false;
            }

            const size_t vertexStart = meshData.vertices.size();
            const size_t indexStart = meshData.indices.size();
            meshData.vertices.reserve(vertexStart + vertexCount);
            meshData.indices.reserve(indexStart + indexCount);
            for (size_t i = 0; i < vertexCount; ++i)
            {
                GLTFSkinnedVertex vertex{};
                vertex.position = {positionValues[i * 3], positionValues[i * 3 + 1], positionValues[i * 3 + 2]};
                vertex.normal = {normalValues[i * 3], normalValues[i * 3 + 1], normalValues[i * 3 + 2]};
                if (attributes.texCoords)
                {
                    vertex.texCoord = {texCoordValues[i * 2], texCoordValues[i * 2 + 1]};
                }
                if (!ReadInfluences(*attributes.joints, weightValues, i, boneOfJoint, vertex, error))
                {
                    error = label + " vertex " + std::to_string(i) + " " + error;
                    return false;
                }
                meshData.vertices.push_back(vertex);
            }

            for (size_t i = 0; i < indexCount; ++i)
            {
                cgltf_uint localIndex = static_cast<cgltf_uint>(i);
                if (primitive.indices &&
                    (!cgltf_accessor_read_uint(primitive.indices, i, &localIndex, 1) || localIndex >= vertexCount))
                {
                    error = label + ": index accessor contains an invalid vertex index";
                    return false;
                }
                meshData.indices.push_back(static_cast<uint32_t>(vertexStart + localIndex));
            }
            meshData.primitives.push_back({static_cast<uint32_t>(indexStart), static_cast<uint32_t>(indexCount)});
            return true;
        }

        bool LoadSkinnedDocument(GLTF::Document& document, GLTFSkinnedMeshData& meshData, std::string& error)
        {
            std::vector<uint32_t> boneOfJoint;
            if (!GLTF::LoadSkinSkeleton(document, meshData.skeleton, boneOfJoint, error))
            {
                return false;
            }

            const cgltf_data& data = *document.data;

            for (cgltf_size meshIndex = 0; meshIndex < data.meshes_count; ++meshIndex)
            {
                const cgltf_mesh& mesh = data.meshes[meshIndex];
                for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh.primitives_count; ++primitiveIndex)
                {
                    const std::string label =
                        "mesh " + std::to_string(meshIndex) + " primitive " + std::to_string(primitiveIndex);
                    if (!AppendSkinnedPrimitive(mesh.primitives[primitiveIndex], label, boneOfJoint, meshData, error))
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    } // namespace
#endif

    bool LoadGLTFSkinnedMesh(const std::filesystem::path& path, GLTFSkinnedMeshData& meshData, std::string& error)
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

        bool loaded = false;
        try
        {
            loaded = LoadSkinnedDocument(document, meshData, error);
        }
        catch (const std::bad_alloc&)
        {
            error = "out of memory while loading glTF skinned mesh";
        }

        if (loaded && (meshData.vertices.empty() || meshData.indices.empty() || meshData.skeleton.bones.empty()))
        {
            loaded = false;
            error = "glTF contains no supported skinned triangle geometry";
        }
        if (!loaded)
        {
            meshData = {};
        }
        return loaded;
#endif
    }

    bool GLTFFileHasSkin(const std::filesystem::path& path, bool& hasSkin, std::string& error)
    {
        hasSkin = false;
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
        hasSkin = document.data->skins_count > 0;
        return true;
#endif
    }
} // namespace Spark::Graphics::Detail
