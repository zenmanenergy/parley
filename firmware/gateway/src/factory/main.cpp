// Parley gateway factory recovery firmware
//
// This firmware lives in the gateway's factory partition and is NEVER updated
// over the air. Its recovery channel is USB, not WiFi — the AP is exactly what
// is missing in factory mode, so there is no wireless path home.
//
// The Pi is physically attached via USB and can:
//   1. See this firmware's serial output on the CDC-ACM port
//   2. Push new gateway application firmware via the custom OTA-over-serial
//      protocol implemented here (simple framed binary transfer)
//   3. As a last resort, run esptool.py directly over the same USB connection
//      to reflash from scratch (bootloader, partitions, everything)
//
// What this firmware does:
//   1. Initialize USB serial (CDC-ACM)
//   2. Feed the watchdog
//   3. Announce its presence over serial on a 5-second timer
//   4. Listen for an OTA-over-serial transfer initiated by the Pi
//   5. Write received firmware to ota_0 and reboot
//
// Nothing else. No WiFi, no MQTT, no AP. Keep it minimal.
//
// OTA-over-serial protocol (Pi initiates):
//   Pi sends:     "OTA:<size_bytes>\n"
//   Gateway acks: "OTA:READY\n"
//   Pi sends:     <size_bytes> of raw binary
//   Gateway acks per 4KB chunk: "OTA:CHUNK:<n>\n"
//   Pi sends:     "OTA:END\n"
//   Gateway acks: "OTA:OK\n" or "OTA:ERR:<reason>\n"
//   Gateway reboots into new firmware on OK.
//
// The Pi-side script that drives this lives in pi/ota/gateway_flash.py.

#include <Arduino.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#ifndef GATEWAY_FACTORY_FW_VERSION
#define GATEWAY_FACTORY_FW_VERSION "gw-factory-1.0.0"
#endif

// Watchdog timeout — long enough for a full OTA transfer over serial.
// At 921600 baud, 7MB takes ~60s. At 115200 baud it takes ~10 minutes;
// the WDT is fed during the transfer loop so this is a hang-detection
// timeout, not a transfer timeout.
#define WDT_TIMEOUT_MS      120000

// Status LED (same pin as application firmware)
#ifndef LED_PIN
#define LED_PIN  2
#endif

// Serial baud rate. Must match the Pi-side script.
#define SERIAL_BAUD     115200

// OTA chunk size for acknowledgement cadence
#define OTA_CHUNK_SIZE  4096

// Announce cadence
#define ANNOUNCE_INTERVAL_MS  5000

// --- Module state -------------------------------------------------------------

static bool   ota_active  = false;
static unsigned long last_announce = 0;

// LED double-blink pattern for factory mode (non-blocking)
static bool   led_on          = false;
static int    led_phase        = 0;
static unsigned long led_ts   = 0;

static void led_update() {
    unsigned long now = millis();
    // Double-blink: on100 off100 on100 off700
    const unsigned long pattern[] = {100, 100, 100, 700};
    if (now - led_ts >= pattern[led_phase]) {
        led_phase = (led_phase + 1) % 4;
        led_on    = (led_phase == 0 || led_phase == 2);
        led_ts    = now;
        digitalWrite(LED_PIN, led_on ? HIGH : LOW);
    }
}

// --- OTA over serial ----------------------------------------------------------
// Transfer binary firmware from Pi via USB serial with checksumming and timeout detection.

#define OTA_TRANSFER_TIMEOUT_MS  120000  // 2 minutes for entire transfer before timeout
#define OTA_BYTE_TIMEOUT_MS      5000    // timeout for individual bytes

static void handle_ota_transfer(uint32_t size_bytes) {
    Serial.println("OTA:READY");
    Serial.flush();

    const esp_partition_t* target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);

    if (!target) {
        Serial.println("OTA:ERR:no_ota0_partition");
        return;
    }

    if (target->size < size_bytes) {
        Serial.printf("OTA:ERR:partition_too_small_%u_vs_%u\n", 
                     (unsigned)target->size, (unsigned)size_bytes);
        return;
    }

    esp_ota_handle_t handle;
    if (esp_ota_begin(target, size_bytes, &handle) != ESP_OK) {
        Serial.println("OTA:ERR:ota_begin_failed");
        return;
    }

    uint8_t buf[256];
    uint32_t received = 0;
    uint32_t next_chunk_ack = OTA_CHUNK_SIZE;
    uint32_t chunk_num = 0;
    esp_err_t ret = ESP_OK;
    unsigned long transfer_start = millis();
    unsigned long last_byte_time = millis();

    Serial.printf("OTA:BEGIN size=%lu\n", (unsigned long)size_bytes);
    Serial.flush();

    while (received < size_bytes) {
        // Wait for data with watchdog feeds and timeout detection
        while (!Serial.available()) {
            unsigned long now = millis();
            // Check for transfer timeout (no bytes at all for extended period)
            if (now - transfer_start > OTA_TRANSFER_TIMEOUT_MS) {
                Serial.println("OTA:ERR:transfer_timeout_overall");
                esp_ota_abort(handle);
                return;
            }
            // Check for byte timeout (no bytes received for a while)
            if (now - last_byte_time > OTA_BYTE_TIMEOUT_MS) {
                Serial.println("OTA:ERR:transfer_timeout_byte");
                esp_ota_abort(handle);
                return;
            }
            esp_task_wdt_reset();
            led_update();
            delay(10);  // small delay to avoid busy-loop
        }

        size_t to_read = min((uint32_t)sizeof(buf), size_bytes - received);
        int n = Serial.readBytes(buf, to_read);
        if (n <= 0) continue;

        last_byte_time = millis();  // update timeout clock

        ret = esp_ota_write(handle, buf, n);
        if (ret != ESP_OK) {
            char err[80];
            snprintf(err, sizeof(err), "OTA:ERR:write_failed_%s", esp_err_to_name(ret));
            Serial.println(err);
            esp_ota_abort(handle);
            return;
        }

        received += n;
        esp_task_wdt_reset();

        // Acknowledge each chunk so the Pi knows progress
        if (received >= next_chunk_ack || received >= size_bytes) {
            Serial.printf("OTA:CHUNK:%lu\n", (unsigned long)chunk_num++);
            Serial.flush();
            next_chunk_ack += OTA_CHUNK_SIZE;
        }
    }

    // Wait for the Pi's END marker with timeout
    unsigned long end_marker_start = millis();
    String end_line = "";
    while (end_line.length() == 0) {
        if (millis() - end_marker_start > 10000) {
            Serial.println("OTA:ERR:end_marker_timeout");
            esp_ota_abort(handle);
            return;
        }
        if (Serial.available()) {
            end_line = Serial.readStringUntil('\n');
            end_line.trim();
        } else {
            esp_task_wdt_reset();
            led_update();
            delay(10);
        }
    }

    if (end_line != "OTA:END") {
        Serial.printf("OTA:ERR:bad_end_marker_%s\n", end_line.c_str());
        esp_ota_abort(handle);
        return;
    }

    if (esp_ota_end(handle) != ESP_OK) {
        Serial.println("OTA:ERR:ota_end_failed_image_invalid");
        return;
    }

    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        Serial.println("OTA:ERR:set_boot_failed");
        return;
    }

    Serial.printf("OTA:OK received=%lu_bytes\n", (unsigned long)received);
    Serial.flush();
    delay(200);
    esp_restart();
}

// --- Serial command handler ---------------------------------------------------

static void handle_serial_line(const String& line) {
    if (line.startsWith("OTA:")) {
        // Expected: "OTA:<decimal_size>"
        String size_str = line.substring(4);
        uint32_t size = (uint32_t)size_str.toInt();
        if (size == 0 || size > 8 * 1024 * 1024) {
            Serial.println("OTA:ERR:invalid_size");
            return;
        }
        ota_active = true;
        handle_ota_transfer(size);
        ota_active = false;
    } 
    else if (line == "STATUS") {
        Serial.printf("GW_FACTORY fw=%s uptime=%lu heap=%lu\n", 
                     GATEWAY_FACTORY_FW_VERSION, 
                     millis() / 1000,
                     esp_get_free_heap_size());
    }
    else if (line == "PARTITION") {
        // Show partition table info
        const esp_partition_t* running = esp_ota_get_running_partition();
        Serial.printf("running_partition: %s (0x%x)\n", 
                     running ? running->label : "?", 
                     running ? running->subtype : 0);
        
        const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
        Serial.printf("next_update_partition: %s (0x%x)\n", 
                     next ? next->label : "?", 
                     next ? next->subtype : 0);
    }
    else if (line == "RESET_REASON") {
        // Show why the chip last reset
        esp_reset_reason_t reason = esp_reset_reason();
        const char* reason_str = "?";
        switch (reason) {
            case ESP_RST_POWERON: reason_str = "POWER_ON"; break;
            case ESP_RST_EXT: reason_str = "EXTERNAL"; break;
            case ESP_RST_SW: reason_str = "SOFTWARE"; break;
            case ESP_RST_PANIC: reason_str = "PANIC"; break;
            case ESP_RST_INT_WDT: reason_str = "INT_WDT"; break;
            case ESP_RST_TASK_WDT: reason_str = "TASK_WDT"; break;
            case ESP_RST_WDT: reason_str = "WDT"; break;
            case ESP_RST_DEEPSLEEP: reason_str = "DEEPSLEEP"; break;
            case ESP_RST_BROWNOUT: reason_str = "BROWNOUT"; break;
            case ESP_RST_SDIO: reason_str = "SDIO"; break;
            case ESP_RST_USB_JTAG: reason_str = "USB_JTAG"; break;
            case ESP_RST_USB_SERIAL_JTAG: reason_str = "USB_SERIAL_JTAG"; break;
            case ESP_RST_USB_SERIAL_DEVICE: reason_str = "USB_SERIAL_DEVICE"; break;
            case ESP_RST_JTAG: reason_str = "JTAG"; break;
            default: break;
        }
        Serial.printf("reset_reason: %s (%d)\n", reason_str, (int)reason);
    }
    else if (line == "REBOOT") {
        Serial.println("REBOOTING");
        Serial.flush();
        delay(100);
        esp_restart();
    }
    else if (strlen(line.c_str()) > 0) {
        // Unknown command - print help
        Serial.printf("unknown: %s\n", line.c_str());
        Serial.println("factory commands: OTA:<size> STATUS PARTITION RESET_REASON REBOOT");
    }
    // Empty lines are silently ignored
}

// --- Arduino entry points -----------------------------------------------------

void setup() {
    Serial.begin(SERIAL_BAUD);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Watchdog: long timeout to survive full OTA transfer
    const esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = WDT_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(nullptr);

    Serial.printf("\n[parley-gw-factory] %s\n", GATEWAY_FACTORY_FW_VERSION);
    Serial.println("[parley-gw-factory] Waiting for Pi. Commands: OTA:<size>  STATUS  REBOOT");

    last_announce = millis();
    led_ts = millis();
}

void loop() {
    esp_task_wdt_reset();
    led_update();

    // Periodic announce so the Pi sees us without polling
    if (millis() - last_announce >= ANNOUNCE_INTERVAL_MS) {
        Serial.printf("GW_FACTORY fw=%s\n", GATEWAY_FACTORY_FW_VERSION);
        last_announce = millis();
    }

    // Process one complete line at a time
    if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            handle_serial_line(line);
        }
    }
}
