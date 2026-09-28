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
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
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

add_executable(SparkFuzzShaderBlob
    FuzzShaderBlob.cpp
    FuzzShaderBlobProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/ShaderDaemonBridge.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzShaderBlob.cpp
# use libstdc++; the production codec and the adapter use libc++ and meet the
# driver only at the C ABI in FuzzShaderBlobProduction.h.
set_source_files_properties(
    FuzzShaderBlobProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/ShaderDaemonBridge.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzShaderBlob PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzShaderBlob PRIVATE cxx_std_23)
target_compile_options(SparkFuzzShaderBlob PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzShaderBlob PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzShaderBlob PRIVATE Threads::Threads c++ c++abi)

# length-overclaim.bin claims a 4 GiB bytecode run in a few bytes; libFuzzer's
# -rss_limit_mb also caps a single malloc at 256 MB, so a decoder that sized the
# allocation before bounding it by the bytes present and the 16 MiB cap would
# fail this test deterministically.
add_test(
    NAME FuzzShaderBlobSmoke
    COMMAND $<TARGET_FILE:SparkFuzzShaderBlob>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=7
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/shader-daemon-blob"
)
set_tests_properties(FuzzShaderBlobSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# The shader daemon codecs are header-only (Utils/ShaderServiceProtocol.h over
# Utils/Serializer.h), so the adapter compiles the whole production decoder.
add_executable(SparkFuzzShaderServiceProtocol
    FuzzShaderServiceProtocol.cpp
    FuzzShaderServiceProtocolProduction.cpp
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the adapter and the decoders it compiles use libc++ and meet the
# driver only at the C ABI in FuzzShaderServiceProtocolProduction.h.
set_source_files_properties(
    FuzzShaderServiceProtocolProduction.cpp
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzShaderServiceProtocol PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzShaderServiceProtocol PRIVATE cxx_std_23)
target_compile_options(SparkFuzzShaderServiceProtocol PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzShaderServiceProtocol PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzShaderServiceProtocol PRIVATE Threads::Threads c++ c++abi)

# The two *-length-overclaim seeds claim a 4 GiB / 2 GiB blob in a few bytes; a
# decoder that resized before checking the bytes present would exceed the
# -rss_limit_mb single-allocation cap and fail this test.
add_test(
    NAME FuzzShaderServiceProtocolSmoke
    COMMAND $<TARGET_FILE:SparkFuzzShaderServiceProtocol>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/shader-service-protocol"
)
set_tests_properties(FuzzShaderServiceProtocolSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# ConfigParser::LoadFromString and SaveToString are header-inline
# (Utils/ConfigParser.h over Utils/StringUtils.h), so the adapter compiles the
# whole production parser; ConfigParser.cpp only adds the LocalFileCache I/O.
add_executable(SparkFuzzConfigParser
    FuzzConfigParser.cpp
    FuzzConfigParserProduction.cpp
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the adapter and the parser it compiles use libc++ and meet the
# driver only at the C ABI in FuzzConfigParserProduction.h.
set_source_files_properties(
    FuzzConfigParserProduction.cpp
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzConfigParser PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzConfigParser PRIVATE cxx_std_23)
target_compile_options(SparkFuzzConfigParser PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzConfigParser PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzConfigParser PRIVATE Threads::Threads c++ c++abi)

# regression-midfile-bom-key.ini is a global key that began with a byte-order
# mark mid-document: the pre-fix parser accepted it, and its own SaveToString
# output then failed to reload, which the adapter's round-trip oracle aborts on.
add_test(
    NAME FuzzConfigParserSmoke
    COMMAND $<TARGET_FILE:SparkFuzzConfigParser>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/config-parser"
)
set_tests_properties(FuzzConfigParserSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_executable(SparkFuzzTelemetrySpool
    FuzzTelemetrySpool.cpp
    FuzzTelemetrySpoolProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/TelemetrySpoolFormat.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the production spool codec and the adapter use libc++ and meet the
# driver only at the C ABI in FuzzTelemetrySpoolProduction.h.
set_source_files_properties(
    FuzzTelemetrySpoolProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/TelemetrySpoolFormat.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzTelemetrySpool PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkSDK/Include"
)
target_compile_features(SparkFuzzTelemetrySpool PRIVATE cxx_std_23)
target_compile_options(SparkFuzzTelemetrySpool PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzTelemetrySpool PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzTelemetrySpool PRIVATE Threads::Threads c++ c++abi)

# event-count-over-max.spool declares 100001 events (one over kAbsoluteMaxEvents)
# and valid-5-events-over-small-cap.spool is valid but over the adapter's
# 4-event caller cap, so both caps are exercised on every replay.
add_test(
    NAME FuzzTelemetrySpoolSmoke
    COMMAND $<TARGET_FILE:SparkFuzzTelemetrySpool>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/telemetry-spool-format"
)
set_tests_properties(FuzzTelemetrySpoolSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# The .spark_save byte codec lives in SaveFileCodec.cpp, split out of SaveSystem.cpp
# so this target links the exact reader Load/QuickLoad/GetSaveSlots run without the
# World/ECS/EventBus closure of the SaveSystem translation unit.
add_executable(SparkFuzzSaveSystem
    FuzzSaveSystem.cpp
    FuzzSaveSystemProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/SaveSystem/SaveFileCodec.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzSaveSystem.cpp use
# libstdc++; the production codec, its logger and the adapter use libc++ and meet
# the driver only at the C ABI in FuzzSaveSystemProduction.h.
set_source_files_properties(
    FuzzSaveSystemProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/SaveSystem/SaveFileCodec.cpp"
    "${SPARK_FUZZ_PRODUCTION_LOGGER}"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzSaveSystem PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzSaveSystem PRIVATE cxx_std_23)
# Logger.cpp routes warnings/errors through DebugHookManager unless
# SPARK_SHIPPING compiles the hooks out; the fuzzer links no hook manager.
target_compile_definitions(SparkFuzzSaveSystem PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzSaveSystem PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzSaveSystem PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzSaveSystem PRIVATE Threads::Threads c++ c++abi)

# entity-count-overclaim declares 4,294,967,295 entities and metadata-length-overclaim
# a 65,520-byte metadata block in a few bytes; a reader that sized or read from either
# before bounding it by the bytes present fails this test under ASan/-rss_limit_mb.
# regression-metadata-carriage-return.save is a v3 save whose sceneName holds a CR: the
# pre-fix reader accepted it although the writer (and so DeserializeWorld) refuses the
# field, which the adapter's re-encode oracle aborts on.
add_test(
    NAME FuzzSaveSystemSmoke
    COMMAND $<TARGET_FILE:SparkFuzzSaveSystem>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=12
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/save-system"
)
set_tests_properties(FuzzSaveSystemSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# PrefabTextFormat is the pure text grammar PrefabAsset::TryLoad delegates to after
# reading the file, so the adapter links it alone (no file I/O, no logger).
add_executable(SparkFuzzEditorPrefab
    FuzzEditorPrefab.cpp
    FuzzEditorPrefabProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEditor/Source/Prefabs/PrefabTextFormat.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzEditorPrefab.cpp
# use libstdc++; the production parser and the adapter use libc++ and meet the
# driver only at the C ABI in FuzzEditorPrefabProduction.h.
set_source_files_properties(
    FuzzEditorPrefabProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEditor/Source/Prefabs/PrefabTextFormat.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzEditorPrefab PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEditor/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzEditorPrefab PRIVATE cxx_std_23)
target_compile_options(SparkFuzzEditorPrefab PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzEditorPrefab PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzEditorPrefab PRIVATE Threads::Threads c++ c++abi)

# duplicate-property.sparkprefab declares one key twice; a parser that kept one of
# them instead of rejecting the file fails the adapter's line-accounting oracle.
add_test(
    NAME FuzzEditorPrefabSmoke
    COMMAND $<TARGET_FILE:SparkFuzzEditorPrefab>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/editor-prefab"
)
set_tests_properties(FuzzEditorPrefabSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# Spark::DeserializeInto is the reflected JSON scene reader behind LoadWorld and
# editor crash recovery. Its closure is the serializer and validation sources,
# the static component reflection registrations that populate ComponentFactory
# and TypeRegistry (ComponentReflection.cpp) and the logger; World is
# header-inline over the pinned EnTT submodule. World's SPARK_REQUIRE checks end
# in Assert::Fail, whose production body (Utils/Assert.cpp) reaches the crash
# handler and console; the adapter defines that fatal sink as print-and-abort,
# which is what production does with suppression off (the default), so a
# violated World precondition is a recorded crash here too.
#
# Core/EngineContext.h reaches the SDK's Version.h, which includes the
# CMake-generated Spark/GeneratedVersion.h. Generate it here from the same
# template and single version source the root project uses, so this target
# builds from the standalone tools/fuzz-policy project as well.
# (The fuzz-policy checker resolves only literal leading arguments, hence the
# module-path include and the argument-free helper.)
list(APPEND CMAKE_MODULE_PATH "${SPARK_FUZZ_PROJECT_ROOT}/cmake")
include(SparkWindowsVersionInfo)
set(SPARK_FUZZ_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
function(spark_fuzz_generate_version_header)
    spark_resolve_engine_version(_spark_fuzz_engine_version)
    string(REPLACE "." ";" _spark_fuzz_version_parts "${_spark_fuzz_engine_version}")
    list(GET _spark_fuzz_version_parts 0 PROJECT_VERSION_MAJOR)
    list(GET _spark_fuzz_version_parts 1 PROJECT_VERSION_MINOR)
    list(GET _spark_fuzz_version_parts 2 PROJECT_VERSION_PATCH)
    file(READ "${SPARK_FUZZ_PROJECT_ROOT}/cmake/SparkGeneratedVersion.h.in" _spark_fuzz_version_template)
    file(CONFIGURE
        OUTPUT "${SPARK_FUZZ_GENERATED_INCLUDE_DIR}/Spark/GeneratedVersion.h"
        CONTENT "${_spark_fuzz_version_template}"
        @ONLY
    )
endfunction()
spark_fuzz_generate_version_header()

add_executable(SparkFuzzReflectedScene
    FuzzReflectedScene.cpp
    FuzzReflectedSceneProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/SceneManager/ReflectedSceneSerializer.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/SceneManager/ReflectedSceneValidation.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Core/ComponentReflection.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the reader, the reflection registry, the logger and the adapter
# use libc++ and meet the driver only at the C ABI in
# FuzzReflectedSceneProduction.h.
set_source_files_properties(
    FuzzReflectedSceneProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/SceneManager/ReflectedSceneSerializer.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/SceneManager/ReflectedSceneValidation.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Core/ComponentReflection.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzReflectedScene PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkSDK/Include"
    "${SPARK_FUZZ_GENERATED_INCLUDE_DIR}"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/json"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/ECS/entt/single_include"
)
target_compile_features(SparkFuzzReflectedScene PRIVATE cxx_std_23)
# Logger.cpp routes warnings/errors through DebugHookManager unless
# SPARK_SHIPPING compiles the hooks out; the fuzzer links no hook manager.
target_compile_definitions(SparkFuzzReflectedScene PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzReflectedScene PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzReflectedScene PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzReflectedScene PRIVATE Threads::Threads c++ c++abi)

add_test(
    NAME FuzzReflectedSceneSmoke
    COMMAND $<TARGET_FILE:SparkFuzzReflectedScene>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=10
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/reflected-scene"
)
set_tests_properties(FuzzReflectedSceneSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# SceneManager's versioned-row, INI and legacy object-line readers live in
# SceneManager/SceneTextFormat.cpp, which depends only on the scene node types,
# so that one source is the whole production closure.
add_executable(SparkFuzzSceneManagerText
    FuzzSceneManagerText.cpp
    FuzzSceneManagerTextProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/SceneManager/SceneTextFormat.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the parsers and the adapter use libc++ and meet the driver only at
# the C ABI in FuzzSceneManagerTextProduction.h.
set_source_files_properties(
    FuzzSceneManagerTextProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/SceneManager/SceneTextFormat.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzSceneManagerText PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzSceneManagerText PRIVATE cxx_std_23)
target_compile_options(SparkFuzzSceneManagerText PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzSceneManagerText PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzSceneManagerText PRIVATE Threads::Threads c++ c++abi)

add_test(
    NAME FuzzSceneManagerTextSmoke
    COMMAND $<TARGET_FILE:SparkFuzzSceneManagerText>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=10
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/scene-manager-text"
)
set_tests_properties(FuzzSceneManagerTextSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# FBXImporter::ImportFromMemory is the importer's whole parse path; its deflate
# arrays inflate through the instrumented SparkFuzzMiniz declared above.
add_executable(SparkFuzzFbx
    FuzzFbx.cpp
    FuzzFbxProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/FBXImporter.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzFbx.cpp use
# libstdc++; the production importer, its logger and the adapter use libc++ and
# meet the driver only at the C ABI in FuzzFbxProduction.h.
set_source_files_properties(
    FuzzFbxProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/FBXImporter.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzFbx PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzFbx PRIVATE cxx_std_23)
# SPARK_MINIZ_AVAILABLE builds the zlib array path the engine ships with.
# Logger.cpp routes warnings/errors through DebugHookManager unless
# SPARK_SHIPPING compiles the hooks out; the fuzzer links no hook manager.
target_compile_definitions(SparkFuzzFbx PRIVATE
    SPARK_SHIPPING=1
    SPARK_MINIZ_AVAILABLE=1
)
target_compile_options(SparkFuzzFbx PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzFbx PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzFbx PRIVATE SparkFuzzMiniz Threads::Threads c++ c++abi)

# array-count-over-1M.fbx declares a ten-million-element (80 MB) zlib array in a
# few bytes. -malloc_limit_mb=64 sits above every allocation a valid document
# under -max_len needs (one 1M-element array is 8 MB), so an importer that sized
# the destination before checking kMaxArrayElements fails this test.
add_test(
    NAME FuzzFbxSmoke
    COMMAND $<TARGET_FILE:SparkFuzzFbx>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        -malloc_limit_mb=64
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/mesh-fbx-importer"
)
set_tests_properties(FuzzFbxSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# The glTF loaders parse through the vendored cgltf, compiled here with the
# fuzzer's coverage and sanitizer instrumentation like the target's own sources.
add_executable(SparkFuzzGltf
    FuzzGltf.cpp
    FuzzGltfProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/GLTFValidation.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/GLTFStaticMeshLoader.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/GLTFSkinnedMeshLoader.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/GLTFSkinSkeleton.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/GLTFAnimationLoader.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/cgltf/cgltf_impl.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzGltf.cpp use
# libstdc++; the production loaders, cgltf and the adapter use libc++ and meet
# the driver only at the C ABI in FuzzGltfProduction.h.
set_source_files_properties(
    FuzzGltfProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/GLTFValidation.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/GLTFStaticMeshLoader.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/GLTFSkinnedMeshLoader.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/GLTFSkinSkeleton.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/GLTFAnimationLoader.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/cgltf/cgltf_impl.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzGltf PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/cgltf"
)
target_compile_features(SparkFuzzGltf PRIVATE cxx_std_23)
target_compile_definitions(SparkFuzzGltf PRIVATE SPARK_HAS_CGLTF=1)
target_compile_options(SparkFuzzGltf PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzGltf PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzGltf PRIVATE Threads::Threads c++ c++abi)

add_test(
    NAME FuzzGltfSmoke
    COMMAND $<TARGET_FILE:SparkFuzzGltf>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=10
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/mesh-gltf-loader"
)
set_tests_properties(FuzzGltfSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_executable(SparkFuzzSoundWav
    FuzzSoundWav.cpp
    FuzzSoundWavProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Audio/SoundEffect.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/MathUtils.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzSoundWav.cpp
# use libstdc++; the production parser, its logger and the adapter use libc++
# and meet the driver only at the C ABI in FuzzSoundWavProduction.h.
set_source_files_properties(
    FuzzSoundWavProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Audio/SoundEffect.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/MathUtils.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzSoundWav PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzSoundWav PRIVATE cxx_std_23)
# Logger.cpp routes warnings/errors through DebugHookManager unless
# SPARK_SHIPPING compiles the hooks out; the fuzzer links no hook manager.
# SoundEffect.cpp's always-on assertions are the SPARK_REQUIRE caller
# preconditions of LoadFromMemory (non-null, non-empty buffer; the adapter never
# passes either) and of the procedural factory; the WAV parse path itself
# (ParseWAVFile, FindChunk, ReadChunkData, IsSupportedWavFormat) asserts nothing.
# DISABLE_ALWAYS_ASSERTS keeps Assert::Fail, and with it the crash-handler and
# console closure, out of this target without changing any parse decision.
target_compile_definitions(SparkFuzzSoundWav PRIVATE
    SPARK_SHIPPING=1
    DISABLE_ALWAYS_ASSERTS=1
)
target_compile_options(SparkFuzzSoundWav PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzSoundWav PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzSoundWav PRIVATE Threads::Threads c++ c++abi)

add_test(
    NAME FuzzSoundWavSmoke
    COMMAND $<TARGET_FILE:SparkFuzzSoundWav>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/audio-sound-effect"
)
set_tests_properties(FuzzSoundWavSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)
