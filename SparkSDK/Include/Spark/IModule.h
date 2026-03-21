/**
 * @file IModule.h
 * @brief Module interface for dynamically loaded game/gameplay modules
 *
 * IModule is the evolution of IGameModule. It provides a richer interface
 * with module metadata (name, version, SDK version, load order) and
 * receives an IEngineContext instead of individual system pointers.
 *
 * The engine's ModuleManager loads one or more module DLLs, each exporting
 * CreateModule()/DestroyModule(). Modules are initialized in load-order
 * and shut down in reverse order.
 *
 * Existing game DLLs using the legacy IGameModule/CreateGameModule exports
 * continue to work through a compatibility adapter in ModuleManager.
 *
 * ## String ownership
 * All `const char*` returns from GetModuleInfo() (name, version) must point
 * to memory owned by the module. The engine copies these strings during
 * module registration — the module must keep them valid until OnUnload().
 * Using string literals is the simplest approach.
 */

#pragma once

#include "IEngineContext.h"
#include "Version.h"
#include <cstdint>

namespace Spark
{

    /**
     * @brief Metadata describing a loaded module
     *
     * All const char* fields must point to storage owned by the module.
     * String literals are the recommended approach. The engine copies
     * these values during registration.
     */
    struct ModuleInfo
    {
        const char* name = "Unnamed";            ///< Module display name (module-owned)
        const char* version = "1.0.0";           ///< Module version string (module-owned)
        uint32_t sdkVersion = SPARK_SDK_VERSION; ///< SDK version this module was built against
        int loadOrder = 1000;                    ///< Lower values load first (default 1000)

        /// Module names this module depends on (loaded/initialized first).
        /// Populated automatically by SPARK_MODULE_DEPENDENCY() or manually.
        const char* const* dependencies = nullptr;
        int dependencyCount = 0;
    };

    /**
     * @brief Interface that dynamically loaded modules implement
     *
     * Each module DLL exports a CreateModule() function returning an IModule*.
     * The engine calls the lifecycle methods in this order:
     *   1. OnLoad()       — module receives the engine context
     *   2. OnUpdate()     — called every frame (variable timestep)
     *   3. OnFixedUpdate() — called at fixed intervals for deterministic logic
     *   4. OnRender()     — called every frame after update
     *   5. OnUnload()     — called before the DLL is unloaded
     *
     * Optional hooks (OnPause, OnResume, OnImGui, OnResize) are called when
     * the corresponding engine events occur.
     */
    class IModule
    {
      public:
        virtual ~IModule() = default;

        /** @brief Return metadata about this module */
        virtual ModuleInfo GetModuleInfo() const = 0;

        /**
         * @brief Called after the DLL is loaded
         * @param context Engine service locator — store this pointer for later use
         * @return true on success, false to abort loading this module
         */
        virtual bool OnLoad(IEngineContext* context) = 0;

        /** @brief Called before the DLL is unloaded. Release all resources. */
        virtual void OnUnload() = 0;

        /**
         * @brief Called every frame to update module state
         * @param deltaTime Seconds since last frame (variable timestep)
         */
        virtual void OnUpdate(float deltaTime) = 0;

        /**
         * @brief Called at a fixed timestep for deterministic simulation
         * @param fixedDeltaTime Fixed timestep interval in seconds (typically 1/60)
         *
         * Use this for physics-dependent logic, movement, and anything that
         * needs frame-rate-independent behavior. Optional — default is no-op.
         */
        virtual void OnFixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

        /** @brief Called every frame after OnUpdate to render. Optional. */
        virtual void OnRender() {}

        /** @brief Called when the window is resized. Optional. */
        virtual void OnResize(int width, int height)
        {
            (void)width;
            (void)height;
        }

        /**
         * @brief Called when the game is paused. Optional.
         *
         * Modules should suspend gameplay timers, AI, and audio here.
         */
        virtual void OnPause() {}

        /**
         * @brief Called when the game is resumed after a pause. Optional.
         */
        virtual void OnResume() {}

        /**
         * @brief Called during the ImGui render pass for debug UI. Optional.
         *
         * Use this to draw debug windows, overlays, and development tools.
         * Only called when the editor/debug UI is active.
         */
        virtual void OnImGui() {}
    };

} // namespace Spark

/**
 * @brief Function signatures for module factory exports
 *
 * Every module DLL must export these two functions:
 *   extern "C" SPARK_MODULE_API Spark::IModule* CreateModule();
 *   extern "C" SPARK_MODULE_API void DestroyModule(Spark::IModule*);
 */
using CreateModuleFn = Spark::IModule* (*)();
using DestroyModuleFn = void (*)(Spark::IModule*);
