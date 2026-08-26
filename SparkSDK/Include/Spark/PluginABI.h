/**
 * @file PluginABI.h
 * @brief Stable, versioned C ABI for tooling and runtime plugins.
 *
 * This ABI is deliberately independent from Spark's C++ module ABI. Public
 * plugins exchange only fixed-width integers, opaque handles, byte spans, and
 * append-only function tables. STL types, C++ vtables, exceptions, and owning
 * allocations must never cross this boundary.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define SPARK_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define SPARK_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define SPARK_PLUGIN_EXPORT
#endif

#if defined(__cplusplus)
#define SPARK_PLUGIN_EXTERN_C extern "C"
#else
#define SPARK_PLUGIN_EXTERN_C extern
#endif

#define SPARK_PLUGIN_ABI_MAGIC UINT32_C(0x47504B53) /* "SKPG" */
#define SPARK_PLUGIN_ABI_MAJOR UINT32_C(1)
#define SPARK_PLUGIN_ABI_MINOR UINT32_C(1)
#define SPARK_PLUGIN_ENTRY_POINT "SparkGetPluginDescriptor"

typedef uint64_t SparkPluginInstance;
typedef uint64_t SparkPluginTask;
typedef uint64_t SparkPluginResource;

/* Fixed-width aliases are part of the ABI. C enum width is implementation-defined. */
typedef uint32_t SparkPluginResult;
#define SPARK_PLUGIN_OK UINT32_C(0)
#define SPARK_PLUGIN_ERROR_INVALID_ARGUMENT UINT32_C(1)
#define SPARK_PLUGIN_ERROR_UNSUPPORTED UINT32_C(2)
#define SPARK_PLUGIN_ERROR_OUT_OF_MEMORY UINT32_C(3)
#define SPARK_PLUGIN_ERROR_NOT_READY UINT32_C(4)
#define SPARK_PLUGIN_ERROR_BUSY UINT32_C(5)
#define SPARK_PLUGIN_ERROR_TIMEOUT UINT32_C(6)
#define SPARK_PLUGIN_ERROR_INTERNAL UINT32_C(7)
#define SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI UINT32_C(8)

typedef uint32_t SparkPluginLogLevel;
#define SPARK_PLUGIN_LOG_TRACE UINT32_C(0)
#define SPARK_PLUGIN_LOG_DEBUG UINT32_C(1)
#define SPARK_PLUGIN_LOG_INFO UINT32_C(2)
#define SPARK_PLUGIN_LOG_WARNING UINT32_C(3)
#define SPARK_PLUGIN_LOG_ERROR UINT32_C(4)

typedef uint64_t SparkPluginCapability;
#define SPARK_PLUGIN_CAP_NONE UINT64_C(0)
#define SPARK_PLUGIN_CAP_TICK (UINT64_C(1) << 0)
#define SPARK_PLUGIN_CAP_HOT_RELOAD (UINT64_C(1) << 1)
#define SPARK_PLUGIN_CAP_ASSET_IMPORTER (UINT64_C(1) << 2)
#define SPARK_PLUGIN_CAP_ASSET_PROCESSOR (UINT64_C(1) << 3)
#define SPARK_PLUGIN_CAP_EDITOR_EXTENSION (UINT64_C(1) << 4)
#define SPARK_PLUGIN_CAP_RUNTIME_EXTENSION (UINT64_C(1) << 5)

typedef struct SparkPluginBytes
{
    const void* data;
    uint64_t size;
} SparkPluginBytes;

typedef struct SparkPluginMutableBytes
{
    void* data;
    uint64_t size;
    uint64_t capacity;
} SparkPluginMutableBytes;

typedef void (*SparkPluginTaskFn)(void* task_context);

/** Host-owned services. Every allocation returned to the host must use these. */
typedef struct SparkPluginHostAPI
{
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t reserved0;
    void* host_context;

    void* (*allocate)(void* host_context, uint64_t size, uint64_t alignment, uint32_t allocation_tag);
    void* (*reallocate)(void* host_context, void* memory, uint64_t size, uint64_t alignment, uint32_t allocation_tag);
    void (*deallocate)(void* host_context, void* memory, uint64_t alignment, uint32_t allocation_tag);
    void (*log)(void* host_context, SparkPluginLogLevel level, const char* category, const char* message);

    SparkPluginResult (*schedule_task)(void* host_context, SparkPluginTaskFn callback, void* task_context,
                                       SparkPluginTask* out_task);
    SparkPluginResult (*wait_task)(void* host_context, SparkPluginTask task, uint32_t timeout_ms);
    SparkPluginResult (*cancel_task)(void* host_context, SparkPluginTask task);
    SparkPluginResult (*resolve_resource)(void* host_context, const char* stable_id, SparkPluginResource* out_resource);

    void* reserved[8];
} SparkPluginHostAPI;

/** Plugin-owned lifecycle table. Missing optional capabilities use null entries. */
typedef struct SparkPluginAPI
{
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t reserved0;
    uint64_t capabilities;

    SparkPluginResult (*create)(const SparkPluginHostAPI* host, SparkPluginInstance* out_instance);
    void (*destroy)(SparkPluginInstance instance);
    SparkPluginResult (*start)(SparkPluginInstance instance);
    void (*stop)(SparkPluginInstance instance);
    SparkPluginResult (*tick)(SparkPluginInstance instance, double delta_seconds);

    SparkPluginResult (*prepare_unload)(SparkPluginInstance instance, uint32_t timeout_ms);
    SparkPluginResult (*save_state)(SparkPluginInstance instance, SparkPluginMutableBytes* state);
    SparkPluginResult (*restore_state)(SparkPluginInstance instance, SparkPluginBytes state);
    /** Undo a successful prepare_unload when a transactional reload aborts. */
    SparkPluginResult (*cancel_unload)(SparkPluginInstance instance);

    void* reserved[7];
} SparkPluginAPI;

typedef struct SparkPluginDescriptor
{
    uint32_t struct_size;
    uint32_t magic;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t sdk_version;
    uint32_t reserved0;
    const char* id;
    const char* name;
    const char* vendor;
    const char* version;
    const SparkPluginAPI* api;
    void* reserved[8];
} SparkPluginDescriptor;

typedef const SparkPluginDescriptor* (*SparkGetPluginDescriptorFn)(uint32_t host_abi_major, uint32_t host_abi_minor);

static inline SparkPluginResult SparkValidatePluginDescriptor(const SparkPluginDescriptor* descriptor)
{
    const size_t minimum_descriptor_size = offsetof(SparkPluginDescriptor, reserved);
    const size_t minimum_api_size = offsetof(SparkPluginAPI, cancel_unload);
    if (descriptor == NULL || descriptor->api == NULL || descriptor->id == NULL || descriptor->name == NULL)
        return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
    if (descriptor->struct_size < minimum_descriptor_size || descriptor->api->struct_size < minimum_api_size)
        return SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI;
    if (descriptor->magic != SPARK_PLUGIN_ABI_MAGIC || descriptor->abi_major != SPARK_PLUGIN_ABI_MAJOR ||
        descriptor->api->abi_major != SPARK_PLUGIN_ABI_MAJOR)
        return SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI;
    if (descriptor->abi_minor > SPARK_PLUGIN_ABI_MINOR || descriptor->api->abi_minor > SPARK_PLUGIN_ABI_MINOR)
        return SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI;
    if (descriptor->api->create == NULL || descriptor->api->destroy == NULL)
        return SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI;
    if ((descriptor->api->capabilities & SPARK_PLUGIN_CAP_HOT_RELOAD) != 0)
    {
        if (descriptor->api->prepare_unload == NULL || descriptor->api->save_state == NULL ||
            descriptor->api->restore_state == NULL)
            return SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI;
        if (descriptor->api->abi_minor >= UINT32_C(1) &&
            (descriptor->api->struct_size < offsetof(SparkPluginAPI, reserved) ||
             descriptor->api->cancel_unload == NULL))
            return SPARK_PLUGIN_ERROR_INCOMPATIBLE_ABI;
    }
    return SPARK_PLUGIN_OK;
}

/** Declare the single public entry point a plugin must export. */
#define SPARK_DECLARE_PLUGIN_ENTRY_POINT()                                                                             \
    SPARK_PLUGIN_EXTERN_C SPARK_PLUGIN_EXPORT const SparkPluginDescriptor* SparkGetPluginDescriptor(                   \
        uint32_t host_abi_major, uint32_t host_abi_minor)

#if defined(__cplusplus)
#include <type_traits>
static_assert(std::is_standard_layout_v<SparkPluginHostAPI>);
static_assert(std::is_standard_layout_v<SparkPluginAPI>);
static_assert(std::is_standard_layout_v<SparkPluginDescriptor>);
#endif
