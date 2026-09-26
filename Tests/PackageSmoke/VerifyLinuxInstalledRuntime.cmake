cmake_minimum_required(VERSION 3.25)

# PLT-210: installed Linux tree runtime-closure verification, run with `cmake -P`.
#
# It proves that an installed Linux SparkEngine tree needs nothing from the
# repository or the toolchain at runtime:
#
#   1. Install mode: `cmake --install` SPARK_ENGINE_BUILD_DIR into a fresh
#      <SPARK_TEST_ROOT>/prefix. Standalone mode: verify an existing
#      SPARK_INSTALL_PREFIX read-only, for example a linux-shipping install
#      (that preset sets BUILD_TESTS=OFF, so it has no CTest of its own).
#   2. For every ELF file in the prefix (readelf -d): each RUNPATH/RPATH entry
#      must be $ORIGIN-relative and stay inside the prefix; an absolute entry,
#      an empty entry or one that escapes the prefix fails.
#   3. For every ELF with NEEDED entries (ldd, empty environment): every library
#      in the closure must resolve, never into the source or build tree, and
#      either inside the prefix or into the host's system library directories.
#      A soname the package ships in <prefix>/lib must resolve to that copy.
#      Every symlink must stay inside the prefix; every .sparkabi sidecar must
#      hash its installed module image (ModuleManager rejects a mismatch).
#   4. Launch the installed SparkEngine -headless with the installed
#      SparkGameFPS from cwd=/ with an empty environment and fresh HOME/XDG
#      directories, parse its lifecycle records with the shared NullRHI parser,
#      and fail if its output names the source/build tree, the run added,
#      removed or changed the content of any entry in the prefix (SHA256 of
#      every file, symlink targets), or it created a new top-level entry in its
#      working directory /. Writes deeper inside / (for example /var) are not
#      observed; TMPDIR and HOME point into the test root.
#
# On a pass, install mode removes the installed prefix and the fresh HOME/TMPDIR
# and keeps only the report and logs; the self test removes each fixture
# prefix once its case passes. On a failure everything is kept for diagnosis.
#
# "System library" is host-specific. The report written to
# <SPARK_TEST_ROOT>/runtime-closure-report.txt records the host glibc and
# libstdc++ and every library resolved outside the prefix; it is evidence for
# this host only, not a distribution range.
#
# Required: SPARK_SOURCE_ROOT SPARK_TEST_ROOT and one of
#           SPARK_ENGINE_BUILD_DIR (install mode) or SPARK_INSTALL_PREFIX.
# Optional: SPARK_CONFIG (install --config), SPARK_FORBIDDEN_ROOTS (extra
#           build trees, e.g. build/linux-shipping in standalone mode).
# Self test: -DSPARK_LINUX_RUNTIME_CLOSURE_SELF_TEST=ON with
#           SPARK_FIXTURE_ENGINE (a build-tree SparkEngine, absolute RUNPATH)
#           and SPARK_FIXTURE_MODULE (a build-tree module with its .sparkabi),
#           SPARK_FORBIDDEN_ROOTS (their build tree) and optional
#           SPARK_SOURCE_ROOT proves each rule rejects a real defective image.
#
# Standalone example (from the source root):
#   cmake -DSPARK_SOURCE_ROOT=$PWD -DSPARK_INSTALL_PREFIX=/tmp/spark-ship \
#         -DSPARK_FORBIDDEN_ROOTS=$PWD/build/linux-shipping \
#         -DSPARK_TEST_ROOT=/tmp/spark-ship-verify \
#         -P Tests/PackageSmoke/VerifyLinuxInstalledRuntime.cmake

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "VerifyLinuxInstalledRuntime.cmake verifies Linux ELF installs only")
endif()

find_program(SPARK_READELF NAMES readelf REQUIRED)
find_program(SPARK_LDD NAMES ldd REQUIRED)
find_program(SPARK_ENV NAMES env REQUIRED)
find_program(SPARK_LDCONFIG NAMES ldconfig PATHS /sbin /usr/sbin)

# A minimal environment for every child: nothing from the caller (for example
# LD_LIBRARY_PATH, LD_PRELOAD or a SPARK_* override) can influence resolution.
set(_spark_clean_env "${SPARK_ENV}" -i "PATH=/usr/bin:/bin" "LC_ALL=C")

# TRUE when PATH equals ROOT or lies below it (lexical, after normalization).
function(_spark_path_is_under path root out_var)
    cmake_path(SET _path NORMALIZE "${path}")
    cmake_path(SET _root NORMALIZE "${root}")
    string(REGEX REPLACE "/+$" "" _path "${_path}")
    string(REGEX REPLACE "/+$" "" _root "${_root}")
    set(_under FALSE)
    if(NOT "${_root}" STREQUAL "")
        # Compare "<path>/" against "<root>/" so /a/bc is never under /a/b.
        string(LENGTH "${_root}/" _root_length)
        string(SUBSTRING "${_path}/" 0 ${_root_length} _head)
        if("${_head}" STREQUAL "${_root}/")
            set(_under TRUE)
        endif()
    endif()
    set(${out_var} ${_under} PARENT_SCOPE)
endfunction()

# TRUE when PATH, lexically or after resolving symlinks, lies under any ROOT.
function(_spark_path_is_under_any path out_var)
    set(_candidates "${path}")
    if(EXISTS "${path}")
        file(REAL_PATH "${path}" _real)
        list(APPEND _candidates "${_real}")
    endif()
    foreach(_candidate IN LISTS _candidates)
        foreach(_root IN LISTS ARGN)
            if("${_root}" STREQUAL "")
                continue()
            endif()
            set(_roots "${_root}")
            if(EXISTS "${_root}")
                file(REAL_PATH "${_root}" _real_root)
                list(APPEND _roots "${_real_root}")
            endif()
            foreach(_each IN LISTS _roots)
                _spark_path_is_under("${_candidate}" "${_each}" _under)
                if(_under)
                    set(${out_var} TRUE PARENT_SCOPE)
                    return()
                endif()
            endforeach()
        endforeach()
    endforeach()
    set(${out_var} FALSE PARENT_SCOPE)
endfunction()

# Classifies one resolved closure entry. RESOLVED is empty for "not found".
# Sets OUT_VIOLATION to a message (empty when accepted) and OUT_KIND to
# prefix/system for an accepted entry.
function(_spark_classify_resolution)
    cmake_parse_arguments(ARG "" "SONAME;RESOLVED;PREFIX;OUT_VIOLATION;OUT_KIND"
        "FORBIDDEN_ROOTS;SYSTEM_DIRS;BUNDLED_SONAMES" ${ARGN})
    set(_violation "")
    set(_kind "")
    if("${ARG_RESOLVED}" STREQUAL "")
        set(_violation "${ARG_SONAME} is not found by the dynamic loader")
    else()
        _spark_path_is_under_any("${ARG_RESOLVED}" _in_prefix "${ARG_PREFIX}")
        _spark_path_is_under_any("${ARG_RESOLVED}" _in_forbidden ${ARG_FORBIDDEN_ROOTS})
        _spark_path_is_under_any("${ARG_RESOLVED}" _in_system ${ARG_SYSTEM_DIRS})
        if(_in_prefix)
            set(_kind prefix)
        elseif(_in_forbidden)
            set(_violation "${ARG_SONAME} resolves into the source/build tree: ${ARG_RESOLVED}")
        elseif(ARG_SONAME IN_LIST ARG_BUNDLED_SONAMES)
            set(_violation "${ARG_SONAME} is shipped by the package but resolves outside the prefix: ${ARG_RESOLVED}")
        elseif(_in_system)
            set(_kind system)
        else()
            set(_violation
                "${ARG_SONAME} resolves outside the prefix and the host system library directories: ${ARG_RESOLVED}")
        endif()
    endif()
    set(${ARG_OUT_VIOLATION} "${_violation}" PARENT_SCOPE)
    set(${ARG_OUT_KIND} "${_kind}" PARENT_SCOPE)
endfunction()

# Parses ldd output into parallel lists of sonames and resolved paths ("" when
# not found). The vDSO and "statically linked" lines carry no file and are
# skipped.
function(_spark_parse_ldd text out_sonames out_paths)
    set(_sonames)
    set(_paths)
    string(REPLACE ";" "\\;" text "${text}")
    string(REPLACE "\n" ";" _lines "${text}")
    foreach(_line IN LISTS _lines)
        string(STRIP "${_line}" _line)
        if(_line MATCHES "^([^ ]+) => not found$")
            list(APPEND _sonames "${CMAKE_MATCH_1}")
            list(APPEND _paths "<not-found>")
        elseif(_line MATCHES "^([^ ]+) => (/[^ ]+) \\(0x[0-9a-f]+\\)$")
            list(APPEND _sonames "${CMAKE_MATCH_1}")
            list(APPEND _paths "${CMAKE_MATCH_2}")
        elseif(_line MATCHES "^(/[^ ]+) \\(0x[0-9a-f]+\\)$")
            set(_path "${CMAKE_MATCH_1}")
            cmake_path(GET _path FILENAME _name)
            list(APPEND _sonames "${_name}")
            list(APPEND _paths "${_path}")
        endif()
    endforeach()
    set(${out_sonames} "${_sonames}" PARENT_SCOPE)
    set(${out_paths} "${_paths}" PARENT_SCOPE)
endfunction()

# The directories the host dynamic loader treats as system locations: the
# multilib defaults plus every directory in the ldconfig cache.
function(_spark_system_library_dirs out_var)
    set(_dirs /lib /lib64 /lib32 /usr/lib /usr/lib64 /usr/lib32)
    if(SPARK_LDCONFIG)
        execute_process(COMMAND ${_spark_clean_env} "${SPARK_LDCONFIG}" -p
            RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_QUIET TIMEOUT 60)
        if(_result EQUAL 0)
            string(REGEX MATCHALL "=> (/[^\n]+)" _entries "${_output}")
            foreach(_entry IN LISTS _entries)
                string(REGEX REPLACE "^=> " "" _entry "${_entry}")
                cmake_path(GET _entry PARENT_PATH _dir)
                list(APPEND _dirs "${_dir}")
            endforeach()
        endif()
    endif()
    list(REMOVE_DUPLICATES _dirs)
    set(${out_var} "${_dirs}" PARENT_SCOPE)
endfunction()

# Content snapshot of a tree: one "<relative path>|<kind>" entry per file
# (kind = SHA256), symlink (kind = its target) and directory, sorted.
function(_spark_tree_snapshot root out_var)
    file(GLOB_RECURSE _entries LIST_DIRECTORIES true RELATIVE "${root}" "${root}/*")
    set(_snapshot)
    foreach(_entry IN LISTS _entries)
        set(_path "${root}/${_entry}")
        if(IS_SYMLINK "${_path}")
            file(READ_SYMLINK "${_path}" _target)
            list(APPEND _snapshot "${_entry}|link:${_target}")
        elseif(IS_DIRECTORY "${_path}")
            list(APPEND _snapshot "${_entry}|dir")
        else()
            file(SHA256 "${_path}" _hash)
            list(APPEND _snapshot "${_entry}|${_hash}")
        endif()
    endforeach()
    list(SORT _snapshot)
    set(${out_var} "${_snapshot}" PARENT_SCOPE)
endfunction()

# Verifies the whole prefix. OUT_VAR receives the violations; REPORT_VAR a
# human-readable resolution report; ELF_COUNT_VAR the number of ELF images.
function(spark_linux_runtime_closure_violations)
    cmake_parse_arguments(ARG "" "PREFIX;OUT_VAR;REPORT_VAR;ELF_COUNT_VAR"
        "FORBIDDEN_ROOTS;BUNDLED_SONAMES" ${ARGN})
    set(_violations)
    set(_external)
    set(_elf_count 0)
    set(_sidecar_count 0)
    file(REAL_PATH "${ARG_PREFIX}" _prefix)
    _spark_system_library_dirs(_system_dirs)

    set(_bundled ${ARG_BUNDLED_SONAMES})
    file(GLOB _shipped_libraries LIST_DIRECTORIES false "${_prefix}/lib/*.so*")
    foreach(_library IN LISTS _shipped_libraries)
        cmake_path(GET _library FILENAME _name)
        list(APPEND _bundled "${_name}")
    endforeach()

    # Directories are listed too: a symlink to a directory is only reported
    # that way, and it must be checked like any other symlink.
    file(GLOB_RECURSE _files LIST_DIRECTORIES true "${_prefix}/*")
    # A ';' in a file name splits the CMake list, so such a file can never be
    # inspected here; it fails instead of being silently skipped.
    execute_process(COMMAND ${_spark_clean_env} find "${_prefix}" -name "*;*"
        RESULT_VARIABLE _find_result OUTPUT_VARIABLE _semicolon_paths ERROR_VARIABLE _find_error TIMEOUT 120)
    if(NOT _find_result EQUAL 0)
        list(APPEND _violations "find failed while scanning the prefix: ${_find_error}")
    elseif(NOT "${_semicolon_paths}" STREQUAL "")
        string(REPLACE ";" "<semicolon>" _semicolon_paths "${_semicolon_paths}")
        string(REPLACE "\n" " " _semicolon_paths "${_semicolon_paths}")
        list(APPEND _violations "file names containing a semicolon cannot be verified: ${_semicolon_paths}")
    endif()
    foreach(_file IN LISTS _files)
        if(IS_DIRECTORY "${_file}" AND NOT IS_SYMLINK "${_file}")
            continue()
        endif()
        if(NOT EXISTS "${_file}" AND NOT IS_SYMLINK "${_file}")
            continue()
        endif()
        file(RELATIVE_PATH _relative "${_prefix}" "${_file}")
        if(IS_SYMLINK "${_file}")
            if(NOT EXISTS "${_file}")
                list(APPEND _violations "${_relative}: symlink target does not exist")
            else()
                file(REAL_PATH "${_file}" _target)
                _spark_path_is_under("${_target}" "${_prefix}" _inside)
                if(NOT _inside)
                    list(APPEND _violations "${_relative}: symlink leaves the prefix (${_target})")
                endif()
            endif()
            continue()
        endif()

        if(_file MATCHES "\\.sparkabi$")
            math(EXPR _sidecar_count "${_sidecar_count} + 1")
            string(REGEX REPLACE "\\.sparkabi$" "" _module "${_file}")
            file(STRINGS "${_file}" _hash_lines REGEX "^binary_sha256=[0-9a-f]+$")
            list(LENGTH _hash_lines _hash_line_count)
            if(NOT EXISTS "${_module}")
                list(APPEND _violations "${_relative}: sidecar has no module image beside it")
            elseif(NOT _hash_line_count EQUAL 1)
                list(APPEND _violations "${_relative}: sidecar has ${_hash_line_count} binary_sha256 lines")
            else()
                string(REGEX REPLACE "^binary_sha256=" "" _expected "${_hash_lines}")
                file(SHA256 "${_module}" _actual)
                if(NOT _actual STREQUAL _expected)
                    string(CONCAT _mismatch
                        "${_relative}: sidecar binary_sha256 ${_expected} does not hash the installed module "
                        "(${_actual}), so ModuleManager rejects it before dlopen")
                    list(APPEND _violations "${_mismatch}")
                endif()
            endif()
            continue()
        endif()

        file(SIZE "${_file}" _size)
        if(_size LESS 4)
            continue()
        endif()
        file(READ "${_file}" _magic LIMIT 4 HEX)
        if(NOT _magic STREQUAL "7f454c46")
            continue()
        endif()
        math(EXPR _elf_count "${_elf_count} + 1")
        cmake_path(GET _file PARENT_PATH _origin)

        execute_process(COMMAND ${_spark_clean_env} "${SPARK_READELF}" -d --wide "${_file}"
            RESULT_VARIABLE _readelf_result OUTPUT_VARIABLE _dynamic ERROR_VARIABLE _readelf_error TIMEOUT 60)
        if(NOT _readelf_result EQUAL 0)
            list(APPEND _violations "${_relative}: readelf -d failed: ${_readelf_error}")
            continue()
        endif()

        string(REGEX MATCHALL "\\((RUNPATH|RPATH)\\)[^\n]*\\[[^]\n]*\\]" _path_records "${_dynamic}")
        foreach(_record IN LISTS _path_records)
            string(REGEX MATCH "^\\((RUNPATH|RPATH)\\)" _unused "${_record}")
            set(_tag "${CMAKE_MATCH_1}")
            string(REGEX REPLACE "^[^[]*\\[(.*)\\]$" "\\1" _value "${_record}")
            string(REPLACE ":" ";" _entries "${_value}")
            if(_value MATCHES "(^|:)(:|$)")
                list(APPEND _violations "${_relative}: ${_tag} '${_value}' has an empty entry")
            endif()
            foreach(_entry IN LISTS _entries)
                if(_entry STREQUAL "")
                    continue()
                endif()
                if(NOT _entry MATCHES "^\\$(ORIGIN|{ORIGIN})(/|$)")
                    _spark_path_is_under_any("${_entry}" _entry_forbidden ${ARG_FORBIDDEN_ROOTS})
                    if(_entry_forbidden)
                        list(APPEND _violations
                            "${_relative}: ${_tag} entry '${_entry}' is an absolute path into the source/build tree")
                    else()
                        list(APPEND _violations "${_relative}: ${_tag} entry '${_entry}' is not $ORIGIN-relative")
                    endif()
                    continue()
                endif()
                string(REGEX REPLACE "^\\$(ORIGIN|{ORIGIN})" "${_origin}" _expanded "${_entry}")
                _spark_path_is_under("${_expanded}" "${_prefix}" _entry_inside)
                if(NOT _entry_inside)
                    list(APPEND _violations "${_relative}: ${_tag} entry '${_entry}' leaves the prefix")
                endif()
            endforeach()
        endforeach()

        string(REGEX MATCHALL "\\(NEEDED\\)[^\n]*\\[[^]\n]+\\]" _needed_records "${_dynamic}")
        if(NOT _needed_records)
            continue()
        endif()
        execute_process(COMMAND ${_spark_clean_env} "${SPARK_LDD}" "${_file}"
            RESULT_VARIABLE _ldd_result OUTPUT_VARIABLE _ldd_output ERROR_VARIABLE _ldd_error TIMEOUT 60)
        if(NOT _ldd_result EQUAL 0)
            list(APPEND _violations "${_relative}: ldd failed (${_ldd_result}): ${_ldd_error}")
            continue()
        endif()
        _spark_parse_ldd("${_ldd_output}" _sonames _paths)
        foreach(_record IN LISTS _needed_records)
            string(REGEX REPLACE "^[^[]*\\[(.*)\\]$" "\\1" _needed "${_record}")
            if(NOT _needed IN_LIST _sonames)
                list(APPEND _violations "${_relative}: NEEDED ${_needed} is missing from the loader's closure")
            endif()
        endforeach()
        list(LENGTH _sonames _closure_size)
        if(_closure_size EQUAL 0)
            continue()
        endif()
        math(EXPR _last "${_closure_size} - 1")
        foreach(_index RANGE ${_last})
            list(GET _sonames ${_index} _soname)
            list(GET _paths ${_index} _resolved)
            if(_resolved STREQUAL "<not-found>")
                set(_resolved "")
            endif()
            _spark_classify_resolution(SONAME "${_soname}" RESOLVED "${_resolved}" PREFIX "${_prefix}"
                FORBIDDEN_ROOTS ${ARG_FORBIDDEN_ROOTS} SYSTEM_DIRS ${_system_dirs} BUNDLED_SONAMES ${_bundled}
                OUT_VIOLATION _violation OUT_KIND _kind)
            if(NOT _violation STREQUAL "")
                list(APPEND _violations "${_relative}: ${_violation}")
            elseif(_kind STREQUAL "system")
                file(REAL_PATH "${_resolved}" _real_resolved)
                list(APPEND _external "${_soname} => ${_real_resolved}")
            endif()
        endforeach()
    endforeach()

    list(REMOVE_DUPLICATES _violations)
    list(REMOVE_DUPLICATES _external)
    list(SORT _external)
    list(JOIN _external "\n  " _external_text)
    set(${ARG_OUT_VAR} "${_violations}" PARENT_SCOPE)
    set(${ARG_ELF_COUNT_VAR} "${_elf_count}" PARENT_SCOPE)
    string(CONCAT _report
        "ELF images: ${_elf_count}\nModule sidecars: ${_sidecar_count}\n"
        "Libraries resolved from host system directories:\n  ${_external_text}\n")
    set(${ARG_REPORT_VAR} "${_report}" PARENT_SCOPE)
endfunction()

if(NOT DEFINED SPARK_TEST_ROOT OR NOT IS_ABSOLUTE "${SPARK_TEST_ROOT}" OR SPARK_TEST_ROOT MATCHES "[\r\n;]")
    message(FATAL_ERROR "SPARK_TEST_ROOT must be a safe absolute path")
endif()
cmake_path(SET _test_root NORMALIZE "${SPARK_TEST_ROOT}")
string(REGEX REPLACE "/+$" "" _test_root "${_test_root}")
if(_test_root STREQUAL "")
    message(FATAL_ERROR "SPARK_TEST_ROOT must not be the filesystem root")
endif()

# ---------------------------------------------------------------------------
# Self test: every rule must reject a real defective image.
# ---------------------------------------------------------------------------
if(SPARK_LINUX_RUNTIME_CLOSURE_SELF_TEST)
    foreach(_required IN ITEMS SPARK_FIXTURE_ENGINE SPARK_FIXTURE_MODULE)
        if(NOT EXISTS "${${_required}}")
            message(FATAL_ERROR "${_required} must name an existing build-tree image")
        endif()
    endforeach()
    if("${SPARK_FORBIDDEN_ROOTS}" STREQUAL "")
        message(FATAL_ERROR "SPARK_FORBIDDEN_ROOTS must name the build tree the fixtures come from")
    endif()
    if(NOT EXISTS "${SPARK_FIXTURE_MODULE}.sparkabi")
        message(FATAL_ERROR "SPARK_FIXTURE_MODULE has no .sparkabi sidecar: ${SPARK_FIXTURE_MODULE}")
    endif()
    file(REMOVE_RECURSE "${_test_root}")

    # The fixtures come from the build tree, so the build tree itself is a
    # forbidden root; each fixture prefix lives under the test root.
    cmake_path(GET SPARK_FIXTURE_ENGINE PARENT_PATH _fixture_bin)
    set(_forbidden ${SPARK_FORBIDDEN_ROOTS})
    list(GET _forbidden 0 _first_forbidden)
    if(DEFINED SPARK_SOURCE_ROOT AND NOT "${SPARK_SOURCE_ROOT}" STREQUAL "")
        list(APPEND _forbidden "${SPARK_SOURCE_ROOT}")
    endif()
    cmake_path(GET SPARK_FIXTURE_MODULE FILENAME _module_name)
    set(_failures)

    function(_spark_expect_closure case_name prefix expected_fragment)
        spark_linux_runtime_closure_violations(PREFIX "${prefix}" OUT_VAR _found REPORT_VAR _unused_report
            ELF_COUNT_VAR _elf_count FORBIDDEN_ROOTS ${_forbidden} BUNDLED_SONAMES libSDL2-2.0.so.0)
        list(JOIN _found "\n    " _found_text)
        set(_case_failure "")
        if(_elf_count EQUAL 0)
            set(_case_failure "${case_name}: no ELF image was scanned")
        elseif("${expected_fragment}" STREQUAL "")
            if(_found)
                set(_case_failure "${case_name}: expected a clean closure, got\n    ${_found_text}")
            endif()
        else()
            string(FIND "${_found_text}" "${expected_fragment}" _position)
            if(_position EQUAL -1)
                set(_case_failure
                    "${case_name}: expected a violation containing '${expected_fragment}', got\n    ${_found_text}")
            endif()
        endif()
        if("${_case_failure}" STREQUAL "")
            # A passing case's staged copy (a full engine image) is not
            # evidence; a failing one is kept for diagnosis.
            file(REMOVE_RECURSE "${prefix}")
        else()
            set(_failures ${_failures} "${_case_failure}" PARENT_SCOPE)
        endif()
    endfunction()

    # Stage the SDL2 the build-tree engine resolves, so the positive control
    # ships its own copy exactly as an install does.
    execute_process(COMMAND ${_spark_clean_env} "${SPARK_LDD}" "${SPARK_FIXTURE_ENGINE}"
        RESULT_VARIABLE _ldd_result OUTPUT_VARIABLE _engine_ldd ERROR_VARIABLE _ldd_error TIMEOUT 60)
    if(NOT _ldd_result EQUAL 0)
        message(FATAL_ERROR "ldd of the fixture engine failed: ${_ldd_error}")
    endif()
    _spark_parse_ldd("${_engine_ldd}" _engine_sonames _engine_paths)
    list(FIND _engine_sonames "libSDL2-2.0.so.0" _sdl_index)
    set(_sdl_path "")
    if(NOT _sdl_index EQUAL -1)
        list(GET _engine_paths ${_sdl_index} _sdl_path)
        if(_sdl_path STREQUAL "<not-found>")
            message(FATAL_ERROR "The build-tree fixture engine cannot resolve its own libSDL2-2.0.so.0")
        endif()
    endif()

    function(_spark_stage_engine prefix runpath with_sdl)
        file(MAKE_DIRECTORY "${prefix}/bin")
        file(COPY_FILE "${SPARK_FIXTURE_ENGINE}" "${prefix}/bin/SparkEngine")
        if(NOT "${runpath}" STREQUAL "<build-tree>")
            file(RPATH_SET FILE "${prefix}/bin/SparkEngine" NEW_RPATH "${runpath}")
        endif()
        if(with_sdl AND NOT _sdl_path STREQUAL "")
            file(MAKE_DIRECTORY "${prefix}/lib")
            file(COPY_FILE "${_sdl_path}" "${prefix}/lib/libSDL2-2.0.so.0")
            file(RPATH_SET FILE "${prefix}/lib/libSDL2-2.0.so.0" NEW_RPATH "$ORIGIN/../lib")
        endif()
    endfunction()

    function(_spark_stage_module prefix)
        file(COPY_FILE "${SPARK_FIXTURE_MODULE}" "${prefix}/bin/${_module_name}")
        file(COPY_FILE "${SPARK_FIXTURE_MODULE}.sparkabi" "${prefix}/bin/${_module_name}.sparkabi")
    endfunction()

    # Positive control: an install-shaped tree must pass, or every negative
    # case below could be passing for the wrong reason.
    _spark_stage_engine("${_test_root}/clean" "$ORIGIN/../lib" TRUE)
    _spark_stage_module("${_test_root}/clean")
    _spark_expect_closure(clean-install-layout "${_test_root}/clean" "")

    # The build-tree engine as-is: its RUNPATH is the absolute build lib/.
    _spark_stage_engine("${_test_root}/build-tree" "<build-tree>" TRUE)
    _spark_expect_closure(build-tree-runpath "${_test_root}/build-tree"
        "is an absolute path into the source/build tree")

    _spark_stage_engine("${_test_root}/absolute" "/opt/spark-host/lib" TRUE)
    _spark_expect_closure(absolute-runpath "${_test_root}/absolute" "is not $ORIGIN-relative")

    _spark_stage_engine("${_test_root}/escape" "$ORIGIN/../../../lib" TRUE)
    _spark_expect_closure(escaping-runpath "${_test_root}/escape" "leaves the prefix")

    _spark_stage_engine("${_test_root}/empty" "$ORIGIN/../lib::" TRUE)
    _spark_expect_closure(empty-runpath-entry "${_test_root}/empty" "has an empty entry")

    # A shipped SDL2 that is missing from the prefix must not silently fall
    # back to whatever the host provides (or to nothing).
    if(NOT _sdl_path STREQUAL "")
        _spark_stage_engine("${_test_root}/missing-sdl" "$ORIGIN/../lib" FALSE)
        _spark_expect_closure(missing-bundled-sdl2 "${_test_root}/missing-sdl" "libSDL2-2.0.so.0")
    endif()

    # The PLT-210 install defect: a module whose RUNPATH changed after its
    # sidecar was written no longer matches binary_sha256.
    _spark_stage_engine("${_test_root}/sidecar" "$ORIGIN/../lib" TRUE)
    _spark_stage_module("${_test_root}/sidecar")
    file(RPATH_SET FILE "${_test_root}/sidecar/bin/${_module_name}" NEW_RPATH "$ORIGIN")
    _spark_expect_closure(rewritten-module-image "${_test_root}/sidecar"
        "does not hash the installed module")

    _spark_stage_engine("${_test_root}/symlink" "$ORIGIN/../lib" TRUE)
    file(CREATE_LINK "${_fixture_bin}" "${_test_root}/symlink/lib/outside" SYMBOLIC)
    _spark_expect_closure(symlink-out-of-prefix "${_test_root}/symlink" "symlink leaves the prefix")

    # Loader-output classification, independent of what this host provides.
    set(_classify_cases
        "libfoo.so.1|${_first_forbidden}/lib/libfoo.so.1|source/build tree"
        "libfoo.so.1||is not found"
        "libSDL2-2.0.so.0|/usr/lib/libSDL2-2.0.so.0|shipped by the package"
        "libbar.so.2|/srv/vendor/libbar.so.2|outside the prefix and the host system"
        "libc.so.6|/usr/lib/libc.so.6|"
        "libfoo.so.1|${_test_root}/clean/lib/libfoo.so.1|")
    foreach(_case IN LISTS _classify_cases)
        string(REPLACE "|" ";" _fields "${_case}")
        list(LENGTH _fields _field_count)
        list(GET _fields 0 _soname)
        list(GET _fields 1 _resolved)
        set(_expected "")
        if(_field_count GREATER 2)
            list(GET _fields 2 _expected)
        endif()
        _spark_classify_resolution(SONAME "${_soname}" RESOLVED "${_resolved}" PREFIX "${_test_root}/clean"
            FORBIDDEN_ROOTS ${_forbidden} SYSTEM_DIRS /lib /usr/lib BUNDLED_SONAMES libSDL2-2.0.so.0
            OUT_VIOLATION _violation OUT_KIND _kind)
        if(_expected STREQUAL "" AND NOT _violation STREQUAL "")
            list(APPEND _failures "classify ${_soname} => '${_resolved}': unexpected violation '${_violation}'")
        elseif(NOT _expected STREQUAL "")
            string(FIND "${_violation}" "${_expected}" _position)
            if(_position EQUAL -1)
                list(APPEND _failures
                    "classify ${_soname} => '${_resolved}': expected '${_expected}', got '${_violation}'")
            endif()
        endif()
    endforeach()

    # The prefix-write rule must see an in-place rewrite that keeps the file
    # name and size, not only new or removed entries.
    set(_snapshot_root "${_test_root}/snapshot")
    file(WRITE "${_snapshot_root}/share/settings.ini" "value=1\n")
    _spark_tree_snapshot("${_snapshot_root}" _snapshot_before)
    file(WRITE "${_snapshot_root}/share/settings.ini" "value=2\n")
    _spark_tree_snapshot("${_snapshot_root}" _snapshot_after)
    if(_snapshot_after STREQUAL _snapshot_before)
        list(APPEND _failures "prefix snapshot missed an in-place rewrite of share/settings.ini")
    endif()
    file(REMOVE_RECURSE "${_snapshot_root}")

    string(CONCAT _ldd_sample
        "\tlinux-vdso.so.1 (0x00007ffd)\n\tlibx.so.1 => not found\n\tliby.so.2 => /usr/lib/liby.so.2 (0x7f00)\n"
        "\t/lib64/ld-linux-x86-64.so.2 (0x7f01)\n")
    _spark_parse_ldd("${_ldd_sample}" _parsed_sonames _parsed_paths)
    if(NOT _parsed_sonames STREQUAL "libx.so.1;liby.so.2;ld-linux-x86-64.so.2" OR
       NOT _parsed_paths STREQUAL "<not-found>;/usr/lib/liby.so.2;/lib64/ld-linux-x86-64.so.2")
        list(APPEND _failures "ldd parser produced '${_parsed_sonames}' / '${_parsed_paths}'")
    endif()

    if(_failures)
        list(JOIN _failures "\n  " _failure_text)
        message(FATAL_ERROR "Linux runtime-closure self test failed:\n  ${_failure_text}")
    endif()
    file(REMOVE_RECURSE "${_test_root}")
    message(STATUS "Linux runtime-closure self test passed: every rule rejects its real defective fixture")
    return()
endif()

# ---------------------------------------------------------------------------
# Install (or adopt) the prefix.
# ---------------------------------------------------------------------------
if(NOT DEFINED SPARK_SOURCE_ROOT OR NOT IS_DIRECTORY "${SPARK_SOURCE_ROOT}")
    message(FATAL_ERROR "SPARK_SOURCE_ROOT must name the engine source tree")
endif()
set(_forbidden "${SPARK_SOURCE_ROOT}" ${SPARK_FORBIDDEN_ROOTS})

if(DEFINED SPARK_ENGINE_BUILD_DIR AND NOT SPARK_ENGINE_BUILD_DIR STREQUAL "")
    list(APPEND _forbidden "${SPARK_ENGINE_BUILD_DIR}")
    set(_prefix "${_test_root}/prefix")
    file(REMOVE_RECURSE "${_test_root}")
    file(MAKE_DIRECTORY "${_test_root}")
    set(_install "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}" --prefix "${_prefix}")
    if(DEFINED SPARK_CONFIG AND NOT SPARK_CONFIG STREQUAL "")
        list(APPEND _install --config "${SPARK_CONFIG}")
    endif()
    execute_process(COMMAND ${_install}
        RESULT_VARIABLE _install_result OUTPUT_VARIABLE _install_output ERROR_VARIABLE _install_error TIMEOUT 600)
    file(WRITE "${_test_root}/install.log" "${_install_output}\n${_install_error}")
    if(NOT _install_result EQUAL 0)
        message(FATAL_ERROR "cmake --install failed (${_install_result}); log: ${_test_root}/install.log\n"
            "${_install_error}")
    endif()
elseif(DEFINED SPARK_INSTALL_PREFIX AND IS_DIRECTORY "${SPARK_INSTALL_PREFIX}")
    file(REAL_PATH "${SPARK_INSTALL_PREFIX}" _prefix)
    _spark_path_is_under("${_test_root}" "${_prefix}" _root_in_prefix)
    _spark_path_is_under("${_prefix}" "${_test_root}" _prefix_in_root)
    if(_root_in_prefix OR _prefix_in_root)
        message(FATAL_ERROR "SPARK_TEST_ROOT and SPARK_INSTALL_PREFIX must not contain each other")
    endif()
    file(REMOVE_RECURSE "${_test_root}")
    file(MAKE_DIRECTORY "${_test_root}")
else()
    message(FATAL_ERROR "Pass SPARK_ENGINE_BUILD_DIR (install mode) or an existing SPARK_INSTALL_PREFIX")
endif()

set(_engine "${_prefix}/bin/SparkEngine")
set(_module "${_prefix}/bin/libSparkGameFPS.so")
foreach(_payload IN ITEMS "${_engine}" "${_module}" "${_module}.sparkabi")
    if(NOT EXISTS "${_payload}")
        message(FATAL_ERROR "The installed tree has no ${_payload}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Static closure: RUNPATH/RPATH, NEEDED, symlinks, module sidecars.
# ---------------------------------------------------------------------------
spark_linux_runtime_closure_violations(PREFIX "${_prefix}" OUT_VAR _violations REPORT_VAR _closure_report
    ELF_COUNT_VAR _elf_count FORBIDDEN_ROOTS ${_forbidden})
if(_elf_count EQUAL 0)
    message(FATAL_ERROR "No ELF image was found under ${_prefix}")
endif()

# ---------------------------------------------------------------------------
# Runtime: installed engine + installed SparkGameFPS from cwd=/.
# ---------------------------------------------------------------------------
set(_home "${_test_root}/home")
foreach(_dir IN ITEMS "${_home}/.local/share" "${_home}/.config" "${_home}/.cache" "${_home}/.local/state"
                      "${_test_root}/tmp")
    file(MAKE_DIRECTORY "${_dir}")
endforeach()
_spark_tree_snapshot("${_prefix}" _prefix_before)
file(GLOB _root_before LIST_DIRECTORIES true "/*")

# The launch runs even after a static violation: its records show how the
# defect surfaces at runtime. Reuse the one strict record parser the
# source-tree NullRHI tests use.
set(SPARK_HEADLESS_NULLRHI_PARSER_ONLY ON)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/RunSparkHeadlessNullRHILifecycle.cmake")
execute_process(
    COMMAND "${SPARK_ENV}" -i
        "PATH=/usr/bin:/bin"
        "HOME=${_home}"
        "XDG_DATA_HOME=${_home}/.local/share"
        "XDG_CONFIG_HOME=${_home}/.config"
        "XDG_CACHE_HOME=${_home}/.cache"
        "XDG_STATE_HOME=${_home}/.local/state"
        "TMPDIR=${_test_root}/tmp"
        "SPARK_RHI_BACKEND=null"
        "${_engine}" -headless -game "${_module}" -require-game -test-frames 8 -threads 2 -no-subprocess
    WORKING_DIRECTORY /
    RESULT_VARIABLE _run_result
    OUTPUT_VARIABLE _run_stdout
    ERROR_VARIABLE _run_stderr
    TIMEOUT 120
    ENCODING UTF-8)
file(WRITE "${_test_root}/engine-stdout.log" "${_run_stdout}")
file(WRITE "${_test_root}/engine-stderr.log" "${_run_stderr}")
set(_run_output "${_run_stdout}")

_spark_validate_headless_nullrhi_result("${_run_result}" "${_run_stdout}" "${_run_stderr}" _run_ok _run_reason)
if(NOT _run_ok)
    list(APPEND _violations "installed SparkEngine + SparkGameFPS lifecycle: ${_run_reason}")
endif()
if(NOT _run_stdout MATCHES
   "(^|\n)SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=[1-9][0-9]* fixed=[1-9][0-9]* render=0 unload=1 destroy=[01] faults=0\n")
    list(APPEND _violations "installed run has no clean SPARK_MODULE_LIFECYCLE record for SparkGameFPS")
endif()

# The prefix and the test root are the only legitimate paths in the output;
# any other mention of the source/build tree is a runtime dependency on it.
set(_scrubbed "${_run_stdout}\n${_run_stderr}")
string(REPLACE "${_prefix}" "<prefix>" _scrubbed "${_scrubbed}")
string(REPLACE "${_test_root}" "<test-root>" _scrubbed "${_scrubbed}")
foreach(_root IN LISTS _forbidden)
    file(REAL_PATH "${_root}" _real_root)
    foreach(_needle IN ITEMS "${_root}" "${_real_root}")
        string(FIND "${_scrubbed}" "${_needle}" _position)
        if(NOT _position EQUAL -1)
            string(SUBSTRING "${_scrubbed}" ${_position} 200 _context)
            list(APPEND _violations "installed run output names the source/build tree: ${_context}")
        endif()
    endforeach()
endforeach()

_spark_tree_snapshot("${_prefix}" _prefix_after)
if(NOT _prefix_after STREQUAL _prefix_before)
    set(_added ${_prefix_after})
    list(REMOVE_ITEM _added ${_prefix_before})
    set(_removed ${_prefix_before})
    list(REMOVE_ITEM _removed ${_prefix_after})
    list(APPEND _violations
        "installed run modified the prefix (new or changed: ${_added}; removed or changed: ${_removed})")
endif()
file(GLOB _root_after LIST_DIRECTORIES true "/*")
if(NOT _root_after STREQUAL _root_before)
    set(_added ${_root_after})
    list(REMOVE_ITEM _added ${_root_before})
    list(APPEND _violations "installed run created top-level entries in its working directory / (${_added})")
endif()

# ---------------------------------------------------------------------------
# Report.
# ---------------------------------------------------------------------------
execute_process(COMMAND ${_spark_clean_env} "${SPARK_LDD}" --version
    OUTPUT_VARIABLE _glibc_version ERROR_QUIET TIMEOUT 30)
string(REGEX MATCH "^[^\n]*" _glibc_version "${_glibc_version}")
string(REGEX MATCH "libstdc\\+\\+\\.so\\.6 => [^\n]*" _libstdcxx "${_closure_report}")
cmake_host_system_information(RESULT _os_release QUERY OS_RELEASE)
cmake_host_system_information(RESULT _os_distro QUERY DISTRIB_PRETTY_NAME)
string(REGEX MATCHALL "(^|\n)SPARK_[A-Z_]+ [^\n]*" _records "${_run_output}")
string(REPLACE ";" "" _records "${_records}")
string(STRIP "${_records}" _records)
list(JOIN _violations "\n  " _violation_text)
if(_violations)
    set(_verdict "FAIL")
else()
    set(_verdict "PASS")
endif()
file(WRITE "${_test_root}/runtime-closure-report.txt"
    "PLT-210 installed Linux runtime closure: ${_verdict}\n"
    "Prefix: ${_prefix}\n"
    "Host: ${_os_distro} (kernel ${_os_release})\n"
    "glibc: ${_glibc_version}\n"
    "libstdc++: ${_libstdcxx}\n"
    "${_closure_report}"
    "Installed runtime records (cwd=/, empty environment, fresh HOME/XDG):\n${_records}\n"
    "Violations:\n  ${_violation_text}\n")

if(_violations)
    message(FATAL_ERROR "Installed Linux tree runtime closure failed for ${_prefix}:\n  ${_violation_text}\n"
        "Report: ${_test_root}/runtime-closure-report.txt")
endif()
# Only the report and logs are evidence; the installed prefix (hundreds of MB)
# and the scratch HOME/TMPDIR are removed. A caller-owned standalone prefix is
# never touched.
file(REMOVE_RECURSE "${_home}" "${_test_root}/tmp")
if(DEFINED SPARK_ENGINE_BUILD_DIR AND NOT SPARK_ENGINE_BUILD_DIR STREQUAL "")
    file(REMOVE_RECURSE "${_prefix}")
endif()
message(STATUS "Installed Linux tree runtime closure passed: ${_elf_count} ELF images under ${_prefix}; "
    "report: ${_test_root}/runtime-closure-report.txt")
