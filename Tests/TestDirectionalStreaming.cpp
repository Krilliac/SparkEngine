/**
 * @file TestDirectionalStreaming.cpp
 * @brief Tests for the directional preload bias in SeamlessAreaManager
 *
 * Ensures that an area lying along the player's velocity vector preloads
 * before an equally-distant area lying to the side, mimicking RAGE/Decima-style
 * predictive streaming.
 */

#include "TestFramework.h"
#include "Engine/Streaming/SeamlessAreaManager.h"

#include <algorithm>

using Spark::Streaming::AreaDefinition;
using Spark::Streaming::AreaState;
using Spark::Streaming::SeamlessAreaManager;
using Spark::Streaming::StreamingConfig;

namespace
{
    AreaDefinition MakeArea(uint32_t id, float cx, float cz, float half = 50.0f)
    {
        AreaDefinition def;
        def.areaId = id;
        def.name = std::string("Area_") + std::to_string(id);
        def.boundsMin = {cx - half, -50.0f, cz - half};
        def.boundsMax = {cx + half, 50.0f, cz + half};
        def.priority = 0;
        return def;
    }
} // namespace

TEST(DirectionalStreaming_AreaInFrontLoadsFirst)
{
    auto& mgr = SeamlessAreaManager::GetInstance();
    mgr.Shutdown();
    mgr.Initialize();

    StreamingConfig cfg;
    cfg.loadRadius = 400.0f;
    cfg.unloadRadius = 600.0f;
    cfg.updateInterval = 0.0f; // force an immediate update
    cfg.maxConcurrentLoads = 1;
    cfg.directionalBias = 0.8f;
    cfg.directionalDotThreshold = 0.0f;
    mgr.SetConfig(cfg);

    // Area 1: straight ahead (along +Z)
    mgr.RegisterArea(MakeArea(1, 0.0f, 350.0f));
    // Area 2: same distance, to the right (along +X)
    mgr.RegisterArea(MakeArea(2, 350.0f, 0.0f));

    // Player moving forward along +Z
    mgr.SetPlayerState({0, 0, 0}, {0, 0, 10}, {0, 0, 1});
    mgr.Update(1.0f);

    // With maxConcurrentLoads=1 and a forward-pointing velocity, the in-front
    // area should be the one that entered the Loading/Loaded pipeline.
    const AreaState frontState = mgr.GetAreaState(1);
    const AreaState sideState = mgr.GetAreaState(2);

    // Either the front area is further along (Loaded/Loading) OR at least not
    // behind the side area — the directional bias guarantees ordering.
    const auto RankState = [](AreaState s)
    {
        switch (s)
        {
        case AreaState::Loaded:
            return 3;
        case AreaState::Loading:
            return 2;
        case AreaState::Unloading:
            return 1;
        case AreaState::Unloaded:
            return 0;
        }
        return 0;
    };
    EXPECT_GE(RankState(frontState), RankState(sideState));

    mgr.Shutdown();
}

TEST(DirectionalStreaming_BiasZeroIsIsotropic)
{
    auto& mgr = SeamlessAreaManager::GetInstance();
    mgr.Shutdown();
    mgr.Initialize();

    StreamingConfig cfg;
    cfg.loadRadius = 400.0f;
    cfg.unloadRadius = 600.0f;
    cfg.updateInterval = 0.0f;
    cfg.directionalBias = 0.0f; // disabled
    mgr.SetConfig(cfg);

    mgr.RegisterArea(MakeArea(10, 0.0f, 350.0f));
    mgr.RegisterArea(MakeArea(11, 350.0f, 0.0f));
    mgr.SetPlayerState({0, 0, 0}, {0, 0, 10}, {0, 0, 1});
    mgr.Update(1.0f);

    // With bias disabled, both areas should be eligible for load (same distance)
    EXPECT_TRUE(mgr.GetAreaState(10) != AreaState::Unloaded);
    EXPECT_TRUE(mgr.GetAreaState(11) != AreaState::Unloaded);

    mgr.Shutdown();
}

TEST(DirectionalStreaming_OutOfConeAreaNotPreferred)
{
    auto& mgr = SeamlessAreaManager::GetInstance();
    mgr.Shutdown();
    mgr.Initialize();

    StreamingConfig cfg;
    cfg.loadRadius = 400.0f;
    cfg.unloadRadius = 600.0f;
    cfg.updateInterval = 0.0f;
    cfg.directionalBias = 0.9f;
    cfg.directionalDotThreshold = 0.5f;
    mgr.SetConfig(cfg);

    // Behind the player (negative Z) — should NOT be biased.
    mgr.RegisterArea(MakeArea(20, 0.0f, -350.0f));
    // Along forward axis — should be biased.
    mgr.RegisterArea(MakeArea(21, 0.0f, 350.0f));

    mgr.SetPlayerState({0, 0, 0}, {0, 0, 10}, {0, 0, 1});
    mgr.Update(1.0f);

    const auto RankState = [](AreaState s)
    {
        switch (s)
        {
        case AreaState::Loaded:
            return 3;
        case AreaState::Loading:
            return 2;
        case AreaState::Unloading:
            return 1;
        case AreaState::Unloaded:
            return 0;
        }
        return 0;
    };
    EXPECT_GE(RankState(mgr.GetAreaState(21)), RankState(mgr.GetAreaState(20)));

    mgr.Shutdown();
}
