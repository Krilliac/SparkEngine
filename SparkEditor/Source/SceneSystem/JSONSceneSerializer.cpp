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
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <array>
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

    static bool IsValidUTF8(std::string_view value)
    {
        size_t i = 0;
        const auto continuation = [&](size_t offset)
        { return offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xc0) == 0x80; };
        while (i < value.size())
        {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (c <= 0x7f)
            {
                ++i;
                continue;
            }
            if (c >= 0xc2 && c <= 0xdf && continuation(i + 1))
            {
                i += 2;
                continue;
            }
            if (c == 0xe0 && i + 2 < value.size() && static_cast<unsigned char>(value[i + 1]) >= 0xa0 &&
                static_cast<unsigned char>(value[i + 1]) <= 0xbf && continuation(i + 2))
            {
                i += 3;
                continue;
            }
            if (((c >= 0xe1 && c <= 0xec) || (c >= 0xee && c <= 0xef)) && continuation(i + 1) && continuation(i + 2))
            {
                i += 3;
                continue;
            }
            if (c == 0xed && i + 2 < value.size() && static_cast<unsigned char>(value[i + 1]) >= 0x80 &&
                static_cast<unsigned char>(value[i + 1]) <= 0x9f && continuation(i + 2))
            {
                i += 3;
                continue;
            }
            if (c == 0xf0 && i + 3 < value.size() && static_cast<unsigned char>(value[i + 1]) >= 0x90 &&
                static_cast<unsigned char>(value[i + 1]) <= 0xbf && continuation(i + 2) && continuation(i + 3))
            {
                i += 4;
                continue;
            }
            if (c >= 0xf1 && c <= 0xf3 && continuation(i + 1) && continuation(i + 2) && continuation(i + 3))
            {
                i += 4;
                continue;
            }
            if (c == 0xf4 && i + 3 < value.size() && static_cast<unsigned char>(value[i + 1]) >= 0x80 &&
                static_cast<unsigned char>(value[i + 1]) <= 0x8f && continuation(i + 2) && continuation(i + 3))
            {
                i += 4;
                continue;
            }
            return false;
        }
        return true;
    }

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

    static bool StringToComponentType(const std::string& s, ComponentType& output)
    {
        return TryParseSceneComponentTypeName(s, output);
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
                   IsValidUTF8(value);
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
        bool validUTF8 = IsValidUTF8(BoundedString(scene.header.sceneName)) &&
                         IsValidUTF8(BoundedString(scene.header.description)) &&
                         IsValidUTF8(scene.environment.skyboxAssetPath);
        for (const auto& object : scene.objects)
            validUTF8 = validUTF8 && IsValidUTF8(object.name) && IsValidUTF8(object.tag);
        for (const auto& reference : scene.assetReferences)
        {
            validUTF8 = validUTF8 && IsValidUTF8(reference.assetPath) && IsValidUTF8(reference.assetType) &&
                        IsValidUTF8(reference.checksum);
            for (const auto& dependency : reference.dependencies)
                validUTF8 = validUTF8 && IsValidUTF8(dependency);
        }
        if (!validUTF8)
        {
            result.errorMessage = "Scene contains a string that is not valid UTF-8";
            return result;
        }
        if (!ValidateScene(scene, result))
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

    // =============================================================================
    // Simple JSON Parser (no external dependencies)
    // =============================================================================

    namespace
    {

        // Forward declarations. std::vector does not require a complete type
        // at the point of member declaration, only when member functions are
        // instantiated, so the ordering below is valid.
        struct JSONMember;
        using JSONObject = std::vector<JSONMember>;

        struct JSONValue
        {
            enum Type
            {
                NONE,
                STRING,
                NUMBER,
                BOOL,
                OBJECT,
                ARRAY
            } type = NONE;
            std::string strVal;
            std::string numText;
            double numVal = 0.0;
            bool boolVal = false;
            JSONObject objVal;
            std::vector<JSONValue> arrVal;

            std::string GetString(const std::string& def = "") const { return type == STRING ? strVal : def; }
            double GetNumber(double def = 0.0) const { return type == NUMBER ? numVal : def; }
            float GetFloat(float def = 0.0f) const
            {
                float value = 0.0f;
                return TryGetFloat(value) ? value : def;
            }
            bool TryGetFloat(float& value) const
            {
                if (type != NUMBER || numVal < -std::numeric_limits<float>::max() ||
                    numVal > std::numeric_limits<float>::max())
                    return false;
                value = static_cast<float>(numVal);
                return true;
            }
            bool TryGetInt(int& value) const
            {
                if (type != NUMBER || numText.empty())
                    return false;
                const auto result = std::from_chars(numText.data(), numText.data() + numText.size(), value);
                return result.ec == std::errc{} && result.ptr == numText.data() + numText.size();
            }
            bool TryGetInt64(int64_t& value) const
            {
                if (type != NUMBER || numText.empty())
                    return false;
                const auto result = std::from_chars(numText.data(), numText.data() + numText.size(), value);
                return result.ec == std::errc{} && result.ptr == numText.data() + numText.size();
            }
            bool TryGetUint32(uint32_t& value) const
            {
                if (type != NUMBER || numText.empty())
                    return false;
                const auto result = std::from_chars(numText.data(), numText.data() + numText.size(), value);
                return result.ec == std::errc{} && result.ptr == numText.data() + numText.size();
            }
            uint32_t GetUint32(uint32_t def = 0) const
            {
                uint32_t value = 0;
                return TryGetUint32(value) ? value : def;
            }
            bool TryGetUint64(uint64_t& value) const
            {
                if (type != NUMBER || numText.empty())
                    return false;
                const auto result = std::from_chars(numText.data(), numText.data() + numText.size(), value);
                return result.ec == std::errc{} && result.ptr == numText.data() + numText.size();
            }
            uint64_t GetUint64(uint64_t def = 0) const
            {
                uint64_t value = 0;
                return TryGetUint64(value) ? value : def;
            }
            bool GetBool(bool def = false) const { return type == BOOL ? boolVal : def; }

            // Defined out-of-line after JSONMember is complete.
            const JSONValue* Find(const std::string& key) const;

            XMFLOAT3 GetFloat3(XMFLOAT3 def = {0, 0, 0}) const
            {
                if (type != ARRAY || arrVal.size() < 3)
                    return def;
                return {arrVal[0].GetFloat(), arrVal[1].GetFloat(), arrVal[2].GetFloat()};
            }

            XMFLOAT4 GetFloat4(XMFLOAT4 def = {0, 0, 0, 1}) const
            {
                if (type != ARRAY || arrVal.size() < 4)
                    return def;
                return {arrVal[0].GetFloat(), arrVal[1].GetFloat(), arrVal[2].GetFloat(), arrVal[3].GetFloat()};
            }
        };

        // Plain struct instead of std::pair to avoid Clang + libstdc++ 14
        // constructibility trait checks on incomplete types.
        struct JSONMember
        {
            std::string key;
            JSONValue value;
        };

        const JSONValue* JSONValue::Find(const std::string& key) const
        {
            if (type != OBJECT)
                return nullptr;
            for (const auto& m : objVal)
            {
                if (m.key == key)
                    return &m.value;
            }
            return nullptr;
        }

        class JSONParser
        {
          public:
            explicit JSONParser(const std::string& input) : m_input(input), m_pos(0) {}

            bool Parse(JSONValue& out)
            {
                SkipWhitespace();
                if (!ParseValue(out))
                    return false;
                SkipWhitespace();
                return m_pos == m_input.size();
            }

          private:
            class DepthScope
            {
              public:
                explicit DepthScope(JSONParser& parser) : m_parser(parser), m_entered(parser.EnterContainer()) {}
                ~DepthScope()
                {
                    if (m_entered)
                        --m_parser.m_depth;
                }
                explicit operator bool() const { return m_entered; }

              private:
                JSONParser& m_parser;
                bool m_entered;
            };

            bool EnterContainer()
            {
                constexpr size_t kMaxDepth = 128;
                if (m_depth >= kMaxDepth)
                    return false;
                ++m_depth;
                return true;
            }

            bool ReserveNode()
            {
                constexpr size_t kMaxNodes = 250'000;
                if (m_nodeCount >= kMaxNodes)
                    return false;
                ++m_nodeCount;
                return true;
            }

            void SkipWhitespace()
            {
                while (m_pos < m_input.size() && std::isspace(static_cast<unsigned char>(m_input[m_pos])))
                    m_pos++;
            }

            char Peek() { return m_pos < m_input.size() ? m_input[m_pos] : '\0'; }
            char Next() { return m_pos < m_input.size() ? m_input[m_pos++] : '\0'; }

            bool ParseValue(JSONValue& val)
            {
                if (!ReserveNode())
                    return false;
                SkipWhitespace();
                char c = Peek();
                if (c == '"')
                    return ParseString(val);
                if (c == '{')
                    return ParseObject(val);
                if (c == '[')
                    return ParseArray(val);
                if (c == 't' || c == 'f')
                    return ParseBool(val);
                if (c == 'n')
                    return ParseNull(val);
                if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
                    return ParseNumber(val);
                return false;
            }

            bool ParseString(JSONValue& val)
            {
                if (Next() != '"')
                    return false;
                std::string s;
                while (m_pos < m_input.size())
                {
                    char c = Next();
                    if (c == '"')
                    {
                        if (!IsValidUTF8(s))
                            return false;
                        val.type = JSONValue::STRING;
                        val.strVal = s;
                        return true;
                    }
                    if (c == '\\')
                    {
                        if (m_pos >= m_input.size())
                            return false;
                        char esc = Next();
                        switch (esc)
                        {
                        case '"':
                            s += '"';
                            break;
                        case '\\':
                            s += '\\';
                            break;
                        case '/':
                            s += '/';
                            break;
                        case 'b':
                            s += '\b';
                            break;
                        case 'f':
                            s += '\f';
                            break;
                        case 'n':
                            s += '\n';
                            break;
                        case 'r':
                            s += '\r';
                            break;
                        case 't':
                            s += '\t';
                            break;
                        case 'u':
                        {
                            uint32_t codePoint = 0;
                            if (!ParseHexCodeUnit(codePoint))
                                return false;
                            if (codePoint >= 0xd800 && codePoint <= 0xdbff)
                            {
                                if (m_pos + 2 > m_input.size() || m_input[m_pos] != '\\' || m_input[m_pos + 1] != 'u')
                                    return false;
                                m_pos += 2;
                                uint32_t low = 0;
                                if (!ParseHexCodeUnit(low) || low < 0xdc00 || low > 0xdfff)
                                    return false;
                                codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
                            }
                            else if (codePoint >= 0xdc00 && codePoint <= 0xdfff)
                            {
                                return false;
                            }
                            AppendUTF8(s, codePoint);
                            break;
                        }
                        default:
                            return false;
                        }
                    }
                    else
                    {
                        if (static_cast<unsigned char>(c) < 0x20)
                            return false;
                        s += c;
                    }
                }
                return false;
            }

            bool ParseHexCodeUnit(uint32_t& value)
            {
                if (m_pos + 4 > m_input.size())
                    return false;
                value = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const unsigned char c = static_cast<unsigned char>(m_input[m_pos++]);
                    value <<= 4;
                    if (c >= '0' && c <= '9')
                        value |= c - '0';
                    else if (c >= 'a' && c <= 'f')
                        value |= c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F')
                        value |= c - 'A' + 10;
                    else
                        return false;
                }
                return true;
            }

            static void AppendUTF8(std::string& output, uint32_t codePoint)
            {
                if (codePoint <= 0x7f)
                    output += static_cast<char>(codePoint);
                else if (codePoint <= 0x7ff)
                {
                    output += static_cast<char>(0xc0 | (codePoint >> 6));
                    output += static_cast<char>(0x80 | (codePoint & 0x3f));
                }
                else if (codePoint <= 0xffff)
                {
                    output += static_cast<char>(0xe0 | (codePoint >> 12));
                    output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
                    output += static_cast<char>(0x80 | (codePoint & 0x3f));
                }
                else
                {
                    output += static_cast<char>(0xf0 | (codePoint >> 18));
                    output += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f));
                    output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
                    output += static_cast<char>(0x80 | (codePoint & 0x3f));
                }
            }

            bool ParseNumber(JSONValue& val)
            {
                size_t start = m_pos;
                if (Peek() == '-')
                    m_pos++;
                if (m_pos >= m_input.size() || !std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                    return false;
                if (m_input[m_pos] == '0')
                    ++m_pos;
                else
                    while (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                        m_pos++;
                if (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                    return false;
                if (m_pos < m_input.size() && m_input[m_pos] == '.')
                {
                    m_pos++;
                    const size_t fractionalStart = m_pos;
                    while (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                        m_pos++;
                    if (m_pos == fractionalStart)
                        return false;
                }
                if (m_pos < m_input.size() && (m_input[m_pos] == 'e' || m_input[m_pos] == 'E'))
                {
                    m_pos++;
                    if (m_pos < m_input.size() && (m_input[m_pos] == '+' || m_input[m_pos] == '-'))
                        m_pos++;
                    const size_t exponentStart = m_pos;
                    while (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                        m_pos++;
                    if (m_pos == exponentStart)
                        return false;
                }
                val.numText = m_input.substr(start, m_pos - start);
                // libc++ versions used by the MSan runner do not provide the
                // floating-point std::from_chars overload. A classic-locale,
                // no-skip stream keeps JSON parsing locale-independent while
                // remaining portable across every supported standard library.
                std::istringstream numberStream(val.numText);
                numberStream.imbue(std::locale::classic());
                numberStream >> std::noskipws >> val.numVal;
                if (numberStream.fail() || numberStream.peek() != std::char_traits<char>::eof())
                    return false;
                if (!std::isfinite(val.numVal))
                    return false;
                val.type = JSONValue::NUMBER;
                return true;
            }

            bool ParseBool(JSONValue& val)
            {
                if (m_input.compare(m_pos, 4, "true") == 0)
                {
                    m_pos += 4;
                    val.type = JSONValue::BOOL;
                    val.boolVal = true;
                    return true;
                }
                if (m_input.compare(m_pos, 5, "false") == 0)
                {
                    m_pos += 5;
                    val.type = JSONValue::BOOL;
                    val.boolVal = false;
                    return true;
                }
                return false;
            }

            bool ParseNull(JSONValue& val)
            {
                if (m_input.compare(m_pos, 4, "null") == 0)
                {
                    m_pos += 4;
                    val.type = JSONValue::NONE;
                    return true;
                }
                return false;
            }

            bool ParseObject(JSONValue& val)
            {
                if (Next() != '{')
                    return false;
                DepthScope depth(*this);
                if (!depth)
                    return false;
                val.type = JSONValue::OBJECT;
                SkipWhitespace();
                if (Peek() == '}')
                {
                    m_pos++;
                    return true;
                }
                while (true)
                {
                    SkipWhitespace();
                    JSONValue key;
                    if (!ReserveNode() || !ParseString(key))
                        return false;
                    SkipWhitespace();
                    if (Next() != ':')
                        return false;
                    JSONValue value;
                    if (!ParseValue(value))
                        return false;
                    val.objVal.push_back(JSONMember{key.strVal, std::move(value)});
                    SkipWhitespace();
                    char c = Next();
                    if (c == '}')
                        return true;
                    if (c != ',')
                        return false;
                }
            }

            bool ParseArray(JSONValue& val)
            {
                if (Next() != '[')
                    return false;
                DepthScope depth(*this);
                if (!depth)
                    return false;
                val.type = JSONValue::ARRAY;
                SkipWhitespace();
                if (Peek() == ']')
                {
                    m_pos++;
                    return true;
                }
                while (true)
                {
                    JSONValue elem;
                    if (!ParseValue(elem))
                        return false;
                    val.arrVal.push_back(std::move(elem));
                    SkipWhitespace();
                    char c = Next();
                    if (c == ']')
                        return true;
                    if (c != ',')
                        return false;
                }
            }

            const std::string& m_input;
            size_t m_pos;
            size_t m_depth = 0;
            size_t m_nodeCount = 0;
        };

        // Helper to read a field safely
        static const JSONValue* Field(const JSONValue& obj, const std::string& key)
        {
            return obj.Find(key);
        }

        static bool OptionalType(const JSONValue& object, const char* key, JSONValue::Type type)
        {
            const JSONValue* value = object.Find(key);
            return !value || value->type == type;
        }

        static bool OptionalFloat(const JSONValue& object, const char* key)
        {
            const JSONValue* value = object.Find(key);
            if (!value)
                return true;
            float parsed = 0.0f;
            return value->TryGetFloat(parsed);
        }

        static bool OptionalInt(const JSONValue& object, const char* key)
        {
            const JSONValue* value = object.Find(key);
            if (!value)
                return true;
            int parsed = 0;
            return value->TryGetInt(parsed);
        }

        static bool OptionalUint32(const JSONValue& object, const char* key)
        {
            const JSONValue* value = object.Find(key);
            if (!value)
                return true;
            uint32_t parsed = 0;
            return value->TryGetUint32(parsed);
        }

        static bool OptionalUint64(const JSONValue& object, const char* key)
        {
            const JSONValue* value = object.Find(key);
            if (!value)
                return true;
            uint64_t parsed = 0;
            return value->TryGetUint64(parsed);
        }

        static bool OptionalFloatVector(const JSONValue& object, const char* key, size_t length)
        {
            const JSONValue* value = object.Find(key);
            if (!value)
                return true;
            if (value->type != JSONValue::ARRAY || value->arrVal.size() != length)
                return false;
            for (const JSONValue& element : value->arrVal)
            {
                float parsed = 0.0f;
                if (!element.TryGetFloat(parsed))
                    return false;
            }
            return true;
        }

        static std::string FieldStr(const JSONValue& obj, const std::string& key, const std::string& def = "")
        {
            auto* v = obj.Find(key);
            return v ? v->GetString(def) : def;
        }
        static float FieldFloat(const JSONValue& obj, const std::string& key, float def = 0.0f)
        {
            auto* v = obj.Find(key);
            return v ? v->GetFloat(def) : def;
        }
        static bool FieldIntChecked(const JSONValue& obj, const std::string& key, int def, int& value)
        {
            auto* v = obj.Find(key);
            if (!v)
            {
                value = def;
                return true;
            }
            return v->TryGetInt(value);
        }
        static uint32_t FieldUint32(const JSONValue& obj, const std::string& key, uint32_t def = 0)
        {
            auto* v = obj.Find(key);
            return v ? v->GetUint32(def) : def;
        }
        static uint64_t FieldUint64(const JSONValue& obj, const std::string& key, uint64_t def = 0)
        {
            auto* v = obj.Find(key);
            return v ? v->GetUint64(def) : def;
        }
        static bool FieldBool(const JSONValue& obj, const std::string& key, bool def = false)
        {
            auto* v = obj.Find(key);
            return v ? v->GetBool(def) : def;
        }
        static XMFLOAT3 FieldFloat3(const JSONValue& obj, const std::string& key, XMFLOAT3 def = {0, 0, 0})
        {
            auto* v = obj.Find(key);
            return v ? v->GetFloat3(def) : def;
        }
        static XMFLOAT4 FieldFloat4(const JSONValue& obj, const std::string& key, XMFLOAT4 def = {0, 0, 0, 1})
        {
            auto* v = obj.Find(key);
            return v ? v->GetFloat4(def) : def;
        }

        class JSONSceneComponentReader final : public SceneComponentFieldReader
        {
          public:
            explicit JSONSceneComponentReader(const JSONValue& fields) : m_fields(fields) {}

            bool HasExactly(std::span<const std::string_view> names) const override
            {
                if (m_fields.type != JSONValue::OBJECT || m_fields.objVal.size() != names.size())
                    return false;
                for (std::string_view name : names)
                {
                    size_t matches = 0;
                    for (const JSONMember& member : m_fields.objVal)
                        matches += member.key == name ? 1u : 0u;
                    if (matches != 1)
                        return false;
                }
                return true;
            }

            bool ReadBool(std::string_view name, bool& value) const override
            {
                const JSONValue* field = Find(name);
                if (!field || field->type != JSONValue::BOOL)
                    return false;
                value = field->boolVal;
                return true;
            }
            bool ReadSigned(std::string_view name, int64_t& value) const override
            {
                const JSONValue* field = Find(name);
                return field && field->TryGetInt64(value);
            }
            bool ReadUnsigned(std::string_view name, uint64_t& value) const override
            {
                const JSONValue* field = Find(name);
                return field && field->TryGetUint64(value);
            }
            bool ReadFloat(std::string_view name, float& value) const override
            {
                const JSONValue* field = Find(name);
                return field && field->TryGetFloat(value);
            }
            bool ReadString(std::string_view name, std::string& value) const override
            {
                const JSONValue* field = Find(name);
                constexpr size_t kMaxComponentStringBytes = 1024 * 1024;
                if (!field || field->type != JSONValue::STRING || field->strVal.size() > kMaxComponentStringBytes ||
                    field->strVal.find('\0') != std::string::npos)
                    return false;
                value = field->strVal;
                return true;
            }
            bool ReadFloat2(std::string_view name, XMFLOAT2& value) const override
            {
                float parsed[2]{};
                if (!ReadFloatArray(name, parsed))
                    return false;
                value = {parsed[0], parsed[1]};
                return true;
            }
            bool ReadFloat3(std::string_view name, XMFLOAT3& value) const override
            {
                float parsed[3]{};
                if (!ReadFloatArray(name, parsed))
                    return false;
                value = {parsed[0], parsed[1], parsed[2]};
                return true;
            }
            bool ReadFloat4(std::string_view name, XMFLOAT4& value) const override
            {
                float parsed[4]{};
                if (!ReadFloatArray(name, parsed))
                    return false;
                value = {parsed[0], parsed[1], parsed[2], parsed[3]};
                return true;
            }

          private:
            const JSONValue* Find(std::string_view name) const { return m_fields.Find(std::string(name)); }

            template <size_t Size> bool ReadFloatArray(std::string_view name, float (&values)[Size]) const
            {
                const JSONValue* field = Find(name);
                if (!field || field->type != JSONValue::ARRAY || field->arrVal.size() != Size)
                    return false;
                for (size_t index = 0; index < Size; ++index)
                    if (!field->arrVal[index].TryGetFloat(values[index]))
                        return false;
                return true;
            }

            const JSONValue& m_fields;
        };

    } // anonymous namespace

    SerializationResult SceneSerializer::LoadJSON(const std::string& filePath, SceneFile& outScene)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading scene (JSON) from: %s", filePath.c_str());
        SerializationResult result;

        std::ifstream file(filePath);
        if (!file.is_open())
        {
            result.success = false;
            result.errorMessage = "Failed to open file: " + filePath;
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to read JSON scene: %s", filePath.c_str());
            return result;
        }

        std::error_code sizeError;
        const uintmax_t fileSize = std::filesystem::file_size(filePath, sizeError);
        if (sizeError || fileSize == 0 || fileSize > m_maxFileSize)
        {
            result.errorMessage = "Scene file is empty or exceeds the configured size limit: " + filePath;
            return result;
        }

        std::string content;
        content.reserve(static_cast<size_t>(fileSize));
        std::array<char, 64 * 1024> readBuffer{};
        while (content.size() <= m_maxFileSize)
        {
            const size_t remaining = m_maxFileSize + 1 - content.size();
            const std::streamsize request = static_cast<std::streamsize>(std::min(remaining, readBuffer.size()));
            file.read(readBuffer.data(), request);
            const std::streamsize count = file.gcount();
            if (count > 0)
                content.append(readBuffer.data(), static_cast<size_t>(count));
            if (content.size() > m_maxFileSize)
            {
                result.errorMessage = "Scene file grew beyond the configured size limit while reading: " + filePath;
                return result;
            }
            if (file.eof())
                break;
            if (!file.good())
            {
                result.errorMessage = "Read error while loading scene: " + filePath;
                return result;
            }
        }
        file.close();

        if (content.empty())
        {
            result.success = false;
            result.errorMessage = "File is empty: " + filePath;
            return result;
        }

        JSONValue root;
        JSONParser parser(content);
        if (!parser.Parse(root) || root.type != JSONValue::OBJECT)
        {
            result.success = false;
            result.errorMessage = "Failed to parse JSON";
            return result;
        }

        if (!OptionalType(root, "sceneName", JSONValue::STRING) ||
            !OptionalType(root, "description", JSONValue::STRING) || !OptionalUint32(root, "version") ||
            !OptionalUint32(root, "objectCount") || !OptionalUint32(root, "componentCount") ||
            !OptionalUint32(root, "assetReferenceCount") || !OptionalUint64(root, "timestamp") ||
            !OptionalFloatVector(root, "gravity", 3) || !OptionalFloatVector(root, "ambientColor", 4) ||
            !OptionalFloat(root, "ambientIntensity"))
        {
            result.errorMessage = "Scene header fields have invalid JSON types or ranges";
            return result;
        }

        // Build a new scene and publish it only after the entire document has
        // parsed and validated. Failed loads must not append to or partially
        // overwrite the caller's live scene.
        SceneFile loadedScene;

        // Header
        {
            std::string name = FieldStr(root, "sceneName");
            std::string desc = FieldStr(root, "description");
            if (name.find('\0') != std::string::npos || desc.find('\0') != std::string::npos ||
                name.size() >= sizeof(loadedScene.header.sceneName) ||
                desc.size() >= sizeof(loadedScene.header.description))
            {
                result.errorMessage = "Scene name or description contains NUL or exceeds its format capacity";
                return result;
            }
            std::memcpy(loadedScene.header.sceneName, name.data(), name.size());
            loadedScene.header.sceneName[name.size()] = '\0';
            std::memcpy(loadedScene.header.description, desc.data(), desc.size());
            loadedScene.header.description[desc.size()] = '\0';
        }
        const size_t versionFieldCount = static_cast<size_t>(std::count_if(
            root.objVal.begin(), root.objVal.end(), [](const JSONMember& member) { return member.key == "version"; }));
        loadedScene.header.version = FieldUint32(root, "version", SCENE_FILE_VERSION);
        if (versionFieldCount != 1 || loadedScene.header.version != SCENE_FILE_VERSION)
        {
            result.errorMessage = "Scene file version is unsupported; legacy raw-memory scene payloads must be resaved "
                                  "by a trusted build";
            return result;
        }
        loadedScene.header.objectCount = FieldUint32(root, "objectCount");
        loadedScene.header.componentCount = FieldUint32(root, "componentCount");
        loadedScene.header.assetReferenceCount = FieldUint32(root, "assetReferenceCount");
        loadedScene.header.timestamp = FieldUint64(root, "timestamp");
        loadedScene.header.gravity = FieldFloat3(root, "gravity", {0, -9.81f, 0});
        loadedScene.header.ambientColor = FieldFloat4(root, "ambientColor", {0.2f, 0.2f, 0.2f, 1.0f});
        loadedScene.header.ambientIntensity = FieldFloat(root, "ambientIntensity", 1.0f);
        loadedScene.header.magic = SCENE_FILE_MAGIC;

        // Objects
        auto* objectsArr = Field(root, "objects");
        if (objectsArr && objectsArr->type != JSONValue::ARRAY)
        {
            result.errorMessage = "Scene objects must be an array";
            return result;
        }
        if (objectsArr)
        {
            for (const auto& objVal : objectsArr->arrVal)
            {
                if (objVal.type != JSONValue::OBJECT)
                {
                    result.errorMessage = "Every scene object entry must be an object";
                    return result;
                }
                if (!OptionalUint64(objVal, "id") || !OptionalType(objVal, "name", JSONValue::STRING) ||
                    !OptionalType(objVal, "tag", JSONValue::STRING) || !OptionalInt(objVal, "layer") ||
                    !OptionalType(objVal, "active", JSONValue::BOOL) ||
                    !OptionalType(objVal, "staticObject", JSONValue::BOOL) ||
                    !OptionalFloatVector(objVal, "position", 3))
                {
                    result.errorMessage = "Scene object fields have invalid JSON types or ranges";
                    return result;
                }
                SceneObject obj;
                obj.id = FieldUint64(objVal, "id", INVALID_OBJECT_ID);
                obj.name = FieldStr(objVal, "name", "GameObject");
                obj.tag = FieldStr(objVal, "tag", "Default");
                if (!FieldIntChecked(objVal, "layer", 0, obj.layer))
                {
                    result.errorMessage = "Object layer must be an in-range integer";
                    return result;
                }
                obj.active = FieldBool(objVal, "active", true);
                obj.staticObject = FieldBool(objVal, "staticObject", false);

                // Transform
                auto* txVal = Field(objVal, "transform");
                if (txVal && txVal->type != JSONValue::OBJECT)
                {
                    result.errorMessage = "Object transform must be an object";
                    return result;
                }
                if (txVal && txVal->type == JSONValue::OBJECT)
                {
                    if (!OptionalFloatVector(*txVal, "position", 3) || !OptionalFloatVector(*txVal, "rotation", 4) ||
                        !OptionalFloatVector(*txVal, "scale", 3) || !OptionalUint64(*txVal, "parentID"))
                    {
                        result.errorMessage = "Object transform fields have invalid JSON types or ranges";
                        return result;
                    }
                    obj.transform.position = FieldFloat3(*txVal, "position");
                    obj.transform.rotation = FieldFloat4(*txVal, "rotation", {0, 0, 0, 1});
                    obj.transform.scale = FieldFloat3(*txVal, "scale", {1, 1, 1});
                    obj.transform.parentID = FieldUint64(*txVal, "parentID", INVALID_OBJECT_ID);

                    auto* childArr = Field(*txVal, "childIDs");
                    if (childArr && childArr->type == JSONValue::ARRAY)
                    {
                        for (const auto& cid : childArr->arrVal)
                        {
                            uint64_t childID = 0;
                            if (!cid.TryGetUint64(childID))
                            {
                                result.errorMessage = "Object child IDs must be unsigned integers";
                                return result;
                            }
                            obj.transform.childIDs.push_back(childID);
                        }
                    }
                    else if (childArr)
                    {
                        result.errorMessage = "Object childIDs must be an array";
                        return result;
                    }
                }
                else
                {
                    // Legacy format: position as top-level array
                    auto* posArr = Field(objVal, "position");
                    if (posArr)
                        obj.transform.position = posArr->GetFloat3();
                }

                // Component types
                auto* ctArr = Field(objVal, "componentTypes");
                if (ctArr && ctArr->type == JSONValue::ARRAY)
                {
                    for (const auto& ct : ctArr->arrVal)
                    {
                        if (ct.type != JSONValue::STRING)
                        {
                            result.errorMessage = "Object component types must be strings";
                            return result;
                        }
                        ComponentType componentType = ComponentType::CUSTOM;
                        if (!StringToComponentType(ct.GetString(), componentType))
                        {
                            result.errorMessage = "Unknown scene object component type";
                            return result;
                        }
                        obj.componentTypes.push_back(componentType);
                    }
                }
                else if (ctArr)
                {
                    result.errorMessage = "Object componentTypes must be an array";
                    return result;
                }

                loadedScene.objects.push_back(std::move(obj));
            }
        }

        // Components
        auto* compsArr = Field(root, "components");
        if (compsArr && compsArr->type != JSONValue::ARRAY)
        {
            result.errorMessage = "Scene components must be an array";
            return result;
        }
        if (compsArr)
        {
            for (const auto& compVal : compsArr->arrVal)
            {
                if (compVal.type != JSONValue::OBJECT)
                {
                    result.errorMessage = "Every scene component entry must be an object";
                    return result;
                }
                if (!OptionalType(compVal, "type", JSONValue::STRING) || !OptionalUint64(compVal, "objectID") ||
                    !OptionalType(compVal, "enabled", JSONValue::BOOL))
                {
                    result.errorMessage = "Scene component fields have invalid JSON types or ranges";
                    return result;
                }
                Component comp;
                if (!StringToComponentType(FieldStr(compVal, "type"), comp.type))
                {
                    result.errorMessage = "Unknown serialized component type";
                    return result;
                }
                comp.objectID = FieldUint64(compVal, "objectID");
                comp.enabled = FieldBool(compVal, "enabled", true);

                const JSONValue* data = Field(compVal, "data");
                const bool markerOnly =
                    comp.type == ComponentType::TRANSFORM || comp.type == ComponentType::SPRITE_ANIMATOR;
                if (markerOnly)
                {
                    if (data)
                    {
                        result.errorMessage = "Marker-only scene component must not contain data";
                        return result;
                    }
                    loadedScene.components.push_back(std::move(comp));
                    continue;
                }
                if (!HasSceneComponentPayloadCodec(comp.type) || !data || data->type != JSONValue::OBJECT ||
                    data->objVal.size() != 2)
                {
                    result.errorMessage = "Scene component requires a registered schema-tagged data object";
                    return result;
                }

                const JSONValue* schema = data->Find("schema");
                const JSONValue* fields = data->Find("fields");
                uint32_t schemaVersion = 0;
                size_t schemaKeys = 0;
                size_t fieldKeys = 0;
                for (const JSONMember& member : data->objVal)
                {
                    schemaKeys += member.key == "schema" ? 1u : 0u;
                    fieldKeys += member.key == "fields" ? 1u : 0u;
                }
                if (schemaKeys != 1 || fieldKeys != 1 || !schema || !schema->TryGetUint32(schemaVersion) ||
                    schemaVersion != SCENE_COMPONENT_SCHEMA_VERSION || !fields || fields->type != JSONValue::OBJECT)
                {
                    result.errorMessage = "Scene component data schema is invalid or unsupported";
                    return result;
                }

                JSONSceneComponentReader payloadReader(*fields);
                std::string codecError;
                if (!DecodeSceneComponentPayload(comp.type, payloadReader, comp, codecError))
                {
                    result.errorMessage = "Cannot decode " + ComponentTypeToString(comp.type) + ": " + codecError;
                    return result;
                }
                loadedScene.components.push_back(std::move(comp));
            }
        }

        // Environment
        auto* envVal = Field(root, "environment");
        if (envVal && envVal->type != JSONValue::OBJECT)
        {
            result.errorMessage = "Scene environment must be an object";
            return result;
        }
        if (envVal && envVal->type == JSONValue::OBJECT)
        {
            const bool validEnvironment =
                OptionalInt(*envVal, "skyType") && OptionalFloatVector(*envVal, "skyColor", 4) &&
                OptionalFloatVector(*envVal, "horizonColor", 4) &&
                OptionalType(*envVal, "skyboxAssetPath", JSONValue::STRING) &&
                OptionalType(*envVal, "fogEnabled", JSONValue::BOOL) && OptionalFloatVector(*envVal, "fogColor", 4) &&
                OptionalFloat(*envVal, "fogDensity") && OptionalFloat(*envVal, "fogStart") &&
                OptionalFloat(*envVal, "fogEnd") && OptionalFloatVector(*envVal, "windDirection", 3) &&
                OptionalFloat(*envVal, "windStrength") && OptionalFloat(*envVal, "windTurbulence") &&
                OptionalType(*envVal, "bloomEnabled", JSONValue::BOOL) && OptionalFloat(*envVal, "bloomIntensity") &&
                OptionalFloat(*envVal, "bloomThreshold") &&
                OptionalType(*envVal, "tonemappingEnabled", JSONValue::BOOL) && OptionalFloat(*envVal, "exposure") &&
                OptionalFloat(*envVal, "gamma");
            if (!validEnvironment)
            {
                result.errorMessage = "Scene environment fields have invalid JSON types or ranges";
                return result;
            }
            auto& env = loadedScene.environment;
            int skyType = 0;
            if (!FieldIntChecked(*envVal, "skyType", 0, skyType) ||
                skyType < static_cast<int>(EnvironmentSettings::SOLID_COLOR) ||
                skyType > static_cast<int>(EnvironmentSettings::PROCEDURAL))
            {
                result.errorMessage = "Environment skyType must be an in-range integer";
                return result;
            }
            env.skyType = static_cast<EnvironmentSettings::SkyType>(skyType);
            env.skyColor = FieldFloat4(*envVal, "skyColor", {0.5f, 0.8f, 1.0f, 1.0f});
            env.horizonColor = FieldFloat4(*envVal, "horizonColor", {0.9f, 0.9f, 0.9f, 1.0f});
            env.skyboxAssetPath = FieldStr(*envVal, "skyboxAssetPath");
            env.fogEnabled = FieldBool(*envVal, "fogEnabled", false);
            env.fogColor = FieldFloat4(*envVal, "fogColor", {0.7f, 0.7f, 0.7f, 1.0f});
            env.fogDensity = FieldFloat(*envVal, "fogDensity", 0.01f);
            env.fogStart = FieldFloat(*envVal, "fogStart", 10.0f);
            env.fogEnd = FieldFloat(*envVal, "fogEnd", 100.0f);
            env.windDirection = FieldFloat3(*envVal, "windDirection", {1, 0, 0});
            env.windStrength = FieldFloat(*envVal, "windStrength", 1.0f);
            env.windTurbulence = FieldFloat(*envVal, "windTurbulence", 0.1f);
            env.bloomEnabled = FieldBool(*envVal, "bloomEnabled", false);
            env.bloomIntensity = FieldFloat(*envVal, "bloomIntensity", 1.0f);
            env.bloomThreshold = FieldFloat(*envVal, "bloomThreshold", 1.0f);
            env.tonemappingEnabled = FieldBool(*envVal, "tonemappingEnabled", true);
            env.exposure = FieldFloat(*envVal, "exposure", 1.0f);
            env.gamma = FieldFloat(*envVal, "gamma", 2.2f);
        }

        // Default camera
        auto* camVal = Field(root, "defaultCamera");
        if (camVal && camVal->type != JSONValue::OBJECT)
        {
            result.errorMessage = "Scene defaultCamera must be an object";
            return result;
        }
        if (camVal && camVal->type == JSONValue::OBJECT)
        {
            if (!OptionalInt(*camVal, "projectionType") || !OptionalFloat(*camVal, "fieldOfView") ||
                !OptionalFloat(*camVal, "orthographicSize") || !OptionalFloat(*camVal, "nearPlane") ||
                !OptionalFloat(*camVal, "farPlane") || !OptionalFloatVector(*camVal, "clearColor", 4) ||
                !OptionalType(*camVal, "isMainCamera", JSONValue::BOOL) || !OptionalInt(*camVal, "renderTargetWidth") ||
                !OptionalInt(*camVal, "renderTargetHeight"))
            {
                result.errorMessage = "Scene camera fields have invalid JSON types or ranges";
                return result;
            }
            auto& cam = loadedScene.defaultCamera;
            int projectionType = 0;
            if (!FieldIntChecked(*camVal, "projectionType", 0, projectionType) ||
                projectionType < static_cast<int>(Camera::PERSPECTIVE) ||
                projectionType > static_cast<int>(Camera::ORTHOGRAPHIC) ||
                !FieldIntChecked(*camVal, "renderTargetWidth", 1920, cam.renderTargetWidth) ||
                !FieldIntChecked(*camVal, "renderTargetHeight", 1080, cam.renderTargetHeight))
            {
                result.errorMessage = "Camera integer fields must be in range";
                return result;
            }
            cam.projectionType = static_cast<Camera::ProjectionType>(projectionType);
            cam.fieldOfView = FieldFloat(*camVal, "fieldOfView", 75.0f);
            cam.orthographicSize = FieldFloat(*camVal, "orthographicSize", 5.0f);
            cam.nearPlane = FieldFloat(*camVal, "nearPlane", 0.1f);
            cam.farPlane = FieldFloat(*camVal, "farPlane", 1000.0f);
            cam.clearColor = FieldFloat4(*camVal, "clearColor", {0.2f, 0.3f, 0.5f, 1.0f});
            cam.isMainCamera = FieldBool(*camVal, "isMainCamera", false);
        }

        // Asset references
        auto* refsArr = Field(root, "assetReferences");
        if (refsArr && refsArr->type != JSONValue::ARRAY)
        {
            result.errorMessage = "Scene assetReferences must be an array";
            return result;
        }
        if (refsArr)
        {
            for (const auto& refVal : refsArr->arrVal)
            {
                if (refVal.type != JSONValue::OBJECT)
                {
                    result.errorMessage = "Every scene asset reference entry must be an object";
                    return result;
                }
                if (!OptionalType(refVal, "assetPath", JSONValue::STRING) ||
                    !OptionalType(refVal, "assetType", JSONValue::STRING) || !OptionalUint64(refVal, "lastModified") ||
                    !OptionalUint64(refVal, "fileSize") || !OptionalType(refVal, "checksum", JSONValue::STRING))
                {
                    result.errorMessage = "Asset reference fields have invalid JSON types or ranges";
                    return result;
                }
                AssetReference ref;
                ref.assetPath = FieldStr(refVal, "assetPath");
                ref.assetType = FieldStr(refVal, "assetType");
                ref.lastModified = FieldUint64(refVal, "lastModified");
                ref.fileSize = FieldUint64(refVal, "fileSize");
                ref.checksum = FieldStr(refVal, "checksum");
                auto* depsArr = Field(refVal, "dependencies");
                if (depsArr && depsArr->type == JSONValue::ARRAY)
                {
                    for (const auto& dep : depsArr->arrVal)
                    {
                        if (dep.type != JSONValue::STRING)
                        {
                            result.errorMessage = "Asset dependencies must be strings";
                            return result;
                        }
                        ref.dependencies.push_back(dep.GetString());
                    }
                }
                else if (depsArr)
                {
                    result.errorMessage = "Asset dependencies must be an array";
                    return result;
                }
                loadedScene.assetReferences.push_back(std::move(ref));
            }
        }

        const auto declaredCountMatches = [&](const char* fieldName, size_t actualCount)
        {
            const JSONValue* value = Field(root, fieldName);
            if (!value)
                return true;
            uint32_t declaredCount = 0;
            return actualCount <= std::numeric_limits<uint32_t>::max() && value->TryGetUint32(declaredCount) &&
                   declaredCount == actualCount;
        };
        if (!declaredCountMatches("objectCount", loadedScene.objects.size()) ||
            !declaredCountMatches("componentCount", loadedScene.components.size()) ||
            !declaredCountMatches("assetReferenceCount", loadedScene.assetReferences.size()))
        {
            result.errorMessage = "Scene header counts do not match the serialized arrays";
            return result;
        }

        if (!HandleVersionCompatibility(loadedScene.header.version, loadedScene, result))
        {
            result.errorMessage = "Scene file version is not supported by this serializer";
            return result;
        }
        if (!ValidateScene(loadedScene, result))
        {
            result.errorMessage = "Scene data failed validation";
            return result;
        }

        loadedScene.header.objectCount = static_cast<uint32_t>(loadedScene.objects.size());
        loadedScene.header.componentCount = static_cast<uint32_t>(loadedScene.components.size());
        loadedScene.header.assetReferenceCount = static_cast<uint32_t>(loadedScene.assetReferences.size());
        outScene = std::move(loadedScene);

        result.success = true;
        result.bytesProcessed = content.size();
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
