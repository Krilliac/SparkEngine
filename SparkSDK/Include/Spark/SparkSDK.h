/**
 * @file SparkSDK.h
 * @brief Master include for the Spark Engine SDK
 *
 * Game modules can include this single header to get the full SDK surface:
 *   - IModule interface and ModuleInfo struct
 *   - IEngineContext service locator
 *   - SPARK_IMPLEMENT_MODULE macro
 *   - Version and compatibility utilities
 *   - Export macros
 */

#pragma once

#include "Version.h"
#include "SparkExport.h"
#include "IEngineContext.h"
#include "IModule.h"
#include "ModuleRegistry.h"
