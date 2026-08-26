cmake_minimum_required(VERSION 3.25)
project(SparkPackageSmoke LANGUAGES CXX)

find_package(SparkEngine REQUIRED)

if(DEFINED SPARK_EXPECTED_ENGINE_VERSION AND
   NOT SparkEngine_VERSION VERSION_EQUAL SPARK_EXPECTED_ENGINE_VERSION)
    message(FATAL_ERROR
        "Installed SparkEngine package version ${SparkEngine_VERSION} does not match "
        "expected release version ${SPARK_EXPECTED_ENGINE_VERSION}")
endif()

add_executable(spark_package_smoke main.cpp)
enable_testing()
add_test(NAME SparkInstalledPackageSmoke COMMAND spark_package_smoke)

target_link_libraries(spark_package_smoke PRIVATE Spark::SparkEngineLib)

# Compile and link the canonical module entrypoint using only the installed
# public include root. LINK_ONLY deliberately prevents SparkEngineLib's private
# include directories from masking public-header dependency regressions.
add_library(spark_package_module_header_smoke MODULE module_header_smoke.cpp)
target_compile_features(spark_package_module_header_smoke PRIVATE cxx_std_23)
target_include_directories(spark_package_module_header_smoke PRIVATE "${SPARK_ENGINE_INCLUDE_DIR}")
if(WIN32)
    target_link_libraries(spark_package_module_header_smoke PRIVATE "$<LINK_ONLY:Spark::SparkEngineLib>")
else()
    target_link_libraries(spark_package_module_header_smoke PRIVATE Spark::SparkEngineInterface)
endif()
add_dependencies(spark_package_smoke spark_package_module_header_smoke)
