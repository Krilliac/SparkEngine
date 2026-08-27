/**
 * @file TestSubsystemIntegrationScenarios.cpp
 * @brief Integration scenarios that boot real engine subsystems and validate cross-system behavior.
 */

#include "../TestFramework.h"

#include "Core/EngineContext.h"
#include "Engine/ECS/Components.h"
#include "Engine/Editor/SceneSnapshotSerializer.h"
#include "Engine/Networking/ClientPrediction.h"
#include "Engine/Networking/InstabilitySimulator.h"
#include "Engine/Networking/NetworkManager.h"
#include "Graphics/ConstantBufferDiff.h"
#include "Physics/PhysicsSystem.h"
#include "Utils/EventBus.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace DirectX;

namespace
{
    struct SubsystemHarness
    {
        Spark::EventBus eventBus;
        PhysicsSystem physics;

        bool InitializePhysicsAndEcs()
        {
            if (!EngineContext::Get())
            {
                EngineContext::SetOwned(std::make_unique<EngineContext>());
            }

            auto* context = EngineContext::Get();
            context->SetEventBus(&eventBus);

            if (FAILED(physics.Initialize()))
            {
                return false;
            }

            context->SetPhysics(&physics);
            return true;
        }

        ~SubsystemHarness() { physics.Shutdown(); }
    };

    struct PerObjectConstants
    {
        XMFLOAT4X4 world{};
    };

    struct PhysicsEntityBinding
    {
        EntityID entity = entt::null;
        std::shared_ptr<PhysicsBody> body;
    };

    static bool SyncTransformFromPhysics(World& world, const PhysicsEntityBinding& binding)
    {
        if (binding.entity == entt::null || !binding.body)
            return false;

        auto* transform = world.GetComponent<Transform>(binding.entity);
        if (!transform)
            return false;

        const XMFLOAT3 position = binding.body->GetPosition();
        transform->position = position;
        return true;
    }

    static bool UpdateWorldMatrixAndConstants(World& world, EntityID entity,
                                              Spark::Graphics::ConstantBufferDiffManager& cbDiff,
                                              PerObjectConstants& outConstants)
    {
        auto* transform = world.GetComponent<Transform>(entity);
        auto* renderer = world.GetComponent<MeshRenderer>(entity);
        if (!transform || !renderer)
        {
            return false;
        }

        XMMATRIX worldMatrix = transform->GetWorldMatrix(world.GetRegistry());
        XMStoreFloat4x4(&renderer->cachedWorldMatrix, worldMatrix);
        renderer->worldMatrixDirty = false;

        outConstants.world = renderer->cachedWorldMatrix;
        return cbDiff.ShouldUpdate("VS_PerObject", &outConstants, sizeof(outConstants));
    }

    struct ReplicationPacket
    {
        uint32_t entity = 0;
        uint32_t ackInput = 0;
        XMFLOAT3 authoritativePosition{0, 0, 0};
    };

    static std::vector<uint8_t> SerializePacket(const ReplicationPacket& packet)
    {
        std::vector<uint8_t> data(sizeof(ReplicationPacket));
        std::memcpy(data.data(), &packet, sizeof(ReplicationPacket));
        return data;
    }

    static ReplicationPacket DeserializePacket(const std::vector<uint8_t>& data)
    {
        ReplicationPacket packet;
        if (data.size() >= sizeof(ReplicationPacket))
        {
            std::memcpy(&packet, data.data(), sizeof(ReplicationPacket));
        }
        return packet;
    }

    static void RegisterIdentitySnapshotSerializers()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }

        auto& serializers = Spark::Editor::ComponentSerializerRegistry::Instance();

        Spark::Editor::ComponentSerializerEntry transformEntry;
        transformEntry.typeName = "IntegrationTransform";
        transformEntry.typeId = 0x1A0A0001;
        transformEntry.serialize = [](Spark::Editor::SnapshotWriter& writer, const void* registryVoid) -> uint32_t
        {
            auto* registry = static_cast<entt::registry*>(const_cast<void*>(registryVoid));
            if (!registry)
            {
                writer.WriteU32(0);
                return 0;
            }

            auto view = registry->view<Transform>();
            std::vector<entt::entity> entities;
            for (auto entity : view)
            {
                entities.push_back(entity);
            }
            std::sort(entities.begin(), entities.end(), [](entt::entity a, entt::entity b)
                      { return static_cast<uint32_t>(a) < static_cast<uint32_t>(b); });

            writer.WriteU32(static_cast<uint32_t>(entities.size()));
            for (auto entity : entities)
            {
                const auto& t = view.get<Transform>(entity);
                writer.WriteU32(static_cast<uint32_t>(entity));
                writer.WriteFloat3(t.position.x, t.position.y, t.position.z);
                writer.WriteFloat3(t.rotation.x, t.rotation.y, t.rotation.z);
                writer.WriteFloat3(t.scale.x, t.scale.y, t.scale.z);
            }

            return static_cast<uint32_t>(entities.size());
        };

        transformEntry.deserialize = [](Spark::Editor::SnapshotReader& reader, void* registryVoid,
                                        uint32_t /*entityCount*/) -> bool
        {
            auto* registry = static_cast<entt::registry*>(registryVoid);
            if (!registry)
            {
                return false;
            }

            const uint32_t componentCount = reader.ReadU32();
            for (uint32_t i = 0; i < componentCount; ++i)
            {
                const uint32_t entityRaw = reader.ReadU32();
                const entt::entity entity = static_cast<entt::entity>(entityRaw);
                while (!registry->valid(entity))
                {
                    [[maybe_unused]] const auto created = registry->create();
                }

                Transform transform;
                reader.ReadFloat3(transform.position.x, transform.position.y, transform.position.z);
                reader.ReadFloat3(transform.rotation.x, transform.rotation.y, transform.rotation.z);
                reader.ReadFloat3(transform.scale.x, transform.scale.y, transform.scale.z);

                if (registry->all_of<Transform>(entity))
                {
                    registry->remove<Transform>(entity);
                }
                registry->emplace<Transform>(entity, transform);
            }
            return reader.IsValid();
        };

        Spark::Editor::ComponentSerializerEntry nameEntry;
        nameEntry.typeName = "IntegrationName";
        nameEntry.typeId = 0x1A0A0002;
        nameEntry.serialize = [](Spark::Editor::SnapshotWriter& writer, const void* registryVoid) -> uint32_t
        {
            auto* registry = static_cast<entt::registry*>(const_cast<void*>(registryVoid));
            if (!registry)
            {
                writer.WriteU32(0);
                return 0;
            }

            auto view = registry->view<NameComponent>();
            std::vector<entt::entity> entities;
            for (auto entity : view)
            {
                entities.push_back(entity);
            }
            std::sort(entities.begin(), entities.end(), [](entt::entity a, entt::entity b)
                      { return static_cast<uint32_t>(a) < static_cast<uint32_t>(b); });

            writer.WriteU32(static_cast<uint32_t>(entities.size()));
            for (auto entity : entities)
            {
                const auto& name = view.get<NameComponent>(entity);
                writer.WriteU32(static_cast<uint32_t>(entity));
                writer.WriteString(name.name);
            }
            return static_cast<uint32_t>(entities.size());
        };

        nameEntry.deserialize = [](Spark::Editor::SnapshotReader& reader, void* registryVoid,
                                   uint32_t /*entityCount*/) -> bool
        {
            auto* registry = static_cast<entt::registry*>(registryVoid);
            if (!registry)
            {
                return false;
            }

            const uint32_t componentCount = reader.ReadU32();
            for (uint32_t i = 0; i < componentCount; ++i)
            {
                const auto entity = static_cast<entt::entity>(reader.ReadU32());
                while (!registry->valid(entity))
                {
                    [[maybe_unused]] const auto created = registry->create();
                }

                NameComponent name;
                name.name = reader.ReadString();
                if (registry->all_of<NameComponent>(entity))
                {
                    registry->remove<NameComponent>(entity);
                }
                registry->emplace<NameComponent>(entity, std::move(name));
            }
            return reader.IsValid();
        };

        serializers.Register(std::move(transformEntry));
        serializers.Register(std::move(nameEntry));
        registered = true;
    }

    static std::unordered_map<uint32_t, std::string> CollectIdentityMap(entt::registry& registry)
    {
        std::unordered_map<uint32_t, std::string> identity;
        auto nameView = registry.view<NameComponent>();
        for (auto entity : nameView)
        {
            const auto& name = nameView.get<NameComponent>(entity);
            identity[static_cast<uint32_t>(entity)] = name.name;
        }
        return identity;
    }

    static std::unordered_map<uint32_t, XMFLOAT3> CollectPositions(entt::registry& registry)
    {
        std::unordered_map<uint32_t, XMFLOAT3> positions;
        auto transformView = registry.view<Transform>();
        for (auto entity : transformView)
        {
            positions[static_cast<uint32_t>(entity)] = transformView.get<Transform>(entity).position;
        }
        return positions;
    }
} // namespace

TEST(Integration_ECSPhysics_TransformSyncAfterSimulationTicks)
{
    SubsystemHarness harness;
    const bool initialized = harness.InitializePhysicsAndEcs();
    EXPECT_TRUE(initialized);
    if (!initialized)
        return;

    World world;
    auto entity = world.CreateEntity("PhysicsSyncEntity");

    auto& transform = world.AddComponent<Transform>(entity);
    transform.position = {0.0f, 8.0f, 0.0f};

    PhysicsBodyDesc desc;
    desc.type = PhysicsBodyType::Dynamic;
    desc.position = transform.position;
    desc.mass = 1.0f;
    desc.shape.type = CollisionShapeType::Box;
    desc.shape.dimensions = {0.5f, 0.5f, 0.5f};

    PhysicsEntityBinding binding;
    binding.entity = entity;
    binding.body = harness.physics.CreateBody(desc);
    EXPECT_TRUE(binding.body != nullptr);
    if (!binding.body)
        return;

    binding.body->SetLinearVelocity({2.0f, 0.0f, 0.0f});

    const float xBeforeTicks = transform.position.x;
    for (int i = 0; i < 6; ++i)
    {
        harness.physics.Update(1.0f / 60.0f);
        EXPECT_TRUE(SyncTransformFromPhysics(world, binding));
    }

    EXPECT_TRUE(transform.position.x > xBeforeTicks + 0.01f);

    const XMFLOAT3 bodyPosition = binding.body->GetPosition();
    EXPECT_NEAR(transform.position.x, bodyPosition.x, 0.05f);
    EXPECT_NEAR(transform.position.y, bodyPosition.y, 0.05f);
}

TEST(Integration_ECSRendering_ConstantBufferAndWorldMatrixUpdate)
{
    World world;
    auto entity = world.CreateEntity("RenderSyncEntity");

    auto& transform = world.AddComponent<Transform>(entity);
    transform.position = {1.0f, 2.0f, 3.0f};

    auto& renderer = world.AddComponent<MeshRenderer>(entity);
    renderer.meshPath = "models/crate.mesh";
    renderer.materialPath = "materials/default.mat";
    renderer.worldMatrixDirty = true;

    auto& cbDiff = Spark::Graphics::ConstantBufferDiffManager::GetInstance();
    cbDiff.Initialize();

    PerObjectConstants constants{};
    const bool firstUploadNeeded = UpdateWorldMatrixAndConstants(world, entity, cbDiff, constants);
    EXPECT_TRUE(firstUploadNeeded);
    EXPECT_FALSE(renderer.worldMatrixDirty);
    EXPECT_NEAR(constants.world.m[3][0], 1.0f, 0.001f);
    EXPECT_NEAR(constants.world.m[3][1], 2.0f, 0.001f);
    EXPECT_NEAR(constants.world.m[3][2], 3.0f, 0.001f);

    const bool redundantUploadNeeded = UpdateWorldMatrixAndConstants(world, entity, cbDiff, constants);
    EXPECT_FALSE(redundantUploadNeeded);

    transform.position = {9.0f, 4.0f, -2.0f};
    renderer.worldMatrixDirty = true;

    const bool changedUploadNeeded = UpdateWorldMatrixAndConstants(world, entity, cbDiff, constants);
    EXPECT_TRUE(changedUploadNeeded);
    EXPECT_NEAR(constants.world.m[3][0], 9.0f, 0.001f);
    EXPECT_NEAR(constants.world.m[3][1], 4.0f, 0.001f);
    EXPECT_NEAR(constants.world.m[3][2], -2.0f, 0.001f);

    cbDiff.Shutdown();
}

TEST(Integration_NetworkingECS_ReplicationLatencyJitterPredictionReconciliation)
{
    auto& networkManager = Spark::NetworkManager::GetInstance();
    const bool networkInitialized = networkManager.Initialize();
    EXPECT_TRUE(networkInitialized);
    if (!networkInitialized)
        return;

    World world;
    auto entity = world.CreateEntity("ReplicatedPawn");
    auto& transform = world.AddComponent<Transform>(entity);
    world.AddComponent<NetworkIdentity>(entity);

    Spark::Net::ReplicatedEntity replicated{};
    replicated.ownerID = 7;
    replicated.entityType = "IntegrationPawn";
    replicated.position = transform.position;
    const uint32_t networkID = networkManager.RegisterReplicatedEntity(replicated);
    EXPECT_TRUE(networkID != 0);
    if (networkID == 0)
        return;

    Spark::ClientPrediction prediction;
    prediction.SetMaxPendingInputs(64);

    auto& simulator = Spark::Net::InstabilitySimulator::GetInstance();
    simulator.Shutdown(); // Ensure deterministic clean state across test runs.
    Spark::Net::InstabilitySettings instability{};
    instability.enabled = true;
    instability.latencyMs = 90.0f;
    instability.jitterMs = 20.0f;
    instability.packetLossPercent = 0.0f;
    instability.reorderPercent = 0.0f;
    simulator.SetSettings(instability);

    std::deque<float> observedDelays;
    Spark::PredictedState localState;

    for (uint32_t tick = 0; tick < 8; ++tick)
    {
        Spark::PredictedInput input;
        input.moveDirection = {1.0f, 0.0f, 0.0f};
        input.timestamp = static_cast<float>(tick) * (1.0f / 60.0f);

        const uint32_t inputSequence = prediction.RecordInput(input);
        prediction.ApplyPrediction(localState, input, 1.0f / 60.0f);

        ReplicationPacket packet;
        packet.entity = static_cast<uint32_t>(entity);
        packet.ackInput = inputSequence;
        packet.authoritativePosition = {localState.position.x - 0.08f, localState.position.y, localState.position.z};

        const float delayMs = simulator.GetDelayMs();
        observedDelays.push_back(delayMs);

        simulator.QueuePacket(SerializePacket(packet), static_cast<float>(tick) * 16.0f + delayMs);
    }

    float currentTimeMs = 0.0f;
    int deliveredPackets = 0;
    float maxCorrectionMagnitude = 0.0f;
    while (deliveredPackets < 8 && currentTimeMs < 700.0f)
    {
        auto readyPackets = simulator.GetReadyPackets(currentTimeMs);
        for (const auto& delayedPacket : readyPackets)
        {
            ReplicationPacket packet = DeserializePacket(delayedPacket.data);
            Spark::PredictedState serverState = prediction.GetState();
            serverState.position = packet.authoritativePosition;
            serverState.lastProcessedInput = packet.ackInput;
            prediction.Reconcile(serverState, 1.0f / 60.0f);
            maxCorrectionMagnitude = std::max(maxCorrectionMagnitude, prediction.GetLastCorrectionMagnitude());

            const auto& reconciled = prediction.GetState();
            transform.position = reconciled.position;
            deliveredPackets++;
        }
        currentTimeMs += 4.0f;
    }

    EXPECT_EQ(deliveredPackets, 8);
    EXPECT_TRUE(maxCorrectionMagnitude > 0.01f);

    const auto& finalPredicted = prediction.GetState();
    EXPECT_NEAR(transform.position.x, finalPredicted.position.x, 0.0001f);

    float minDelay = 1000.0f;
    float maxDelay = 0.0f;
    for (float delay : observedDelays)
    {
        minDelay = std::min(minDelay, delay);
        maxDelay = std::max(maxDelay, delay);
    }

    EXPECT_TRUE(minDelay >= 69.0f);
    EXPECT_TRUE(maxDelay <= 111.0f);
    EXPECT_TRUE((maxDelay - minDelay) > 0.01f);

    simulator.Shutdown();
    networkManager.Shutdown();
}

TEST(Integration_SceneSaveLoad_RoundTripIdentityForEntityAndComponentSets)
{
    RegisterIdentitySnapshotSerializers();

    World sourceWorld;
    auto first = sourceWorld.CreateEntity("Hero");
    auto second = sourceWorld.CreateEntity("Torch");
    auto third = sourceWorld.CreateEntity("Spawner");

    sourceWorld.AddComponent<Transform>(first).position = {10.0f, 1.0f, -2.0f};
    sourceWorld.AddComponent<Transform>(second).position = {-3.0f, 5.0f, 11.0f};
    sourceWorld.AddComponent<Transform>(third).position = {0.0f, 0.0f, 0.0f};

    const auto sourceIdentity = CollectIdentityMap(sourceWorld.GetRegistry());
    const auto sourcePositions = CollectPositions(sourceWorld.GetRegistry());

    auto snapshot = Spark::Editor::SceneSnapshotSerializer::Serialize(
        &sourceWorld.GetRegistry(), static_cast<uint32_t>(sourceWorld.GetEntityCount()));
    EXPECT_TRUE(!snapshot.empty());
    EXPECT_TRUE(Spark::Editor::SceneSnapshotSerializer::Validate(snapshot));
    if (snapshot.empty())
        return;

    World restoredWorld;
    const bool loaded = Spark::Editor::SceneSnapshotSerializer::Deserialize(snapshot, &restoredWorld.GetRegistry());
    EXPECT_TRUE(loaded);
    if (!loaded)
        return;

    const auto restoredIdentity = CollectIdentityMap(restoredWorld.GetRegistry());
    const auto restoredPositions = CollectPositions(restoredWorld.GetRegistry());

    EXPECT_EQ(restoredIdentity.size(), sourceIdentity.size());
    EXPECT_EQ(restoredPositions.size(), sourcePositions.size());

    for (const auto& [entityId, name] : sourceIdentity)
    {
        auto it = restoredIdentity.find(entityId);
        EXPECT_TRUE(it != restoredIdentity.end());
        if (it == restoredIdentity.end())
            continue;
        EXPECT_EQ(it->second, name);
    }

    for (const auto& [entityId, expectedPos] : sourcePositions)
    {
        auto it = restoredPositions.find(entityId);
        EXPECT_TRUE(it != restoredPositions.end());
        if (it == restoredPositions.end())
            continue;
        EXPECT_NEAR(it->second.x, expectedPos.x, 0.001f);
        EXPECT_NEAR(it->second.y, expectedPos.y, 0.001f);
        EXPECT_NEAR(it->second.z, expectedPos.z, 0.001f);
    }
}
