/**
 * @file LuaScriptEngine.cpp
 * @brief Lua scripting stub — logs warnings when Lua/Luau is not available
 *
 * Full implementation will be activated when SPARK_LUA_AVAILABLE is defined
 * and ThirdParty/Scripting/luau/ is populated.
 */

#include "LuaScriptEngine.h"
#include "../../Utils/LogMacros.h"
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
        SPARK_LOG_WARN(Spark::LogCategory::Scripting,
                       "[LuaScript] Lua scripting not available (SPARK_LUA_AVAILABLE not defined). "
                       "Add Luau to ThirdParty/Scripting/luau/ to enable.");
        return false;
    }

    void LuaScriptEngine::Shutdown()
    {
        m_instances.clear();
        m_initialized = false;
    }

    bool LuaScriptEngine::LoadScript(const std::string& /*path*/)
    {
        return false;
    }
    bool LuaScriptEngine::AttachScript(EntityID /*entity*/, const std::string& /*scriptPath*/)
    {
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
