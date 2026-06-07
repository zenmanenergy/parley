#include "unity_stub.h"

// Forward declarations for all test groups
void test_recovery_cascade_run(void);
void test_network_run(void);
void test_logging_run(void);
void test_can_bus_run(void);
void test_nvs_run(void);

int main(void) {
    UNITY_BEGIN();
    
    // Run all test groups
    test_recovery_cascade_run();
    test_network_run();
    test_logging_run();
    test_can_bus_run();
    test_nvs_run();
    
    UNITY_END();
}
