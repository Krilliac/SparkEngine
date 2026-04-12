/**
 * @file AssetTypes.cpp
 * @brief Asset type implementations — platform-split redirect
 *
 * All implementations have been split into platform-specific files:
 *   - AssetTypesWindows.cpp — D3D11 GPU buffer creation, WIC texture loading
 *   - AssetTypesLinux.cpp   — tinyobjloader, cgltf, stb_image, FBXImporter
 *
 * This file intentionally left empty. CMake GLOB_RECURSE discovers the
 * platform files automatically; each guards itself with
 * #ifdef / #ifndef SPARK_PLATFORM_WINDOWS so only the correct one compiles.
 */
