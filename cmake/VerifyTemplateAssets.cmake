if(NOT DEFINED SPARK_TEMPLATE_ROOT)
    message(FATAL_ERROR "SPARK_TEMPLATE_ROOT is required")
endif()

set(lock_file "${SPARK_TEMPLATE_ROOT}/assets.lock.json")
if(NOT EXISTS "${lock_file}")
    message(FATAL_ERROR "Template asset lock is missing: ${lock_file}")
endif()

file(READ "${lock_file}" lock_json)
string(JSON asset_count LENGTH "${lock_json}" assets)
if(asset_count LESS 1)
    message(FATAL_ERROR "Template asset lock contains no assets")
endif()

math(EXPR last_asset "${asset_count} - 1")
foreach(index RANGE 0 ${last_asset})
    string(JSON relative_path MEMBER "${lock_json}" assets ${index})
    string(JSON expected_hash GET "${lock_json}" assets "${relative_path}")
    set(asset_path "${SPARK_TEMPLATE_ROOT}/${relative_path}")
    if(NOT EXISTS "${asset_path}")
        message(FATAL_ERROR "Locked template asset is missing: ${relative_path}")
    endif()
    file(SHA256 "${asset_path}" actual_hash)
    if(NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR
            "Template asset hash mismatch: ${relative_path}\n"
            "expected ${expected_hash}\n"
            "actual   ${actual_hash}")
    endif()
endforeach()

message(STATUS "Verified ${asset_count} locked template assets")
