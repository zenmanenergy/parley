// Parley Gateway Firmware
//
// This ESP32-S3 bridges the Raspberry Pi and peripheral nodes.
//
// Responsibilities:
//   1. WiFi Access Point (SSID: ParleyNet) for peripheral nodes
//   2. MQTT broker for nodes connecting over WiFi
//   3. USB serial tunnel to Pi (forward MQTT messages over serial)
//   4. Serial diagnostics console

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

#ifndef GATEWAY_FW_VERSION
#define GATEWAY_FW_VERSION "gateway-1.0.0"
#endif

// ESP32-S3 built-in RGB LED (WS2812/NeoPixel on GPIO 48)
#define RGB_LED_PIN 48
#define NUM_LEDS 1
Adafruit_NeoPixel strip(NUM_LEDS, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

#define AP_SSID            "ParleyNet"
#define AP_PASS            "parley-secret"
#define AP_IP              IPAddress(192, 168, 4, 1)
#define AP_GATEWAY         IPAddress(192, 168, 4, 1)
#define AP_SUBNET          IPAddress(255, 255, 255, 0)

#define MQTT_LISTEN_PORT   1883
#define WDT_TIMEOUT_MS     60000

// Global state
static WiFiServer mqtt_server(MQTT_LISTEN_PORT);
static WiFiClient mqtt_clients[8];
static unsigned long last_activity = 0;
static unsigned long last_status_print = 0;
static unsigned long last_heartbeat = 0;

// Forward declarations
static void setup_ap();
static void handle_mqtt_connections();
static void tunnel_to_pi(const char* op, const char* topic, const char* payload);
static void print_status();

// Utility
static const char* ip_to_string(IPAddress ip) {
    static char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d.%d.%d.%d",
             ip[0], ip[1], ip[2], ip[3]);
    return buffer;
}

// WiFi AP Setup
static void setup_ap() {
    Serial.println("[gateway] setting up WiFi AP...");

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);

    if (!WiFi.softAP(AP_SSID, AP_PASS)) {
        Serial.println("[gateway] ERROR: failed to start AP");
        esp_restart();
    }

    Serial.printf("[gateway] AP started: %s\n", AP_SSID);
    Serial.printf("[gateway] AP IP: %s\n", ip_to_string(AP_IP));
    Serial.printf("[gateway] max clients: %d\n", 8);

    mqtt_server.begin();
    Serial.printf("[gateway] MQTT broker listening on port %d\n", MQTT_LISTEN_PORT);
}

// Tunnel to Pi over USB serial
// Format: {"op":"...","topic":"...","payload":"..."}
static void tunnel_to_pi(const char* op, const char* topic, const char* payload) {
    DynamicJsonDocument doc(512);
    doc["op"] = op;
    if (topic) doc["topic"] = topic;
    if (payload) doc["payload"] = payload;

    serializeJson(doc, Serial);
    Serial.println();
}

// MQTT Connection Handling
static void handle_mqtt_connections() {
    // Check for new incoming connections
    if (mqtt_server.hasClient()) {
        Serial.println("[mqtt] incoming connection");

        // Find an empty slot
        for (int i = 0; i < 8; i++) {
            if (!mqtt_clients[i]) {
                mqtt_clients[i] = mqtt_server.accept();
                Serial.printf("[mqtt] client %d connected from %s\n",
                              i, mqtt_clients[i].remoteIP().toString().c_str());
                last_activity = millis();
                
                // Announce to Pi
                tunnel_to_pi("client_connect", nullptr, mqtt_clients[i].remoteIP().toString().c_str());
                break;
            }
        }
    }

    // Check each client for incoming data
    for (int i = 0; i < 8; i++) {
        if (mqtt_clients[i] && mqtt_clients[i].connected()) {
            if (mqtt_clients[i].available()) {
                uint8_t byte = mqtt_clients[i].read();
                Serial.printf("[mqtt] client %d -> 0x%02x\n", i, byte);

                // MQTT CONNECT packet (0x10)
                if (byte == 0x10) {
                    uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
                    mqtt_clients[i].write(connack, sizeof(connack));
                    Serial.printf("[mqtt] sent CONNACK to client %d\n", i);
                    tunnel_to_pi("mqtt_connect", nullptr, nullptr);
                }

                last_activity = millis();
            }
        } else if (mqtt_clients[i]) {
            // Client disconnected
            Serial.printf("[mqtt] client %d disconnected\n", i);
            tunnel_to_pi("client_disconnect", nullptr, nullptr);
            mqtt_clients[i].stop();
        }
    }
}

// Status Reporting
static void print_status() {
    uint8_t client_count = WiFi.softAPgetStationNum();
    Serial.printf("[gateway] status: %u WiFi clients, uptime: %lus\n",
                  client_count, millis() / 1000);
}

// LED Control
static void set_led_color(uint8_t r, uint8_t g, uint8_t b) {
    strip.setPixelColor(0, strip.Color(r, g, b));
    strip.show();
}

static void update_led() {
    uint8_t client_count = WiFi.softAPgetStationNum();
    
    if (client_count > 0) {
        // Connected: solid green
        set_led_color(0, 255, 0);
    } else {
        // Disconnected: solid red
        set_led_color(255, 0, 0);
    }
}

// Setup
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("\n[parley] gateway firmware %s\n", GATEWAY_FW_VERSION);

    // Initialize RGB LED
    strip.begin();
    strip.show();
    set_led_color(255, 0, 0);  // Start red (disconnected)
    Serial.println("[gateway] RGB LED initialized");

    // Watchdog
    esp_task_wdt_init(WDT_TIMEOUT_MS / 1000, true);
    esp_task_wdt_add(nullptr);

    // Initialize WiFi AP and MQTT server
    setup_ap();

    last_activity = millis();
    last_status_print = millis();
    last_heartbeat = millis();

    // Announce gateway boot
    tunnel_to_pi("gateway_boot", "system/gateway", GATEWAY_FW_VERSION);

    // Announce gateway as a discoverable node
    char mac_str[18];
    uint8_t mac[6];
    WiFi.softAPmacAddress(mac);
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    StaticJsonDocument<256> discovery;
    discovery["mac"] = mac_str;
    discovery["ip"] = ip_to_string(AP_IP);
    discovery["node_id"] = "gateway";
    discovery["status"] = "active";
    discovery["fw"] = GATEWAY_FW_VERSION;
    discovery["type"] = "gateway";
    
    char discovery_payload[256];
    serializeJson(discovery, discovery_payload, sizeof(discovery_payload));
    tunnel_to_pi("publish", "system/discovery", discovery_payload);
}

// Loop
void loop() {
    esp_task_wdt_reset();

    // Handle MQTT connections
    handle_mqtt_connections();

    // Update LED based on connection status
    update_led();

    // Print status every 30 seconds
    unsigned long now = millis();
    if (now - last_status_print >= 30000) {
        print_status();
        last_status_print = now;
    }

    // Send heartbeat every 30 seconds to keep node visible in dashboard
    if (now - last_heartbeat >= 30000) {
        uint8_t mac[6];
        WiFi.softAPmacAddress(mac);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        
        StaticJsonDocument<256> heartbeat;
        heartbeat["mac"] = mac_str;
        heartbeat["ip"] = ip_to_string(AP_IP);
        heartbeat["node_id"] = "gateway";
        heartbeat["status"] = "active";
        heartbeat["fw"] = GATEWAY_FW_VERSION;
        heartbeat["type"] = "gateway";
        heartbeat["clients"] = WiFi.softAPgetStationNum();
        heartbeat["uptime"] = millis() / 1000;
        
        char heartbeat_payload[256];
        serializeJson(heartbeat, heartbeat_payload, sizeof(heartbeat_payload));
        tunnel_to_pi("publish", "system/discovery", heartbeat_payload);
        
        last_heartbeat = now;
    }

    delay(100);
}

