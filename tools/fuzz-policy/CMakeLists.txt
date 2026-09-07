cmake_minimum_required(VERSION 3.25)
project(SparkFuzzPolicy NONE)

set(BUILD_TESTS ON)
# This standalone project exists only to run the blocking SEC-120 gate, so the
# checks may not be switched off from the configure command line.
set(SPARK_ENABLE_FUZZ_POLICY_CHECKS ON CACHE BOOL "" FORCE)
enable_testing()
get_filename_component(SPARK_FUZZ_POLICY_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${SPARK_FUZZ_POLICY_SOURCE_ROOT}/cmake/SparkFuzzPolicy.cmake")
spark_enable_fuzz_policy("${SPARK_FUZZ_POLICY_SOURCE_ROOT}")
