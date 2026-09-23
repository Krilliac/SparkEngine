/**
 * @file EditorAssetReference.h
 * @brief Validation for typed, project-relative editor asset references.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace SparkEditor
{
    enum class EditorAssetKind
    {
        Mesh,
        Material,
    };

    /**
     * Validate the serialized reference carried by editor asset drag/drop.
     *
     * This is intentionally independent of the UI producer. ImGui payloads
     * are untrusted input at every target, so the consumer must enforce the
     * same project-relative and typed contract even if another producer is
     * added later.
     */
    inline bool IsValidEditorAssetReference(std::string_view reference, EditorAssetKind kind)
    {
        if (reference.size() < 8 || reference.substr(0, 7) != "Assets/" || reference.find('\0') != std::string_view::npos ||
            reference.find('\\') != std::string_view::npos || reference.find(':') != std::string_view::npos)
            return false;

        std::string_view path = reference.substr(7);
        if (path.empty() || path.front() == '/' || path.back() == '/')
            return false;

        size_t segmentStart = 0;
        while (segmentStart <= path.size())
        {
            const size_t separator = path.find('/', segmentStart);
            const std::string_view segment = path.substr(segmentStart, separator == std::string_view::npos
                                                                     ? std::string_view::npos
                                                                     : separator - segmentStart);
            if (segment.empty() || segment == "." || segment == "..")
                return false;
            if (separator == std::string_view::npos)
                break;
            segmentStart = separator + 1;
        }

        const size_t dot = path.find_last_of('.');
        const size_t slash = path.find_last_of('/');
        if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
            return false;

        std::string extension(path.substr(dot));
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (kind == EditorAssetKind::Mesh)
            return extension == ".obj" || extension == ".fbx" || extension == ".gltf" || extension == ".glb" ||
                   extension == ".mesh";
        return extension == ".mat" || extension == ".material" || extension == ".json";
    }
} // namespace SparkEditor
