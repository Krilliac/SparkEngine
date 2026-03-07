/**
 * @file SparkExport.h
 * @brief DLL export/import macros for the Spark Engine module system
 *
 * Defines platform-specific export/import macros used by game modules
 * and the engine SDK. Game DLLs use SPARK_MODULE_API (or the legacy
 * SPARK_GAME_API) to export their factory functions.
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
 * @brief Use SPARK_MODULE_API on functions exported from module DLLs
 *
 * In module DLL projects, define SPARK_MODULE_DLL before including this header.
 * This ensures CreateModule and DestroyModule are exported.
 */
#ifdef SPARK_MODULE_DLL
    #define SPARK_MODULE_API SPARK_EXPORT
#else
    #define SPARK_MODULE_API SPARK_IMPORT
#endif

/**
 * @brief Legacy macro — use SPARK_MODULE_API for new code
 *
 * Kept for backward compatibility with existing game DLLs that define
 * SPARK_GAME_DLL and use SPARK_GAME_API.
 */
#ifdef SPARK_GAME_DLL
    #define SPARK_GAME_API SPARK_EXPORT
#else
    #define SPARK_GAME_API SPARK_IMPORT
#endif
