cmake_minimum_required(VERSION 3.25)

# Match the engine's MSVC/sccache contract for the nested consumer projects.
# The multi-config generator performs a Debug try-compile during project(),
# and /Zi leaves sccache looking for a PDB that the probe does not produce.
# This also keeps nested consumer try-compiles deterministic.
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")

project(SparkFPSProgressionConsumer LANGUAGES CXX)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(NOT DEFINED SPARK_ENGINE_INCLUDE_DIR)
    find_package(SparkEngine REQUIRED)
endif()

# The installed include root also contains SparkEngine internals. Stage only
# the canonical SDK so even a SparkEngine/... include fails in this consumer.
foreach(_required_sdk_header IN ITEMS SparkSDK.h IModule.h Version.h)
    if(NOT EXISTS "${SPARK_ENGINE_INCLUDE_DIR}/Spark/${_required_sdk_header}")
        message(FATAL_ERROR
            "The canonical Spark SDK is incomplete: missing Spark/${_required_sdk_header}")
    endif()
endforeach()
set(_public_sdk "${CMAKE_CURRENT_BINARY_DIR}/public-sdk")
# A removed installed header must not survive from an earlier configure.
file(REMOVE_RECURSE "${_public_sdk}")
file(COPY "${SPARK_ENGINE_INCLUDE_DIR}/Spark" DESTINATION "${_public_sdk}")
set(_fps_source "${CMAKE_CURRENT_LIST_DIR}/../../../GameModules/SparkGameFPS/Source")
enable_testing()

# Compile the production FPS module entrypoint header against only the installed
# Spark SDK. This is a bounded boundary proof: the complete module still has
# additional private-engine dependencies tracked by MOD-310.
add_executable(spark_fps_module_api_consumer module_api.cpp)
target_compile_features(spark_fps_module_api_consumer PRIVATE cxx_std_23)
target_include_directories(spark_fps_module_api_consumer PRIVATE "${_fps_source}" "${_public_sdk}")
set_target_properties(spark_fps_module_api_consumer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_test(NAME SparkFPSModuleAPIConsumer COMMAND spark_fps_module_api_consumer)
set_tests_properties(SparkFPSModuleAPIConsumer PROPERTIES TIMEOUT 60 LABELS "sdk;package;fps;integration")

add_test(NAME SparkFPSModuleSourceBoundary
    COMMAND "${CMAKE_COMMAND}"
        "-DFPS_SOURCE=${_fps_source}"
        -P "${CMAKE_CURRENT_LIST_DIR}/TestModuleSourceBoundary.cmake"
)
set_tests_properties(SparkFPSModuleSourceBoundary PROPERTIES TIMEOUT 60 LABELS "sdk;package;fps;static;integration;process")

# Compile the production FPS logic without linking an engine target: its
# transitive include directories must never hide a private-header dependency.
add_executable(spark_fps_progression_consumer
    main.cpp
    "${_fps_source}/Game/ProgressionSystem.cpp"
    "${_fps_source}/Game/FPSLocalProfile.cpp"
)
target_compile_features(spark_fps_progression_consumer PRIVATE cxx_std_23)
# Match the existing installed-consumer artifact layout (including the
# generator's per-configuration suffix on Windows).
set_target_properties(spark_fps_progression_consumer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_include_directories(spark_fps_progression_consumer PRIVATE
    "${_fps_source}"
    "${_public_sdk}"
)
add_test(NAME SparkFPSProgressionConsumer COMMAND spark_fps_progression_consumer)
set_tests_properties(SparkFPSProgressionConsumer PROPERTIES TIMEOUT 60 LABELS "sdk;package;fps;integration")

# Asset discovery is production FPS code too. Keep its compiler inputs just as
# isolated; linking an engine target would mask accidental private includes.
add_executable(spark_fps_assets_consumer
    assets.cpp
    "${_fps_source}/Game/FPSAssetPaths.cpp"
)
target_compile_features(spark_fps_assets_consumer PRIVATE cxx_std_23)
target_include_directories(spark_fps_assets_consumer PRIVATE "${_fps_source}" "${_public_sdk}")
set_target_properties(spark_fps_assets_consumer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
set(_asset_scenarios working fallback)
if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _asset_scenarios executable parent)
endif()
foreach(_scenario IN LISTS _asset_scenarios)
    add_test(NAME SparkFPSAssets_${_scenario}
        COMMAND "${CMAKE_COMMAND}"
            "-DCONSUMER=$<TARGET_FILE:spark_fps_assets_consumer>"
            "-DFIXTURE=${CMAKE_CURRENT_BINARY_DIR}/asset-fixtures/$<CONFIG>/${_scenario}-é-世界"
            "-DSCENARIO=${_scenario}"
            -P "${CMAKE_CURRENT_LIST_DIR}/RunAssetsConsumer.cmake"
    )
    set_tests_properties(SparkFPSAssets_${_scenario} PROPERTIES
        TIMEOUT 60
        LABELS "sdk;package;fps;asset;integration")
endforeach()

add_executable(spark_fps_weapons_consumer
    weapons.cpp
    "${_fps_source}/Projectiles/WeaponStats.cpp"
)
target_compile_features(spark_fps_weapons_consumer PRIVATE cxx_std_23)
target_include_directories(spark_fps_weapons_consumer PRIVATE "${_fps_source}" "${_public_sdk}")
set_target_properties(spark_fps_weapons_consumer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_test(NAME SparkFPSWeaponsConsumer COMMAND spark_fps_weapons_consumer)
set_tests_properties(SparkFPSWeaponsConsumer PROPERTIES TIMEOUT 60 LABELS "sdk;package;fps;integration")

add_test(NAME SparkSDKHeaderRefresh
    COMMAND "${CMAKE_COMMAND}"
        "-DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DFIXTURE=${CMAKE_CURRENT_BINARY_DIR}/sdk-refresh/$<CONFIG>"
        "-DINSTALLED_INCLUDE_DIR=${SPARK_ENGINE_INCLUDE_DIR}"
        "-DCONSUMER_CONFIG=$<CONFIG>"
        "-DCONSUMER_GENERATOR=${CMAKE_GENERATOR}"
        "-DCONSUMER_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DCONSUMER_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DCONSUMER_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCONSUMER_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCONSUMER_TOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}"
        -P "${CMAKE_CURRENT_LIST_DIR}/TestSDKHeaderRefresh.cmake"
)
set_tests_properties(SparkSDKHeaderRefresh PROPERTIES
    TIMEOUT 300
    LABELS "sdk;package;fps;integration;process")

# Rebuild a copied consumer tree that contains only the FPS source slice and
# staged Spark/ headers. This catches any target that reaches back into the
# checkout for an engine-private header through a relative include.
add_test(NAME SparkFPSProgressionConsumerSourceBoundary
    COMMAND "${CMAKE_COMMAND}"
        "-DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DFPS_SOURCE=${_fps_source}"
        "-DFIXTURE=${CMAKE_CURRENT_BINARY_DIR}/sdk-source-boundary/$<CONFIG>"
        "-DINSTALLED_INCLUDE_DIR=${SPARK_ENGINE_INCLUDE_DIR}"
        "-DCONSUMER_CONFIG=$<CONFIG>"
        "-DCONSUMER_GENERATOR=${CMAKE_GENERATOR}"
        "-DCONSUMER_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DCONSUMER_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DCONSUMER_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCONSUMER_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCONSUMER_TOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}"
        "-DCONSUMER_CTEST=${CMAKE_CTEST_COMMAND}"
        -P "${CMAKE_CURRENT_LIST_DIR}/TestConsumerSourceBoundary.cmake"
)
set_tests_properties(SparkFPSProgressionConsumerSourceBoundary PROPERTIES
    TIMEOUT 300
    LABELS "sdk;package;fps;integration;process")

# ClassSystem and Player use this exact ability implementation in the FPS module.
add_executable(spark_fps_abilities_consumer
    abilities.cpp
    "${_fps_source}/Game/ClassAbilityState.cpp"
)
target_compile_features(spark_fps_abilities_consumer PRIVATE cxx_std_23)
target_include_directories(spark_fps_abilities_consumer PRIVATE "${_fps_source}" "${_public_sdk}")
set_target_properties(spark_fps_abilities_consumer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_test(NAME SparkFPSAbilitiesConsumer COMMAND spark_fps_abilities_consumer)
set_tests_properties(SparkFPSAbilitiesConsumer PROPERTIES TIMEOUT 60 LABELS "sdk;package;fps;integration")

# These inline mutation implementations are the same ones used by the engine event adapters.
add_executable(spark_fps_inventory_quests_consumer inventory_quests.cpp)
target_compile_features(spark_fps_inventory_quests_consumer PRIVATE cxx_std_23)
target_include_directories(spark_fps_inventory_quests_consumer PRIVATE "${_fps_source}" "${_public_sdk}")
set_target_properties(spark_fps_inventory_quests_consumer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
add_test(NAME SparkFPSInventoryQuestsConsumer COMMAND spark_fps_inventory_quests_consumer)
set_tests_properties(SparkFPSInventoryQuestsConsumer PROPERTIES TIMEOUT 60 LABELS "sdk;package;fps;integration")

# SDK-only ModuleLog isolation proof. Each shared library has two translation
# units, so the test catches both cross-TU duplication and ELF interposition
# between module images. Neither library links an engine target or includes
# SparkEngine/Source.
add_library(spark_module_logging_a SHARED
    module_logging_a_bind.cpp
    module_logging_a_observe.cpp
)
add_library(spark_module_logging_b SHARED
    module_logging_b_bind.cpp
    module_logging_b_observe.cpp
)
foreach(_module_logging_target IN ITEMS spark_module_logging_a spark_module_logging_b)
    target_compile_features(${_module_logging_target} PRIVATE cxx_std_23)
    target_include_directories(${_module_logging_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_public_sdk}")
    if(MSVC)
        target_compile_options(${_module_logging_target} PRIVATE /Od /Ob0)
    else()
        target_compile_options(${_module_logging_target} PRIVATE -O0 -fno-inline)
    endif()
endforeach()
target_compile_definitions(spark_module_logging_a PRIVATE SPARK_MODULE_LOG_PROBE_BUILD_A)
target_compile_definitions(spark_module_logging_b PRIVATE SPARK_MODULE_LOG_PROBE_BUILD_B)
add_executable(spark_module_logging_consumer module_logging_consumer.cpp)
target_compile_features(spark_module_logging_consumer PRIVATE cxx_std_23)
target_include_directories(spark_module_logging_consumer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${_public_sdk}")
target_link_libraries(spark_module_logging_consumer PRIVATE spark_module_logging_a spark_module_logging_b)
if(MSVC)
    target_compile_options(spark_module_logging_consumer PRIVATE /Od /Ob0)
else()
    target_compile_options(spark_module_logging_consumer PRIVATE -O0 -fno-inline)
endif()
add_test(NAME SparkSDKModuleLogImageIsolation COMMAND spark_module_logging_consumer)
set_tests_properties(SparkSDKModuleLogImageIsolation PROPERTIES TIMEOUT 60 LABELS "sdk;package;fps;static;integration")

# Header-only FPS logging contract: rollover, zero quota, and deferred message
# construction are checked with a deterministic clock seam in RateLimiter.
add_executable(spark_fps_logging_consumer fps_logging_consumer.cpp)
target_compile_features(spark_fps_logging_consumer PRIVATE cxx_std_23)
target_include_directories(spark_fps_logging_consumer PRIVATE "${_fps_source}" "${_public_sdk}")
add_test(NAME SparkFPSLoggingRateLimit COMMAND spark_fps_logging_consumer)
set_tests_properties(SparkFPSLoggingRateLimit PROPERTIES TIMEOUT 60 LABELS "sdk;package;fps;static;integration")

# Release builds must retain the FPS precondition checks. The Python wrapper
# exercises intentional failures as subprocesses and also checks one evaluation.
add_executable(spark_fps_preconditions_consumer fps_preconditions_consumer.cpp)
target_compile_features(spark_fps_preconditions_consumer PRIVATE cxx_std_23)
target_include_directories(spark_fps_preconditions_consumer PRIVATE "${_fps_source}" "${_public_sdk}")
target_compile_definitions(spark_fps_preconditions_consumer PRIVATE NDEBUG)
add_test(NAME SparkFPSPreconditions
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_LIST_DIR}/RunFPSPreconditions.py"
            "$<TARGET_FILE:spark_fps_preconditions_consumer>")
set_tests_properties(SparkFPSPreconditions PROPERTIES TIMEOUT 60 LABELS "sdk;package;fps;static;integration")
