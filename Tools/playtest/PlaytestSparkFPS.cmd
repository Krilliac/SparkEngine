@echo off
setlocal
set "SPARK_PLAYTEST_BIN=%~dp0"
set "SPARK_PLAYTEST_ENGINE=%SPARK_PLAYTEST_BIN%SparkEngine.exe"
set "SPARK_PLAYTEST_MODULE=%SPARK_PLAYTEST_BIN%SparkGameFPS.dll"
set "SPARK_PLAYTEST_ISSUE_URL=https://github.com/Krilliac/SparkEngine/issues/new?template=playtest_bug.md"

if /I "%~1"=="report-url" goto report_url
if /I "%~1"=="report" goto report
if /I "%~1"=="check" goto check
if /I "%~1"=="smoke" goto smoke
if "%~1"=="" goto run
if /I "%~1"=="run" goto run
echo Usage: PlaytestSparkFPS.cmd [run^|check^|smoke^|report^|report-url]
exit /b 2

:require_package
if not exist "%SPARK_PLAYTEST_ENGINE%" (
    echo SparkEngine.exe is missing from this installed package.
    exit /b 1
)
if not exist "%SPARK_PLAYTEST_MODULE%" (
    echo SparkGameFPS.dll is missing from this installed package.
    exit /b 1
)
exit /b 0

:check
call :require_package
if errorlevel 1 exit /b 1
"%SPARK_PLAYTEST_ENGINE%" --version
exit /b %errorlevel%

:run
call :require_package
if errorlevel 1 exit /b 1
pushd "%SPARK_PLAYTEST_BIN%" || exit /b 1
"%SPARK_PLAYTEST_ENGINE%" -game "%SPARK_PLAYTEST_MODULE%" -require-game
set "SPARK_PLAYTEST_EXIT=%errorlevel%"
popd
exit /b %SPARK_PLAYTEST_EXIT%

:smoke
call :require_package
if errorlevel 1 exit /b 1
set "SPARK_RHI_BACKEND=null"
pushd "%SPARK_PLAYTEST_BIN%" || exit /b 1
"%SPARK_PLAYTEST_ENGINE%" -headless -game "%SPARK_PLAYTEST_MODULE%" -require-game -test-frames 8 -threads 2 -no-subprocess
set "SPARK_PLAYTEST_EXIT=%errorlevel%"
popd
exit /b %SPARK_PLAYTEST_EXIT%

:report_url
echo %SPARK_PLAYTEST_ISSUE_URL%
exit /b 0

:report
start "" "%SPARK_PLAYTEST_ISSUE_URL%"
if errorlevel 1 (
    echo Could not open a browser. Open this address manually:
    call :report_url
    exit /b 1
)
exit /b 0
