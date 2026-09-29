/**
 * @file EditorAssetDrag.cpp
 * @brief ImGui-free producer and consumer halves of the editor asset drag/drop payload.
 */

#include "EditorAssetDrag.h"

#include <algorithm>
#include <cstddef>
#include <system_error>
#include <utility>

namespace SparkEditor
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr int kMinPayloadBytes = 2;
        constexpr int kMaxPayloadBytes = 4096;

        std::string GenericUtf8(const fs::path& path)
        {
            const auto utf8 = path.generic_u8string();
            return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        }
    } // namespace

    std::string MakeAssetDragReference(const fs::path& assetPath, const fs::path& assetsRoot)
    {
        std::error_code ec;
        const fs::path relative = fs::relative(assetPath, assetsRoot, ec);
        if (ec || relative.empty() || relative.is_absolute())
        {
            return {};
        }

        for (const auto& component : relative)
        {
            if (component == "." || component == "..")
            {
                return {};
            }
        }

        std::string reference;
        try
        {
            reference = "Assets/" + GenericUtf8(relative);
        }
        catch (...)
        {
            return {};
        }
        std::replace(reference.begin(), reference.end(), '\\', '/');
        if (!IsValidEditorAssetReference(reference, EditorAssetKind::Mesh) &&
            !IsValidEditorAssetReference(reference, EditorAssetKind::Material))
        {
            return {};
        }
        return reference;
    }

    bool DecodeAssetDragPayload(const void* data, int size, EditorAssetKind kind, std::string& reference)
    {
        if (data == nullptr || size < kMinPayloadBytes || size > kMaxPayloadBytes)
        {
            return false;
        }

        const auto* bytes = static_cast<const char*>(data);
        const auto* terminator = std::find(bytes, bytes + size, '\0');
        if (terminator != bytes + size - 1)
        {
            return false;
        }

        std::string decoded(bytes, static_cast<std::size_t>(size - 1));
        if (!IsValidEditorAssetReference(decoded, kind))
        {
            return false;
        }
        reference = std::move(decoded);
        return true;
    }
} // namespace SparkEditor
