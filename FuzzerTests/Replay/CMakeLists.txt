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

# Each replay executable is declared with a literal add_executable() so the
# build-matrix inventory (Tools/buildmatrix/inventory.py) resolves its name
# statically; spark_fuzz_replay_test() then attaches the shared settings and
# the corpus test.
# spark_fuzz_replay_test(<target> <corpus> [LIBRARIES ...])
function(spark_fuzz_replay_test target corpus)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "" "LIBRARIES")
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_include_directories(${target} PRIVATE "${_spark_fuzz_dir}")
    target_link_libraries(${target} PRIVATE ${ARG_LIBRARIES} Threads::Threads)
    add_dependencies(SparkTests ${target})
    add_test(NAME FuzzReplay_${corpus}
             COMMAND $<TARGET_FILE:${target}> --timeout-ms 5000 "${_spark_fuzz_dir}/corpora/${corpus}")
    set_tests_properties(FuzzReplay_${corpus} PROPERTIES
                         LABELS "fuzz-replay;security;unit"
                         TIMEOUT 60)
endfunction()

find_package(Threads REQUIRED)

add_executable(SparkFuzzReplayJsonUtils FuzzReplayMain.cpp
    "${_spark_fuzz_dir}/FuzzJsonUtils.cpp" "${_spark_fuzz_dir}/FuzzJsonUtilsProduction.cpp")
spark_fuzz_replay_test(SparkFuzzReplayJsonUtils json-utils LIBRARIES SparkEngineLib)

add_executable(SparkFuzzReplaySceneManifest FuzzReplayMain.cpp
    "${_spark_fuzz_dir}/FuzzSceneManifest.cpp" "${_spark_fuzz_dir}/FuzzSceneManifestProduction.cpp")
spark_fuzz_replay_test(SparkFuzzReplaySceneManifest scene-manifest LIBRARIES SparkEngineLib)

add_executable(SparkFuzzReplayArchive FuzzReplayMain.cpp
    "${_spark_fuzz_dir}/FuzzArchive.cpp" "${_spark_fuzz_dir}/FuzzArchiveProduction.cpp")
spark_fuzz_replay_test(SparkFuzzReplayArchive sparkpak-reader LIBRARIES SparkEngineLib)

# The editor material parser is UI-free but lives outside SparkEngineLib. Compile
# the shipped parser and shared PBR schema directly so every committed .spkmat
# seed also runs in normal MSVC/GCC/Clang builds.
add_executable(SparkFuzzReplayMaterialEditorFiles FuzzReplayMain.cpp
    "${_spark_fuzz_dir}/FuzzMaterialEditorFiles.cpp"
    "${_spark_fuzz_dir}/FuzzMaterialEditorFilesProduction.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/MaterialEditorMaterialParser.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source/Panels/MaterialEditorDefaults.cpp")
target_include_directories(SparkFuzzReplayMaterialEditorFiles PRIVATE
    "${CMAKE_SOURCE_DIR}/SparkEditor/Source")
target_compile_definitions(SparkFuzzReplayMaterialEditorFiles PRIVATE SPARK_SHIPPING=1)
spark_fuzz_replay_test(SparkFuzzReplayMaterialEditorFiles material-editor-files)

# The crash reporter is its own executable, so its manifest parser is compiled
# in exactly as SparkCrashReporterManifestTests compiles it.
set(_spark_crash_reporter_src "${CMAKE_SOURCE_DIR}/SparkCrashReporter/src")
set(_spark_crash_reporter_libs)
if(WIN32)
    set(_spark_crash_reporter_libs kernel32 user32 advapi32 bcrypt)
endif()
add_executable(SparkFuzzReplayCrashManifest FuzzReplayMain.cpp
    "${_spark_fuzz_dir}/FuzzCrashManifest.cpp"
    "${_spark_fuzz_dir}/FuzzCrashManifestProduction.cpp"
    "${_spark_crash_reporter_src}/CrashReporterApp.cpp"
    "${_spark_crash_reporter_src}/CrashAutoIssues.cpp")
spark_fuzz_replay_test(SparkFuzzReplayCrashManifest crash-manifest LIBRARIES ${_spark_crash_reporter_libs})
target_include_directories(SparkFuzzReplayCrashManifest PRIVATE
    "${_spark_crash_reporter_src}"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source")
# The root project already defines the Windows platform macros for every target.
target_compile_definitions(SparkFuzzReplayCrashManifest PRIVATE
    SPARK_CRASH_REPORTER_VERSION="${PROJECT_VERSION}")

# These two adapters stage each input through mkstemp/unistd.h, so they replay
# on POSIX builds only; the Windows replay covers the other four corpora.
if(NOT WIN32)
    add_executable(SparkFuzzReplayNeuralWeights FuzzReplayMain.cpp "${_spark_fuzz_dir}/FuzzNeuralWeights.cpp")
    spark_fuzz_replay_test(SparkFuzzReplayNeuralWeights neural-weights-nnw LIBRARIES SparkEngineLib)
    add_executable(SparkFuzzReplayTextureStex FuzzReplayMain.cpp
        "${_spark_fuzz_dir}/FuzzTextureStex.cpp" "${_spark_fuzz_dir}/FuzzTextureStexProduction.cpp")
    spark_fuzz_replay_test(SparkFuzzReplayTextureStex texture-stex LIBRARIES SparkEngineLib)
endif()
