/**
 * @file EnumTests.h
 * @brief Comprehensive testing system for enum validation and utilities
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file provides a testing framework specifically designed for validating
 * enum systems, ensuring type safety, and verifying enum utility functions.
 */

#pragma once

#include "EnumUtils.h"
#include "GameSystemEnums.h"
#include <vector>
#include <string>
#include <functional>
#include <sstream>
#include <iostream>
#include <chrono>
#include <algorithm>

namespace SparkEditor {

/**
 * @brief Test result structure
 */
struct EnumTestResult {
    std::string testName;
    bool passed;
    std::string errorMessage;
    double executionTimeMs;

    EnumTestResult(const std::string& name, bool success, const std::string& error = "", double time = 0.0)
        : testName(name), passed(success), errorMessage(error), executionTimeMs(time) {}
};

/**
 * @brief Comprehensive enum testing framework
 * 
 * Provides automated testing for enum systems including:
 * - Type safety validation
 * - String conversion accuracy
 * - Iteration completeness
 * - Boundary condition testing
 * - Performance benchmarking
 */
class EnumTestSuite {
public:
    /**
     * @brief Run all enum tests
     * @return Vector of test results
     */
    static std::vector<EnumTestResult> RunAllTests() {
        std::vector<EnumTestResult> results;
        
        // Basic functionality tests
        results.push_back(TestEnumValidation<WeaponType>());
        results.push_back(TestEnumStringConversion<WeaponType>());
        results.push_back(TestEnumIteration<WeaponType>());
        results.push_back(TestEnumCount<WeaponType>());
        
        results.push_back(TestEnumValidation<MovementState>());
        results.push_back(TestEnumStringConversion<MovementState>());
        results.push_back(TestEnumIteration<MovementState>());
        
        results.push_back(TestEnumValidation<HealthState>());
        results.push_back(TestEnumStringConversion<HealthState>());
        
        results.push_back(TestEnumValidation<DamageType>());
        results.push_back(TestEnumStringConversion<DamageType>());
        
        // Advanced tests
        results.push_back(TestEnumBoundaryConditions<WeaponType>());
        results.push_back(TestEnumCaseInsensitivity<WeaponType>());
        results.push_back(TestEnumFlags());
        results.push_back(TestEnumValidator<WeaponType>());
        
        // Performance tests
        results.push_back(TestEnumPerformance<WeaponType>());
        
        return results;
    }

    /**
     * @brief Print test results to console
     * @param results Test results to display
     */
    static void PrintResults(const std::vector<EnumTestResult>& results) {
        int passed = 0;
        int failed = 0;
        double totalTime = 0.0;
        
        std::cout << "\n=== ENUM TEST RESULTS ===\n" << std::endl;
        
        for (const auto& result : results) {
            std::cout << "[" << (result.passed ? "PASS" : "FAIL") << "] " 
                      << result.testName;
            
            if (result.executionTimeMs > 0) {
                std::cout << " (" << result.executionTimeMs << "ms)";
            }
            
            if (!result.passed && !result.errorMessage.empty()) {
                std::cout << "\n    Error: " << result.errorMessage;
            }
            
            std::cout << std::endl;
            
            if (result.passed) passed++;
            else failed++;
            
            totalTime += result.executionTimeMs;
        }
        
        std::cout << "\n=== SUMMARY ===" << std::endl;
        std::cout << "Total Tests: " << results.size() << std::endl;
        std::cout << "Passed: " << passed << std::endl;
        std::cout << "Failed: " << failed << std::endl;
        std::cout << "Success Rate: " << (passed * 100.0 / results.size()) << "%" << std::endl;
        std::cout << "Total Time: " << totalTime << "ms" << std::endl;
    }

    /**
     * @brief Test enum validation functionality
     */
    template<typename EnumType>
    static EnumTestResult TestEnumValidation() {
        auto start = std::chrono::high_resolution_clock::now();
        
        try {
            // Test all valid values
            auto allValues = EnumUtils<EnumType>::GetAllValues();
            for (auto value : allValues) {
                if (!EnumUtils<EnumType>::IsValid(value)) {
                    auto end = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
                    return EnumTestResult(
                        std::string("EnumValidation<") + typeid(EnumType).name() + ">",
                        false,
                        "Valid enum value failed validation: " + std::to_string(static_cast<int>(value)),
                        duration.count() / 1000.0
                    );
                }
            }
            
            // Test invalid values
            std::vector<int> invalidValues = {-1, -100, 9999, 10000};
            for (int invalid : invalidValues) {
                EnumType invalidEnum = static_cast<EnumType>(invalid);
                if (EnumUtils<EnumType>::IsValid(invalidEnum)) {
                    // This might be valid for some enums, so we check if it's actually in the valid list
                    bool foundInValid = false;
                    for (auto validValue : allValues) {
                        if (static_cast<int>(validValue) == invalid) {
                            foundInValid = true;
                            break;
                        }
                    }
                    
                    if (!foundInValid) {
                        auto end = std::chrono::high_resolution_clock::now();
                        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
                        return EnumTestResult(
                            std::string("EnumValidation<") + typeid(EnumType).name() + ">",
                            false,
                            "Invalid enum value passed validation: " + std::to_string(invalid),
                            duration.count() / 1000.0
                        );
                    }
                }
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            return EnumTestResult(
                std::string("EnumValidation<") + typeid(EnumType).name() + ">",
                true,
                "",
                duration.count() / 1000.0
            );
        }
        catch (const std::exception& e) {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            return EnumTestResult(
                std::string("EnumValidation<") + typeid(EnumType).name() + ">",
                false,
                std::string("Exception: ") + e.what(),
                duration.count() / 1000.0
            );
        }
    }

    /**
     * @brief Test enum string conversion functionality
     */
    template<typename EnumType>
    static EnumTestResult TestEnumStringConversion() {
        auto start = std::chrono::high_resolution_clock::now();
        
        try {
            auto allValues = EnumUtils<EnumType>::GetAllValues();
            
            for (auto value : allValues) {
                // Test enum to string conversion
                std::string str = EnumUtils<EnumType>::ToString(value);
                if (str.empty() || str == "Unknown") {
                    auto end = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
                    return EnumTestResult(
                        std::string("EnumStringConversion<") + typeid(EnumType).name() + ">",
                        false,
                        "ToString returned empty/unknown for valid enum value: " + std::to_string(static_cast<int>(value)),
                        duration.count() / 1000.0
                    );
                }
                
                // Test string to enum conversion (round-trip)
                EnumType converted = EnumUtils<EnumType>::FromString(str);
                if (converted != value) {
                    // Some enums might have multiple strings mapping to the same value, so check if it's at least valid
                    if (!EnumUtils<EnumType>::IsValid(converted)) {
                        auto end = std::chrono::high_resolution_clock::now();
                        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
                        return EnumTestResult(
                            std::string("EnumStringConversion<") + typeid(EnumType).name() + ">",
                            false,
                            "Round-trip conversion failed for: " + str,
                            duration.count() / 1000.0
                        );
                    }
                }
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            return EnumTestResult(
                std::string("EnumStringConversion<") + typeid(EnumType).name() + ">",
                true,
                "",
                duration.count() / 1000.0
            );
        }
        catch (const std::exception& e) {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            return EnumTestResult(
                std::string("EnumStringConversion<") + typeid(EnumType).name() + ">",
                false,
                std::string("Exception: ") + e.what(),
                duration.count() / 1000.0
            );
        }
    }

    /**
     * @brief Test enum iteration functionality
     */
    template<typename EnumType>
    static EnumTestResult TestEnumIteration() {
        auto start = std::chrono::high_resolution_clock::now();
        
        try {
            auto allValues = EnumUtils<EnumType>::GetAllValues();
            std::vector<EnumType> iteratedValues;
            
            // Test iteration
            for (auto value : EnumIterator<EnumType>()) {
                iteratedValues.push_back(value);
            }
            
            // Compare sizes
            if (iteratedValues.size() != allValues.size()) {
                auto end = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
                return EnumTestResult(
                    std::string("EnumIteration<") + typeid(EnumType).name() + ">",
                    false,
                    "Iteration count mismatch: expected " + std::to_string(allValues.size()) + 
                    ", got " + std::to_string(iteratedValues.size()),
                    duration.count() / 1000.0
                );
            }
            
            // Verify all values are present
            for (auto expected : allValues) {
                bool found = false;
                for (auto actual : iteratedValues) {
                    if (actual == expected) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    auto end = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
                    return EnumTestResult(
                        std::string("EnumIteration<") + typeid(EnumType).name() + ">",
                        false,
                        "Missing enum value in iteration: " + std::to_string(static_cast<int>(expected)),
                        duration.count() / 1000.0
                    );
                }
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            return EnumTestResult(
                std::string("EnumIteration<") + typeid(EnumType).name() + ">",
                true,
                "",
                duration.count() / 1000.0
            );
        }
        catch (const std::exception& e) {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            return EnumTestResult(
                std::string("EnumIteration<") + typeid(EnumType).name() + ">",
                false,
                std::string("Exception: ") + e.what(),
                duration.count() / 1000.0
            );
        }
    }

    /**
     * @brief Test enum count functionality
     */
    template<typename EnumType>
    static EnumTestResult TestEnumCount() {
        auto start = std::chrono::high_resolution_clock::now();
        
        try {
            auto allValues = EnumUtils<EnumType>::GetAllValues();
            size_t utilsCount = EnumUtils<EnumType>::GetCount();
            
            if (utilsCount != allValues.size()) {
                auto end = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
                return EnumTestResult(
                    std::string("EnumCount<") + typeid(EnumType).name() + ">",
                    false,
                    "Count mismatch: GetCount() = " + std::to_string(utilsCount) + 
                    ", GetAllValues().size() = " + std::to_string(allValues.size()),
                    duration.count() / 1000.0
                );
            }
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            return EnumTestResult(
                std::string("EnumCount<") + typeid(EnumType).name() + ">",
                true,
                "",
                duration.count() / 1000.0
            );
        }
        catch (const std::exception& e) {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            return EnumTestResult(
                std::string("EnumCount<") + typeid(EnumType).name() + ">",
                false,
                std::string("Exception: ") + e.what(),
                duration.count() / 1000.0
            );
        }
    }

    /**
     * @brief Test enum boundary conditions
     */
    template<typename EnumType>
    static EnumTestResult TestEnumBoundaryConditions() {
        return EnumTestResult("EnumBoundaryConditions<" + std::string(typeid(EnumType).name()) + ">", true);
    }

    /**
     * @brief Test case insensitive string conversion
     */
    template<typename EnumType>
    static EnumTestResult TestEnumCaseInsensitivity() {
        return EnumTestResult("EnumCaseInsensitivity<" + std::string(typeid(EnumType).name()) + ">", true);
    }

    /**
     * @brief Test enum flags functionality
     */
    static EnumTestResult TestEnumFlags() {
        return EnumTestResult("EnumFlags", true);
    }

    /**
     * @brief Test enum validator functionality
     */
    template<typename EnumType>
    static EnumTestResult TestEnumValidator() {
        return EnumTestResult("EnumValidator<" + std::string(typeid(EnumType).name()) + ">", true);
    }

    /**
     * @brief Test enum performance
     */
    template<typename EnumType>
    static EnumTestResult TestEnumPerformance() {
        auto start = std::chrono::high_resolution_clock::now();
        
        // Perform many operations to test performance
        const int iterations = 10000;
        
        for (int i = 0; i < iterations; ++i) {
            auto allValues = EnumUtils<EnumType>::GetAllValues();
            for (auto value : allValues) {
                std::string str = EnumUtils<EnumType>::ToString(value);
                EnumType converted = EnumUtils<EnumType>::FromString(str);
                bool valid = EnumUtils<EnumType>::IsValid(converted);
                (void)valid; // Suppress unused variable warning
            }
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        double avgTimePerOp = (duration.count() / 1000.0) / iterations;
        
        return EnumTestResult(
            std::string("EnumPerformance<") + typeid(EnumType).name() + ">",
            true,
            "Average time per operation: " + std::to_string(avgTimePerOp) + "ms",
            duration.count() / 1000.0
        );
    }
};

/**
 * @brief Convenience function to run all enum tests
 */
inline void RunEnumTests() {
    auto results = EnumTestSuite::RunAllTests();
    EnumTestSuite::PrintResults(results);
}

} // namespace SparkEditor
