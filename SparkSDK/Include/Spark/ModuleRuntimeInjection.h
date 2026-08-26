/**
 * @file ModuleRuntimeInjection.h
 * @brief Public declaration for the module-local runtime injection bridge.
 *
 * ModuleDllMain.h calls this bridge from the module DLL. Its implementation is
 * provided by SparkEngineLib so public SDK headers do not depend on private
 * EngineContext or PhysicsSystem definitions.
 */

#pragma once

namespace Spark::Detail
{
    /**
     * @brief Bind the host context and initialize runtime state for this module image.
     *
     * @param hostEngineContext Non-owning pointer supplied by SparkEngine.
     */
    void InjectModuleEngineRuntime(void* hostEngineContext);
} // namespace Spark::Detail
