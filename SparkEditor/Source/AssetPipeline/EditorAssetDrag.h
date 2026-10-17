/**
 * @file EditorAssetDrag.h
 * @brief ImGui-free producer and consumer halves of the editor asset drag/drop payload.
 *
 * The Asset Browser produces the payload and the inspector asset slots consume
 * it. Both sides live here so the wire contract (payload type name, byte
 * layout, and typed validation) has exactly one definition and can be tested
 * without an ImGui context.
 */

#pragma once

// EditorAssetReference.h uses std::string without including <string>; it must
// arrive first because this header is the first include of EditorAssetDrag.cpp.
#include <string>

#include "EditorAssetReference.h"

#include <filesystem>

namespace SparkEditor
{
    /// ImGui drag/drop payload type carrying one NUL-terminated Assets/... reference.
    inline constexpr const char* kAssetDragPayloadType = "SPARK_ASSET_PATH";

    /**
     * Build the project-relative reference carried by an asset drag.
     *
     * Returns an `Assets/<relative>` reference with forward slashes when
     * @p assetPath lies inside @p assetsRoot (after symlink resolution) and is
     * assignable as a mesh or material. Returns an empty string otherwise, so
     * no drag payload is offered. Never emits an absolute filesystem path:
     * saved scenes must stay machine-independent and inside the runtime's
     * project boundary.
     */
    std::string MakeAssetDragReference(const std::filesystem::path& assetPath, const std::filesystem::path& assetsRoot);

    /**
     * Decode and validate a received drag payload for a slot of @p kind.
     *
     * Payload bytes are untrusted: the size must be 2..4096, the only NUL must
     * be the final byte, and the text must be a valid reference of @p kind.
     * On success @p reference holds the reference without its terminator.
     */
    bool DecodeAssetDragPayload(const void* data, int size, EditorAssetKind kind, std::string& reference);
} // namespace SparkEditor
