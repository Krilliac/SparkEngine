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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

namespace fs = std::filesystem;

// ============================================================================
// Static singleton pointer
// ============================================================================

AngelScriptEngine* AngelScriptEngine::s_instance = nullptr;

// ============================================================================
// Helper: log a warning to stderr (used by stubs and real paths alike)
// ============================================================================

static void LogWarning(const std::string& message)
{
    std::cerr << "[AngelScriptEngine] WARNING: " << message << "\n";
}

static void LogError(const std::string& message)
{
    std::cerr << "[AngelScriptEngine] ERROR: " << message << "\n";
}

static void LogInfo(const std::string& message)
{
    std::cout << "[AngelScriptEngine] " << message << "\n";
}

// ============================================================================
// Global functions callable from AngelScript
// ============================================================================

void ASPrint(const std::string& message)
{
    std::cout << "[Script] " << message << "\n";
}

EntityID ASCreateEntity([[maybe_unused]] const std::string& name)
{
    // Requires a World pointer — in a real integration the engine context
    // would supply it.  For now return null entity.
    LogWarning("ASCreateEntity called but no World is bound to the scripting engine.");
    return entt::null;
}

Transform* ASGetTransform([[maybe_unused]] EntityID entity)
{
    LogWarning("ASGetTransform called but no World is bound to the scripting engine.");
    return nullptr;
}

bool ASGetKeyDown([[maybe_unused]] const std::string& key)
{
    // Requires InputManager binding — stub for now.
    return false;
}

bool ASGetKey([[maybe_unused]] const std::string& key)
{
    return false;
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

    LogInfo("Compiled script file: " + scriptPath + " -> module '" + moduleName + "'.");
    return true;
}

bool AngelScriptEngine::CompileScriptFromString(const std::string& script, const std::string& moduleName)
{
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
    asIScriptObject* obj = *static_cast<asIScriptObject**>(ctx->GetAddressOfReturnValue());
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
    if (!inst || !inst->startMethod)
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
    if (!inst || !inst->updateMethod)
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
    if (!inst || !inst->onCollisionMethod)
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
    LogWarning("AngelScript support is not compiled in (SPARK_ANGELSCRIPT_SUPPORT not defined).");
    s_instance = this;
    return true;
}

void AngelScriptEngine::Shutdown()
{
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
