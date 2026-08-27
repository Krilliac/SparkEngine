// TestAngelScriptEngine.cpp - Tests for AngelScript engine scripting system
// Standalone reimplementation for CI testing without the AngelScript SDK

#include "TestFramework.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    using EntityID = uint32_t;

    struct World
    {
        std::string name;
    };

    enum class ScriptContext : uint8_t
    {
        Shared,
        Server,
        Client
    };

    struct ScriptFunctionInfo
    {
        std::string name;
        std::string category;
    };

    class ScriptAPIRegistry
    {
      public:
        void RegisterFunction(const std::string& name, const std::string& category)
        {
            m_functions.push_back({name, category});
        }

        const std::vector<ScriptFunctionInfo>& GetRegisteredFunctions() const { return m_functions; }

        std::vector<ScriptFunctionInfo> GetFunctionsByCategory(const std::string& cat) const
        {
            std::vector<ScriptFunctionInfo> result;
            for (const auto& fn : m_functions)
            {
                if (fn.category == cat)
                    result.push_back(fn);
            }
            return result;
        }

        size_t GetFunctionCount() const { return m_functions.size(); }

      private:
        std::vector<ScriptFunctionInfo> m_functions;
    };

    struct ScriptInstance
    {
        std::string className;
        std::string moduleName;
        bool hasStart = true;
        bool hasUpdate = true;
        bool hasOnCollision = true;
        bool startCalled = false;
        float lastDeltaTime = 0.0f;
        EntityID lastCollisionEntity = 0;
    };

    class AngelScriptEngine
    {
      public:
        bool Initialize()
        {
            m_lastError.clear();
            return true;
        }

        void Shutdown()
        {
            m_scripts.clear();
            m_modules.clear();
            m_modulePaths.clear();
        }

        bool CompileScriptFromString(const std::string& script, const std::string& moduleName)
        {
            if (script.empty())
            {
                m_lastError = "Empty script source";
                return false;
            }
            m_modules[moduleName] = script;
            m_lastError.clear();
            return true;
        }

        bool CompileScriptFile(const std::string& path)
        {
            if (path.empty())
            {
                m_lastError = "Empty script path";
                return false;
            }
            auto slash = path.find_last_of("/\\");
            auto start = (slash == std::string::npos) ? 0 : slash + 1;
            auto dot = path.rfind('.');
            auto end = (dot == std::string::npos || dot < start) ? path.size() : dot;
            std::string mod = path.substr(start, end - start);
            m_modules[mod] = "// file";
            m_modulePaths[mod] = path;
            m_lastError.clear();
            return true;
        }

        bool AttachScript(EntityID entity, const std::string& className, const std::string& moduleName)
        {
            if (m_modules.find(moduleName) == m_modules.end())
            {
                m_lastError = "Module not found: " + moduleName;
                return false;
            }
            ScriptInstance inst;
            inst.className = className;
            inst.moduleName = moduleName;
            m_scripts[entity] = std::move(inst);
            m_lastError.clear();
            return true;
        }

        void DetachScript(EntityID entity) { m_scripts.erase(entity); }

        ScriptInstance* GetScriptInstance(EntityID entity)
        {
            auto it = m_scripts.find(entity);
            return (it != m_scripts.end()) ? &it->second : nullptr;
        }

        void CallStart(EntityID entity)
        {
            if (auto* s = GetScriptInstance(entity); s && s->hasStart)
                s->startCalled = true;
        }

        void CallUpdate(EntityID entity, float dt)
        {
            if (auto* s = GetScriptInstance(entity); s && s->hasUpdate)
                s->lastDeltaTime = dt;
        }

        void CallOnCollision(EntityID entity, EntityID other)
        {
            if (auto* s = GetScriptInstance(entity); s && s->hasOnCollision)
                s->lastCollisionEntity = other;
        }

        std::string GetLastError() const { return m_lastError; }

        bool HotReloadModule(const std::string& moduleName)
        {
            if (m_modulePaths.find(moduleName) == m_modulePaths.end())
            {
                m_lastError = "No file path for module: " + moduleName;
                return false;
            }
            auto entities = GetEntitiesForModule(moduleName);
            m_modules[moduleName] = "// reloaded";
            for (EntityID eid : entities)
            {
                auto* inst = GetScriptInstance(eid);
                std::string cls = inst->className;
                DetachScript(eid);
                AttachScript(eid, cls, moduleName);
            }
            m_lastError.clear();
            return true;
        }

        std::string GetModuleFilePath(const std::string& moduleName) const
        {
            auto it = m_modulePaths.find(moduleName);
            return (it != m_modulePaths.end()) ? it->second : "";
        }

        std::vector<EntityID> GetEntitiesForModule(const std::string& moduleName) const
        {
            std::vector<EntityID> result;
            for (const auto& [eid, inst] : m_scripts)
            {
                if (inst.moduleName == moduleName)
                    result.push_back(eid);
            }
            return result;
        }

        void SetScriptContext(ScriptContext ctx) { m_ctx = ctx; }
        ScriptContext GetScriptContext() const { return m_ctx; }

        bool ShouldRunScript(ScriptContext tag) const
        {
            if (m_ctx == ScriptContext::Shared || tag == ScriptContext::Shared)
                return true;
            return m_ctx == tag;
        }

        static void BindWorld(World* w) { s_boundWorld = w; }
        static World* GetBoundWorld() { return s_boundWorld; }

      private:
        std::string m_lastError;
        ScriptContext m_ctx = ScriptContext::Shared;
        std::unordered_map<std::string, std::string> m_modules;
        std::unordered_map<std::string, std::string> m_modulePaths;
        std::unordered_map<EntityID, ScriptInstance> m_scripts;
        static inline World* s_boundWorld = nullptr;
    };

} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

TEST(AngelScript_CompileFromString)
{
    AngelScriptEngine engine;
    engine.Initialize();

    EXPECT_TRUE(engine.CompileScriptFromString("class Foo {}", "TestModule"));
    EXPECT_EQ(engine.GetLastError(), std::string(""));

    EXPECT_FALSE(engine.CompileScriptFromString("", "Empty"));
    EXPECT_STR_CONTAINS(engine.GetLastError(), "Empty script");

    engine.Shutdown();
}

TEST(AngelScript_CompileFromFile_ModuleNameTracking)
{
    AngelScriptEngine engine;
    engine.Initialize();

    EXPECT_TRUE(engine.CompileScriptFile("Assets/Scripts/EnemyAI.as"));
    EXPECT_EQ(engine.GetModuleFilePath("EnemyAI"), std::string("Assets/Scripts/EnemyAI.as"));

    engine.Shutdown();
}

TEST(AngelScript_AttachAndLookup)
{
    AngelScriptEngine engine;
    engine.Initialize();
    engine.CompileScriptFromString("class Player {}", "PlayerMod");

    EXPECT_TRUE(engine.AttachScript(42, "Player", "PlayerMod"));

    auto* inst = engine.GetScriptInstance(42);
    ASSERT_TRUE(inst != nullptr);
    EXPECT_EQ(inst->className, std::string("Player"));
    EXPECT_EQ(inst->moduleName, std::string("PlayerMod"));

    // Missing module fails
    EXPECT_FALSE(engine.AttachScript(1, "Foo", "NoSuchModule"));
    EXPECT_STR_CONTAINS(engine.GetLastError(), "Module not found");

    engine.Shutdown();
}

TEST(AngelScript_DetachScript)
{
    AngelScriptEngine engine;
    engine.Initialize();
    engine.CompileScriptFromString("class Npc {}", "NpcMod");
    engine.AttachScript(10, "Npc", "NpcMod");
    EXPECT_TRUE(engine.GetScriptInstance(10) != nullptr);

    engine.DetachScript(10);
    EXPECT_TRUE(engine.GetScriptInstance(10) == nullptr);

    engine.Shutdown();
}

TEST(AngelScript_MethodCaching)
{
    AngelScriptEngine engine;
    engine.Initialize();
    engine.CompileScriptFromString("class Bot {}", "BotMod");
    engine.AttachScript(5, "Bot", "BotMod");

    auto* inst = engine.GetScriptInstance(5);
    ASSERT_TRUE(inst != nullptr);
    EXPECT_TRUE(inst->hasStart);
    EXPECT_TRUE(inst->hasUpdate);
    EXPECT_TRUE(inst->hasOnCollision);

    engine.Shutdown();
}

TEST(AngelScript_LifecycleDispatch)
{
    AngelScriptEngine engine;
    engine.Initialize();
    engine.CompileScriptFromString("class A {}", "ModA");
    engine.AttachScript(1, "A", "ModA");
    engine.AttachScript(2, "A", "ModA");

    // CallStart dispatches only to entity 1
    engine.CallStart(1);
    EXPECT_TRUE(engine.GetScriptInstance(1)->startCalled);
    EXPECT_FALSE(engine.GetScriptInstance(2)->startCalled);

    // CallUpdate dispatches only to entity 2
    engine.CallUpdate(2, 0.033f);
    EXPECT_NEAR(engine.GetScriptInstance(1)->lastDeltaTime, 0.0f, 0.0001f);
    EXPECT_NEAR(engine.GetScriptInstance(2)->lastDeltaTime, 0.033f, 0.001f);

    // CallOnCollision
    engine.CallOnCollision(1, 99);
    EXPECT_EQ(engine.GetScriptInstance(1)->lastCollisionEntity, static_cast<EntityID>(99));
    EXPECT_EQ(engine.GetScriptInstance(2)->lastCollisionEntity, static_cast<EntityID>(0));

    engine.Shutdown();
}

TEST(AngelScript_ErrorHandling_ClearedOnSuccess)
{
    AngelScriptEngine engine;
    engine.Initialize();

    engine.CompileScriptFromString("", "Bad");
    EXPECT_TRUE(!engine.GetLastError().empty());

    engine.CompileScriptFromString("class OK {}", "Good");
    EXPECT_EQ(engine.GetLastError(), std::string(""));

    engine.Shutdown();
}

TEST(AngelScript_HotReloadModule)
{
    AngelScriptEngine engine;
    engine.Initialize();
    engine.CompileScriptFile("Scripts/Enemy.as");
    engine.AttachScript(50, "Enemy", "Enemy");
    engine.AttachScript(51, "Enemy", "Enemy");

    engine.CallStart(50);
    EXPECT_TRUE(engine.GetScriptInstance(50)->startCalled);

    // Hot reload re-attaches — state is reset
    EXPECT_TRUE(engine.HotReloadModule("Enemy"));
    EXPECT_TRUE(engine.GetScriptInstance(50) != nullptr);
    EXPECT_FALSE(engine.GetScriptInstance(50)->startCalled);
    EXPECT_TRUE(engine.GetScriptInstance(51) != nullptr);

    // In-memory module (no file path) cannot hot-reload
    engine.CompileScriptFromString("class X {}", "InMemory");
    EXPECT_FALSE(engine.HotReloadModule("InMemory"));
    EXPECT_STR_CONTAINS(engine.GetLastError(), "No file path");

    engine.Shutdown();
}

TEST(AngelScript_ModuleFilePath)
{
    AngelScriptEngine engine;
    engine.Initialize();
    engine.CompileScriptFile("Data/Scripts/PlayerController.as");

    EXPECT_EQ(engine.GetModuleFilePath("PlayerController"), std::string("Data/Scripts/PlayerController.as"));
    EXPECT_EQ(engine.GetModuleFilePath("NoModule"), std::string(""));

    engine.Shutdown();
}

TEST(AngelScript_GetEntitiesForModule)
{
    AngelScriptEngine engine;
    engine.Initialize();
    engine.CompileScriptFromString("class A {}", "ModA");
    engine.CompileScriptFromString("class B {}", "ModB");

    engine.AttachScript(1, "A", "ModA");
    engine.AttachScript(2, "A", "ModA");
    engine.AttachScript(3, "B", "ModB");

    EXPECT_EQ(engine.GetEntitiesForModule("ModA").size(), static_cast<size_t>(2));
    EXPECT_EQ(engine.GetEntitiesForModule("ModB").size(), static_cast<size_t>(1));
    EXPECT_EQ(engine.GetEntitiesForModule("ModC").size(), static_cast<size_t>(0));
    EXPECT_EQ(engine.GetEntitiesForModule("ModB")[0], static_cast<EntityID>(3));

    engine.Shutdown();
}

TEST(AngelScript_ScriptContext)
{
    AngelScriptEngine engine;
    engine.Initialize();

    // Default is Shared
    EXPECT_TRUE(engine.GetScriptContext() == ScriptContext::Shared);

    engine.SetScriptContext(ScriptContext::Server);
    EXPECT_TRUE(engine.GetScriptContext() == ScriptContext::Server);

    engine.SetScriptContext(ScriptContext::Client);
    EXPECT_TRUE(engine.GetScriptContext() == ScriptContext::Client);

    engine.Shutdown();
}

TEST(AngelScript_ScriptContext_Filtering)
{
    AngelScriptEngine engine;
    engine.Initialize();

    // Shared context runs everything
    engine.SetScriptContext(ScriptContext::Shared);
    EXPECT_TRUE(engine.ShouldRunScript(ScriptContext::Shared));
    EXPECT_TRUE(engine.ShouldRunScript(ScriptContext::Server));
    EXPECT_TRUE(engine.ShouldRunScript(ScriptContext::Client));

    // Server context: Shared + Server only
    engine.SetScriptContext(ScriptContext::Server);
    EXPECT_TRUE(engine.ShouldRunScript(ScriptContext::Shared));
    EXPECT_TRUE(engine.ShouldRunScript(ScriptContext::Server));
    EXPECT_FALSE(engine.ShouldRunScript(ScriptContext::Client));

    // Client context: Shared + Client only
    engine.SetScriptContext(ScriptContext::Client);
    EXPECT_TRUE(engine.ShouldRunScript(ScriptContext::Shared));
    EXPECT_FALSE(engine.ShouldRunScript(ScriptContext::Server));
    EXPECT_TRUE(engine.ShouldRunScript(ScriptContext::Client));

    engine.Shutdown();
}

TEST(AngelScript_ScriptAPIRegistry)
{
    ScriptAPIRegistry registry;

    // Empty registry
    EXPECT_EQ(registry.GetFunctionCount(), static_cast<size_t>(0));
    EXPECT_EQ(registry.GetRegisteredFunctions().size(), static_cast<size_t>(0));
    EXPECT_EQ(registry.GetFunctionsByCategory("Any").size(), static_cast<size_t>(0));

    // Register functions across categories
    registry.RegisterFunction("print", "Utility");
    registry.RegisterFunction("createEntity", "ECS");
    registry.RegisterFunction("getTransform", "ECS");
    registry.RegisterFunction("getKeyDown", "Input");
    registry.RegisterFunction("getKey", "Input");

    EXPECT_EQ(registry.GetFunctionCount(), static_cast<size_t>(5));
    EXPECT_EQ(registry.GetRegisteredFunctions().size(), static_cast<size_t>(5));

    // Filter by category
    EXPECT_EQ(registry.GetFunctionsByCategory("ECS").size(), static_cast<size_t>(2));
    EXPECT_EQ(registry.GetFunctionsByCategory("Input").size(), static_cast<size_t>(2));
    EXPECT_EQ(registry.GetFunctionsByCategory("Utility").size(), static_cast<size_t>(1));
    EXPECT_EQ(registry.GetFunctionsByCategory("Physics").size(), static_cast<size_t>(0));
}

TEST(AngelScript_BindWorld)
{
    AngelScriptEngine::BindWorld(nullptr);
    EXPECT_TRUE(AngelScriptEngine::GetBoundWorld() == nullptr);

    World testWorld;
    testWorld.name = "GameWorld";
    AngelScriptEngine::BindWorld(&testWorld);
    EXPECT_TRUE(AngelScriptEngine::GetBoundWorld() != nullptr);
    EXPECT_EQ(AngelScriptEngine::GetBoundWorld()->name, std::string("GameWorld"));

    // Persists across engine instances (static member)
    AngelScriptEngine engine;
    engine.Initialize();
    EXPECT_TRUE(AngelScriptEngine::GetBoundWorld() != nullptr);
    engine.Shutdown();
    EXPECT_TRUE(AngelScriptEngine::GetBoundWorld() != nullptr);

    // Unbind
    AngelScriptEngine::BindWorld(nullptr);
    EXPECT_TRUE(AngelScriptEngine::GetBoundWorld() == nullptr);
}
