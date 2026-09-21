if(NOT DEFINED FPS_SOURCE OR "${FPS_SOURCE}" STREQUAL "")
    message(FATAL_ERROR "FPS_SOURCE is required")
endif()

file(GLOB_RECURSE _module_sources LIST_DIRECTORIES FALSE
    "${FPS_SOURCE}/*.h"
    "${FPS_SOURCE}/*.hpp"
    "${FPS_SOURCE}/*.cpp"
    "${FPS_SOURCE}/*.cc"
    "${FPS_SOURCE}/*.cxx"
)
if(NOT _module_sources)
    message(FATAL_ERROR "No SparkGameFPS source files found under: ${FPS_SOURCE}")
endif()

foreach(_source IN LISTS _module_sources)
    file(READ "${_source}" _source_text)
    if(_source_text MATCHES "Core/IGameModule\\.h" OR
       _source_text MATCHES "public[ \t\r\n]+IGameModule")
        message(FATAL_ERROR "SparkGameFPS source still exposes the private legacy IGameModule contract: ${_source}")
    endif()
    if(_source_text MATCHES "Core/EngineContext\\.h" OR
       _source_text MATCHES "EngineContext::Get[ \t\r\n]*\\(")
        message(FATAL_ERROR "SparkGameFPS source still bypasses its injected public IEngineContext: ${_source}")
    endif()
    if(_source_text MATCHES "CreateGameModule[ \t\r\n]*\\(" OR
       _source_text MATCHES "DestroyGameModule[ \t\r\n]*\\(")
        message(FATAL_ERROR "SparkGameFPS source still exports the retired legacy game-module factories: ${_source}")
    endif()
endforeach()

list(LENGTH _module_sources _module_source_count)
message(STATUS "SparkGameFPS module API/source boundary contract passed (${_module_source_count} source files scanned)")
