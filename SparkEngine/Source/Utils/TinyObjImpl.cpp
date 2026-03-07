// TinyObjImpl.cpp
// ---------------------------------------------------------------------------------
// This file compiles the full tinyobjloader implementation exactly once.
// Placing the implementation in a single .cpp prevents multiple-definition
// linker errors that would occur if the implementation were included in
// multiple translation units. Other files should only #include <tiny_obj_loader.h>
// to see only declarations, while this file emits the actual function bodies.
//
// Do NOT move this define into a header or include it elsewhere—doing so
// would cause each .cpp that includes the header to generate its own copy
// of every tinyobjloader symbol, resulting in linker failures.
// ---------------------------------------------------------------------------------
#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>
