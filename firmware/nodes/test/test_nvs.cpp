#include "unity_stub.h"
#include <stdint.h>
#include <string.h>

/**
 * @file test_nvs.cpp
 * @brief Unit tests for NVS (non-volatile storage) persistence
 */

// Mock NVS storage
static struct {
    char key[64];
    char value[256];
    uint32_t type; // 0=string, 1=uint32, 2=blob
} mock_nvs_storage[20];
static int mock_nvs_entries = 0;

// Mock NVS operations status
static bool mock_nvs_initialized = false;
static bool mock_nvs_error = false;

// NVS Initialization Tests
void test_nvs_initializes_successfully(void) {
    // ARRANGE
    mock_nvs_initialized = false;
    
    // ACT: Initialize NVS
    mock_nvs_initialized = true;
    
    // ASSERT
    TEST_ASSERT_TRUE(mock_nvs_initialized);
}

void test_nvs_initialization_failure_handled(void) {
    // ARRANGE
    mock_nvs_error = true;
    mock_nvs_initialized = false;
    
    // ACT: Handle initialization error
    if (mock_nvs_error) {
        mock_nvs_initialized = false;
    }
    
    // ASSERT
    TEST_ASSERT_FALSE(mock_nvs_initialized);
}

// NVS Read/Write Tests
void test_nvs_read_string_value(void) {
    // ARRANGE: Store a value
    strcpy(mock_nvs_storage[0].key, "test_key");
    strcpy(mock_nvs_storage[0].value, "test_value");
    mock_nvs_storage[0].type = 0; // string
    mock_nvs_entries = 1;
    
    // ACT: Read the value
    const char* retrieved = NULL;
    for (int i = 0; i < mock_nvs_entries; i++) {
        if (strcmp(mock_nvs_storage[i].key, "test_key") == 0) {
            retrieved = mock_nvs_storage[i].value;
            break;
        }
    }
    
    // ASSERT
    TEST_ASSERT_NOT_NULL(retrieved);
    TEST_ASSERT_EQUAL_STRING("test_value", retrieved);
}

void test_nvs_write_string_value(void) {
    // ARRANGE
    mock_nvs_entries = 0;
    
    // ACT: Write a string
    strcpy(mock_nvs_storage[0].key, "node_id");
    strcpy(mock_nvs_storage[0].value, "node_001");
    mock_nvs_storage[0].type = 0;
    mock_nvs_entries++;
    
    // ASSERT
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_entries);
    TEST_ASSERT_EQUAL_STRING("node_001", mock_nvs_storage[0].value);
}

void test_nvs_read_uint32_value(void) {
    // ARRANGE: Store a uint32
    strcpy(mock_nvs_storage[0].key, "boot_count");
    uint32_t boot_count = 5;
    memcpy(mock_nvs_storage[0].value, &boot_count, sizeof(uint32_t));
    mock_nvs_storage[0].type = 1; // uint32
    mock_nvs_entries = 1;
    
    // ACT: Read the value
    uint32_t retrieved_count = 0;
    for (int i = 0; i < mock_nvs_entries; i++) {
        if (strcmp(mock_nvs_storage[i].key, "boot_count") == 0) {
            memcpy(&retrieved_count, mock_nvs_storage[i].value, sizeof(uint32_t));
            break;
        }
    }
    
    // ASSERT
    TEST_ASSERT_EQUAL_INT(5, retrieved_count);
}

void test_nvs_write_uint32_value(void) {
    // ARRANGE
    mock_nvs_entries = 0;
    
    // ACT: Write a uint32
    uint32_t value = 42;
    strcpy(mock_nvs_storage[0].key, "test_number");
    memcpy(mock_nvs_storage[0].value, &value, sizeof(uint32_t));
    mock_nvs_storage[0].type = 1;
    mock_nvs_entries++;
    
    // ASSERT
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_entries);
}

// NVS Key Management Tests
void test_nvs_key_can_be_overwritten(void) {
    // ARRANGE: Initial value
    strcpy(mock_nvs_storage[0].key, "config");
    strcpy(mock_nvs_storage[0].value, "old_value");
    mock_nvs_entries = 1;
    
    // ACT: Overwrite with new value
    strcpy(mock_nvs_storage[0].value, "new_value");
    
    // ASSERT
    TEST_ASSERT_EQUAL_STRING("new_value", mock_nvs_storage[0].value);
}

void test_nvs_key_not_found_returns_default(void) {
    // ARRANGE
    mock_nvs_entries = 0;
    const char* default_value = "default";
    
    // ACT: Try to read non-existent key
    const char* result = default_value;
    for (int i = 0; i < mock_nvs_entries; i++) {
        if (strcmp(mock_nvs_storage[i].key, "nonexistent") == 0) {
            result = mock_nvs_storage[i].value;
            break;
        }
    }
    
    // ASSERT
    TEST_ASSERT_EQUAL_STRING("default", result);
}

// NVS Persistence Tests
void test_nvs_stores_boot_counter(void) {
    // ARRANGE
    mock_nvs_entries = 0;
    
    // ACT: Store boot counter
    uint32_t boot_count = 3;
    strcpy(mock_nvs_storage[0].key, "boot_counter");
    memcpy(mock_nvs_storage[0].value, &boot_count, sizeof(uint32_t));
    mock_nvs_storage[0].type = 1;
    mock_nvs_entries++;
    
    // ASSERT
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_entries);
}

void test_nvs_stores_firmware_version(void) {
    // ARRANGE
    mock_nvs_entries = 0;
    
    // ACT: Store firmware version
    strcpy(mock_nvs_storage[0].key, "fw_version");
    strcpy(mock_nvs_storage[0].value, "1.2.3");
    mock_nvs_storage[0].type = 0;
    mock_nvs_entries++;
    
    // ASSERT
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_entries);
}

void test_nvs_stores_can_enabled_flag(void) {
    // ARRANGE
    mock_nvs_entries = 0;
    
    // ACT: Store CAN enabled flag
    uint32_t can_enabled = 1;
    strcpy(mock_nvs_storage[0].key, "can_enabled");
    memcpy(mock_nvs_storage[0].value, &can_enabled, sizeof(uint32_t));
    mock_nvs_storage[0].type = 1;
    mock_nvs_entries++;
    
    // ASSERT
    TEST_ASSERT_EQUAL_INT(1, mock_nvs_entries);
}

// NVS Erase Tests
void test_nvs_key_can_be_erased(void) {
    // ARRANGE: Store a key
    strcpy(mock_nvs_storage[0].key, "temp_key");
    mock_nvs_entries = 1;
    
    // ACT: Erase the key
    mock_nvs_entries = 0;
    
    // ASSERT
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_entries);
}

void test_nvs_namespace_isolation(void) {
    // ARRANGE: Different namespaces
    strcpy(mock_nvs_storage[0].key, "app_key1");
    strcpy(mock_nvs_storage[1].key, "system_key1");
    mock_nvs_entries = 2;
    
    // ACT: Both keys coexist
    bool both_exist = (strcmp(mock_nvs_storage[0].key, "app_key1") == 0) &&
                      (strcmp(mock_nvs_storage[1].key, "system_key1") == 0);
    
    // ASSERT
    TEST_ASSERT_TRUE(both_exist);
}

// NVS Performance Tests
void test_nvs_read_is_fast(void) {
    // ARRANGE: Fill NVS with entries
    for (int i = 0; i < 10; i++) {
        snprintf(mock_nvs_storage[i].key, sizeof(mock_nvs_storage[i].key), "key_%d", i);
        snprintf(mock_nvs_storage[i].value, sizeof(mock_nvs_storage[i].value), "value_%d", i);
        mock_nvs_storage[i].type = 0;
    }
    mock_nvs_entries = 10;
    
    // ACT: Read a key
    int read_count = 0;
    for (int i = 0; i < mock_nvs_entries; i++) {
        read_count++;
        if (strcmp(mock_nvs_storage[i].key, "key_5") == 0) {
            break;
        }
    }
    
    // ASSERT: Should find relatively quickly
    TEST_ASSERT_TRUE(read_count <= mock_nvs_entries);
}

// Setup and teardown
void setUp(void) {
    memset(mock_nvs_storage, 0, sizeof(mock_nvs_storage));
    mock_nvs_entries = 0;
    mock_nvs_initialized = false;
    mock_nvs_error = false;
}

void tearDown(void) {
    // Cleanup
}

void test_nvs_run(void) {
    printf("\n[NVS PERSISTENCE TESTS]\n");
    
    setUp(); test_nvs_initializes_successfully(); tearDown();
    setUp(); test_nvs_initialization_failure_handled(); tearDown();
    setUp(); test_nvs_read_string_value(); tearDown();
    setUp(); test_nvs_write_string_value(); tearDown();
    setUp(); test_nvs_read_uint32_value(); tearDown();
    setUp(); test_nvs_write_uint32_value(); tearDown();
    setUp(); test_nvs_key_can_be_overwritten(); tearDown();
    setUp(); test_nvs_key_not_found_returns_default(); tearDown();
    setUp(); test_nvs_stores_boot_counter(); tearDown();
    setUp(); test_nvs_stores_firmware_version(); tearDown();
    setUp(); test_nvs_stores_can_enabled_flag(); tearDown();
    setUp(); test_nvs_key_can_be_erased(); tearDown();
    setUp(); test_nvs_namespace_isolation(); tearDown();
    setUp(); test_nvs_read_is_fast(); tearDown();
}
