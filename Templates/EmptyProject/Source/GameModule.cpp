/**
 * @file GameModule.cpp
 * @brief EmptyProject module DLL exports
 *
 * This file uses SPARK_IMPLEMENT_MODULE to generate the CreateModule()
 * and DestroyModule() exports that the engine needs to load this DLL.
 */

#include "GameModule.h"
#include <Spark/ModuleDllMain.h>

// Generate the DLL factory exports for this module
SPARK_IMPLEMENT_MODULE(EmptyProjectModule)
