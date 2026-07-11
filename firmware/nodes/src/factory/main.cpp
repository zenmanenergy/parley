// Parley peripheral node factory recovery firmware
//
// This firmware lives in a peripheral node's factory partition and is NEVER
// updated over the air. When both A and B slots are broken, the node falls back
// to this firmware. Its job is minimal: connect to WiFi and MQTT, announce
// itself in factory mode, and wait to receive new application firmware.
//
// Recovery channel: WiFi to gateway's AP, then to Pi's broker via MQTT.
// The gateway is the recovery anchor; factory firmware has no way to directly
// recover if the gateway is down, but nodes will reconnect when it comes back.
//
// What this firmware does:
//   1. Initialize Serial for diagnostics
//   2. Connect to gateway's WiFi AP (credentials from NVS or hardcoded)
//   3. Connect to MQTT broker
//   4. Announce on system/discovery with status="factory_mode"
//   5. Subscribe to nodes/<mac>/ota and listen for new firmware
//   6. Feed watchdog continuously
//   7. On OTA reception, write to app slot A and reboot
//
// That's all. No sensor reading, no actuator control, no complex logic.
// The simpler this firmware is, the less likely it fails.

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <Preferences.h>
#include <HTTPClient.h>

#ifndef FACTORY_FW_VERSION
#define FACTORY_FW_VERSION "factory-1.0.0"
#endif

// Watchdog timeout
#define WDT_TIMEOUT_MS 30000

// WiFi credentials (from docs: ParleyNet / parley-secret baked into factory firmware)
#define FACTORY_WIFI_SSID    "ParleyNet"
#define FACTORY_WIFI_PASS    "parley-secret"

// MQTT endpoint (gateway is at 192.168.4.1, AP IP)
#define FACTORY_MQTT_HOST    "192.168.4.1"
#define FACTORY_MQTT_PORT    1883

// Timeouts
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define MQTT_CONNECT_TIMEOUT_MS 10000

// NVS
#define NVS_NAMESPACE "parley"
#define NVS_NODE_ID   "node_id"

// Module state
static char s_node_id[32];
static char s_topic_ota[64];
static char s_topic_status[64];
static char s_topic_cmd_provision[80];

static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

static bool ota_in_progress = false;
static unsigned long last_announce = 0;

// Registration state
static bool node_id_assigned = false;

// ============================================================================
// Utilities
// ============================================================================

static void get_node_id() {
	Preferences prefs;
	prefs.begin(NVS_NAMESPACE, true);
	if (!prefs.getString(NVS_NODE_ID, s_node_id, sizeof(s_node_id))) {
		// No stored ID; generate one from MAC address
		uint8_t mac[6];
		WiFi.macAddress(mac);
		snprintf(s_node_id, sizeof(s_node_id), "node-%02x%02x%02x", mac[3], mac[4], mac[5]);
	}
	prefs.end();

	snprintf(s_topic_ota,    sizeof(s_topic_ota),    "nodes/%s/ota", s_node_id);
	snprintf(s_topic_status, sizeof(s_topic_status), "nodes/%s/status", s_node_id);
}

static void mqtt_callback(char* topic, uint8_t* payload, unsigned int len) {
	// Handle OTA updates
	if (strcmp(topic, s_topic_ota) == 0) {
		JsonDocument doc;
		if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
			Serial.println("[factory] OTA: invalid JSON");
			return;
		}

		const char* url = doc["url"];
		if (!url || strlen(url) == 0) {
			Serial.println("[factory] OTA: missing url");
			return;
		}

		if (ota_in_progress) {
			Serial.println("[factory] OTA: already in progress");
			return;
		}

		ota_in_progress = true;
		Serial.printf("[factory] OTA: starting from %s\n", url);

		// Perform OTA
		const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
		if (!target) {
			Serial.println("[factory] OTA: no update partition");
			ota_in_progress = false;
			return;
		}

		HTTPClient http;
		http.begin(url);
		http.setTimeout(60000);

		int code = http.GET();
		if (code != HTTP_CODE_OK) {
			Serial.printf("[factory] OTA: HTTP %d\n", code);
			http.end();
			ota_in_progress = false;
			return;
		}

		int total = http.getSize();
		esp_ota_handle_t handle;
		esp_err_t ret = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
		if (ret != ESP_OK) {
			Serial.printf("[factory] OTA: begin failed %s\n", esp_err_to_name(ret));
			http.end();
			ota_in_progress = false;
			return;
		}

		WiFiClient* stream = http.getStreamPtr();
		uint8_t buf[1024];
		int written = 0;

		while (http.connected() && (total < 0 || written < total)) {
			int available = stream->available();
			if (available > 0) {
				int n = stream->readBytes(buf, min(available, (int)sizeof(buf)));
				ret = esp_ota_write(handle, buf, n);
				if (ret != ESP_OK) {
					Serial.printf("[factory] OTA: write failed %s\n", esp_err_to_name(ret));
					break;
				}
				written += n;
			}
			esp_task_wdt_reset();
			delay(10);
		}

		http.end();

		if (ret != ESP_OK || esp_ota_end(handle) != ESP_OK) {
			Serial.println("[factory] OTA: end failed");
			ota_in_progress = false;
			return;
		}

		if (esp_ota_set_boot_partition(target) != ESP_OK) {
			Serial.println("[factory] OTA: set_boot failed");
			ota_in_progress = false;
			return;
		}

		Serial.printf("[factory] OTA: complete %d bytes, rebooting\n", written);
		delay(200);
		esp_restart();
		return;
	}

	// Handle node ID provisioning (from system/cmd/provision/<mac>)
	if (strstr(topic, "system/cmd/provision/") != NULL) {
		JsonDocument doc;
		if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
			Serial.println("[factory] provision: invalid JSON");
			return;
		}

		const char* assigned_id = doc["node_id"];
		if (!assigned_id || strlen(assigned_id) == 0) {
			Serial.println("[factory] provision: missing node_id");
			return;
		}

		Serial.printf("[factory] provision: assigning node_id=%s\n", assigned_id);

		// Store in NVS
		Preferences prefs;
		prefs.begin(NVS_NAMESPACE, false);
		prefs.putString(NVS_NODE_ID, assigned_id);
		prefs.end();

		node_id_assigned = true;
		Serial.println("[factory] provision: node_id stored, reboot to apply");

		// Announce registration complete
		JsonDocument announce;
		announce["node_id"] = assigned_id;
		announce["mac"] = WiFi.macAddress();
		announce["status"] = "provisioned";
		char buf[256];
		serializeJson(announce, buf, sizeof(buf));
		mqttClient.publish("system/registered", (const uint8_t*)buf, strlen(buf));

		// Give MQTT time to send, then reboot
		delay(500);
		esp_restart();
	}
}

// ============================================================================
// Arduino entry points
// ============================================================================

void setup() {
	Serial.begin(115200);
	delay(1000);

	Serial.printf("\n[factory] %s\n", FACTORY_FW_VERSION);

	// Watchdog
	const esp_task_wdt_config_t wdt_cfg = {
		.timeout_ms     = WDT_TIMEOUT_MS,
		.idle_core_mask = 0,
		.trigger_panic  = true,
	};
	esp_task_wdt_reconfigure(&wdt_cfg);
	esp_task_wdt_add(nullptr);

	// Get node ID
	get_node_id();
	Serial.printf("[factory] node_id=%s\n", s_node_id);

	// WiFi
	Serial.printf("[factory] connecting to %s\n", FACTORY_WIFI_SSID);
	WiFi.mode(WIFI_STA);
	WiFi.begin(FACTORY_WIFI_SSID, FACTORY_WIFI_PASS);

	unsigned long start = millis();
	while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
		delay(250);
		esp_task_wdt_reset();
	}

	if (WiFi.status() == WL_CONNECTED) {
		Serial.printf("[factory] WiFi connected IP=%s\n", WiFi.localIP().toString().c_str());
	} else {
		Serial.println("[factory] WiFi failed");
	}

	// MQTT
	mqttClient.setServer(FACTORY_MQTT_HOST, FACTORY_MQTT_PORT);
	mqttClient.setCallback(mqtt_callback);

	last_announce = millis();
}

void loop() {
	esp_task_wdt_reset();

	// Maintain MQTT connection
	if (!mqttClient.connected()) {
		unsigned long now = millis();
		if (now - last_announce >= 5000) {
			Serial.printf("[factory] MQTT connecting to %s:%d\n", FACTORY_MQTT_HOST, FACTORY_MQTT_PORT);

			char client_id[48];
			snprintf(client_id, sizeof(client_id), "node-%s-factory", s_node_id);

			if (mqttClient.connect(client_id)) {
				Serial.println("[factory] MQTT connected");
				mqttClient.subscribe(s_topic_ota);

				// Subscribe to provisioning command for this node's MAC
				char cmd_provision[96];
				snprintf(cmd_provision, sizeof(cmd_provision), "system/cmd/provision/%s", WiFi.macAddress().c_str());
				mqttClient.subscribe(cmd_provision);
				Serial.printf("[factory] subscribed to %s\n", cmd_provision);

				// Announce factory mode
				JsonDocument disc;
				disc["node_id"] = s_node_id;
				disc["status"] = "factory_mode";
				disc["fw"] = FACTORY_FW_VERSION;
				disc["mac"] = WiFi.macAddress();
				disc["ip"] = WiFi.localIP().toString();

				char buf[256];
				serializeJson(disc, buf, sizeof(buf));
				mqttClient.publish("system/discovery", buf, true);
				Serial.println("[factory] announced factory mode");

				last_announce = now;
			}
		}
	}

	mqttClient.loop();

	// Periodic status
	unsigned long now = millis();
	if (now - last_announce >= 30000) {
		JsonDocument status;
		status["node_id"] = s_node_id;
		status["status"] = "factory_mode";
		status["fw"] = FACTORY_FW_VERSION;
		status["uptime_s"] = now / 1000;
		status["rssi"] = WiFi.RSSI();
		status["free_heap"] = esp_get_free_heap_size();

		char buf[256];
		serializeJson(status, buf, sizeof(buf));
		mqttClient.publish(s_topic_status, buf, false);

		last_announce = now;
	}

	delay(100);
}


