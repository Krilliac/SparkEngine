# spark_exclude_sources_matching(<list_var> <root> <regex>)
#
# Removes every entry of <list_var> whose path RELATIVE to <root> (with a leading
# "/") matches <regex>. file(GLOB_RECURSE) returns absolute paths, so filtering
# them directly also matches directories above the checkout: a clone under
# ~/lane-toolchain-..., ~/examples/ or C:/Users/Tester/ silently dropped every
# miniz, engine or module source. Filtering the root-relative path keeps the
# exclusion to subpaths of the tree being globbed.
function(spark_exclude_sources_matching list_var root regex)
    set(_spark_kept)
    foreach(_spark_path IN LISTS ${list_var})
        cmake_path(RELATIVE_PATH _spark_path BASE_DIRECTORY "${root}" OUTPUT_VARIABLE _spark_relative)
        if(NOT "/${_spark_relative}" MATCHES "${regex}")
            list(APPEND _spark_kept "${_spark_path}")
        endif()
    endforeach()
    set(${list_var} "${_spark_kept}" PARENT_SCOPE)
endfunction()
