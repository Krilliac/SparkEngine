/**
 * @file ModuleRegistry.h
 * @brief Helper macros for implementing and exporting modules
 *
 * Provides convenience macros that generate the boilerplate CreateModule()
 * and DestroyModule() exports for a module class.
 */

#pragma once

#include "IModule.h"
#include "ModuleABI.h"
#include "SparkExport.h"

/**
 * @brief Implement the required DLL exports for a module class
 *
 * Usage (in one .cpp file per module DLL):
 * @code
 *   SPARK_IMPLEMENT_MODULE(MyGameModule)
 * @endcode
 *
 * This generates the mandatory pre-instantiation compatibility descriptor
 * plus the extern "C" CreateModule/DestroyModule functions that the engine's
 * ModuleManager looks for when loading the DLL.
 */
#define SPARK_IMPLEMENT_MODULE(ModuleClass)                                                                            \
    SPARK_EXPORT_MODULE_COMPATIBILITY()                                                                                \
    extern "C"                                                                                                         \
    {                                                                                                                  \
        SPARK_MODULE_API Spark::IModule* CreateModule()                                                                \
        {                                                                                                              \
            return new ModuleClass();                                                                                  \
        }                                                                                                              \
        SPARK_MODULE_API void DestroyModule(Spark::IModule* mod)                                                       \
        {                                                                                                              \
            delete mod;                                                                                                \
        }                                                                                                              \
    }

/**
 * @brief Declare module dependencies (ezEngine-inspired pattern)
 *
 * Place in your module's GetModuleInfo() to declare that this module
 * depends on other modules being loaded first. ModuleManager resolves
 * load order via topological sort using these declarations.
 *
 * Usage:
 * @code
 *   Spark::ModuleInfo GetModuleInfo() const override {
 *       Spark::ModuleInfo info;
 *       info.name = "CombatModule";
 *       SPARK_MODULE_DEPENDENCIES(info, "CoreGameplay", "WeaponSystem");
 *       return info;
 *   }
 * @endcode
 */
#define SPARK_MODULE_DEPENDENCIES(infoVar, ...)                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        static const char* const _sparkDeps[] = {__VA_ARGS__};                                                         \
        (infoVar).dependencies = _sparkDeps;                                                                           \
        (infoVar).dependencyCount = static_cast<int>(sizeof(_sparkDeps) / sizeof(_sparkDeps[0]));                      \
    } while (0)
