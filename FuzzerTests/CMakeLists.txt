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
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent
        # or write mutated units into the tracked corpus.
        -runs=7
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/json-utils"
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
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/neural-weights-nnw"
)
set_tests_properties(FuzzNeuralWeightsSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_executable(SparkFuzzCrashManifest
    FuzzCrashManifest.cpp
    FuzzCrashManifestProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkCrashReporter/src/CrashReporterApp.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkCrashReporter/src/CrashAutoIssues.cpp"
)
target_include_directories(SparkFuzzCrashManifest PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkCrashReporter/src"
)
target_compile_features(SparkFuzzCrashManifest PRIVATE cxx_std_23)
target_compile_options(SparkFuzzCrashManifest PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
)
target_link_options(SparkFuzzCrashManifest PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
)
target_link_libraries(SparkFuzzCrashManifest PRIVATE Threads::Threads)

add_test(
    NAME FuzzCrashManifestSmoke
    COMMAND $<TARGET_FILE:SparkFuzzCrashManifest>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=6
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/crash-manifest"
)
set_tests_properties(FuzzCrashManifestSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_executable(SparkFuzzTextureStex
    FuzzTextureStex.cpp
    FuzzTextureStexProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/TextureCompressor.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/TextureBlockCompression.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzTextureStex.cpp
# use libstdc++; the production loader, its logger, and the adapter use libc++
# and meet the driver only at the C ABI in FuzzTextureStexProduction.h.
set_source_files_properties(
    FuzzTextureStexProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/TextureCompressor.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/TextureBlockCompression.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzTextureStex PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzTextureStex PRIVATE cxx_std_23)
# Logger.cpp routes warnings/errors through DebugHookManager unless
# SPARK_SHIPPING compiles the hooks out; the fuzzer links no hook manager.
target_compile_definitions(SparkFuzzTextureStex PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzTextureStex PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzTextureStex PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzTextureStex PRIVATE Threads::Threads c++ c++abi)

# The corpus holds two minimized regressions (regression-*.stex) that made the
# pre-fix loader allocate 16 GiB / 4 GiB from a header field; libFuzzer's
# -rss_limit_mb also caps a single malloc at 256 MB, so replaying them against
# an unbounded loader fails this test deterministically.
add_test(
    NAME FuzzTextureStexSmoke
    COMMAND $<TARGET_FILE:SparkFuzzTextureStex>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/texture-stex"
)
set_tests_properties(FuzzTextureStexSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_executable(SparkFuzzSceneManifest
    FuzzSceneManifest.cpp
    FuzzSceneManifestProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Modding/VirtualFileSystem.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzSceneManifest.cpp
# use libstdc++; the header-only production parser (compiled into the adapter),
# the VFS path policy and the logger use libc++ and meet the driver only at the
# C ABI in FuzzSceneManifestProduction.h.
set_source_files_properties(
    FuzzSceneManifestProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Modding/VirtualFileSystem.cpp"
    "${SPARK_FUZZ_PRODUCTION_LOGGER}"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzSceneManifest PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzSceneManifest PRIVATE cxx_std_23)
# Logger.cpp routes warnings/errors through DebugHookManager unless
# SPARK_SHIPPING compiles the hooks out; the fuzzer links no hook manager.
target_compile_definitions(SparkFuzzSceneManifest PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzSceneManifest PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzSceneManifest PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzSceneManifest PRIVATE Threads::Threads c++ c++abi)

add_test(
    NAME FuzzSceneManifestSmoke
    COMMAND $<TARGET_FILE:SparkFuzzSceneManifest>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/scene-manifest"
)
set_tests_properties(FuzzSceneManifestSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# The SparkPak reader inflates deflate entries and its TOC through the vendored
# miniz. It is compiled here with the fuzzer's coverage and sanitizer
# instrumentation (instead of linking the root project's uninstrumented miniz
# target) so libFuzzer can steer through inflate and ASan sees its buffer use.
enable_language(C)
set(SPARK_FUZZ_MINIZ_ROOT "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/miniz")
add_library(SparkFuzzMiniz STATIC
    "${SPARK_FUZZ_MINIZ_ROOT}/miniz.c"
    "${SPARK_FUZZ_MINIZ_ROOT}/miniz_tdef.c"
    "${SPARK_FUZZ_MINIZ_ROOT}/miniz_tinfl.c"
)
include(GenerateExportHeader)
generate_export_header(SparkFuzzMiniz
    BASE_NAME miniz
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/miniz-export/miniz_export.h"
)
target_include_directories(SparkFuzzMiniz PUBLIC
    "${SPARK_FUZZ_MINIZ_ROOT}"
    "${CMAKE_CURRENT_BINARY_DIR}/miniz-export"
)
target_compile_definitions(SparkFuzzMiniz PUBLIC MINIZ_STATIC_DEFINE)
# Coverage instrumentation only (the executable links the libFuzzer driver).
target_compile_options(SparkFuzzMiniz PRIVATE
    -fsanitize=fuzzer-no-link
    -fsanitize=address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -w
)

add_executable(SparkFuzzArchive
    FuzzArchive.cpp
    FuzzArchiveProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Core/SparkPak.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Modding/VirtualFileSystem.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzArchive.cpp use
# libstdc++; the production reader, the VFS path policy, the logger and the
# adapter use libc++ and meet the driver only at the C ABI in
# FuzzArchiveProduction.h.
set_source_files_properties(
    FuzzArchiveProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Core/SparkPak.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Modding/VirtualFileSystem.cpp"
    "${SPARK_FUZZ_PRODUCTION_LOGGER}"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzArchive PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/zstd"
)
target_compile_features(SparkFuzzArchive PRIVATE cxx_std_23)
# Build the reader with both codecs the engine ships (the root project defines
# the same two macros when it links miniz and the vendored zstd). Logger.cpp
# routes warnings/errors through DebugHookManager unless SPARK_SHIPPING compiles
# the hooks out; the fuzzer links no hook manager.
target_compile_definitions(SparkFuzzArchive PRIVATE
    SPARK_SHIPPING=1
    SPARK_MINIZ_AVAILABLE=1
    SPARK_ZSTD_AVAILABLE=1
)
target_compile_options(SparkFuzzArchive PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzArchive PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzArchive PRIVATE SparkFuzzMiniz Threads::Threads c++ c++abi)

add_test(
    NAME FuzzArchiveSmoke
    COMMAND $<TARGET_FILE:SparkFuzzArchive>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=512
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=11
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/sparkpak-reader"
)
set_tests_properties(FuzzArchiveSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)
