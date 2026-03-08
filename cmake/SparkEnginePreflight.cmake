# SparkEnginePreflight.cmake
# ---------------------------------------------------------------------------
# Pre-flight validation for SparkEngine SDK installations.
#
# Include this BEFORE calling find_package(SparkEngine REQUIRED) so that
# users get actionable diagnostics instead of a cryptic CMake "include could
# not find requested file" error when their installation is incomplete.
# ---------------------------------------------------------------------------

# If the caller already set CMAKE_PREFIX_PATH or SparkEngine_DIR, try to
# locate the config directory and verify that every expected file is present.

function(spark_preflight_check)
  # Collect candidate directories where SparkEngineConfig.cmake might live.
  set(_candidates)

  if(DEFINED SparkEngine_DIR AND EXISTS "${SparkEngine_DIR}")
    list(APPEND _candidates "${SparkEngine_DIR}")
  endif()

  foreach(_prefix ${CMAKE_PREFIX_PATH})
    foreach(_subdir
      "lib/cmake/SparkEngine"
      "share/cmake/SparkEngine"
      "cmake")
      if(EXISTS "${_prefix}/${_subdir}/SparkEngineConfig.cmake")
        list(APPEND _candidates "${_prefix}/${_subdir}")
      endif()
    endforeach()
  endforeach()

  if(NOT _candidates)
    # Nothing found — find_package will report its own "package not found"
    # message, which is already clear enough.
    return()
  endif()

  # Check every candidate for the required companion files.
  set(_required_files
    SparkEngineTargets.cmake
    SparkGameModule.cmake
  )

  foreach(_dir ${_candidates})
    foreach(_file ${_required_files})
      if(NOT EXISTS "${_dir}/${_file}")
        message(FATAL_ERROR
          "\n"
          "SparkEngine installation is INCOMPLETE.\n"
          "\n"
          "  Found config at : ${_dir}/SparkEngineConfig.cmake\n"
          "  Missing file    : ${_dir}/${_file}\n"
          "\n"
          "This usually means the CMake install step was interrupted or\n"
          "you are pointing at a build tree instead of an install prefix.\n"
          "\n"
          "To fix this, re-run the SparkEngine install:\n"
          "\n"
          "  cmake --install <build-dir> --prefix <install-prefix>\n"
          "\n"
          "Then configure this project with:\n"
          "\n"
          "  cmake -B build -DCMAKE_PREFIX_PATH=<install-prefix>\n"
          "\n"
        )
      endif()
    endforeach()
  endforeach()
endfunction()

spark_preflight_check()
