// Parley gateway node firmware
//
// The gateway has four jobs:
//   1. WiFi access point — peripheral nodes connect here (192.168.4.0/24)
//   2. USB CDC-ECM — presents a network interface to the Pi (Pi gets 192.168.4.2)
//   3. IP routing — forwards packets between the AP subnet and the USB link so
//      peripheral nodes can reach the Pi's MQTT broker transparently
//   4. Provisioning — listens on system/discovery and maintains the node registry
//
// Network topology:
//   Peripheral nodes:  192.168.4.10 – 192.168.4.100  (DHCP from this gateway)
//   Gateway AP IP:     192.168.4.1
//   Pi (usb0):         192.168.4.2  (static, configured on Pi side)
//   Pi MQTT broker:    192.168.4.2:1883
//
// Peripheral nodes connect to the gateway AP and reach the Pi's broker at
// 192.168.4.2 — they never know the Pi is a separate device.
//
// USB CDC-ECM setup: The ESP32-S3 native USB port presents itself as a USB
// Ethernet adapter. On the Pi, this appears as usb0 and should be assigned
// the static IP 192.168.4.2 via /etc/network/interfaces or NetworkManager.
// The Pi's Mosquitto broker must bind on 0.0.0.0 or specifically 192.168.4.2.
//
// NOTE: CONFIG_LWIP_IP_FORWARD and CONFIG_LWIP_IPV4_NAPT must be enabled
// in sdkconfig.defaults (already done) for IP routing to function.

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#include <lwip/lwip_napt.h>

// --- Build-time defaults -------------------------------------------------------

#ifndef GATEWAY_FW_VERSION
#define GATEWAY_FW_VERSION "gateway-1.0.0"
#endif

// WiFi AP credentials — nodes have these baked into their factory firmware
#define DEFAULT_AP_SSID     "ParleyNet"
#define DEFAULT_AP_PASS     "parley-secret"
#define DEFAULT_AP_CHANNEL  6
// The ESP32 AP supports up to 8 simultaneous stations by default (SDK limit).
#define AP_MAX_CLIENTS      8

// IP addressing — must match what the Pi is configured to use on usb0
#define AP_IP           "192.168.4.1"
#define AP_GATEWAY      "192.168.4.1"
#define AP_SUBNET       "255.255.255.0"
#define DEFAULT_PI_IP   "192.168.4.2"   // Pi's static IP on the USB interface
#define DEFAULT_MQTT_PORT  1883

// Watchdog
#define WDT_TIMEOUT_MS      30000

// Validation gate: gateway must confirm AP + MQTT healthy within this window.
// Matches the per-node template default.
#define VALIDATION_WINDOW_MS    60000

// After this long of healthy operation the boot counter resets to zero.
// Matches the per-node template stability period.
#define STABILITY_PERIOD_MS     300000

// Boot counter threshold before the template falls back to factory mode.
// Stored in NVS under the same namespace peripheral nodes use.
#define BOOT_COUNT_THRESHOLD    5

// Status LED GPIO. Change to match your board.
// Common defaults: 2 (generic DevKit), 47 (S3 DevKitC-1 RGB via single colour).
#ifndef LED_PIN
#define LED_PIN  2
#endif

// Heartbeat cadence
#define HEARTBEAT_INTERVAL_MS       15000
#define REGISTRY_SAVE_INTERVAL_MS   60000

// A node whose heartbeat has been silent for this long is flagged on anomalies.
// Matches the spec value: 2 minutes.
#define NODE_SILENCE_THRESHOLD_S    120

// A node whose boot counter reaches this value is flagged as flaky.
// Matches the per-node template default threshold.
#define BOOT_COUNT_FLAKY_THRESHOLD  3

// How often the gateway scans for silent / flaky nodes (seconds resolution).
#define HEALTH_CHECK_INTERVAL_MS    30000

// Node registry file on LittleFS
#define REGISTRY_PATH   "/node_registry.json"

// MQTT topics
#define TOPIC_DISCOVERY     "system/discovery"
#define TOPIC_ANOMALIES     "system/anomalies"
#define TOPIC_REGISTERED    "system/registered"

// --- NVS keys -----------------------------------------------------------------
#define NVS_NAMESPACE   "parley_gw"
#define NVS_AP_SSID     "ap_ssid"
#define NVS_AP_PASS     "ap_pass"
#define NVS_PI_IP       "pi_ip"
#define NVS_BOOT_COUNT  "boot_count"

// --- Module state -------------------------------------------------------------

static char ap_ssid[64];
static char ap_pass[64];
static char pi_ip[24];
static int  mqtt_port = DEFAULT_MQTT_PORT;

static char topic_gw_status[64];
static const char topic_gw_id[] = "gateway";

static WiFiClient   wifi_client;
static PubSubClient mqtt(wifi_client);

static unsigned long last_heartbeat    = 0;
static unsigned long last_registry_save = 0;
static unsigned long last_health_check  = 0;

// Boot counter and validation gate state.
static uint32_t boot_counter_value = 0;
static bool     fw_validated       = false;  // set once after AP+MQTT confirmed
static bool     fw_stable          = false;  // set after stability period
static unsigned long boot_time_ms  = 0;

// --- Status LED ---------------------------------------------------------------
// Non-blocking blink state machine. Pattern is updated by set_led_state().
// States per spec:
//   BOOTING        fast blink 5Hz   (100ms on, 100ms off)
//   AP_UP_NO_MQTT  slow blink 1Hz   (500ms on, 500ms off)
//   OPERATIONAL    solid on
//   FACTORY        double-blink     (on 100, off 100, on 100, off 700)
//   ERROR          rapid flash 10Hz (50ms on, 50ms off)

enum LedState { LED_BOOTING, LED_AP_UP, LED_OPERATIONAL, LED_FACTORY, LED_ERROR };
static LedState led_state = LED_BOOTING;
static bool     led_pin_on = false;
static unsigned long led_last_change = 0;
static int      led_phase = 0;  // for multi-step patterns (double-blink)

static void led_init() {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
}

static void set_led_state(LedState s) {
    if (s != led_state) {
        led_state = s;
        led_phase = 0;
        led_last_change = millis();
    }
}

static void led_update() {
    unsigned long now = millis();
    unsigned long elapsed = now - led_last_change;

    switch (led_state) {
        case LED_BOOTING:
            // 5 Hz: 100ms on / 100ms off
            if (elapsed >= 100) {
                led_pin_on = !led_pin_on;
                led_last_change = now;
            }
            break;

        case LED_AP_UP:
            // 1 Hz: 500ms on / 500ms off
            if (elapsed >= 500) {
                led_pin_on = !led_pin_on;
                led_last_change = now;
            }
            break;

        case LED_OPERATIONAL:
            led_pin_on = true;
            break;

        case LED_FACTORY: {
            // Double-blink: on100 off100 on100 off700
            const unsigned long pattern[] = {100, 100, 100, 700};
            if (elapsed >= pattern[led_phase]) {
                led_phase = (led_phase + 1) % 4;
                led_pin_on = (led_phase == 0 || led_phase == 2);
                led_last_change = now;
            }
            break;
        }

        case LED_ERROR:
            // 10 Hz: 50ms on / 50ms off
            if (elapsed >= 50) {
                led_pin_on = !led_pin_on;
                led_last_change = now;
            }
            break;
    }

    digitalWrite(LED_PIN, led_pin_on ? HIGH : LOW);
}: mac -> {node_id, last_seen, fw, status}
// Stored as a simple JsonDocument. Not suitable for hundreds of nodes, but
// fine for the scale Parley targets (sub-100 nodes).
static JsonDocument registry_doc;
static bool registry_dirty = false;

// --- Boot counter -------------------------------------------------------------
// The same boot counter logic that applies to all peripheral nodes also applies
// to the gateway. The counter lives in NVS and is managed here since the
// universal template hasn't been built yet.

static void boot_counter_init() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);

    esp_reset_reason_t reason = esp_reset_reason();
    // Only increment for software-attributable failures (matches the filter
    // table in 03-recovery-and-resilience.md).
    bool count_it = (reason == ESP_RST_PANIC    ||
                     reason == ESP_RST_INT_WDT  ||
                     reason == ESP_RST_TASK_WDT ||
                     reason == ESP_RST_WDT      ||
                     reason == ESP_RST_SW);

    if (reason == ESP_RST_POWERON) {
        // Clean power-on: reset counter rather than incrementing.
        prefs.putUInt(NVS_BOOT_COUNT, 0);
        boot_counter_value = 0;
    } else if (count_it) {
        boot_counter_value = prefs.getUInt(NVS_BOOT_COUNT, 0) + 1;
        prefs.putUInt(NVS_BOOT_COUNT, boot_counter_value);
    } else {
        boot_counter_value = prefs.getUInt(NVS_BOOT_COUNT, 0);
    }

    prefs.end();

    Serial.printf("[boot] reset_reason=%d  boot_count=%lu\n",
                  (int)reason, (unsigned long)boot_counter_value);

    // If boot counter exceeds threshold, fall back to factory partition.
    if (boot_counter_value > BOOT_COUNT_THRESHOLD) {
        Serial.println("[boot] boot count exceeded threshold — entering factory mode");
        Preferences p2;
        p2.begin(NVS_NAMESPACE, false);
        p2.putUInt(NVS_BOOT_COUNT, 0);
        p2.end();

        const esp_partition_t* factory = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
        if (factory) {
            esp_ota_set_boot_partition(factory);
        }
        esp_restart();
    }
}

static void boot_counter_reset() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt(NVS_BOOT_COUNT, 0);
    prefs.end();
    boot_counter_value = 0;
    Serial.println("[boot] boot counter reset after stability period");
}

// Called once AP is up and MQTT is connected. Marks the firmware valid so
// the bootloader will not roll back on the next reboot.
static void try_validate() {
    if (fw_validated) return;
    if (millis() - boot_time_ms > VALIDATION_WINDOW_MS) {
        // Window expired without validating — rollback will fire on next reboot.
        Serial.println("[boot] validation window expired without confirming health");
        return;
    }
    esp_ota_mark_app_valid_cancel_rollback();
    fw_validated = true;
    Serial.println("[boot] firmware validated — rollback cancelled");
}

// --- Config -------------------------------------------------------------------

static void load_config() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);

    if (!prefs.getString(NVS_AP_SSID, ap_ssid, sizeof(ap_ssid))) {
        strlcpy(ap_ssid, DEFAULT_AP_SSID, sizeof(ap_ssid));
    }
    if (!prefs.getString(NVS_AP_PASS, ap_pass, sizeof(ap_pass))) {
        strlcpy(ap_pass, DEFAULT_AP_PASS, sizeof(ap_pass));
    }
    if (!prefs.getString(NVS_PI_IP, pi_ip, sizeof(pi_ip))) {
        strlcpy(pi_ip, DEFAULT_PI_IP, sizeof(pi_ip));
    }

    prefs.end();

    snprintf(topic_gw_status, sizeof(topic_gw_status), "nodes/%s/status", topic_gw_id);
}

// --- WiFi AP ------------------------------------------------------------------

static void start_ap() {
    WiFi.mode(WIFI_AP);

    IPAddress ip, gw, sn;
    ip.fromString(AP_IP);
    gw.fromString(AP_GATEWAY);
    sn.fromString(AP_SUBNET);
    WiFi.softAPConfig(ip, gw, sn);

    bool ok = WiFi.softAP(ap_ssid, ap_pass, DEFAULT_AP_CHANNEL, 0, AP_MAX_CLIENTS);
    if (!ok) {
        Serial.println("[ap] softAP start failed — rebooting");
        set_led_state(LED_ERROR);
        esp_restart();
    }

    Serial.printf("[ap] broadcasting SSID: %s  IP: %s\n",
                  ap_ssid, WiFi.softAPIP().toString().c_str());

    set_led_state(LED_AP_UP);  // AP up, MQTT not yet connected

    // Enable NAT so peripheral nodes (192.168.4.x) can reach the Pi (192.168.4.2)
    // through whichever interface the USB link is on. With IP forwarding enabled
    // in sdkconfig, lwIP will route packets between interfaces.
    ip_napt_enable(ip.v4(), 1);
    Serial.println("[ap] NAT enabled on AP interface");
}

// --- MQTT connection ----------------------------------------------------------

static void mqtt_callback(char* topic, uint8_t* payload, unsigned int len);

static void connect_mqtt() {
    mqtt.setServer(pi_ip, mqtt_port);
    mqtt.setCallback(mqtt_callback);
    mqtt.setBufferSize(1024);
    mqtt.setSocketTimeout(10);

    char lwt[128];
    snprintf(lwt, sizeof(lwt),
             "{\"node_id\":\"gateway\",\"status\":\"offline\"}", topic_gw_id);

    for (int i = 0; i < 10; i++) {
        bool ok = mqtt.connect(
            "parley-gateway",
            nullptr, nullptr,
            topic_gw_status, 0, false, lwt
        );

        if (ok) {
            mqtt.subscribe(TOPIC_DISCOVERY);
            mqtt.subscribe(TOPIC_REGISTERED);
            mqtt.subscribe("nodes/gateway/ota");
            Serial.printf("[mqtt] connected to broker at %s:%d\n", pi_ip, mqtt_port);
            set_led_state(LED_OPERATIONAL);
            try_validate();
            return;
        }

        Serial.printf("[mqtt] connect failed rc=%d, retry %d/10\n", mqtt.state(), i + 1);
        delay(3000);
        esp_task_wdt_reset();
    }

    Serial.println("[mqtt] cannot reach Pi broker — will retry from loop");
}

// --- Registry -----------------------------------------------------------------

static void load_registry() {
    if (!LittleFS.exists(REGISTRY_PATH)) {
        registry_doc.to<JsonObject>();
        Serial.println("[registry] no file found, starting fresh");
        return;
    }

    File f = LittleFS.open(REGISTRY_PATH, "r");
    if (!f) {
        Serial.println("[registry] open failed");
        registry_doc.to<JsonObject>();
        return;
    }

    DeserializationError err = deserializeJson(registry_doc, f);
    f.close();

    if (err) {
        Serial.printf("[registry] parse error: %s — starting fresh\n", err.c_str());
        registry_doc.to<JsonObject>();
    } else {
        Serial.printf("[registry] loaded %d nodes\n", registry_doc.size());
    }
}

static void save_registry() {
    File f = LittleFS.open(REGISTRY_PATH, "w");
    if (!f) {
        Serial.println("[registry] write open failed");
        return;
    }
    serializeJson(registry_doc, f);
    f.close();
    registry_dirty = false;
}

// Called when a node announces itself on system/discovery.
// Updates or creates the registry entry for this MAC address.
static void handle_discovery(const uint8_t* payload, unsigned int len) {
    JsonDocument msg;
    if (deserializeJson(msg, payload, len) != DeserializationError::Ok) {
        Serial.println("[discovery] invalid JSON");
        return;
    }

    const char* mac     = msg["mac"];
    const char* node_id = msg["node_id"];
    const char* status  = msg["status"];
    const char* fw      = msg["fw"];
    const char* ip      = msg["ip"];

    if (!mac || !node_id) {
        Serial.println("[discovery] missing mac or node_id field");
        return;
    }

    Serial.printf("[discovery] node_id=%-20s  mac=%s  status=%s  fw=%s\n",
                  node_id, mac, status ? status : "?", fw ? fw : "?");

    // Upsert into registry keyed by MAC
    JsonObject entry = registry_doc[mac].isNull()
                       ? registry_doc[mac].to<JsonObject>()
                       : registry_doc[mac].as<JsonObject>();

    entry["node_id"]   = node_id;
    entry["status"]    = status  ? status  : "unknown";
    entry["fw"]        = fw      ? fw      : "unknown";
    entry["ip"]        = ip      ? ip      : "unknown";
    entry["last_seen"] = millis() / 1000;

    // Track boot counter so the gateway can flag climbing values.
    if (msg["boot_count"].is<unsigned int>()) {
        entry["boot_count"] = msg["boot_count"].as<unsigned int>();
    }

    registry_dirty = true;

    // Distinguish new unprovisioned boards from known nodes in recovery.
    // "needs_provisioning" triggers a registration conversation on the Pi/AI.
    // "factory" means the node has an assigned ID but fell back to recovery;
    //   the Pi should push the last-known-good application firmware.
    if (status && (strcmp(status, "needs_provisioning") == 0 || strcmp(status, "factory") == 0)) {
        bool needs_reg = (strcmp(status, "needs_provisioning") == 0);

        Serial.printf("[discovery] node %s: %s\n", node_id,
                      needs_reg ? "NEEDS PROVISIONING (new board)" : "FACTORY MODE (recovery)");

        if (mqtt.connected()) {
            JsonDocument anomaly;
            anomaly["type"]    = needs_reg ? "needs_provisioning" : "factory_mode";
            anomaly["node_id"] = node_id;
            anomaly["mac"]     = mac;
            anomaly["ip"]      = ip ? ip : "unknown";

            char buf[256];
            serializeJson(anomaly, buf, sizeof(buf));
            mqtt.publish(TOPIC_ANOMALIES, buf, false);
        }
    }
}

// --- Registration completion --------------------------------------------------
// Published by a node's factory firmware on system/registered when it accepts
// a provisioning command (node_id assignment) from the Pi. The gateway updates
// its local registry entry so the new stable ID is reflected immediately.

static void handle_registered(const uint8_t* payload, unsigned int len) {
    JsonDocument msg;
    if (deserializeJson(msg, payload, len) != DeserializationError::Ok) return;

    const char* mac     = msg["mac"];
    const char* node_id = msg["node_id"];
    if (!mac || !node_id) return;

    Serial.printf("[registered] node_id=%s  mac=%s\n", node_id, mac);

    // Update or create registry entry with the now-assigned node ID.
    JsonObject entry = registry_doc[mac].isNull()
                       ? registry_doc[mac].to<JsonObject>()
                       : registry_doc[mac].as<JsonObject>();

    entry["node_id"]    = node_id;
    entry["status"]     = "factory";   // still in factory; app fw not yet pushed
    entry["registered"] = true;
    entry["last_seen"]  = millis() / 1000;

    registry_dirty = true;

    Serial.printf("[registered] registry updated for %s\n", node_id);
}

// --- Node health scan --------------------------------------------------------
// Runs periodically to detect nodes that have gone silent or show climbing
// boot counters. Publishes anomalies to system/anomalies for the Pi and AI.

static void check_node_health() {
    if (!mqtt.connected()) return;

    uint32_t now_s = millis() / 1000;

    for (JsonPair kv : registry_doc.as<JsonObject>()) {
        JsonObject entry = kv.value().as<JsonObject>();
        const char* nid      = entry["node_id"] | "unknown";
        uint32_t last_seen   = entry["last_seen"] | 0;
        uint32_t boot_count  = entry["boot_count"] | 0;
        const char* status   = entry["status"] | "unknown";
        const char* mac      = kv.key().c_str();

        uint32_t age_s = (now_s > last_seen) ? (now_s - last_seen) : 0;

        // Flag nodes silent for > NODE_SILENCE_THRESHOLD_S
        if (age_s > NODE_SILENCE_THRESHOLD_S) {
            Serial.printf("[health] node %s silent for %lus\n", nid, (unsigned long)age_s);

            JsonDocument anomaly;
            anomaly["type"]      = "node_silent";
            anomaly["node_id"]   = nid;
            anomaly["mac"]       = mac;
            anomaly["silent_s"]  = age_s;
            anomaly["last_status"] = status;

            char buf[256];
            serializeJson(anomaly, buf, sizeof(buf));
            mqtt.publish(TOPIC_ANOMALIES, buf, false);
        }

        // Flag nodes with climbing boot counters (but not factory — their
        // counter is managed by the application template, not factory code).
        if (boot_count >= BOOT_COUNT_FLAKY_THRESHOLD && strcmp(status, "factory") != 0) {
            Serial.printf("[health] node %s boot_count=%lu — flaky\n", nid, (unsigned long)boot_count);

            JsonDocument anomaly;
            anomaly["type"]       = "high_boot_count";
            anomaly["node_id"]    = nid;
            anomaly["mac"]        = mac;
            anomaly["boot_count"] = boot_count;

            char buf[256];
            serializeJson(anomaly, buf, sizeof(buf));
            mqtt.publish(TOPIC_ANOMALIES, buf, false);
        }
    }
}

// --- OTA for the gateway itself -----------------------------------------------
// The Pi can push gateway firmware updates via nodes/gateway/ota using the same
// JSON format as peripheral node OTA: {"url": "http://...", "version": "..."}

static bool ota_in_progress = false;

static void handle_gateway_ota(const uint8_t* payload, unsigned int len) {
    if (ota_in_progress) return;

    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) return;

    const char* url = doc["url"];
    if (!url || strlen(url) == 0) return;

    ota_in_progress = true;
    Serial.printf("[ota] starting gateway update from %s\n", url);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(60000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[ota] HTTP GET failed, code %d\n", code);
        http.end();
        ota_in_progress = false;
        return;
    }

    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (!target) {
        Serial.println("[ota] no update partition available");
        http.end();
        ota_in_progress = false;
        return;
    }

    esp_ota_handle_t handle;
    if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        Serial.println("[ota] esp_ota_begin failed");
        http.end();
        ota_in_progress = false;
        return;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    int total = http.getSize();
    int written = 0;
    esp_err_t ret = ESP_OK;

    while (http.connected() && (total < 0 || written < total)) {
        int available = stream->available();
        if (available > 0) {
            int n = stream->readBytes(buf, min(available, (int)sizeof(buf)));
            ret = esp_ota_write(handle, buf, n);
            if (ret != ESP_OK) break;
            written += n;
        }
        esp_task_wdt_reset();
    }

    http.end();

    if (ret != ESP_OK || esp_ota_end(handle) != ESP_OK) {
        Serial.println("[ota] write or end failed");
        ota_in_progress = false;
        return;
    }

    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        Serial.println("[ota] set_boot_partition failed");
        ota_in_progress = false;
        return;
    }

    Serial.printf("[ota] complete — %d bytes. rebooting.\n", written);
    delay(500);
    esp_restart();
}

// --- MQTT callback ------------------------------------------------------------

static void mqtt_callback(char* topic, uint8_t* payload, unsigned int len) {
    if (strcmp(topic, TOPIC_DISCOVERY) == 0) {
        handle_discovery(payload, len);
    } else if (strcmp(topic, TOPIC_REGISTERED) == 0) {
        handle_registered(payload, len);
    } else if (strcmp(topic, "nodes/gateway/ota") == 0) {
        handle_gateway_ota(payload, len);
    }
}

// --- Diagnostic serial console -----------------------------------------------
// The Pi has direct serial access to the gateway over USB (CDC-ACM). This
// function reads lines from Serial and responds to simple text commands.
// It is the system's last-resort diagnostic channel: if all wireless is broken,
// the Pi-gateway serial link still works.
//
// Commands (send as a line with newline):
//   clients   — list stations currently connected to the AP
//   state     — dump current network/MQTT state
//   registry  — dump the in-memory node registry as JSON
//   reboot    — force an immediate software reboot

static void handle_console() {
    static char line_buf[64];
    static int  line_len = 0;

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n' || line_len >= (int)sizeof(line_buf) - 1) {
            line_buf[line_len] = '\0';
            line_len = 0;

            // Trim leading spaces
            const char* cmd = line_buf;
            while (*cmd == ' ') cmd++;

            if (strcmp(cmd, "clients") == 0) {
                wifi_sta_list_t sta_list;
                esp_wifi_ap_get_sta_list(&sta_list);
                Serial.printf("[console] AP clients: %d\n", sta_list.num);
                for (int i = 0; i < sta_list.num; i++) {
                    const wifi_sta_info_t& s = sta_list.sta[i];
                    Serial.printf("  [%d] %02x:%02x:%02x:%02x:%02x:%02x  rssi=%d\n",
                                  i,
                                  s.mac[0], s.mac[1], s.mac[2],
                                  s.mac[3], s.mac[4], s.mac[5],
                                  s.rssi);
                }
            } else if (strcmp(cmd, "state") == 0) {
                Serial.printf("[console] fw=%s  uptime=%lus\n",
                              GATEWAY_FW_VERSION, millis() / 1000);
                Serial.printf("[console] AP: ssid=%s  clients=%d\n",
                              ap_ssid, WiFi.softAPgetStationNum());
                Serial.printf("[console] MQTT: %s  broker=%s:%d\n",
                              mqtt.connected() ? "connected" : "DISCONNECTED",
                              pi_ip, mqtt_port);
                Serial.printf("[console] registry: %d nodes\n", registry_doc.size());
                Serial.printf("[console] free_heap: %lu bytes\n",
                              (unsigned long)esp_get_free_heap_size());
            } else if (strcmp(cmd, "registry") == 0) {
                serializeJsonPretty(registry_doc, Serial);
                Serial.println();
            } else if (strcmp(cmd, "reboot") == 0) {
                Serial.println("[console] rebooting now");
                delay(100);
                esp_restart();
            } else if (strlen(cmd) > 0) {
                Serial.printf("[console] unknown command: %s\n", cmd);
                Serial.println("[console] commands: clients  state  registry  reboot");
            }
        } else {
            line_buf[line_len++] = c;
        }
    }
}

// --- Status publish -----------------------------------------------------------

static void publish_status() {
    // The gateway is healthy when: AP is broadcasting, MQTT bridge is up.
    // USB connectivity is implied by the fact that we can reach the broker.
    bool ap_ok   = (WiFi.softAPgetStationNum() >= 0);  // AP is up if softAP started
    bool mqtt_ok = mqtt.connected();
    bool healthy = ap_ok && mqtt_ok;

    JsonDocument doc;
    doc["node_id"]      = "gateway";
    doc["fw"]           = GATEWAY_FW_VERSION;
    doc["healthy"]      = healthy;
    doc["ap_ssid"]      = ap_ssid;
    doc["ap_clients"]   = WiFi.softAPgetStationNum();
    doc["mqtt_ok"]      = mqtt_ok;
    doc["pi_ip"]        = pi_ip;
    doc["known_nodes"]  = registry_doc.size();
    doc["uptime_s"]     = millis() / 1000;
    doc["free_heap"]    = esp_get_free_heap_size();
    doc["boot_count"]   = boot_counter_value;
    doc["validated"]    = fw_validated;

    char buf[512];
    serializeJson(doc, buf, sizeof(buf));
    mqtt.publish(topic_gw_status, buf, false);
}

// --- Arduino entry points -----------------------------------------------------

void setup() {
    Serial.begin(115200);
    Serial.printf("\n[parley] gateway firmware %s\n", GATEWAY_FW_VERSION);

    led_init();
    set_led_state(LED_BOOTING);

    const esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = WDT_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
    esp_task_wdt_add(nullptr);

    boot_time_ms = millis();
    boot_counter_init();
    load_config();

    if (!LittleFS.begin(true)) {
        Serial.println("[fs] LittleFS mount failed — rebooting");
        esp_restart();
    }
    load_registry();

    start_ap();

    // The Pi may take a few seconds to bring up usb0 and its MQTT broker.
    // connect_mqtt() will retry from loop() if it fails here.
    connect_mqtt();

    if (mqtt.connected()) {
        publish_status();
    }

    last_heartbeat     = millis();
    last_registry_save = millis();
    last_health_check  = millis();

    Serial.println("[console] ready — commands: clients  state  registry  reboot");
}

void loop() {
    esp_task_wdt_reset();
    led_update();

    // Process any diagnostic commands from the Pi over USB serial
    handle_console();

    // Reconnect MQTT if lost — Pi may have rebooted
    if (!mqtt.connected()) {
        set_led_state(LED_AP_UP);  // AP still up, just lost MQTT
        static unsigned long last_attempt = 0;
        if (millis() - last_attempt >= 10000) {
            last_attempt = millis();
            Serial.println("[mqtt] reconnecting...");
            connect_mqtt();
        }
    } else {
        mqtt.loop();
    }

    unsigned long now = millis();

    if (now - last_heartbeat >= HEARTBEAT_INTERVAL_MS) {
        if (mqtt.connected()) publish_status();
        last_heartbeat = now;
    }

    if (registry_dirty && (now - last_registry_save >= REGISTRY_SAVE_INTERVAL_MS)) {
        save_registry();
        last_registry_save = now;
    }

    if (now - last_health_check >= HEALTH_CHECK_INTERVAL_MS) {
        check_node_health();
        last_health_check = now;
    }

    // Reset boot counter after stability period, same as peripheral template.
    if (fw_validated && !fw_stable && (millis() - boot_time_ms >= STABILITY_PERIOD_MS)) {
        boot_counter_reset();
        fw_stable = true;
    }
}
