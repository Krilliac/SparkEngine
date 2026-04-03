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

void ASDebugTrace(uint32_t nodeId, const std::string& nodeName, const std::string& output)
{
    SPARK_LOG_INFO(Spark::LogCategory::Scripting, "[Trace] Node %u (%s): %s", nodeId, nodeName.c_str(), output.c_str());
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
    RegisterEngineAPI();

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
        return false;
    }

    result = builder.AddSectionFromFile(scriptPath.c_str());
    if (result < 0)
    {
        SetLastError("Failed to add script section from file '" + scriptPath + "'.");
        LogError(m_lastError);
        return false;
    }

    result = builder.BuildModule();
    if (result < 0)
    {
        SetLastError("Compilation failed for module '" + moduleName + "'.");
        LogError(m_lastError);
        return false;
    }

    asIScriptModule* mod = m_engine->GetModule(moduleName.c_str());
    if (mod)
    {
        m_modules[moduleName] = mod;
    }

    m_moduleFilePaths[moduleName] = scriptPath;

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

    // 2. Detach all scripts from this module
    for (const auto& binding : bindings)
    {
        DetachScript(binding.entity);
    }

    // 3. Recompile the module from disk
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
        return false;
    }

    result = builder.AddSectionFromMemory("inline", script.c_str(), static_cast<unsigned int>(script.size()));
    if (result < 0)
    {
        SetLastError("Failed to add inline script section for module '" + moduleName + "'.");
        LogError(m_lastError);
        return false;
    }

    result = builder.BuildModule();
    if (result < 0)
    {
        SetLastError("Compilation failed for module '" + moduleName + "'.");
        LogError(m_lastError);
        return false;
    }

    asIScriptModule* mod = m_engine->GetModule(moduleName.c_str());
    if (mod)
    {
        m_modules[moduleName] = mod;
    }

    LogInfo("Compiled inline script -> module '" + moduleName + "'.");
    return true;
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

    ctx->Prepare(factory);
    int execResult = ctx->Execute();
    if (execResult != asEXECUTION_FINISHED)
    {
        SetLastError("Factory execution failed for class '" + className + "'.");
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

    inst->context->Prepare(inst->startMethod);
    inst->context->SetObject(inst->object);
    int result = inst->context->Execute();
    if (result == asEXECUTION_EXCEPTION)
    {
        SetLastError(std::string("Exception in Start(): ") + inst->context->GetExceptionString());
        LogError(m_lastError);
    }
}

void AngelScriptEngine::CallUpdate(EntityID entity, float deltaTime)
{
    ScriptInstance* inst = GetScriptInstance(entity);
    if (!inst || !inst->updateMethod || !inst->context)
        return;

    inst->context->Prepare(inst->updateMethod);
    inst->context->SetObject(inst->object);
    inst->context->SetArgFloat(0, deltaTime);
    int result = inst->context->Execute();
    if (result == asEXECUTION_EXCEPTION)
    {
        SetLastError(std::string("Exception in Update(): ") + inst->context->GetExceptionString());
        LogError(m_lastError);
    }
}

void AngelScriptEngine::CallOnCollision(EntityID entity, EntityID other)
{
    ScriptInstance* inst = GetScriptInstance(entity);
    if (!inst || !inst->onCollisionMethod || !inst->context)
        return;

    inst->context->Prepare(inst->onCollisionMethod);
    inst->context->SetObject(inst->object);
    inst->context->SetArgDWord(0, static_cast<asDWORD>(other));
    int result = inst->context->Execute();
    if (result == asEXECUTION_EXCEPTION)
    {
        SetLastError(std::string("Exception in OnCollision(): ") + inst->context->GetExceptionString());
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

void AngelScriptEngine::RegisterGlobalFunctions()
{
    m_engine->RegisterGlobalFunction("void print(const string &in)", asFUNCTION(ASPrint), asCALL_CDECL);

    m_engine->RegisterGlobalFunction("EntityID createEntity(const string &in)", asFUNCTION(ASCreateEntity),
                                     asCALL_CDECL);

    m_engine->RegisterGlobalFunction("Transform@ getTransform(EntityID)", asFUNCTION(ASGetTransform), asCALL_CDECL);

    m_engine->RegisterGlobalFunction("bool getKeyDown(const string &in)", asFUNCTION(ASGetKeyDown), asCALL_CDECL);

    m_engine->RegisterGlobalFunction("bool getKey(const string &in)", asFUNCTION(ASGetKey), asCALL_CDECL);

    // Visual Script API — entity manipulation
    m_engine->RegisterGlobalFunction("void destroyEntity(EntityID)", asFUNCTION(ASDestroyEntity), asCALL_CDECL);
    m_engine->RegisterGlobalFunction("Vector3 getPosition(EntityID)", asFUNCTION(ASGetPosition), asCALL_CDECL);
    m_engine->RegisterGlobalFunction("void setPosition(EntityID, const Vector3 &in)", asFUNCTION(ASSetPosition),
                                     asCALL_CDECL);
    m_engine->RegisterGlobalFunction("Vector3 getRotation(EntityID)", asFUNCTION(ASGetRotation), asCALL_CDECL);
    m_engine->RegisterGlobalFunction("void setRotation(EntityID, const Vector3 &in)", asFUNCTION(ASSetRotation),
                                     asCALL_CDECL);
    m_engine->RegisterGlobalFunction("float getHealth(EntityID)", asFUNCTION(ASGetHealth), asCALL_CDECL);
    m_engine->RegisterGlobalFunction("void setHealth(EntityID, float)", asFUNCTION(ASSetHealth), asCALL_CDECL);
    m_engine->RegisterGlobalFunction("float getSpeed(EntityID)", asFUNCTION(ASGetSpeed), asCALL_CDECL);
    m_engine->RegisterGlobalFunction("void applyForce(EntityID, const Vector3 &in)", asFUNCTION(ASApplyForce),
                                     asCALL_CDECL);
    m_engine->RegisterGlobalFunction("void playSound(EntityID, const string &in)", asFUNCTION(ASPlaySound),
                                     asCALL_CDECL);
    m_engine->RegisterGlobalFunction("void playAnimation(EntityID, const string &in)", asFUNCTION(ASPlayAnimation),
                                     asCALL_CDECL);
    m_engine->RegisterGlobalFunction("EntityID getEntityByName(const string &in)", asFUNCTION(ASGetEntityByName),
                                     asCALL_CDECL);
    m_engine->RegisterGlobalFunction("void fireEvent(const string &in)", asFUNCTION(ASFireEvent), asCALL_CDECL);
    m_engine->RegisterGlobalFunction("void debugTrace(uint, const string &in, const string &in)",
                                     asFUNCTION(ASDebugTrace), asCALL_CDECL);
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
