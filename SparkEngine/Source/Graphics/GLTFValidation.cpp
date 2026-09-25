/**
 * @file GLTFValidation.cpp
 * @brief Root-confined cgltf parsing and pre-load structural validation for glTF mesh imports.
 */

#include "GLTFValidation.h"

#if SPARK_HAS_CGLTF

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <system_error>

namespace Spark::Graphics::Detail::GLTF
{
    namespace
    {
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
                const auto* root = static_cast<const std::filesystem::path*>(fileOptions->user_data);
                std::filesystem::path filePath = std::filesystem::u8path(path);
                if (!root || !IsWithinRoot(filePath, *root))
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
    } // namespace

    bool ValidateDocumentStructure(const cgltf_data& data, std::string& error)
    {
        if (data.extensions_required_count != 0)
        {
            error = "required glTF extensions are unsupported";
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
            if (buffer.size == 0 || buffer.size > kMaxSourceBytes || AddWouldOverflow(totalBufferBytes, buffer.size))
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
            if (view.stride != 0 && view.stride % 4 != 0)
            {
                error = "buffer view byte stride is not 4-byte aligned";
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

    bool ParseDocument(const std::filesystem::path& path, Document& document, std::string& error)
    {
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

        document.root = canonicalPath.parent_path();
        document.options = {};
        document.options.file.read = ReadFile;
        document.options.file.release = ReleaseFile;
        document.options.file.user_data = &document.root;

        const std::u8string utf8 = canonicalPath.u8string();
        document.sourcePath.assign(utf8.begin(), utf8.end());
        cgltf_data* rawData = nullptr;
        const cgltf_result result = cgltf_parse_file(&document.options, document.sourcePath.c_str(), &rawData);
        if (result != cgltf_result_success)
        {
            error = std::string("cgltf parse failed: ") + ResultName(result);
            return false;
        }
        document.data.reset(rawData);
        return true;
    }

    bool LoadAndValidateBuffers(Document& document, std::string& error)
    {
        cgltf_result result = cgltf_load_buffers(&document.options, document.data.get(), document.sourcePath.c_str());
        if (result != cgltf_result_success)
        {
            error = std::string("cgltf buffer load failed: ") + ResultName(result);
            return false;
        }
        for (cgltf_size i = 0; i < document.data->buffers_count; ++i)
        {
            if (!document.data->buffers[i].data)
            {
                error = "glTF buffer data is unavailable";
                return false;
            }
        }

        result = cgltf_validate(document.data.get());
        if (result != cgltf_result_success)
        {
            error = std::string("cgltf validation failed: ") + ResultName(result);
            return false;
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
        if (cgltf_accessor_unpack_floats(&accessor, values.data(), floatCount) != floatCount)
        {
            error = "failed to unpack finite accessor values";
            return false;
        }
        for (float value : values)
        {
            if (!std::isfinite(value))
            {
                error = "failed to unpack finite accessor values";
                return false;
            }
        }
        return true;
    }
} // namespace Spark::Graphics::Detail::GLTF

#endif
