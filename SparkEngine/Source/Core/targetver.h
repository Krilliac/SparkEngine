/**
 * @file targetver.h
 * @brief Platform version targeting
 * @author Spark Engine Team
 * @date 2025
 *
 * On Windows, this defines the target Windows platform version.
 * On other platforms, this is a no-op.
 */

#pragma once

#if defined(_WIN32) && defined(_MSC_VER)
// Including SDKDDKVer.h defines the highest available Windows platform.
// If you wish to build your application for a previous Windows platform, include WinSDKVer.h and
// set the _WIN32_WINNT macro to the platform you wish to support before including SDKDDKVer.h.
#include <SDKDDKVer.h>
#elif defined(_WIN32)
// MinGW: define minimum Windows version (Windows 10)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#endif
