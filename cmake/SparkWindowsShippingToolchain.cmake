# Build-input policy only; successful configuration is not release certification.
# Include after project() so every comparison uses CMake's measured identities.
option(SPARK_ENFORCE_WINDOWS_SHIPPING_TOOLCHAIN "Require the locked stable-v1 Windows Shipping toolchain" OFF)

function(spark_validate_windows_shipping_toolchain)
    set(_policy_path "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WindowsShippingToolchain.json")
    if(NOT EXISTS "${_policy_path}" OR IS_DIRECTORY "${_policy_path}")
        message(FATAL_ERROR "Windows Shipping toolchain policy is missing: ${_policy_path}")
    endif()
    file(SIZE "${_policy_path}" _policy_size)
    if(_policy_size GREATER 16384)
        message(FATAL_ERROR "Windows Shipping toolchain policy exceeds 16 KiB")
    endif()
    file(READ "${_policy_path}" _policy)
    set(_keys schemaVersion profile platform configuration generator architecture platformToolset
        hostArchitecture msvcToolsVersion compilerId compilerVersion windowsSdkVersion)
    string(JSON _count ERROR_VARIABLE _error LENGTH "${_policy}")
    # CMake's JSON reader collapses duplicate keys; also count the raw flat keys.
    string(REGEX MATCHALL "\"[^\"]*\"[ \t\r\n]*:" _raw_keys "${_policy}")
    list(LENGTH _raw_keys _raw_count)
    if(_error OR NOT _count EQUAL 12 OR NOT _raw_count EQUAL 12)
        message(FATAL_ERROR "Windows Shipping toolchain policy does not match its closed schema")
    endif()
    foreach(_key IN LISTS _keys)
        string(JSON _type ERROR_VARIABLE _error TYPE "${_policy}" "${_key}")
        if(_error OR NOT _type STREQUAL "STRING")
            message(FATAL_ERROR "Windows Shipping toolchain policy ${_key} must be a string")
        endif()
        string(JSON _value GET "${_policy}" "${_key}")
        set(_locked_${_key} "${_value}")
    endforeach()
    foreach(_pair IN ITEMS "schemaVersion|spark-windows-shipping-toolchain-v1" "profile|stable-v1"
            "platform|Windows" "configuration|MinSizeRel" "generator|Visual Studio 17 2022"
            "architecture|x64" "platformToolset|v143" "hostArchitecture|x64" "compilerId|MSVC")
        string(REPLACE "|" ";" _parts "${_pair}")
        list(GET _parts 0 _key)
        list(GET _parts 1 _expected)
        if(NOT "${_locked_${_key}}" STREQUAL "${_expected}")
            message(FATAL_ERROR "Windows Shipping toolchain policy ${_key} must be ${_expected}")
        endif()
    endforeach()
    if(NOT _locked_msvcToolsVersion MATCHES "^14[.][0-9]+[.][0-9]+$"
            OR NOT _locked_compilerVersion MATCHES "^19[.][0-9]+[.][0-9]+[.][0-9]+$"
            OR NOT _locked_windowsSdkVersion MATCHES "^10[.]0[.][0-9]+[.][0-9]+$")
        message(FATAL_ERROR "Windows Shipping toolchain policy version is malformed")
    endif()

    # Never assign CMAKE_GENERATOR_TOOLSET here: presets select it before project().
    set(_selector "${_locked_platformToolset},host=${_locked_hostArchitecture},version=${_locked_msvcToolsVersion}")
    set(_checks
        "CMAKE_SYSTEM_NAME|${_locked_platform}"
        "CMAKE_BUILD_TYPE|${_locked_configuration}"
        "CMAKE_GENERATOR|${_locked_generator}"
        "CMAKE_GENERATOR_PLATFORM|${_locked_architecture}"
        "CMAKE_GENERATOR_TOOLSET|${_selector}"
        "CMAKE_VS_PLATFORM_TOOLSET|${_locked_platformToolset}"
        "CMAKE_VS_PLATFORM_TOOLSET_VERSION|${_locked_msvcToolsVersion}"
        "CMAKE_VS_PLATFORM_TOOLSET_HOST_ARCHITECTURE|${_locked_hostArchitecture}"
        "CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION|${_locked_windowsSdkVersion}")
    foreach(_language IN ITEMS C CXX)
        list(APPEND _checks "CMAKE_${_language}_COMPILER_ID|${_locked_compilerId}"
            "CMAKE_${_language}_COMPILER_VERSION|${_locked_compilerVersion}"
            "CMAKE_${_language}_COMPILER_ARCHITECTURE_ID|${_locked_architecture}")
    endforeach()
    foreach(_pair IN LISTS _checks)
        string(REPLACE "|" ";" _parts "${_pair}")
        list(GET _parts 0 _key)
        list(GET _parts 1 _expected)
        if(NOT "${${_key}}" STREQUAL "${_expected}")
            message(FATAL_ERROR "Windows Shipping toolchain mismatch: ${_key}='${${_key}}', requires '${_expected}'")
        endif()
    endforeach()
    if(NOT CMAKE_GENERATOR_INSTANCE MATCHES "^([A-Za-z]:[/\\]|[/\\][/\\])")
        message(FATAL_ERROR "Windows Shipping toolchain requires an absolute Visual Studio instance")
    endif()
    string(REPLACE "\\" "/" _instance "${CMAKE_GENERATOR_INSTANCE}")
    string(REGEX REPLACE "/+$" "" _instance "${_instance}")
    set(_bin "${_instance}/VC/Tools/MSVC/${_locked_msvcToolsVersion}/bin/Host${_locked_hostArchitecture}/${_locked_architecture}")
    foreach(_pair IN ITEMS "CMAKE_C_COMPILER|cl.exe" "CMAKE_CXX_COMPILER|cl.exe"
            "CMAKE_LINKER|link.exe" "CMAKE_AR|lib.exe")
        string(REPLACE "|" ";" _parts "${_pair}")
        list(GET _parts 0 _key)
        list(GET _parts 1 _executable)
        string(REPLACE "\\" "/" _actual "${${_key}}")
        string(TOLOWER "${_actual}" _actual)
        string(TOLOWER "${_bin}/${_executable}" _expected)
        if(NOT _actual STREQUAL _expected)
            message(FATAL_ERROR "Windows Shipping toolchain mismatch: ${_key}='${${_key}}', requires '${_bin}/${_executable}'")
        endif()
    endforeach()
endfunction()

if(SPARK_ENFORCE_WINDOWS_SHIPPING_TOOLCHAIN)
    spark_validate_windows_shipping_toolchain()
endif()
