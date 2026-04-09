cmake_minimum_required(VERSION 3.25)
project(SparkPackageSmoke LANGUAGES CXX)

find_package(SparkEngine REQUIRED)

add_executable(spark_package_smoke main.cpp)

target_compile_features(spark_package_smoke PRIVATE cxx_std_23)
target_link_libraries(spark_package_smoke PRIVATE Spark::SparkEngineLib)

target_include_directories(spark_package_smoke PRIVATE
    ${SPARK_ENGINE_INCLUDE_DIR}
    ${SPARK_ENGINE_SDK_DIR}
)
