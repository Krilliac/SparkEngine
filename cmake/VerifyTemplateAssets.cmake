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

file(GLOB manifest_files "${SPARK_TEMPLATE_ROOT}/*/Assets/manifest.json")
list(LENGTH manifest_files manifest_count)
if(NOT manifest_count EQUAL 9)
    message(FATAL_ERROR "Expected 9 template asset manifests, found ${manifest_count}")
endif()

set(declared_asset_count 0)
foreach(manifest_file IN LISTS manifest_files)
    file(READ "${manifest_file}" manifest_json)
    string(JSON manifest_asset_count LENGTH "${manifest_json}" assets)
    if(manifest_asset_count LESS 1)
        message(FATAL_ERROR "Template asset manifest contains no assets: ${manifest_file}")
    endif()
    get_filename_component(asset_directory "${manifest_file}" DIRECTORY)
    get_filename_component(package_directory "${asset_directory}" DIRECTORY)
    get_filename_component(package_name "${package_directory}" NAME)
    math(EXPR last_manifest_asset "${manifest_asset_count} - 1")
    foreach(manifest_index RANGE 0 ${last_manifest_asset})
        string(JSON declared_path GET "${manifest_json}" assets ${manifest_index} path)
        string(JSON declared_hash GET "${manifest_json}" assets ${manifest_index} sha256)
        set(relative_path "${package_name}/Assets/${declared_path}")
        string(JSON locked_hash ERROR_VARIABLE lock_error GET "${lock_json}" assets "${relative_path}")
        if(NOT lock_error STREQUAL "NOTFOUND")
            message(FATAL_ERROR "Manifest asset is not locked: ${relative_path} (${lock_error})")
        endif()
        if(NOT declared_hash STREQUAL locked_hash)
            message(FATAL_ERROR
                "Manifest/lock hash mismatch: ${relative_path}\n"
                "manifest ${declared_hash}\n"
                "lock     ${locked_hash}")
        endif()
        math(EXPR declared_asset_count "${declared_asset_count} + 1")
    endforeach()
endforeach()

if(NOT declared_asset_count EQUAL asset_count)
    message(FATAL_ERROR
        "Template manifest/lock coverage differs: ${declared_asset_count} declared, ${asset_count} locked")
endif()

message(STATUS "Verified ${asset_count} locked assets across ${manifest_count} template manifests")
