// TestSeamlessAreaManager.cpp - Tests for predictive area streaming logic
// Standalone reimplementation for CI testing without DirectXMath dependency

#include "TestFramework.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct Float3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };

    using AreaID = uint32_t;
    constexpr AreaID INVALID_AREA_ID = 0;

    enum class AreaState : uint8_t
    {
        Unloaded,
        Loading,
        Loaded,
        Unloading
    };

    struct AreaDefinition
    {
        AreaID areaId = INVALID_AREA_ID;
        std::string name;
        Float3 boundsMin{0, 0, 0};
        Float3 boundsMax{0, 0, 0};
        int priority = 0;
    };

    struct ManagedArea
    {
        AreaDefinition definition;
        AreaState state = AreaState::Unloaded;
        float distanceToPlayer = 0.0f;
    };

    struct StreamingConfig
    {
        float loadRadius = 500.0f;
        float unloadRadius = 800.0f;
        float lookaheadTime = 3.0f;
        float updateInterval = 0.25f;
        uint32_t maxConcurrentLoads = 2;
    };

    using AreaStateChangedCallback = std::function<void(AreaID, AreaState)>;

    // ========================================================================
    // Standalone SeamlessAreaManager reimplementation
    // ========================================================================

    class SeamlessAreaManager
    {
      public:
        void Initialize()
        {
            m_areas.clear();
            m_loadedAreaIds.clear();
            m_stateCallbacks.clear();
            m_currentAreaId = INVALID_AREA_ID;
            m_playerPos = m_playerVel = {0, 0, 0};
            m_cameraDir = {0, 0, 1};
            m_config = {};
        }
        void Shutdown()
        {
            m_areas.clear();
            m_loadedAreaIds.clear();
        }
        void RegisterArea(const AreaDefinition& def) { m_areas[def.areaId] = {def, AreaState::Unloaded, 0.0f}; }
        void UnregisterArea(AreaID areaId)
        {
            if (m_areas.erase(areaId))
            {
                auto it = std::find(m_loadedAreaIds.begin(), m_loadedAreaIds.end(), areaId);
                if (it != m_loadedAreaIds.end())
                    m_loadedAreaIds.erase(it);
            }
        }
        void SetPlayerState(const Float3& pos, const Float3& vel, const Float3& dir)
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_playerPos = pos;
            m_playerVel = vel;
            m_cameraDir = dir;
        }
        Float3 GetPlayerPosition() const
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            return m_playerPos;
        }
        Float3 GetPlayerVelocity() const
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            return m_playerVel;
        }
        void SetConfig(const StreamingConfig& c) { m_config = c; }
        const StreamingConfig& GetConfig() const { return m_config; }
        AreaState GetAreaState(AreaID id) const
        {
            auto it = m_areas.find(id);
            return it != m_areas.end() ? it->second.state : AreaState::Unloaded;
        }
        const std::vector<AreaID>& GetLoadedAreas() const { return m_loadedAreaIds; }
        AreaID GetCurrentArea() const { return m_currentAreaId; }
        void RegisterStateCallback(AreaStateChangedCallback cb) { m_stateCallbacks.push_back(std::move(cb)); }

        Float3 PredictFuturePosition() const
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            float t = m_config.lookaheadTime;
            return {m_playerPos.x + m_playerVel.x * t, m_playerPos.y + m_playerVel.y * t,
                    m_playerPos.z + m_playerVel.z * t};
        }

        static float DistanceToArea(const Float3& p, const AreaDefinition& a)
        {
            float dx = std::max(0.0f, std::max(a.boundsMin.x - p.x, p.x - a.boundsMax.x));
            float dy = std::max(0.0f, std::max(a.boundsMin.y - p.y, p.y - a.boundsMax.y));
            float dz = std::max(0.0f, std::max(a.boundsMin.z - p.z, p.z - a.boundsMax.z));
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        AreaID FindContainingArea(const Float3& p) const
        {
            for (const auto& [id, m] : m_areas)
            {
                const auto& d = m.definition;
                if (p.x >= d.boundsMin.x && p.x <= d.boundsMax.x && p.y >= d.boundsMin.y && p.y <= d.boundsMax.y &&
                    p.z >= d.boundsMin.z && p.z <= d.boundsMax.z)
                    return id;
            }
            return INVALID_AREA_ID;
        }

        void Update(float /*dt*/)
        {
            Float3 predicted = PredictFuturePosition();
            m_currentAreaId = FindContainingArea(GetPlayerPosition());

            struct Candidate
            {
                AreaID id;
                float dist;
                int pri;
            };
            std::vector<Candidate> candidates;
            for (auto& [id, ma] : m_areas)
            {
                float d = DistanceToArea(predicted, ma.definition);
                ma.distanceToPlayer = d;
                if (ma.state == AreaState::Unloaded && d <= m_config.loadRadius)
                    candidates.push_back({id, d, ma.definition.priority});
            }
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
                      { return a.pri != b.pri ? a.pri > b.pri : a.dist < b.dist; });

            uint32_t active = 0;
            for (const auto& [id, ma] : m_areas)
                if (ma.state == AreaState::Loading)
                    active++;
            for (const auto& c : candidates)
            {
                if (active >= m_config.maxConcurrentLoads)
                    break;
                Transition(m_areas[c.id], AreaState::Loading);
                active++;
            }
            Float3 pPos = GetPlayerPosition();
            for (auto& [id, ma] : m_areas)
            {
                if (ma.state == AreaState::Loaded && DistanceToArea(pPos, ma.definition) > m_config.unloadRadius)
                {
                    Transition(ma, AreaState::Unloading);
                    auto it = std::find(m_loadedAreaIds.begin(), m_loadedAreaIds.end(), id);
                    if (it != m_loadedAreaIds.end())
                        m_loadedAreaIds.erase(it);
                }
            }
        }
        void SimulateLoadComplete(AreaID id)
        {
            auto it = m_areas.find(id);
            if (it != m_areas.end() && it->second.state == AreaState::Loading)
            {
                Transition(it->second, AreaState::Loaded);
                m_loadedAreaIds.push_back(id);
            }
        }

      private:
        void Transition(ManagedArea& area, AreaState newState)
        {
            area.state = newState;
            for (const auto& cb : m_stateCallbacks)
                cb(area.definition.areaId, newState);
        }

        std::unordered_map<AreaID, ManagedArea> m_areas;
        std::vector<AreaID> m_loadedAreaIds;
        AreaID m_currentAreaId = INVALID_AREA_ID;
        mutable std::mutex m_mutex;
        Float3 m_playerPos{0, 0, 0}, m_playerVel{0, 0, 0}, m_cameraDir{0, 0, 1};
        StreamingConfig m_config;
        std::vector<AreaStateChangedCallback> m_stateCallbacks;
    };

    AreaDefinition MakeArea(AreaID id, const std::string& name, Float3 bmin, Float3 bmax, int priority = 0)
    {
        AreaDefinition def;
        def.areaId = id;
        def.name = name;
        def.boundsMin = bmin;
        def.boundsMax = bmax;
        def.priority = priority;
        return def;
    }

    StreamingConfig MakeCfg(float load, float unload, float lookahead, uint32_t maxLoads)
    {
        StreamingConfig c;
        c.loadRadius = load;
        c.unloadRadius = unload;
        c.lookaheadTime = lookahead;
        c.maxConcurrentLoads = maxLoads;
        return c;
    }

} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

TEST(SeamlessArea_RegisterAndUnregister)
{
    SeamlessAreaManager mgr;
    mgr.Initialize();

    mgr.RegisterArea(MakeArea(1, "Town", {-100, 0, -100}, {100, 50, 100}));
    EXPECT_EQ(static_cast<int>(mgr.GetAreaState(1)), static_cast<int>(AreaState::Unloaded));

    mgr.UnregisterArea(1);
    EXPECT_EQ(static_cast<int>(mgr.GetAreaState(1)), static_cast<int>(AreaState::Unloaded));

    mgr.Shutdown();
}

TEST(SeamlessArea_AreaStateTracking)
{
    SeamlessAreaManager mgr;
    mgr.Initialize();

    mgr.RegisterArea(MakeArea(10, "Forest", {0, 0, 0}, {200, 100, 200}));
    EXPECT_EQ(static_cast<int>(mgr.GetAreaState(10)), static_cast<int>(AreaState::Unloaded));
    EXPECT_EQ(static_cast<int>(mgr.GetAreaState(999)), static_cast<int>(AreaState::Unloaded));

    mgr.Shutdown();
}

TEST(SeamlessArea_DistanceCalculation)
{
    auto area = MakeArea(1, "Box", {-10, -10, -10}, {10, 10, 10});

    // Inside AABB: distance = 0
    EXPECT_NEAR(SeamlessAreaManager::DistanceToArea({0, 0, 0}, area), 0.0f, 0.001f);

    // On boundary: distance = 0
    EXPECT_NEAR(SeamlessAreaManager::DistanceToArea({10, 0, 0}, area), 0.0f, 0.001f);

    // Outside along one axis
    EXPECT_NEAR(SeamlessAreaManager::DistanceToArea({20, 0, 0}, area), 10.0f, 0.001f);

    // Outside along two axes: sqrt(3^2 + 4^2) = 5
    EXPECT_NEAR(SeamlessAreaManager::DistanceToArea({13, 14, 0}, area), 5.0f, 0.001f);

    // Outside in negative direction
    EXPECT_NEAR(SeamlessAreaManager::DistanceToArea({-20, 0, 0}, area), 10.0f, 0.001f);
}

TEST(SeamlessArea_PredictFuturePosition)
{
    SeamlessAreaManager mgr;
    mgr.Initialize();

    StreamingConfig cfg;
    cfg.lookaheadTime = 2.0f;
    mgr.SetConfig(cfg);
    mgr.SetPlayerState({100, 0, 50}, {10, 0, 5}, {0, 0, 1});

    // position + velocity * lookaheadTime = (120, 0, 60)
    Float3 predicted = mgr.PredictFuturePosition();
    EXPECT_NEAR(predicted.x, 120.0f, 0.001f);
    EXPECT_NEAR(predicted.y, 0.0f, 0.001f);
    EXPECT_NEAR(predicted.z, 60.0f, 0.001f);

    mgr.Shutdown();
}

TEST(SeamlessArea_LoadRadius)
{
    SeamlessAreaManager mgr;
    mgr.Initialize();
    mgr.SetConfig(MakeCfg(100.0f, 200.0f, 0.0f, 10));

    mgr.RegisterArea(MakeArea(1, "Near", {40, 0, 0}, {60, 10, 20}));
    mgr.RegisterArea(MakeArea(2, "Far", {500, 0, 0}, {600, 10, 20}));

    mgr.SetPlayerState({0, 0, 0}, {0, 0, 0}, {1, 0, 0});
    mgr.Update(0.0f);

    EXPECT_EQ(static_cast<int>(mgr.GetAreaState(1)), static_cast<int>(AreaState::Loading));
    EXPECT_EQ(static_cast<int>(mgr.GetAreaState(2)), static_cast<int>(AreaState::Unloaded));

    mgr.Shutdown();
}

TEST(SeamlessArea_UnloadRadius)
{
    SeamlessAreaManager mgr;
    mgr.Initialize();
    mgr.SetConfig(MakeCfg(100.0f, 50.0f, 0.0f, 10));

    mgr.RegisterArea(MakeArea(1, "Village", {10, 0, 10}, {30, 10, 30}));

    // Player close -> area starts loading, then simulate load complete
    mgr.SetPlayerState({20, 0, 20}, {0, 0, 0}, {1, 0, 0});
    mgr.Update(0.0f);
    mgr.SimulateLoadComplete(1);
    EXPECT_EQ(static_cast<int>(mgr.GetAreaState(1)), static_cast<int>(AreaState::Loaded));

    // Player moves far away -> area transitions to Unloading
    mgr.SetPlayerState({500, 0, 500}, {0, 0, 0}, {1, 0, 0});
    mgr.Update(0.0f);
    EXPECT_EQ(static_cast<int>(mgr.GetAreaState(1)), static_cast<int>(AreaState::Unloading));

    mgr.Shutdown();
}

TEST(SeamlessArea_MaxConcurrentLoads)
{
    SeamlessAreaManager mgr;
    mgr.Initialize();
    mgr.SetConfig(MakeCfg(1000.0f, 2000.0f, 0.0f, 2));

    for (AreaID i = 1; i <= 5; i++)
    {
        float off = static_cast<float>(i) * 10.0f;
        mgr.RegisterArea(MakeArea(i, "A" + std::to_string(i), {off, 0, 0}, {off + 5, 5, 5}));
    }

    mgr.SetPlayerState({0, 0, 0}, {0, 0, 0}, {1, 0, 0});
    mgr.Update(0.0f);

    uint32_t loadingCount = 0;
    for (AreaID i = 1; i <= 5; i++)
    {
        if (mgr.GetAreaState(i) == AreaState::Loading)
            loadingCount++;
    }
    EXPECT_EQ(loadingCount, 2u);

    mgr.Shutdown();
}

TEST(SeamlessArea_PriorityOrdering)
{
    SeamlessAreaManager mgr;
    mgr.Initialize();
    mgr.SetConfig(MakeCfg(1000.0f, 2000.0f, 0.0f, 1));

    // Two overlapping areas at same distance, different priority
    mgr.RegisterArea(MakeArea(1, "LowPri", {50, 0, 0}, {60, 10, 10}, 1));
    mgr.RegisterArea(MakeArea(2, "HighPri", {50, 0, 0}, {60, 10, 10}, 10));

    mgr.SetPlayerState({0, 0, 0}, {0, 0, 0}, {1, 0, 0});
    mgr.Update(0.0f);

    EXPECT_EQ(static_cast<int>(mgr.GetAreaState(2)), static_cast<int>(AreaState::Loading));
    EXPECT_EQ(static_cast<int>(mgr.GetAreaState(1)), static_cast<int>(AreaState::Unloaded));

    mgr.Shutdown();
}

TEST(SeamlessArea_FindContainingArea)
{
    SeamlessAreaManager mgr;
    mgr.Initialize();
    mgr.SetConfig(MakeCfg(1000.0f, 2000.0f, 0.0f, 10));

    mgr.RegisterArea(MakeArea(1, "Zone1", {0, 0, 0}, {100, 50, 100}));
    mgr.RegisterArea(MakeArea(2, "Zone2", {200, 0, 0}, {300, 50, 100}));

    mgr.SetPlayerState({50, 25, 50}, {0, 0, 0}, {1, 0, 0});
    mgr.Update(0.0f);
    EXPECT_EQ(mgr.GetCurrentArea(), 1u);

    mgr.SetPlayerState({250, 25, 50}, {0, 0, 0}, {1, 0, 0});
    mgr.Update(0.0f);
    EXPECT_EQ(mgr.GetCurrentArea(), 2u);

    mgr.SetPlayerState({150, 25, 50}, {0, 0, 0}, {1, 0, 0});
    mgr.Update(0.0f);
    EXPECT_EQ(mgr.GetCurrentArea(), INVALID_AREA_ID);

    mgr.Shutdown();
}

TEST(SeamlessArea_StateChangeCallbacks)
{
    SeamlessAreaManager mgr;
    mgr.Initialize();
    mgr.SetConfig(MakeCfg(100.0f, 200.0f, 0.0f, 10));

    std::vector<std::pair<AreaID, AreaState>> log;
    mgr.RegisterStateCallback([&log](AreaID id, AreaState s) { log.push_back({id, s}); });

    mgr.RegisterArea(MakeArea(1, "CbTest", {10, 0, 10}, {30, 10, 30}));

    mgr.SetPlayerState({20, 5, 20}, {0, 0, 0}, {1, 0, 0});
    mgr.Update(0.0f);

    EXPECT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].first, 1u);
    EXPECT_EQ(static_cast<int>(log[0].second), static_cast<int>(AreaState::Loading));

    mgr.SimulateLoadComplete(1);
    EXPECT_EQ(log.size(), 2u);
    EXPECT_EQ(static_cast<int>(log[1].second), static_cast<int>(AreaState::Loaded));

    mgr.Shutdown();
}

TEST(SeamlessArea_StreamingConfig)
{
    // Verify defaults
    StreamingConfig cfg;
    EXPECT_NEAR(cfg.loadRadius, 500.0f, 0.001f);
    EXPECT_NEAR(cfg.unloadRadius, 800.0f, 0.001f);
    EXPECT_NEAR(cfg.lookaheadTime, 3.0f, 0.001f);
    EXPECT_NEAR(cfg.updateInterval, 0.25f, 0.001f);
    EXPECT_EQ(cfg.maxConcurrentLoads, 2u);

    // Verify custom values round-trip through SetConfig/GetConfig
    SeamlessAreaManager mgr;
    mgr.Initialize();
    cfg.loadRadius = 250.0f;
    cfg.unloadRadius = 400.0f;
    cfg.lookaheadTime = 5.0f;
    cfg.updateInterval = 0.5f;
    cfg.maxConcurrentLoads = 4;
    mgr.SetConfig(cfg);

    const auto& r = mgr.GetConfig();
    EXPECT_NEAR(r.loadRadius, 250.0f, 0.001f);
    EXPECT_NEAR(r.unloadRadius, 400.0f, 0.001f);
    EXPECT_NEAR(r.lookaheadTime, 5.0f, 0.001f);
    EXPECT_NEAR(r.updateInterval, 0.5f, 0.001f);
    EXPECT_EQ(r.maxConcurrentLoads, 4u);
    mgr.Shutdown();
}

TEST(SeamlessArea_PlayerStateThreadSafety)
{
    SeamlessAreaManager mgr;
    mgr.Initialize();

    mgr.SetPlayerState({10, 20, 30}, {1, 2, 3}, {0, 1, 0});

    Float3 pos = mgr.GetPlayerPosition();
    EXPECT_NEAR(pos.x, 10.0f, 0.001f);
    EXPECT_NEAR(pos.y, 20.0f, 0.001f);
    EXPECT_NEAR(pos.z, 30.0f, 0.001f);

    Float3 vel = mgr.GetPlayerVelocity();
    EXPECT_NEAR(vel.x, 1.0f, 0.001f);
    EXPECT_NEAR(vel.y, 2.0f, 0.001f);
    EXPECT_NEAR(vel.z, 3.0f, 0.001f);

    // Overwrite and verify
    mgr.SetPlayerState({-5, -10, -15}, {0, 0, 0}, {1, 0, 0});
    pos = mgr.GetPlayerPosition();
    EXPECT_NEAR(pos.x, -5.0f, 0.001f);
    EXPECT_NEAR(pos.y, -10.0f, 0.001f);
    EXPECT_NEAR(pos.z, -15.0f, 0.001f);

    mgr.Shutdown();
}
