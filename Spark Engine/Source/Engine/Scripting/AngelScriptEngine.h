#pragma once
#include <angelscript.h>
#include <scriptstdstring/scriptstdstring.h>
#include <scriptbuilder/scriptbuilder.h>
#include <scriptarray/scriptarray.h>
#include <unordered_map>
#include <string>
#include "../ECS/Components.h"

class AngelScriptEngine {
public:
    bool Initialize();
    void Shutdown();
    
    bool CompileScriptFile(const std::string& scriptPath);
    bool CompileScriptFromString(const std::string& script, const std::string& moduleName);
    
    bool AttachScript(EntityID entity, const std::string& className, const std::string& moduleName);
    void DetachScript(EntityID entity);
    
    void CallStart(EntityID entity);
    void CallUpdate(EntityID entity, float deltaTime);
    void CallOnCollision(EntityID entity, EntityID other);
    
    std::string GetLastError() const { return m_lastError; }
    
    static AngelScriptEngine* GetInstance() { return s_instance; }
    
private:
    static AngelScriptEngine* s_instance;
    
    asIScriptEngine* m_engine = nullptr;
    std::unordered_map<std::string, asIScriptModule*> m_modules;
    
    struct ScriptInstance {
        asIScriptObject* object = nullptr;
        asITypeInfo* typeInfo = nullptr;
        asIScriptContext* context = nullptr;
        asIScriptFunction* startMethod = nullptr;
        asIScriptFunction* updateMethod = nullptr;
        asIScriptFunction* onCollisionMethod = nullptr;
        std::string className;
        std::string moduleName;
    };
    
    std::unordered_map<EntityID, ScriptInstance> m_entityScripts;
    std::string m_lastError;
    
    void RegisterStandardLibrary();
    void RegisterEngineAPI();
    void RegisterMathTypes();
    void RegisterComponentTypes();
    void RegisterGlobalFunctions();
    
    ScriptInstance* GetScriptInstance(EntityID entity);
    void CacheScriptMethods(ScriptInstance& instance);
    void CleanupScriptInstance(ScriptInstance& instance);
    
    static void MessageCallback(const asSMessageInfo* msg, void* param);
    void SetLastError(const std::string& error);
};

// Global functions callable from AngelScript
void ASPrint(const std::string& message);
EntityID ASCreateEntity(const std::string& name);
Transform* ASGetTransform(EntityID entity);
bool ASGetKeyDown(const std::string& key);
bool ASGetKey(const std::string& key);

