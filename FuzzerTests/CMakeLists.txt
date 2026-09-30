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

# The asset daemon codecs are header-only (Utils/AssetServiceProtocol.h over
# Utils/Serializer.h and Utils/DaemonProtocol.h), so the adapter compiles the
# whole production decoder.
add_executable(SparkFuzzAssetServiceProtocol
    FuzzAssetServiceProtocol.cpp
    FuzzAssetServiceProtocolProduction.cpp
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the adapter and the decoders it compiles use libc++ and meet the
# driver only at the C ABI in FuzzAssetServiceProtocolProduction.h.
set_source_files_properties(
    FuzzAssetServiceProtocolProduction.cpp
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzAssetServiceProtocol PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzAssetServiceProtocol PRIVATE cxx_std_23)
target_compile_options(SparkFuzzAssetServiceProtocol PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzAssetServiceProtocol PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzAssetServiceProtocol PRIVATE Threads::Threads c++ c++abi)

# The blob and path overclaim seeds claim 4 GiB in a few bytes; a decoder that
# resized before checking the bytes present would exceed the -rss_limit_mb
# single-allocation cap and fail this test.
add_test(
    NAME FuzzAssetServiceProtocolSmoke
    COMMAND $<TARGET_FILE:SparkFuzzAssetServiceProtocol>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=9
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/asset-service-protocol"
)
set_tests_properties(FuzzAssetServiceProtocolSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# The daemon frame receive path is header-only (Utils/DaemonFraming.h over
# Utils/DaemonProtocol.h); the adapter drives the POSIX branch of RecvFrame
# through a Unix socket pair.
add_executable(SparkFuzzDaemonFrame
    FuzzDaemonFrame.cpp
    FuzzDaemonFrameProduction.cpp
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the adapter and the receive path it compiles use libc++ and meet
# the driver only at the C ABI in FuzzDaemonFrameProduction.h.
set_source_files_properties(
    FuzzDaemonFrameProduction.cpp
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzDaemonFrame PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzDaemonFrame PRIVATE cxx_std_23)
target_compile_options(SparkFuzzDaemonFrame PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzDaemonFrame PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzDaemonFrame PRIVATE Threads::Threads c++ c++abi)

# regression-header-claims-max-payload.bin claims 16 MiB and sends ten bytes;
# the adapter aborts if the payload buffer grows past what actually arrived.
add_test(
    NAME FuzzDaemonFrameSmoke
    COMMAND $<TARGET_FILE:SparkFuzzDaemonFrame>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=7
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/daemon-protocol-frame"
)
set_tests_properties(FuzzDaemonFrameSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# The daemon's collaboration and orchestration DTO codecs are header-only
# (SparkDaemon/src CollaborationProtocol.h and OrchestrationProtocol.h over
# BoundedWireCodec.h), so the adapter compiles every production decoder.
add_executable(SparkFuzzDaemonWire
    FuzzDaemonWire.cpp
    FuzzDaemonWireProduction.cpp
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the adapter and the decoders it compiles use libc++ and meet the
# driver only at the C ABI in FuzzDaemonWireProduction.h.
set_source_files_properties(
    FuzzDaemonWireProduction.cpp
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzDaemonWire PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkDaemon/src"
)
target_compile_features(SparkFuzzDaemonWire PRIVATE cxx_std_23)
target_compile_options(SparkFuzzDaemonWire PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzDaemonWire PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzDaemonWire PRIVATE Threads::Threads c++ c++abi)

# The count-overclaim seeds claim up to 65,536 snapshot records in a few bytes.
# Everything a valid message under -max_len decodes to stays far below 1 MiB,
# while reserving 65,536 locks or edits takes 2-4 MiB, so -malloc_limit_mb=1
# fails a decoder that reserved before checking the bytes present.
add_test(
    NAME FuzzDaemonWireSmoke
    COMMAND $<TARGET_FILE:SparkFuzzDaemonWire>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        -malloc_limit_mb=1
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=10
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/daemon-bounded-wire-codec"
)
set_tests_properties(FuzzDaemonWireSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# Spark::BinaryReader is header-only (Utils/Serializer.h), so the adapter
# compiles the whole production reader and drives it with an op script.
add_executable(SparkFuzzBinaryReader
    FuzzBinaryReader.cpp
    FuzzBinaryReaderProduction.cpp
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the adapter and the reader it compiles use libc++ and meet the
# driver only at the C ABI in FuzzBinaryReaderProduction.h.
set_source_files_properties(
    FuzzBinaryReaderProduction.cpp
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzBinaryReader PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzBinaryReader PRIVATE cxx_std_23)
target_compile_options(SparkFuzzBinaryReader PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzBinaryReader PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzBinaryReader PRIVATE Threads::Threads c++ c++abi)

# string-length-overclaim.bin claims a 4 GiB string in front of three bytes and
# skip-near-size-max.bin skips SIZE_MAX bytes; a reader whose bounds check could
# wrap would read past the exactly sized ASan buffer.
add_test(
    NAME FuzzBinaryReaderSmoke
    COMMAND $<TARGET_FILE:SparkFuzzBinaryReader>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=6
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/binary-serializer"
)
set_tests_properties(FuzzBinaryReaderSmoke PROPERTIES
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
        -runs=13
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

# The vendored decoders behind the texture and OBJ loaders (tinyexr over
# SparkFuzzMiniz for its deflate path, the stb_image stub, tinyobjloader) are
# compiled into their fuzz targets with the fuzzer's instrumentation, so
# libFuzzer steers through them and ASan sees their buffer use.
#
# EXRLoader::Load is header-only (Graphics/EXRLoader.h), so the adapter compiles
# the whole production loader over the instrumented tinyexr.
add_executable(SparkFuzzExr
    FuzzExr.cpp
    FuzzExrProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/tinyexr/tinyexr_impl.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzExr.cpp use
# libstdc++; the loader, tinyexr and the adapter use libc++ and meet the driver
# only at the C ABI in FuzzExrProduction.h.
set_source_files_properties(
    FuzzExrProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/tinyexr/tinyexr_impl.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzExr PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/tinyexr"
)
target_compile_features(SparkFuzzExr PRIVATE cxx_std_23)
target_compile_definitions(SparkFuzzExr PRIVATE SPARK_HAS_TINYEXR=1)
target_compile_options(SparkFuzzExr PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzExr PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzExr PRIVATE SparkFuzzMiniz Threads::Threads c++ c++abi)

# data-window-overclaim-small-body.exr declares a 16384x16384 float image (inside
# the per-side cap) with a one-entry chunk table; libFuzzer's -rss_limit_mb also
# caps a single malloc at 256 MB, so a loader without EXRLoader's working-set
# bound lets tinyexr allocate a 1 GiB channel plane and fails this test.
# regression-header-error-leak.exr fails two header checks, which made tinyexr
# overwrite (leak) its first error string; LeakSanitizer fails the replay.
add_test(
    NAME FuzzExrSmoke
    COMMAND $<TARGET_FILE:SparkFuzzExr>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=10
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/texture-exr-loader"
)
set_tests_properties(FuzzExrSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_executable(SparkFuzzTextureLinux
    FuzzTextureLinux.cpp
    FuzzTextureLinuxProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/TextureSystemLinuxTexture.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/stb/stb_impl.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/tinyexr/tinyexr_impl.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzTextureLinux.cpp
# use libstdc++; the texture loader, stb_image, tinyexr and the adapter use libc++
# and meet the driver only at the C ABI in FuzzTextureLinuxProduction.h.
set_source_files_properties(
    FuzzTextureLinuxProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/TextureSystemLinuxTexture.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/stb/stb_impl.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/tinyexr/tinyexr_impl.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzTextureLinux PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/stb"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/tinyexr"
)
target_compile_features(SparkFuzzTextureLinux PRIVATE cxx_std_23)
# The engine defines both macros when it links stb_image and tinyexr.
target_compile_definitions(SparkFuzzTextureLinux PRIVATE SPARK_HAS_STB_IMAGE=1 SPARK_HAS_TINYEXR=1)
target_compile_options(SparkFuzzTextureLinux PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzTextureLinux PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzTextureLinux PRIVATE SparkFuzzMiniz Threads::Threads c++ c++abi)

# regression-undecodable-reports-loaded.png is a PNG signature the stb_image stub
# cannot decode: the pre-fix loader returned S_OK with a zero-sized "loaded"
# texture, which the adapter aborts on.
add_test(
    NAME FuzzTextureLinuxSmoke
    COMMAND $<TARGET_FILE:SparkFuzzTextureLinux>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=9
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/texture-loader-linux"
)
set_tests_properties(FuzzTextureLinuxSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_executable(SparkFuzzObjStatic
    FuzzObjStatic.cpp
    FuzzObjStaticProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/OBJStaticMeshLoader.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/TinyObjImpl.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzObjStatic.cpp use
# libstdc++; the OBJ loader, tinyobjloader and the adapter use libc++ and meet the
# driver only at the C ABI in FuzzObjStaticProduction.h.
set_source_files_properties(
    FuzzObjStaticProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/OBJStaticMeshLoader.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/TinyObjImpl.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzObjStatic PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/ThirdParty/Utils/tinyobjloader/include"
)
target_compile_features(SparkFuzzObjStatic PRIVATE cxx_std_23)
target_compile_options(SparkFuzzObjStatic PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzObjStatic PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzObjStatic PRIVATE Threads::Threads c++ c++abi)

# regression-mtllib-parent-escape.obj names ../canary.mtl, the adapter's library
# outside the asset directory: the pre-fix loader read it, so the face's material
# changed with the canary's presence, which the adapter aborts on.
# regression-ngon-quadratic-time.obj (one 3000-corner polygon) took over 4 s to
# ear-clip, past -timeout=1; regression-normal-float-overflow.obj produced zero
# and NaN normals, which the adapter's unit-normal check aborts on.
add_test(
    NAME FuzzObjStaticSmoke
    COMMAND $<TARGET_FILE:SparkFuzzObjStatic>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=11
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/mesh-obj-static-loader"
)
set_tests_properties(FuzzObjStaticSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)


# SEC-120 game-module network decoders: each adapter compiles the production
# codec TU the module runs on untrusted datagrams, with the same libstdc++
# driver / libc++ adapter split as SparkFuzzJsonUtils. The MMO decoders read
# through NetBuffer, so they also compile its production reader and the
# logger it reports overruns through (already libc++ via the property set on
# SPARK_FUZZ_PRODUCTION_LOGGER above).

# The TERRAFRONT LAN beacon codec is self-contained (no NetBuffer, no logger).
add_executable(SparkFuzzTFLanBeacon
    FuzzTFLanBeacon.cpp
    FuzzTFLanBeaconProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMOFPS/Source/Game/TFLanBeaconCodec.cpp"
)
set_source_files_properties(
    FuzzTFLanBeaconProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMOFPS/Source/Game/TFLanBeaconCodec.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzTFLanBeacon PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMOFPS/Source"
)
target_compile_features(SparkFuzzTFLanBeacon PRIVATE cxx_std_23)
target_compile_options(SparkFuzzTFLanBeacon PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzTFLanBeacon PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzTFLanBeacon PRIVATE Threads::Threads c++ c++abi)

# fills-list-past-cap.bin frames 65 distinct beacons from one host, so every
# replay drives UpsertLanServer into its kTFLanMaxServers refusal path.
add_test(
    NAME FuzzTFLanBeaconSmoke
    COMMAND $<TARGET_FILE:SparkFuzzTFLanBeacon>
        -max_len=8192
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=7
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/tf-lan-discovery-beacon"
)
set_tests_properties(FuzzTFLanBeaconSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# MMOChatSystem.h reaches Spark/Version.h, which includes the configure-time
# Spark/GeneratedVersion.h; the chat target reuses the header generated above
# for SparkFuzzReflectedScene (SPARK_FUZZ_GENERATED_INCLUDE_DIR).
add_executable(SparkFuzzMMOChatWire
    FuzzMMOChatWire.cpp
    FuzzMMOChatWireProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source/Chat/MMOChatWire.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Networking/NetworkBuffer.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
set_source_files_properties(
    FuzzMMOChatWireProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source/Chat/MMOChatWire.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Networking/NetworkBuffer.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzMMOChatWire PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkSDK/Include"
    "${SPARK_FUZZ_GENERATED_INCLUDE_DIR}"
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source"
)
target_compile_features(SparkFuzzMMOChatWire PRIVATE cxx_std_23)
# The decoders are networking code (ENABLE_NETWORKING); Logger.cpp routes
# warnings through DebugHookManager unless SPARK_SHIPPING compiles the hooks
# out, and the fuzzer links no hook manager.
target_compile_definitions(SparkFuzzMMOChatWire PRIVATE ENABLE_NETWORKING=1 SPARK_SHIPPING=1)
target_compile_options(SparkFuzzMMOChatWire PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzMMOChatWire PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzMMOChatWire PRIVATE Threads::Threads c++ c++abi)

# string-len-overclaim.bin claims a 65535-byte sender in a few bytes; the
# NetBuffer reader must refuse it before allocating.
add_test(
    NAME FuzzMMOChatWireSmoke
    COMMAND $<TARGET_FILE:SparkFuzzMMOChatWire>
        -max_len=1024
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=7
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/mmo-chat-wire"
)
set_tests_properties(FuzzMMOChatWireSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_executable(SparkFuzzMMOClientState
    FuzzMMOClientState.cpp
    FuzzMMOClientStateProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source/World/MMOClientStateCodec.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Networking/NetworkBuffer.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
set_source_files_properties(
    FuzzMMOClientStateProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source/World/MMOClientStateCodec.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Networking/NetworkBuffer.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzMMOClientState PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkSDK/Include"
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source"
)
target_compile_features(SparkFuzzMMOClientState PRIVATE cxx_std_23)
# The decoders are networking code (ENABLE_NETWORKING); Logger.cpp routes
# warnings through DebugHookManager unless SPARK_SHIPPING compiles the hooks
# out, and the fuzzer links no hook manager.
target_compile_definitions(SparkFuzzMMOClientState PRIVATE ENABLE_NETWORKING=1 SPARK_SHIPPING=1)
target_compile_options(SparkFuzzMMOClientState PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzMMOClientState PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzMMOClientState PRIVATE Threads::Threads c++ c++abi)

add_test(
    NAME FuzzMMOClientStateSmoke
    COMMAND $<TARGET_FILE:SparkFuzzMMOClientState>
        -max_len=1024
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=6
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/mmo-client-state-request"
)
set_tests_properties(FuzzMMOClientStateSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_executable(SparkFuzzMMOEntityEvents
    FuzzMMOEntityEvents.cpp
    FuzzMMOEntityEventsProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source/Player/MMOEntityEventCodec.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Networking/NetworkBuffer.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
set_source_files_properties(
    FuzzMMOEntityEventsProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source/Player/MMOEntityEventCodec.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Networking/NetworkBuffer.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzMMOEntityEvents PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkSDK/Include"
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source"
)
target_compile_features(SparkFuzzMMOEntityEvents PRIVATE cxx_std_23)
# The decoders are networking code (ENABLE_NETWORKING); Logger.cpp routes
# warnings through DebugHookManager unless SPARK_SHIPPING compiles the hooks
# out, and the fuzzer links no hook manager.
target_compile_definitions(SparkFuzzMMOEntityEvents PRIVATE ENABLE_NETWORKING=1 SPARK_SHIPPING=1)
target_compile_options(SparkFuzzMMOEntityEvents PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzMMOEntityEvents PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzMMOEntityEvents PRIVATE Threads::Threads c++ c++abi)

# regression-spawn-nan-position.bin is a spawn whose position is NaN: the
# pre-fix handler decoded it straight into a remote player's position.
add_test(
    NAME FuzzMMOEntityEventsSmoke
    COMMAND $<TARGET_FILE:SparkFuzzMMOEntityEvents>
        -max_len=1024
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=7
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/mmo-player-entity-events"
)
set_tests_properties(FuzzMMOEntityEventsSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# SparkBuild lists a downloaded ZIP in process before any extractor runs. The
# adapter compiles the shipped ArchiveExtraction.cpp; --gc-sections drops the
# staging and commit helpers it never calls.
add_executable(SparkFuzzZipListing
    FuzzZipListing.cpp
    FuzzZipListingProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkBuild/src/ArchiveExtraction.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the SparkBuild archive code and the adapter use libc++ and meet
# the driver only at the C ABI in FuzzZipListingProduction.h.
set_source_files_properties(
    FuzzZipListingProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkBuild/src/ArchiveExtraction.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzZipListing PRIVATE "${SPARK_FUZZ_PROJECT_ROOT}/SparkBuild/src")
# SparkBuild compiles as C++17 (SparkBuild/CMakeLists.txt), and ArchiveExtraction.cpp
# relies on C++17 semantics (path::generic_u8string returns std::string), so the
# fuzz target builds the shipped source under the same standard.
set_target_properties(SparkFuzzZipListing PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_compile_options(SparkFuzzZipListing PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzZipListing PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzZipListing PRIVATE Threads::Threads c++ c++abi)

# comment-65535.zip carries the largest legal archive comment with a decoy end
# record planted inside it, so -max_len admits it (131072, not 65536).
# entry-count-overclaim.zip and cd-offset-past-eof.zip are refused before the
# directory is read.
add_test(
    NAME FuzzZipListingSmoke
    COMMAND $<TARGET_FILE:SparkFuzzZipListing>
        -max_len=131072
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=9
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/sparkbuild-archive-download"
)
set_tests_properties(FuzzZipListingSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# ParseExecScript lives in Core/ExecScriptParse.cpp, apart from the
# ExecScriptPlayer members that call SimpleConsole, so the shipped parser links
# alone: GNU ld reports undefined symbols even in sections --gc-sections drops.
add_executable(SparkFuzzExecScript
    FuzzExecScript.cpp
    FuzzExecScriptProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Core/ExecScriptParse.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the production parser and the adapter use libc++ and meet the
# driver only at the C ABI in FuzzExecScriptProduction.h.
set_source_files_properties(
    FuzzExecScriptProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Core/ExecScriptParse.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzExecScript PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzExecScript PRIVATE cxx_std_23)
target_compile_options(SparkFuzzExecScript PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzExecScript PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzExecScript PRIVATE Threads::Threads c++ c++abi)

# t-400-digit-seconds.txt overflows a double, huge-frame-number.txt overflows
# an int, and t-dots-only.txt is a digit followed only by dots: each must fall
# back to a frame-0 entry instead of scheduling a non-finite or garbage time.
add_test(
    NAME FuzzExecScriptSmoke
    COMMAND $<TARGET_FILE:SparkFuzzExecScript>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=7
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/exec-script-file"
)
set_tests_properties(FuzzExecScriptSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# ConfigManager::LoadFromStream parses sparkbuild.ini; the adapter then builds
# the cmake commands ProcessRunner runs through /bin/sh -c, so both the reader
# and the shell quoting are under test. Config.cpp is SparkBuild's own source.
add_executable(SparkFuzzSparkBuildConfig
    FuzzSparkBuildConfig.cpp
    FuzzSparkBuildConfigProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkBuild/src/Config.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the SparkBuild configuration code and the adapter use libc++ and
# meet the driver only at the C ABI in FuzzSparkBuildConfigProduction.h.
set_source_files_properties(
    FuzzSparkBuildConfigProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkBuild/src/Config.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzSparkBuildConfig PRIVATE "${SPARK_FUZZ_PROJECT_ROOT}/SparkBuild/src")
# SparkBuild compiles as C++17 (SparkBuild/CMakeLists.txt); build its source the same way.
set_target_properties(SparkFuzzSparkBuildConfig PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_compile_options(SparkFuzzSparkBuildConfig PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzSparkBuildConfig PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzSparkBuildConfig PRIVATE Threads::Threads c++ c++abi)

# dollar-in-path, quote-in-path and backslash-path load but must be refused by
# the command builder; duplicate-key, jobs-overflow and unknown-section are
# rejected after earlier keys reached the draft, so the transactional-load
# oracle runs on every replay.
add_test(
    NAME FuzzSparkBuildConfigSmoke
    COMMAND $<TARGET_FILE:SparkFuzzSparkBuildConfig>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/sparkbuild-config"
)
set_tests_properties(FuzzSparkBuildConfigSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# The .vscript decoder (VisualScriptGraphIO.cpp) with the writer that
# serializes its round trip and the palette that names node types and pin
# kinds; JsonUtils is header-only apart from the logger.
add_executable(SparkFuzzVisualScriptGraph
    FuzzVisualScriptGraph.cpp
    FuzzVisualScriptGraphProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Scripting/VisualScriptGraphIO.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Scripting/VisualScriptGraphWriter.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Scripting/VisualScriptNodePalette.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the production decoder, writer, palette, logger and adapter use
# libc++ and meet the driver only at the C ABI in FuzzVisualScriptGraphProduction.h.
set_source_files_properties(
    FuzzVisualScriptGraphProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Scripting/VisualScriptGraphIO.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Scripting/VisualScriptGraphWriter.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Scripting/VisualScriptNodePalette.cpp"
    "${SPARK_FUZZ_PRODUCTION_LOGGER}"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzVisualScriptGraph PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzVisualScriptGraph PRIVATE cxx_std_23)
# Logger.cpp routes warnings/errors through DebugHookManager unless
# SPARK_SHIPPING compiles the hooks out; the fuzzer links no hook manager.
target_compile_definitions(SparkFuzzVisualScriptGraph PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzVisualScriptGraph PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzVisualScriptGraph PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzVisualScriptGraph PRIVATE Threads::Threads c++ c++abi)

# game-manager.vscript is the shipped GameVisualScript graph, already canonical.
# Six seeds carry one defect the decoder must reject (unknown node type,
# duplicate id, id above kMaxNodeId, wire to a missing pin, second wire out of
# one execution output, 32-level nesting). A Float default of 2^24 + 1 is
# accepted and rounds to 2^24, so its canonical round trip must still hold.
add_test(
    NAME FuzzVisualScriptGraphSmoke
    COMMAND $<TARGET_FILE:SparkFuzzVisualScriptGraph>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/visual-script-graph"
)
set_tests_properties(FuzzVisualScriptGraphSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_executable(SparkFuzzNavMesh
    FuzzNavMesh.cpp
    FuzzNavMeshProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/AI/NavMeshFormat.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the production decoder and the adapter use libc++ and meet the
# driver only at the C ABI in FuzzNavMeshProduction.h. NavMeshFormat.cpp holds
# DecodeSnav and RebuildTriangleAdjacency with no Recast/Detour or logger
# dependency, so it is the whole link closure.
set_source_files_properties(
    FuzzNavMeshProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/AI/NavMeshFormat.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzNavMesh PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzNavMesh PRIVATE cxx_std_23)
target_compile_options(SparkFuzzNavMesh PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzNavMesh PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzNavMesh PRIVATE Threads::Threads c++ c++abi)

# The two regression-*.snav seeds name a vertex past the vertex array and a
# triangle past the triangle array; the pre-fix loader accepted both, which the
# adapter's index oracles abort on. vertex-count-overclaim.snav declares 10M
# vertices in 48 bytes, so a decoder that sized the allocation before bounding
# it by the bytes present would exceed the -rss_limit_mb single-malloc cap.
add_test(
    NAME FuzzNavMeshSmoke
    COMMAND $<TARGET_FILE:SparkFuzzNavMesh>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/navmesh-loader"
)
set_tests_properties(FuzzNavMeshSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_executable(SparkFuzzSparkTerrain
    FuzzSparkTerrain.cpp
    FuzzSparkTerrainProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/TerrainAssetFormat.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and the harness use
# libstdc++; the production decoder and the adapter use libc++ and meet the
# driver only at the C ABI in FuzzSparkTerrainProduction.h. TerrainAssetFormat.cpp
# holds DecodeRuntime with no renderer or logger dependency.
set_source_files_properties(
    FuzzSparkTerrainProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Graphics/TerrainAssetFormat.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzSparkTerrain PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzSparkTerrain PRIVATE cxx_std_23)
target_compile_options(SparkFuzzSparkTerrain PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzSparkTerrain PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzSparkTerrain PRIVATE Threads::Threads c++ c++abi)

# regression-nonsquare-8x2.sparkterrain was accepted and published as
# resolution 8 with 16 samples, which the adapter's resolution^2 oracle aborts
# on. heightmap-8194 and splat-overclaim declare sizes past the format bounds
# and past the bytes present.
add_test(
    NAME FuzzSparkTerrainSmoke
    COMMAND $<TARGET_FILE:SparkFuzzSparkTerrain>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/terrain-sparkterrain"
)
set_tests_properties(FuzzSparkTerrainSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# DataTable's CSV/JSON loaders are header-only production code. Logger.cpp
# provides the shipped logging path used by parser diagnostics.
add_executable(SparkFuzzDataTable
    FuzzDataTable.cpp
    FuzzDataTableProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
set_source_files_properties(
    FuzzDataTableProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzDataTable PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzDataTable PRIVATE cxx_std_23)
target_compile_definitions(SparkFuzzDataTable PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzDataTable PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzDataTable PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzDataTable PRIVATE Threads::Threads c++ c++abi)

# The 8 MiB + 1 regression seed reaches the production cap instead of being
# dropped at the harness boundary; the malformed JSON seed checks full parse.
add_test(
    NAME FuzzDataTableSmoke
    COMMAND $<TARGET_FILE:SparkFuzzDataTable>
        -max_len=8388609
        -timeout=1
        -rss_limit_mb=256
        -runs=7
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/datatable-system"
)
set_tests_properties(FuzzDataTableSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# The same extracted wire codec is compiled into the editor and SparkTests.
# The fuzz target reaches it without pulling in the editor's network session.
add_executable(SparkFuzzEditorCollaboration
    FuzzEditorCollaboration.cpp
    FuzzEditorCollaborationProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEditor/Source/Communication/CollaborativeWireCodec.cpp"
)
set_source_files_properties(
    FuzzEditorCollaborationProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEditor/Source/Communication/CollaborativeWireCodec.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzEditorCollaboration PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEditor/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkDaemon/src"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkSDK/Include"
    "${SPARK_FUZZ_GENERATED_INCLUDE_DIR}"
)
target_compile_features(SparkFuzzEditorCollaboration PRIVATE cxx_std_23)
target_compile_options(SparkFuzzEditorCollaboration PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzEditorCollaboration PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzEditorCollaboration PRIVATE Threads::Threads c++ c++abi)

# The five-byte regression frame used to partially publish a rejected decode.
add_test(
    NAME FuzzEditorCollaborationSmoke
    COMMAND $<TARGET_FILE:SparkFuzzEditorCollaboration>
        -max_len=1048576
        -timeout=1
        -rss_limit_mb=256
        -runs=4
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/editor-collab-session-wire"
)
set_tests_properties(FuzzEditorCollaborationSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

add_executable(SparkFuzzLocalization
    FuzzLocalization.cpp
    FuzzLocalizationProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Localization/StringTable.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
set_source_files_properties(
    FuzzLocalizationProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Localization/StringTable.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzLocalization PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzLocalization PRIVATE cxx_std_23)
target_compile_definitions(SparkFuzzLocalization PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzLocalization PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzLocalization PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzLocalization PRIVATE Threads::Threads c++ c++abi)

# A malformed catalog must preserve a previously loaded table.
add_test(
    NAME FuzzLocalizationSmoke
    COMMAND $<TARGET_FILE:SparkFuzzLocalization>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        -runs=6
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/localization-system"
)
set_tests_properties(FuzzLocalizationSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# DialogueTreeLoader.cpp is the same parser translation unit used by the
# engine; the editor/runtime conversation system is outside this link closure.
add_executable(SparkFuzzDialogue
    FuzzDialogue.cpp
    FuzzDialogueProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Dialogue/DialogueTreeLoader.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
set_source_files_properties(
    FuzzDialogueProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Dialogue/DialogueTreeLoader.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzDialogue PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzDialogue PRIVATE cxx_std_23)
target_compile_definitions(SparkFuzzDialogue PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzDialogue PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzDialogue PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzDialogue PRIVATE Threads::Threads c++ c++abi)

# The large regression fixture reaches the production 8 MiB file cap.
add_test(
    NAME FuzzDialogueSmoke
    COMMAND $<TARGET_FILE:SparkFuzzDialogue>
        -max_len=8388609
        -timeout=1
        -rss_limit_mb=256
        -runs=3
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/dialogue-system"
)
set_tests_properties(FuzzDialogueSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# MultiplayerNetFlow::HandleSnapshotMessage calls this same bounded decoder.
add_executable(SparkFuzzFpsSnapshot
    FuzzFpsSnapshot.cpp
    FuzzFpsSnapshotProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameFPS/Source/Game/FpsSnapshotCodec.cpp"
)
set_source_files_properties(
    FuzzFpsSnapshotProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameFPS/Source/Game/FpsSnapshotCodec.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzFpsSnapshot PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkSDK/Include"
    "${SPARK_FUZZ_GENERATED_INCLUDE_DIR}"
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameFPS/Source"
)
target_compile_features(SparkFuzzFpsSnapshot PRIVATE cxx_std_23)
target_compile_options(SparkFuzzFpsSnapshot PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzFpsSnapshot PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzFpsSnapshot PRIVATE Threads::Threads c++ c++abi)

# Exact packet seeds include the noncanonical boolean and action-bit regressions.
add_test(
    NAME FuzzFpsSnapshotSmoke
    COMMAND $<TARGET_FILE:SparkFuzzFpsSnapshot>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/fps-multiplayer-player-state"
)
set_tests_properties(FuzzFpsSnapshotSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# MMO::SessionGateWire::Decode is the only byte grammar the session gate (MMOSessionGate.cpp) reads
# from a client or server peer; the harness also requires every accepted packet to re-encode canonically.
add_executable(SparkFuzzSessionGateProtocol
    FuzzSessionGateProtocol.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source/Session/MMOSessionGateProtocol.cpp"
)
target_include_directories(SparkFuzzSessionGateProtocol PRIVATE "${SPARK_FUZZ_PROJECT_ROOT}")
target_compile_features(SparkFuzzSessionGateProtocol PRIVATE cxx_std_23)
target_compile_options(SparkFuzzSessionGateProtocol PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzSessionGateProtocol PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)

# Replay the six reviewed seeds exactly once.
add_test(
    NAME FuzzSessionGateProtocolSmoke
    COMMAND $<TARGET_FILE:SparkFuzzSessionGateProtocol>
        -max_len=512
        -timeout=1
        -rss_limit_mb=256
        -runs=6
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/session-gate"
)
set_tests_properties(FuzzSessionGateProtocolSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# InstallState.cpp is the whole reader closure: Load for .sparkengine-install.json and
# ReadPendingMarker for .sparkengine-install.pending (Installer.cpp only calls them).
add_executable(SparkFuzzInstallState
    FuzzInstallState.cpp
    FuzzInstallStateProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkInstaller/src/InstallState.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzInstallState.cpp use
# libstdc++; the production reader and the adapter use libc++ and meet the driver
# only at the C ABI in FuzzInstallStateProduction.h.
set_source_files_properties(
    FuzzInstallStateProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkInstaller/src/InstallState.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzInstallState PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkInstaller/src"
)
target_compile_features(SparkFuzzInstallState PRIVATE cxx_std_23)
target_compile_options(SparkFuzzInstallState PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzInstallState PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzInstallState PRIVATE Threads::Threads c++ c++abi)

# regression-schema-overflow.json declares schema 4294967297, which atoi truncated to 1
# on LP64; regression-escaped-quote.json and regression-windows-destination.json hold a
# '\"' and '\\' that the pre-fix reader did not unescape, so their Save -> Load round
# trip changed. The adapter aborts on all three against the old reader.
add_test(
    NAME FuzzInstallStateSmoke
    COMMAND $<TARGET_FILE:SparkFuzzInstallState>
        -max_len=65537
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=6
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/installer-state-manifest"
)
set_tests_properties(FuzzInstallStateSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# ScanForMods lives in ModSystem.cpp and parses each mod.json in ModSystemIO.cpp
# (Json::ParseBounded is header-only in Utils/JsonUtils.h); FileUtils converts the
# UTF-8 paths and Logger backs the log macros. Load/unload and their guarded
# callbacks are in ModSystemLifecycle.cpp, so this closure needs no fault isolator
# (GNU ld reports undefined symbols even in sections --gc-sections drops).
add_executable(SparkFuzzModManifest
    FuzzModManifest.cpp
    FuzzModManifestProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Modding/ModSystem.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Modding/ModSystemIO.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/FileUtils.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
# Same split as SparkFuzzJsonUtils: the libFuzzer driver and FuzzModManifest.cpp use
# libstdc++; the production closure and the adapter use libc++ and meet the driver
# only at the C ABI in FuzzModManifestProduction.h.
set_source_files_properties(
    FuzzModManifestProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Modding/ModSystem.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Modding/ModSystemIO.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/FileUtils.cpp"
    "${SPARK_FUZZ_PRODUCTION_LOGGER}"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzModManifest PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzModManifest PRIVATE cxx_std_23)
# Logger.cpp routes warnings/errors through DebugHookManager unless
# SPARK_SHIPPING compiles the hooks out; the fuzzer links no hook manager.
target_compile_definitions(SparkFuzzModManifest PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzModManifest PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzModManifest PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzModManifest PRIVATE Threads::Threads c++ c++abi)

# regression-control-byte-id.json declares the id "x\u0001y" and
# regression-duplicate-id-pair.bin two directories with one id; the pre-fix scan
# published the first and counted both of the second, which the adapter aborts on.
add_test(
    NAME FuzzModManifestSmoke
    COMMAND $<TARGET_FILE:SparkFuzzModManifest>
        -max_len=131073
        -timeout=1
        -rss_limit_mb=256
        # Replay every reviewed seed exactly once; mutation campaigns belong
        # to scheduled jobs and must not make this merge gate time-dependent.
        -runs=6
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/mod-manifest"
)
set_tests_properties(FuzzModManifestSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# ReplaySystem.cpp is the same translation unit the editor's ReplayPanel loads
# shared .replay files through; the adapter drives seek, playback and kill cam.
add_executable(SparkFuzzReplay
    FuzzReplay.cpp
    FuzzReplayProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Replay/ReplaySystem.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
set_source_files_properties(
    FuzzReplayProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Replay/ReplaySystem.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzReplay PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzReplay PRIVATE cxx_std_23)
target_compile_definitions(SparkFuzzReplay PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzReplay PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzReplay PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzReplay PRIVATE Threads::Threads c++ c++abi)

# The regression seeds (NaN duration, descending frame timestamps, unknown
# version) were accepted before LoadFromFile validated the decoded timeline.
add_test(
    NAME FuzzReplaySmoke
    COMMAND $<TARGET_FILE:SparkFuzzReplay>
        -max_len=1048577
        -timeout=1
        -rss_limit_mb=256
        -runs=7
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/replay-system"
)
set_tests_properties(FuzzReplaySmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# AnimationBinaryFormat.cpp holds the decoders AnimationManager::LoadSkeleton and
# LoadAnimations call for .skel/.sanim; glTF, EnTT and the runtime are outside it.
add_executable(SparkFuzzAnimationBinary
    FuzzAnimationBinary.cpp
    FuzzAnimationBinaryProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Animation/AnimationBinaryFormat.cpp"
)
set_source_files_properties(
    FuzzAnimationBinaryProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Animation/AnimationBinaryFormat.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzAnimationBinary PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzAnimationBinary PRIVATE cxx_std_23)
target_compile_definitions(SparkFuzzAnimationBinary PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzAnimationBinary PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzAnimationBinary PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzAnimationBinary PRIVATE Threads::Threads c++ c++abi)

# -malloc_limit_mb turns a declared key count the file cannot hold into a
# deterministic failure: regression-key-count-amplification.sanim forced a
# 20 MB key vector from 60 bytes before counts were checked against the input.
add_test(
    NAME FuzzAnimationBinarySmoke
    COMMAND $<TARGET_FILE:SparkFuzzAnimationBinary>
        -max_len=1048577
        -timeout=1
        -rss_limit_mb=256
        -malloc_limit_mb=16
        -runs=7
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/animation-skel-sanim"
)
set_tests_properties(FuzzAnimationBinarySmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# OrchestrationJournal.cpp is the translation unit OrchestrationService::LoadJournalLocked
# restores supervised processes through; the protocol codecs it calls are header-only.
add_executable(SparkFuzzOrchestrationJournal
    FuzzOrchestrationJournal.cpp
    FuzzOrchestrationJournalProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkDaemon/src/OrchestrationJournal.cpp"
)
set_source_files_properties(
    FuzzOrchestrationJournalProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkDaemon/src/OrchestrationJournal.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzOrchestrationJournal PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkDaemon/src"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzOrchestrationJournal PRIVATE cxx_std_23)
target_compile_options(SparkFuzzOrchestrationJournal PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzOrchestrationJournal PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzOrchestrationJournal PRIVATE Threads::Threads c++ c++abi)

# The regression seeds (unrepresentable crash timestamp and drain deadline, a
# repeated process id or client instance, an empty client instance) were
# accepted before LoadOrchestrationJournal refused what the service cannot restore.
add_test(
    NAME FuzzOrchestrationJournalSmoke
    COMMAND $<TARGET_FILE:SparkFuzzOrchestrationJournal>
        -max_len=262144
        -timeout=1
        -rss_limit_mb=256
        -runs=11
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/daemon-orchestration-journal"
)
set_tests_properties(FuzzOrchestrationJournalSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# AsyncDatabase.cpp is the store every AsyncDatabasePool connection (and so the MMO
# character, guild and world persistence) loads through SQLiteConnection::Open.
add_executable(SparkFuzzAsyncDatabase
    FuzzAsyncDatabase.cpp
    FuzzAsyncDatabaseProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Persistence/AsyncDatabase.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/SaveFileDurability.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
)
set_source_files_properties(
    FuzzAsyncDatabaseProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Engine/Persistence/AsyncDatabase.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/SaveFileDurability.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source/Utils/Logger.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzAsyncDatabase PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzAsyncDatabase PRIVATE cxx_std_23)
target_compile_definitions(SparkFuzzAsyncDatabase PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzAsyncDatabase PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzAsyncDatabase PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzAsyncDatabase PRIVATE Threads::Threads c++ c++abi)

# regression-legacy-rewrite-over-budget.db and regression-raw-tab-rewrite-over-budget.db
# fit the budget as files but not once FlushToDisk escapes them; Open accepted both.
add_test(
    NAME FuzzAsyncDatabaseSmoke
    COMMAND $<TARGET_FILE:SparkFuzzAsyncDatabase>
        -max_len=65537
        -timeout=1
        -rss_limit_mb=256
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/async-database-kv"
)
set_tests_properties(FuzzAsyncDatabaseSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# MMOCharacterRecord.cpp is the row codec MMOPersistenceSystem::LoadCharacter,
# ListCharacters and BuildCharacterSave call; the persistence system itself needs the
# engine context and database pool and is outside the target.
add_executable(SparkFuzzMMOCharacterRecord
    FuzzMMOCharacterRecord.cpp
    FuzzMMOCharacterRecordProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source/Persistence/MMOCharacterRecord.cpp"
)
set_source_files_properties(
    FuzzMMOCharacterRecordProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source/Persistence/MMOCharacterRecord.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzMMOCharacterRecord PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameMMO/Source"
)
target_compile_features(SparkFuzzMMOCharacterRecord PRIVATE cxx_std_23)
target_compile_options(SparkFuzzMMOCharacterRecord PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzMMOCharacterRecord PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzMMOCharacterRecord PRIVATE Threads::Threads c++ c++abi)

# The regression seeds (a negative account id, trailing text, a NaN stat, extra
# fields, a float the 6-digit writer could not reproduce) all loaded before the row
# codec parsed whole tokens and wrote shortest round-trip floats.
add_test(
    NAME FuzzMMOCharacterRecordSmoke
    COMMAND $<TARGET_FILE:SparkFuzzMMOCharacterRecord>
        -max_len=4096
        -timeout=1
        -rss_limit_mb=256
        -runs=9
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/mmo-character-record"
)
set_tests_properties(FuzzMMOCharacterRecordSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# GatewayAreaControlState.cpp is the epoch-state codec LocalAreaControlService::LoadState
# and SaveState call; sockets, keys and the service thread are outside the target.
add_executable(SparkFuzzGatewayAreaControlState
    FuzzGatewayAreaControlState.cpp
    FuzzGatewayAreaControlStateProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkGateway/src/GatewayAreaControlState.cpp"
)
set_source_files_properties(
    FuzzGatewayAreaControlStateProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkGateway/src/GatewayAreaControlState.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzGatewayAreaControlState PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkGateway/src"
)
target_compile_features(SparkFuzzGatewayAreaControlState PRIVATE cxx_std_23)
target_compile_options(SparkFuzzGatewayAreaControlState PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzGatewayAreaControlState PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzGatewayAreaControlState PRIVATE Threads::Threads c++ c++abi)

# The regression seeds (a negative epoch, a wrapped phase, a signed area id) were
# accepted while the reader used std::istream's signed-tolerant unsigned extraction.
add_test(
    NAME FuzzGatewayAreaControlStateSmoke
    COMMAND $<TARGET_FILE:SparkFuzzGatewayAreaControlState>
        -max_len=65536
        -timeout=1
        -rss_limit_mb=256
        -runs=9
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/gateway-area-control-state"
)
set_tests_properties(FuzzGatewayAreaControlStateSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# OrchestratorIdentity.cpp is the lease every SparkOrchestrator mutation takes before
# it is sent; the adapter drives the real lock, private-file checks and atomic rewrite.
add_executable(SparkFuzzOrchestratorIdentity
    FuzzOrchestratorIdentity.cpp
    FuzzOrchestratorIdentityProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkDaemon/src/OrchestratorIdentity.cpp"
)
set_source_files_properties(
    FuzzOrchestratorIdentityProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkDaemon/src/OrchestratorIdentity.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzOrchestratorIdentity PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkDaemon/src"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzOrchestratorIdentity PRIVATE cxx_std_23)
target_compile_options(SparkFuzzOrchestratorIdentity PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzOrchestratorIdentity PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzOrchestratorIdentity PRIVATE Threads::Threads c++ c++abi)

add_test(
    NAME FuzzOrchestratorIdentitySmoke
    COMMAND $<TARGET_FILE:SparkFuzzOrchestratorIdentity>
        -max_len=257
        -timeout=1
        -rss_limit_mb=256
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/daemon-orchestrator-identity-state"
)
set_tests_properties(FuzzOrchestratorIdentitySmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)

# RTSPersistenceCodec.cpp decodes the save slot's custom state and RTSPersistence.cpp
# validates it; RTSCommandSystem.cpp supplies IsCommandValid. Apply and Capture, which
# need the live systems, are never referenced and --gc-sections drops them.
add_executable(SparkFuzzRTSPersistence
    FuzzRTSPersistence.cpp
    FuzzRTSPersistenceProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameRTS/Source/Core/RTSPersistenceCodec.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameRTS/Source/Core/RTSPersistence.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameRTS/Source/Command/RTSCommandSystem.cpp"
)
set_source_files_properties(
    FuzzRTSPersistenceProduction.cpp
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameRTS/Source/Core/RTSPersistenceCodec.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameRTS/Source/Core/RTSPersistence.cpp"
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameRTS/Source/Command/RTSCommandSystem.cpp"
    PROPERTIES COMPILE_OPTIONS "-stdlib=libc++"
)
target_include_directories(SparkFuzzRTSPersistence PRIVATE
    "${SPARK_FUZZ_PROJECT_ROOT}/GameModules/SparkGameRTS/Source"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkSDK/Include"
    "${SPARK_FUZZ_PROJECT_ROOT}/SparkEngine/Source"
)
target_compile_features(SparkFuzzRTSPersistence PRIVATE cxx_std_23)
target_compile_definitions(SparkFuzzRTSPersistence PRIVATE SPARK_SHIPPING=1)
target_compile_options(SparkFuzzRTSPersistence PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -O1
    -ffunction-sections
    -fdata-sections
)
target_link_options(SparkFuzzRTSPersistence PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
    -Wl,--gc-sections
)
target_link_libraries(SparkFuzzRTSPersistence PRIVATE Threads::Threads c++ c++abi)

add_test(
    NAME FuzzRTSPersistenceSmoke
    COMMAND $<TARGET_FILE:SparkFuzzRTSPersistence>
        -max_len=262144
        -timeout=1
        -rss_limit_mb=256
        -runs=8
        "${SPARK_FUZZ_PROJECT_ROOT}/FuzzerTests/corpora/rts-save-snapshot"
)
set_tests_properties(FuzzRTSPersistenceSmoke PROPERTIES
    LABELS "fuzz;fuzz-smoke;security"
    TIMEOUT 10
    WORKING_DIRECTORY "${SPARK_FUZZ_PROJECT_ROOT}"
)
