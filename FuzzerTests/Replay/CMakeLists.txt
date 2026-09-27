# SEC-120 corpus replay on the normal test build (MSVC, GCC and Clang).
#
# The libFuzzer targets in FuzzerTests/ build only on non-Apple UNIX Clang, so
# without this the committed corpora -- every regression-* fixture included --
# never ran on Windows, the primary platform, nor in the GCC test build. Each
# executable compiles a production harness and its adapter against the shipped
# engine library with FuzzReplayMain.cpp in place of the libFuzzer driver, and
# FuzzReplay_<corpus> feeds it every seed once. An abort, crash or per-input
# timeout fails the test. These are not fuzz targets: the SEC-120 fuzz policy
# counts only FuzzerTests/CMakeLists.txt, and the label is fuzz-replay, never
# fuzz, so run_campaign.py does not mutate them.

set(_spark_fuzz_dir "${CMAKE_SOURCE_DIR}/FuzzerTests")

# spark_add_fuzz_replay(<Name> <corpus> SOURCES ... [LIBRARIES ...])
function(spark_add_fuzz_replay name corpus)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "" "SOURCES;LIBRARIES")
    set(_target "SparkFuzzReplay${name}")
    add_executable(${_target} FuzzReplayMain.cpp ${ARG_SOURCES})
    target_compile_features(${_target} PRIVATE cxx_std_23)
    target_include_directories(${_target} PRIVATE "${_spark_fuzz_dir}")
    target_link_libraries(${_target} PRIVATE ${ARG_LIBRARIES} Threads::Threads)
    add_dependencies(SparkTests ${_target})
    add_test(NAME FuzzReplay_${corpus}
             COMMAND $<TARGET_FILE:${_target}> --timeout-ms 5000 "${_spark_fuzz_dir}/corpora/${corpus}")
    set_tests_properties(FuzzReplay_${corpus} PROPERTIES
                         LABELS "fuzz-replay;security;unit"
                         TIMEOUT 60)
endfunction()

find_package(Threads REQUIRED)

spark_add_fuzz_replay(JsonUtils json-utils
    SOURCES "${_spark_fuzz_dir}/FuzzJsonUtils.cpp" "${_spark_fuzz_dir}/FuzzJsonUtilsProduction.cpp"
    LIBRARIES SparkEngineLib)

spark_add_fuzz_replay(SceneManifest scene-manifest
    SOURCES "${_spark_fuzz_dir}/FuzzSceneManifest.cpp" "${_spark_fuzz_dir}/FuzzSceneManifestProduction.cpp"
    LIBRARIES SparkEngineLib)

spark_add_fuzz_replay(Archive sparkpak-reader
    SOURCES "${_spark_fuzz_dir}/FuzzArchive.cpp" "${_spark_fuzz_dir}/FuzzArchiveProduction.cpp"
    LIBRARIES SparkEngineLib)

# The crash reporter is its own executable, so its manifest parser is compiled
# in exactly as SparkCrashReporterManifestTests compiles it.
set(_spark_crash_reporter_src "${CMAKE_SOURCE_DIR}/SparkCrashReporter/src")
set(_spark_crash_reporter_libs)
if(WIN32)
    set(_spark_crash_reporter_libs kernel32 user32 advapi32 bcrypt)
endif()
spark_add_fuzz_replay(CrashManifest crash-manifest
    SOURCES
        "${_spark_fuzz_dir}/FuzzCrashManifest.cpp"
        "${_spark_fuzz_dir}/FuzzCrashManifestProduction.cpp"
        "${_spark_crash_reporter_src}/CrashReporterApp.cpp"
        "${_spark_crash_reporter_src}/CrashAutoIssues.cpp"
    LIBRARIES ${_spark_crash_reporter_libs})
target_include_directories(SparkFuzzReplayCrashManifest PRIVATE
    "${_spark_crash_reporter_src}"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source")
# The root project already defines the Windows platform macros for every target.
target_compile_definitions(SparkFuzzReplayCrashManifest PRIVATE
    SPARK_CRASH_REPORTER_VERSION="${PROJECT_VERSION}")

# These two adapters stage each input through mkstemp/unistd.h, so they replay
# on POSIX builds only; the Windows replay covers the other four corpora.
if(NOT WIN32)
    spark_add_fuzz_replay(NeuralWeights neural-weights-nnw
        SOURCES "${_spark_fuzz_dir}/FuzzNeuralWeights.cpp"
        LIBRARIES SparkEngineLib)
    spark_add_fuzz_replay(TextureStex texture-stex
        SOURCES "${_spark_fuzz_dir}/FuzzTextureStex.cpp" "${_spark_fuzz_dir}/FuzzTextureStexProduction.cpp"
        LIBRARIES SparkEngineLib)
endif()
