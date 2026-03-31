/**
 * @file TestSceneSerializer.cpp
 * @brief Tests for SceneSerializer: round-trip serialization of game objects,
 *        components, hierarchies, and scene properties.
 *
 * Standalone reimplementation — mirrors SparkEditor scene serialization
 * without engine/editor dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        bool operator==(const Vec3& o) const
        {
            return std::abs(x - o.x) < 0.001f && std::abs(y - o.y) < 0.001f && std::abs(z - o.z) < 0.001f;
        }
    };

    using EntityID = uint32_t;

    // --- Component types for serialization ---

    struct TransformData
    {
        Vec3 position{};
        Vec3 rotation{};
        Vec3 scale{1, 1, 1};
    };

    struct MeshComponentData
    {
        std::string meshPath;
        std::string materialPath;
        bool castShadows = true;
    };

    struct LightComponentData
    {
        enum class LightType
        {
            Point,
            Directional,
            Spot
        };
        LightType type = LightType::Point;
        float intensity = 1.0f;
        Vec3 color{1, 1, 1};
        float range = 10.0f;
    };

    // --- Serializable entity ---

    struct SerializedEntity
    {
        EntityID id = 0;
        std::string name;
        EntityID parentId = 0;
        TransformData transform;
        bool hasMesh = false;
        MeshComponentData mesh;
        bool hasLight = false;
        LightComponentData light;
        bool active = true;
    };

    // --- Scene data ---

    struct SceneData
    {
        std::string sceneName;
        std::string version = "1.0";
        Vec3 ambientColor{0.1f, 0.1f, 0.15f};
        float gravity = -9.81f;
        std::vector<SerializedEntity> entities;
    };

    // --- Simple scene serializer (key=value format) ---

    class SceneSerializer
    {
      public:
        std::string Serialize(const SceneData& scene) const
        {
            std::ostringstream out;

            out << "[Scene]\n";
            out << "name=" << scene.sceneName << "\n";
            out << "version=" << scene.version << "\n";
            out << "ambientR=" << scene.ambientColor.x << "\n";
            out << "ambientG=" << scene.ambientColor.y << "\n";
            out << "ambientB=" << scene.ambientColor.z << "\n";
            out << "gravity=" << scene.gravity << "\n";
            out << "entityCount=" << scene.entities.size() << "\n";

            for (size_t i = 0; i < scene.entities.size(); i++)
            {
                const auto& e = scene.entities[i];
                std::string prefix = "entity" + std::to_string(i) + ".";

                out << "\n[Entity" << i << "]\n";
                out << prefix << "id=" << e.id << "\n";
                out << prefix << "name=" << e.name << "\n";
                out << prefix << "parentId=" << e.parentId << "\n";
                out << prefix << "active=" << (e.active ? "true" : "false") << "\n";
                out << prefix << "posX=" << e.transform.position.x << "\n";
                out << prefix << "posY=" << e.transform.position.y << "\n";
                out << prefix << "posZ=" << e.transform.position.z << "\n";
                out << prefix << "rotX=" << e.transform.rotation.x << "\n";
                out << prefix << "rotY=" << e.transform.rotation.y << "\n";
                out << prefix << "rotZ=" << e.transform.rotation.z << "\n";
                out << prefix << "scaleX=" << e.transform.scale.x << "\n";
                out << prefix << "scaleY=" << e.transform.scale.y << "\n";
                out << prefix << "scaleZ=" << e.transform.scale.z << "\n";
                out << prefix << "hasMesh=" << (e.hasMesh ? "true" : "false") << "\n";

                if (e.hasMesh)
                {
                    out << prefix << "meshPath=" << e.mesh.meshPath << "\n";
                    out << prefix << "materialPath=" << e.mesh.materialPath << "\n";
                    out << prefix << "castShadows=" << (e.mesh.castShadows ? "true" : "false") << "\n";
                }

                out << prefix << "hasLight=" << (e.hasLight ? "true" : "false") << "\n";
                if (e.hasLight)
                {
                    out << prefix << "lightType=" << static_cast<int>(e.light.type) << "\n";
                    out << prefix << "lightIntensity=" << e.light.intensity << "\n";
                    out << prefix << "lightRange=" << e.light.range << "\n";
                }
            }

            return out.str();
        }

        SceneData Deserialize(const std::string& data) const
        {
            SceneData scene;
            auto lines = SplitLines(data);
            std::unordered_map<std::string, std::string> kvs;

            for (auto& line : lines)
            {
                if (line.empty() || line[0] == '[')
                    continue;
                auto eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                kvs[line.substr(0, eq)] = line.substr(eq + 1);
            }

            scene.sceneName = GetStr(kvs, "name");
            scene.version = GetStr(kvs, "version", "1.0");
            scene.ambientColor.x = GetFloat(kvs, "ambientR", 0.1f);
            scene.ambientColor.y = GetFloat(kvs, "ambientG", 0.1f);
            scene.ambientColor.z = GetFloat(kvs, "ambientB", 0.15f);
            scene.gravity = GetFloat(kvs, "gravity", -9.81f);

            int entityCount = GetInt(kvs, "entityCount", 0);
            for (int i = 0; i < entityCount; i++)
            {
                std::string p = "entity" + std::to_string(i) + ".";
                SerializedEntity e;
                e.id = static_cast<EntityID>(GetInt(kvs, p + "id", 0));
                e.name = GetStr(kvs, p + "name");
                e.parentId = static_cast<EntityID>(GetInt(kvs, p + "parentId", 0));
                e.active = GetBool(kvs, p + "active", true);
                e.transform.position = {GetFloat(kvs, p + "posX"), GetFloat(kvs, p + "posY"),
                                        GetFloat(kvs, p + "posZ")};
                e.transform.rotation = {GetFloat(kvs, p + "rotX"), GetFloat(kvs, p + "rotY"),
                                        GetFloat(kvs, p + "rotZ")};
                e.transform.scale = {GetFloat(kvs, p + "scaleX", 1.0f), GetFloat(kvs, p + "scaleY", 1.0f),
                                     GetFloat(kvs, p + "scaleZ", 1.0f)};
                e.hasMesh = GetBool(kvs, p + "hasMesh", false);
                if (e.hasMesh)
                {
                    e.mesh.meshPath = GetStr(kvs, p + "meshPath");
                    e.mesh.materialPath = GetStr(kvs, p + "materialPath");
                    e.mesh.castShadows = GetBool(kvs, p + "castShadows", true);
                }
                e.hasLight = GetBool(kvs, p + "hasLight", false);
                if (e.hasLight)
                {
                    e.light.type = static_cast<LightComponentData::LightType>(GetInt(kvs, p + "lightType", 0));
                    e.light.intensity = GetFloat(kvs, p + "lightIntensity", 1.0f);
                    e.light.range = GetFloat(kvs, p + "lightRange", 10.0f);
                }
                scene.entities.push_back(e);
            }

            return scene;
        }

      private:
        std::vector<std::string> SplitLines(const std::string& data) const
        {
            std::vector<std::string> lines;
            std::istringstream stream(data);
            std::string line;
            while (std::getline(stream, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                    line.pop_back();
                lines.push_back(line);
            }
            return lines;
        }

        std::string GetStr(const std::unordered_map<std::string, std::string>& kvs, const std::string& key,
                           const std::string& def = "") const
        {
            auto it = kvs.find(key);
            return (it != kvs.end()) ? it->second : def;
        }

        float GetFloat(const std::unordered_map<std::string, std::string>& kvs, const std::string& key,
                       float def = 0.0f) const
        {
            auto it = kvs.find(key);
            if (it == kvs.end())
                return def;
            try
            {
                return std::stof(it->second);
            }
            catch (...)
            {
                return def;
            }
        }

        int GetInt(const std::unordered_map<std::string, std::string>& kvs, const std::string& key, int def = 0) const
        {
            auto it = kvs.find(key);
            if (it == kvs.end())
                return def;
            try
            {
                return std::stoi(it->second);
            }
            catch (...)
            {
                return def;
            }
        }

        bool GetBool(const std::unordered_map<std::string, std::string>& kvs, const std::string& key,
                     bool def = false) const
        {
            auto it = kvs.find(key);
            if (it == kvs.end())
                return def;
            return it->second == "true" || it->second == "1";
        }
    };

} // anonymous namespace

// ============================================================================
// Round-Trip Serialization Tests
// ============================================================================

TEST(SceneSerializer_EmptyScene)
{
    SceneSerializer serializer;
    SceneData original;
    original.sceneName = "EmptyScene";

    auto data = serializer.Serialize(original);
    auto restored = serializer.Deserialize(data);

    EXPECT_EQ(restored.sceneName, std::string("EmptyScene"));
    EXPECT_EQ(restored.entities.size(), (size_t)0);
}

TEST(SceneSerializer_SceneProperties)
{
    SceneSerializer serializer;
    SceneData original;
    original.sceneName = "TestLevel";
    original.version = "2.0";
    original.gravity = -15.0f;
    original.ambientColor = {0.2f, 0.3f, 0.4f};

    auto data = serializer.Serialize(original);
    auto restored = serializer.Deserialize(data);

    EXPECT_EQ(restored.sceneName, std::string("TestLevel"));
    EXPECT_EQ(restored.version, std::string("2.0"));
    EXPECT_NEAR(restored.gravity, -15.0f, 0.01f);
    EXPECT_NEAR(restored.ambientColor.x, 0.2f, 0.01f);
    EXPECT_NEAR(restored.ambientColor.y, 0.3f, 0.01f);
    EXPECT_NEAR(restored.ambientColor.z, 0.4f, 0.01f);
}

TEST(SceneSerializer_SingleEntity)
{
    SceneSerializer serializer;
    SceneData original;
    original.sceneName = "Test";

    SerializedEntity entity;
    entity.id = 1;
    entity.name = "Player";
    entity.transform.position = {10, 20, 30};
    entity.transform.rotation = {0, 90, 0};
    entity.transform.scale = {1, 1, 1};
    original.entities.push_back(entity);

    auto data = serializer.Serialize(original);
    auto restored = serializer.Deserialize(data);

    EXPECT_EQ(restored.entities.size(), (size_t)1);
    EXPECT_EQ(restored.entities[0].id, static_cast<EntityID>(1));
    EXPECT_EQ(restored.entities[0].name, std::string("Player"));
    EXPECT_NEAR(restored.entities[0].transform.position.x, 10.0f, 0.01f);
    EXPECT_NEAR(restored.entities[0].transform.position.y, 20.0f, 0.01f);
    EXPECT_NEAR(restored.entities[0].transform.position.z, 30.0f, 0.01f);
    EXPECT_NEAR(restored.entities[0].transform.rotation.y, 90.0f, 0.01f);
}

TEST(SceneSerializer_EntityWithMesh)
{
    SceneSerializer serializer;
    SceneData original;
    original.sceneName = "Test";

    SerializedEntity entity;
    entity.id = 1;
    entity.name = "Cube";
    entity.hasMesh = true;
    entity.mesh.meshPath = "models/cube.obj";
    entity.mesh.materialPath = "materials/default.mat";
    entity.mesh.castShadows = true;
    original.entities.push_back(entity);

    auto data = serializer.Serialize(original);
    auto restored = serializer.Deserialize(data);

    EXPECT_TRUE(restored.entities[0].hasMesh);
    EXPECT_EQ(restored.entities[0].mesh.meshPath, std::string("models/cube.obj"));
    EXPECT_EQ(restored.entities[0].mesh.materialPath, std::string("materials/default.mat"));
    EXPECT_TRUE(restored.entities[0].mesh.castShadows);
}

TEST(SceneSerializer_EntityWithLight)
{
    SceneSerializer serializer;
    SceneData original;
    original.sceneName = "Test";

    SerializedEntity entity;
    entity.id = 2;
    entity.name = "SunLight";
    entity.hasLight = true;
    entity.light.type = LightComponentData::LightType::Directional;
    entity.light.intensity = 2.5f;
    entity.light.range = 100.0f;
    original.entities.push_back(entity);

    auto data = serializer.Serialize(original);
    auto restored = serializer.Deserialize(data);

    EXPECT_TRUE(restored.entities[0].hasLight);
    EXPECT_EQ(static_cast<int>(restored.entities[0].light.type),
              static_cast<int>(LightComponentData::LightType::Directional));
    EXPECT_NEAR(restored.entities[0].light.intensity, 2.5f, 0.01f);
    EXPECT_NEAR(restored.entities[0].light.range, 100.0f, 0.1f);
}

TEST(SceneSerializer_MultipleEntities)
{
    SceneSerializer serializer;
    SceneData original;
    original.sceneName = "TestLevel";

    for (int i = 0; i < 5; i++)
    {
        SerializedEntity entity;
        entity.id = static_cast<EntityID>(i + 1);
        entity.name = "Entity" + std::to_string(i);
        entity.transform.position = {static_cast<float>(i * 10), 0, 0};
        original.entities.push_back(entity);
    }

    auto data = serializer.Serialize(original);
    auto restored = serializer.Deserialize(data);

    EXPECT_EQ(restored.entities.size(), (size_t)5);
    for (int i = 0; i < 5; i++)
    {
        EXPECT_EQ(restored.entities[i].name, "Entity" + std::to_string(i));
        EXPECT_NEAR(restored.entities[i].transform.position.x, static_cast<float>(i * 10), 0.1f);
    }
}

TEST(SceneSerializer_ParentChildRelationship)
{
    SceneSerializer serializer;
    SceneData original;
    original.sceneName = "Test";

    SerializedEntity parent;
    parent.id = 1;
    parent.name = "Parent";
    parent.parentId = 0;
    original.entities.push_back(parent);

    SerializedEntity child;
    child.id = 2;
    child.name = "Child";
    child.parentId = 1;
    original.entities.push_back(child);

    auto data = serializer.Serialize(original);
    auto restored = serializer.Deserialize(data);

    EXPECT_EQ(restored.entities[0].parentId, static_cast<EntityID>(0));
    EXPECT_EQ(restored.entities[1].parentId, static_cast<EntityID>(1));
}

TEST(SceneSerializer_InactiveEntity)
{
    SceneSerializer serializer;
    SceneData original;
    original.sceneName = "Test";

    SerializedEntity entity;
    entity.id = 1;
    entity.name = "Hidden";
    entity.active = false;
    original.entities.push_back(entity);

    auto data = serializer.Serialize(original);
    auto restored = serializer.Deserialize(data);

    EXPECT_FALSE(restored.entities[0].active);
}

// ============================================================================
// Serialization Format Tests
// ============================================================================

TEST(SceneSerializer_OutputContainsSceneName)
{
    SceneSerializer serializer;
    SceneData scene;
    scene.sceneName = "MyLevel";

    auto data = serializer.Serialize(scene);
    EXPECT_TRUE(data.find("name=MyLevel") != std::string::npos);
}

TEST(SceneSerializer_OutputContainsEntityCount)
{
    SceneSerializer serializer;
    SceneData scene;
    scene.sceneName = "Test";

    SerializedEntity e;
    e.id = 1;
    e.name = "Obj1";
    scene.entities.push_back(e);

    auto data = serializer.Serialize(scene);
    EXPECT_TRUE(data.find("entityCount=1") != std::string::npos);
}

// ============================================================================
// Transform Round-Trip Precision Tests
// ============================================================================

TEST(SceneSerializer_TransformPrecision)
{
    SceneSerializer serializer;
    SceneData original;
    original.sceneName = "Test";

    SerializedEntity entity;
    entity.id = 1;
    entity.name = "PrecisionTest";
    entity.transform.position = {123.456f, -78.901f, 0.001f};
    entity.transform.rotation = {45.5f, 180.25f, -90.0f};
    entity.transform.scale = {0.5f, 2.0f, 1.5f};
    original.entities.push_back(entity);

    auto data = serializer.Serialize(original);
    auto restored = serializer.Deserialize(data);

    auto& t = restored.entities[0].transform;
    EXPECT_NEAR(t.position.x, 123.456f, 0.01f);
    EXPECT_NEAR(t.position.y, -78.901f, 0.01f);
    EXPECT_NEAR(t.position.z, 0.001f, 0.001f);
    EXPECT_NEAR(t.rotation.x, 45.5f, 0.1f);
    EXPECT_NEAR(t.rotation.y, 180.25f, 0.1f);
    EXPECT_NEAR(t.scale.x, 0.5f, 0.01f);
    EXPECT_NEAR(t.scale.y, 2.0f, 0.01f);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(SceneSerializer_EmptyEntityName)
{
    SceneSerializer serializer;
    SceneData original;
    original.sceneName = "Test";

    SerializedEntity entity;
    entity.id = 1;
    entity.name = "";
    original.entities.push_back(entity);

    auto data = serializer.Serialize(original);
    auto restored = serializer.Deserialize(data);

    EXPECT_EQ(restored.entities.size(), (size_t)1);
    EXPECT_EQ(restored.entities[0].name, std::string(""));
}

TEST(SceneSerializer_ZeroScale)
{
    SceneSerializer serializer;
    SceneData original;
    original.sceneName = "Test";

    SerializedEntity entity;
    entity.id = 1;
    entity.name = "ZeroScale";
    entity.transform.scale = {0, 0, 0};
    original.entities.push_back(entity);

    auto data = serializer.Serialize(original);
    auto restored = serializer.Deserialize(data);

    EXPECT_NEAR(restored.entities[0].transform.scale.x, 0.0f, 0.01f);
}
