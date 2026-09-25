# SparkCpuFloor.cmake -- stable-v1 x86-64 CPU floor (BLD-100, owner decision OD-04).
#
# The stable-v1 CPU floor is x86-64 with SSE4.2 (the x86-64-v2 level: SSE3,
# SSSE3, SSE4.1, SSE4.2 and POPCNT). AVX, AVX2, AVX-512, FMA, F16C, LZCNT and
# BMI (TZCNT) are above the floor, as are MOVBE, AES-NI, PCLMULQDQ, SHA-NI,
# GFNI, RDRAND, RDSEED and ADX: a binary compiled with them faults with an
# illegal instruction on a supported CPU, or -- for LZCNT, which older CPUs
# decode as BSR -- silently computes wrong results.
#
# Vendored Jolt defaults to USE_AVX2/USE_AVX/USE_LZCNT/USE_TZCNT/USE_F16C/
# USE_FMADD=ON and publishes the matching -mavx2 -mfma ... (or /arch:AVX2)
# options PUBLIC, so every target linking Jolt inherited an AVX2 requirement.
# spark_configure_jolt_cpu_floor() pins Jolt to the floor before its
# add_subdirectory(); spark_assert_cpu_floor() runs at the end of configuration
# and rejects any target or global flag that still selects an instruction set
# above the floor.
#
# SPARK_NATIVE_ARCH=ON is the only escape hatch: it tunes for the build host and
# is documented as unsuitable for distribution, so the floor is not enforced
# there. The windows-shipping and linux-shipping presets pin it OFF and
# Tools/buildmatrix/check_parity.py rejects a distributed profile that enables
# it. Per-source-file COMPILE_OPTIONS are not scanned; runtime-dispatched ISA
# variants must be selected by CPUID (see Utils/MultiISA.h), never assumed.
#
# Tests/Tools/test_cpu_floor.py exercises both functions against real CMake
# configurations, including the vendored Jolt build.

include_guard(GLOBAL)

# True when the configured target is x86/x86-64 and the build is not tuned for
# the build host, i.e. when the stable-v1 CPU floor applies.
function(spark_cpu_floor_enforced out_var)
    set(_enforced FALSE)
    if(NOT SPARK_NATIVE_ARCH AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x86|X86|i[3-6]86)$")
        set(_enforced TRUE)
    endif()
    set(${out_var} ${_enforced} PARENT_SCOPE)
endfunction()

# Append every entry of the remaining arguments that selects an instruction set
# above the floor to the list named by out_var. Entries may be raw compiler
# flags, preprocessor definitions or generator expressions wrapping either;
# matching is textual, so a flag hidden behind a $<CONFIG:...> guard is still
# reported because some configuration would compile with it.
function(spark_cpu_floor_violations out_var)
    set(_violations "")
    foreach(_entry IN LISTS ARGN)
        set(_bad FALSE)
        # GCC/Clang ISA extensions above x86-64-v2 (-mavx covers -mavx2, -mavx512*
        # and -mavxvnni; -mfma covers -mfma4; -mbmi covers -mbmi2).
        if(_entry MATCHES "(^|[^A-Za-z0-9_])-m(avx|fma|f16c|lzcnt|bmi)")
            set(_bad TRUE)
        endif()
        # Legacy-encoded extensions that are also above x86-64-v2 (MOVBE is v3;
        # AES-NI, PCLMULQDQ, SHA-NI, GFNI, RDRAND, RDSEED and ADX are in no x86-64
        # level at or below the floor). Whole-flag match: -msha must not catch -mshstk.
        if(_entry MATCHES "(^|[^A-Za-z0-9_])-m(movbe|aes|pclmul|sha|gfni|rdrnd|rdseed|adx|vaes|vpclmulqdq)($|[^A-Za-z0-9_])")
            set(_bad TRUE)
        endif()
        # -march=/-mcpu= may only name the generic floor levels.
        if(_entry MATCHES "-m(arch|cpu)=([A-Za-z0-9_.-]+)")
            if(NOT CMAKE_MATCH_2 MATCHES "^(x86-64|x86-64-v2)$")
                set(_bad TRUE)
            endif()
        endif()
        # MSVC /arch:AVX, /arch:AVX2, /arch:AVX512, /arch:AVX10.x (either spelling).
        if(_entry MATCHES "[/-][Aa][Rr][Cc][Hh]:[Aa][Vv][Xx]")
            set(_bad TRUE)
        endif()
        # Jolt instruction-set selectors; its headers emit the matching intrinsics.
        if(_entry MATCHES "JPH_USE_(AVX|LZCNT|TZCNT|F16C|FMADD)")
            set(_bad TRUE)
        endif()
        if(_bad)
            list(APPEND _violations "${_entry}")
        endif()
    endforeach()
    set(${out_var} "${_violations}" PARENT_SCOPE)
endfunction()

# Pin vendored Jolt's x86 instruction-set options. Must be called before
# add_subdirectory() of Jolt's Build directory. FORCE is required because Jolt's
# option() defaults would otherwise win on a fresh cache and a stale cache would
# keep whatever a previous configuration chose.
function(spark_configure_jolt_cpu_floor)
    spark_cpu_floor_enforced(_enforced)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x86|X86|i[3-6]86)$")
        return()
    endif()
    if(_enforced)
        set(_above_floor OFF)
        message(STATUS "Jolt Physics: x86-64 SSE4.2 CPU floor (no AVX/AVX2/FMA/F16C/LZCNT/TZCNT)")
    else()
        # SPARK_NATIVE_ARCH=ON: restore Jolt's upstream defaults so switching the
        # option back and forth never leaves a stale floor or a stale AVX2 cache.
        set(_above_floor ON)
        message(STATUS "Jolt Physics: SPARK_NATIVE_ARCH=ON, using Jolt's AVX2 instruction-set defaults")
    endif()
    set(USE_SSE4_1 ON CACHE BOOL "Enable SSE4.1" FORCE)
    set(USE_SSE4_2 ON CACHE BOOL "Enable SSE4.2" FORCE)
    set(USE_AVX ${_above_floor} CACHE BOOL "Enable AVX" FORCE)
    set(USE_AVX2 ${_above_floor} CACHE BOOL "Enable AVX2" FORCE)
    set(USE_AVX512 OFF CACHE BOOL "Enable AVX512" FORCE)
    set(USE_LZCNT ${_above_floor} CACHE BOOL "Enable LZCNT" FORCE)
    set(USE_TZCNT ${_above_floor} CACHE BOOL "Enable TZCNT" FORCE)
    set(USE_F16C ${_above_floor} CACHE BOOL "Enable F16C" FORCE)
    set(USE_FMADD ${_above_floor} CACHE BOOL "Enable FMADD" FORCE)
endfunction()

function(_spark_cpu_floor_collect_targets dir out_var)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        _spark_cpu_floor_collect_targets("${_subdir}" _nested)
        list(APPEND _targets ${_nested})
    endforeach()
    set(${out_var} "${_targets}" PARENT_SCOPE)
endfunction()

# Fail configuration when any global flag or any target declared anywhere in the
# build tree requires an instruction set above the floor. Call once, after every
# add_subdirectory().
function(spark_assert_cpu_floor)
    spark_cpu_floor_enforced(_enforced)
    if(NOT _enforced)
        return()
    endif()

    set(_findings "")
    set(_configs Debug Release MinSizeRel RelWithDebInfo ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    list(REMOVE_DUPLICATES _configs)
    foreach(_lang IN ITEMS C CXX)
        set(_flag_vars CMAKE_${_lang}_FLAGS)
        foreach(_config IN LISTS _configs)
            string(TOUPPER "${_config}" _config_upper)
            list(APPEND _flag_vars CMAKE_${_lang}_FLAGS_${_config_upper})
        endforeach()
        foreach(_flag_var IN LISTS _flag_vars)
            separate_arguments(_flags NATIVE_COMMAND "${${_flag_var}}")
            spark_cpu_floor_violations(_bad ${_flags})
            foreach(_flag IN LISTS _bad)
                list(APPEND _findings "${_flag_var}: ${_flag}")
            endforeach()
        endforeach()
    endforeach()

    _spark_cpu_floor_collect_targets("${CMAKE_SOURCE_DIR}" _targets)
    foreach(_target IN LISTS _targets)
        foreach(_property IN ITEMS COMPILE_OPTIONS INTERFACE_COMPILE_OPTIONS COMPILE_DEFINITIONS
                                   INTERFACE_COMPILE_DEFINITIONS COMPILE_FLAGS)
            get_target_property(_values ${_target} ${_property})
            if(NOT _values)
                continue()
            endif()
            if(_property STREQUAL "COMPILE_FLAGS")
                separate_arguments(_values NATIVE_COMMAND "${_values}")
            endif()
            spark_cpu_floor_violations(_bad ${_values})
            foreach(_value IN LISTS _bad)
                list(APPEND _findings "${_target} ${_property}: ${_value}")
            endforeach()
        endforeach()
    endforeach()

    if(_findings)
        list(JOIN _findings "\n  " _report)
        message(FATAL_ERROR
            "BLD-100/OD-04: the stable-v1 CPU floor is x86-64 SSE4.2 (x86-64-v2), but this configuration "
            "selects instruction sets above it:\n  ${_report}\n"
            "Remove the flag, gate it behind CPUID runtime dispatch, or configure with SPARK_NATIVE_ARCH=ON "
            "for a non-distributable host-tuned build.")
    endif()
    list(LENGTH _targets _target_count)
    message(STATUS "CPU floor: x86-64 SSE4.2 verified for global flags and ${_target_count} targets")
endfunction()
