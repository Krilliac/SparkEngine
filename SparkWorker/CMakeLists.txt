cmake_minimum_required(VERSION 3.25)
project(SparkWorker LANGUAGES CXX)
if(NOT TARGET SparkAssetPipelineCore)
    add_subdirectory("../SparkAssetPipelineCore" "${CMAKE_CURRENT_BINARY_DIR}/SparkAssetPipelineCore")
endif()
add_executable(SparkWorker src/main.cpp)
target_link_libraries(SparkWorker PRIVATE SparkAssetPipelineCore)
target_compile_features(SparkWorker PRIVATE cxx_std_23)
set_target_properties(SparkWorker PROPERTIES FOLDER "Tools")
install(TARGETS SparkWorker RUNTIME DESTINATION bin COMPONENT tools)

if(BUILD_TESTS)
    enable_testing()
    add_test(
        NAME SparkWorkerCliContract
        COMMAND
            ${CMAKE_COMMAND}
            -DSPARK_WORKER=$<TARGET_FILE:SparkWorker>
            -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/worker-cli
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/WorkerCliTests.cmake
    )
    set_tests_properties(SparkWorkerCliContract PROPERTIES LABELS "worker;cli;integration" TIMEOUT 30)
endif()
