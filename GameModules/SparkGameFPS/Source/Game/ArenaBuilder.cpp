/**
 * @file ArenaBuilder.cpp
 * @brief Procedural arena construction with multi-room layouts
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include "Core/Platform.h"
#endif

#include "ArenaBuilder.h"
#include "Game.h"
#include "CubeObject.h"
#include "PlaneObject.h"
#include "WallObject.h"
#include "RampObject.h"
#include "PyramidObject.h"
#include "SphereObject.h"
#include "Graphics/GraphicsEngine.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

using namespace DirectX;

namespace Spark
{

    // Helper: spawn a cube (wall/pillar/crate) at position with scale
    static bool SpawnCube(Game* game, GraphicsEngine* gfx, float size, XMFLOAT3 pos, const std::string& name)
    {
        auto cube = std::make_unique<CubeObject>(size);
        HRESULT hr = cube->Initialize(gfx->GetDevice(), gfx->GetContext());
        if (FAILED(hr))
            return false;
        cube->SetPosition(pos);
        cube->SetName(name);
        // Use Game's internal object list via SpawnObject or direct push
        // Since SpawnObject only supports basic types, we push directly
        return game->SpawnObject("cube", pos.x, pos.y, pos.z);
    }

    void ArenaBuilder::BuildArena(Game* game, GraphicsEngine* graphics)
    {
        if (!game || !graphics)
            return;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Building arena geometry");

        // NOTE: Arena geometry is now defined in the scene file (Assets/Scenes/level1.scene)
        // as [Object] entries. The entire arena layout — floors, walls, cover, platforms,
        // ramps, and corridors — can be placed and edited in the SparkEditor without
        // recompiling. The BuildXxx() methods below show the equivalent C++ approach.
        //
        // To use the code-based approach instead of scene file, uncomment:
        // BuildFloor(game, graphics);
        // BuildWalls(game, graphics);
        // BuildCover(game, graphics);
        // BuildPlatforms(game, graphics);
        // BuildRamps(game, graphics);
        // BuildCorridors(game, graphics);

        LOG_TO_CONSOLE_IMMEDIATE(L"Arena geometry loaded from scene file (code builder available)", L"SUCCESS");
    }

    void ArenaBuilder::BuildFloor(Game* game, GraphicsEngine* graphics)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Building arena floor geometry");
        auto* dev = graphics->GetDevice();
        auto* ctx = graphics->GetContext();

        // Main arena floor - large central area
        auto mainFloor = std::make_unique<PlaneObject>(50.0f, 50.0f);
        HRESULT hr = mainFloor->Initialize(dev, ctx);
        if (SUCCEEDED(hr))
        {
            mainFloor->SetPosition({0.0f, -0.5f, 0.0f});
            mainFloor->SetName("Arena_Floor_Main");
            game->SpawnObject("plane", 0.0f, -0.5f, 0.0f);
        }

        // Elevated platform floors (east and west)
        game->SpawnObject("plane", 25.0f, 4.0f, 0.0f);  // East platform
        game->SpawnObject("plane", -25.0f, 4.0f, 0.0f); // West platform

        // Underground corridor floor
        game->SpawnObject("plane", 0.0f, -3.0f, 0.0f);
    }

    void ArenaBuilder::BuildWalls(Game* game, GraphicsEngine* graphics)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Building arena wall segments");
        (void)graphics;

        // North wall segments (with gaps for corridors)
        game->SpawnObject("wall", -20.0f, 2.0f, 28.0f);
        game->SpawnObject("wall", -10.0f, 2.0f, 28.0f);
        game->SpawnObject("wall", 10.0f, 2.0f, 28.0f);
        game->SpawnObject("wall", 20.0f, 2.0f, 28.0f);

        // South wall segments
        game->SpawnObject("wall", -20.0f, 2.0f, -28.0f);
        game->SpawnObject("wall", -10.0f, 2.0f, -28.0f);
        game->SpawnObject("wall", 10.0f, 2.0f, -28.0f);
        game->SpawnObject("wall", 20.0f, 2.0f, -28.0f);

        // East wall (with door gaps)
        game->SpawnObject("wall", 28.0f, 2.0f, -15.0f);
        game->SpawnObject("wall", 28.0f, 2.0f, 15.0f);

        // West wall (with door gaps)
        game->SpawnObject("wall", -28.0f, 2.0f, -15.0f);
        game->SpawnObject("wall", -28.0f, 2.0f, 15.0f);
    }

    void ArenaBuilder::BuildCover(Game* game, GraphicsEngine* graphics)
    {
        (void)graphics;

        // Central cover cluster — cross-shaped barriers
        game->SpawnObject("cube", 0.0f, 0.75f, 5.0f);
        game->SpawnObject("cube", 0.0f, 0.75f, -5.0f);
        game->SpawnObject("cube", 5.0f, 0.75f, 0.0f);
        game->SpawnObject("cube", -5.0f, 0.75f, 0.0f);

        // Quarter-arena pillar pairs (symmetrical)
        game->SpawnObject("cube", 12.0f, 1.5f, 12.0f);
        game->SpawnObject("cube", -12.0f, 1.5f, 12.0f);
        game->SpawnObject("cube", 12.0f, 1.5f, -12.0f);
        game->SpawnObject("cube", -12.0f, 1.5f, -12.0f);

        // Low cover walls near spawn areas
        game->SpawnObject("wall", 8.0f, 0.5f, 20.0f);
        game->SpawnObject("wall", -8.0f, 0.5f, 20.0f);
        game->SpawnObject("wall", 8.0f, 0.5f, -20.0f);
        game->SpawnObject("wall", -8.0f, 0.5f, -20.0f);

        // Destructible crate clusters
        game->SpawnObject("cube", 15.0f, 0.5f, 5.0f);
        game->SpawnObject("cube", 15.5f, 0.5f, 5.5f);
        game->SpawnObject("cube", -15.0f, 0.5f, -5.0f);
        game->SpawnObject("cube", -15.5f, 0.5f, -5.5f);

        // Mid-field barriers for engagement distance control
        game->SpawnObject("wall", 0.0f, 1.0f, 15.0f);
        game->SpawnObject("wall", 0.0f, 1.0f, -15.0f);
    }

    void ArenaBuilder::BuildPlatforms(Game* game, GraphicsEngine* graphics)
    {
        (void)graphics;

        // East sniper platform (elevated, overlooking arena center)
        game->SpawnObject("cube", 22.0f, 3.0f, 0.0f);  // Platform base
        game->SpawnObject("cube", 24.0f, 3.0f, 0.0f);  // Platform extension
        game->SpawnObject("cube", 22.0f, 3.0f, 2.0f);  // Side cover
        game->SpawnObject("cube", 22.0f, 3.0f, -2.0f); // Side cover

        // West sniper platform (mirror of east)
        game->SpawnObject("cube", -22.0f, 3.0f, 0.0f);
        game->SpawnObject("cube", -24.0f, 3.0f, 0.0f);
        game->SpawnObject("cube", -22.0f, 3.0f, 2.0f);
        game->SpawnObject("cube", -22.0f, 3.0f, -2.0f);

        // North watchtower base
        game->SpawnObject("cube", 0.0f, 2.0f, 22.0f);
        game->SpawnObject("cube", 0.0f, 4.0f, 22.0f);

        // South watchtower base
        game->SpawnObject("cube", 0.0f, 2.0f, -22.0f);
        game->SpawnObject("cube", 0.0f, 4.0f, -22.0f);
    }

    void ArenaBuilder::BuildRamps(Game* game, GraphicsEngine* graphics)
    {
        (void)graphics;

        // Ramps to east platform
        game->SpawnObject("ramp", 18.0f, 1.5f, 0.0f);

        // Ramps to west platform
        game->SpawnObject("ramp", -18.0f, 1.5f, 0.0f);

        // Ramps to north watchtower
        game->SpawnObject("ramp", 3.0f, 1.0f, 22.0f);

        // Ramps to south watchtower
        game->SpawnObject("ramp", -3.0f, 1.0f, -22.0f);
    }

    void ArenaBuilder::BuildCorridors(Game* game, GraphicsEngine* graphics)
    {
        (void)graphics;

        // North corridor (flanking route)
        game->SpawnObject("wall", -5.0f, 1.5f, 25.0f);
        game->SpawnObject("wall", 5.0f, 1.5f, 25.0f);

        // South corridor (flanking route)
        game->SpawnObject("wall", -5.0f, 1.5f, -25.0f);
        game->SpawnObject("wall", 5.0f, 1.5f, -25.0f);

        // Underground passage markers (using spheres as light sources)
        game->SpawnObject("sphere", 0.0f, -2.0f, 10.0f);
        game->SpawnObject("sphere", 0.0f, -2.0f, -10.0f);
    }

} // namespace Spark
