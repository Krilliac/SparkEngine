/**
 * @file TestMOD390VisualScriptHotReloadReal.cpp
 * @brief MOD-390: the running SparkGameVisualScript demo hot-reloads its generated scripts (vs_reload).
 *
 * Plays the module's real DemoWorld (VisualScriptDemoWorld.cpp) through the
 * shared headless harness (VisualScriptGameplayHarness.h) from a private copy
 * of the shipped Generated .as scripts, so the edits made here never touch the
 * repository. DemoWorld::ReloadScripts() is the body of the vs_reload console
 * command: it re-reads and validates all five scripts, then recompiles each
 * entity's per-entity module from its bound new source with
 * AngelScriptEngine::HotReloadModuleFromSource(), carrying script fields by the
 * engine's hot-reload state rules.
 *
 * The cases pin: an identical reload keeps the score, positions and collected
 * coins and play continues to the win; an edited constant takes effect without
 * moving the player; and a compile error, a lost selfEntity placeholder or a
 * renamed class rejects the whole reload with a file diagnostic and changes
 * nothing, not even the scripts validated before the broken one.
 */

#include "TestFramework.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include "VisualScriptGameplayHarness.h"
#include "../GameModules/SparkGameVisualScript/Source/Core/VisualScriptDemoRuntime.h"
#include "../GameModules/SparkGameVisualScript/Source/Core/VisualScriptDemoWorld.h"
#include "Engine/ECS/Components.h"
#include "Engine/Scripting/AngelScriptEngine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using VisualScriptGameplayHarness::GameplayFixture;
    using VisualScriptGameplayHarness::kCoinRoute;
    using VisualScriptGameplayHarness::kFrameDelta;

    constexpr int kFrameBudget = 60 * 60; // one minute of game time

    /// PlayerController's generated walk/sprint speed select; the tests edit the walk literal.
    constexpr std::string_view kWalkSpeed8 = "14.000000f : 8.000000f";
    constexpr std::string_view kWalkSpeed16 = "14.000000f : 16.000000f";

    /// A private copy of the shipped scripts; removed with the fixture.
    struct ScriptCopy
    {
        fs::path root;
        bool copied = false;

        ScriptCopy()
        {
            static std::atomic<uint32_t> sequence{0};
            root =
                fs::temp_directory_path() /
                ("spark_mod390_reload_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "_" + std::to_string(sequence++));
            std::error_code error;
            fs::create_directories(root, error);
            copied = !error;
            for (const auto& asset : Spark::VisualScriptDemo::ScriptManifest)
            {
                copied = copied && fs::copy_file(VisualScriptGameplayHarness::SourceScriptRoot() / asset.fileName,
                                                 root / asset.fileName, fs::copy_options::overwrite_existing, error);
            }
        }

        ~ScriptCopy()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }

        ScriptCopy(const ScriptCopy&) = delete;
        ScriptCopy& operator=(const ScriptCopy&) = delete;

        std::string Read(std::string_view fileName) const
        {
            std::ifstream in(root / fileName, std::ios::binary);
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        /// Replace the single occurrence of @p from; false if it is not there exactly once.
        bool Rewrite(std::string_view fileName, std::string_view from, std::string_view to) const
        {
            std::string text = Read(fileName);
            const size_t at = text.find(from);
            if (at == std::string::npos || text.find(from, at + from.size()) != std::string::npos)
                return false;
            text.replace(at, from.size(), to);
            std::ofstream out(root / fileName, std::ios::binary | std::ios::trunc);
            out << text;
            return static_cast<bool>(out);
        }

        /// 1-based line of the one line containing @p needle (0 when absent or not unique).
        size_t LineOf(std::string_view fileName, std::string_view needle) const
        {
            std::istringstream in(Read(fileName));
            size_t found = 0;
            size_t lineNumber = 0;
            for (std::string line; std::getline(in, line);)
            {
                ++lineNumber;
                if (line.find(needle) == std::string::npos)
                    continue;
                if (found != 0)
                    return 0;
                found = lineNumber;
            }
            return found;
        }

        std::string DiagnosticPath(std::string_view fileName) const { return (root / fileName).generic_string(); }
    };

    /// The demo playing from a private script copy (declared first, so it outlives the demo).
    struct HotReloadFixture
    {
        ScriptCopy scripts;
        std::array<fs::path, 1> searchPaths;
        GameplayFixture game;
        bool ready = false;

        HotReloadFixture() : searchPaths{scripts.root}, game(searchPaths) { ready = scripts.copied && game.ready; }

        bool Reload() { return game.demo->ReloadScripts(); }

        /// Steer to coin VS_Coin_<index> until its Collectible script hides it; false when the budget runs out.
        bool CollectCoin(int coinIndex, int& frame)
        {
            const EntityID coin = game.Find("VS_Coin_" + std::to_string(coinIndex));
            if (coin == entt::null)
                return false;
            const auto target = game.Position(coin);
            while (game.Position(coin).y > -50.0f)
            {
                if (frame >= kFrameBudget)
                    return false;
                game.SteerTowards(target.x, target.z);
                game.Tick();
                ++frame;
            }
            return true;
        }

        /// Hold D for one frame and return how far the PlayerController script moved the player along +X.
        float StepRight()
        {
            const EntityID player = game.Find("VS_Player");
            const float before = game.Position(player).x;
            game.SetKey('D', true);
            game.Tick();
            game.SetKey('D', false);
            return game.Position(player).x - before;
        }

        /// Demo entities whose script instance is attached from their own module and not faulted.
        uint32_t LiveScripts()
        {
            uint32_t live = 0;
            for (EntityID entity : game.demo->GetEntities())
            {
                const auto* script = game.world.GetComponent<Script>(entity);
                if (!script || game.engine.IsScriptFaulted(entity))
                    continue;
                const auto attached = game.engine.GetEntitiesForModule(script->moduleName);
                if (attached.size() == 1 && attached.front() == entity)
                    ++live;
            }
            return live;
        }
    };

    float FrameDelta()
    {
        return Spark::VisualScriptDemo::SanitizeDeltaTime(kFrameDelta);
    }
} // namespace

TEST(VisualScriptHotReload_ReloadKeepsScoreAndPositions)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    auto& game = fx.game;
    const EntityID player = game.Find("VS_Player");
    const EntityID manager = game.Find("VS_GameManager");
    ASSERT_TRUE(player != entt::null);
    ASSERT_TRUE(manager != entt::null);

    int frame = 0;
    ASSERT_TRUE(fx.CollectCoin(kCoinRoute[0], frame));
    ASSERT_TRUE(fx.CollectCoin(kCoinRoute[1], frame));
    game.ReleaseAllKeys();
    game.Tick();
    ASSERT_EQ(game.Health(manager), 200.0f);

    const float playerHealth = game.Health(player);
    const auto playerPosition = game.Position(player);
    std::vector<DirectX::XMFLOAT3> coinPositions;
    for (int i = 0; i < 5; ++i)
        coinPositions.push_back(game.Position(game.Find("VS_Coin_" + std::to_string(i))));

    // Byte-identical sources: every field of every instance is carried, nothing is new or dropped.
    ASSERT_TRUE(fx.Reload());
    const std::string summary = game.demo->GetReloadSummary();
    EXPECT_STR_CONTAINS(summary, "PlayerController: 1 instance(s), fields carried 8, defaulted 0, dropped 0");
    EXPECT_STR_CONTAINS(summary, "Collectible: 5 instance(s), fields carried 45, defaulted 0, dropped 0");
    EXPECT_STR_CONTAINS(summary, "EnemyPatrol: 3 instance(s), fields carried 42, defaulted 0, dropped 0");
    EXPECT_STR_CONTAINS(summary, "GameManager: 1 instance(s), fields carried 7, defaulted 0, dropped 0");
    EXPECT_STR_CONTAINS(summary, "HealthPickup: 1 instance(s), fields carried 10, defaulted 0, dropped 0");
    EXPECT_EQ(fx.LiveScripts(), Spark::VisualScriptDemo::ExpectedEntityCount);

    // The reload touches no ECS state and does not re-run Start(): GameManager.Start() would zero the score
    // and print the banner again.
    EXPECT_EQ(game.Health(manager), 200.0f);
    EXPECT_EQ(game.Health(player), playerHealth);
    EXPECT_EQ(game.Position(player).x, playerPosition.x);
    EXPECT_EQ(game.Position(player).y, playerPosition.y);
    EXPECT_EQ(game.Position(player).z, playerPosition.z);
    for (int i = 0; i < 5; ++i)
    {
        const auto position = game.Position(game.Find("VS_Coin_" + std::to_string(i)));
        EXPECT_EQ(position.y, coinPositions[static_cast<size_t>(i)].y);
    }
    EXPECT_EQ(game.CountOutput("=== VISUAL SCRIPT GAME ==="), static_cast<size_t>(1));

    // Play continues on the reloaded scripts to the win; the two collected coins stay collected.
    for (size_t i = 2; i < kCoinRoute.size(); ++i)
        ASSERT_TRUE(fx.CollectCoin(kCoinRoute[i], frame));
    game.ReleaseAllKeys();
    game.Tick();
    EXPECT_EQ(game.CountOutput("Collected! +100 points"), static_cast<size_t>(5));
    EXPECT_EQ(game.Health(manager), 500.0f);
    EXPECT_EQ(game.CountOutput("*** YOU WIN! ***"), static_cast<size_t>(1));
    EXPECT_FALSE(game.AnyScriptFaulted());
}

TEST(VisualScriptHotReload_EditedConstantTakesEffect)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    auto& game = fx.game;
    const EntityID player = game.Find("VS_Player");
    ASSERT_TRUE(player != entt::null);
    const float dt = FrameDelta();

    EXPECT_NEAR(fx.StepRight(), 8.0f * dt, 1e-5f);

    ASSERT_TRUE(fx.scripts.Rewrite("PlayerController.as", kWalkSpeed8, kWalkSpeed16));
    const auto position = game.Position(player);
    ASSERT_TRUE(fx.Reload());

    // The player stays where it was and the next frame moves at the edited speed.
    EXPECT_EQ(game.Position(player).x, position.x);
    EXPECT_EQ(game.Position(player).z, position.z);
    EXPECT_NEAR(fx.StepRight(), 16.0f * dt, 1e-5f);
    EXPECT_FALSE(game.AnyScriptFaulted());

    // vs_restart after a reload binds the new sources too.
    ASSERT_TRUE(game.demo->Spawn());
    EXPECT_NEAR(fx.StepRight(), 16.0f * dt, 1e-5f);
}

TEST(VisualScriptHotReload_CompileErrorLeavesDemoRunning)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    auto& game = fx.game;
    const float dt = FrameDelta();

    // PlayerController is validated before Collectible: its valid edit must not be applied either.
    ASSERT_TRUE(fx.scripts.Rewrite("PlayerController.as", kWalkSpeed8, kWalkSpeed16));
    const size_t line = fx.scripts.LineOf("Collectible.as", "    int scoreValue = 100;");
    ASSERT_TRUE(line != 0);
    ASSERT_TRUE(fx.scripts.Rewrite("Collectible.as", "    int scoreValue = 100;",
                                   "    int scoreValue = undefinedMod390Reload;"));

    EXPECT_FALSE(fx.Reload());
    const std::string error = game.demo->GetLastError();
    EXPECT_STR_CONTAINS(error, "Failed to compile");
    EXPECT_STR_CONTAINS(error, fx.scripts.DiagnosticPath("Collectible.as") + ":" + std::to_string(line) + ":");
    EXPECT_STR_CONTAINS(error, "undefinedMod390Reload");
    EXPECT_TRUE(game.demo->GetReloadSummary().empty());

    // All 11 scripts still run the old code, and the demo still plays to the win.
    EXPECT_EQ(fx.LiveScripts(), Spark::VisualScriptDemo::ExpectedEntityCount);
    EXPECT_NEAR(fx.StepRight(), 8.0f * dt, 1e-5f);
    ASSERT_TRUE(game.CollectAllCoins(kFrameBudget) < kFrameBudget);
    game.ReleaseAllKeys();
    game.Tick();
    EXPECT_EQ(game.Health(game.Find("VS_GameManager")), 500.0f);
    EXPECT_EQ(game.CountOutput("*** YOU WIN! ***"), static_cast<size_t>(1));
    EXPECT_FALSE(game.AnyScriptFaulted());
}

TEST(VisualScriptHotReload_UnboundSelfEntityRejected)
{
    HotReloadFixture fx;
    ASSERT_TRUE(fx.ready);
    auto& game = fx.game;
    const float dt = FrameDelta();
    ASSERT_TRUE(fx.scripts.Rewrite("PlayerController.as", kWalkSpeed8, kWalkSpeed16));

    // A literal selfEntity compiles, but the reload could no longer bind each instance to its entity.
    ASSERT_TRUE(fx.scripts.Rewrite("GameManager.as", "uint selfEntity = 0;", "uint selfEntity = 7;"));
    EXPECT_FALSE(fx.Reload());
    EXPECT_STR_CONTAINS(game.demo->GetLastError(),
                        fx.scripts.DiagnosticPath("GameManager.as") + ": GameManager must declare");
    EXPECT_STR_CONTAINS(game.demo->GetLastError(), "found none");
    EXPECT_EQ(fx.LiveScripts(), Spark::VisualScriptDemo::ExpectedEntityCount);
    EXPECT_NEAR(fx.StepRight(), 8.0f * dt, 1e-5f);

    // A renamed class is rejected the same way instead of leaving its entity without a script.
    ASSERT_TRUE(fx.scripts.Rewrite("GameManager.as", "uint selfEntity = 7;", "uint selfEntity = 0;"));
    ASSERT_TRUE(fx.scripts.Rewrite("HealthPickup.as", "class HealthPickup\n", "class HealthPickupRenamed\n"));
    EXPECT_FALSE(fx.Reload());
    EXPECT_STR_CONTAINS(game.demo->GetLastError(),
                        fx.scripts.DiagnosticPath("HealthPickup.as") + ": does not declare class HealthPickup");
    EXPECT_EQ(fx.LiveScripts(), Spark::VisualScriptDemo::ExpectedEntityCount);
    EXPECT_NEAR(fx.StepRight(), 8.0f * dt, 1e-5f);

    // Repaired, the same edit reloads.
    ASSERT_TRUE(fx.scripts.Rewrite("HealthPickup.as", "class HealthPickupRenamed\n", "class HealthPickup\n"));
    ASSERT_TRUE(fx.Reload());
    EXPECT_NEAR(fx.StepRight(), 16.0f * dt, 1e-5f);
    EXPECT_EQ(fx.LiveScripts(), Spark::VisualScriptDemo::ExpectedEntityCount);
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
