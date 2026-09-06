@echo off
REM generate.bat - Standalone CMake configuration generator for SparkEngine

set "GEN=Visual Studio 17 2022"
set CONFIG=Debug

REM %2 keeps the caller's quotes, and `set GEN=%2` stored them plus the trailing
REM space before `&`, so `cmake -G "%GEN%"` was handed a doubly quoted generator.
REM `set "GEN=%~2"` strips one quote level and terminates the assignment.
:parse_args
if "%~1"=="" goto after_args
    if /I "%~1"=="release" (set CONFIG=Release)
    if /I "%~1"=="debug"   (set CONFIG=Debug)
    if /I "%~1"=="-g" (
        if "%~2"=="" (
            echo [!] -g requires a generator name
            exit /b 1
        )
        set "GEN=%~2"
        shift
    )
    shift
goto parse_args
:after_args

if not exist build (
  mkdir build
)

cd build

echo [*] Running CMake configure for %GEN% (%CONFIG%)...
cmake .. -G "%GEN%" -DCMAKE_BUILD_TYPE=%CONFIG%

cd ..
echo [*] CMake configuration/sln generation complete.