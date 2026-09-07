cmake_minimum_required(VERSION 3.25)
project(SparkFPSProgressionConsumer LANGUAGES CXX)

if(NOT DEFINED SPARK_ENGINE_INCLUDE_DIR)
    find_package(SparkEngine REQUIRED)
endif()

# The installed include root also contains SparkEngine internals. Stage only
# the canonical SDK so even a SparkEngine/... include fails in this consumer.
if(NOT EXISTS "${SPARK_ENGINE_INCLUDE_DIR}/Spark/IModule.h")
    message(FATAL_ERROR "The canonical Spark SDK headers are missing")
endif()
set(_public_sdk "${CMAKE_CURRENT_BINARY_DIR}/public-sdk")
file(COPY "${SPARK_ENGINE_INCLUDE_DIR}/Spark" DESTINATION "${_public_sdk}")

# Compile the production FPS logic without linking an engine target: its
# transitive include directories must never hide a private-header dependency.
set(_fps_source "${CMAKE_CURRENT_LIST_DIR}/../../../GameModules/SparkGameFPS/Source")
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
enable_testing()
add_test(NAME SparkFPSProgressionConsumer COMMAND spark_fps_progression_consumer)

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
endforeach()
