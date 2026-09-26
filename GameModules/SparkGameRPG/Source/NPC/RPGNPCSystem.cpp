/**
 * @file RPGNPCSystem.cpp
 * @brief NPC behavior, schedule updates, NavMesh schedule travel, patrol movement, and disposition
 */

#include "RPGNPCSystem.h"
#include "World/RPGWorldSetup.h"
#include "Engine/AI/NavMesh.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <sstream>

namespace RPG
{

    namespace
    {
        /// NPCs stand on the module's ground plane: every NPC and schedule post is authored at y = 0.
        constexpr float kGroundHeight = 0.0f;

        /// How far off the NavMesh an NPC or its post may be and still be snapped onto it.
        constexpr float kNavMeshSnapRadius = 5.0f;

        float HorizontalDistance(float fromX, float fromZ, float toX, float toZ)
        {
            const float dx = toX - fromX;
            const float dz = toZ - fromZ;
            return std::sqrt(dx * dx + dz * dz);
        }

        /// Index of the schedule entry covering hour, or -1 when none does.
        int FindScheduleEntry(const std::vector<NPCScheduleEntry>& schedule, float hour)
        {
            for (size_t index = 0; index < schedule.size(); ++index)
            {
                const NPCScheduleEntry& entry = schedule[index];
                // Ranges with start > end wrap past midnight (e.g. 20-6).
                const bool inRange = entry.startHour < entry.endHour
                                         ? (hour >= entry.startHour && hour < entry.endHour)
                                         : (hour >= entry.startHour || hour < entry.endHour);
                if (inRange)
                    return static_cast<int>(index);
            }
            return -1;
        }
    } // namespace

    /// One area's baked NavMesh and the query every NPC of that area plans with.
    struct RPGNPCSystem::AreaNavigation
    {
        std::unique_ptr<Spark::AI::NavMeshData> navMesh;
        std::unique_ptr<Spark::AI::NavMeshQuery> query;
    };

    RPGNPCSystem::RPGNPCSystem() = default;
    RPGNPCSystem::~RPGNPCSystem() = default;

    bool RPGNPCSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterDefaultNPCs();

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG NPC system initialized with %zu NPCs", m_npcs.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] NPC system initialized (" + std::to_string(m_npcs.size()) +
                                                    " NPCs)");
        return true;
    }

    void RPGNPCSystem::Update(float deltaTime)
    {
        deltaTime = std::max(0.0f, deltaTime);

        // Advance world time
        m_worldTime += deltaTime;
        // fmod keeps the hour in [0, 24) even across a frame longer than a game day.
        m_worldHour = std::fmod(m_worldHour + deltaTime / SECONDS_PER_GAME_HOUR, 24.0f);

        UpdateSchedules();
        // Patrols skip NPCs still on a schedule route, so a route that ends this frame is not walked twice.
        UpdatePatrols(deltaTime);
        UpdateRoutes(deltaTime);
    }

    void RPGNPCSystem::Shutdown()
    {
        m_npcs.clear();
        m_areaNavigation.clear();
    }

    // === Navigation ===

    bool RPGNPCSystem::BuildAreaNavigation(const std::vector<RPGAreaInfo>& areas)
    {
        std::vector<uint32_t> npcAreas;
        for (const auto& [id, npc] : m_npcs)
        {
            if (std::find(npcAreas.begin(), npcAreas.end(), npc.areaId) == npcAreas.end())
                npcAreas.push_back(npc.areaId);
        }
        std::sort(npcAreas.begin(), npcAreas.end());

        bool allBuilt = true;
        for (const uint32_t areaId : npcAreas)
        {
            const auto area = std::find_if(areas.begin(), areas.end(),
                                           [areaId](const RPGAreaInfo& info) { return info.areaId == areaId; });
            if (area == areas.end() || area->boundsMinY > kGroundHeight || area->boundsMaxY < kGroundHeight ||
                !(area->boundsMaxX > area->boundsMinX) || !(area->boundsMaxZ > area->boundsMinZ))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "RPG NPC area %u has no ground to navigate", areaId);
                allBuilt = false;
                continue;
            }

            // One quad over the area footprint, wound counter-clockwise seen from above (+Y normal).
            const std::vector<XMFLOAT3> vertices = {
                {area->boundsMinX, kGroundHeight, area->boundsMinZ},
                {area->boundsMinX, kGroundHeight, area->boundsMaxZ},
                {area->boundsMaxX, kGroundHeight, area->boundsMaxZ},
                {area->boundsMaxX, kGroundHeight, area->boundsMinZ},
            };
            const std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};
            allBuilt = BuildAreaNavMesh(areaId, vertices, indices) && allBuilt;
        }
        return allBuilt;
    }

    bool RPGNPCSystem::BuildAreaNavMesh(uint32_t areaId, const std::vector<XMFLOAT3>& vertices,
                                        const std::vector<uint32_t>& indices)
    {
        if (vertices.empty() || indices.size() < 3)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "RPG NavMesh bake for area %u has no geometry", areaId);
            return false;
        }

        // Area footprints span up to two kilometres. Cells grow with the footprint so the voxel grid
        // stays near kMaxBakeCells per side (a bake takes milliseconds, not seconds), edges stay
        // unsplit (edgeMaxLen 0) and detail heights are sampled no closer than the footprint itself.
        // That keeps the triangle count, and with it the builder's pairwise adjacency pass and every
        // linear query, small. The detail distance must stay >= 0.9 cells: below that the Recast
        // backend turns sampling off, and Recast's detail pass then triangulates an empty hull.
        float minX = vertices.front().x;
        float maxX = minX;
        float minZ = vertices.front().z;
        float maxZ = minZ;
        for (const XMFLOAT3& vertex : vertices)
        {
            minX = std::min(minX, vertex.x);
            maxX = std::max(maxX, vertex.x);
            minZ = std::min(minZ, vertex.z);
            maxZ = std::max(maxZ, vertex.z);
        }
        constexpr float kMaxBakeCells = 400.0f;
        Spark::AI::NavMeshBuildSettings settings;
        settings.cellSize = std::max(1.0f, std::max(maxX - minX, maxZ - minZ) / kMaxBakeCells);
        settings.cellHeight = 0.2f;
        settings.edgeMaxLen = 0.0f;
        settings.detailSampleDist = 4096.0f;

        auto navigation = std::make_unique<AreaNavigation>();
        navigation->navMesh = Spark::AI::NavMeshBuilder::Build(vertices, indices, settings);
        if (!navigation->navMesh || navigation->navMesh->triangles.empty())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "RPG NavMesh bake for area %u produced no walkable surface",
                            areaId);
            return false;
        }
        navigation->query = std::make_unique<Spark::AI::NavMeshQuery>(navigation->navMesh.get());

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG NavMesh for area %u: %zu triangles", areaId,
                       navigation->navMesh->triangles.size());
        m_areaNavigation[areaId] = std::move(navigation);

        // Routes planned on the replaced NavMesh are stale: replan from where each NPC stands.
        for (auto& [id, npc] : m_npcs)
        {
            if (npc.areaId != areaId)
                continue;
            npc.route.clear();
            npc.routeIndex = 0;
            npc.activeScheduleEntry = -1;
        }
        return true;
    }

    bool RPGNPCSystem::PlanRoute(NPCData& npc, const XMFLOAT3& destination)
    {
        npc.route.clear();
        npc.routeIndex = 0;

        const auto navigation = m_areaNavigation.find(npc.areaId);
        if (navigation == m_areaNavigation.end())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "RPG NPC %s has no NavMesh in area %u; staying put",
                           npc.name.c_str(), npc.areaId);
            return false;
        }

        const Spark::AI::NavMeshQuery& query = *navigation->second->query;
        const Spark::AI::NavMeshHit from = query.FindNearestPoint({npc.posX, npc.posY, npc.posZ}, kNavMeshSnapRadius);
        const Spark::AI::NavMeshHit to = query.FindNearestPoint(destination, kNavMeshSnapRadius);
        if (!from.hit || !to.hit)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "RPG NPC %s or its post is off the area %u NavMesh; staying put",
                           npc.name.c_str(), npc.areaId);
            return false;
        }

        Spark::AI::PathRequest request;
        request.start = from.position;
        request.end = to.position;
        const Spark::AI::PathResult path = query.FindPath(request);
        if (!path.found || path.path.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "RPG NPC %s has no NavMesh path to its post; staying put",
                           npc.name.c_str());
            return false;
        }

        npc.route.reserve(path.path.size());
        for (const Spark::AI::PathPoint& point : path.path)
            npc.route.push_back(point.position);
        return true;
    }

    void RPGNPCSystem::RegisterDefaultNPCs()
    {
        // === Village NPCs (Area 1) ===

        // Blacksmith — merchant by day, idle at night
        NPCData blacksmith;
        blacksmith.npcId = 1;
        blacksmith.name = "Grimjaw the Blacksmith";
        blacksmith.areaId = 1;
        blacksmith.currentBehavior = NPCBehavior::Merchant;
        blacksmith.disposition = NPCDisposition::Friendly;
        blacksmith.dispositionValue = 60;
        blacksmith.posX = 50.0f;
        blacksmith.posY = 0.0f;
        blacksmith.posZ = 30.0f;
        blacksmith.dialogueTreeId = 1;
        blacksmith.shopItems = {10, 11, 20, 21, 30, 31, 60, 70, 80};
        blacksmith.schedule = {
            {6.0f, 20.0f, NPCBehavior::Merchant, 50.0f, 0.0f, 30.0f},
            {20.0f, 6.0f, NPCBehavior::Idle, 55.0f, 0.0f, 40.0f},
        };
        m_npcs[1] = blacksmith;

        // Village Elder — quest giver
        NPCData elder;
        elder.npcId = 2;
        elder.name = "Elder Mirwen";
        elder.areaId = 1;
        elder.currentBehavior = NPCBehavior::QuestGiver;
        elder.disposition = NPCDisposition::Friendly;
        elder.dispositionValue = 70;
        elder.posX = 0.0f;
        elder.posY = 0.0f;
        elder.posZ = 0.0f;
        elder.dialogueTreeId = 2;
        elder.questId = 1;
        elder.schedule = {
            {7.0f, 19.0f, NPCBehavior::QuestGiver, 0.0f, 0.0f, 0.0f},
            {19.0f, 7.0f, NPCBehavior::Idle, 10.0f, 0.0f, -20.0f},
        };
        m_npcs[2] = elder;

        // Village Guard — patrols by day, guards gate at night
        NPCData guard;
        guard.npcId = 3;
        guard.name = "Captain Aldric";
        guard.areaId = 1;
        guard.currentBehavior = NPCBehavior::Patrol;
        guard.disposition = NPCDisposition::Neutral;
        guard.dispositionValue = 50;
        guard.posX = -50.0f;
        guard.posY = 0.0f;
        guard.posZ = 100.0f;
        guard.patrolPath = {
            {-50.0f, 0.0f, 100.0f, 3.0f},
            {50.0f, 0.0f, 100.0f, 2.0f},
            {50.0f, 0.0f, -100.0f, 3.0f},
            {-50.0f, 0.0f, -100.0f, 2.0f},
        };
        guard.schedule = {
            {6.0f, 22.0f, NPCBehavior::Patrol, -50.0f, 0.0f, 100.0f},
            {22.0f, 6.0f, NPCBehavior::Guard, 0.0f, 0.0f, 180.0f},
        };
        m_npcs[3] = guard;

        // Herbalist — merchant, sells potions
        NPCData herbalist;
        herbalist.npcId = 4;
        herbalist.name = "Liana the Herbalist";
        herbalist.areaId = 1;
        herbalist.currentBehavior = NPCBehavior::Merchant;
        herbalist.disposition = NPCDisposition::Friendly;
        herbalist.dispositionValue = 65;
        herbalist.posX = -30.0f;
        herbalist.posY = 0.0f;
        herbalist.posZ = -50.0f;
        herbalist.shopItems = {1, 2, 3, 4};
        herbalist.schedule = {
            {8.0f, 18.0f, NPCBehavior::Merchant, -30.0f, 0.0f, -50.0f},
            {18.0f, 8.0f, NPCBehavior::Idle, -35.0f, 0.0f, -45.0f},
        };
        m_npcs[4] = herbalist;

        // === Forest NPCs (Area 2) ===

        // Hermit — quest giver, reclusive
        NPCData hermit;
        hermit.npcId = 5;
        hermit.name = "Old Theron";
        hermit.areaId = 2;
        hermit.currentBehavior = NPCBehavior::QuestGiver;
        hermit.disposition = NPCDisposition::Neutral;
        hermit.dispositionValue = 40;
        hermit.posX = 800.0f;
        hermit.posY = 0.0f;
        hermit.posZ = -200.0f;
        hermit.questId = 3;
        m_npcs[5] = hermit;

        // Forest Ranger — patrols the woods
        NPCData ranger;
        ranger.npcId = 6;
        ranger.name = "Ranger Elara";
        ranger.areaId = 2;
        ranger.currentBehavior = NPCBehavior::Patrol;
        ranger.disposition = NPCDisposition::Friendly;
        ranger.dispositionValue = 55;
        ranger.posX = 500.0f;
        ranger.posY = 0.0f;
        ranger.posZ = 0.0f;
        ranger.patrolPath = {
            {500.0f, 0.0f, 0.0f, 4.0f},
            {700.0f, 0.0f, 200.0f, 3.0f},
            {900.0f, 0.0f, -100.0f, 4.0f},
            {700.0f, 0.0f, -300.0f, 3.0f},
        };
        m_npcs[6] = ranger;

        // === Swamp NPCs (Area 5) ===

        // Swamp Merchant — trades rare ingredients
        NPCData swampMerchant;
        swampMerchant.npcId = 7;
        swampMerchant.name = "Bogsworth";
        swampMerchant.areaId = 5;
        swampMerchant.currentBehavior = NPCBehavior::Merchant;
        swampMerchant.disposition = NPCDisposition::Neutral;
        swampMerchant.dispositionValue = 45;
        swampMerchant.posX = -350.0f;
        swampMerchant.posY = 0.0f;
        swampMerchant.posZ = -300.0f;
        swampMerchant.shopItems = {3, 4, 51};
        swampMerchant.questId = 5;
        m_npcs[7] = swampMerchant;
    }

    // === NPC queries ===

    NPCData* RPGNPCSystem::GetNPC(uint32_t npcId)
    {
        auto it = m_npcs.find(npcId);
        return it != m_npcs.end() ? &it->second : nullptr;
    }

    const NPCData* RPGNPCSystem::GetNPC(uint32_t npcId) const
    {
        auto it = m_npcs.find(npcId);
        return it != m_npcs.end() ? &it->second : nullptr;
    }

    std::vector<const NPCData*> RPGNPCSystem::GetNPCsInArea(uint32_t areaId) const
    {
        std::vector<const NPCData*> result;
        for (const auto& [id, npc] : m_npcs)
        {
            if (npc.areaId == areaId)
                result.push_back(&npc);
        }
        return result;
    }

    std::string RPGNPCSystem::GetNPCListString() const
    {
        std::ostringstream ss;
        ss << "=== RPG NPCs ===\n";
        ss << "World Hour: " << static_cast<int>(m_worldHour) << ":"
           << static_cast<int>((m_worldHour - static_cast<int>(m_worldHour)) * 60) << "\n";

        for (const auto& [id, npc] : m_npcs)
        {
            ss << "[" << id << "] " << npc.name << " (Area " << npc.areaId << ") - ";

            switch (npc.currentBehavior)
            {
            case NPCBehavior::Idle:
                ss << "Idle";
                break;
            case NPCBehavior::Patrol:
                ss << "Patrol";
                break;
            case NPCBehavior::Guard:
                ss << "Guard";
                break;
            case NPCBehavior::Merchant:
                ss << "Merchant";
                break;
            case NPCBehavior::QuestGiver:
                ss << "QuestGiver";
                break;
            default:
                ss << "Unknown";
                break;
            }

            ss << " ["
               << (npc.disposition == NPCDisposition::Friendly  ? "Friendly"
                   : npc.disposition == NPCDisposition::Neutral ? "Neutral"
                                                                : "Hostile")
               << "]\n";
        }
        return ss.str();
    }

    // === Disposition ===

    void RPGNPCSystem::AdjustDisposition(uint32_t npcId, int change)
    {
        auto* npc = GetNPC(npcId);
        if (!npc)
            return;

        npc->dispositionValue += change;

        // Clamp to 0-100
        if (npc->dispositionValue < 0)
            npc->dispositionValue = 0;
        if (npc->dispositionValue > 100)
            npc->dispositionValue = 100;

        npc->disposition = GetDispositionTier(npc->dispositionValue);

        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "RPG NPC %s disposition changed to %d", npc->name.c_str(),
                        npc->dispositionValue);
        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] " + npc->name +
                                                    " disposition: " + std::to_string(npc->dispositionValue));
    }

    NPCDisposition RPGNPCSystem::GetDispositionTier(int value) const
    {
        if (value < 25)
            return NPCDisposition::Hostile;
        if (value < 60)
            return NPCDisposition::Neutral;
        return NPCDisposition::Friendly;
    }

    // === Persistence ===

    NPCSystemSnapshot RPGNPCSystem::CaptureState() const
    {
        NPCSystemSnapshot snapshot;
        snapshot.worldTime = m_worldTime;
        snapshot.worldHour = m_worldHour;
        snapshot.npcs.reserve(m_npcs.size());
        for (const auto& [id, npc] : m_npcs)
        {
            snapshot.npcs.push_back({id, npc.dispositionValue, npc.currentBehavior, npc.posX, npc.posY, npc.posZ,
                                     npc.currentWaypointIndex, npc.waypointWaitTimer});
        }
        // m_npcs is unordered; sort so identical state always serializes identically.
        std::sort(snapshot.npcs.begin(), snapshot.npcs.end(),
                  [](const NPCPersistentState& left, const NPCPersistentState& right)
                  { return left.npcId < right.npcId; });
        return snapshot;
    }

    NPCSystemSnapshot RPGNPCSystem::CaptureDefaultState()
    {
        // Registering on a private instance yields the defaults without Initialize()'s logging or context.
        RPGNPCSystem defaults;
        defaults.RegisterDefaultNPCs();
        return defaults.CaptureState();
    }

    bool RPGNPCSystem::ValidateState(const NPCSystemSnapshot& snapshot) const
    {
        if (!std::isfinite(snapshot.worldTime) || snapshot.worldTime < 0.0f || !std::isfinite(snapshot.worldHour) ||
            snapshot.worldHour < 0.0f || snapshot.worldHour >= 24.0f || snapshot.npcs.size() != m_npcs.size())
            return false;

        std::vector<uint32_t> seen;
        seen.reserve(snapshot.npcs.size());
        for (const NPCPersistentState& state : snapshot.npcs)
        {
            const auto npc = m_npcs.find(state.npcId);
            if (npc == m_npcs.end() || std::find(seen.begin(), seen.end(), state.npcId) != seen.end())
                return false;
            seen.push_back(state.npcId);

            // A patrol index must address the NPC's own path; NPCs without a path always sit at 0.
            const int waypointLimit = std::max(1, static_cast<int>(npc->second.patrolPath.size()));
            if (state.dispositionValue < 0 || state.dispositionValue > 100 || state.behavior >= NPCBehavior::Count ||
                !std::isfinite(state.posX) || !std::isfinite(state.posY) || !std::isfinite(state.posZ) ||
                state.currentWaypointIndex < 0 || state.currentWaypointIndex >= waypointLimit ||
                !std::isfinite(state.waypointWaitTimer) || state.waypointWaitTimer < 0.0f)
                return false;
        }
        return true;
    }

    bool RPGNPCSystem::RestoreState(const NPCSystemSnapshot& snapshot)
    {
        if (!ValidateState(snapshot))
            return false;

        m_worldTime = snapshot.worldTime;
        m_worldHour = snapshot.worldHour;
        for (const NPCPersistentState& state : snapshot.npcs)
        {
            NPCData& npc = m_npcs.at(state.npcId);
            npc.dispositionValue = state.dispositionValue;
            npc.disposition = GetDispositionTier(state.dispositionValue);
            npc.currentBehavior = state.behavior;
            npc.posX = state.posX;
            npc.posY = state.posY;
            npc.posZ = state.posZ;
            npc.currentWaypointIndex = state.currentWaypointIndex;
            npc.waypointWaitTimer = state.waypointWaitTimer;
            // Routes are not saved: the next update replans from the restored position toward the current post.
            npc.route.clear();
            npc.routeIndex = 0;
            npc.activeScheduleEntry = -1;
        }
        return true;
    }

    // === Internal updates ===

    void RPGNPCSystem::UpdateSchedules()
    {
        for (auto& [id, npc] : m_npcs)
        {
            const int entryIndex = FindScheduleEntry(npc.schedule, m_worldHour);
            if (entryIndex < 0 || entryIndex == npc.activeScheduleEntry)
                continue;

            npc.activeScheduleEntry = entryIndex;
            const NPCScheduleEntry& entry = npc.schedule[static_cast<size_t>(entryIndex)];
            npc.currentBehavior = entry.behavior;

            // A patrolling NPC resumes its round at the waypoint it was heading for; others go to the entry's post.
            XMFLOAT3 destination{entry.posX, entry.posY, entry.posZ};
            if (entry.behavior == NPCBehavior::Patrol && !npc.patrolPath.empty())
            {
                const PatrolWaypoint& waypoint = npc.patrolPath[static_cast<size_t>(npc.currentWaypointIndex)];
                destination = {waypoint.x, waypoint.y, waypoint.z};
            }

            if (HorizontalDistance(npc.posX, npc.posZ, destination.x, destination.z) > ARRIVAL_DISTANCE)
            {
                PlanRoute(npc, destination);
            }
            else
            {
                npc.route.clear();
                npc.routeIndex = 0;
            }
        }
    }

    void RPGNPCSystem::UpdateRoutes(float deltaTime)
    {
        for (auto& [id, npc] : m_npcs)
        {
            float budget = WALK_SPEED * deltaTime;
            while (npc.routeIndex < npc.route.size())
            {
                const XMFLOAT3& target = npc.route[npc.routeIndex];
                const float dx = target.x - npc.posX;
                const float dy = target.y - npc.posY;
                const float dz = target.z - npc.posZ;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (dist <= budget)
                {
                    npc.posX = target.x;
                    npc.posY = target.y;
                    npc.posZ = target.z;
                    budget -= dist;
                    ++npc.routeIndex;
                    continue;
                }

                npc.posX += dx / dist * budget;
                npc.posY += dy / dist * budget;
                npc.posZ += dz / dist * budget;
                break;
            }

            if (!npc.route.empty() && npc.routeIndex >= npc.route.size())
            {
                npc.route.clear();
                npc.routeIndex = 0;
            }
        }
    }

    void RPGNPCSystem::UpdatePatrols(float deltaTime)
    {
        for (auto& [id, npc] : m_npcs)
        {
            if (npc.currentBehavior != NPCBehavior::Patrol || npc.patrolPath.empty() || !npc.route.empty())
                continue;

            auto& wp = npc.patrolPath[npc.currentWaypointIndex];

            // Simple move-toward-waypoint
            float dx = wp.x - npc.posX;
            float dz = wp.z - npc.posZ;
            float dist = std::sqrt(dx * dx + dz * dz);

            if (dist < 1.0f)
            {
                // At waypoint — wait
                npc.waypointWaitTimer -= deltaTime;
                if (npc.waypointWaitTimer <= 0.0f)
                {
                    npc.currentWaypointIndex = (npc.currentWaypointIndex + 1) % static_cast<int>(npc.patrolPath.size());
                    auto& nextWp = npc.patrolPath[npc.currentWaypointIndex];
                    npc.waypointWaitTimer = nextWp.waitTime;
                }
            }
            else
            {
                // Move toward waypoint
                float step = WALK_SPEED * deltaTime;
                if (step > dist)
                    step = dist;

                npc.posX += (dx / dist) * step;
                npc.posZ += (dz / dist) * step;
            }
        }
    }

    void RPGNPCSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RPG NPC System"))
        {
            ImGui::Text("NPCs: %zu | World Hour: %.1f", m_npcs.size(), m_worldHour);

            for (const auto& [id, npc] : m_npcs)
            {
                ImGui::PushID(static_cast<int>(id));
                if (ImGui::TreeNode(npc.name.c_str()))
                {
                    ImGui::Text("Area: %u | Pos: (%.1f, %.1f, %.1f)", npc.areaId, npc.posX, npc.posY, npc.posZ);

                    const char* behaviorStr = "Unknown";
                    switch (npc.currentBehavior)
                    {
                    case NPCBehavior::Idle:
                        behaviorStr = "Idle";
                        break;
                    case NPCBehavior::Patrol:
                        behaviorStr = "Patrol";
                        break;
                    case NPCBehavior::Guard:
                        behaviorStr = "Guard";
                        break;
                    case NPCBehavior::Merchant:
                        behaviorStr = "Merchant";
                        break;
                    case NPCBehavior::QuestGiver:
                        behaviorStr = "QuestGiver";
                        break;
                    default:
                        break;
                    }
                    ImGui::Text("Behavior: %s | Disposition: %d", behaviorStr, npc.dispositionValue);

                    if (!npc.shopItems.empty())
                        ImGui::Text("Shop items: %zu", npc.shopItems.size());
                    if (npc.dialogueTreeId > 0)
                        ImGui::Text("Dialogue tree: %u", npc.dialogueTreeId);
                    if (npc.questId > 0)
                        ImGui::Text("Quest: %u", npc.questId);

                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace RPG
