cmake_minimum_required(VERSION 3.25)
project(SparkPackageSmoke LANGUAGES CXX)

find_package(SparkEngine REQUIRED)

add_executable(spark_package_smoke main.cpp)
enable_testing()
add_test(NAME SparkInstalledPackageSmoke COMMAND spark_package_smoke)

target_link_libraries(spark_package_smoke PRIVATE Spark::SparkEngineLib)
