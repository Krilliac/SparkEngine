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
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cctype>

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
        for (char c : s)
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
            default:
                out += c;
                break;
            }
        }
        return out;
    }

    static std::string ComponentTypeToString(ComponentType type)
    {
        switch (type)
        {
        case ComponentType::TRANSFORM:
            return "Transform";
        case ComponentType::MESH_RENDERER:
            return "MeshRenderer";
        case ComponentType::LIGHT:
            return "Light";
        case ComponentType::CAMERA:
            return "Camera";
        case ComponentType::RIGID_BODY:
            return "RigidBody";
        case ComponentType::COLLIDER:
            return "Collider";
        case ComponentType::AUDIO_SOURCE:
            return "AudioSource";
        case ComponentType::SCRIPT:
            return "Script";
        case ComponentType::PARTICLE_SYSTEM:
            return "ParticleSystem";
        case ComponentType::ANIMATION:
            return "Animation";
        case ComponentType::TERRAIN:
            return "Terrain";
        default:
            return "Custom_" + std::to_string(static_cast<uint32_t>(type));
        }
    }

    static ComponentType StringToComponentType(const std::string& s)
    {
        if (s == "Transform")
            return ComponentType::TRANSFORM;
        if (s == "MeshRenderer")
            return ComponentType::MESH_RENDERER;
        if (s == "Light")
            return ComponentType::LIGHT;
        if (s == "Camera")
            return ComponentType::CAMERA;
        if (s == "RigidBody")
            return ComponentType::RIGID_BODY;
        if (s == "Collider")
            return ComponentType::COLLIDER;
        if (s == "AudioSource")
            return ComponentType::AUDIO_SOURCE;
        if (s == "Script")
            return ComponentType::SCRIPT;
        if (s == "ParticleSystem")
            return ComponentType::PARTICLE_SYSTEM;
        if (s == "Animation")
            return ComponentType::ANIMATION;
        if (s == "Terrain")
            return ComponentType::TERRAIN;
        return ComponentType::CUSTOM;
    }

    static std::string BytesToHex(const std::vector<uint8_t>& data)
    {
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (uint8_t b : data)
            ss << std::setw(2) << static_cast<int>(b);
        return ss.str();
    }

    static std::vector<uint8_t> HexToBytes(const std::string& hex)
    {
        std::vector<uint8_t> bytes;
        bytes.reserve(hex.size() / 2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2)
        {
            try
            {
                uint8_t b = static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16));
                bytes.push_back(b);
            }
            catch (const std::exception&)
            {
                // Malformed hex digit pair — stop parsing
                break;
            }
        }
        return bytes;
    }

    // Write helpers for indented JSON
    class JSONWriter
    {
      public:
        explicit JSONWriter(std::ostream& os, bool pretty = true) : m_os(os), m_pretty(pretty) {}

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

    SerializationResult SceneSerializer::SaveJSON(const SceneFile& scene, const std::string& filePath)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Saving scene (JSON) to: %s (%zu objects)", filePath.c_str(),
                       scene.objects.size());
        SerializationResult result;

        std::ofstream file(filePath);
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
        w.KV("sceneName", std::string(scene.header.sceneName));
        w.KV("version", scene.header.version);
        w.KV("description", std::string(scene.header.description));
        w.KV("objectCount", scene.header.objectCount);
        w.KV("componentCount", scene.header.componentCount);
        w.KV("assetReferenceCount", scene.header.assetReferenceCount);
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
            if (!comp.data.empty())
            {
                w.KV("data", BytesToHex(comp.data));
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
            std::filesystem::remove(filePath, ec);
            return result;
        }

        result.success = true;
        result.bytesProcessed = static_cast<size_t>(file.tellp());
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
            double numVal = 0.0;
            bool boolVal = false;
            JSONObject objVal;
            std::vector<JSONValue> arrVal;

            std::string GetString(const std::string& def = "") const { return type == STRING ? strVal : def; }
            double GetNumber(double def = 0.0) const { return type == NUMBER ? numVal : def; }
            float GetFloat(float def = 0.0f) const { return static_cast<float>(GetNumber(def)); }
            int GetInt(int def = 0) const { return static_cast<int>(GetNumber(def)); }
            uint32_t GetUint32(uint32_t def = 0) const { return static_cast<uint32_t>(GetNumber(def)); }
            uint64_t GetUint64(uint64_t def = 0) const { return static_cast<uint64_t>(GetNumber(def)); }
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
                return ParseValue(out);
            }

          private:
            void SkipWhitespace()
            {
                while (m_pos < m_input.size() && std::isspace(static_cast<unsigned char>(m_input[m_pos])))
                    m_pos++;
            }

            char Peek() { return m_pos < m_input.size() ? m_input[m_pos] : '\0'; }
            char Next() { return m_pos < m_input.size() ? m_input[m_pos++] : '\0'; }

            bool ParseValue(JSONValue& val)
            {
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
                        val.type = JSONValue::STRING;
                        val.strVal = s;
                        return true;
                    }
                    if (c == '\\')
                    {
                        char esc = Next();
                        switch (esc)
                        {
                        case '"':
                            s += '"';
                            break;
                        case '\\':
                            s += '\\';
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
                        default:
                            s += esc;
                            break;
                        }
                    }
                    else
                    {
                        s += c;
                    }
                }
                return false;
            }

            bool ParseNumber(JSONValue& val)
            {
                size_t start = m_pos;
                if (Peek() == '-')
                    m_pos++;
                while (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                    m_pos++;
                if (m_pos < m_input.size() && m_input[m_pos] == '.')
                {
                    m_pos++;
                    while (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                        m_pos++;
                }
                if (m_pos < m_input.size() && (m_input[m_pos] == 'e' || m_input[m_pos] == 'E'))
                {
                    m_pos++;
                    if (m_pos < m_input.size() && (m_input[m_pos] == '+' || m_input[m_pos] == '-'))
                        m_pos++;
                    while (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                        m_pos++;
                }
                val.type = JSONValue::NUMBER;
                val.numVal = std::stod(m_input.substr(start, m_pos - start));
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
                    if (!ParseString(key))
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
        };

        // Helper to read a field safely
        static const JSONValue* Field(const JSONValue& obj, const std::string& key)
        {
            return obj.Find(key);
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
        static int FieldInt(const JSONValue& obj, const std::string& key, int def = 0)
        {
            auto* v = obj.Find(key);
            return v ? v->GetInt(def) : def;
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

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
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

        // Header
        {
            std::string name = FieldStr(root, "sceneName");
            std::strncpy(outScene.header.sceneName, name.c_str(), sizeof(outScene.header.sceneName) - 1);
            outScene.header.sceneName[sizeof(outScene.header.sceneName) - 1] = '\0';
            std::string desc = FieldStr(root, "description");
            std::strncpy(outScene.header.description, desc.c_str(), sizeof(outScene.header.description) - 1);
            outScene.header.description[sizeof(outScene.header.description) - 1] = '\0';
        }
        outScene.header.version = FieldUint32(root, "version", SCENE_FILE_VERSION);
        outScene.header.objectCount = FieldUint32(root, "objectCount");
        outScene.header.componentCount = FieldUint32(root, "componentCount");
        outScene.header.assetReferenceCount = FieldUint32(root, "assetReferenceCount");
        outScene.header.timestamp = FieldUint64(root, "timestamp");
        outScene.header.gravity = FieldFloat3(root, "gravity", {0, -9.81f, 0});
        outScene.header.ambientColor = FieldFloat4(root, "ambientColor", {0.2f, 0.2f, 0.2f, 1.0f});
        outScene.header.ambientIntensity = FieldFloat(root, "ambientIntensity", 1.0f);
        outScene.header.magic = SCENE_FILE_MAGIC;

        // Objects
        auto* objectsArr = Field(root, "objects");
        if (objectsArr && objectsArr->type == JSONValue::ARRAY)
        {
            for (const auto& objVal : objectsArr->arrVal)
            {
                if (objVal.type != JSONValue::OBJECT)
                    continue;
                SceneObject obj;
                obj.id = FieldUint64(objVal, "id", INVALID_OBJECT_ID);
                obj.name = FieldStr(objVal, "name", "GameObject");
                obj.tag = FieldStr(objVal, "tag", "Default");
                obj.layer = FieldInt(objVal, "layer", 0);
                obj.active = FieldBool(objVal, "active", true);
                obj.staticObject = FieldBool(objVal, "staticObject", false);

                // Transform
                auto* txVal = Field(objVal, "transform");
                if (txVal && txVal->type == JSONValue::OBJECT)
                {
                    obj.transform.position = FieldFloat3(*txVal, "position");
                    obj.transform.rotation = FieldFloat4(*txVal, "rotation", {0, 0, 0, 1});
                    obj.transform.scale = FieldFloat3(*txVal, "scale", {1, 1, 1});
                    obj.transform.parentID = FieldUint64(*txVal, "parentID", INVALID_OBJECT_ID);

                    auto* childArr = Field(*txVal, "childIDs");
                    if (childArr && childArr->type == JSONValue::ARRAY)
                    {
                        for (const auto& cid : childArr->arrVal)
                            obj.transform.childIDs.push_back(cid.GetUint64());
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
                        obj.componentTypes.push_back(StringToComponentType(ct.GetString()));
                }

                outScene.objects.push_back(std::move(obj));
            }
        }

        // Components
        auto* compsArr = Field(root, "components");
        if (compsArr && compsArr->type == JSONValue::ARRAY)
        {
            for (const auto& compVal : compsArr->arrVal)
            {
                if (compVal.type != JSONValue::OBJECT)
                    continue;
                Component comp;
                comp.type = StringToComponentType(FieldStr(compVal, "type"));
                comp.objectID = FieldUint64(compVal, "objectID");
                comp.enabled = FieldBool(compVal, "enabled", true);
                std::string hexData = FieldStr(compVal, "data");
                if (!hexData.empty())
                    comp.data = HexToBytes(hexData);
                outScene.components.push_back(std::move(comp));
            }
        }

        // Environment
        auto* envVal = Field(root, "environment");
        if (envVal && envVal->type == JSONValue::OBJECT)
        {
            auto& env = outScene.environment;
            env.skyType = static_cast<EnvironmentSettings::SkyType>(FieldInt(*envVal, "skyType", 0));
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
        if (camVal && camVal->type == JSONValue::OBJECT)
        {
            auto& cam = outScene.defaultCamera;
            cam.projectionType = static_cast<Camera::ProjectionType>(FieldInt(*camVal, "projectionType", 0));
            cam.fieldOfView = FieldFloat(*camVal, "fieldOfView", 75.0f);
            cam.orthographicSize = FieldFloat(*camVal, "orthographicSize", 5.0f);
            cam.nearPlane = FieldFloat(*camVal, "nearPlane", 0.1f);
            cam.farPlane = FieldFloat(*camVal, "farPlane", 1000.0f);
            cam.clearColor = FieldFloat4(*camVal, "clearColor", {0.2f, 0.3f, 0.5f, 1.0f});
            cam.isMainCamera = FieldBool(*camVal, "isMainCamera", false);
            cam.renderTargetWidth = FieldInt(*camVal, "renderTargetWidth", 1920);
            cam.renderTargetHeight = FieldInt(*camVal, "renderTargetHeight", 1080);
        }

        // Asset references
        auto* refsArr = Field(root, "assetReferences");
        if (refsArr && refsArr->type == JSONValue::ARRAY)
        {
            for (const auto& refVal : refsArr->arrVal)
            {
                if (refVal.type != JSONValue::OBJECT)
                    continue;
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
                        ref.dependencies.push_back(dep.GetString());
                }
                outScene.assetReferences.push_back(std::move(ref));
            }
        }

        HandleVersionCompatibility(outScene.header.version, outScene, result);

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
        auto* json = new std::string();
        std::ostringstream ss;
        ss << "{\"type\":\"" << ComponentTypeToString(component.type) << "\"," << "\"objectID\":" << component.objectID
           << "," << "\"enabled\":" << (component.enabled ? "true" : "false");
        if (!component.data.empty())
        {
            ss << ",\"data\":\"" << BytesToHex(component.data) << "\"";
        }
        ss << "}";
        *json = ss.str();
        return json;
    }

    bool SceneSerializer::JSONToComponent(void* json, Component& component)
    {
        if (!json)
            return false;
        const auto* str = static_cast<const std::string*>(json);
        if (str->empty())
            return false;

        // Extract type string
        auto typePos = str->find("\"type\":\"");
        if (typePos != std::string::npos)
        {
            typePos += 8;
            auto endPos = str->find('"', typePos);
            if (endPos != std::string::npos)
            {
                std::string typeStr = str->substr(typePos, endPos - typePos);
                component.type = StringToComponentType(typeStr);
            }
        }

        // Extract objectID
        auto oidPos = str->find("\"objectID\":");
        if (oidPos != std::string::npos)
        {
            char* end = nullptr;
            component.objectID = std::strtoull(str->c_str() + oidPos + 11, &end, 10);
        }

        // Extract enabled
        auto enPos = str->find("\"enabled\":");
        if (enPos != std::string::npos)
        {
            component.enabled = (str->find("true", enPos + 10) == enPos + 10);
        }

        // Extract data hex string
        auto dataPos = str->find("\"data\":\"");
        if (dataPos != std::string::npos)
        {
            dataPos += 8;
            auto endPos = str->find('"', dataPos);
            if (endPos != std::string::npos)
            {
                std::string hexStr = str->substr(dataPos, endPos - dataPos);
                component.data = HexToBytes(hexStr);
            }
        }
        return true;
    }

} // namespace SparkEditor
