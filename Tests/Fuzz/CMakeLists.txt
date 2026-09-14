if(NOT UNIX OR APPLE OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "SparkFuzzJsonUtils requires a non-Apple UNIX host and the Clang compiler")
endif()

if(NOT DEFINED SPARK_FUZZ_PROJECT_ROOT)
    set(SPARK_FUZZ_PROJECT_ROOT "${CMAKE_SOURCE_DIR}")
endif()

add_executable(SparkFuzzJsonUtils
    FuzzJsonUtils.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)

find_package(Threads REQUIRED)

target_include_directories(SparkFuzzJsonUtils PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzJsonUtils PRIVATE cxx_std_23)
target_compile_definitions(SparkFuzzJsonUtils PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzJsonUtils PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzJsonUtils PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzJsonUtils PRIVATE Threads::Threads)

add_test(
    NAME FuzzJsonUtilsSmoke
    COMMAND $<TARGET_FILE:SparkFuzzJsonUtils>
        -max_len=4096
        -timeout=1
        -rss_limit_mb=128
        -max_total_time=4
        "${SPARK_FUZZ_PROJECT_ROOT}/Tests/fuzz-corpora/json-utils"
)
set_tests_properties(FuzzJsonUtilsSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 5
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)
