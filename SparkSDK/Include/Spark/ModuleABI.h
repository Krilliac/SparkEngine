/**
 * @file ModuleABI.h
 * @brief Pre-instantiation compatibility contract for dynamic Spark modules
 *
 * The C++ IModule vtable cannot be queried safely until the engine knows that
 * the module was built for the same ABI. Every supported module therefore
 * exports SparkGetModuleCompatibility(), a pure-C entry point returning this
 * fixed-layout descriptor. ModuleManager checks it before calling any Spark
 * injection hook, CreateModule(), or legacy CreateGameModule().
 */

#pragma once

#include "SparkExport.h"
#include "Version.h"
#include <cstddef>
#include <cstdint>

/** FourCC "SPRK", used to reject unrelated or malformed exports. */
#define SPARK_MODULE_ABI_MAGIC 0x4B525053u
#define SPARK_MODULE_ABI_DESCRIPTOR_SIZE 64u
#define SPARK_MODULE_ABI_DESCRIPTOR_VERSION 1u
#define SPARK_MODULE_RUNTIME_ABI_VERSION 1u
#define SPARK_MODULE_COMPATIBILITY_EXPORT_NAME "SparkGetModuleCompatibility"

#if defined(_MSC_VER)
#define SPARK_MODULE_COMPILER_FAMILY 1u
#define SPARK_MODULE_COMPILER_ABI_VERSION static_cast<uint32_t>(_MSC_VER)
#elif defined(__clang__)
#define SPARK_MODULE_COMPILER_FAMILY 2u
#define SPARK_MODULE_COMPILER_ABI_VERSION static_cast<uint32_t>((__clang_major__ * 100) + __clang_minor__)
#elif defined(__GNUC__)
#define SPARK_MODULE_COMPILER_FAMILY 3u
#define SPARK_MODULE_COMPILER_ABI_VERSION static_cast<uint32_t>((__GNUC__ * 100) + __GNUC_MINOR__)
#else
#define SPARK_MODULE_COMPILER_FAMILY 0u
#define SPARK_MODULE_COMPILER_ABI_VERSION 0u
#endif

#if defined(_MSC_VER) && defined(_DLL) && defined(_DEBUG)
#define SPARK_MODULE_RUNTIME_LIBRARY 2u /* MSVC dynamic debug CRT (/MDd) */
#elif defined(_MSC_VER) && defined(_DLL)
#define SPARK_MODULE_RUNTIME_LIBRARY 1u /* MSVC dynamic release CRT (/MD) */
#elif defined(_MSC_VER) && defined(_DEBUG)
#define SPARK_MODULE_RUNTIME_LIBRARY 4u /* MSVC static debug CRT (/MTd) */
#elif defined(_MSC_VER)
#define SPARK_MODULE_RUNTIME_LIBRARY 3u /* MSVC static release CRT (/MT) */
#else
#define SPARK_MODULE_RUNTIME_LIBRARY 0u /* platform default runtime */
#endif

#if defined(_MSC_VER)
#define SPARK_MODULE_CXX_LANGUAGE_LEVEL static_cast<uint32_t>(_MSVC_LANG)
#else
#define SPARK_MODULE_CXX_LANGUAGE_LEVEL static_cast<uint32_t>(__cplusplus)
#endif

#if defined(_ITERATOR_DEBUG_LEVEL)
#define SPARK_MODULE_ITERATOR_DEBUG_LEVEL static_cast<uint32_t>(_ITERATOR_DEBUG_LEVEL)
#else
#define SPARK_MODULE_ITERATOR_DEBUG_LEVEL 0u
#endif

/**
 * @brief C-layout compatibility descriptor returned by every supported module.
 *
 * Keep this structure append-only. A future descriptor version may append
 * fields and increase structSize; readers must never inspect fields beyond the
 * size they understand.
 */
struct SparkModuleCompatibilityDescriptor
{
    uint32_t structSize;
    uint32_t magic;
    uint32_t descriptorVersion;
    uint32_t sdkVersion;
    uint32_t runtimeABIVersion;
    uint32_t compilerFamily;
    uint32_t compilerABIVersion;
    uint32_t cxxLanguageLevel;
    uint32_t runtimeLibrary;
    uint32_t iteratorDebugLevel;
    uint32_t pointerSize;
    uint32_t reserved[5];
};

// The descriptor is read by hosts of other SDK versions (that is how an old host
// rejects a new module with a diagnostic instead of a crash), and its fields are
// mirrored field-for-field by the .sparkabi sidecar that cmake/SparkGameModule.cmake
// derives from the macros above. Pinned for SPARK_SDK_VERSION 4 / descriptor v1;
// an SDK bump re-pins this block with SparkSDK/ABI/sdk-abi-surface.json.
static_assert(SPARK_SDK_VERSION == 4 && SPARK_MODULE_ABI_DESCRIPTOR_VERSION == 1u,
              "SDK or descriptor version changed: re-pin the compatibility descriptor layout below");
static_assert(sizeof(SparkModuleCompatibilityDescriptor) == SPARK_MODULE_ABI_DESCRIPTOR_SIZE,
              "Update the sidecar ABI writer when the compatibility descriptor changes");
static_assert(SPARK_MODULE_ABI_DESCRIPTOR_SIZE == 64u, "Compatibility descriptor size changed");
static_assert(offsetof(SparkModuleCompatibilityDescriptor, structSize) == 0, "Descriptor layout changed");
static_assert(offsetof(SparkModuleCompatibilityDescriptor, magic) == 4, "Descriptor layout changed");
static_assert(offsetof(SparkModuleCompatibilityDescriptor, descriptorVersion) == 8, "Descriptor layout changed");
static_assert(offsetof(SparkModuleCompatibilityDescriptor, sdkVersion) == 12, "Descriptor layout changed");
static_assert(offsetof(SparkModuleCompatibilityDescriptor, runtimeABIVersion) == 16, "Descriptor layout changed");
static_assert(offsetof(SparkModuleCompatibilityDescriptor, compilerFamily) == 20, "Descriptor layout changed");
static_assert(offsetof(SparkModuleCompatibilityDescriptor, compilerABIVersion) == 24, "Descriptor layout changed");
static_assert(offsetof(SparkModuleCompatibilityDescriptor, cxxLanguageLevel) == 28, "Descriptor layout changed");
static_assert(offsetof(SparkModuleCompatibilityDescriptor, runtimeLibrary) == 32, "Descriptor layout changed");
static_assert(offsetof(SparkModuleCompatibilityDescriptor, iteratorDebugLevel) == 36, "Descriptor layout changed");
static_assert(offsetof(SparkModuleCompatibilityDescriptor, pointerSize) == 40, "Descriptor layout changed");
static_assert(offsetof(SparkModuleCompatibilityDescriptor, reserved) == 44, "Descriptor layout changed");

/** Pure-C export signature resolved before any C++ module factory is called. */
using SparkGetModuleCompatibilityFn = const SparkModuleCompatibilityDescriptor* (*)();

namespace Spark
{
    inline constexpr SparkModuleCompatibilityDescriptor kExpectedModuleCompatibility = {
        sizeof(SparkModuleCompatibilityDescriptor),
        SPARK_MODULE_ABI_MAGIC,
        SPARK_MODULE_ABI_DESCRIPTOR_VERSION,
        SPARK_SDK_VERSION,
        SPARK_MODULE_RUNTIME_ABI_VERSION,
        SPARK_MODULE_COMPILER_FAMILY,
        SPARK_MODULE_COMPILER_ABI_VERSION,
        SPARK_MODULE_CXX_LANGUAGE_LEVEL,
        SPARK_MODULE_RUNTIME_LIBRARY,
        SPARK_MODULE_ITERATOR_DEBUG_LEVEL,
        sizeof(void*),
        {0u, 0u, 0u, 0u, 0u},
    };

    enum class ModuleCompatibilityStatus : uint8_t
    {
        Compatible,
        MissingDescriptor,
        DescriptorTooSmall,
        BadMagic,
        DescriptorVersionMismatch,
        SDKVersionMismatch,
        RuntimeABIVersionMismatch,
        CompilerFamilyMismatch,
        CompilerABIVersionMismatch,
        CxxLanguageLevelMismatch,
        RuntimeLibraryMismatch,
        IteratorDebugLevelMismatch,
        PointerSizeMismatch,
    };

    inline constexpr ModuleCompatibilityStatus CheckModuleCompatibility(
        const SparkModuleCompatibilityDescriptor* descriptor)
    {
        if (!descriptor)
            return ModuleCompatibilityStatus::MissingDescriptor;
        if (descriptor->structSize < sizeof(SparkModuleCompatibilityDescriptor))
            return ModuleCompatibilityStatus::DescriptorTooSmall;
        if (descriptor->magic != kExpectedModuleCompatibility.magic)
            return ModuleCompatibilityStatus::BadMagic;
        if (descriptor->descriptorVersion != kExpectedModuleCompatibility.descriptorVersion)
            return ModuleCompatibilityStatus::DescriptorVersionMismatch;
        if (descriptor->sdkVersion != kExpectedModuleCompatibility.sdkVersion)
            return ModuleCompatibilityStatus::SDKVersionMismatch;
        if (descriptor->runtimeABIVersion != kExpectedModuleCompatibility.runtimeABIVersion)
            return ModuleCompatibilityStatus::RuntimeABIVersionMismatch;
        if (descriptor->compilerFamily != kExpectedModuleCompatibility.compilerFamily)
            return ModuleCompatibilityStatus::CompilerFamilyMismatch;
        if (descriptor->compilerABIVersion != kExpectedModuleCompatibility.compilerABIVersion)
            return ModuleCompatibilityStatus::CompilerABIVersionMismatch;
        if (descriptor->cxxLanguageLevel != kExpectedModuleCompatibility.cxxLanguageLevel)
            return ModuleCompatibilityStatus::CxxLanguageLevelMismatch;
        if (descriptor->runtimeLibrary != kExpectedModuleCompatibility.runtimeLibrary)
            return ModuleCompatibilityStatus::RuntimeLibraryMismatch;
        if (descriptor->iteratorDebugLevel != kExpectedModuleCompatibility.iteratorDebugLevel)
            return ModuleCompatibilityStatus::IteratorDebugLevelMismatch;
        if (descriptor->pointerSize != kExpectedModuleCompatibility.pointerSize)
            return ModuleCompatibilityStatus::PointerSizeMismatch;
        return ModuleCompatibilityStatus::Compatible;
    }

    inline constexpr const char* ModuleCompatibilityStatusName(ModuleCompatibilityStatus status)
    {
        switch (status)
        {
        case ModuleCompatibilityStatus::Compatible:
            return "compatible";
        case ModuleCompatibilityStatus::MissingDescriptor:
            return "missing compatibility descriptor";
        case ModuleCompatibilityStatus::DescriptorTooSmall:
            return "compatibility descriptor is too small";
        case ModuleCompatibilityStatus::BadMagic:
            return "compatibility descriptor magic mismatch";
        case ModuleCompatibilityStatus::DescriptorVersionMismatch:
            return "compatibility descriptor version mismatch";
        case ModuleCompatibilityStatus::SDKVersionMismatch:
            return "SDK ABI version mismatch";
        case ModuleCompatibilityStatus::RuntimeABIVersionMismatch:
            return "module runtime ABI version mismatch";
        case ModuleCompatibilityStatus::CompilerFamilyMismatch:
            return "compiler ABI family mismatch";
        case ModuleCompatibilityStatus::CompilerABIVersionMismatch:
            return "compiler ABI version mismatch";
        case ModuleCompatibilityStatus::CxxLanguageLevelMismatch:
            return "C++ language level mismatch";
        case ModuleCompatibilityStatus::RuntimeLibraryMismatch:
            return "runtime library mismatch";
        case ModuleCompatibilityStatus::IteratorDebugLevelMismatch:
            return "iterator debug level mismatch";
        case ModuleCompatibilityStatus::PointerSizeMismatch:
            return "pointer size mismatch";
        }
        return "unknown compatibility error";
    }

    /** @brief The descriptor field a failed check compared, as both sides see it. */
    struct ModuleCompatibilityMismatch
    {
        const char* field = nullptr; ///< .sparkabi sidecar key, or nullptr when there is no single field
        uint32_t moduleValue = 0;    ///< Value the module advertised
        uint32_t hostValue = 0;      ///< Value this host requires
    };

    /**
     * @brief Name the field behind a CheckModuleCompatibility() failure and both values.
     *
     * Lets a host say "module sdk_version=5, host sdk_version=4" instead of just
     * "SDK ABI version mismatch". Returns an empty mismatch (field == nullptr) for
     * Compatible and MissingDescriptor.
     */
    inline constexpr ModuleCompatibilityMismatch GetModuleCompatibilityMismatch(
        const SparkModuleCompatibilityDescriptor* descriptor, ModuleCompatibilityStatus status)
    {
        if (!descriptor)
            return {};
        const SparkModuleCompatibilityDescriptor& host = kExpectedModuleCompatibility;
        switch (status)
        {
        case ModuleCompatibilityStatus::DescriptorTooSmall:
            return {"struct_size", descriptor->structSize, host.structSize};
        case ModuleCompatibilityStatus::BadMagic:
            return {"magic", descriptor->magic, host.magic};
        case ModuleCompatibilityStatus::DescriptorVersionMismatch:
            return {"format", descriptor->descriptorVersion, host.descriptorVersion};
        case ModuleCompatibilityStatus::SDKVersionMismatch:
            return {"sdk_version", descriptor->sdkVersion, host.sdkVersion};
        case ModuleCompatibilityStatus::RuntimeABIVersionMismatch:
            return {"runtime_abi_version", descriptor->runtimeABIVersion, host.runtimeABIVersion};
        case ModuleCompatibilityStatus::CompilerFamilyMismatch:
            return {"compiler_family", descriptor->compilerFamily, host.compilerFamily};
        case ModuleCompatibilityStatus::CompilerABIVersionMismatch:
            return {"compiler_abi_version", descriptor->compilerABIVersion, host.compilerABIVersion};
        case ModuleCompatibilityStatus::CxxLanguageLevelMismatch:
            return {"cxx_language_level", descriptor->cxxLanguageLevel, host.cxxLanguageLevel};
        case ModuleCompatibilityStatus::RuntimeLibraryMismatch:
            return {"runtime_library", descriptor->runtimeLibrary, host.runtimeLibrary};
        case ModuleCompatibilityStatus::IteratorDebugLevelMismatch:
            return {"iterator_debug_level", descriptor->iteratorDebugLevel, host.iteratorDebugLevel};
        case ModuleCompatibilityStatus::PointerSizeMismatch:
            return {"pointer_size", descriptor->pointerSize, host.pointerSize};
        case ModuleCompatibilityStatus::Compatible:
        case ModuleCompatibilityStatus::MissingDescriptor:
            break;
        }
        return {};
    }
} // namespace Spark

/**
 * @brief Export the mandatory compatibility descriptor without factory code.
 *
 * SPARK_IMPLEMENT_MODULE includes this automatically. Use this standalone
 * macro only for a module that declares its factory exports manually.
 */
#define SPARK_EXPORT_MODULE_COMPATIBILITY()                                                                            \
    extern "C" SPARK_MODULE_API const SparkModuleCompatibilityDescriptor* SparkGetModuleCompatibility()                \
    {                                                                                                                  \
        return &Spark::kExpectedModuleCompatibility;                                                                   \
    }
