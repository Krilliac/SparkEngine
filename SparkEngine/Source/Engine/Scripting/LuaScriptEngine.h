/**
 * @file LuaScriptEngine.h
 * @brief Lua scripting engine for gameplay logic (LumixEngine-inspired)
 *
 * Optional secondary scripting language alongside AngelScript. Provides a
 * simpler, more approachable syntax for designers and rapid iteration.
 *
 * ## Architecture
 * - Each entity can have a Lua script component
 * - Scripts run in sandboxed environments (no file I/O, no OS access)
 * - Hot-reload: file changes trigger automatic recompilation
 * - Engine API exposed to Lua: entity queries, input, math, audio
 *
 * ## Build
 * Guarded by `SPARK_LUA_AVAILABLE`. When Lua/Luau is not available,
 * all methods are no-ops that log warnings (same pattern as AngelScriptEngine).
 *
 * ## Usage
 * ```lua
 * -- Scripts/Player.lua
 * function OnStart(entity)
 *     entity:SetHealth(100)
 * end
 *
 * function OnUpdate(entity, dt)
 *     local input = Engine.GetInput()
 *     if input:IsKeyDown("W") then
 *         entity:MoveForward(5.0 * dt)
 *     end
 * end
 * ```
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Scripting
{

    using EntityID = uint32_t;

    /// Script lifecycle callbacks exposed to Lua.
    enum class LuaCallback
    {
        OnStart,
        OnUpdate,
        OnDestroy,
        OnCollisionEnter,
        OnCollisionExit
    };

    /// Per-entity Lua script instance data.
    struct LuaScriptInstance
    {
        std::string scriptPath;    ///< Path to .lua file
        std::string moduleName;    ///< Lua module name
        bool initialized = false;  ///< Whether OnStart has been called
        bool hasUpdate = false;    ///< Whether script defines OnUpdate
        bool hasCollision = false; ///< Whether script defines collision callbacks
    };

    /**
     * @brief Lua scripting engine with sandboxed execution and hot-reload.
     *
     * Follows the same singleton + stub pattern as AngelScriptEngine.
     * When SPARK_LUA_AVAILABLE is not defined, all methods are safe no-ops.
     */
    class LuaScriptEngine
    {
      public:
        static LuaScriptEngine& GetInstance()
        {
            static LuaScriptEngine instance;
            return instance;
        }

        /// Initialize the Lua VM and register engine API bindings.
        bool Initialize();

        /// Shut down the VM and release all script instances.
        void Shutdown();

        /// Load and compile a Lua script file.
        bool LoadScript(const std::string& path);

        /// Attach a script to an entity.
        bool AttachScript(EntityID entity, const std::string& scriptPath);

        /// Detach a script from an entity.
        void DetachScript(EntityID entity);

        /// Call OnStart for all newly attached scripts.
        void CallStart();

        /// Call OnUpdate for all active scripts.
        void CallUpdate(float deltaTime);

        /// Hot-reload a specific script file.
        bool HotReloadScript(const std::string& path);

        /// Check if the Lua VM is available.
        bool IsAvailable() const;

        /// Get number of active script instances.
        size_t GetActiveScriptCount() const { return m_instances.size(); }

        /// Console integration.
        std::string Console_GetStatus() const;
        std::string Console_ListScripts() const;

      private:
        LuaScriptEngine() = default;
        ~LuaScriptEngine();

        LuaScriptEngine(const LuaScriptEngine&) = delete;
        LuaScriptEngine& operator=(const LuaScriptEngine&) = delete;

        std::unordered_map<EntityID, LuaScriptInstance> m_instances;
        bool m_initialized = false;

#ifdef SPARK_LUA_AVAILABLE
        // Lua state would be stored here when Luau is integrated
        // lua_State* m_luaState = nullptr;
#endif
    };

} // namespace Spark::Scripting
