# MinGW Case-Alias + DirectXMath Shims

When cross-compiling SparkEngine for Windows on a Linux host via MinGW-w64, a
few Windows SDK headers have different filenames or contents than the real
Microsoft SDK:

| Header              | MinGW-w64                          | Windows SDK / MSVC  |
|---------------------|------------------------------------|---------------------|
| `DirectXMath.h`     | `directxmath.h` (lowercase, stub)  | `DirectXMath.h`     |
| `DirectXCollision.h`| *not present*                      | `DirectXCollision.h`|
| `DirectXColors.h`   | *not present*                      | `DirectXColors.h`   |

The Linux filesystem is case-sensitive, so `#include <DirectXMath.h>` fails
outright, and MinGW-w64's bundled `directxmath.h` only defines the storage
types (`XMFLOAT2/3/4`, `XMUINT*`, etc.) — **no `XMVECTOR`, no `XMMATRIX`,
no intrinsics, no math operations**. SparkEngine uses all of those, so the
full Microsoft DirectXMath headers must be present at build time.

## How the fix works

`cmake/toolchains/mingw-w64-x86_64.cmake` calls
`FetchContent_MakeAvailable(DirectXMath)` during configuration, which
downloads the `oct2024` (or newer) release of
<https://github.com/microsoft/DirectXMath> to the CMake build tree and adds
its `Inc/` directory to `include_directories(BEFORE SYSTEM)`. Engine code
continues to `#include <DirectXMath.h>` with no change required, and
MinGW-w64's lowercase `directxmath.h` is shadowed by the real one.

## Offline / airgapped builds

If network access is unavailable at configure time you can drop a copy of
the Microsoft headers directly into this directory (alongside this README)
and `FetchContent` will skip the download. The expected file names are:

```
cmake/mingw-shims/
├── DirectXMath.h           (required)
├── DirectXMathConvert.inl  (required)
├── DirectXMathMatrix.inl   (required)
├── DirectXMathMisc.inl     (required)
├── DirectXMathVector.inl   (required)
├── DirectXCollision.h      (optional — only needed by collision helpers)
├── DirectXCollision.inl    (optional)
├── DirectXColors.h         (optional)
├── DirectXPackedVector.h   (optional)
└── DirectXPackedVector.inl (optional)
```

Microsoft's DirectXMath is MIT-licensed; see
<https://github.com/microsoft/DirectXMath/blob/main/LICENSE>.
