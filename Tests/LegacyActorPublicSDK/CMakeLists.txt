cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0131 NEW)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
project(SparkLegacyActorPublicSDK LANGUAGES CXX)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(PROJECT_IS_TOP_LEVEL)
    # A standalone run must link the real installed archive. Never compile a
    # replacement GameObject, Mesh, primitive, or Model implementation here.
    find_package(SparkEngine CONFIG REQUIRED)
    set(_actor_engine Spark::SparkEngineLib)
    set(_actor_sdk_source "${SPARK_ENGINE_INCLUDE_DIR}")
else()
    set(_actor_engine SparkEngineLib)
    set(_actor_sdk_source "${CMAKE_SOURCE_DIR}/SparkSDK/Include")
endif()
if(NOT TARGET ${_actor_engine})
    message(FATAL_ERROR "Actor SDK consumer requires the real SparkEngineLib target")
endif()

# The existing POSIX native compatibility header uses SDL's public SDK when
# the producer enables SDL. Carry only that dependency, never the engine's
# private include directories, across the isolated compile boundary.
set(_actor_platform_dependency "")
if(NOT WIN32)
    if(TARGET SDL2::SDL2)
        set(_actor_platform_dependency SDL2::SDL2)
    elseif(TARGET SDL2)
        set(_actor_platform_dependency SDL2)
    endif()
endif()
set(_actor_system_includes "")
if(_actor_platform_dependency)
    set(_actor_system_includes "$<TARGET_PROPERTY:${_actor_platform_dependency},INTERFACE_INCLUDE_DIRECTORIES>")
endif()

# The consumer never sees the full source or installed include/SparkEngine tree.
# These are the complete canonical actor declarations and their native closure.
set(_actor_public_sdk "${CMAKE_CURRENT_BINARY_DIR}/public-sdk")
set(_actor_headers
    GameObject.h Model.h CubeObject.h PlaneObject.h SphereObject.h WallObject.h
    Mesh.h MeshTypes.h NativeTypes.h
    Detail/PlatformTypes.h Detail/PlatformDirectXMathStubs.h Detail/PlatformD3DStubs.h)
foreach(_actor_header IN LISTS _actor_headers)
    if(NOT EXISTS "${_actor_sdk_source}/Spark/${_actor_header}")
        message(FATAL_ERROR "Missing canonical actor SDK header: Spark/${_actor_header}")
    endif()
    configure_file("${_actor_sdk_source}/Spark/${_actor_header}"
        "${_actor_public_sdk}/Spark/${_actor_header}" COPYONLY)
endforeach()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/verify_public_boundary.py"
        --sdk "${_actor_public_sdk}"
    RESULT_VARIABLE _actor_boundary_result)
if(NOT _actor_boundary_result EQUAL 0)
    message(FATAL_ERROR "Actor SDK closure contains a private or unresolved repository include")
endif()

function(actor_public_consumer target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    # Replace any inherited directory-level include paths as well as avoiding
    # engine target include propagation. LINK_ONLY below is intentional.
    set_property(TARGET ${target} PROPERTY INCLUDE_DIRECTORIES "${_actor_public_sdk}")
    target_compile_definitions(${target} PRIVATE
        "$<TARGET_PROPERTY:${_actor_engine},INTERFACE_COMPILE_DEFINITIONS>")
    target_compile_options(${target} PRIVATE
        "$<TARGET_PROPERTY:${_actor_engine},INTERFACE_COMPILE_OPTIONS>")
    if(_actor_platform_dependency)
        target_link_libraries(${target} PRIVATE ${_actor_platform_dependency})
    endif()
    if(MSVC)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        target_compile_options(${target} PRIVATE /Zc:preprocessor /Zc:__cplusplus)
    endif()
endfunction()

add_library(spark_legacy_actor_module MODULE actor_module.cpp)
actor_public_consumer(spark_legacy_actor_module)
target_link_libraries(spark_legacy_actor_module PRIVATE "$<LINK_ONLY:${_actor_engine}>")
set_target_properties(spark_legacy_actor_module PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(spark_legacy_actor_module PRIVATE -fno-gnu-unique)
endif()

add_executable(spark_legacy_actor_host actor_host.cpp)
actor_public_consumer(spark_legacy_actor_host)
# The host uses inline accessors and virtual dispatch into the DLL. Linking no
# engine archive here makes accidental test-side replacement implementations
# unable to satisfy the module's required real engine symbols.
target_link_libraries(spark_legacy_actor_host PRIVATE ${CMAKE_DL_LIBS})
# UBSan's vptr check references the typeinfo of every polymorphic type the host
# touches, and typeinfo for GameObject is emitted only in the engine archive the
# host deliberately does not link. Drop that one check here; the module, which
# links the real engine, keeps it.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*undefined")
    target_compile_options(spark_legacy_actor_host PRIVATE -fno-sanitize=vptr)
endif()
if(WIN32)
    target_link_libraries(spark_legacy_actor_host PRIVATE d3d11)
endif()
add_dependencies(spark_legacy_actor_host spark_legacy_actor_module)

# Each public entry header must also compile first, without another actor header
# accidentally supplying a missing dependency.
set(_actor_header_units "")
foreach(_actor_header IN ITEMS GameObject Model CubeObject PlaneObject SphereObject WallObject Mesh MeshTypes NativeTypes)
    set(_actor_unit "${CMAKE_CURRENT_BINARY_DIR}/header-${_actor_header}.cpp")
    file(WRITE "${_actor_unit}" "#include <Spark/${_actor_header}.h>\n")
    list(APPEND _actor_header_units "${_actor_unit}")
endforeach()
add_library(spark_legacy_actor_header_checks OBJECT ${_actor_header_units})
actor_public_consumer(spark_legacy_actor_header_checks)
add_dependencies(spark_legacy_actor_host spark_legacy_actor_header_checks)

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/actor-includes-$<CONFIG>.txt"
    CONTENT "$<TARGET_PROPERTY:spark_legacy_actor_module,INCLUDE_DIRECTORIES>\n$<TARGET_PROPERTY:spark_legacy_actor_host,INCLUDE_DIRECTORIES>\n$<TARGET_PROPERTY:spark_legacy_actor_header_checks,INCLUDE_DIRECTORIES>\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/actor-system-includes-$<CONFIG>.txt"
    CONTENT "${_actor_system_includes}\n")

enable_testing()
add_test(NAME SparkLegacyActorPublicSDK
    COMMAND spark_legacy_actor_host "$<TARGET_FILE:spark_legacy_actor_module>")
add_test(NAME SparkLegacyActorPublicSDKBoundary
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/verify_public_boundary.py"
        --sdk "${_actor_public_sdk}"
        --usage-file "${CMAKE_CURRENT_BINARY_DIR}/actor-includes-$<CONFIG>.txt"
        --system-includes-file "${CMAKE_CURRENT_BINARY_DIR}/actor-system-includes-$<CONFIG>.txt")
add_test(NAME SparkLegacyActorPublicSDKBoundaryRejectsPrivate
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/verify_public_boundary.py" --self-test)
set_tests_properties(SparkLegacyActorPublicSDK SparkLegacyActorPublicSDKBoundary
    SparkLegacyActorPublicSDKBoundaryRejectsPrivate PROPERTIES
    TIMEOUT 60 LABELS "sdk;module;fps;actor;integration")
