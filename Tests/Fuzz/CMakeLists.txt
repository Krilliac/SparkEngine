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

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" SPARK_FUZZ_SYSTEM_PROCESSOR)
if(SPARK_FUZZ_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
   SPARK_FUZZ_SYSTEM_PROCESSOR STREQUAL "amd64")
    set(SPARK_LIBFUZZER_ARCH x86_64)
elseif(SPARK_FUZZ_SYSTEM_PROCESSOR STREQUAL "aarch64" OR
       SPARK_FUZZ_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(SPARK_LIBFUZZER_ARCH aarch64)
else()
    message(FATAL_ERROR
        "SparkFuzzJsonUtils does not know the compiler-rt libFuzzer archive for ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(SPARK_LIBFUZZER_RUNTIME_NAME "libclang_rt.fuzzer-${SPARK_LIBFUZZER_ARCH}.a")
execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" "-print-file-name=${SPARK_LIBFUZZER_RUNTIME_NAME}"
    OUTPUT_VARIABLE SPARK_LIBFUZZER_RUNTIME
    ERROR_VARIABLE SPARK_LIBFUZZER_RUNTIME_ERROR
    RESULT_VARIABLE SPARK_LIBFUZZER_RUNTIME_RESULT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT SPARK_LIBFUZZER_RUNTIME_RESULT STREQUAL "0" OR
   NOT IS_ABSOLUTE "${SPARK_LIBFUZZER_RUNTIME}" OR
   NOT EXISTS "${SPARK_LIBFUZZER_RUNTIME}")
    message(FATAL_ERROR
        "Could not resolve compiler-rt libFuzzer archive ${SPARK_LIBFUZZER_RUNTIME_NAME}: "
        "${SPARK_LIBFUZZER_RUNTIME_ERROR}${SPARK_LIBFUZZER_RUNTIME}")
endif()

target_include_directories(SparkFuzzJsonUtils PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzJsonUtils PRIVATE cxx_std_23)
target_compile_definitions(SparkFuzzJsonUtils PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzJsonUtils PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzJsonUtils PRIVATE
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
    "-Wl,--whole-archive,${SPARK_LIBFUZZER_RUNTIME},--no-whole-archive"
)
# Ubuntu's compiler-rt libFuzzer archive is built with libstdc++, while the
# production sources use libc++ for their C++23 implementation. Force-load the
# archive here and place libstdc++ in target_link_libraries so its dual-ABI
# symbols are resolved after the archive. The fuzzer callback has no C++ type
# boundary, so the two standard libraries do not exchange C++ objects.
target_link_libraries(SparkFuzzJsonUtils PRIVATE Threads::Threads stdc++)

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
