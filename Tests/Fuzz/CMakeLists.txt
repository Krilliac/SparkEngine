if(NOT UNIX OR APPLE OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "Spark fuzz targets require a non-Apple UNIX host and the Clang compiler")
endif()

if(NOT DEFINED SPARK_FUZZ_PROJECT_ROOT)
    set(SPARK_FUZZ_PROJECT_ROOT "${CMAKE_SOURCE_DIR}")
endif()

add_executable(SparkFuzzJsonUtils
    FuzzJsonUtils.cpp
    FuzzJsonUtilsProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)

find_package(Threads REQUIRED)

set(SPARK_FUZZ_PRODUCTION_LOGGER
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp")
set_source_files_properties(
    FuzzJsonUtilsProduction.cpp
    "${SPARK_FUZZ_PRODUCTION_LOGGER}"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)

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
# Ubuntu's compiler-rt libFuzzer archive and this harness use libstdc++. The
# production adapter and logger use libc++ and expose only the C byte-buffer
# entry point above, so no C++ standard-library object crosses that boundary.
target_link_libraries(SparkFuzzJsonUtils PRIVATE Threads::Threads c++ c++abi)

add_executable(SparkFuzzNeuralWeights
    FuzzNeuralWeights.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/Neural/NeuralWeights.cpp"
)
target_include_directories(SparkFuzzNeuralWeights PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzNeuralWeights PRIVATE cxx_std_23)
target_compile_options(SparkFuzzNeuralWeights PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzNeuralWeights PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
add_test(
    NAME FuzzJsonUtilsSmoke
    COMMAND $<TARGET_FILE:SparkFuzzJsonUtils>
        -max_len=4096
        -timeout=1
        -rss_limit_mb=256
        -max_total_time=4
        "${SPARK_FUZZ_PROJECT_ROOT}/Tests/fuzz-corpora/json-utils"
)
set_tests_properties(FuzzJsonUtilsSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_test(
    NAME FuzzNeuralWeightsSmoke
    COMMAND $<TARGET_FILE:SparkFuzzNeuralWeights>
        -max_len=4096
        -timeout=1
        -rss_limit_mb=256
        -max_total_time=4
        # CI replays every reviewed seed under sanitizers without mutating the
        # source corpus. Mutation campaigns use a disposable working corpus.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/Tests/fuzz-corpora/neural-weights-nnw"
)
set_tests_properties(FuzzNeuralWeightsSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)
