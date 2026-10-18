/**
 * @file JSONSceneSerializer.cpp
 * @brief JSON serialization and deserialization for scene files
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains JSONWriter, JSONParser, SaveJSON, LoadJSON, and JSON-based
 * transform/component conversion helpers.  Split from SceneSerializer.cpp.
 */

#include "SceneSerializer.h"
#include "SceneComponentCodec.h"
#include "SceneJSONReader.h"
#include "../Utils/EditorFileRead.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

using namespace DirectX;
namespace SparkEditor
{

    // =============================================================================
    // JSON Writing Helpers
    // =============================================================================

    static std::string EscapeJSON(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        static constexpr char hex[] = "0123456789abcdef";
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (c < 0x20)
                {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 0x0f];
                }
                else
                {
                    out += static_cast<char>(c);
                }
                break;
            }
        }
        return out;
    }

    static std::filesystem::path MakeTemporarySibling(const std::filesystem::path& destination)
    {
        static std::atomic<uint64_t> counter{0};
        const auto nonce = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                           counter.fetch_add(1, std::memory_order_relaxed);
        return destination.parent_path() / (destination.filename().string() + ".tmp." + std::to_string(nonce));
    }

    static bool ReplaceFileAtomically(const std::filesystem::path& temporary, const std::filesystem::path& destination,
                                      std::error_code& error)
    {
#if defined(_WIN32)
        if (::MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
        return false;
#else
        std::filesystem::rename(temporary, destination, error);
        return !error;
#endif
    }

    class TemporaryFileCleanup
    {
      public:
        explicit TemporaryFileCleanup(std::filesystem::path path) : m_path(std::move(path)) {}
        ~TemporaryFileCleanup()
        {
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
        }

      private:
        std::filesystem::path m_path;
    };

    static std::string ComponentTypeToString(ComponentType type)
    {
        const char* name = SceneComponentTypeName(type);
        return name ? name : std::string{};
    }

    template <size_t Size> static std::string BoundedString(const char (&text)[Size])
    {
        return std::string(text, std::find(text, text + Size, '\0'));
    }

    // Write helpers for indented JSON
    class JSONWriter
    {
      public:
        explicit JSONWriter(std::ostream& os, bool pretty = true) : m_os(os), m_pretty(pretty)
        {
            m_os.imbue(std::locale::classic());
            m_os << std::setprecision(std::numeric_limits<float>::max_digits10);
        }

        void BeginObject()
        {
            m_os << "{";
            if (m_pretty)
                m_os << "\n";
            m_depth++;
            m_first.push_back(true);
        }
        void EndObject()
        {
            m_depth--;
            if (m_pretty)
                Indent();
            m_os << "}";
            if (!m_first.empty())
                m_first.pop_back();
        }
        void BeginArray()
        {
            m_os << "[";
            if (m_pretty)
                m_os << "\n";
            m_depth++;
            m_first.push_back(true);
        }
        void EndArray()
        {
            m_depth--;
            if (m_pretty)
                Indent();
            m_os << "]";
            if (!m_first.empty())
                m_first.pop_back();
        }

        void Key(const std::string& k)
        {
            Sep();
            if (m_pretty)
                Indent();
            m_os << "\"" << k << "\": ";
        }
        void Value(const std::string& v) { m_os << "\"" << EscapeJSON(v) << "\""; }
        void Value(int v) { m_os << v; }
        void Value(uint32_t v) { m_os << v; }
        void Value(uint64_t v) { m_os << v; }
        void Value(int64_t v) { m_os << v; }
        void Value(float v) { m_os << v; }
        void Value(bool v) { m_os << (v ? "true" : "false"); }
// size_t overload only when it differs from uint64_t (e.g. 32-bit builds).
// On 64-bit Windows (MSVC and MinGW) and 64-bit Linux, size_t == uint64_t.
#if !defined(_WIN64) && !defined(__LP64__) && !defined(__x86_64__)
        void Value(size_t v) { m_os << v; }
#endif

        void KV(const std::string& k, const std::string& v)
        {
            Key(k);
            Value(v);
        }
        void KV(const std::string& k, const char* v)
        {
            Key(k);
            Value(std::string(v));
        }
        void KV(const std::string& k, int v)
        {
            Key(k);
            Value(v);
        }
        void KV(const std::string& k, uint32_t v)
        {
            Key(k);
            Value(v);
        }
        void KV(const std::string& k, uint64_t v)
        {
            Key(k);
            Value(v);
        }
        void KV(const std::string& k, int64_t v)
        {
            Key(k);
            Value(v);
        }
        void KV(const std::string& k, float v)
        {
            Key(k);
            Value(v);
        }
        void KV(const std::string& k, bool v)
        {
            Key(k);
            Value(v);
        }

        void Float3(const std::string& k, const XMFLOAT3& v)
        {
            Key(k);
            m_os << "[" << v.x << ", " << v.y << ", " << v.z << "]";
        }
        void Float2(const std::string& k, const XMFLOAT2& v)
        {
            Key(k);
            m_os << "[" << v.x << ", " << v.y << "]";
        }
        void Float4(const std::string& k, const XMFLOAT4& v)
        {
            Key(k);
            m_os << "[" << v.x << ", " << v.y << ", " << v.z << ", " << v.w << "]";
        }

        void ArrayElement()
        {
            Sep();
            if (m_pretty)
                Indent();
        }

      private:
        void Sep()
        {
            if (!m_first.empty() && !m_first.back())
                m_os << ",";
            if (!m_first.empty() && !m_first.back() && m_pretty)
                m_os << "\n";
            if (!m_first.empty())
                m_first.back() = false;
        }
        void Indent()
        {
            for (int i = 0; i < m_depth; ++i)
                m_os << "  ";
        }

        std::ostream& m_os;
        bool m_pretty;
        int m_depth = 0;
        std::vector<bool> m_first;
    };

    class JSONSceneComponentWriter final : public SceneComponentFieldWriter
    {
      public:
        explicit JSONSceneComponentWriter(JSONWriter& writer) : m_writer(writer) {}
        bool WriteBool(std::string_view name, bool value) override
        {
            m_writer.KV(std::string(name), value);
            return true;
        }
        bool WriteSigned(std::string_view name, int64_t value) override
        {
            m_writer.KV(std::string(name), value);
            return true;
        }
        bool WriteUnsigned(std::string_view name, uint64_t value) override
        {
            m_writer.KV(std::string(name), value);
            return true;
        }
        bool WriteFloat(std::string_view name, float value) override
        {
            m_writer.KV(std::string(name), value);
            return std::isfinite(value);
        }
        bool WriteString(std::string_view name, std::string_view value) override
        {
            m_writer.KV(std::string(name), std::string(value));
            return true;
        }
        bool WriteFloat2(std::string_view name, const XMFLOAT2& value) override
        {
            m_writer.Float2(std::string(name), value);
            return std::isfinite(value.x) && std::isfinite(value.y);
        }
        bool WriteFloat3(std::string_view name, const XMFLOAT3& value) override
        {
            m_writer.Float3(std::string(name), value);
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }
        bool WriteFloat4(std::string_view name, const XMFLOAT4& value) override
        {
            m_writer.Float4(std::string(name), value);
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
        }

      private:
        JSONWriter& m_writer;
    };

    class ValidatingSceneComponentWriter final : public SceneComponentFieldWriter
    {
      public:
        bool WriteBool(std::string_view, bool) override { return true; }
        bool WriteSigned(std::string_view, int64_t) override { return true; }
        bool WriteUnsigned(std::string_view, uint64_t) override { return true; }
        bool WriteFloat(std::string_view, float value) override { return std::isfinite(value); }
        bool WriteString(std::string_view, std::string_view value) override
        {
            constexpr size_t kMaxComponentStringBytes = 1024 * 1024;
            return value.size() <= kMaxComponentStringBytes && value.find('\0') == std::string_view::npos &&
                   IsValidSceneUTF8(value);
        }
        bool WriteFloat2(std::string_view, const XMFLOAT2& value) override
        {
            return std::isfinite(value.x) && std::isfinite(value.y);
        }
        bool WriteFloat3(std::string_view, const XMFLOAT3& value) override
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }
        bool WriteFloat4(std::string_view, const XMFLOAT4& value) override
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
        }
    };

    SerializationResult SceneSerializer::SaveJSON(const SceneFile& scene, const std::string& filePath)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Saving scene (JSON) to: %s (%zu objects)", filePath.c_str(),
                       scene.objects.size());
        SerializationResult result;

        const auto finite3 = [](const XMFLOAT3& value)
        { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
        const auto finite4 = [](const XMFLOAT4& value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
        };
        bool finite = finite3(scene.header.gravity) && finite4(scene.header.ambientColor) &&
                      std::isfinite(scene.header.ambientIntensity);
        for (const auto& object : scene.objects)
            finite = finite && finite3(object.transform.position) && finite4(object.transform.rotation) &&
                     finite3(object.transform.scale);
        const auto& environment = scene.environment;
        finite = finite && finite4(environment.skyColor) && finite4(environment.horizonColor) &&
                 finite4(environment.fogColor) && std::isfinite(environment.fogDensity) &&
                 std::isfinite(environment.fogStart) && std::isfinite(environment.fogEnd) &&
                 finite3(environment.windDirection) && std::isfinite(environment.windStrength) &&
                 std::isfinite(environment.windTurbulence) && std::isfinite(environment.bloomIntensity) &&
                 std::isfinite(environment.bloomThreshold) && std::isfinite(environment.exposure) &&
                 std::isfinite(environment.gamma);
        const auto& camera = scene.defaultCamera;
        finite = finite && std::isfinite(camera.fieldOfView) && std::isfinite(camera.orthographicSize) &&
                 std::isfinite(camera.nearPlane) && std::isfinite(camera.farPlane) && finite4(camera.clearColor);
        if (!finite)
        {
            result.errorMessage = "Scene contains non-finite floating-point values";
            return result;
        }
        const int skyType = static_cast<int>(environment.skyType);
        const int projectionType = static_cast<int>(camera.projectionType);
        if (skyType < static_cast<int>(EnvironmentSettings::SOLID_COLOR) ||
            skyType > static_cast<int>(EnvironmentSettings::PROCEDURAL) ||
            projectionType < static_cast<int>(Camera::PERSPECTIVE) ||
            projectionType > static_cast<int>(Camera::ORTHOGRAPHIC))
        {
            result.errorMessage = "Scene contains an invalid environment or camera enum value";
            return result;
        }
        if (scene.objects.size() > std::numeric_limits<uint32_t>::max() ||
            scene.components.size() > std::numeric_limits<uint32_t>::max() ||
            scene.assetReferences.size() > std::numeric_limits<uint32_t>::max())
        {
            result.errorMessage = "Scene collection count exceeds the JSON format limit";
            return result;
        }
        if (std::find(std::begin(scene.header.sceneName), std::end(scene.header.sceneName), '\0') ==
                std::end(scene.header.sceneName) ||
            std::find(std::begin(scene.header.description), std::end(scene.header.description), '\0') ==
                std::end(scene.header.description))
        {
            result.errorMessage = "Scene header strings must be null terminated";
            return result;
        }
        bool validUTF8 = IsValidSceneUTF8(BoundedString(scene.header.sceneName)) &&
                         IsValidSceneUTF8(BoundedString(scene.header.description)) &&
                         IsValidSceneUTF8(scene.environment.skyboxAssetPath);
        for (const auto& object : scene.objects)
            validUTF8 = validUTF8 && IsValidSceneUTF8(object.name) && IsValidSceneUTF8(object.tag);
        for (const auto& reference : scene.assetReferences)
        {
            validUTF8 = validUTF8 && IsValidSceneUTF8(reference.assetPath) && IsValidSceneUTF8(reference.assetType) &&
                        IsValidSceneUTF8(reference.checksum);
            for (const auto& dependency : reference.dependencies)
                validUTF8 = validUTF8 && IsValidSceneUTF8(dependency);
        }
        if (!validUTF8)
        {
            result.errorMessage = "Scene contains a string that is not valid UTF-8";
            return result;
        }
        if (!AppendSceneValidation(scene, result))
        {
            result.errorMessage = "Scene data failed validation before save";
            return result;
        }

        ValidatingSceneComponentWriter payloadValidator;
        for (const Component& component : scene.components)
        {
            const bool markerOnly =
                component.type == ComponentType::TRANSFORM || component.type == ComponentType::SPRITE_ANIMATOR;
            if (markerOnly)
            {
                if (component.HasData())
                {
                    result.errorMessage = "Marker-only component unexpectedly contains a payload";
                    return result;
                }
                continue;
            }
            std::string codecError;
            if (!EncodeSceneComponentPayload(component, payloadValidator, codecError))
            {
                result.errorMessage = "Cannot persist " + ComponentTypeToString(component.type) + ": " + codecError;
                return result;
            }
        }

        const std::filesystem::path destination(filePath);
        const std::filesystem::path temporary = MakeTemporarySibling(destination);
        TemporaryFileCleanup temporaryCleanup(temporary);
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            result.success = false;
            result.errorMessage = "Failed to open file for writing: " + filePath;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to write JSON scene: %s", filePath.c_str());
            return result;
        }

        JSONWriter w(file, m_prettyPrintJSON);
        w.BeginObject();

        // Header
        w.KV("sceneName", BoundedString(scene.header.sceneName));
        w.KV("version", scene.header.version);
        w.KV("description", BoundedString(scene.header.description));
        w.KV("objectCount", static_cast<uint32_t>(scene.objects.size()));
        w.KV("componentCount", static_cast<uint32_t>(scene.components.size()));
        w.KV("assetReferenceCount", static_cast<uint32_t>(scene.assetReferences.size()));
        w.KV("timestamp", scene.header.timestamp);
        w.Float3("gravity", scene.header.gravity);
        w.Float4("ambientColor", scene.header.ambientColor);
        w.KV("ambientIntensity", scene.header.ambientIntensity);

        // Objects
        w.Key("objects");
        w.BeginArray();
        for (const auto& obj : scene.objects)
        {
            w.ArrayElement();
            w.BeginObject();
            w.KV("id", obj.id);
            w.KV("name", obj.name);
            w.KV("tag", obj.tag);
            w.KV("layer", obj.layer);
            w.KV("active", obj.active);
            w.KV("staticObject", obj.staticObject);

            // Transform
            w.Key("transform");
            w.BeginObject();
            w.Float3("position", obj.transform.position);
            w.Float4("rotation", obj.transform.rotation);
            w.Float3("scale", obj.transform.scale);
            w.KV("parentID", obj.transform.parentID);
            if (!obj.transform.childIDs.empty())
            {
                w.Key("childIDs");
                w.BeginArray();
                for (auto cid : obj.transform.childIDs)
                {
                    w.ArrayElement();
                    w.Value(cid);
                }
                w.EndArray();
            }
            w.EndObject(); // transform

            // Component types
            if (!obj.componentTypes.empty())
            {
                w.Key("componentTypes");
                w.BeginArray();
                for (auto ct : obj.componentTypes)
                {
                    w.ArrayElement();
                    w.Value(ComponentTypeToString(ct));
                }
                w.EndArray();
            }

            w.EndObject(); // object
        }
        w.EndArray(); // objects

        // Components
        w.Key("components");
        w.BeginArray();
        for (const auto& comp : scene.components)
        {
            w.ArrayElement();
            w.BeginObject();
            w.KV("type", ComponentTypeToString(comp.type));
            w.KV("objectID", comp.objectID);
            w.KV("enabled", comp.enabled);
            if (HasSceneComponentPayloadCodec(comp.type))
            {
                w.Key("data");
                w.BeginObject();
                w.KV("schema", SCENE_COMPONENT_SCHEMA_VERSION);
                w.Key("fields");
                w.BeginObject();
                JSONSceneComponentWriter payloadWriter(w);
                std::string ignoredError;
                (void)EncodeSceneComponentPayload(comp, payloadWriter, ignoredError);
                w.EndObject();
                w.EndObject();
            }
            w.EndObject();
        }
        w.EndArray(); // components

        // Environment
        w.Key("environment");
        w.BeginObject();
        const auto& env = scene.environment;
        w.KV("skyType", static_cast<int>(env.skyType));
        w.Float4("skyColor", env.skyColor);
        w.Float4("horizonColor", env.horizonColor);
        w.KV("skyboxAssetPath", env.skyboxAssetPath);
        w.KV("fogEnabled", env.fogEnabled);
        w.Float4("fogColor", env.fogColor);
        w.KV("fogDensity", env.fogDensity);
        w.KV("fogStart", env.fogStart);
        w.KV("fogEnd", env.fogEnd);
        w.Float3("windDirection", env.windDirection);
        w.KV("windStrength", env.windStrength);
        w.KV("windTurbulence", env.windTurbulence);
        w.KV("bloomEnabled", env.bloomEnabled);
        w.KV("bloomIntensity", env.bloomIntensity);
        w.KV("bloomThreshold", env.bloomThreshold);
        w.KV("tonemappingEnabled", env.tonemappingEnabled);
        w.KV("exposure", env.exposure);
        w.KV("gamma", env.gamma);
        w.EndObject(); // environment

        // Default camera
        w.Key("defaultCamera");
        w.BeginObject();
        const auto& cam = scene.defaultCamera;
        w.KV("projectionType", static_cast<int>(cam.projectionType));
        w.KV("fieldOfView", cam.fieldOfView);
        w.KV("orthographicSize", cam.orthographicSize);
        w.KV("nearPlane", cam.nearPlane);
        w.KV("farPlane", cam.farPlane);
        w.Float4("clearColor", cam.clearColor);
        w.KV("isMainCamera", cam.isMainCamera);
        w.KV("renderTargetWidth", cam.renderTargetWidth);
        w.KV("renderTargetHeight", cam.renderTargetHeight);
        w.EndObject(); // defaultCamera

        // Asset references
        w.Key("assetReferences");
        w.BeginArray();
        for (const auto& ref : scene.assetReferences)
        {
            w.ArrayElement();
            w.BeginObject();
            w.KV("assetPath", ref.assetPath);
            w.KV("assetType", ref.assetType);
            w.KV("lastModified", ref.lastModified);
            w.KV("fileSize", ref.fileSize);
            w.KV("checksum", ref.checksum);
            if (!ref.dependencies.empty())
            {
                w.Key("dependencies");
                w.BeginArray();
                for (const auto& dep : ref.dependencies)
                {
                    w.ArrayElement();
                    w.Value(dep);
                }
                w.EndArray();
            }
            w.EndObject();
        }
        w.EndArray(); // assetReferences

        w.EndObject(); // root
        file << "\n";

        if (!file.good())
        {
            result.success = false;
            result.errorMessage = "Write error while saving JSON: " + filePath;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Write error saving JSON scene, removing partial file: %s",
                            filePath.c_str());
            file.close();
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
            return result;
        }

        const std::streamoff writtenBytes = static_cast<std::streamoff>(file.tellp());
        if (writtenBytes < 0 || static_cast<uint64_t>(writtenBytes) > m_maxFileSize)
        {
            result.errorMessage = "JSON scene exceeds the configured size limit: " + filePath;
            file.close();
            return result;
        }
        result.bytesProcessed = static_cast<size_t>(writtenBytes);
        file.close();
        if (!file.good())
        {
            result.errorMessage = "Failed to flush JSON scene: " + filePath;
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return result;
        }

        std::error_code replaceError;
        if (!ReplaceFileAtomically(temporary, destination, replaceError))
        {
            result.errorMessage = "Failed to replace JSON scene: " + replaceError.message();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return result;
        }

        result.success = true;
        m_totalBytesWritten += result.bytesProcessed;
        return result;
    }

    SerializationResult SceneSerializer::LoadJSON(const std::string& filePath, SceneFile& outScene)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading scene (JSON) from: %s", filePath.c_str());
        SerializationResult result;

        // Size and type are decided on the opened handle. The size used to be read by path after
        // the open, so it could describe a different file than the stream being read.
        std::string content;
        switch (ReadRegularFileBounded(std::filesystem::path(filePath), m_maxFileSize, content))
        {
        case BoundedReadStatus::Ok:
            break;
        case BoundedReadStatus::TooLarge:
            result.errorMessage = "Scene file is empty or exceeds the configured size limit: " + filePath;
            return result;
        case BoundedReadStatus::Missing:
        case BoundedReadStatus::NotRegularFile:
        case BoundedReadStatus::Failed:
            result.success = false;
            result.errorMessage = "Failed to open file: " + filePath;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to read JSON scene: %s", filePath.c_str());
            return result;
        }

        if (content.empty())
        {
            result.errorMessage = "Scene file is empty or exceeds the configured size limit: " + filePath;
            return result;
        }

        if (!DecodeSceneJSONDocument(content, outScene, result))
        {
            return result;
        }
        m_totalBytesRead += content.size();
        return result;
    }

    void* SceneSerializer::TransformToJSON(const Transform& transform)
    {
        // Produce a heap-allocated JSON string for the transform.
        // Caller must delete the returned std::string* when done.
        auto* json = new std::string();
        std::ostringstream ss;
        ss << std::setprecision(6);
        ss << "{\"position\":[" << transform.position.x << "," << transform.position.y << "," << transform.position.z
           << "]," << "\"rotation\":[" << transform.rotation.x << "," << transform.rotation.y << ","
           << transform.rotation.z << "," << transform.rotation.w << "]," << "\"scale\":[" << transform.scale.x << ","
           << transform.scale.y << "," << transform.scale.z << "]," << "\"parentID\":" << transform.parentID << "}";
        *json = ss.str();
        return json;
    }

    bool SceneSerializer::JSONToTransform(void* json, Transform& transform)
    {
        if (!json)
            return false;
        const auto* str = static_cast<const std::string*>(json);
        if (str->empty())
            return false;

        // Minimal parser: extract arrays by key name
        auto extractArray = [&](const std::string& key, float* out, int count) -> bool
        {
            auto pos = str->find("\"" + key + "\"");
            if (pos == std::string::npos)
                return false;
            pos = str->find('[', pos);
            if (pos == std::string::npos)
                return false;
            ++pos;
            for (int i = 0; i < count; ++i)
            {
                if (pos >= str->size())
                    return false;
                char* end = nullptr;
                out[i] = std::strtof(str->c_str() + pos, &end);
                if (end == str->c_str() + pos)
                    return false; // No numeric conversion occurred
                pos = static_cast<size_t>(end - str->c_str());
                if (pos < str->size() && (*end == ',' || *end == ']'))
                    ++pos;
            }
            return true;
        };

        extractArray("position", &transform.position.x, 3);
        extractArray("rotation", &transform.rotation.x, 4);
        extractArray("scale", &transform.scale.x, 3);

        // Extract parentID
        auto pidPos = str->find("\"parentID\"");
        if (pidPos != std::string::npos)
        {
            pidPos = str->find(':', pidPos);
            if (pidPos != std::string::npos)
            {
                char* end = nullptr;
                transform.parentID = std::strtoull(str->c_str() + pidPos + 1, &end, 10);
            }
        }
        return true;
    }

    void* SceneSerializer::ComponentToJSON(const Component& component)
    {
        (void)component;
        // The old string helper had no structured error channel and emitted
        // ABI-dependent object images. Callers must use SaveScene, which owns
        // the schema-tagged codec transaction.
        return nullptr;
    }

    bool SceneSerializer::JSONToComponent(void* json, Component& component)
    {
        (void)json;
        (void)component;
        return false;
    }

} // namespace SparkEditor
