/**
 * @file FuzzEditorSceneFingerprint.h
 * @brief Canonical field-by-field rendering of an editor SceneFile, shared by the editor scene
 *        fuzz adapters (SparkFuzzEditorSceneJson, SparkFuzzEditorSceneLoad).
 *
 * Two scenes are the same scene exactly when their fingerprints are equal: every header,
 * object, transform, component payload, asset reference, environment and camera field is
 * rendered, floats by their bit pattern and strings with their length, so -0.0 differs from
 * 0.0 and a string cannot run into its neighbour. Component payloads are rendered through
 * the production codec's EncodeSceneComponentPayload.
 */

#pragma once

#include "SceneSystem/SceneComponentCodec.h"
#include "SceneSystem/SceneFile.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace SparkFuzzEditorScene
{
    inline void Bits(std::string& out, float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        out += std::to_string(bits);
        out += ',';
    }

    inline void Text(std::string& out, std::string_view value)
    {
        out += std::to_string(value.size());
        out += ':';
        out.append(value.data(), value.size());
        out += ',';
    }

    inline void Number(std::string& out, std::uint64_t value)
    {
        out += std::to_string(value);
        out += ',';
    }

    inline void Float3(std::string& out, const DirectX::XMFLOAT3& value)
    {
        Bits(out, value.x);
        Bits(out, value.y);
        Bits(out, value.z);
    }

    inline void Float4(std::string& out, const DirectX::XMFLOAT4& value)
    {
        Bits(out, value.x);
        Bits(out, value.y);
        Bits(out, value.z);
        Bits(out, value.w);
    }

    /// Records every field a component codec writes; reports whether each value is one the
    /// JSON writer can persist (finite floats; strings without NUL, at most 1 MiB, valid UTF-8
    /// per @p utf8).
    class RecordingWriter final : public SparkEditor::SceneComponentFieldWriter
    {
      public:
        using Utf8Check = bool (*)(std::string_view);

        explicit RecordingWriter(std::string& out, Utf8Check utf8) : m_out(out), m_utf8(utf8) {}

        bool WriteBool(std::string_view name, bool value) override
        {
            Text(m_out, name);
            Number(m_out, value ? 1u : 0u);
            return true;
        }
        bool WriteSigned(std::string_view name, std::int64_t value) override
        {
            Text(m_out, name);
            Number(m_out, static_cast<std::uint64_t>(value));
            return true;
        }
        bool WriteUnsigned(std::string_view name, std::uint64_t value) override
        {
            Text(m_out, name);
            Number(m_out, value);
            return true;
        }
        bool WriteFloat(std::string_view name, float value) override
        {
            Text(m_out, name);
            Bits(m_out, value);
            return std::isfinite(value);
        }
        bool WriteString(std::string_view name, std::string_view value) override
        {
            Text(m_out, name);
            Text(m_out, value);
            return value.size() <= std::size_t{1024} * 1024 && value.find('\0') == std::string_view::npos &&
                   m_utf8(value);
        }
        bool WriteFloat2(std::string_view name, const DirectX::XMFLOAT2& value) override
        {
            Text(m_out, name);
            Bits(m_out, value.x);
            Bits(m_out, value.y);
            return std::isfinite(value.x) && std::isfinite(value.y);
        }
        bool WriteFloat3(std::string_view name, const DirectX::XMFLOAT3& value) override
        {
            Text(m_out, name);
            Float3(m_out, value);
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }
        bool WriteFloat4(std::string_view name, const DirectX::XMFLOAT4& value) override
        {
            Text(m_out, name);
            Float4(m_out, value);
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
        }

      private:
        std::string& m_out;
        Utf8Check m_utf8;
    };

    /// Renders @p scene. When @p persistable is non-null it is cleared if any component
    /// payload fails to encode or holds a value the JSON writer refuses.
    inline std::string Fingerprint(const SparkEditor::SceneFile& scene, RecordingWriter::Utf8Check utf8,
                                   bool* persistable = nullptr)
    {
        using namespace SparkEditor;
        std::string out;
        const SceneHeader& header = scene.header;
        Number(out, header.magic);
        Number(out, header.version);
        Number(out, header.objectCount);
        Number(out, header.componentCount);
        Number(out, header.assetReferenceCount);
        Number(out, header.timestamp);
        Text(out, std::string_view(header.sceneName, sizeof(header.sceneName)));
        Text(out, std::string_view(header.description, sizeof(header.description)));
        Float3(out, header.gravity);
        Float4(out, header.ambientColor);
        Bits(out, header.ambientIntensity);

        for (const SceneObject& object : scene.objects)
        {
            out += "O";
            Number(out, object.id);
            Text(out, object.name);
            Text(out, object.tag);
            Number(out, static_cast<std::uint64_t>(static_cast<std::int64_t>(object.layer)));
            Number(out, object.active ? 1u : 0u);
            Number(out, object.staticObject ? 1u : 0u);
            for (const ComponentType type : object.componentTypes)
            {
                Number(out, static_cast<std::uint32_t>(type));
            }
            out += "T";
            Float3(out, object.transform.position);
            Float4(out, object.transform.rotation);
            Float3(out, object.transform.scale);
            Number(out, object.transform.parentID);
            for (const ObjectID child : object.transform.childIDs)
            {
                Number(out, child);
            }
        }

        for (const Component& component : scene.components)
        {
            out += "C";
            Number(out, static_cast<std::uint32_t>(component.type));
            Number(out, component.objectID);
            Number(out, component.enabled ? 1u : 0u);
            Number(out, component.HasData() ? 1u : 0u);
            if (component.HasData())
            {
                RecordingWriter writer(out, utf8);
                std::string error;
                if (!EncodeSceneComponentPayload(component, writer, error) && persistable)
                {
                    *persistable = false;
                }
            }
        }

        for (const AssetReference& reference : scene.assetReferences)
        {
            out += "A";
            Text(out, reference.assetPath);
            Text(out, reference.assetType);
            Number(out, reference.lastModified);
            Number(out, reference.fileSize);
            Text(out, reference.checksum);
            for (const std::string& dependency : reference.dependencies)
            {
                Text(out, dependency);
            }
        }

        const EnvironmentSettings& env = scene.environment;
        out += "E";
        Number(out, static_cast<std::uint64_t>(static_cast<std::int64_t>(env.skyType)));
        Float4(out, env.skyColor);
        Float4(out, env.horizonColor);
        Text(out, env.skyboxAssetPath);
        Number(out, env.fogEnabled ? 1u : 0u);
        Float4(out, env.fogColor);
        Bits(out, env.fogDensity);
        Bits(out, env.fogStart);
        Bits(out, env.fogEnd);
        Float3(out, env.windDirection);
        Bits(out, env.windStrength);
        Bits(out, env.windTurbulence);
        Number(out, env.bloomEnabled ? 1u : 0u);
        Bits(out, env.bloomIntensity);
        Bits(out, env.bloomThreshold);
        Number(out, env.tonemappingEnabled ? 1u : 0u);
        Bits(out, env.exposure);
        Bits(out, env.gamma);

        const Camera& cam = scene.defaultCamera;
        out += "K";
        Number(out, static_cast<std::uint64_t>(static_cast<std::int64_t>(cam.projectionType)));
        Bits(out, cam.fieldOfView);
        Bits(out, cam.orthographicSize);
        Bits(out, cam.nearPlane);
        Bits(out, cam.farPlane);
        Float4(out, cam.clearColor);
        Number(out, cam.isMainCamera ? 1u : 0u);
        Number(out, static_cast<std::uint64_t>(static_cast<std::int64_t>(cam.renderTargetWidth)));
        Number(out, static_cast<std::uint64_t>(static_cast<std::int64_t>(cam.renderTargetHeight)));
        return out;
    }

    /// A non-default scene used as the caller's live document: a rejected load must leave it
    /// exactly as it was.
    inline SparkEditor::SceneFile SentinelScene()
    {
        using namespace SparkEditor;
        SceneFile scene;
        std::strncpy(scene.header.sceneName, "LiveDocument", sizeof(scene.header.sceneName) - 1);
        scene.header.timestamp = 1234567;
        scene.header.ambientIntensity = 0.5f;
        SceneObject object;
        object.id = 77;
        object.name = "Sentinel";
        object.transform.position = {1.0f, 2.0f, 3.0f};
        scene.objects.push_back(object);
        scene.environment.fogEnabled = true;
        scene.defaultCamera.fieldOfView = 60.0f;
        scene.UpdateHeader();
        scene.header.timestamp = 1234567;
        return scene;
    }
} // namespace SparkFuzzEditorScene
