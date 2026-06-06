// Parley factory recovery firmware
//
// This firmware lives in the factory partition and is NEVER updated over the air.
// Its entire job is to get a broken or freshly flashed node back into service:
//   1. Read WiFi credentials and node ID from NVS (fall back to defaults if absent)
//   2. Connect to the gateway WiFi AP
//   3. Connect to the MQTT broker on the Pi
//   4. Announce factory mode on system/discovery
//   5. Wait for an OTA command on nodes/<node_id>/ota
//   6. Download the application firmware from the Pi's HTTP server, write to ota_0
//   7. Reboot into the new firmware
//
// Nothing else. No sensor reads, no actuator control, no complex logic.
// The simpler this firmware is, the less likely it is to fail.
//
// Requires ESP-IDF 5.x (ships with espressif32 platform 6.x+).

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>

// --- Build-time defaults -------------------------------------------------------
// These are the fallback values used when NVS has not been provisioned.
// The gateway AP SSID and password must match what the gateway node broadcasts.
// Override at build time via -D flags in platformio.ini if needed.

#ifndef FACTORY_FW_VERSION
#define FACTORY_FW_VERSION "factory-1.0.0"
#endif

#define DEFAULT_WIFI_SSID    "ParleyNet"
#define DEFAULT_WIFI_PASS    "parley-secret"
#define DEFAULT_MQTT_HOST    "192.168.4.2"
#define DEFAULT_MQTT_PORT    1883

// Watchdog timeout. Long enough for HTTP OTA download. WDT is fed inside loops.
#define WDT_TIMEOUT_MS       60000

// How often to publish heartbeat / re-announce on discovery
#define HEARTBEAT_INTERVAL_MS   10000
#define DISCOVERY_INTERVAL_MS   30000

// Maximum WiFi connection attempts before rebooting
#define WIFI_MAX_ATTEMPTS    40

// HTTP read buffer for OTA streaming
#define OTA_BUF_SIZE         1024

// --- NVS keys -----------------------------------------------------------------
#define NVS_NAMESPACE    "parley"
#define NVS_WIFI_SSID    "wifi_ssid"
#define NVS_WIFI_PASS    "wifi_pass"
#define NVS_MQTT_HOST    "mqtt_host"
#define NVS_NODE_ID      "node_id"
#define NVS_BOOT_COUNT   "boot_count"

// Attempts with NVS credentials before falling back to hardcoded defaults.
// Per the spec, factory firmware must not give up — it falls back to baked-in
// credentials rather than rebooting, because NVS creds may be corrupt.
#define WIFI_NVS_ATTEMPTS       20
#define WIFI_FALLBACK_DELAY_MS  30000

// --- Module state -------------------------------------------------------------
static char wifi_ssid[64];
static char wifi_pass[64];
static char mqtt_host[64];
static int  mqtt_port = DEFAULT_MQTT_PORT;
static char node_id[32];

// Pre-built topic strings
static char topic_status[80];
static char topic_ota[80];
static char topic_provision[80];
static const char topic_discovery[]   = "system/discovery";
static const char topic_registered[]  = "system/registered";

// True if node_id was loaded from NVS (assigned by the Pi during a previous
// registration). False if it was auto-derived from the MAC address, meaning
// this node has never been provisioned. The distinction controls the status
// field in discovery announcements so the Pi and AI can tell the difference
// between a brand-new board and a known node that fell back to factory.
static bool node_id_assigned = false;

static WiFiClient  wifi_client;
static PubSubClient mqtt(wifi_client);

static unsigned long last_heartbeat  = 0;
static unsigned long last_discovery  = 0;
static bool ota_in_progress = false;

// Captured once in setup() and included in every heartbeat so the Pi/AI can
// see why the node last restarted without needing to SSH in.
static esp_reset_reason_t boot_reset_reason;
static const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_watchdog";
        case ESP_RST_TASK_WDT:  return "task_watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        default:                return "unknown";
    }
}

// Boot counter read from NVS — reported in heartbeats so the Pi can detect
// climbing counters. Factory firmware reads it but does not modify it;
// the template manages the counter for application firmware.
static uint32_t boot_counter_value = 0;

static void read_boot_counter() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    boot_counter_value = prefs.getUInt(NVS_BOOT_COUNT, 0);
    prefs.end();
}

// --- Config -------------------------------------------------------------------

static void load_config() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);

    if (!prefs.getString(NVS_WIFI_SSID, wifi_ssid, sizeof(wifi_ssid))) {
        strlcpy(wifi_ssid, DEFAULT_WIFI_SSID, sizeof(wifi_ssid));
    }
    if (!prefs.getString(NVS_WIFI_PASS, wifi_pass, sizeof(wifi_pass))) {
        strlcpy(wifi_pass, DEFAULT_WIFI_PASS, sizeof(wifi_pass));
    }
    if (!prefs.getString(NVS_MQTT_HOST, mqtt_host, sizeof(mqtt_host))) {
        strlcpy(mqtt_host, DEFAULT_MQTT_HOST, sizeof(mqtt_host));
    }
    if (prefs.getString(NVS_NODE_ID, node_id, sizeof(node_id))) {
        node_id_assigned = true;
    } else {
        // No assigned ID yet — derive one from the last 3 octets of MAC so the
        // Pi can address this node before registration assigns a real ID.
        uint8_t mac[6];
        WiFi.macAddress(mac);
        snprintf(node_id, sizeof(node_id), "node-%02x%02x%02x", mac[3], mac[4], mac[5]);
        node_id_assigned = false;
    }

    prefs.end();

    // Rebuild all topics from (possibly updated) node_id.
    snprintf(topic_status,    sizeof(topic_status),    "nodes/%s/status",    node_id);
    snprintf(topic_ota,       sizeof(topic_ota),       "nodes/%s/ota",       node_id);
    snprintf(topic_provision, sizeof(topic_provision), "nodes/%s/provision", node_id);
}

// --- WiFi ---------------------------------------------------------------------
// Tries NVS-loaded credentials first (WIFI_NVS_ATTEMPTS * 500ms).
// If that fails, waits WIFI_FALLBACK_DELAY_MS then switches to hardcoded
// defaults. This handles NVS corruption or stale credentials without rebooting.
// Only reboots if the fallback credentials also fail — at that point the gateway
// itself is likely unreachable and a reboot is the right move.

static bool try_connect(const char* ssid, const char* pass, int attempts) {
    WiFi.disconnect(true);
    WiFi.begin(ssid, pass);
    for (int i = 0; i < attempts; i++) {
        if (WiFi.status() == WL_CONNECTED) return true;
        delay(500);
        esp_task_wdt_reset();
    }
    return WiFi.status() == WL_CONNECTED;
}

static void connect_wifi() {
    WiFi.mode(WIFI_STA);

    Serial.printf("[wifi] trying NVS credentials: %s\n", wifi_ssid);
    if (try_connect(wifi_ssid, wifi_pass, WIFI_NVS_ATTEMPTS)) {
        Serial.printf("[wifi] connected via NVS creds. IP: %s\n",
                      WiFi.localIP().toString().c_str());
        return;
    }

    // NVS creds failed. If they match the hardcoded defaults, there is nothing
    // more to try — reboot and hope something changes.
    if (strcmp(wifi_ssid, DEFAULT_WIFI_SSID) == 0) {
        Serial.println("[wifi] NVS creds are the defaults and failed — rebooting");
        esp_restart();
    }

    Serial.printf("[wifi] NVS creds failed. Waiting %ds before trying hardcoded fallback...\n",
                  WIFI_FALLBACK_DELAY_MS / 1000);
    unsigned long wait_start = millis();
    while (millis() - wait_start < WIFI_FALLBACK_DELAY_MS) {
        delay(500);
        esp_task_wdt_reset();
    }

    Serial.printf("[wifi] trying hardcoded fallback: %s\n", DEFAULT_WIFI_SSID);
    if (try_connect(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS, WIFI_MAX_ATTEMPTS)) {
        Serial.printf("[wifi] connected via fallback. IP: %s\n",
                      WiFi.localIP().toString().c_str());
        return;
    }

    Serial.println("[wifi] fallback also failed — rebooting");
    esp_restart();
}

// --- MQTT publish helpers -----------------------------------------------------

static void publish_discovery() {
    JsonDocument doc;
    doc["node_id"] = node_id;
    // "needs_provisioning" = brand-new board, never registered.
    // "factory"             = previously-registered node in recovery mode.
    // The Pi and AI use this to decide whether to start a registration
    // conversation or simply push the last-known-good application firmware.
    doc["status"]  = node_id_assigned ? "factory" : "needs_provisioning";
    doc["fw"]      = FACTORY_FW_VERSION;
    doc["mac"]     = WiFi.macAddress();
    doc["ip"]      = WiFi.localIP().toString();

    char buf[256];
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(topic_discovery, buf, /*retain=*/true);
}

// --- Provisioning -------------------------------------------------------------
// The Pi sends a provision command to nodes/<node_id>/provision during Phase 5
// of the registration workflow to assign a stable human-readable node ID.
//
// Payload JSON:
//   {
//     "node_id":  "imu-01",          // required — stable ID for this node
//     "mqtt_host": "192.168.4.2"     // optional — update stored broker IP
//   }
//
// On receipt the node saves the new ID to NVS, rebuilds its topics, and
// re-announces on system/discovery with status "factory" (now provisioned).
// The Pi can then push application firmware via the new OTA topic.

static void handle_provision(const uint8_t* payload, unsigned int len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
        Serial.println("[provision] invalid JSON");
        return;
    }

    const char* new_id = doc["node_id"];
    if (!new_id || strlen(new_id) == 0) {
        Serial.println("[provision] missing node_id field");
        return;
    }

    Serial.printf("[provision] assigning node_id: %s\n", new_id);

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_NODE_ID, new_id);

    const char* new_host = doc["mqtt_host"];
    if (new_host && strlen(new_host) > 0) {
        prefs.putString(NVS_MQTT_HOST, new_host);
        strlcpy(mqtt_host, new_host, sizeof(mqtt_host));
        Serial.printf("[provision] updated mqtt_host: %s\n", new_host);
    }

    prefs.end();

    // Unsubscribe from old topics, update state, resubscribe.
    mqtt.unsubscribe(topic_ota);
    mqtt.unsubscribe(topic_provision);

    strlcpy(node_id, new_id, sizeof(node_id));
    node_id_assigned = true;
    load_config();  // rebuilds topic strings from new node_id

    mqtt.subscribe(topic_ota);
    mqtt.subscribe(topic_provision);

    Serial.printf("[provision] now listening on %s and %s\n", topic_ota, topic_provision);

    // Publish retained discovery with new ID and status=factory so the Pi knows
    // the ID was accepted and the node is ready to receive application firmware.
    publish_discovery();

    // Announce the completed provisioning on system/registered so the gateway
    // and Pi can update their registries.
    JsonDocument reg;
    reg["node_id"] = node_id;
    reg["mac"]     = WiFi.macAddress();
    reg["ip"]      = WiFi.localIP().toString();
    reg["fw"]      = FACTORY_FW_VERSION;

    char buf[192];
    serializeJson(reg, buf, sizeof(buf));
    mqtt.publish(topic_registered, buf, false);

    Serial.println("[provision] complete");
}

static void publish_heartbeat() {
    JsonDocument doc;
    doc["node_id"]      = node_id;
    doc["status"]       = node_id_assigned ? "factory" : "needs_provisioning";
    doc["fw"]           = FACTORY_FW_VERSION;
    doc["rssi"]         = WiFi.RSSI();
    doc["uptime_s"]     = millis() / 1000;
    doc["free_heap"]    = esp_get_free_heap_size();
    doc["boot_count"]   = boot_counter_value;
    doc["reset_reason"] = reset_reason_str(boot_reset_reason);

    char buf[256];
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(topic_status, buf, /*retain=*/false);
}

static void publish_ota_status(const char* state, const char* detail = nullptr) {
    JsonDocument doc;
    doc["node_id"] = node_id;
    doc["ota"]     = state;
    if (detail) doc["detail"] = detail;

    char buf[192];
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(topic_status, buf, /*retain=*/false);
    mqtt.loop();
}

// --- OTA ----------------------------------------------------------------------
// Firmware URL arrives as JSON on nodes/<node_id>/ota:
//   {"url": "http://192.168.4.2:8080/firmware/app_v1.2.3.bin", "version": "1.2.3"}
//
// The binary is streamed from the Pi's HTTP server and written directly into the
// ota_0 partition using the ESP-IDF OTA write API. Factory firmware always targets
// ota_0 — no slot selection logic needed at this level.

static void handle_ota(const uint8_t* payload, unsigned int len) {
    if (ota_in_progress) return;

    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
        Serial.println("[ota] invalid JSON payload");
        return;
    }

    const char* url = doc["url"];
    if (!url || strlen(url) == 0) {
        Serial.println("[ota] missing 'url' field");
        return;
    }

    Serial.printf("[ota] starting update from %s\n", url);
    ota_in_progress = true;
    publish_ota_status("starting", url);

    // Always write to ota_0
    const esp_partition_t* target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);

    if (!target) {
        Serial.println("[ota] ota_0 partition not found");
        publish_ota_status("error", "ota_0 not found");
        ota_in_progress = false;
        return;
    }

    HTTPClient http;
    http.begin(url);
    http.setTimeout(60000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[ota] HTTP GET failed, code %d\n", code);
        char detail[32];
        snprintf(detail, sizeof(detail), "http_code_%d", code);
        publish_ota_status("error", detail);
        http.end();
        ota_in_progress = false;
        return;
    }

    int total = http.getSize();
    Serial.printf("[ota] content size: %d bytes\n", total);

    esp_ota_handle_t handle;
    esp_err_t ret = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (ret != ESP_OK) {
        Serial.printf("[ota] esp_ota_begin failed: %s\n", esp_err_to_name(ret));
        publish_ota_status("error", "ota_begin_failed");
        http.end();
        ota_in_progress = false;
        return;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[OTA_BUF_SIZE];
    int written = 0;

    while (http.connected() && (total < 0 || written < total)) {
        int available = stream->available();
        if (available > 0) {
            int n = stream->readBytes(buf, min(available, (int)sizeof(buf)));
            ret = esp_ota_write(handle, buf, n);
            if (ret != ESP_OK) {
                Serial.printf("[ota] write error at byte %d: %s\n", written, esp_err_to_name(ret));
                break;
            }
            written += n;
        }
        esp_task_wdt_reset();
    }

    http.end();

    if (esp_ota_end(handle) != ESP_OK) {
        Serial.println("[ota] esp_ota_end failed (image corrupt or incomplete)");
        publish_ota_status("error", "ota_end_failed");
        ota_in_progress = false;
        return;
    }

    ret = esp_ota_set_boot_partition(target);
    if (ret != ESP_OK) {
        Serial.printf("[ota] set_boot_partition failed: %s\n", esp_err_to_name(ret));
        publish_ota_status("error", "set_boot_failed");
        ota_in_progress = false;
        return;
    }

    Serial.printf("[ota] complete — %d bytes written. rebooting.\n", written);
    publish_ota_status("complete");
    delay(500);
    esp_restart();
}

// --- MQTT callback ------------------------------------------------------------

static void mqtt_callback(char* topic, uint8_t* payload, unsigned int len) {
    if (strcmp(topic, topic_ota) == 0) {
        handle_ota(payload, len);
    } else if (strcmp(topic, topic_provision) == 0) {
        handle_provision(payload, len);
    }
}

// --- MQTT connection ----------------------------------------------------------
// Returns true on success. Does NOT reboot on failure - the spec says factory
// firmware should be patient. The loop() calls this again when !mqtt.connected().
// Rebooting on MQTT failure would cause a reboot-loop any time the Pi is slow
// to start Mosquitto, which is normal at boot.

static bool connect_mqtt() {
    if (WiFi.status() != WL_CONNECTED) return false;

    mqtt.setServer(mqtt_host, mqtt_port);
    mqtt.setCallback(mqtt_callback);
    mqtt.setBufferSize(512);
    mqtt.setSocketTimeout(10);

    // Last will: mark node offline if MQTT connection drops unexpectedly
    char lwt[128];
    snprintf(lwt, sizeof(lwt), "{\"node_id\":\"%s\",\"status\":\"offline\"}", node_id);

    char client_id[48];
    snprintf(client_id, sizeof(client_id), "factory-%s", node_id);

    bool ok = mqtt.connect(
        client_id,
        /*user*/nullptr, /*pass*/nullptr,
        topic_status, /*qos*/0, /*retain*/false, lwt
    );

    if (ok) {
        Serial.println("[mqtt] connected");
        mqtt.subscribe(topic_ota);
        mqtt.subscribe(topic_provision);
    } else {
        Serial.printf("[mqtt] connect failed rc=%d (will retry from loop)\n",
                      mqtt.state());
    }

    return ok;
}

// --- Arduino entry points -----------------------------------------------------

void setup() {
    Serial.begin(115200);
    Serial.printf("\n[parley] factory firmware %s\n", FACTORY_FW_VERSION);

    // Watchdog: panic on timeout (requires IDF 5.x)
    const esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = WDT_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(nullptr);

    // Capture reset reason before anything else modifies state.
    boot_reset_reason = esp_reset_reason();
    Serial.printf("[parley] reset reason: %s\n", reset_reason_str(boot_reset_reason));

    load_config();
    read_boot_counter();
    Serial.printf("[parley] node_id: %s  boot_count: %lu\n", node_id, (unsigned long)boot_counter_value);

    connect_wifi();
    connect_mqtt();  // best-effort; loop() retries if broker isn't ready yet

    if (mqtt.connected()) {
        publish_discovery();
        publish_heartbeat();
    }

    last_heartbeat = millis();
    last_discovery = millis();
}

void loop() {
    esp_task_wdt_reset();

    // Recover lost WiFi
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[wifi] lost - reconnecting");
        connect_wifi();
        return;
    }

    // Reconnect MQTT if lost - throttled to avoid hammering the broker
    if (!mqtt.connected()) {
        static unsigned long last_mqtt_attempt = 0;
        if (millis() - last_mqtt_attempt >= 10000) {
            last_mqtt_attempt = millis();
            Serial.println("[mqtt] not connected - attempting");
            if (connect_mqtt()) {
                publish_discovery();
                publish_heartbeat();
            }
        }
        return;
    }

    mqtt.loop();

    unsigned long now = millis();

    if (now - last_heartbeat >= HEARTBEAT_INTERVAL_MS) {
        publish_heartbeat();
        last_heartbeat = now;
    }

    if (now - last_discovery >= DISCOVERY_INTERVAL_MS) {
        publish_discovery();
        last_discovery = now;
    }
}
