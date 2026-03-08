@echo off
REM generate.bat - Standalone CMake configuration generator for SparkEngine

set "GEN=Visual Studio 17 2022"
set CONFIG=Debug

:parse_args
if "%~1"=="" goto after_args
    if /I "%~1"=="release" (set CONFIG=Release)
    if /I "%~1"=="debug"   (set CONFIG=Debug)
    if /I "%~1"=="-g"      (set GEN=%2 & shift)
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