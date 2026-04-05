/**
 * @file TestCrossSystemIntegration.cpp
 * @brief Integration tests for cross-system interaction
 *
 * Standalone reimplementations for CI testing without engine dependencies.
 */

#include "TestFramework.h"

#include <algorithm>
#include <any>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

// ============================================================================
// Standalone ECS mock
// ============================================================================

namespace IntegrationTest
{

    using EntityId = uint32_t;

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        bool operator==(const Vec3& other) const { return x == other.x && y == other.y && z == other.z; }
    };

    // Simple component storage keyed by (entity, type)
    class ECSWorld
    {
      public:
        EntityId CreateEntity() { return m_nextId++; }

        template <typename T> void AddComponent(EntityId entity, const T& component)
        {
            m_components[{entity, std::type_index(typeid(T))}] = component;
        }

        template <typename T> T* GetComponent(EntityId entity)
        {
            auto it = m_components.find({entity, std::type_index(typeid(T))});
            if (it == m_components.end())
            {
                return nullptr;
            }
            return std::any_cast<T>(&it->second);
        }

        template <typename T> bool HasComponent(EntityId entity) const
        {
            return m_components.count({entity, std::type_index(typeid(T))}) > 0;
        }

        const std::vector<EntityId>& GetEntities() const { return m_entities; }

        EntityId CreateAndTrack()
        {
            auto id = CreateEntity();
            m_entities.push_back(id);
            return id;
        }

      private:
        struct PairHash
        {
            size_t operator()(const std::pair<EntityId, std::type_index>& p) const
            {
                return std::hash<EntityId>{}(p.first) ^ (std::hash<std::type_index>{}(p.second) << 16);
            }
        };

        EntityId m_nextId = 1;
        std::unordered_map<std::pair<EntityId, std::type_index>, std::any, PairHash> m_components;
        std::vector<EntityId> m_entities;
    };

    // Component types
    struct TransformComponent
    {
        Vec3 position;
        Vec3 rotation;
        Vec3 scale{1.0f, 1.0f, 1.0f};
    };

    struct RigidBodyComponent
    {
        Vec3 velocity;
        float mass = 1.0f;
    };

    struct HealthComponent
    {
        int current = 100;
        int max = 100;
    };

    struct NameComponent
    {
        std::string name;
    };

    // ============================================================================
    // System ordering
    // ============================================================================

    struct SystemEntry
    {
        std::string name;
        int priority = 0;
    };

    // ============================================================================
    // Simple event bus
    // ============================================================================

    class SimpleEventBus
    {
      public:
        using HandlerId = uint32_t;

        template <typename EventT> HandlerId Subscribe(std::function<void(const EventT&)> handler)
        {
            auto id = m_nextHandler++;
            auto& handlers = m_handlers[std::type_index(typeid(EventT))];
            handlers.push_back({id, [handler](const std::any& e) { handler(std::any_cast<const EventT&>(e)); }});
            return id;
        }

        template <typename EventT> void Publish(const EventT& event)
        {
            auto it = m_handlers.find(std::type_index(typeid(EventT)));
            if (it == m_handlers.end())
            {
                return;
            }
            for (auto& [id, fn] : it->second)
            {
                fn(event);
            }
        }

      private:
        struct HandlerEntry
        {
            HandlerId id;
            std::function<void(const std::any&)> handler;
        };

        HandlerId m_nextHandler = 1;
        std::unordered_map<std::type_index, std::vector<HandlerEntry>> m_handlers;
    };

    // ============================================================================
    // Simple serialization
    // ============================================================================

    class ByteBuffer
    {
      public:
        void WriteFloat(float v) { WriteRaw(&v, sizeof(v)); }
        void WriteInt(int v) { WriteRaw(&v, sizeof(v)); }
        void WriteUint32(uint32_t v) { WriteRaw(&v, sizeof(v)); }
        void WriteString(const std::string& s)
        {
            auto len = static_cast<uint32_t>(s.size());
            WriteUint32(len);
            WriteRaw(s.data(), len);
        }

        float ReadFloat()
        {
            float v;
            ReadRaw(&v, sizeof(v));
            return v;
        }
        int ReadInt()
        {
            int v;
            ReadRaw(&v, sizeof(v));
            return v;
        }
        uint32_t ReadUint32()
        {
            uint32_t v;
            ReadRaw(&v, sizeof(v));
            return v;
        }
        std::string ReadString()
        {
            auto len = ReadUint32();
            std::string s(len, '\0');
            ReadRaw(s.data(), len);
            return s;
        }

        void ResetRead() { m_readPos = 0; }
        size_t Size() const { return m_data.size(); }

      private:
        void WriteRaw(const void* data, size_t size)
        {
            auto* bytes = static_cast<const uint8_t*>(data);
            m_data.insert(m_data.end(), bytes, bytes + size);
        }

        void ReadRaw(void* data, size_t size)
        {
            std::memcpy(data, m_data.data() + m_readPos, size);
            m_readPos += size;
        }

        std::vector<uint8_t> m_data;
        size_t m_readPos = 0;
    };

} // namespace IntegrationTest

// ============================================================================
// Tests
// ============================================================================

TEST(Integration_PhysicsUpdatesTransform)
{
    using namespace IntegrationTest;

    ECSWorld world;
    auto entity = world.CreateAndTrack();

    TransformComponent transform;
    transform.position = {0.0f, 0.0f, 0.0f};
    world.AddComponent(entity, transform);

    RigidBodyComponent body;
    body.velocity = {1.0f, 2.0f, 0.5f};
    body.mass = 1.0f;
    world.AddComponent(entity, body);

    // Simulate one physics step (dt = 1.0)
    float dt = 1.0f;
    for (auto id : world.GetEntities())
    {
        auto* rb = world.GetComponent<RigidBodyComponent>(id);
        auto* tf = world.GetComponent<TransformComponent>(id);
        if (rb && tf)
        {
            tf->position.x += rb->velocity.x * dt;
            tf->position.y += rb->velocity.y * dt;
            tf->position.z += rb->velocity.z * dt;
        }
    }

    auto* result = world.GetComponent<TransformComponent>(entity);
    EXPECT_NEAR(result->position.x, 1.0f, 0.001f);
    EXPECT_NEAR(result->position.y, 2.0f, 0.001f);
    EXPECT_NEAR(result->position.z, 0.5f, 0.001f);
}

TEST(Integration_ECSSystemExecutionOrder)
{
    using namespace IntegrationTest;

    // Documented order: Physics(0) -> Animation(1) -> AI(2) -> Audio(3) -> Lifecycle(4) -> Render(5)
    std::vector<SystemEntry> systems = {
        {"Render", 5}, {"AI", 2}, {"Physics", 0}, {"Audio", 3}, {"Animation", 1}, {"Lifecycle", 4},
    };

    std::sort(systems.begin(), systems.end(),
              [](const SystemEntry& a, const SystemEntry& b) { return a.priority < b.priority; });

    EXPECT_TRUE(systems[0].name == "Physics");
    EXPECT_TRUE(systems[1].name == "Animation");
    EXPECT_TRUE(systems[2].name == "AI");
    EXPECT_TRUE(systems[3].name == "Audio");
    EXPECT_TRUE(systems[4].name == "Lifecycle");
    EXPECT_TRUE(systems[5].name == "Render");
}

TEST(Integration_SceneRoundtrip)
{
    using namespace IntegrationTest;

    // Create 10 entities with varying component data
    struct EntityData
    {
        Vec3 position;
        int health;
        std::string name;
    };

    std::vector<EntityData> original;
    for (int i = 0; i < 10; i++)
    {
        original.push_back({
            {static_cast<float>(i * 10), static_cast<float>(i * 20), static_cast<float>(i * 30)},
            100 - i * 5,
            "Entity_" + std::to_string(i),
        });
    }

    // Serialize
    ByteBuffer buffer;
    buffer.WriteUint32(static_cast<uint32_t>(original.size()));
    for (const auto& e : original)
    {
        buffer.WriteFloat(e.position.x);
        buffer.WriteFloat(e.position.y);
        buffer.WriteFloat(e.position.z);
        buffer.WriteInt(e.health);
        buffer.WriteString(e.name);
    }

    // Deserialize
    buffer.ResetRead();
    auto count = buffer.ReadUint32();
    EXPECT_EQ(count, 10u);

    std::vector<EntityData> loaded;
    for (uint32_t i = 0; i < count; i++)
    {
        EntityData e;
        e.position.x = buffer.ReadFloat();
        e.position.y = buffer.ReadFloat();
        e.position.z = buffer.ReadFloat();
        e.health = buffer.ReadInt();
        e.name = buffer.ReadString();
        loaded.push_back(e);
    }

    // Verify all match
    for (int i = 0; i < 10; i++)
    {
        EXPECT_TRUE(original[i].position == loaded[i].position);
        EXPECT_EQ(original[i].health, loaded[i].health);
        EXPECT_TRUE(original[i].name == loaded[i].name);
    }
}

TEST(Integration_EventBusCrossSystem)
{
    using namespace IntegrationTest;

    struct CollisionEvent
    {
        EntityId entityA;
        EntityId entityB;
        Vec3 contactPoint;
    };

    SimpleEventBus bus;

    // System A: damage system
    int damageSystemCalls = 0;
    EntityId lastDamagedA = 0;
    EntityId lastDamagedB = 0;

    bus.Subscribe<CollisionEvent>(
        [&](const CollisionEvent& e)
        {
            damageSystemCalls++;
            lastDamagedA = e.entityA;
            lastDamagedB = e.entityB;
        });

    // System B: audio system
    int audioSystemCalls = 0;
    Vec3 lastSoundPos;

    bus.Subscribe<CollisionEvent>(
        [&](const CollisionEvent& e)
        {
            audioSystemCalls++;
            lastSoundPos = e.contactPoint;
        });

    // Fire collision event
    bus.Publish(CollisionEvent{1, 2, {5.0f, 10.0f, 0.0f}});

    EXPECT_EQ(damageSystemCalls, 1);
    EXPECT_EQ(audioSystemCalls, 1);
    EXPECT_EQ(lastDamagedA, 1u);
    EXPECT_EQ(lastDamagedB, 2u);
    EXPECT_NEAR(lastSoundPos.x, 5.0f, 0.001f);
    EXPECT_NEAR(lastSoundPos.y, 10.0f, 0.001f);
}
