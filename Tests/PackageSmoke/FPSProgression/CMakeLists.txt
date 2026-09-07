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
