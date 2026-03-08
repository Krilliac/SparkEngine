/**
 * @file BuildSystemEnums.h
 * @brief Enumerations for the build and deployment system
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file contains all enumerations related to the build and deployment system,
 * including platform targets, configurations, architectures, and build status types.
 */

#pragma once

namespace SparkEditor
{

    /**
 * @brief Target platforms for building
 */
    enum class BuildPlatform
    {
        WINDOWS_X64 = 0,      ///< Windows 64-bit
        WINDOWS_X86 = 1,      ///< Windows 32-bit
        LINUX_X64 = 2,        ///< Linux 64-bit
        MACOS_X64 = 3,        ///< macOS Intel
        MACOS_ARM64 = 4,      ///< macOS Apple Silicon
        ANDROID_ARM64 = 5,    ///< Android ARM64
        ANDROID_ARM32 = 6,    ///< Android ARM32
        IOS_ARM64 = 7,        ///< iOS ARM64
        WEBGL = 8,            ///< WebGL/WebAssembly
        XBOX_ONE = 9,         ///< Xbox One
        XBOX_SERIES = 10,     ///< Xbox Series X/S
        PLAYSTATION_4 = 11,   ///< PlayStation 4
        PLAYSTATION_5 = 12,   ///< PlayStation 5
        NINTENDO_SWITCH = 13, ///< Nintendo Switch
        CUSTOM = 14           ///< Custom platform
    };

    /**
 * @brief Build configuration types
 */
    enum class BuildConfiguration
    {
        DEBUG = 0,       ///< Debug build with symbols
        DEVELOPMENT = 1, ///< Development build with some optimizations
        RELEASE = 2,     ///< Release build with full optimizations
        SHIPPING = 3,    ///< Shipping build with no debug features
        PROFILING = 4    ///< Profiling build with instrumentation
    };

    /**
 * @brief Build architecture
 */
    enum class BuildArchitecture
    {
        X86 = 0,      ///< x86 32-bit
        X64 = 1,      ///< x64 64-bit
        ARM32 = 2,    ///< ARM 32-bit
        ARM64 = 3,    ///< ARM 64-bit
        UNIVERSAL = 4 ///< Universal binary (multiple architectures)
    };

    /**
 * @brief Build status
 */
    enum class BuildStatus
    {
        IDLE = 0,      ///< No build in progress
        QUEUED = 1,    ///< Build queued
        PREPARING = 2, ///< Preparing build
        COMPILING = 3, ///< Compiling source code
        LINKING = 4,   ///< Linking object files
        PACKAGING = 5, ///< Packaging assets
        DEPLOYING = 6, ///< Deploying to target
        COMPLETED = 7, ///< Build completed successfully
        FAILED = 8,    ///< Build failed
        CANCELLED = 9  ///< Build was cancelled
    };

    /**
 * @brief Deployment target types
 */
    enum class DeploymentTarget
    {
        LOCAL = 0,         ///< Local deployment
        REMOTE_DEVICE = 1, ///< Remote device deployment
        CLOUD_SERVICE = 2, ///< Cloud service deployment
        APP_STORE = 3,     ///< App store deployment
        CUSTOM_SERVER = 4  ///< Custom server deployment
    };

    /**
 * @brief Code signing types
 */
    enum class CodeSigningType
    {
        NONE = 0,         ///< No code signing
        DEVELOPMENT = 1,  ///< Development signing
        DISTRIBUTION = 2, ///< Distribution signing
        ENTERPRISE = 3,   ///< Enterprise signing
        AD_HOC = 4        ///< Ad-hoc signing
    };

    /**
 * @brief Compression types for packaging
 */
    enum class PackageCompressionType
    {
        NONE = 0,  ///< No compression
        ZIP = 1,   ///< ZIP compression
        GZIP = 2,  ///< GZIP compression
        LZMA = 3,  ///< LZMA compression
        BROTLI = 4 ///< Brotli compression
    };

} // namespace SparkEditor