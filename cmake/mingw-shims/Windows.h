#pragma once

// The Microsoft SDK exposes Windows.h while MinGW-w64 installs windows.h.
// Linux hosts use case-sensitive paths, so forward the SDK spelling to the
// real MinGW header in the next include directory.
#include_next <windows.h>
