/**
 * @file FuzzEditorSceneJsonProduction.cpp
 * @brief libc++-compiled production adapter for the editor JSON scene libFuzzer harness.
 *
 * SceneSerializer::LoadJSON reads a .sparkscene/.json/.scenejson file and hands its bytes to
 * SparkEditor::DecodeSceneJSONDocument; the adapter hands it the fuzz bytes the same way,
 * over a live, non-default scene. A violated contract aborts so libFuzzer records a crash:
 *  - the return value, result.success and the presence of an error message agree, and
 *    decoding the same bytes twice gives the same verdict, error and scene;
 *  - a rejected document leaves the caller's scene exactly as it was;
 *  - an accepted document decodes to a scene the editor can save again: SceneFile::Validate
 *    passes, the header carries the current magic and version and counts equal to the
 *    arrays, both header strings end inside their buffers, every float is finite, the sky
 *    and projection enums are in range, marker components carry no payload and every other
 *    payload encodes through the production codec to finite floats and NUL-free, bounded,
 *    UTF-8 strings (the checks SceneSerializer::SaveJSON makes before it writes a file).
 */

#include "FuzzEditorSceneJsonProduction.h"

#include "FuzzEditorSceneFingerprint.h"
#include "SceneSystem/SceneJSONReader.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEditorSceneJson: DecodeSceneJSONDocument violated: %s\n", what);
        std::abort();
    }

    bool Finite3(const DirectX::XMFLOAT3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    bool Finite4(const DirectX::XMFLOAT4& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
    }

    template <std::size_t Size> bool Terminated(const char (&text)[Size])
    {
        return std::find(text, text + Size, '\0') != text + Size;
    }

    void CheckAccepted(const SparkEditor::SceneFile& scene)
    {
        using namespace SparkEditor;
        const SceneHeader& header = scene.header;
        if (header.magic != SCENE_FILE_MAGIC || header.version != SCENE_FILE_VERSION)
        {
            InvariantFailure("an accepted scene does not carry the current magic and version");
        }
        if (header.objectCount != scene.objects.size() || header.componentCount != scene.components.size() ||
            header.assetReferenceCount != scene.assetReferences.size())
        {
            InvariantFailure("an accepted scene's header counts differ from its arrays");
        }
        if (!Terminated(header.sceneName) || !Terminated(header.description))
        {
            InvariantFailure("an accepted header string is not terminated inside its buffer");
        }

        std::vector<std::string> errors;
        if (!scene.Validate(errors) || !errors.empty())
        {
            InvariantFailure("an accepted scene fails SceneFile::Validate");
        }

        if (!Finite3(header.gravity) || !Finite4(header.ambientColor) || !std::isfinite(header.ambientIntensity))
        {
            InvariantFailure("an accepted header float is not finite");
        }
        for (const SceneObject& object : scene.objects)
        {
            if (!Finite3(object.transform.position) || !Finite4(object.transform.rotation) ||
                !Finite3(object.transform.scale))
            {
                InvariantFailure("an accepted transform float is not finite");
            }
            if (!IsValidSceneUTF8(object.name) || !IsValidSceneUTF8(object.tag))
            {
                InvariantFailure("an accepted object name or tag is not UTF-8");
            }
        }
        const EnvironmentSettings& env = scene.environment;
        if (!Finite4(env.skyColor) || !Finite4(env.horizonColor) || !Finite4(env.fogColor) ||
            !std::isfinite(env.fogDensity) || !std::isfinite(env.fogStart) || !std::isfinite(env.fogEnd) ||
            !Finite3(env.windDirection) || !std::isfinite(env.windStrength) || !std::isfinite(env.windTurbulence) ||
            !std::isfinite(env.bloomIntensity) || !std::isfinite(env.bloomThreshold) || !std::isfinite(env.exposure) ||
            !std::isfinite(env.gamma))
        {
            InvariantFailure("an accepted environment float is not finite");
        }
        if (static_cast<int>(env.skyType) < EnvironmentSettings::SOLID_COLOR ||
            static_cast<int>(env.skyType) > EnvironmentSettings::PROCEDURAL || !IsValidSceneUTF8(env.skyboxAssetPath))
        {
            InvariantFailure("an accepted environment has an out-of-range sky type or a non-UTF-8 skybox path");
        }
        const Camera& cam = scene.defaultCamera;
        if (!std::isfinite(cam.fieldOfView) || !std::isfinite(cam.orthographicSize) || !std::isfinite(cam.nearPlane) ||
            !std::isfinite(cam.farPlane) || !Finite4(cam.clearColor))
        {
            InvariantFailure("an accepted camera float is not finite");
        }
        if (static_cast<int>(cam.projectionType) < Camera::PERSPECTIVE ||
            static_cast<int>(cam.projectionType) > Camera::ORTHOGRAPHIC)
        {
            InvariantFailure("an accepted camera has an out-of-range projection type");
        }
        for (const AssetReference& reference : scene.assetReferences)
        {
            bool text = IsValidSceneUTF8(reference.assetPath) && IsValidSceneUTF8(reference.assetType) &&
                        IsValidSceneUTF8(reference.checksum);
            for (const std::string& dependency : reference.dependencies)
            {
                text = text && IsValidSceneUTF8(dependency);
            }
            if (!text)
            {
                InvariantFailure("an accepted asset reference string is not UTF-8");
            }
        }

        for (const Component& component : scene.components)
        {
            const bool marker =
                component.type == ComponentType::TRANSFORM || component.type == ComponentType::SPRITE_ANIMATOR;
            if (marker && component.HasData())
            {
                InvariantFailure("an accepted marker-only component carries a payload");
            }
            if (!marker && (!HasSceneComponentPayloadCodec(component.type) || !component.HasData()))
            {
                InvariantFailure("an accepted component has no registered payload");
            }
        }
        bool persistable = true;
        (void)SparkFuzzEditorScene::Fingerprint(scene, &IsValidSceneUTF8, &persistable);
        if (!persistable)
        {
            InvariantFailure("an accepted component payload holds a value the scene writer refuses");
        }
    }
} // namespace

extern "C" int SparkFuzzDecodeEditorSceneJson(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string content = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);

    static const SparkEditor::SceneFile sentinel = SparkFuzzEditorScene::SentinelScene();
    static const std::string sentinelPrint =
        SparkFuzzEditorScene::Fingerprint(sentinel, &SparkEditor::IsValidSceneUTF8);

    SparkEditor::SceneFile scene = sentinel;
    SparkEditor::SerializationResult result;
    const bool accepted = SparkEditor::DecodeSceneJSONDocument(content, scene, result);
    const std::string print = SparkFuzzEditorScene::Fingerprint(scene, &SparkEditor::IsValidSceneUTF8);

    SparkEditor::SceneFile again = sentinel;
    SparkEditor::SerializationResult againResult;
    if (SparkEditor::DecodeSceneJSONDocument(content, again, againResult) != accepted ||
        againResult.errorMessage != result.errorMessage ||
        SparkFuzzEditorScene::Fingerprint(again, &SparkEditor::IsValidSceneUTF8) != print)
    {
        InvariantFailure("decoding the same bytes twice gave different results");
    }
    if (accepted != result.success)
    {
        InvariantFailure("the return value disagrees with result.success");
    }

    if (!accepted)
    {
        if (print != sentinelPrint)
        {
            InvariantFailure("a rejected document changed the caller's scene");
        }
        if (result.errorMessage.empty())
        {
            InvariantFailure("a rejected document carries no error message");
        }
        return 0;
    }
    if (!result.errorMessage.empty())
    {
        InvariantFailure("an accepted document reports an error");
    }
    if (result.bytesProcessed != content.size())
    {
        InvariantFailure("an accepted document reports a different byte count");
    }
    CheckAccepted(scene);
    return 0;
}
