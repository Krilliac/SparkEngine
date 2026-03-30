/**
 * @file LuaScriptEngine.cpp
 * @brief Lua scripting stub — logs warnings when Lua/Luau is not available
 *
 * Full implementation will be activated when SPARK_LUA_AVAILABLE is defined
 * and ThirdParty/Scripting/luau/ is populated.
 */

#include "LuaScriptEngine.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/SparkConsole.h"
#include <sstream>

namespace Spark::Scripting
{

    LuaScriptEngine::~LuaScriptEngine()
    {
        Shutdown();
    }

#ifdef SPARK_LUA_AVAILABLE

    // Full Lua implementation would go here when Luau is integrated.
    // For now, this section is empty — the #else stub below handles everything.

#else // !SPARK_LUA_AVAILABLE

    bool LuaScriptEngine::Initialize()
    {
        SPARK_LOG_WARN(Spark::LogCategory::Scripting, "Lua scripting not available (SPARK_LUA_AVAILABLE not defined)");
        Spark::SimpleConsole::GetInstance().LogWarning(
            "[LuaScript] Lua scripting not available (SPARK_LUA_AVAILABLE not defined). "
            "Add Luau to ThirdParty/Scripting/luau/ to enable.");
        return false;
    }

    void LuaScriptEngine::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Scripting, "LuaScriptEngine shutting down (%zu instances)",
                       m_instances.size());
        m_instances.clear();
        m_initialized = false;
    }

    bool LuaScriptEngine::LoadScript(const std::string& /*path*/)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Scripting, "LoadScript called but Lua is not available");
        return false;
    }
    bool LuaScriptEngine::AttachScript(EntityID /*entity*/, const std::string& /*scriptPath*/)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Scripting, "AttachScript called but Lua is not available");
        return false;
    }
    void LuaScriptEngine::DetachScript(EntityID entity)
    {
        m_instances.erase(entity);
    }
    void LuaScriptEngine::CallStart() {}
    void LuaScriptEngine::CallUpdate(float /*deltaTime*/) {}
    bool LuaScriptEngine::HotReloadScript(const std::string& /*path*/)
    {
        return false;
    }
    bool LuaScriptEngine::IsAvailable() const
    {
        return false;
    }

    std::string LuaScriptEngine::Console_GetStatus() const
    {
        return "Lua scripting: NOT AVAILABLE (Luau not integrated)\n";
    }

    std::string LuaScriptEngine::Console_ListScripts() const
    {
        return "No Lua scripts loaded (engine unavailable)\n";
    }

#endif // SPARK_LUA_AVAILABLE

} // namespace Spark::Scripting
