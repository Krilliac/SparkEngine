#pragma once

// Platform detection
#if defined(_WIN32) || defined(_WIN64)
#define SPARK_PLATFORM_WINDOWS 1
#define SPARK_PLATFORM_NAME "Windows"
#elif defined(__APPLE__) && defined(__MACH__)
#define SPARK_PLATFORM_MACOS 1
#define SPARK_PLATFORM_NAME "macOS"
#elif defined(__linux__)
#define SPARK_PLATFORM_LINUX 1
#define SPARK_PLATFORM_NAME "Linux"
#else
#error "Unsupported platform"
#endif

// Unix-like platforms
#if defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
#define SPARK_PLATFORM_UNIX 1
#endif

// Path separator
#ifdef SPARK_PLATFORM_WINDOWS
#define SPARK_PATH_SEP "\\"
#define SPARK_PATH_SEP_CHAR '\\'
#define SPARK_EXE_EXT ".exe"
#else
#define SPARK_PATH_SEP "/"
#define SPARK_PATH_SEP_CHAR '/'
#define SPARK_EXE_EXT ""
#endif

// Shared library extension
#ifdef SPARK_PLATFORM_WINDOWS
#define SPARK_SHARED_EXT ".dll"
#elif defined(SPARK_PLATFORM_MACOS)
#define SPARK_SHARED_EXT ".dylib"
#else
#define SPARK_SHARED_EXT ".so"
#endif
