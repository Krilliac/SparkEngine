/**
 * @file AngelScriptEngine.h
 * @brief AngelScript virtual machine wrapper with hot-reload scripting support
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a high-level C++ wrapper around the AngelScript scripting engine,
 * enabling gameplay logic to be written in AngelScript and attached to ECS
 * entities at runtime. The system supports:
 *
 * - **Script compilation** from files or in-memory strings
 * - **Entity binding** — attach/detach script classes to ECS entities
 * - **Lifecycle callbacks** — Start(), Update(float), OnCollision(EntityID)
 * - **Engine API exposure** — math types, components, input, and utility
 *   functions are registered and callable from script code
 * - **Error reporting** — compilation and runtime errors are captured and
 *   retrievable via GetLastError()
 *
 * ## Typical workflow
 *
 * @code
 *   AngelScriptEngine engine;
 *   engine.Initialize();
 *
 *   // Compile a script file
 *   engine.CompileScriptFile("Assets/Scripts/EnemyAI.as");
 *
 *   // Attach the script class "EnemyBehavior" to an entity
 *   engine.AttachScript(enemyEntity, "EnemyBehavior", "EnemyAI");
 *
 *   // In the game loop:
 *   engine.CallStart(enemyEntity);       // called once
 *   engine.CallUpdate(enemyEntity, dt);  // called every frame
 * @endcode
 *
 * ## AngelScript API available to scripts
 *
 * The following global functions are registered and callable from script code:
 * - `void print(const string& msg)` — output to the debug console
 * - `EntityID createEntity(const string& name)` — create a new ECS entity
 * - `Transform@ getTransform(EntityID id)` — get an entity's transform
 * - `bool getKeyDown(const string& key)` — check for key press (single frame)
 * - `bool getKey(const string& key)` — check if key is currently held
 *
 * @see Components.h (for EntityID and Transform), ECSWorld
 */

#pragma once

#ifdef SPARK_ANGELSCRIPT_SUPPORT
#include <angelscript.h>
#include <scriptstdstring/scriptstdstring.h>
#include <scriptbuilder/scriptbuilder.h>
#include <scriptarray/scriptarray.h>
#else
// Forward-declare / stub AngelScript types so the header compiles without the SDK.
struct asIScriptEngine;
struct asIScriptModule;
struct asIScriptObject;
struct asITypeInfo;
struct asIScriptContext;
struct asIScriptFunction;
struct asSMessageInfo;
#endif // SPARK_ANGELSCRIPT_SUPPORT

#include <unordered_map>
#include <memory>
#include <string>
#include "../ECS/Components.h"
#include "ScriptSandbox.h"

/**
 * @brief High-level AngelScript engine wrapper for gameplay scripting
 *
 * AngelScriptEngine manages the AngelScript virtual machine, script module
 * compilation, entity-script binding, and lifecycle callback dispatch. It
 * acts as the bridge between the C++ engine and AngelScript gameplay code.
 *
 * The engine uses a singleton pattern accessible via GetInstance(). Script
 * instances are tracked per-entity using EntityID keys, and each instance
 * maintains cached function pointers for Start(), Update(), and OnCollision()
 * to avoid repeated lookups.
 *
 * @note Initialize() must be called before any script operations.
 * @warning Script contexts are not thread-safe. All script calls must happen
 *          on the same thread (typically the main game loop thread).
 */
class AngelScriptEngine
{
  public:
    /**
     * @brief Initialize the AngelScript engine and register the engine API
     *
     * Creates the AngelScript engine, registers the standard library (strings,
     * arrays), math types, component types, and global utility functions.
     *
     * @return true if initialization succeeded, false on failure
     */
    bool Initialize();

    /**
     * @brief Shut down the engine and release all script resources
     *
     * Detaches all entity scripts, releases all modules and contexts, and
     * destroys the AngelScript engine instance.
     */
    void Shutdown();

    // ========================================================================
    // Script Compilation
    // ========================================================================

    /**
     * @brief Compile a script from a file on disk
     *
     * Reads and compiles an AngelScript source file (.as). The module name
     * is derived from the filename. If compilation fails, the error is stored
     * and retrievable via GetLastError().
     *
     * @param scriptPath Path to the .as script file
     * @return true if compilation succeeded, false on error
     */
    bool CompileScriptFile(const std::string& scriptPath);

    /**
     * @brief Compile a script from an in-memory string
     *
     * Compiles AngelScript source code provided as a string. Useful for
     * runtime-generated scripts, console input, or testing.
     *
     * @param script     The AngelScript source code string
     * @param moduleName Unique name for the compiled module
     * @return true if compilation succeeded, false on error
     */
    bool CompileScriptFromString(const std::string& script, const std::string& moduleName);

    // ========================================================================
    // Entity Script Binding
    // ========================================================================

    /**
     * @brief Attach a script class instance to an ECS entity
     *
     * Creates an instance of the specified script class from the given module
     * and binds it to the entity. The script's Start(), Update(), and
     * OnCollision() methods (if present) are cached for fast dispatch.
     *
     * @param entity     The ECS entity ID to bind the script to
     * @param className  Name of the script class to instantiate
     * @param moduleName Name of the compiled module containing the class
     * @return true if the script was successfully attached, false on error
     */
    bool AttachScript(EntityID entity, const std::string& className, const std::string& moduleName);

    /**
     * @brief Detach and destroy a script instance from an entity
     *
     * Cleans up the script object, context, and cached method pointers
     * associated with the given entity.
     *
     * @param entity The ECS entity ID to detach the script from
     */
    void DetachScript(EntityID entity);

    // ========================================================================
    // Lifecycle Callback Dispatch
    // ========================================================================

    /**
     * @brief Call the script's Start() method for an entity (called once)
     * @param entity The entity whose script Start() should be invoked
     */
    void CallStart(EntityID entity);

    /**
     * @brief Call the script's Update(float) method for an entity (called every frame)
     * @param entity    The entity whose script Update() should be invoked
     * @param deltaTime Time elapsed since the last frame in seconds
     */
    void CallUpdate(EntityID entity, float deltaTime);

    /**
     * @brief Call the script's OnCollision(EntityID) method for an entity
     * @param entity The entity whose script OnCollision() should be invoked
     * @param other  The entity ID of the other object in the collision
     */
    void CallOnCollision(EntityID entity, EntityID other);

    // ========================================================================
    // Error Handling and Singleton Access
    // ========================================================================

    /**
     * @brief Get the last error message from compilation or runtime
     * @return Error string, or empty string if no error has occurred
     */
    std::string GetLastError() const { return m_lastError; }

    /**
     * @brief Get the script execution sandbox
     * @return Pointer to the ScriptSandbox, or nullptr if not initialized
     */
    Spark::ScriptSandbox* GetSandbox() { return m_sandbox.get(); }

    /**
     * @brief Get the global singleton instance
     * @return Pointer to the AngelScriptEngine instance, or nullptr if not created
     */
    static AngelScriptEngine* GetInstance() { return s_instance; }

  private:
    static AngelScriptEngine* s_instance; ///< Singleton instance pointer
    static World* s_boundWorld;           ///< World pointer bound for script API (createEntity, getTransform)

  public:
    /**
     * @brief Bind an ECS World for script API functions (createEntity, getTransform).
     *
     * Must be called after Initialize() and before any scripts call createEntity()
     * or getTransform(). Typically called once per scene load.
     *
     * @param world Non-owning pointer to the active World. Pass nullptr to unbind.
     */
    static void BindWorld(World* world) { s_boundWorld = world; }

    /**
     * @brief Get the currently bound World pointer.
     * @return Pointer to the bound World, or nullptr if none is bound.
     */
    static World* GetBoundWorld() { return s_boundWorld; }

  private:
    asIScriptEngine* m_engine = nullptr;                         ///< The core AngelScript engine
    std::unordered_map<std::string, asIScriptModule*> m_modules; ///< Compiled script modules by name

    /**
     * @brief Per-entity script instance data
     *
     * Holds the AngelScript object, type info, execution context, and cached
     * method pointers for a single entity's attached script.
     */
    struct ScriptInstance
    {
        asIScriptObject* object = nullptr;              ///< The instantiated script object
        asITypeInfo* typeInfo = nullptr;                ///< Type metadata for the script class
        asIScriptContext* context = nullptr;            ///< Execution context for calling methods
        asIScriptFunction* startMethod = nullptr;       ///< Cached pointer to the Start() method
        asIScriptFunction* updateMethod = nullptr;      ///< Cached pointer to the Update(float) method
        asIScriptFunction* onCollisionMethod = nullptr; ///< Cached pointer to the OnCollision(EntityID) method
        std::string className;                          ///< Name of the script class
        std::string moduleName;                         ///< Name of the module containing the class
    };

    std::unordered_map<EntityID, ScriptInstance> m_entityScripts; ///< Active script instances by entity ID
    std::string m_lastError;                                      ///< Last error message from AS engine
    std::unique_ptr<Spark::ScriptSandbox> m_sandbox;              ///< Script execution sandbox

    // ========================================================================
    // Engine API Registration (called during Initialize)
    // ========================================================================

    /** @brief Register AngelScript standard library (strings, arrays, etc.) */
    void RegisterStandardLibrary();
    /** @brief Register the full Spark Engine API callable from scripts */
    void RegisterEngineAPI();
    /** @brief Register math types (Vector3, Matrix, etc.) */
    void RegisterMathTypes();
    /** @brief Register ECS component types (Transform, etc.) */
    void RegisterComponentTypes();
    /** @brief Register global utility functions (print, createEntity, etc.) */
    void RegisterGlobalFunctions();

    // ========================================================================
    // Internal Helpers
    // ========================================================================

    // ========================================================================
    // Hot-Reload Support
    // ========================================================================

    /**
     * @brief Recompile a module and re-attach all entity scripts that reference it
     *
     * Serializes script state via optional Serialize() callbacks, recompiles the
     * module from disk, and re-attaches all entity scripts from the module.
     * State is restored via Deserialize() if available.
     *
     * @param moduleName Name of the module to reload
     * @return true if recompilation and re-attachment succeeded
     */
    bool HotReloadModule(const std::string& moduleName);

    /**
     * @brief Get the file path associated with a compiled module
     * @param moduleName Module name
     * @return File path, or empty string if not found
     */
    std::string GetModuleFilePath(const std::string& moduleName) const;

    /**
     * @brief Get all entity IDs that have scripts attached from a given module
     * @param moduleName Module name
     * @return Vector of entity IDs
     */
    std::vector<EntityID> GetEntitiesForModule(const std::string& moduleName) const;

    // ========================================================================
    // Script Execution Context (Client/Server)
    // ========================================================================

    /**
     * @brief Script execution context for multiplayer separation
     */
    enum class ScriptContext : uint8_t
    {
        Shared, ///< Runs on both client and server (default)
        Server, ///< Runs only on the dedicated server
        Client  ///< Runs only on the client
    };

    /**
     * @brief Set the current execution context
     *
     * When set to Server, scripts tagged [client] are skipped.
     * When set to Client, scripts tagged [server] are skipped.
     *
     * @param context The execution context
     */
    void SetScriptContext(ScriptContext context) { m_scriptContext = context; }

    /**
     * @brief Get the current execution context
     */
    ScriptContext GetScriptContext() const { return m_scriptContext; }

  private:
    ScriptContext m_scriptContext = ScriptContext::Shared;

    /// Maps module name -> source file path for hot-reload
    std::unordered_map<std::string, std::string> m_moduleFilePaths;

    /**
     * @brief Look up a script instance by entity ID
     * @param entity The entity to look up
     * @return Pointer to the ScriptInstance, or nullptr if not found
     */
    ScriptInstance* GetScriptInstance(EntityID entity);

    /**
     * @brief Cache method pointers (Start, Update, OnCollision) for a script instance
     * @param instance The script instance to cache methods for
     */
    void CacheScriptMethods(ScriptInstance& instance);

    /**
     * @brief Release all resources held by a script instance
     * @param instance The script instance to clean up
     */
    void CleanupScriptInstance(ScriptInstance& instance);

    /**
     * @brief AngelScript message callback for compilation/runtime errors
     * @param msg  Message info from the AS engine
     * @param param User parameter (pointer to this engine instance)
     */
    static void MessageCallback(const asSMessageInfo* msg, void* param);

    /**
     * @brief Store an error message for retrieval via GetLastError()
     * @param error The error message string
     */
    void SetLastError(const std::string& error);
};

// ============================================================================
// Global Functions Callable from AngelScript
// ============================================================================

/**
 * @brief Print a message to the debug console (callable from AngelScript as `print()`)
 * @param message The message to print
 */
void ASPrint(const std::string& message);

/**
 * @brief Create a new ECS entity (callable from AngelScript as `createEntity()`)
 * @param name Human-readable name for the entity
 * @return The EntityID of the newly created entity
 */
EntityID ASCreateEntity(const std::string& name);

/**
 * @brief Get an entity's Transform component (callable from AngelScript as `getTransform()`)
 * @param entity The entity to query
 * @return Pointer to the entity's Transform, or nullptr if not found
 */
Transform* ASGetTransform(EntityID entity);

/**
 * @brief Check if a key was pressed this frame (callable from AngelScript as `getKeyDown()`)
 * @param key Key name string (e.g., "W", "Space", "LeftShift")
 * @return true if the key was pressed this frame
 */
bool ASGetKeyDown(const std::string& key);

/**
 * @brief Check if a key is currently held down (callable from AngelScript as `getKey()`)
 * @param key Key name string (e.g., "W", "Space", "LeftShift")
 * @return true if the key is currently held
 */
bool ASGetKey(const std::string& key);

// ============================================================================
// Visual Script API — Entity manipulation functions for generated scripts
// ============================================================================

/** @brief Destroy an ECS entity (callable from AngelScript as `destroyEntity()`) */
void ASDestroyEntity(EntityID entity);

/** @brief Get entity position as Vector3 (callable as `getPosition()`) */
DirectX::XMFLOAT3 ASGetPosition(EntityID entity);

/** @brief Set entity position (callable as `setPosition()`) */
void ASSetPosition(EntityID entity, const DirectX::XMFLOAT3& pos);

/** @brief Get entity rotation as euler angles (callable as `getRotation()`) */
DirectX::XMFLOAT3 ASGetRotation(EntityID entity);

/** @brief Set entity rotation from euler angles (callable as `setRotation()`) */
void ASSetRotation(EntityID entity, const DirectX::XMFLOAT3& rot);

/** @brief Get entity health value (callable as `getHealth()`) */
float ASGetHealth(EntityID entity);

/** @brief Set entity health value (callable as `setHealth()`) */
void ASSetHealth(EntityID entity, float health);

/** @brief Get entity movement speed (callable as `getSpeed()`) */
float ASGetSpeed(EntityID entity);

/** @brief Apply a physics force to an entity (callable as `applyForce()`) */
void ASApplyForce(EntityID entity, const DirectX::XMFLOAT3& force);

/** @brief Play a sound effect on an entity (callable as `playSound()`) */
void ASPlaySound(EntityID entity, const std::string& soundName);

/** @brief Play an animation on an entity (callable as `playAnimation()`) */
void ASPlayAnimation(EntityID entity, const std::string& animName);

/** @brief Find an entity by name (callable as `getEntityByName()`) */
EntityID ASGetEntityByName(const std::string& name);

/** @brief Fire a named event (callable as `fireEvent()`) */
void ASFireEvent(const std::string& eventName);

/** @brief Print a debug trace message (callable as `debugTrace()`) */
void ASDebugTrace(uint32_t nodeId, const std::string& nodeName, const std::string& output);

/** @brief Callback type for debug trace messages (editor hooks into this) */
using DebugTraceCallback = void (*)(uint32_t nodeId, const char* nodeName, const char* output);

/** @brief Set a callback to receive debug trace messages from running scripts */
void ASSetDebugTraceCallback(DebugTraceCallback callback);
