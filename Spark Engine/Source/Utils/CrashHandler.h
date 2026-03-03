#pragma once
#include <string>

struct CrashConfig {
    std::wstring dumpPrefix = L"GameEngineCrash";
    std::string  uploadURL = "";
    bool         captureScreenshot = true;
    bool         captureSystemInfo = true;
    bool         captureAllThreads = true;
    bool         zipBeforeUpload = true;
    bool         triggerCrashOnAssert = false;  // NEW: Control assertion crash behavior
    int          connectTimeoutSeconds = 5;
};

// Install the unhandled‐exception filter
void InstallCrashHandler(const CrashConfig& cfg);

// Called by Assert::Fail - configurable to not crash the app
void TriggerCrashHandler(const char* assertMsg);

// Optional: Runtime toggle for assert crash behavior
void SetAssertCrashBehavior(bool shouldCrash);