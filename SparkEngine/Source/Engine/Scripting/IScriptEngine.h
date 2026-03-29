/**
 * @file IScriptEngine.h
 * @brief Abstract interface for scripting engine backends
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines the interface that scripting implementations (AngelScript, Lua, etc.)
 * must implement. This abstraction enables multiple scripting backends to be
 * used interchangeably through EngineContext.
 */

#pragma once

#include "../ECS/Components.h"

#include <string>

namespace Spark::Scripting
{

    /**
     * @brief Abstract interface for scripting engine backends.
     *
     * All script operations go through this interface. Implementations
     * manage their own VM, modules, and per-entity script instances.
     */
    class IScriptEngine
    {
      public:
        virtual ~IScriptEngine() = default;

        // ================================================================
        // Lifecycle
        // ================================================================

        /// @brief Initialize the scripting engine and register the engine API.
        /// @return true on success.
        virtual bool Initialize() = 0;

        /// @brief Shut down and release all script resources.
        virtual void Shutdown() = 0;

        // ================================================================
        // Compilation
        // ================================================================

        /// @brief Compile a script from a file on disk.
        /// @param scriptPath Path to the script file.
        /// @return true if compilation succeeded.
        virtual bool CompileScriptFile(const std::string& scriptPath) = 0;

        /// @brief Compile a script from an in-memory string.
        /// @param script Source code string.
        /// @param moduleName Unique name for the compiled module.
        /// @return true if compilation succeeded.
        virtual bool CompileScriptFromString(const std::string& script, const std::string& moduleName) = 0;

        // ================================================================
        // Entity Binding
        // ================================================================

        /// @brief Attach a script class instance to an ECS entity.
        /// @param entity Entity ID to bind to.
        /// @param className Name of the script class.
        /// @param moduleName Name of the compiled module.
        /// @return true on success.
        virtual bool AttachScript(EntityID entity, const std::string& className, const std::string& moduleName) = 0;

        /// @brief Detach and destroy a script instance from an entity.
        virtual void DetachScript(EntityID entity) = 0;

        // ================================================================
        // Lifecycle Callbacks
        // ================================================================

        /// @brief Call the script's Start() method for an entity.
        virtual void CallStart(EntityID entity) = 0;

        /// @brief Call the script's Update(float) method for an entity.
        virtual void CallUpdate(EntityID entity, float deltaTime) = 0;

        /// @brief Call the script's OnCollision(EntityID) method.
        virtual void CallOnCollision(EntityID entity, EntityID other) = 0;

        // ================================================================
        // Query
        // ================================================================

        /// @brief Get the last error message.
        virtual std::string GetLastError() const = 0;

        /// @brief Get the backend name (e.g., "AngelScript", "Lua").
        virtual const char* GetBackendName() const = 0;
    };

} // namespace Spark::Scripting
