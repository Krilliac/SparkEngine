/**
 * @file GLTFValidation.h
 * @brief Internal cgltf document loading and bounds validation shared by the glTF mesh loaders.
 *
 * Both the static and the skinned glTF loaders parse untrusted files. This header owns the
 * parts they must agree on: root-confined file reads, source-size limits, and the structural
 * pre-validation of buffers, buffer views, accessors and primitives that runs before cgltf
 * touches any binary payload. Only engine .cpp files include it; it is not a public API.
 */

#pragma once

#if SPARK_HAS_CGLTF

#include <cgltf.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Graphics::Detail::GLTF
{
    /// Largest accepted .gltf/.glb file and largest combined buffer payload (1 GiB).
    inline constexpr cgltf_size kMaxSourceBytes = 1024ull * 1024ull * 1024ull;
    /// Largest accepted vertex count across all primitives of one import.
    inline constexpr size_t kMaxVertices = 16ull * 1024ull * 1024ull;
    /// Largest accepted index count across all primitives of one import.
    inline constexpr size_t kMaxIndices = kMaxVertices * 3ull;

    /**
     * @brief A parsed cgltf document plus the file-read context its callbacks reference.
     *
     * The cgltf options hold a pointer to @c root, so a Document is neither copyable nor movable.
     */
    struct Document
    {
        Document() = default;
        Document(const Document&) = delete;
        Document& operator=(const Document&) = delete;

        std::filesystem::path root;
        std::string sourcePath;
        cgltf_options options{};
        std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data{nullptr, &cgltf_free};
    };

    /**
     * @brief Parse the glTF JSON (and GLB container) without loading external buffers.
     *
     * External URIs are confined to the source file's directory and every read is size-limited.
     * @return false with a diagnostic in @p error on failure.
     */
    bool ParseDocument(const std::filesystem::path& path, Document& document, std::string& error);

    /**
     * @brief Structural checks that must pass before cgltf_load_buffers runs.
     *
     * Rejects required extensions, empty mesh lists, oversized buffers, compressed or unaligned
     * buffer views, sparse or out-of-range accessors, morph targets, and non-triangle primitives.
     */
    bool ValidateDocumentStructure(const cgltf_data& data, std::string& error);

    /**
     * @brief Load buffer payloads and run cgltf_validate on a pre-validated document.
     */
    bool LoadAndValidateBuffers(Document& document, std::string& error);

    /**
     * @brief Unpack an accessor into @p components floats per element, rejecting non-finite values.
     */
    bool UnpackFloats(const cgltf_accessor& accessor, cgltf_size components, std::vector<float>& values,
                      std::string& error);
} // namespace Spark::Graphics::Detail::GLTF

#endif
