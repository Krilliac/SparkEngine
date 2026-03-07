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
 */

#pragma once

#include "IEngineContext.h"
#include "Version.h"
#include <cstdint>

namespace Spark {

/**
 * @brief Metadata describing a loaded module
 */
struct ModuleInfo
{
    const char* name    = "Unnamed";   ///< Module display name
    const char* version = "1.0.0";     ///< Module version string
    uint32_t sdkVersion = SPARK_SDK_VERSION; ///< SDK version this module was built against
    int      loadOrder  = 1000;        ///< Lower values load first (default 1000)
};

/**
 * @brief Interface that dynamically loaded modules implement
 *
 * Each module DLL exports a CreateModule() function returning an IModule*.
 * The engine calls the lifecycle methods in this order:
 *   1. OnLoad()   — module receives the engine context
 *   2. OnUpdate() — called every frame
 *   3. OnRender() — called every frame after update
 *   4. OnUnload() — called before the DLL is unloaded
 */
class IModule
{
public:
    virtual ~IModule() = default;

    /** @brief Return metadata about this module */
    virtual ModuleInfo GetModuleInfo() const = 0;

    /**
     * @brief Called after the DLL is loaded
     * @param context Engine service locator — store this pointer
     * @return true on success, false to abort loading this module
     */
    virtual bool OnLoad(IEngineContext* context) = 0;

    /** @brief Called before the DLL is unloaded. Release all resources. */
    virtual void OnUnload() = 0;

    /**
     * @brief Called every frame to update module state
     * @param deltaTime Seconds since last frame
     */
    virtual void OnUpdate(float deltaTime) = 0;

    /** @brief Called every frame after OnUpdate to render. Optional. */
    virtual void OnRender() {}

    /** @brief Called when the window is resized. Optional. */
    virtual void OnResize(int width, int height) { (void)width; (void)height; }
};

} // namespace Spark

/**
 * @brief Function signatures for module factory exports
 *
 * Every module DLL must export these two functions:
 *   extern "C" SPARK_MODULE_API Spark::IModule* CreateModule();
 *   extern "C" SPARK_MODULE_API void DestroyModule(Spark::IModule*);
 */
using CreateModuleFn  = Spark::IModule* (*)();
using DestroyModuleFn = void (*)(Spark::IModule*);
