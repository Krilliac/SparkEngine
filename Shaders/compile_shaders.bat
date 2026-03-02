@echo off
REM ============================================================================
REM Spark Engine - Shader Compilation Pipeline (Windows)
REM ============================================================================
REM
REM Compiles GLSL shaders to SPIR-V for Vulkan backend.
REM Requires Vulkan SDK installed with glslangValidator in PATH.
REM
REM Usage:
REM   compile_shaders.bat              - Compile all shaders
REM   compile_shaders.bat --clean      - Remove compiled shaders
REM ============================================================================

setlocal EnableDelayedExpansion

set SCRIPT_DIR=%~dp0
set GLSL_DIR=%SCRIPT_DIR%GLSL
set SPIRV_DIR=%SCRIPT_DIR%SPIRV
set PASS=0
set FAIL=0

REM Find glslangValidator
set GLSLANG=glslangValidator
where glslangValidator >nul 2>&1
if errorlevel 1 (
    if defined VULKAN_SDK (
        set GLSLANG=%VULKAN_SDK%\Bin\glslangValidator.exe
        if not exist "!GLSLANG!" (
            echo ERROR: glslangValidator not found. Install the Vulkan SDK.
            exit /b 1
        )
    ) else (
        echo ERROR: glslangValidator not found. Install the Vulkan SDK.
        exit /b 1
    )
)

if "%1"=="--clean" (
    echo Cleaning compiled shaders...
    if exist "%SPIRV_DIR%\*.spv" del /q "%SPIRV_DIR%\*.spv"
    echo Done.
    exit /b 0
)

echo === Spark Engine Shader Compilation ===
if not exist "%SPIRV_DIR%" mkdir "%SPIRV_DIR%"

REM --- Vertex Shaders ---
echo.
echo --- Vertex Shaders ---
call :compile "%GLSL_DIR%\BasicVS.glsl" vert "" "BasicVS_VS"
call :compile "%GLSL_DIR%\FullscreenQuad.glsl" vert "" "FullscreenQuad_VS"

REM --- Fragment Shaders ---
echo.
echo --- Fragment Shaders ---
call :compile "%GLSL_DIR%\BasicPS.glsl" frag "" "BasicPS_PS"
call :compile "%GLSL_DIR%\PBRSurface.glsl" frag "" "PBRSurface_PS"
call :compile "%GLSL_DIR%\SSAO.glsl" frag "" "SSAO_PS"
call :compile "%GLSL_DIR%\BloomExtract.glsl" frag "" "BloomExtract_PS"
call :compile "%GLSL_DIR%\DebugVisualize.glsl" frag "" "DebugVisualize_PS"

REM --- Post-Processing ---
echo.
echo --- Post-Processing ---
call :compile "%GLSL_DIR%\PostProcess.glsl" frag "" "PostProcess_PS"
call :compile "%GLSL_DIR%\PostProcess.glsl" frag "-DFXAA_PASS" "PostProcess_FXAA_PS"

REM --- Gaussian Blur ---
echo.
echo --- Gaussian Blur ---
call :compile "%GLSL_DIR%\GaussianBlur.glsl" frag "-DBLUR_HORIZONTAL" "GaussianBlur_BlurH_PS"
call :compile "%GLSL_DIR%\GaussianBlur.glsl" frag "" "GaussianBlur_BlurV_PS"

REM --- Multi-Stage Shaders ---
echo.
echo --- Multi-Stage Shaders ---
for %%S in (Phong Rim Water EnvReflection) do (
    if exist "%GLSL_DIR%\%%S.glsl" (
        call :compile "%GLSL_DIR%\%%S.glsl" vert "-DVERTEX_SHADER" "%%S_VS"
        call :compile "%GLSL_DIR%\%%S.glsl" frag "-DFRAGMENT_SHADER" "%%S_PS"
    )
)

REM --- Summary ---
echo.
echo === Compilation Summary ===
echo   Passed: %PASS%
echo   Failed: %FAIL%

if %FAIL% gtr 0 (
    echo Some shaders failed. Check output above.
    exit /b 1
)

echo All shaders compiled successfully!
exit /b 0

:compile
set SRC=%~1
set STAGE=%~2
set DEFINES=%~3
set OUTPUT=%~4

echo   Compiling %OUTPUT% (%STAGE%)...
%GLSLANG% -V -S %STAGE% %DEFINES% -o "%SPIRV_DIR%\%OUTPUT%.spv" "%SRC%" >nul 2>&1
if errorlevel 1 (
    echo     FAIL
    set /a FAIL+=1
) else (
    echo     OK
    set /a PASS+=1
)
goto :eof
