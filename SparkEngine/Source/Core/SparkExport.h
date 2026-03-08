/**
 * @file SparkExport.h
 * @brief DLL export/import macros for the Spark Engine module system
 *
 * This is the internal engine copy. For game modules, prefer including
 * <Spark/SparkExport.h> from the SparkSDK instead.
 *
 * Game DLLs use SPARK_MODULE_API (new) or SPARK_GAME_API (legacy) to
 * export their factory functions.
 */

#pragma once

#ifdef _WIN32
#define SPARK_EXPORT __declspec(dllexport)
#define SPARK_IMPORT __declspec(dllimport)
#else
#define SPARK_EXPORT __attribute__((visibility("default")))
#define SPARK_IMPORT
#endif

/**
 * @brief Use SPARK_MODULE_API on functions exported from module DLLs (new API)
 */
#ifdef SPARK_MODULE_DLL
#define SPARK_MODULE_API SPARK_EXPORT
#else
#define SPARK_MODULE_API SPARK_IMPORT
#endif

/**
 * @brief Use SPARK_GAME_API on functions exported from game DLLs (legacy API)
 */
#ifdef SPARK_GAME_DLL
#define SPARK_GAME_API SPARK_EXPORT
#else
#define SPARK_GAME_API SPARK_IMPORT
#endif
