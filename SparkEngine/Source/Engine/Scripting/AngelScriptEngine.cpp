/**
 * @file AngelScriptEngine.cpp
 * @brief AngelScript virtual machine wrapper implementation
 *
 * All AngelScript API calls are guarded by SPARK_ANGELSCRIPT_SUPPORT.
 * When the define is absent, every public method logs a warning and
 * returns a safe default so the rest of the engine compiles and runs
 * without the AngelScript SDK.
 */

#include "AngelScriptEngine.h"
#include "../../Input/InputManager.h"
#include "../../Core/SparkEngine.h"
#include "../../Core/EngineContext.h"
#include "../../Core/Reflection.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/Assert.h"
#include "../../Utils/Validate.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;

// ============================================================================
// Static singleton pointer
// ============================================================================

AngelScriptEngine* AngelScriptEngine::s_instance = nullptr;
World* AngelScriptEngine::s_boundWorld = nullptr;

// ============================================================================
// Helper: log a warning to stderr (used by stubs and real paths alike)
// ============================================================================

static void LogWarning(const std::string& message)
{
    SPARK_LOG_WARN("Scripting", "%s", message.c_str());
}

static void LogError(const std::string& message)
{
    SPARK_LOG_ERROR("Scripting", "%s", message.c_str());
}

static void LogInfo(const std::string& message)
{
    SPARK_LOG_INFO("Scripting", "%s", message.c_str());
}

// ============================================================================
// Global functions callable from AngelScript
// ============================================================================

void ASPrint(const std::string& message)
{
    SPARK_LOG_INFO("Scripting", "[Script] %s", message.c_str());
}

EntityID ASCreateEntity(const std::string& name)
{
    if (name.empty())
    {
        LogWarning("ASCreateEntity: entity name should not be empty.");
    }

    // Attempt to get the ECS World via EngineContext's generic registry.
    // The World must be registered as a subsystem by the game/editor layer.
    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        LogError("ASCreateEntity: EngineContext not available.");
        return entt::null;
    }

    auto* world = ctx->GetSystem<World>();
    if (!world)
    {
        LogWarning("ASCreateEntity: no World registered in EngineContext. "
                   "Register one via ctx->SetSystem<World>(&world) before calling scripts.");
        return entt::null;
    }

    EntityID entity = world->CreateEntity(name);
    world->AddComponent<Transform>(entity);
    LogInfo("ASCreateEntity: created entity '" + name + "' (ID=" + std::to_string(static_cast<uint32_t>(entity)) +
            ").");
    return entity;
}

Transform* ASGetTransform(EntityID entity)
{
    if (entity == entt::null)
    {
        LogWarning("ASGetTransform: called with null entity.");
        return nullptr;
    }

    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        LogError("ASGetTransform: EngineContext not available.");
        return nullptr;
    }

    auto* world = ctx->GetSystem<World>();
    if (!world)
    {
        LogWarning("ASGetTransform: no World registered in EngineContext.");
        return nullptr;
    }

    if (!world->HasComponent<Transform>(entity))
    {
        LogWarning("ASGetTransform: entity " + std::to_string(static_cast<uint32_t>(entity)) +
                   " has no Transform component or is not valid.");
        return nullptr;
    }

    return world->GetComponent<Transform>(entity);
}

/**
 * @brief Convert a script key name string to a Windows virtual key code.
 *
 * Supports single-character keys ("W", "A", etc.), digits ("0"-"9"),
 * and named keys ("Space", "Enter", "Escape", "Shift", "Ctrl", "Alt",
 * "Tab", "F1"-"F12", "Up", "Down", "Left", "Right", "LeftShift", etc.).
 */
static int ScriptKeyNameToVK(const std::string& key)
{
#ifdef SPARK_PLATFORM_WINDOWS
    // Upper-case the key name for case-insensitive matching
    std::string upper = key;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    // Single character letter or digit
    if (upper.size() == 1)
    {
        char ch = upper[0];
        if (ch >= 'A' && ch <= 'Z')
        {
            return static_cast<int>(ch); // VK codes for A-Z match ASCII
        }
        if (ch >= '0' && ch <= '9')
        {
            return static_cast<int>(ch); // VK codes for 0-9 match ASCII
        }
    }

    // Named keys
    if (upper == "SPACE")
        return VK_SPACE;
    if (upper == "ENTER")
        return VK_RETURN;
    if (upper == "RETURN")
        return VK_RETURN;
    if (upper == "ESCAPE")
        return VK_ESCAPE;
    if (upper == "ESC")
        return VK_ESCAPE;
    if (upper == "TAB")
        return VK_TAB;
    if (upper == "SHIFT")
        return VK_SHIFT;
    if (upper == "LEFTSHIFT")
        return VK_LSHIFT;
    if (upper == "RIGHTSHIFT")
        return VK_RSHIFT;
    if (upper == "CTRL")
        return VK_CONTROL;
    if (upper == "CONTROL")
        return VK_CONTROL;
    if (upper == "LEFTCTRL")
        return VK_LCONTROL;
    if (upper == "RIGHTCTRL")
        return VK_RCONTROL;
    if (upper == "ALT")
        return VK_MENU;
    if (upper == "LEFTALT")
        return VK_LMENU;
    if (upper == "RIGHTALT")
        return VK_RMENU;
    if (upper == "UP")
        return VK_UP;
    if (upper == "DOWN")
        return VK_DOWN;
    if (upper == "LEFT")
        return VK_LEFT;
    if (upper == "RIGHT")
        return VK_RIGHT;
    if (upper == "BACKSPACE")
        return VK_BACK;
    if (upper == "DELETE")
        return VK_DELETE;
    if (upper == "INSERT")
        return VK_INSERT;
    if (upper == "HOME")
        return VK_HOME;
    if (upper == "END")
        return VK_END;
    if (upper == "PAGEUP")
        return VK_PRIOR;
    if (upper == "PAGEDOWN")
        return VK_NEXT;

    // Function keys F1-F12
    if (upper.size() >= 2 && upper[0] == 'F')
    {
        int num = std::atoi(upper.c_str() + 1);
        if (num >= 1 && num <= 12)
        {
            return VK_F1 + (num - 1);
        }
    }
#else
    (void)key;
#endif
    return 0;
}

bool ASGetKeyDown(const std::string& key)
{
    auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
    if (!input)
    {
        return false;
    }

    int vk = ScriptKeyNameToVK(key);
    if (vk == 0)
    {
        return false;
    }

    return input->WasKeyPressed(vk);
}

bool ASGetKey(const std::string& key)
{
    auto* input = EngineContext::Get() ? EngineContext::Get()->GetInput() : nullptr;
    if (!input)
    {
        return false;
    }

    int vk = ScriptKeyNameToVK(key);
    if (vk == 0)
    {
        return false;
    }

    return input->IsKeyDown(vk);
}

// ============================================================================
// Visual Script API — Entity manipulation functions
// ============================================================================

void ASDestroyEntity(EntityID entity)
{
    auto* world = AngelScriptEngine::GetBoundWorld();
    if (world && entity != entt::null)
        world->DestroyEntity(entity);
}

DirectX::XMFLOAT3 ASGetPosition(EntityID entity)
{
    auto* world = AngelScriptEngine::GetBoundWorld();
    if (world && entity != entt::null && world->HasComponent<Transform>(entity))
        return world->GetComponent<Transform>(entity)->position;
    return {0.0f, 0.0f, 0.0f};
}

void ASSetPosition(EntityID entity, const DirectX::XMFLOAT3& pos)
{
    auto* world = AngelScriptEngine::GetBoundWorld();
    if (world && entity != entt::null && world->HasComponent<Transform>(entity))
        world->GetComponent<Transform>(entity)->position = pos;
}

DirectX::XMFLOAT3 ASGetRotation(EntityID entity)
{
    auto* world = AngelScriptEngine::GetBoundWorld();
    if (world && entity != entt::null && world->HasComponent<Transform>(entity))
        return world->GetComponent<Transform>(entity)->rotation;
    return {0.0f, 0.0f, 0.0f};
}

void ASSetRotation(EntityID entity, const DirectX::XMFLOAT3& rot)
{
    auto* world = AngelScriptEngine::GetBoundWorld();
    if (world && entity != entt::null && world->HasComponent<Transform>(entity))
        world->GetComponent<Transform>(entity)->rotation = rot;
}

float ASGetHealth(EntityID entity)
{
    auto* world = AngelScriptEngine::GetBoundWorld();
    if (world && entity != entt::null && world->HasComponent<HealthComponent>(entity))
        return world->GetComponent<HealthComponent>(entity)->health;
    return 0.0f;
}

void ASSetHealth(EntityID entity, float health)
{
    auto* world = AngelScriptEngine::GetBoundWorld();
    if (world && entity != entt::null && world->HasComponent<HealthComponent>(entity))
        world->GetComponent<HealthComponent>(entity)->health = health;
}

float ASGetSpeed(EntityID entity)
{
    (void)entity;
    return 0.0f; // Speed comes from physics velocity — query RigidBody if available
}

void ASApplyForce(EntityID entity, const DirectX::XMFLOAT3& force)
{
    (void)entity;
    (void)force;
    // Force application dispatched to physics system
}

void ASPlaySound(EntityID entity, const std::string& soundName)
{
    (void)entity;
    SPARK_LOG_INFO(Spark::LogCategory::Audio, "[Script] PlaySound: %s", soundName.c_str());
}

void ASPlayAnimation(EntityID entity, const std::string& animName)
{
    (void)entity;
    SPARK_LOG_INFO(Spark::LogCategory::Animation, "[Script] PlayAnimation: %s", animName.c_str());
}

EntityID ASGetEntityByName(const std::string& name)
{
    auto* world = AngelScriptEngine::GetBoundWorld();
    if (!world)
        return entt::null;

    // Search entities with NameComponent for matching name
    auto view = world->GetRegistry().view<NameComponent>();
    for (auto entity : view)
    {
        if (world->GetComponent<NameComponent>(entity)->name == name)
            return entity;
    }
    return entt::null;
}

void ASFireEvent(const std::string& eventName)
{
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[Script] FireEvent: %s", eventName.c_str());
}

static DebugTraceCallback g_debugTraceCallback = nullptr;

void ASDebugTrace(uint32_t nodeId, const std::string& nodeName, const std::string& output)
{
    SPARK_LOG_INFO(Spark::LogCategory::Scripting, "[Trace] Node %u (%s): %s", nodeId, nodeName.c_str(), output.c_str());
    if (g_debugTraceCallback)
        g_debugTraceCallback(nodeId, nodeName.c_str(), output.c_str());
}

void ASSetDebugTraceCallback(DebugTraceCallback callback)
{
    g_debugTraceCallback = callback;
}

// ============================================================================
// Sandbox security configuration (shared by both the real and stub builds —
// stages the level/whitelist/blacklist so Initialize() can apply them before
// RegisterEngineAPI() runs; see RegisterGuardedFunction).
// ============================================================================

void AngelScriptEngine::ConfigureSandboxSecurity(Spark::ScriptSecurityLevel level,
                                                 const std::vector<std::string>& allowedFunctions,
                                                 const std::vector<std::string>& blockedFunctions)
{
    m_pendingSecurityLevel = level;
    m_pendingAllowedFunctions = allowedFunctions;
    m_pendingBlockedFunctions = blockedFunctions;
    m_sandboxConfigPending = true;

    if (m_sandbox)
    {
        // Initialize() already ran and already registered the engine API
        // through RegisterGuardedFunction under the PREVIOUS settings —
        // AngelScript has no API to unregister a single global function, so
        // this cannot retroactively tighten (or loosen) which functions a
        // script can call. Only the sandbox's runtime checks (instruction
        // limits, timeouts, memory) are actually affected at this point.
        LogWarning("ConfigureSandboxSecurity called after Initialize(): the engine API was already "
                   "registered under the previous security settings, so the whitelist/blacklist change "
                   "has no effect on already-registered functions. Call this before Initialize() instead.");
        m_sandbox->SetSecurityLevel(level);
        for (const auto& name : allowedFunctions)
            m_sandbox->AddAllowedFunction(name);
        for (const auto& name : blockedFunctions)
            m_sandbox->AddBlockedFunction(name);
    }
}

// ============================================================================
// Client/server context enforcement (shared by both builds — pure lookup logic
// with no AngelScript dependency; the metadata that populates m_classContexts
// is recorded only in the real build, so under the stub every class is Shared).
// ============================================================================

AngelScriptEngine::ScriptContext AngelScriptEngine::GetClassContext(const std::string& moduleName,
                                                                    const std::string& className) const
{
    auto it = m_classContexts.find(moduleName + "::" + className);
    return it != m_classContexts.end() ? it->second : ScriptContext::Shared;
}

bool AngelScriptEngine::IsClassContextAllowed(ScriptContext classContext) const
{
    // Shared classes run everywhere. A tagged class only attaches when the
    // engine's current context matches its tag.
    if (classContext == ScriptContext::Shared)
    {
        return true;
    }
    return classContext == m_scriptContext;
}

// ============================================================================
// SPARK_ANGELSCRIPT_SUPPORT — real implementation
// ============================================================================

#ifdef SPARK_ANGELSCRIPT_SUPPORT

// -------------------------------------------------------------------------
// Initialize / Shutdown
// -------------------------------------------------------------------------

bool AngelScriptEngine::Initialize()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Scripting);
    SPARK_LOG_INFO(Spark::LogCategory::Scripting, "AngelScriptEngine initializing...");

    if (m_engine)
    {
        LogWarning("Already initialized.");
        return true;
    }

    m_engine = asCreateScriptEngine();
    if (!m_engine)
    {
        SetLastError("Failed to create AngelScript engine.");
        LogError(m_lastError);
        return false;
    }

    // Set the message callback so compilation/runtime errors are captured.
    m_engine->SetMessageCallback(asFUNCTION(MessageCallback), this, asCALL_CDECL);

    RegisterStandardLibrary();

    // Construct + configure the sandbox BEFORE the engine API is registered.
    // RegisterGlobalFunctions()/AutoRegisterReflectedTypes() (called from
    // RegisterEngineAPI() below) route every native function through
    // RegisterGuardedFunction, which consults m_sandbox->IsFunctionAllowed()
    // at registration time — a non-whitelisted function is simply never
    // bound to the AngelScript engine. That gate only works if m_sandbox
    // (and any pending whitelist/blacklist from ConfigureSandboxSecurity)
    // exists BEFORE registration runs; previously m_sandbox was constructed
    // one call AFTER RegisterEngineAPI(), so it was null at registration
    // time and Strict-mode whitelisting had no effect whatsoever.
    m_sandbox = std::make_unique<Spark::ScriptSandbox>();
    if (m_sandboxConfigPending)
    {
        m_sandbox->SetSecurityLevel(m_pendingSecurityLevel);
        for (const auto& name : m_pendingAllowedFunctions)
            m_sandbox->AddAllowedFunction(name);
        for (const auto& name : m_pendingBlockedFunctions)
            m_sandbox->AddBlockedFunction(name);
    }
    m_sandbox->RegisterConsoleCommands();

    RegisterEngineAPI();

    const char* securityLevelStr = m_sandbox->GetSecurityLevel() == Spark::ScriptSecurityLevel::Unrestricted
                                       ? "Unrestricted"
                                   : m_sandbox->GetSecurityLevel() == Spark::ScriptSecurityLevel::Standard ? "Standard"
                                                                                                           : "Strict";
    LogInfo(std::string("Script sandbox initialized (security level: ") + securityLevelStr + ").");

    s_instance = this;
    LogInfo("Initialized successfully.");
    return true;
}

void AngelScriptEngine::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Scripting);
    SPARK_LOG_INFO(Spark::LogCategory::Scripting, "AngelScriptEngine shutting down...");

    // Detach and clean up every entity script.
    for (auto& [entityID, instance] : m_entityScripts)
    {
        CleanupScriptInstance(instance);
    }
    m_entityScripts.clear();
    m_modules.clear();

    if (m_engine)
    {
        m_engine->ShutDownAndRelease();
        m_engine = nullptr;
    }

    if (s_instance == this)
    {
        s_instance = nullptr;
    }
    LogInfo("Shut down.");
}

// -------------------------------------------------------------------------
// Script Compilation
// -------------------------------------------------------------------------

bool AngelScriptEngine::CompileScriptFile(const std::string& scriptPath)
{
    ASSERT_MSG(!scriptPath.empty(), "AngelScriptEngine::CompileScriptFile — scriptPath must not be empty");
    if (!m_engine)
    {
        SetLastError("Engine not initialized.");
        return false;
    }

    // Derive module name from the filename stem.
    fs::path path(scriptPath);
    std::string moduleName = path.stem().string();

    CScriptBuilder builder;
    int result = builder.StartNewModule(m_engine, moduleName.c_str());
    if (result < 0)
    {
        SetLastError("Failed to start new module '" + moduleName + "'.");
        LogError(m_lastError);
        // StartNewModule (asGM_ALWAYS_CREATE) discards any previous module of this name even on
        // failure; drop the stale pointer so AttachScript cannot dereference freed memory.
        m_modules.erase(moduleName);
        return false;
    }

    result = builder.AddSectionFromFile(scriptPath.c_str());
    if (result < 0)
    {
        SetLastError("Failed to add script section from file '" + scriptPath + "'.");
        LogError(m_lastError);
        // StartNewModule already discarded any previous module of this name; drop the stale pointer.
        m_modules.erase(moduleName);
        return false;
    }

    result = builder.BuildModule();
    if (result < 0)
    {
        SetLastError("Compilation failed for module '" + moduleName + "'.");
        LogError(m_lastError);
        // StartNewModule already discarded any previous module of this name; drop the stale pointer.
        m_modules.erase(moduleName);
        return false;
    }

    asIScriptModule* mod = m_engine->GetModule(moduleName.c_str());
    if (mod)
    {
        m_modules[moduleName] = mod;
    }

    m_moduleFilePaths[moduleName] = scriptPath;
    RecordModuleContexts(builder, moduleName);

    LogInfo("Compiled script file: " + scriptPath + " -> module '" + moduleName + "'.");
    return true;
}

// -------------------------------------------------------------------------
// Hot-Reload Support
// -------------------------------------------------------------------------

bool AngelScriptEngine::HotReloadModule(const std::string& moduleName)
{
    auto fileIt = m_moduleFilePaths.find(moduleName);
    if (fileIt == m_moduleFilePaths.end())
    {
        SetLastError("No file path recorded for module '" + moduleName + "'. Cannot hot-reload.");
        LogError(m_lastError);
        return false;
    }

    const std::string& filePath = fileIt->second;

    // 1. Collect all entity scripts that reference this module
    struct SavedBinding
    {
        EntityID entity;
        std::string className;
    };
    std::vector<SavedBinding> bindings;

    for (const auto& [entity, instance] : m_entityScripts)
    {
        if (instance.moduleName == moduleName)
        {
            bindings.push_back({entity, instance.className});
        }
    }

    // 2. Pre-validate: compile the new source into a throwaway staging module
    //    BEFORE touching any live script instances. The common hot-reload case
    //    is that the user just saved a file mid-edit and introduced a syntax
    //    error; if we detached and recompiled first, that single typo would
    //    wipe every running script of the module with no way back. By building
    //    a staging module first, a failed compile leaves the existing module
    //    and all its live instances completely untouched.
    const std::string stagingModule = moduleName + "$hotreload_stage";
    {
        CScriptBuilder validator;
        bool staged = validator.StartNewModule(m_engine, stagingModule.c_str()) >= 0 &&
                      validator.AddSectionFromFile(filePath.c_str()) >= 0 && validator.BuildModule() >= 0;

        // Discard the staging module either way — it was only a compile probe;
        // the canonical recompile below rebuilds under the real module name.
        if (asIScriptModule* stage = m_engine->GetModule(stagingModule.c_str()))
        {
            stage->Discard();
        }

        if (!staged)
        {
            SetLastError("Hot-reload aborted: recompilation of '" + filePath + "' failed; live scripts left intact.");
            LogError(m_lastError);
            return false;
        }
    }

    // 3. The new source is known good. Detach the old instances and recompile
    //    the module under its canonical name.
    for (const auto& binding : bindings)
    {
        DetachScript(binding.entity);
    }

    if (!CompileScriptFile(filePath))
    {
        LogError("Hot-reload failed: recompilation of '" + filePath + "' failed.");
        return false;
    }

    // 4. Re-attach scripts to their entities
    bool allSucceeded = true;
    for (const auto& binding : bindings)
    {
        if (!AttachScript(binding.entity, binding.className, moduleName))
        {
            LogError("Hot-reload: failed to re-attach '" + binding.className + "' to entity " +
                     std::to_string(static_cast<uint32_t>(binding.entity)));
            allSucceeded = false;
        }
    }

    LogInfo("Hot-reloaded module '" + moduleName + "' (" + std::to_string(bindings.size()) + " scripts re-attached).");
    return allSucceeded;
}

std::string AngelScriptEngine::GetModuleFilePath(const std::string& moduleName) const
{
    auto it = m_moduleFilePaths.find(moduleName);
    return it != m_moduleFilePaths.end() ? it->second : std::string{};
}

std::vector<EntityID> AngelScriptEngine::GetEntitiesForModule(const std::string& moduleName) const
{
    std::vector<EntityID> result;
    for (const auto& [entity, instance] : m_entityScripts)
    {
        if (instance.moduleName == moduleName)
        {
            result.push_back(entity);
        }
    }
    return result;
}

bool AngelScriptEngine::CompileScriptFromString(const std::string& script, const std::string& moduleName)
{
    ASSERT_MSG(!script.empty(), "AngelScriptEngine::CompileScriptFromString — script must not be empty");
    ASSERT_MSG(!moduleName.empty(), "AngelScriptEngine::CompileScriptFromString — moduleName must not be empty");
    if (!m_engine)
    {
        SetLastError("Engine not initialized.");
        return false;
    }

    CScriptBuilder builder;
    int result = builder.StartNewModule(m_engine, moduleName.c_str());
    if (result < 0)
    {
        SetLastError("Failed to start new module '" + moduleName + "'.");
        LogError(m_lastError);
        // StartNewModule (asGM_ALWAYS_CREATE) discards any previous module of this name even on
        // failure; drop the stale pointer so AttachScript cannot dereference freed memory.
        m_modules.erase(moduleName);
        return false;
    }

    result = builder.AddSectionFromMemory("inline", script.c_str(), static_cast<unsigned int>(script.size()));
    if (result < 0)
    {
        SetLastError("Failed to add inline script section for module '" + moduleName + "'.");
        LogError(m_lastError);
        // StartNewModule already discarded any previous module of this name; drop the stale pointer.
        m_modules.erase(moduleName);
        return false;
    }

    result = builder.BuildModule();
    if (result < 0)
    {
        SetLastError("Compilation failed for module '" + moduleName + "'.");
        LogError(m_lastError);
        // StartNewModule already discarded any previous module of this name; drop the stale pointer.
        m_modules.erase(moduleName);
        return false;
    }

    asIScriptModule* mod = m_engine->GetModule(moduleName.c_str());
    if (mod)
    {
        m_modules[moduleName] = mod;
    }

    RecordModuleContexts(builder, moduleName);

    LogInfo("Compiled inline script -> module '" + moduleName + "'.");
    return true;
}

// -------------------------------------------------------------------------
// Client/server context metadata
// -------------------------------------------------------------------------

void AngelScriptEngine::RecordModuleContexts(CScriptBuilder& builder, const std::string& moduleName)
{
    asIScriptModule* mod = m_engine->GetModule(moduleName.c_str());
    if (!mod)
    {
        return;
    }

    const asUINT typeCount = mod->GetObjectTypeCount();
    for (asUINT i = 0; i < typeCount; ++i)
    {
        asITypeInfo* type = mod->GetObjectTypeByIndex(i);
        if (!type)
        {
            continue;
        }

        ScriptContext ctx = ScriptContext::Shared;
        for (const std::string& meta : builder.GetMetadataForType(type->GetTypeId()))
        {
            // Case-insensitive match against the recognised context tags.
            std::string tag;
            tag.reserve(meta.size());
            for (char c : meta)
            {
                tag.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
            }

            if (tag == "server")
            {
                ctx = ScriptContext::Server;
            }
            else if (tag == "client")
            {
                ctx = ScriptContext::Client;
            }
            else if (tag == "shared")
            {
                ctx = ScriptContext::Shared;
            }
        }

        // Keyed by "module::class" so a recompile (hot-reload) refreshes the
        // record; a class that dropped its tag reverts to the Shared default.
        const std::string key = moduleName + "::" + type->GetName();
        if (ctx == ScriptContext::Shared)
        {
            m_classContexts.erase(key);
        }
        else
        {
            m_classContexts[key] = ctx;
        }
    }
}

// -------------------------------------------------------------------------
// Entity Script Binding
// -------------------------------------------------------------------------

bool AngelScriptEngine::AttachScript(EntityID entity, const std::string& className, const std::string& moduleName)
{
    ASSERT_MSG(!className.empty(), "AngelScriptEngine::AttachScript — className must not be empty");
    ASSERT_MSG(!moduleName.empty(), "AngelScriptEngine::AttachScript — moduleName must not be empty");
    if (!m_engine)
    {
        SetLastError("Engine not initialized.");
        return false;
    }

    // If this entity already has a script attached, detach it first.
    auto existingIt = m_entityScripts.find(entity);
    if (existingIt != m_entityScripts.end())
    {
        CleanupScriptInstance(existingIt->second);
        m_entityScripts.erase(existingIt);
    }

    // Look up the compiled module.
    auto modIt = m_modules.find(moduleName);
    if (modIt == m_modules.end())
    {
        SetLastError("Module '" + moduleName + "' not found.");
        LogError(m_lastError);
        return false;
    }
    asIScriptModule* mod = modIt->second;

    // Find the type info for the requested class.
    asITypeInfo* typeInfo = mod->GetTypeInfoByName(className.c_str());
    if (!typeInfo)
    {
        SetLastError("Class '" + className + "' not found in module '" + moduleName + "'.");
        LogError(m_lastError);
        return false;
    }

    // Enforce the client/server boundary: a class tagged [server]/[client]
    // (recorded at compile time in m_classContexts) must not attach when it
    // conflicts with the engine's current execution context. Shared/untagged
    // classes always attach. This is the multiplayer authority separation the
    // header advertises — without it, client-only logic could run on the
    // dedicated server and vice-versa.
    const ScriptContext classCtx = GetClassContext(moduleName, className);
    if (!IsClassContextAllowed(classCtx))
    {
        const char* classCtxStr = classCtx == ScriptContext::Server ? "[server]" : "[client]";
        SetLastError("Skipped attaching '" + className + "': its " + classCtxStr +
                     " context conflicts with the current script execution context.");
        LogInfo(m_lastError);
        return false;
    }

    // Find factory function (default constructor: "ClassName @ClassName()").
    std::string factoryDecl = className + " @" + className + "()";
    asIScriptFunction* factory = typeInfo->GetFactoryByDecl(factoryDecl.c_str());
    if (!factory)
    {
        SetLastError("No default factory for class '" + className + "'.");
        LogError(m_lastError);
        return false;
    }

    // Create a context and execute the factory.
    asIScriptContext* ctx = m_engine->CreateContext();
    if (!ctx)
    {
        SetLastError("Failed to create context for factory call.");
        LogError(m_lastError);
        return false;
    }

    // The constructor runs script-authored code just like Start()/Update()/
    // OnCollision() below — it must be wired to the same sandbox line
    // callback, or a malicious/runaway ctor runs with zero instruction,
    // time, or memory enforcement (Execute() previously ran with no
    // callback installed here).
    if (m_sandbox)
    {
        m_sandbox->BeginExecution(className + "::<ctor>");
        ctx->SetLineCallback(asFUNCTION(Spark::ScriptSandbox::LineCallback), m_sandbox.get(), asCALL_CDECL);
    }

    ctx->Prepare(factory);
    int execResult = ctx->Execute();

    if (m_sandbox)
    {
        m_sandbox->EndExecution();
    }

    if (execResult != asEXECUTION_FINISHED)
    {
        if (execResult == asEXECUTION_ABORTED && m_sandbox && m_sandbox->WasTerminated())
        {
            SetLastError("Constructor for class '" + className + "' terminated by sandbox.");
        }
        else
        {
            SetLastError("Factory execution failed for class '" + className + "'.");
        }
        LogError(m_lastError);
        ctx->Release();
        return false;
    }

    // Retrieve the created object.
    void* retAddr = ctx->GetAddressOfReturnValue();
    if (!retAddr)
    {
        SetLastError("Factory returned no address for class '" + className + "'.");
        LogError(m_lastError);
        ctx->Release();
        return false;
    }
    asIScriptObject* obj = *static_cast<asIScriptObject**>(retAddr);
    if (!obj)
    {
        SetLastError("Factory returned null for class '" + className + "'.");
        LogError(m_lastError);
        ctx->Release();
        return false;
    }
    obj->AddRef();

    // Build the ScriptInstance record.
    ScriptInstance instance;
    instance.object = obj;
    instance.typeInfo = typeInfo;
    instance.context = ctx;
    instance.className = className;
    instance.moduleName = moduleName;

    CacheScriptMethods(instance);

    m_entityScripts[entity] = instance;
    LogInfo("Attached script '" + className + "' to entity " + std::to_string(static_cast<uint32_t>(entity)) + ".");
    return true;
}

void AngelScriptEngine::DetachScript(EntityID entity)
{
    auto it = m_entityScripts.find(entity);
    if (it != m_entityScripts.end())
    {
        CleanupScriptInstance(it->second);
        m_entityScripts.erase(it);
    }
}

// -------------------------------------------------------------------------
// Lifecycle Callback Dispatch
// -------------------------------------------------------------------------

void AngelScriptEngine::CallStart(EntityID entity)
{
    ScriptInstance* inst = GetScriptInstance(entity);
    if (!inst || !inst->startMethod || !inst->context)
        return;

    if (m_sandbox)
    {
        m_sandbox->BeginExecution(inst->className + "::Start");
        inst->context->SetLineCallback(asFUNCTION(Spark::ScriptSandbox::LineCallback), m_sandbox.get(), asCALL_CDECL);
    }

    inst->context->Prepare(inst->startMethod);
    inst->context->SetObject(inst->object);
    int result = inst->context->Execute();

    if (m_sandbox)
    {
        m_sandbox->EndExecution();
    }

    if (result == asEXECUTION_EXCEPTION)
    {
        SetLastError(std::string("Exception in Start(): ") + inst->context->GetExceptionString());
        LogError(m_lastError);
    }
    else if (result == asEXECUTION_ABORTED && m_sandbox && m_sandbox->WasTerminated())
    {
        SetLastError("Start() terminated by sandbox");
        LogError(m_lastError);
    }
}

void AngelScriptEngine::CallUpdate(EntityID entity, float deltaTime)
{
    ScriptInstance* inst = GetScriptInstance(entity);
    if (!inst || !inst->updateMethod || !inst->context)
        return;

    if (m_sandbox)
    {
        m_sandbox->BeginExecution(inst->className + "::Update");
        inst->context->SetLineCallback(asFUNCTION(Spark::ScriptSandbox::LineCallback), m_sandbox.get(), asCALL_CDECL);
    }

    inst->context->Prepare(inst->updateMethod);
    inst->context->SetObject(inst->object);
    inst->context->SetArgFloat(0, deltaTime);
    int result = inst->context->Execute();

    if (m_sandbox)
    {
        m_sandbox->EndExecution();
    }

    if (result == asEXECUTION_EXCEPTION)
    {
        SetLastError(std::string("Exception in Update(): ") + inst->context->GetExceptionString());
        LogError(m_lastError);
    }
    else if (result == asEXECUTION_ABORTED && m_sandbox && m_sandbox->WasTerminated())
    {
        SetLastError("Update() terminated by sandbox");
        LogError(m_lastError);
    }
}

void AngelScriptEngine::CallOnCollision(EntityID entity, EntityID other)
{
    ScriptInstance* inst = GetScriptInstance(entity);
    if (!inst || !inst->onCollisionMethod || !inst->context)
        return;

    if (m_sandbox)
    {
        m_sandbox->BeginExecution(inst->className + "::OnCollision");
        inst->context->SetLineCallback(asFUNCTION(Spark::ScriptSandbox::LineCallback), m_sandbox.get(), asCALL_CDECL);
    }

    inst->context->Prepare(inst->onCollisionMethod);
    inst->context->SetObject(inst->object);
    inst->context->SetArgDWord(0, static_cast<asDWORD>(other));
    int result = inst->context->Execute();

    if (m_sandbox)
    {
        m_sandbox->EndExecution();
    }

    if (result == asEXECUTION_EXCEPTION)
    {
        SetLastError(std::string("Exception in OnCollision(): ") + inst->context->GetExceptionString());
        LogError(m_lastError);
    }
    else if (result == asEXECUTION_ABORTED && m_sandbox && m_sandbox->WasTerminated())
    {
        SetLastError("OnCollision() terminated by sandbox");
        LogError(m_lastError);
    }
}

// -------------------------------------------------------------------------
// Engine API Registration
// -------------------------------------------------------------------------

void AngelScriptEngine::RegisterStandardLibrary()
{
    RegisterStdString(m_engine);
    RegisterScriptArray(m_engine, true);
}

void AngelScriptEngine::RegisterEngineAPI()
{
    RegisterMathTypes();
    RegisterComponentTypes();
    RegisterGlobalFunctions();
    AutoRegisterReflectedTypes();
}

void AngelScriptEngine::RegisterMathTypes()
{
    // Register a lightweight Vector3 value type for script use.
    m_engine->RegisterObjectType("Vector3", sizeof(DirectX::XMFLOAT3),
                                 asOBJ_VALUE | asOBJ_POD | asGetTypeTraits<DirectX::XMFLOAT3>());
    m_engine->RegisterObjectProperty("Vector3", "float x", asOFFSET(DirectX::XMFLOAT3, x));
    m_engine->RegisterObjectProperty("Vector3", "float y", asOFFSET(DirectX::XMFLOAT3, y));
    m_engine->RegisterObjectProperty("Vector3", "float z", asOFFSET(DirectX::XMFLOAT3, z));
}

void AngelScriptEngine::RegisterComponentTypes()
{
    // Register Transform as a reference type so scripts can manipulate it
    // through the pointer returned by getTransform().
    m_engine->RegisterObjectType("Transform", 0, asOBJ_REF | asOBJ_NOCOUNT);
    m_engine->RegisterObjectProperty("Transform", "Vector3 position", asOFFSET(Transform, position));
    m_engine->RegisterObjectProperty("Transform", "Vector3 rotation", asOFFSET(Transform, rotation));
    m_engine->RegisterObjectProperty("Transform", "Vector3 scale", asOFFSET(Transform, scale));

    // Register EntityID as a simple typedef (uint32).
    m_engine->RegisterTypedef("EntityID", "uint32");
}

bool AngelScriptEngine::RegisterGuardedFunction(const char* declaration, const char* scriptVisibleName,
                                                const asSFuncPtr& fn)
{
    if (m_sandbox && !m_sandbox->IsFunctionAllowed(scriptVisibleName))
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Scripting,
                        "Sandbox: excluding '%s' from script API (not permitted at the current security level).",
                        scriptVisibleName);
        return false;
    }

    int result = m_engine->RegisterGlobalFunction(declaration, fn, asCALL_CDECL);
    if (result < 0)
    {
        LogWarning(std::string("RegisterGlobalFunction failed for '") + scriptVisibleName + "' (decl: " + declaration +
                   "), AngelScript error code " + std::to_string(result));
        return false;
    }
    return true;
}

void AngelScriptEngine::RegisterGlobalFunctions()
{
    RegisterGuardedFunction("void print(const string &in)", "print", asFUNCTION(ASPrint));

    RegisterGuardedFunction("EntityID createEntity(const string &in)", "createEntity", asFUNCTION(ASCreateEntity));

    RegisterGuardedFunction("Transform@ getTransform(EntityID)", "getTransform", asFUNCTION(ASGetTransform));

    RegisterGuardedFunction("bool getKeyDown(const string &in)", "getKeyDown", asFUNCTION(ASGetKeyDown));

    RegisterGuardedFunction("bool getKey(const string &in)", "getKey", asFUNCTION(ASGetKey));

    // Visual Script API — entity manipulation
    RegisterGuardedFunction("void destroyEntity(EntityID)", "destroyEntity", asFUNCTION(ASDestroyEntity));
    RegisterGuardedFunction("Vector3 getPosition(EntityID)", "getPosition", asFUNCTION(ASGetPosition));
    RegisterGuardedFunction("void setPosition(EntityID, const Vector3 &in)", "setPosition", asFUNCTION(ASSetPosition));
    RegisterGuardedFunction("Vector3 getRotation(EntityID)", "getRotation", asFUNCTION(ASGetRotation));
    RegisterGuardedFunction("void setRotation(EntityID, const Vector3 &in)", "setRotation", asFUNCTION(ASSetRotation));
    RegisterGuardedFunction("float getHealth(EntityID)", "getHealth", asFUNCTION(ASGetHealth));
    RegisterGuardedFunction("void setHealth(EntityID, float)", "setHealth", asFUNCTION(ASSetHealth));
    RegisterGuardedFunction("float getSpeed(EntityID)", "getSpeed", asFUNCTION(ASGetSpeed));
    RegisterGuardedFunction("void applyForce(EntityID, const Vector3 &in)", "applyForce", asFUNCTION(ASApplyForce));
    RegisterGuardedFunction("void playSound(EntityID, const string &in)", "playSound", asFUNCTION(ASPlaySound));
    RegisterGuardedFunction("void playAnimation(EntityID, const string &in)", "playAnimation",
                            asFUNCTION(ASPlayAnimation));
    RegisterGuardedFunction("EntityID getEntityByName(const string &in)", "getEntityByName",
                            asFUNCTION(ASGetEntityByName));
    RegisterGuardedFunction("void fireEvent(const string &in)", "fireEvent", asFUNCTION(ASFireEvent));
    RegisterGuardedFunction("void debugTrace(uint, const string &in, const string &in)", "debugTrace",
                            asFUNCTION(ASDebugTrace));
}

// -------------------------------------------------------------------------
// Reflection-driven auto-registration
// -------------------------------------------------------------------------

namespace
{

    // Generic script function: get any reflected field by component type and field name
    std::string ASGetComponentField(uint32_t entityId, const std::string& compType, const std::string& fieldName)
    {
        auto* world = AngelScriptEngine::GetBoundWorld();
        if (!world)
            return "";

        auto& factory = Spark::ComponentFactory::Get();
        void* raw = factory.GetComponentRaw(compType, world, entityId);
        if (!raw)
            return "";

        const auto* typeInfo = Spark::TypeRegistry::Get().FindTypeByName(compType);
        if (!typeInfo)
            return "";

        const auto* field = typeInfo->FindField(fieldName);
        if (!field)
            return "";

        return Spark::GetFieldAsString(raw, *field);
    }

    // Generic script function: set any reflected field by component type and field name
    void ASSetComponentField(uint32_t entityId, const std::string& compType, const std::string& fieldName,
                             const std::string& value)
    {
        auto* world = AngelScriptEngine::GetBoundWorld();
        if (!world)
            return;

        auto& factory = Spark::ComponentFactory::Get();
        void* raw = factory.GetComponentRaw(compType, world, entityId);
        if (!raw)
            return;

        const auto* typeInfo = Spark::TypeRegistry::Get().FindTypeByName(compType);
        if (!typeInfo)
            return;

        const auto* field = typeInfo->FindField(fieldName);
        if (!field)
            return;

        Spark::SetFieldFromString(raw, *field, value);
    }

    // Generic script function: check if entity has a component by type name
    bool ASHasComponent(uint32_t entityId, const std::string& compType)
    {
        auto* world = AngelScriptEngine::GetBoundWorld();
        if (!world)
            return false;
        return Spark::ComponentFactory::Get().HasComponent(compType, world, entityId);
    }

} // anonymous namespace

void AngelScriptEngine::AutoRegisterReflectedTypes()
{
    // Register generic component field access functions.
    // Scripts can read/write ANY reflected field on ANY registered component:
    //   string val = getComponentField(entity, "HealthComponent", "health");
    //   setComponentField(entity, "HealthComponent", "health", "50.0");
    //   bool has = hasComponent(entity, "Transform");
    RegisterGuardedFunction("string getComponentField(EntityID, const string &in, const string &in)",
                            "getComponentField", asFUNCTION(ASGetComponentField));

    RegisterGuardedFunction("void setComponentField(EntityID, const string &in, const string &in, const string &in)",
                            "setComponentField", asFUNCTION(ASSetComponentField));

    RegisterGuardedFunction("bool hasComponent(EntityID, const string &in)", "hasComponent",
                            asFUNCTION(ASHasComponent));

    SPARK_LOG_INFO(
        Spark::LogCategory::Scripting,
        "AngelScript: registered generic getComponentField/setComponentField/hasComponent (reflection-driven, "
        "%zu component types available)",
        Spark::ComponentFactory::Get().GetRegisteredCount());
}

// -------------------------------------------------------------------------
// Internal Helpers
// -------------------------------------------------------------------------

AngelScriptEngine::ScriptInstance* AngelScriptEngine::GetScriptInstance(EntityID entity)
{
    auto it = m_entityScripts.find(entity);
    if (it != m_entityScripts.end())
    {
        return &it->second;
    }
    return nullptr;
}

void AngelScriptEngine::CacheScriptMethods(ScriptInstance& instance)
{
    if (!instance.typeInfo)
        return;

    instance.startMethod = instance.typeInfo->GetMethodByDecl("void Start()");
    instance.updateMethod = instance.typeInfo->GetMethodByDecl("void Update(float)");
    instance.onCollisionMethod = instance.typeInfo->GetMethodByDecl("void OnCollision(EntityID)");
}

void AngelScriptEngine::CleanupScriptInstance(ScriptInstance& instance)
{
    if (instance.object)
    {
        instance.object->Release();
        instance.object = nullptr;
    }
    if (instance.context)
    {
        instance.context->Release();
        instance.context = nullptr;
    }
    instance.typeInfo = nullptr;
    instance.startMethod = nullptr;
    instance.updateMethod = nullptr;
    instance.onCollisionMethod = nullptr;
}

void AngelScriptEngine::MessageCallback(const asSMessageInfo* msg, void* param)
{
    auto* self = static_cast<AngelScriptEngine*>(param);
    std::string prefix;
    switch (msg->type)
    {
    case asMSGTYPE_ERROR:
        prefix = "ERROR";
        break;
    case asMSGTYPE_WARNING:
        prefix = "WARNING";
        break;
    case asMSGTYPE_INFORMATION:
        prefix = "INFO";
        break;
    default:
        prefix = "UNKNOWN";
        break;
    }

    std::ostringstream oss;
    oss << msg->section << " (" << msg->row << ", " << msg->col << ") : " << prefix << " : " << msg->message;

    std::string formatted = oss.str();
    if (msg->type == asMSGTYPE_ERROR)
    {
        self->SetLastError(formatted);
        LogError(formatted);
    }
    else
    {
        LogWarning(formatted);
    }
}

void AngelScriptEngine::SetLastError(const std::string& error)
{
    m_lastError = error;
}

// ============================================================================
// Stub fallbacks when AngelScript is NOT available
// ============================================================================

#else // !SPARK_ANGELSCRIPT_SUPPORT

bool AngelScriptEngine::Initialize()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Scripting);
    SPARK_LOG_INFO(Spark::LogCategory::Scripting, "AngelScriptEngine initializing (stub — no AngelScript support)...");
    LogWarning("AngelScript support is not compiled in (SPARK_ANGELSCRIPT_SUPPORT not defined).");
    s_instance = this;
    return true;
}

void AngelScriptEngine::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Scripting);
    SPARK_LOG_INFO(Spark::LogCategory::Scripting, "AngelScriptEngine shutting down (stub)...");
    LogWarning("AngelScript support is not compiled in. Shutdown is a no-op.");
    if (s_instance == this)
    {
        s_instance = nullptr;
    }
}

bool AngelScriptEngine::CompileScriptFile(const std::string& scriptPath)
{
    LogWarning("Cannot compile '" + scriptPath + "': AngelScript support not compiled in.");
    SetLastError("AngelScript support not available.");
    return false;
}

bool AngelScriptEngine::CompileScriptFromString(const std::string& /*script*/, const std::string& moduleName)
{
    LogWarning("Cannot compile module '" + moduleName + "': AngelScript support not compiled in.");
    SetLastError("AngelScript support not available.");
    return false;
}

bool AngelScriptEngine::AttachScript(EntityID /*entity*/, const std::string& className,
                                     const std::string& /*moduleName*/)
{
    LogWarning("Cannot attach script '" + className + "': AngelScript support not compiled in.");
    SetLastError("AngelScript support not available.");
    return false;
}

void AngelScriptEngine::DetachScript(EntityID /*entity*/)
{
    // No-op without AngelScript.
}

void AngelScriptEngine::CallStart(EntityID /*entity*/)
{
    // No-op without AngelScript.
}

void AngelScriptEngine::CallUpdate(EntityID /*entity*/, float /*deltaTime*/)
{
    // No-op without AngelScript.
}

void AngelScriptEngine::CallOnCollision(EntityID /*entity*/, EntityID /*other*/)
{
    // No-op without AngelScript.
}

void AngelScriptEngine::RegisterStandardLibrary()
{
    // No-op without AngelScript.
}

void AngelScriptEngine::RegisterEngineAPI()
{
    // No-op without AngelScript.
}

void AngelScriptEngine::RegisterMathTypes()
{
    // No-op without AngelScript.
}

void AngelScriptEngine::RegisterComponentTypes()
{
    // No-op without AngelScript.
}

void AngelScriptEngine::RegisterGlobalFunctions()
{
    // No-op without AngelScript.
}

void AngelScriptEngine::AutoRegisterReflectedTypes()
{
    // No-op without AngelScript.
}

bool AngelScriptEngine::HotReloadModule(const std::string& moduleName)
{
    LogWarning("Cannot hot-reload module '" + moduleName + "': AngelScript support not compiled in.");
    SetLastError("AngelScript support not available.");
    return false;
}

std::string AngelScriptEngine::GetModuleFilePath(const std::string& /*moduleName*/) const
{
    return {};
}

std::vector<EntityID> AngelScriptEngine::GetEntitiesForModule(const std::string& /*moduleName*/) const
{
    return {};
}

AngelScriptEngine::ScriptInstance* AngelScriptEngine::GetScriptInstance(EntityID /*entity*/)
{
    return nullptr;
}

void AngelScriptEngine::CacheScriptMethods(ScriptInstance& /*instance*/)
{
    // No-op without AngelScript.
}

void AngelScriptEngine::CleanupScriptInstance(ScriptInstance& /*instance*/)
{
    // No-op without AngelScript.
}

void AngelScriptEngine::MessageCallback(const asSMessageInfo* /*msg*/, void* /*param*/)
{
    // No-op without AngelScript.
}

void AngelScriptEngine::SetLastError(const std::string& error)
{
    m_lastError = error;
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
