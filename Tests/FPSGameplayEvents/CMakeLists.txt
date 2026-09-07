cmake_minimum_required(VERSION 3.25)
project(SparkFPSGameplayEventsTests LANGUAGES CXX)

# This is an engine integration test, deliberately separate from the isolated
# canonical-SDK consumers. Compile the production adapter and real EventBus.
set(_engine_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(spark_fps_gameplay_events_tests
    main.cpp
    "${_engine_root}/GameModules/SparkGameFPS/Source/Game/FPSGameplayEvents.cpp"
)
target_compile_features(spark_fps_gameplay_events_tests PRIVATE cxx_std_23)
target_include_directories(spark_fps_gameplay_events_tests PRIVATE
    "${_engine_root}/GameModules/SparkGameFPS/Source"
    "${_engine_root}/SparkEngine/Source"
)
find_package(Threads REQUIRED)
target_link_libraries(spark_fps_gameplay_events_tests PRIVATE Threads::Threads)
enable_testing()
add_test(NAME SparkFPSGameplayEvents COMMAND spark_fps_gameplay_events_tests)
