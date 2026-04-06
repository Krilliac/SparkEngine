/**
 * @file zstd_impl.cpp
 * @brief Compilation unit for zstd stub (ensures the header compiles cleanly)
 *
 * The real zstd library would have hundreds of .c files here. This stub
 * uses header-only inline functions, so this file just validates compilation.
 */

#include "zstd.h"

// Validate stub compiles and links correctly
static_assert(ZSTD_VERSION_MAJOR == 1, "zstd stub version check");
