/**
 * @file unity_stub.h
 * @brief Minimal Unity framework stub for test compilation
 * 
 * This provides the minimum Unity interface needed to run tests
 * on platforms without full Unity framework installed.
 */

#ifndef UNITY_STUB_H
#define UNITY_STUB_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Test counters
static int unity_test_count = 0;
static int unity_pass_count = 0;
static int unity_fail_count = 0;
static const char* unity_current_test = "unknown";

// Minimal assertions
#define TEST_ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            printf("  ✗ FAIL: %s (line %d) - condition is false\n", \
                   __FILE__, __LINE__); \
            unity_fail_count++; \
        } else { \
            unity_pass_count++; \
        } \
        unity_test_count++; \
    } while(0)

#define TEST_ASSERT_FALSE(condition) \
    TEST_ASSERT_TRUE(!(condition))

#define TEST_ASSERT_EQUAL_INT(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            printf("  ✗ FAIL: %s (line %d) - expected %d, got %d\n", \
                   __FILE__, __LINE__, (int)(expected), (int)(actual)); \
            unity_fail_count++; \
        } else { \
            unity_pass_count++; \
        } \
        unity_test_count++; \
    } while(0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            printf("  ✗ FAIL: %s (line %d) - expected '%s', got '%s'\n", \
                   __FILE__, __LINE__, (expected), (actual)); \
            unity_fail_count++; \
        } else { \
            unity_pass_count++; \
        } \
        unity_test_count++; \
    } while(0)

#define TEST_ASSERT_NOT_NULL(pointer) \
    TEST_ASSERT_TRUE((pointer) != NULL)

#define TEST_ASSERT_NULL(pointer) \
    TEST_ASSERT_TRUE((pointer) == NULL)

#define TEST_PASS() \
    do { \
        unity_pass_count++; \
        unity_test_count++; \
    } while(0)

#define UNITY_BEGIN() \
    do { \
        printf("\n"); \
        printf("================================\n"); \
        printf("  PARLEY FIRMWARE UNIT TESTS\n"); \
        printf("================================\n"); \
        printf("\n"); \
        unity_test_count = 0; \
        unity_pass_count = 0; \
        unity_fail_count = 0; \
    } while(0)

#define UNITY_END() \
    do { \
        printf("\n"); \
        printf("================================\n"); \
        printf("  TEST RESULTS\n"); \
        printf("================================\n"); \
        printf("Total tests: %d\n", unity_test_count); \
        printf("Passed:      %d ✓\n", unity_pass_count); \
        printf("Failed:      %d ✗\n", unity_fail_count); \
        printf("================================\n\n"); \
        return (unity_fail_count == 0) ? 0 : 1; \
    } while(0)

#endif // UNITY_STUB_H
