cmake_minimum_required(VERSION 3.25)
project(SparkAssetPipelineCore LANGUAGES CXX)
add_library(SparkAssetPipelineCore STATIC src/AssetCooker.cpp)
target_include_directories(SparkAssetPipelineCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(SparkAssetPipelineCore PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(SparkAssetPipelineCore PRIVATE /W4 /permissive-)
else()
    target_compile_options(SparkAssetPipelineCore PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_target_properties(SparkAssetPipelineCore PROPERTIES FOLDER "Tools")

if(BUILD_TESTS)
    enable_testing()
    add_executable(SparkAssetPipelineCoreTests tests/AssetCookerTests.cpp)
    target_link_libraries(SparkAssetPipelineCoreTests PRIVATE SparkAssetPipelineCore)
    add_test(NAME SparkAssetPipelineCoreTests COMMAND SparkAssetPipelineCoreTests)
endif()
