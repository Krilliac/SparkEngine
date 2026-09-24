cmake_minimum_required(VERSION 3.25)

if(NOT TARGET SparkEngineLib)
    message(FATAL_ERROR "SparkServer must be added after SparkEngineLib")
endif()

if(NOT ENABLE_NETWORKING)
    message(STATUS "SparkServer: skipped because ENABLE_NETWORKING is OFF")
    return()
endif()

add_library(SparkServerCore STATIC
    src/ServerApplication.cpp
    src/ServerApplication.h
    src/ServerHealth.cpp
    src/ServerHealth.h
)

# Operator health carries the exact source identity. The identity header is
# regenerated on every build (and rewritten only when it changes) so a commit
# made after configure still stamps the binary correctly. Source archives
# without git metadata can supply the commit through SPARK_BUILD_COMMIT; live
# git metadata always takes precedence over it.
set(SPARK_BUILD_COMMIT "" CACHE STRING
    "Fallback commit hash for SparkServer health, used only when git cannot report HEAD")
find_package(Git QUIET)
set(_spark_server_identity_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_spark_server_identity_header "${_spark_server_identity_dir}/SparkServerBuildIdentity.h")
add_custom_target(SparkServerBuildIdentity
    COMMAND "${CMAKE_COMMAND}"
        "-DSPARK_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DSPARK_OUTPUT=${_spark_server_identity_header}"
        "-DSPARK_VERSION=${SPARK_ENGINE_VERSION}"
        "-DSPARK_COMMIT_OVERRIDE=${SPARK_BUILD_COMMIT}"
        "-DSPARK_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SparkServerBuildIdentity.cmake"
    BYPRODUCTS "${_spark_server_identity_header}"
    COMMENT "Stamping SparkServer build identity"
    VERBATIM
)

target_compile_features(SparkServerCore PUBLIC cxx_std_23)
target_include_directories(SparkServerCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(WIN32)
    target_link_libraries(SparkServerCore PUBLIC SparkEngineLib)
else()
    # SparkGame modules resolve every engine call from their host process. A
    # normal static-library link discards archive members that SparkServer does
    # not call directly, even though a loaded module may need them later.
    # WHOLE_ARCHIVE maps to --whole-archive on ELF and -force_load on Apple so
    # the complete module-facing runtime is present for ENABLE_EXPORTS below.
    target_link_libraries(SparkServerCore PUBLIC
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,SparkEngineLib>")
    # SparkGatewayCore links SparkEngineLib without a feature. CMake 3.28 (the
    # Ubuntu 24.04 package) rejects that mix as a conflict unless the consumer
    # states which feature wins; newer CMake already resolves it in favour of
    # WHOLE_ARCHIVE, so the override keeps every supported toolchain identical.
    set_property(TARGET SparkServerCore PROPERTY
        LINK_LIBRARY_OVERRIDE "WHOLE_ARCHIVE,SparkEngineLib")
endif()
if(NOT TARGET SparkGatewayCore)
    message(FATAL_ERROR "SparkServer requires SparkGatewayCore; add SparkGateway before SparkServer")
endif()
target_link_libraries(SparkServerCore PUBLIC SparkGatewayCore)
if(WIN32)
    # GetProcessMemoryInfo reports the resident set published in health.
    target_link_libraries(SparkServerCore PRIVATE psapi)
endif()

add_executable(SparkServer src/main.cpp)
target_compile_features(SparkServer PRIVATE cxx_std_23)
target_link_libraries(SparkServer PRIVATE SparkServerCore)
# Only the executable's entry point consumes the stamp, so a new commit
# recompiles main.cpp and relinks SparkServer, not SparkServerCore consumers.
add_dependencies(SparkServer SparkServerBuildIdentity)
target_include_directories(SparkServer PRIVATE "${_spark_server_identity_dir}")

# POSIX game modules intentionally leave engine symbols unresolved until
# dlopen(). SparkServer is a module host just like SparkEngine, so its dynamic
# symbol table must expose the SparkEngineLib symbols that those modules bind.
# Without ENABLE_EXPORTS, the dedicated host builds successfully but rejects
# libSparkGame.so/libSparkGame.dylib immediately at runtime.
if(NOT WIN32)
    set_target_properties(SparkServer PROPERTIES ENABLE_EXPORTS ON)
    set_property(TARGET SparkServer PROPERTY
        LINK_LIBRARY_OVERRIDE "WHOLE_ARCHIVE,SparkEngineLib")
    if(APPLE)
        # SparkGame is shared by multiple hosts, so it cannot use CMake's
        # single-executable -bundle_loader relationship. Its dynamic_lookup
        # references require the dedicated host to retain all global symbols.
        target_link_options(SparkServer PRIVATE "LINKER:-export_dynamic")
    endif()
endif()

if(MSVC)
    target_compile_options(SparkServerCore PRIVATE /W4 /permissive- /Zc:preprocessor)
    target_compile_options(SparkServer PRIVATE /W4 /permissive- /Zc:preprocessor)
else()
    target_compile_options(SparkServerCore PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(SparkServer PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(SparkServer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

install(TARGETS SparkServer RUNTIME DESTINATION bin COMPONENT runtime)
install(FILES config/server.example.ini DESTINATION share/SparkEngine/examples COMPONENT runtime)

if(BUILD_TESTS)
    enable_testing()
    add_test(NAME SparkServerHelp COMMAND $<TARGET_FILE:SparkServer> --help)
    set_tests_properties(SparkServerHelp PROPERTIES TIMEOUT 10 LABELS "server;cli;integration;process")
    # OPS-110: the stamped identity names the configured version and a full
    # commit hash (or "unknown" without git metadata). CMake regexes have no
    # bounded repetition; ServerHealth tests cover the exact field contract.
    add_test(NAME SparkServerVersion COMMAND $<TARGET_FILE:SparkServer> --version)
    string(REPLACE "." "[.]" _spark_server_version_regex "${SPARK_ENGINE_VERSION}")
    set_tests_properties(SparkServerVersion PROPERTIES
        PASS_REGULAR_EXPRESSION
            "^SparkServer ${_spark_server_version_regex} ([0-9a-f]+|unknown) [(](clean|dirty|unknown)[)]"
        LABELS "server;observability"
        TIMEOUT 10)
endif()
