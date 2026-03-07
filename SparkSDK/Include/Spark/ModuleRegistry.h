/**
 * @file ModuleRegistry.h
 * @brief Helper macros for implementing and exporting modules
 *
 * Provides convenience macros that generate the boilerplate CreateModule()
 * and DestroyModule() exports for a module class.
 */

#pragma once

#include "IModule.h"
#include "SparkExport.h"

/**
 * @brief Implement the required DLL exports for a module class
 *
 * Usage (in one .cpp file per module DLL):
 * @code
 *   SPARK_IMPLEMENT_MODULE(MyGameModule)
 * @endcode
 *
 * This generates the extern "C" CreateModule/DestroyModule functions
 * that the engine's ModuleManager looks for when loading the DLL.
 */
#define SPARK_IMPLEMENT_MODULE(ModuleClass) \
    extern "C" { \
        SPARK_MODULE_API Spark::IModule* CreateModule() { return new ModuleClass(); } \
        SPARK_MODULE_API void DestroyModule(Spark::IModule* mod) { delete mod; } \
    }
