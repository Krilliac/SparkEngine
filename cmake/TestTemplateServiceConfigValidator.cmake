cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS SPARK_TEMPLATE_SOURCE SPARK_VALIDATOR SPARK_TEST_ROOT SPARK_BINARY_ROOT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

set(resolved_test_root "${SPARK_TEST_ROOT}")
set(resolved_binary_root "${SPARK_BINARY_ROOT}")
cmake_path(ABSOLUTE_PATH resolved_test_root NORMALIZE)
cmake_path(ABSOLUTE_PATH resolved_binary_root NORMALIZE)
cmake_path(IS_PREFIX resolved_binary_root "${resolved_test_root}" NORMALIZE test_root_is_bounded)
if(NOT test_root_is_bounded)
    message(FATAL_ERROR "Refusing to use template-validator scratch path outside the build tree")
endif()
file(REMOVE_RECURSE "${resolved_test_root}")
file(MAKE_DIRECTORY "${resolved_test_root}")

function(spark_expect_invalid case_name relative_file search replacement)
    set(case_root "${resolved_test_root}/${case_name}")
    file(MAKE_DIRECTORY "${case_root}")
    file(COPY "${SPARK_TEMPLATE_SOURCE}/Blank3D" DESTINATION "${case_root}")
    set(target "${case_root}/Blank3D/${relative_file}")
    file(READ "${target}" original)
    string(REPLACE "${search}" "${replacement}" mutated "${original}")
    if(mutated STREQUAL original)
        message(FATAL_ERROR "${case_name}: mutation did not match ${relative_file}")
    endif()
    file(WRITE "${target}" "${mutated}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DSPARK_TEMPLATE_ROOT=${case_root} -P "${SPARK_VALIDATOR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 0)
        message(FATAL_ERROR "${case_name}: validator accepted an invalid template\n${output}\n${error}")
    endif()
endfunction()

spark_expect_invalid(
    wrong_section
    "Config/server.ini"
    "[Network]\nport = 27015"
    "[Network]\n# port deliberately moved\n[Decoy]\nport = 27015")
spark_expect_invalid(
    scene_traversal
    "template.json"
    "\"defaultScene\": \"Scenes/Default.sparkscene\""
    "\"defaultScene\": \"../CMakeLists.txt\"")
spark_expect_invalid(
    mismatched_area_port
    "Config/gateway.ini"
    "[Area.Main]\nhost = 127.0.0.1\nport = 27015"
    "[Area.Main]\nhost = 127.0.0.1\nport = 27016")
spark_expect_invalid(
    duplicate_area
    "Config/gateway.ini"
    "[Status]"
    "[Area.Secondary]\nhost = 127.0.0.1\nport = 27015\ninter_server_port = 27201\nmax_clients = 16\ntick_rate = 60\nscene = Scenes/Default.sparkscene\n\n[Status]")

message(STATUS "Template service validator rejected all malformed fixtures")
