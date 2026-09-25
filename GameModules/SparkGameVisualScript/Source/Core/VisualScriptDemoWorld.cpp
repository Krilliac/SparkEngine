/**
 * @file VisualScriptDemoWorld.cpp
 * @brief Fail-fast script validation, entity spawn and rollback for the visual-script demo.
 */

#include "VisualScriptDemoWorld.h"

#include "VisualScriptDemoRuntime.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace Spark::VisualScriptDemo
{
    namespace
    {
        /// 1-based line number of a byte offset inside a script source.
        size_t LineOfOffset(std::string_view source, size_t offset)
        {
            return 1 + static_cast<size_t>(std::count(source.begin(), source.begin() + offset, '\n'));
        }

        /**
         * Per-entity modules are compiled from the file source with only the
         * selfEntity declaration rewritten in place, so module line numbers map
         * 1:1 onto the file. Rewrite "<module>:<line>" locations to name the file.
         */
        std::string LocateInFile(std::string diagnostic, const std::string& moduleName, const std::string& filePath)
        {
            const std::string moduleLocation = moduleName + ":";
            const std::string fileLocation = filePath + ":";
            for (size_t at = diagnostic.find(moduleLocation); at != std::string::npos;
                 at = diagnostic.find(moduleLocation, at + fileLocation.size()))
            {
                diagnostic.replace(at, moduleLocation.size(), fileLocation);
            }
            return diagnostic;
        }
    } // namespace

    DemoWorld::DemoWorld(World& world, AngelScriptEngine& scriptEngine) : m_world(world), m_scriptEngine(scriptEngine)
    {
    }

    DemoWorld::~DemoWorld()
    {
        DestroyEntities();
    }

    void DemoWorld::Fail(const std::string& message)
    {
        m_lastError = message;
        SimpleConsole::GetInstance().LogError("[VisualScript] " + message);
    }

    bool DemoWorld::LoadScripts(std::span<const std::filesystem::path> searchPaths)
    {
        auto& console = SimpleConsole::GetInstance();
        m_lastError.clear();
        m_scriptSources.clear();
        m_scriptRoot.clear();

        const auto root = SelectCompleteScriptRoot(searchPaths,
                                                   [](const std::filesystem::path& path)
                                                   {
                                                       std::error_code error;
                                                       return std::filesystem::is_regular_file(path, error);
                                                   });
        if (!root)
        {
            // Name every manifest file the first candidate lacks so the operator knows what to restore.
            std::string missing;
            if (!searchPaths.empty())
            {
                for (const auto& asset : ScriptManifest)
                {
                    const auto path = searchPaths.front() / std::filesystem::path(asset.fileName);
                    std::error_code error;
                    if (!std::filesystem::is_regular_file(path, error))
                        missing += (missing.empty() ? "" : ", ") + path.generic_string();
                }
            }
            Fail("Could not find a complete five-script asset set; missing: " + (missing.empty() ? "?" : missing));
            return false;
        }

        std::unordered_map<std::string, std::string> sources;
        for (const auto& asset : ScriptManifest)
        {
            const auto path = *root / std::filesystem::path(asset.fileName);
            std::ifstream stream(path, std::ios::binary);
            std::ostringstream source;
            if (stream)
                source << stream.rdbuf();
            if (!stream || source.str().empty())
            {
                Fail("Failed to read: " + path.generic_string());
                return false;
            }

            // CompileScriptFile names the section after the file path, so the
            // engine diagnostic already reads "<path>:<line>:<column>". The
            // script builder normalizes that path to forward slashes, so every
            // diagnostic here uses generic_string() to name files the same way
            // on Windows and POSIX.
            if (!m_scriptEngine.CompileScriptFile(path.string()))
            {
                Fail("Failed to compile: " + path.generic_string() + " — " + m_scriptEngine.GetLastError());
                return false;
            }

            const std::string text = source.str();
            const size_t first = text.find(SelfEntityDeclaration);
            if (first == std::string::npos)
            {
                Fail(path.generic_string() + ": " + std::string(asset.className) + " must declare '" +
                     std::string(SelfEntityDeclaration) + "' exactly once (found none)");
                return false;
            }
            const size_t second = text.find(SelfEntityDeclaration, first + SelfEntityDeclaration.size());
            if (second != std::string::npos)
            {
                Fail(path.generic_string() + ":" + std::to_string(LineOfOffset(text, second)) + ": " +
                     std::string(asset.className) + " declares '" + std::string(SelfEntityDeclaration) +
                     "' again (first at line " + std::to_string(LineOfOffset(text, first)) + ")");
                return false;
            }

            sources.emplace(std::string(asset.className), text);
            console.LogSuccess("[VisualScript] Validated: " + std::string(asset.className));
        }

        m_scriptRoot = *root;
        m_scriptSources = std::move(sources);
        console.LogInfo("[VisualScript] Validated 5 visual scripts from " + m_scriptRoot.string());
        return true;
    }

    bool DemoWorld::Spawn()
    {
        auto& console = SimpleConsole::GetInstance();
        DestroyEntities();
        m_lastError.clear();
        AngelScriptEngine::BindWorld(&m_world);

        // Returns false after rolling back everything this call created.
        const auto rollBack = [this]()
        {
            DestroyEntities();
            if (AngelScriptEngine::GetBoundWorld() == &m_world)
                AngelScriptEngine::BindWorld(nullptr);
            return false;
        };

        // --- Player: "PlayerController" handles WASD movement, sprint, jump, health ---
        {
            auto player = m_world.CreateEntity("VS_Player");
            m_world.AddComponent<Transform>(player, Transform{{0.0f, 1.0f, 0.0f}, {0, 0, 0}, {1, 1, 1}});
            m_world.AddComponent<HealthComponent>(player, HealthComponent{100.0f, 100.0f});
            m_world.AddComponent<MeshRenderer>(player).meshPath = "Assets/Models/character.obj";
            if (!AttachScript(player, "PlayerController"))
                return rollBack();
            console.LogInfo("[VisualScript] Spawned Player with PlayerController script");
        }

        // --- Collectibles: "Collectible" handles spin, proximity pickup and score increment ---
        for (int i = 0; i < 5; i++)
        {
            const float x = -10.0f + i * 5.0f;
            const float z = 8.0f + (i % 2) * 4.0f;

            auto coin = m_world.CreateEntity("VS_Coin_" + std::to_string(i));
            m_world.AddComponent<Transform>(coin, Transform{{x, 0.5f, z}, {0, 0, 0}, {0.5f, 0.5f, 0.5f}});
            auto& coinMesh = m_world.AddComponent<MeshRenderer>(coin);
            coinMesh.meshPath = "Assets/Models/Sphere.obj";
            coinMesh.emissive = 1.0f;
            if (!AttachScript(coin, "Collectible"))
                return rollBack();
        }
        console.LogInfo("[VisualScript] Spawned 5 collectible items with Collectible script");

        // --- Enemies: "EnemyPatrol" handles waypoint patrol, detection, chase, attack ---
        for (int i = 0; i < 3; i++)
        {
            const float x = 15.0f + i * 10.0f;

            auto enemy = m_world.CreateEntity("VS_Enemy_" + std::to_string(i));
            m_world.AddComponent<Transform>(enemy, Transform{{x, 0.0f, 5.0f}, {0, 0, 0}, {1, 1, 1}});
            m_world.AddComponent<HealthComponent>(enemy, HealthComponent{50.0f, 50.0f});
            m_world.AddComponent<MeshRenderer>(enemy).meshPath = "Assets/Models/Pyramid.obj";
            if (!AttachScript(enemy, "EnemyPatrol"))
                return rollBack();
        }
        console.LogInfo("[VisualScript] Spawned 3 enemies with EnemyPatrol script");

        // --- Game manager: "GameManager" tracks score (in HealthComponent::health) and win/lose ---
        {
            auto manager = m_world.CreateEntity("VS_GameManager");
            m_world.AddComponent<HealthComponent>(manager, HealthComponent{0.0f, 500.0f});
            if (!AttachScript(manager, "GameManager"))
                return rollBack();
            console.LogInfo("[VisualScript] Spawned GameManager with scoring/win-condition script");
        }

        // --- Healing pickup: "HealthPickup" handles proximity healing and respawn cooldown ---
        {
            auto heal = m_world.CreateEntity("VS_HealthPack");
            m_world.AddComponent<Transform>(heal, Transform{{-5.0f, 0.3f, -5.0f}, {0, 0, 0}, {0.7f, 0.7f, 0.7f}});
            auto& healthMesh = m_world.AddComponent<MeshRenderer>(heal);
            healthMesh.meshPath = "Assets/Models/Cube.obj";
            healthMesh.emissive = 0.5f;
            if (!AttachScript(heal, "HealthPickup"))
                return rollBack();
            console.LogInfo("[VisualScript] Spawned HealthPack with HealthPickup script");
        }

        if (m_entities.size() != ExpectedEntityCount)
        {
            Fail("Spawned " + std::to_string(m_entities.size()) + " script entities; the manifest requires " +
                 std::to_string(ExpectedEntityCount));
            return rollBack();
        }

        console.LogInfo("[VisualScript] All game entities spawned — 11 entities, 5 script types, 0 lines of C++ "
                        "game code");
        return true;
    }

    bool DemoWorld::AttachScript(EntityID entity, const std::string& className)
    {
        // Track the entity before any fallible operation so a failed partial
        // load is rolled back by DestroyEntities().
        m_entities.push_back(entity);

        const std::filesystem::path scriptFile = m_scriptRoot / (className + ".as");
        const std::string filePath = scriptFile.string();
        const std::string diagnosticPath = scriptFile.generic_string();
        const auto source = m_scriptSources.find(className);
        if (source == m_scriptSources.end())
        {
            Fail("Missing validated source for " + className + " (" + diagnosticPath +
                 "); LoadScripts() must succeed before Spawn()");
            return false;
        }

        const uint32_t entityValue = static_cast<uint32_t>(entity);
        const auto boundSource = BindSelfEntity(source->second, entityValue);
        if (!boundSource)
        {
            Fail(diagnosticPath + ": " + className + " must declare '" + std::string(SelfEntityDeclaration) +
                 "' exactly once");
            return false;
        }

        const std::string moduleName = className + "_Entity_" + std::to_string(entityValue);
        if (!m_scriptEngine.CompileScriptFromString(*boundSource, moduleName))
        {
            Fail("Failed to bind " + className + " to entity " + std::to_string(entityValue) + " — " +
                 LocateInFile(m_scriptEngine.GetLastError(), moduleName, diagnosticPath));
            return false;
        }

        Script script;
        script.scriptPath = filePath;
        script.className = className;
        script.moduleName = moduleName;
        m_world.AddComponent<Script>(entity, script);

        if (!m_scriptEngine.AttachScript(entity, className, moduleName))
        {
            Fail("Failed to attach " + className + " to entity " + std::to_string(entityValue) + " — " +
                 LocateInFile(m_scriptEngine.GetLastError(), moduleName, diagnosticPath));
            return false;
        }

        m_scriptEngine.CallStart(entity);
        m_world.GetComponent<Script>(entity)->started = true;
        return true;
    }

    void DemoWorld::DestroyEntities()
    {
        for (auto it = m_entities.rbegin(); it != m_entities.rend(); ++it)
        {
            m_scriptEngine.DetachScript(*it);
            if (m_world.GetRegistry().valid(*it))
                m_world.DestroyEntity(*it);
        }
        m_entities.clear();
    }
} // namespace Spark::VisualScriptDemo
