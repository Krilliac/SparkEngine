/**
 * @file TestMOD390VisualScriptDiagnosticsReal.cpp
 * @brief MOD-390: the SparkGameVisualScript load path rejects broken scripts and rolls back.
 *
 * Drives the module's real demo world builder (VisualScriptDemoWorld.cpp, the
 * code OnLoad and vs_restart run) against a real World and the production
 * AngelScriptEngine. Each case copies the shipped Generated .as scripts to a
 * temporary root and breaks one of them: a compile error, a constructor fault
 * during attach, a missing manifest file, and a missing or duplicated
 * selfEntity placeholder. Every case must be rejected with a diagnostic that
 * names the script file (and line where the fault has one) and must leave no
 * VS_ entity, no builder-owned entity and no world bound to the script API.
 */

#include "TestFramework.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT

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
#include <optional>
#include <sstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;
    using Spark::VisualScriptDemo::DemoWorld;

    const fs::path kShippedScripts =
        fs::path(SPARK_TEST_SOURCE_DIR) / "GameModules/SparkGameVisualScript/Assets/Scripts/Generated";

    /// Real World + initialized AngelScriptEngine + a private copy of the shipped scripts.
    struct DiagnosticsFixture
    {
        World world;
        AngelScriptEngine engine;
        fs::path scriptRoot;
        std::optional<DemoWorld> demo; // destroyed first (declared last)
        bool ready = false;

        DiagnosticsFixture()
        {
            static std::atomic<uint32_t> sequence{0};
            scriptRoot =
                fs::temp_directory_path() /
                ("spark_mod390_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(sequence++));
            std::error_code error;
            fs::create_directories(scriptRoot, error);
            bool copied = !error;
            for (const auto& asset : Spark::VisualScriptDemo::ScriptManifest)
            {
                copied = copied && fs::copy_file(kShippedScripts / asset.fileName, scriptRoot / asset.fileName,
                                                 fs::copy_options::overwrite_existing, error);
            }
            ready = copied && engine.Initialize();
            demo.emplace(world, engine);
        }

        ~DiagnosticsFixture()
        {
            demo.reset();
            if (AngelScriptEngine::GetBoundWorld() == &world)
                AngelScriptEngine::BindWorld(nullptr);
            engine.Shutdown();
            std::error_code error;
            fs::remove_all(scriptRoot, error);
        }

        bool Load()
        {
            const std::array<fs::path, 1> searchPaths = {scriptRoot};
            return demo->LoadScripts(searchPaths);
        }

        /// Replace the single occurrence of `from` in a copied script; false if it is not there exactly once.
        bool Rewrite(std::string_view fileName, const std::string& from, const std::string& to) const
        {
            const fs::path path = scriptRoot / fileName;
            std::ostringstream buffer;
            {
                std::ifstream in(path, std::ios::binary);
                buffer << in.rdbuf();
            }
            std::string text = buffer.str();
            const size_t at = text.find(from);
            if (at == std::string::npos || text.find(from, at + from.size()) != std::string::npos)
                return false;
            text.replace(at, from.size(), to);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << text;
            return static_cast<bool>(out);
        }

        uint32_t CountDemoEntities()
        {
            uint32_t count = 0;
            for (auto entity : world.GetEntitiesWith<NameComponent>())
            {
                const auto* name = world.GetComponent<NameComponent>(entity);
                if (name && name->name.starts_with("VS_"))
                    ++count;
            }
            return count;
        }

        /// The full no-residue contract every rejected load must satisfy.
        void ExpectNothingLeftBehind()
        {
            EXPECT_EQ(CountDemoEntities(), 0u);
            EXPECT_EQ(demo->GetEntities().size(), static_cast<size_t>(0));
            EXPECT_TRUE(AngelScriptEngine::GetBoundWorld() != &world);
        }
    };
} // namespace

TEST(VisualScriptDiagnostics_ShippedScriptsBuildElevenEntityWorld)
{
    DiagnosticsFixture fx;
    ASSERT_TRUE(fx.ready);

    ASSERT_TRUE(fx.Load());
    EXPECT_TRUE(fx.demo->GetLastError().empty());
    EXPECT_TRUE(fx.demo->GetScriptRoot() == fx.scriptRoot);
    ASSERT_TRUE(fx.demo->Spawn());

    EXPECT_EQ(fx.CountDemoEntities(), Spark::VisualScriptDemo::ExpectedEntityCount);
    EXPECT_EQ(fx.demo->GetEntities().size(), static_cast<size_t>(Spark::VisualScriptDemo::ExpectedEntityCount));
    EXPECT_TRUE(AngelScriptEngine::GetBoundWorld() == &fx.world);
    for (EntityID entity : fx.demo->GetEntities())
    {
        const auto* script = fx.world.GetComponent<Script>(entity);
        ASSERT_TRUE(script != nullptr);
        EXPECT_TRUE(script->started);
        EXPECT_FALSE(fx.engine.IsScriptFaulted(entity));
    }

    // The Blender kit props are script-less dressing: each names a shipped kit mesh that exists on disk.
    EXPECT_EQ(fx.demo->GetKitProps().size(), static_cast<size_t>(5));
    for (EntityID prop : fx.demo->GetKitProps())
    {
        const auto* mesh = fx.world.GetComponent<MeshRenderer>(prop);
        ASSERT_TRUE(mesh != nullptr);
        EXPECT_STR_CONTAINS(mesh->meshPath, "Assets/Models/VisualScript/Kit/");
        EXPECT_TRUE(fs::is_regular_file(fs::path(SPARK_TEST_SOURCE_DIR) / mesh->meshPath));
        EXPECT_TRUE(fx.world.GetComponent<Script>(prop) == nullptr);
    }

    // vs_restart path: Spawn() again replaces the demo instead of duplicating it.
    ASSERT_TRUE(fx.demo->Spawn());
    EXPECT_EQ(fx.CountDemoEntities(), Spark::VisualScriptDemo::ExpectedEntityCount);
    EXPECT_EQ(fx.demo->GetKitProps().size(), static_cast<size_t>(5));

    const EntityID firstProp = fx.demo->GetKitProps().front();
    fx.demo->DestroyEntities();
    EXPECT_EQ(fx.CountDemoEntities(), 0u);
    EXPECT_TRUE(fx.demo->GetKitProps().empty());
    EXPECT_FALSE(fx.world.GetRegistry().valid(firstProp));
}

TEST(VisualScriptDiagnostics_CompileErrorRejectsLoadWithFileLine)
{
    DiagnosticsFixture fx;
    ASSERT_TRUE(fx.ready);

    // Collectible.as line 22 is "        baseY = pos.y;" inside Start().
    ASSERT_TRUE(fx.Rewrite("Collectible.as", "        baseY = pos.y;\n", "        baseY = undefinedMod390Symbol;\n"));

    EXPECT_FALSE(fx.Load());
    const std::string error = fx.demo->GetLastError();
    EXPECT_STR_CONTAINS(error, "Failed to compile");
    EXPECT_STR_CONTAINS(error, (fx.scriptRoot / "Collectible.as").generic_string() + ":22:");
    EXPECT_STR_CONTAINS(error, "undefinedMod390Symbol");
    EXPECT_TRUE(fx.demo->GetScriptRoot().empty());

    // The module never spawns after a rejected load; even if asked to, the
    // builder refuses without validated sources and leaves nothing behind.
    EXPECT_FALSE(fx.demo->Spawn());
    EXPECT_STR_CONTAINS(fx.demo->GetLastError(), "LoadScripts() must succeed before Spawn()");
    fx.ExpectNothingLeftBehind();
}

TEST(VisualScriptDiagnostics_AttachFailureRollsBackAllEntities)
{
    DiagnosticsFixture fx;
    ASSERT_TRUE(fx.ready);

    // HealthPickup is spawned last, so ten entities already hold started
    // scripts when its constructor faults on line 10 (integer division by a
    // runtime zero during member initialization).
    ASSERT_TRUE(fx.Rewrite("HealthPickup.as", "    float healAmount = 30.0f;\n",
                           "    int faultDivisor = 0; float healAmount = 30.0f + float(1 / faultDivisor);\n"));

    ASSERT_TRUE(fx.Load()); // compiles: the fault is only reachable at construction
    EXPECT_FALSE(fx.demo->Spawn());

    const std::string error = fx.demo->GetLastError();
    EXPECT_STR_CONTAINS(error, "Failed to attach HealthPickup");
    EXPECT_STR_CONTAINS(error, (fx.scriptRoot / "HealthPickup.as").generic_string() + ":10");
    fx.ExpectNothingLeftBehind();

    // The rollback leaves the world and engine reusable: repairing the script
    // and reloading produces the complete demo.
    ASSERT_TRUE(fx.Rewrite("HealthPickup.as",
                           "    int faultDivisor = 0; float healAmount = 30.0f + float(1 / faultDivisor);\n",
                           "    float healAmount = 30.0f;\n"));
    ASSERT_TRUE(fx.Load());
    ASSERT_TRUE(fx.demo->Spawn());
    EXPECT_EQ(fx.CountDemoEntities(), Spark::VisualScriptDemo::ExpectedEntityCount);
}

TEST(VisualScriptDiagnostics_IncompleteManifestRejected)
{
    DiagnosticsFixture fx;
    ASSERT_TRUE(fx.ready);

    std::error_code removeError;
    ASSERT_TRUE(fs::remove(fx.scriptRoot / "GameManager.as", removeError));

    EXPECT_FALSE(fx.Load());
    const std::string error = fx.demo->GetLastError();
    EXPECT_STR_CONTAINS(error, "Could not find a complete five-script asset set");
    EXPECT_STR_CONTAINS(error, (fx.scriptRoot / "GameManager.as").generic_string());
    // Only the missing file is named; present files are not reported as missing.
    EXPECT_TRUE(error.find("Collectible.as") == std::string::npos);
    EXPECT_TRUE(fx.demo->GetScriptRoot().empty());
    fx.ExpectNothingLeftBehind();
}

TEST(VisualScriptDiagnostics_SelfEntityPlaceholderRejectedWithFileLine)
{
    // Missing placeholder: the script compiles but can never be bound to its entity.
    {
        DiagnosticsFixture fx;
        ASSERT_TRUE(fx.ready);
        ASSERT_TRUE(fx.Rewrite("EnemyPatrol.as", "    uint selfEntity = 0;\n", "    uint selfEntity = 7;\n"));

        EXPECT_FALSE(fx.Load());
        const std::string error = fx.demo->GetLastError();
        EXPECT_STR_CONTAINS(error, (fx.scriptRoot / "EnemyPatrol.as").generic_string() + ": EnemyPatrol must declare");
        EXPECT_STR_CONTAINS(error, "found none");
        fx.ExpectNothingLeftBehind();
    }

    // Duplicate placeholder on line 9: ambiguous binding is rejected with the offending line.
    {
        DiagnosticsFixture fx;
        ASSERT_TRUE(fx.ready);
        ASSERT_TRUE(fx.Rewrite("EnemyPatrol.as", "    uint selfEntity = 0;\n",
                               "    uint selfEntity = 0; // uint selfEntity = 0;\n"));

        EXPECT_FALSE(fx.Load());
        const std::string error = fx.demo->GetLastError();
        EXPECT_STR_CONTAINS(error, (fx.scriptRoot / "EnemyPatrol.as").generic_string() + ":9: EnemyPatrol declares");
        EXPECT_STR_CONTAINS(error, "first at line 9");
        fx.ExpectNothingLeftBehind();
    }
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
