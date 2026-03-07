/**
 * @file SparkExport.h
 * @brief DLL export/import macros for the Spark Engine module system
 *
 * Game DLLs use SPARK_GAME_API to export their CreateGameModule/DestroyGameModule
 * functions. The engine uses these macros when it needs to import symbols.
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
 * @brief Use SPARK_GAME_API on functions exported from game DLLs
 *
 * In game DLL projects, define SPARK_GAME_DLL before including this header.
 * This ensures CreateGameModule and DestroyGameModule are exported.
 */
#ifdef SPARK_GAME_DLL
    #define SPARK_GAME_API SPARK_EXPORT
#else
    #define SPARK_GAME_API SPARK_IMPORT
#endif
