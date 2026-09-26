/**
 * @file TestMOD350RPGNPCNavigationReal.cpp
 * @brief MOD-350: RPG NPC schedule changes walk NPCs along the engine NavMesh instead of teleporting them
 *
 * Every test drives the production RPGNPCSystem. Area NavMeshes are baked through the engine's
 * NavMeshBuilder (Recast when ENABLE_RECAST is on, the triangle-soup builder otherwise) either from
 * the real RPGWorldSetup areas, exactly as SparkGameRPGModule::OnLoad does, or from test ground with a
 * wall or a gap in it. The clock runs through RPGNPCSystem::Update in short frames so every frame's
 * movement can be checked against the walking speed.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameRPG/Source/NPC/RPGNPCSystem.h"
#include "../GameModules/SparkGameRPG/Source/World/RPGWorldSetup.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace RPG;

namespace
{
    constexpr uint32_t kVillageArea = 1;
    constexpr uint32_t kBlacksmithNpc = 1; // Merchant at (50, 0, 30) until 20:00, then idles at (55, 0, 40)
    constexpr uint32_t kElderNpc = 2;      // Quest giver at (0, 0, 0) until 19:00, then idles at (10, 0, -20)

    constexpr float kFrameSeconds = 0.05f;
    constexpr float kWalkSpeed = 3.0f;       // RPGNPCSystem::WALK_SPEED
    constexpr float kSecondsPerHour = 60.0f; // RPGNPCSystem::SECONDS_PER_GAME_HOUR

    /// Rectangle on the XZ plane.
    struct GroundRect
    {
        float minX;
        float minZ;
        float maxX;
        float maxZ;

        bool Contains(float x, float z) const { return x > minX && x < maxX && z > minZ && z < maxZ; }
    };

    /// Flat ground at y = 0 as a grid of square cells, leaving out every cell inside a blocked rectangle.
    void AppendGround(const GroundRect& ground, float cellSize, const std::vector<GroundRect>& blocked,
                      std::vector<XMFLOAT3>& vertices, std::vector<uint32_t>& indices)
    {
        const int columns = static_cast<int>(std::lround((ground.maxX - ground.minX) / cellSize));
        const int rows = static_cast<int>(std::lround((ground.maxZ - ground.minZ) / cellSize));
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        for (int row = 0; row <= rows; ++row)
        {
            for (int column = 0; column <= columns; ++column)
            {
                vertices.push_back({ground.minX + static_cast<float>(column) * cellSize, 0.0f,
                                    ground.minZ + static_cast<float>(row) * cellSize});
            }
        }

        const auto vertexAt = [&](int column, int row)
        { return base + static_cast<uint32_t>(row * (columns + 1) + column); };
        for (int row = 0; row < rows; ++row)
        {
            for (int column = 0; column < columns; ++column)
            {
                const float centerX = ground.minX + (static_cast<float>(column) + 0.5f) * cellSize;
                const float centerZ = ground.minZ + (static_cast<float>(row) + 0.5f) * cellSize;
                bool isBlocked = false;
                for (const GroundRect& rect : blocked)
                    isBlocked = isBlocked || rect.Contains(centerX, centerZ);
                if (isBlocked)
                    continue;

                // Counter-clockwise seen from above, so the face normal points up (+Y).
                indices.insert(indices.end(),
                               {vertexAt(column, row), vertexAt(column, row + 1), vertexAt(column + 1, row + 1),
                                vertexAt(column, row), vertexAt(column + 1, row + 1), vertexAt(column + 1, row)});
            }
        }
    }

    float Distance2D(float ax, float az, float bx, float bz)
    {
        return std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
    }

    /// Everything one NPC did while the clock ran frame by frame.
    struct Walk
    {
        float largestStep = 0.0f;
        float travelled = 0.0f;
        bool enteredCore = false;
        bool arrived = false;
    };

    /// Tick the system in short frames until the NPC stands within tolerance of the target (or time runs out).
    Walk WalkUntilArrived(RPGNPCSystem& npcs, uint32_t npcId, float targetX, float targetZ, float maxSeconds,
                          const GroundRect* core = nullptr)
    {
        Walk walk;
        const NPCData* npc = npcs.GetNPC(npcId);
        for (float elapsed = 0.0f; elapsed < maxSeconds; elapsed += kFrameSeconds)
        {
            const float beforeX = npc->posX;
            const float beforeZ = npc->posZ;
            npcs.Update(kFrameSeconds);
            const float step = Distance2D(beforeX, beforeZ, npc->posX, npc->posZ);
            walk.largestStep = std::max(walk.largestStep, step);
            walk.travelled += step;
            walk.enteredCore = walk.enteredCore || (core && core->Contains(npc->posX, npc->posZ));
            if (npc->route.empty() && Distance2D(npc->posX, npc->posZ, targetX, targetZ) < 0.25f)
            {
                walk.arrived = true;
                break;
            }
        }
        return walk;
    }
} // namespace

TEST(RPGNPCNavigation_ScheduleChangeWalksAlongWorldNavMesh)
{
    RPGWorldSetup world;
    RPGNPCSystem npcs;
    ASSERT_TRUE(world.Initialize(nullptr));
    ASSERT_TRUE(npcs.Initialize(nullptr));
    ASSERT_TRUE(npcs.BuildAreaNavigation(world.GetAreas()));

    // Run to 19:59 in one frame, then cross 20:00 in walking-sized frames.
    npcs.Update(11.0f * kSecondsPerHour + 59.0f);
    const NPCData* blacksmith = npcs.GetNPC(kBlacksmithNpc);
    ASSERT_TRUE(blacksmith != nullptr);
    EXPECT_TRUE(blacksmith->currentBehavior == NPCBehavior::Merchant);
    EXPECT_NEAR(blacksmith->posX, 50.0f, 0.001f);
    EXPECT_NEAR(blacksmith->posZ, 30.0f, 0.001f);

    npcs.Update(1.0f);
    ASSERT_TRUE(npcs.GetWorldHour() >= 20.0f);
    // The schedule switches behaviour at once but the body still stands at the forge, holding a NavMesh route.
    EXPECT_TRUE(blacksmith->currentBehavior == NPCBehavior::Idle);
    EXPECT_FALSE(blacksmith->route.empty());
    EXPECT_LT(Distance2D(blacksmith->posX, blacksmith->posZ, 50.0f, 30.0f), kWalkSpeed * 1.0f + 0.01f);

    const Walk walk = WalkUntilArrived(npcs, kBlacksmithNpc, 55.0f, 40.0f, 30.0f);
    EXPECT_TRUE(walk.arrived);
    EXPECT_LE(walk.largestStep, kWalkSpeed * kFrameSeconds + 0.001f);
    EXPECT_NEAR(blacksmith->posX, 55.0f, 0.25f);
    EXPECT_NEAR(blacksmith->posZ, 40.0f, 0.25f);

    npcs.Shutdown();
    world.Shutdown();
}

TEST(RPGNPCNavigation_ScheduleRouteDetoursAroundBlockedGround)
{
    RPGNPCSystem npcs;
    ASSERT_TRUE(npcs.Initialize(nullptr));

    // Village ground with a wall straight across the elder's line from (0, 0) to her evening post (10, -20).
    const GroundRect ground{-60.0f, -40.0f, 60.0f, 40.0f};
    const GroundRect wall{-28.0f, -12.0f, 40.0f, -8.0f};
    std::vector<XMFLOAT3> vertices;
    std::vector<uint32_t> indices;
    AppendGround(ground, 4.0f, {wall}, vertices, indices);
    ASSERT_TRUE(npcs.BuildAreaNavMesh(kVillageArea, vertices, indices));

    npcs.Update(10.0f * kSecondsPerHour + 59.0f); // 18:59, the elder still at her quest post
    const NPCData* elder = npcs.GetNPC(kElderNpc);
    ASSERT_TRUE(elder != nullptr);
    EXPECT_NEAR(elder->posX, 0.0f, 0.001f);
    EXPECT_NEAR(elder->posZ, 0.0f, 0.001f);

    // Half a metre inside the wall: no route along the NavMesh may cross it.
    const GroundRect wallCore{wall.minX + 0.5f, wall.minZ + 0.5f, wall.maxX - 0.5f, wall.maxZ - 0.5f};
    const Walk walk = WalkUntilArrived(npcs, kElderNpc, 10.0f, -20.0f, 120.0f, &wallCore);
    EXPECT_TRUE(elder->currentBehavior == NPCBehavior::Idle);
    EXPECT_TRUE(walk.arrived);
    EXPECT_FALSE(walk.enteredCore);
    EXPECT_LE(walk.largestStep, kWalkSpeed * kFrameSeconds + 0.001f);
    // Around the wall's end is far longer than the blocked straight line.
    EXPECT_GT(walk.travelled, 1.5f * Distance2D(0.0f, 0.0f, 10.0f, -20.0f));

    npcs.Shutdown();
}

TEST(RPGNPCNavigation_UnreachablePostKeepsNPCInPlace)
{
    // Two islands: the elder stands on one, her evening post lies on the other.
    RPGNPCSystem islands;
    ASSERT_TRUE(islands.Initialize(nullptr));
    std::vector<XMFLOAT3> vertices;
    std::vector<uint32_t> indices;
    AppendGround({-20.0f, -4.0f, 20.0f, 20.0f}, 4.0f, {}, vertices, indices);
    AppendGround({-20.0f, -40.0f, 20.0f, -16.0f}, 4.0f, {}, vertices, indices);
    ASSERT_TRUE(islands.BuildAreaNavMesh(kVillageArea, vertices, indices));

    islands.Update(10.0f * kSecondsPerHour + 59.0f);
    for (int frame = 0; frame < 200; ++frame)
        islands.Update(kFrameSeconds);
    const NPCData* elder = islands.GetNPC(kElderNpc);
    EXPECT_TRUE(elder->currentBehavior == NPCBehavior::Idle);
    EXPECT_TRUE(elder->route.empty());
    EXPECT_NEAR(elder->posX, 0.0f, 0.001f);
    EXPECT_NEAR(elder->posZ, 0.0f, 0.001f);
    islands.Shutdown();

    // No NavMesh at all: the schedule still changes behaviour, and the NPC still does not teleport.
    RPGNPCSystem unbaked;
    ASSERT_TRUE(unbaked.Initialize(nullptr));
    unbaked.Update(11.0f * kSecondsPerHour + 1.0f);
    const NPCData* elderWithoutMesh = unbaked.GetNPC(kElderNpc);
    EXPECT_TRUE(elderWithoutMesh->currentBehavior == NPCBehavior::Idle);
    EXPECT_NEAR(elderWithoutMesh->posX, 0.0f, 0.001f);
    EXPECT_NEAR(elderWithoutMesh->posZ, 0.0f, 0.001f);
    unbaked.Shutdown();
}

TEST(RPGNPCNavigation_RestoredMidRouteNPCResumesWalk)
{
    RPGWorldSetup world;
    ASSERT_TRUE(world.Initialize(nullptr));

    RPGNPCSystem before;
    ASSERT_TRUE(before.Initialize(nullptr));
    ASSERT_TRUE(before.BuildAreaNavigation(world.GetAreas()));
    before.Update(10.0f * kSecondsPerHour + 59.0f);
    for (int frame = 0; frame < 40; ++frame) // 18:59 + 2 s: the elder is a few metres along her route
        before.Update(kFrameSeconds);
    const NPCData* walking = before.GetNPC(kElderNpc);
    ASSERT_FALSE(walking->route.empty());
    const float savedX = walking->posX;
    const float savedZ = walking->posZ;
    ASSERT_TRUE(Distance2D(savedX, savedZ, 0.0f, 0.0f) > 1.0f);
    const NPCSystemSnapshot saved = before.CaptureState();
    before.Shutdown();

    RPGNPCSystem after;
    ASSERT_TRUE(after.Initialize(nullptr));
    ASSERT_TRUE(after.BuildAreaNavigation(world.GetAreas()));
    ASSERT_TRUE(after.RestoreState(saved));
    const NPCData* resumed = after.GetNPC(kElderNpc);
    EXPECT_NEAR(resumed->posX, savedX, 0.0001f);
    EXPECT_NEAR(resumed->posZ, savedZ, 0.0001f);

    const Walk walk = WalkUntilArrived(after, kElderNpc, 10.0f, -20.0f, 30.0f);
    EXPECT_TRUE(walk.arrived);
    EXPECT_LE(walk.largestStep, kWalkSpeed * kFrameSeconds + 0.001f);

    after.Shutdown();
    world.Shutdown();
}

TEST(RPGNPCNavigation_MissingNPCAreaFailsBuild)
{
    RPGWorldSetup world;
    ASSERT_TRUE(world.Initialize(nullptr));
    std::vector<RPGAreaInfo> withoutSwamp;
    for (const RPGAreaInfo& area : world.GetAreas())
    {
        if (area.areaId != 5) // Bogsworth the swamp merchant lives here
            withoutSwamp.push_back(area);
    }

    RPGNPCSystem npcs;
    ASSERT_TRUE(npcs.Initialize(nullptr));
    EXPECT_FALSE(npcs.BuildAreaNavigation(withoutSwamp));
    EXPECT_FALSE(npcs.BuildAreaNavMesh(kVillageArea, {}, {}));
    npcs.Shutdown();
    world.Shutdown();
}

#endif // SPARK_TEST_HAS_IMGUI
