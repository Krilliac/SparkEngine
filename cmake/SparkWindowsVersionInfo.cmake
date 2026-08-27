include_guard(GLOBAL)

set_property(GLOBAL PROPERTY SPARK_WINDOWS_VERSIONINFO_MODULE_DIR
    "${CMAKE_CURRENT_LIST_DIR}")

function(spark_resolve_engine_version output_variable)
    if(DEFINED SPARK_ENGINE_VERSION AND NOT SPARK_ENGINE_VERSION STREQUAL "")
        set(_spark_engine_version "${SPARK_ENGINE_VERSION}")
    else()
        get_property(_spark_version_module_dir GLOBAL
            PROPERTY SPARK_WINDOWS_VERSIONINFO_MODULE_DIR)
        set(_spark_root_cmake "${_spark_version_module_dir}/../CMakeLists.txt")
        file(STRINGS "${_spark_root_cmake}" _spark_version_declaration
            REGEX "^set\\(SPARK_ENGINE_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+\" CACHE STRING")
        list(LENGTH _spark_version_declaration _spark_version_declaration_count)
        if(NOT _spark_version_declaration_count EQUAL 1)
            message(FATAL_ERROR
                "Expected one SPARK_ENGINE_VERSION declaration in ${_spark_root_cmake}")
        endif()
        string(REGEX REPLACE
            "^set\\(SPARK_ENGINE_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\".*$"
            "\\1" _spark_engine_version "${_spark_version_declaration}")
    endif()

    if(NOT _spark_engine_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR
            "SPARK_ENGINE_VERSION must be MAJOR.MINOR.PATCH; got '${_spark_engine_version}'")
    endif()

    set(${output_variable} "${_spark_engine_version}" PARENT_SCOPE)
endfunction()

function(spark_generate_windows_version_resource target_name output_resource)
    if(NOT target_name MATCHES "^[A-Za-z0-9_.+-]+$")
        message(FATAL_ERROR "Invalid Windows version-resource target name '${target_name}'")
    endif()
    spark_resolve_engine_version(_spark_engine_version)
    string(REPLACE "." ";" _spark_version_components "${_spark_engine_version}")
    list(GET _spark_version_components 0 SPARK_VERSION_MAJOR)
    list(GET _spark_version_components 1 SPARK_VERSION_MINOR)
    list(GET _spark_version_components 2 SPARK_VERSION_PATCH)
    set(SPARK_VERSION_STRING "${_spark_engine_version}.0")
    set(SPARK_VERSION_INTERNAL_NAME "${target_name}")
    set(SPARK_VERSION_ORIGINAL_FILENAME "${target_name}.exe")
    set(SPARK_VERSION_FILE_DESCRIPTION "${target_name} - SparkEngine executable")
    set(SPARK_VERSION_PRODUCT_NAME "SparkEngine")
    if(target_name STREQUAL "SparkBuild")
        set(SPARK_VERSION_FILE_DESCRIPTION "SparkBuild - Cross-Platform Build Tool")
        set(SPARK_VERSION_PRODUCT_NAME "SparkBuild")
    elseif(target_name STREQUAL "SparkInstaller")
        set(SPARK_VERSION_FILE_DESCRIPTION
            "SparkInstaller - SparkEngine Bootstrap Installer/Updater")
        set(SPARK_VERSION_PRODUCT_NAME "SparkInstaller")
    endif()

    get_property(_spark_version_module_dir GLOBAL
        PROPERTY SPARK_WINDOWS_VERSIONINFO_MODULE_DIR)
    get_filename_component(_spark_version_output_dir "${output_resource}" DIRECTORY)
    file(MAKE_DIRECTORY "${_spark_version_output_dir}")
    configure_file(
        "${_spark_version_module_dir}/SparkWindowsVersionInfo.rc.in"
        "${output_resource}"
        @ONLY)
endfunction()

function(spark_target_windows_version_info target_name)
    if(NOT WIN32)
        return()
    endif()
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "Cannot add Windows version information to unknown target '${target_name}'")
    endif()

    get_target_property(_spark_target_type "${target_name}" TYPE)
    if(NOT _spark_target_type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR
            "Windows version information requires an executable target; "
            "${target_name} is ${_spark_target_type}")
    endif()

    get_target_property(_spark_version_attached "${target_name}"
        SPARK_WINDOWS_VERSIONINFO_ATTACHED)
    if(_spark_version_attached)
        return()
    endif()

    set(_spark_version_resource
        "${CMAKE_CURRENT_BINARY_DIR}/spark-windows-version-info/${target_name}VersionInfo.rc")
    spark_generate_windows_version_resource(
        "${target_name}" "${_spark_version_resource}")

    target_sources("${target_name}" PRIVATE "${_spark_version_resource}")
    source_group("Resource Files" FILES "${_spark_version_resource}")
    set_property(TARGET "${target_name}"
        PROPERTY SPARK_WINDOWS_VERSIONINFO_ATTACHED TRUE)
endfunction()

function(spark_attach_shipped_windows_version_info)
    set(_spark_shipped_executables
        SparkEngine
        SparkConsole
        SparkEditor
        SparkLauncher
        SparkServer
        SparkGateway
        SparkDaemon
        SparkCollabServer
        SparkOrchestrator
        SparkCooker
        SparkWorker
        SparkAutomation
        SparkBuild
        SparkInstaller
        SparkShaderCompiler
        SparkCrashReporter)
    foreach(_spark_executable IN LISTS _spark_shipped_executables)
        if(TARGET "${_spark_executable}")
            spark_target_windows_version_info("${_spark_executable}")
        endif()
    endforeach()
endfunction()
