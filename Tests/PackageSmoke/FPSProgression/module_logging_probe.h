#pragma once

#include <Spark/IEngineContext.h>

#if defined(_WIN32)
#if defined(SPARK_MODULE_LOG_PROBE_BUILD_A)
#define SPARK_MODULE_LOG_PROBE_A_API __declspec(dllexport)
#define SPARK_MODULE_LOG_PROBE_B_API __declspec(dllimport)
#elif defined(SPARK_MODULE_LOG_PROBE_BUILD_B)
#define SPARK_MODULE_LOG_PROBE_A_API __declspec(dllimport)
#define SPARK_MODULE_LOG_PROBE_B_API __declspec(dllexport)
#else
#define SPARK_MODULE_LOG_PROBE_A_API __declspec(dllimport)
#define SPARK_MODULE_LOG_PROBE_B_API __declspec(dllimport)
#endif
#else
#define SPARK_MODULE_LOG_PROBE_A_API __attribute__((visibility("default")))
#define SPARK_MODULE_LOG_PROBE_B_API __attribute__((visibility("default")))
#endif

extern "C" SPARK_MODULE_LOG_PROBE_A_API void ModuleLogProbeBindA(Spark::IEngineContext* context);
extern "C" SPARK_MODULE_LOG_PROBE_A_API bool ModuleLogProbeObserveA(Spark::IEngineContext* expected);
extern "C" SPARK_MODULE_LOG_PROBE_B_API void ModuleLogProbeBindB(Spark::IEngineContext* context);
extern "C" SPARK_MODULE_LOG_PROBE_B_API bool ModuleLogProbeObserveB(Spark::IEngineContext* expected);
