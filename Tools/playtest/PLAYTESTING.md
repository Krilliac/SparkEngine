# SparkGameFPS installed-build playtest

This is an early, uncertified `stable-v1` candidate. Use a Windows package that
contains both `bin/SparkEngine.exe` and `bin/SparkGameFPS.dll` from the same
build. Do not mix files from different packages.

From the installed package's `bin` directory:

1. Run `PlaytestSparkFPS.cmd check` to confirm the engine and FPS module exist
   and to print the engine version.
2. Run `PlaytestSparkFPS.cmd smoke` for a bounded, headless NullRHI launch.
   A pass is useful but does not prove graphics, gameplay, or release readiness.
3. Double-click `PlaytestSparkFPS.cmd` (or run it with `run`) to play the real
   installed FPS module. Press `F11` to start or restart the arena match.
4. Run `PlaytestSparkFPS.cmd report` to open the SparkEngine GitHub playtest
   issue form. Sign in to GitHub to submit. If no browser opens, run
   `PlaytestSparkFPS.cmd report-url` and paste the printed link into a browser.

In the issue, include the engine version, package/release identity, Windows and
graphics details, reproduction steps, expected/actual result, and whether the
smoke ran. Include the exact build commit only if the package supplied it; do
not infer it from a source checkout. Review any excerpt before posting. Crash
dumps, raw logs, and screenshots are not uploaded by this launcher.
