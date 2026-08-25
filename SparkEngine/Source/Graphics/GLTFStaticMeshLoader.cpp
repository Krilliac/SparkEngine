/**
 * @file GLTFStaticMeshLoader.cpp
 * @brief Validated CPU-side glTF 2.0 static-mesh loading via cgltf.
 */

#include "GLTFStaticMeshLoader.h"

#if SPARK_HAS_CGLTF
#include <cgltf.h>
#endif

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <system_error>

namespace Spark::Graphics::Detail
{
#if SPARK_HAS_CGLTF
    namespace
    {
        constexpr cgltf_size kMaxSourceBytes = 1024ull * 1024ull * 1024ull;
        constexpr size_t kMaxVertices = 16ull * 1024ull * 1024ull;
        constexpr size_t kMaxIndices = kMaxVertices * 3ull;

        struct FileReadContext
        {
            std::filesystem::path root;
        };

        bool IsWithinRoot(const std::filesystem::path& path, const std::filesystem::path& root)
        {
            std::error_code ec;
            const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, ec);
            if (ec)
            {
                return false;
            }

            const std::filesystem::path relative = std::filesystem::relative(canonicalPath, root, ec);
            if (ec || relative.empty() || relative.is_absolute())
            {
                return false;
            }

            for (const auto& component : relative)
            {
                if (component == "..")
                {
                    return false;
                }
            }
            return true;
        }

        cgltf_result ReadFile(const cgltf_memory_options* memoryOptions, const cgltf_file_options* fileOptions,
                              const char* path, cgltf_size* size, void** data)
        {
            if (!memoryOptions || !fileOptions || !path || !size || !data)
            {
                return cgltf_result_invalid_options;
            }

            try
            {
                const auto* context = static_cast<const FileReadContext*>(fileOptions->user_data);
                std::filesystem::path filePath = std::filesystem::u8path(path);
                if (!context || !IsWithinRoot(filePath, context->root))
                {
                    return cgltf_result_io_error;
                }

                std::ifstream file(filePath, std::ios::binary | std::ios::ate);
                if (!file)
                {
                    return cgltf_result_file_not_found;
                }

                const std::streamoff end = file.tellg();
                if (end <= 0 || static_cast<uintmax_t>(end) > kMaxSourceBytes)
                {
                    return cgltf_result_data_too_short;
                }

                const cgltf_size available = static_cast<cgltf_size>(end);
                const cgltf_size requested = *size == 0 ? available : *size;
                if (requested == 0 || requested > available || requested > kMaxSourceBytes)
                {
                    return cgltf_result_data_too_short;
                }

                void* fileData = memoryOptions->alloc_func
                                     ? memoryOptions->alloc_func(memoryOptions->user_data, requested)
                                     : std::malloc(requested);
                if (!fileData)
                {
                    return cgltf_result_out_of_memory;
                }

                file.seekg(0, std::ios::beg);
                file.read(static_cast<char*>(fileData), static_cast<std::streamsize>(requested));
                if (!file || static_cast<cgltf_size>(file.gcount()) != requested)
                {
                    if (memoryOptions->free_func)
                    {
                        memoryOptions->free_func(memoryOptions->user_data, fileData);
                    }
                    else
                    {
                        std::free(fileData);
                    }
                    return cgltf_result_io_error;
                }

                *size = requested;
                *data = fileData;
                return cgltf_result_success;
            }
            catch (const std::bad_alloc&)
            {
                return cgltf_result_out_of_memory;
            }
            catch (...)
            {
                return cgltf_result_io_error;
            }
        }

        void ReleaseFile(const cgltf_memory_options* memoryOptions, const cgltf_file_options*, void* data)
        {
            if (memoryOptions && memoryOptions->free_func)
            {
                memoryOptions->free_func(memoryOptions->user_data, data);
            }
            else
            {
                std::free(data);
            }
        }

        const char* ResultName(cgltf_result result)
        {
            switch (result)
            {
            case cgltf_result_success:
                return "success";
            case cgltf_result_data_too_short:
                return "data too short";
            case cgltf_result_unknown_format:
                return "unknown format";
            case cgltf_result_invalid_json:
                return "invalid JSON";
            case cgltf_result_invalid_gltf:
                return "invalid glTF";
            case cgltf_result_invalid_options:
                return "invalid options";
            case cgltf_result_file_not_found:
                return "file not found";
            case cgltf_result_io_error:
                return "I/O error";
            case cgltf_result_out_of_memory:
                return "out of memory";
            case cgltf_result_legacy_gltf:
                return "legacy glTF";
            default:
                return "unknown cgltf error";
            }
        }

        bool AddWouldOverflow(cgltf_size left, cgltf_size right)
        {
            return right > std::numeric_limits<cgltf_size>::max() - left;
        }

        bool MultiplyWouldOverflow(cgltf_size left, cgltf_size right)
        {
            return left != 0 && right > std::numeric_limits<cgltf_size>::max() / left;
        }

        bool ValidateAccessorBounds(const cgltf_accessor& accessor, std::string& error)
        {
            if (accessor.count == 0)
            {
                error = "zero-count accessors are unsupported";
                return false;
            }
            if (accessor.is_sparse)
            {
                error = "sparse accessors are unsupported";
                return false;
            }
            if (accessor.extensions_count != 0)
            {
                error = "accessor extensions are unsupported";
                return false;
            }
            if (!accessor.buffer_view || !accessor.buffer_view->buffer)
            {
                error = "accessor has no buffer view";
                return false;
            }

            const cgltf_buffer_view& view = *accessor.buffer_view;
            if (view.has_meshopt_compression || view.extensions_count != 0 || view.data != nullptr)
            {
                error = "compressed or extended buffer views are unsupported";
                return false;
            }

            const cgltf_size elementSize = cgltf_calc_size(accessor.type, accessor.component_type);
            if (elementSize == 0 || accessor.stride < elementSize)
            {
                error = "invalid accessor element size or stride";
                return false;
            }

            const cgltf_size remainingElements = accessor.count - 1;
            if (MultiplyWouldOverflow(accessor.stride, remainingElements))
            {
                error = "accessor stride calculation overflow";
                return false;
            }
            const cgltf_size tail = accessor.stride * remainingElements;
            if (AddWouldOverflow(accessor.offset, tail) || AddWouldOverflow(accessor.offset + tail, elementSize))
            {
                error = "accessor range calculation overflow";
                return false;
            }
            if (accessor.offset + tail + elementSize > view.size)
            {
                error = "accessor range exceeds its buffer view";
                return false;
            }
            return true;
        }

        bool ValidateDocumentBeforeCgltf(const cgltf_data& data, std::string& error)
        {
            if (data.extensions_required_count != 0)
            {
                error = "required glTF extensions are unsupported";
                return false;
            }
            if (data.skins_count != 0 || data.animations_count != 0)
            {
                error = "skins and animations are outside the static-mesh subset";
                return false;
            }
            if (data.meshes_count == 0)
            {
                error = "glTF contains no meshes";
                return false;
            }

            cgltf_size totalBufferBytes = 0;
            for (cgltf_size i = 0; i < data.buffers_count; ++i)
            {
                const cgltf_buffer& buffer = data.buffers[i];
                if (buffer.size == 0 || buffer.size > kMaxSourceBytes ||
                    AddWouldOverflow(totalBufferBytes, buffer.size))
                {
                    error = "invalid or oversized glTF buffer";
                    return false;
                }
                totalBufferBytes += buffer.size;
                if (totalBufferBytes > kMaxSourceBytes)
                {
                    error = "combined glTF buffers exceed the import limit";
                    return false;
                }
            }

            for (cgltf_size i = 0; i < data.buffer_views_count; ++i)
            {
                const cgltf_buffer_view& view = data.buffer_views[i];
                if (!view.buffer || view.has_meshopt_compression || view.extensions_count != 0 || view.data != nullptr)
                {
                    error = "invalid, compressed, or extended buffer view";
                    return false;
                }
                if (AddWouldOverflow(view.offset, view.size) || view.offset + view.size > view.buffer->size)
                {
                    error = "buffer view range exceeds its buffer";
                    return false;
                }
            }

            for (cgltf_size i = 0; i < data.accessors_count; ++i)
            {
                if (!ValidateAccessorBounds(data.accessors[i], error))
                {
                    return false;
                }
            }

            for (cgltf_size meshIndex = 0; meshIndex < data.meshes_count; ++meshIndex)
            {
                const cgltf_mesh& mesh = data.meshes[meshIndex];
                if (mesh.weights_count != 0 || mesh.target_names_count != 0 || mesh.extensions_count != 0)
                {
                    error = "morph targets or mesh extensions are unsupported";
                    return false;
                }
                for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh.primitives_count; ++primitiveIndex)
                {
                    const cgltf_primitive& primitive = mesh.primitives[primitiveIndex];
                    if (primitive.type != cgltf_primitive_type_triangles || primitive.targets_count != 0 ||
                        primitive.has_draco_mesh_compression || primitive.extensions_count != 0)
                    {
                        error = "only unextended static triangle primitives are supported";
                        return false;
                    }
                }
            }
            return true;
        }

        bool IsFinite(const std::vector<float>& values)
        {
            for (float value : values)
            {
                if (!std::isfinite(value))
                {
                    return false;
                }
            }
            return true;
        }

        bool UnpackFloats(const cgltf_accessor& accessor, cgltf_size components, std::vector<float>& values,
                          std::string& error)
        {
            if (MultiplyWouldOverflow(accessor.count, components))
            {
                error = "accessor float count overflow";
                return false;
            }
            const cgltf_size floatCount = accessor.count * components;
            values.resize(static_cast<size_t>(floatCount));
            if (cgltf_accessor_unpack_floats(&accessor, values.data(), floatCount) != floatCount || !IsFinite(values))
            {
                error = "failed to unpack finite accessor values";
                return false;
            }
            return true;
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
        if (path.empty())
        {
            error = "glTF path is empty";
            return false;
        }

        std::error_code ec;
        const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, ec);
        if (ec || !std::filesystem::is_regular_file(canonicalPath, ec))
        {
            error = "glTF source is not a regular file";
            return false;
        }

        FileReadContext fileContext{canonicalPath.parent_path()};
        cgltf_options options{};
        options.file.read = ReadFile;
        options.file.release = ReleaseFile;
        options.file.user_data = &fileContext;

        const std::u8string utf8 = canonicalPath.u8string();
        const std::string sourcePath(utf8.begin(), utf8.end());
        cgltf_data* rawData = nullptr;
        cgltf_result result = cgltf_parse_file(&options, sourcePath.c_str(), &rawData);
        if (result != cgltf_result_success)
        {
            error = std::string("cgltf parse failed: ") + ResultName(result);
            return false;
        }
        const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(rawData, &cgltf_free);

        try
        {
            if (!ValidateDocumentBeforeCgltf(*data, error))
            {
                return false;
            }

            result = cgltf_load_buffers(&options, data.get(), sourcePath.c_str());
            if (result != cgltf_result_success)
            {
                error = std::string("cgltf buffer load failed: ") + ResultName(result);
                return false;
            }
            for (cgltf_size i = 0; i < data->buffers_count; ++i)
            {
                if (!data->buffers[i].data)
                {
                    error = "glTF buffer data is unavailable";
                    return false;
                }
            }

            result = cgltf_validate(data.get());
            if (result != cgltf_result_success)
            {
                error = std::string("cgltf validation failed: ") + ResultName(result);
                return false;
            }

            for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex)
            {
                const cgltf_mesh& mesh = data->meshes[meshIndex];
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
