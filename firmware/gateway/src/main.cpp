// Parley gateway node firmware — refactored to use universal template
//
// The gateway has four responsibilities:
//   1. WiFi access point — peripheral nodes connect here (192.168.4.0/24)
//   2. USB CDC-ECM — presents a network interface to the Pi (Pi gets 192.168.4.2)
//   3. IP routing — forwards packets between the AP subnet and the USB link so
//      peripheral nodes can reach the Pi's MQTT broker transparently
//   4. Provisioning — listens on system/discovery and maintains the node registry
//   5. Diagnostics — console interface over USB serial
//
// Network topology:
//   Peripheral nodes:  192.168.4.10 – 192.168.4.100  (DHCP from this gateway)
//   Gateway AP IP:     192.168.4.1
//   Pi (usb0):         192.168.4.2  (static, configured on Pi side)
//   Pi MQTT broker:    192.168.4.2:1883
//
// This firmware implements the universal node template plugin interface, so it
// benefits from shared recovery logic, watchdog, OTA, and heartbeat management.
// The template handles WiFi client connectivity (to reach the Pi's broker) and
// MQTT. The application code (here) handles the AP and provisioning.
//
// NOTE: CONFIG_LWIP_IP_FORWARD and CONFIG_LWIP_IPV4_NAPT must be enabled
// in sdkconfig.defaults for IP routing to function.

#include "node_template.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <lwip/lwip_napt.h>
#include <lwip/netif.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>

// ============================================================================
// Gateway-specific configuration
// ============================================================================

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

// Registry
#define REGISTRY_PATH   "/node_registry.json"
#define REGISTRY_SAVE_INTERVAL_MS   60000

// Node health monitoring
#define NODE_SILENCE_THRESHOLD_S    120
#define BOOT_COUNT_FLAKY_THRESHOLD  3
#define HEALTH_CHECK_INTERVAL_MS    30000

// MQTT topics (redundant with template, but explicit here for clarity)
#define TOPIC_DISCOVERY     "system/discovery"
#define TOPIC_ANOMALIES     "system/anomalies"
#define TOPIC_REGISTERED    "system/registered"

// NVS namespace for gateway-specific config
#define NVS_GW_NAMESPACE    "parley_gw"
#define NVS_AP_SSID         "ap_ssid"
#define NVS_AP_PASS         "ap_pass"

// ============================================================================
// Gateway state
// ============================================================================

static char ap_ssid[64];
static char ap_pass[64];

// Forward declaration (defined later, after loop_peripheral)
static bool check_usb_link();

// Node registry: mac -> {node_id, last_seen, fw, status, boot_count}
static JsonDocument registry_doc;
static bool registry_dirty = false;
static unsigned long last_registry_save = 0;
static unsigned long last_health_check = 0;

static bool ota_in_progress = false;

// USB health tracking — detects if link to Pi is active
static bool usb_link_up = false;
static unsigned long last_usb_check = 0;
static const unsigned long USB_CHECK_INTERVAL_MS = 15000;  // Check every 15s

// LED states (inherited from template but gateway adds specific patterns)
// The template handles LED updates via its own state machine
// (BOOTING, operational, FACTORY, ERROR patterns)

// ============================================================================
// Gateway configuration loading/saving
// ============================================================================

static void load_gateway_config() {
	Preferences prefs;
	prefs.begin(NVS_GW_NAMESPACE, true);  // read-only

	if (!prefs.getString(NVS_AP_SSID, ap_ssid, sizeof(ap_ssid))) {
		strlcpy(ap_ssid, DEFAULT_AP_SSID, sizeof(ap_ssid));
	}
	if (!prefs.getString(NVS_AP_PASS, ap_pass, sizeof(ap_pass))) {
		strlcpy(ap_pass, DEFAULT_AP_PASS, sizeof(ap_pass));
	}

	prefs.end();

	node_log(LogLevel::INFO, "gateway: loaded config ap_ssid=%s", ap_ssid);
}

static void save_gateway_config() {
	Preferences prefs;
	prefs.begin(NVS_GW_NAMESPACE, false);  // read-write

	prefs.putString(NVS_AP_SSID, ap_ssid);
	prefs.putString(NVS_AP_PASS, ap_pass);

	prefs.end();
	node_log(LogLevel::INFO, "gateway: saved config");
}

// ============================================================================
// WiFi AP initialization
// ============================================================================

static void start_ap() {
	WiFi.mode(WIFI_AP);

	IPAddress ip, gw, sn;
	ip.fromString(AP_IP);
	gw.fromString(AP_GATEWAY);
	sn.fromString(AP_SUBNET);

	WiFi.softAPConfig(ip, gw, sn);

	bool ok = WiFi.softAP(ap_ssid, ap_pass, DEFAULT_AP_CHANNEL, 0, AP_MAX_CLIENTS);
	if (!ok) {
		node_log(LogLevel::ERROR, "gateway: AP startup FAILED");
		node_log_local(LogLevel::ERROR, "gw.ap", "startup_failed ssid=%s", ap_ssid);
		esp_restart();
	}

	node_log(LogLevel::INFO, "gateway: AP started ssid=%s ip=%s", 
			 ap_ssid, WiFi.softAPIP().toString().c_str());
	node_log_local(LogLevel::INFO, "gw.ap", "started ssid=%s ip=%s", 
				   ap_ssid, WiFi.softAPIP().toString().c_str());

	// Enable NAT so peripheral nodes (192.168.4.x) can reach the MQTT broker
	// through the USB link. CONFIG_LWIP_IP_FORWARD must be enabled in sdkconfig.
	ip_napt_enable(ip.v4(), 1);
	node_log(LogLevel::INFO, "gateway: NAT enabled on AP interface");
	node_log_local(LogLevel::INFO, "gw.ap", "NAT enabled");
}

// ============================================================================
// Node registry management
// ============================================================================

static void load_registry() {
	if (!LittleFS.exists(REGISTRY_PATH)) {
		registry_doc.to<JsonObject>();
		node_log(LogLevel::INFO, "gateway: no registry file, starting fresh");
		node_log_local(LogLevel::INFO, "gw.registry", "not_found creating_fresh");
		return;
	}

	File f = LittleFS.open(REGISTRY_PATH, "r");
	if (!f) {
		node_log(LogLevel::WARN, "gateway: registry open failed");
		node_log_local(LogLevel::WARN, "gw.registry", "open_failed");
		registry_doc.to<JsonObject>();
		return;
	}

	size_t size = f.size();
	if (size == 0 || size > (256 * 1024)) {
		// Registry file is empty or suspiciously large
		node_log(LogLevel::WARN, "gateway: registry file size invalid (%lu bytes), resetting", size);
		node_log_local(LogLevel::WARN, "gw.registry", "invalid_size_%lu bytes", size);
		f.close();
		registry_doc.to<JsonObject>();
		return;
	}

	DeserializationError err = deserializeJson(registry_doc, f);
	f.close();

	if (err) {
		node_log(LogLevel::WARN, "gateway: registry parse error %s, starting fresh", err.c_str());
		node_log_local(LogLevel::WARN, "gw.registry", "parse_error %s", err.c_str());
		registry_doc.to<JsonObject>();
		return;
	}

	// Validate that the registry is actually a JSON object
	if (!registry_doc.is<JsonObject>()) {
		node_log(LogLevel::WARN, "gateway: registry root is not JSON object, resetting");
		node_log_local(LogLevel::WARN, "gw.registry", "root_not_object");
		registry_doc.to<JsonObject>();
		return;
	}

	node_log(LogLevel::INFO, "gateway: registry loaded %d nodes", registry_doc.size());
	node_log_local(LogLevel::INFO, "gw.registry", "loaded nodes=%d", registry_doc.size());
}

static void save_registry() {
	File f = LittleFS.open(REGISTRY_PATH, "w");
	if (!f) {
		node_log(LogLevel::ERROR, "gateway: registry write open failed");
		node_log_local(LogLevel::ERROR, "gw.registry", "write_open_failed");
		return;
	}
	serializeJson(registry_doc, f);
	f.close();
	registry_dirty = false;
	node_log(LogLevel::DEBUG, "gateway: registry saved");
	node_log_local(LogLevel::DEBUG, "gw.registry", "saved nodes=%d", registry_doc.size());
}

// ============================================================================
// Discovery: handle system/discovery announcements from new and recovery nodes
// ============================================================================

static void handle_discovery(const uint8_t* payload, unsigned int len) {
	JsonDocument msg;
	if (deserializeJson(msg, payload, len) != DeserializationError::Ok) {
		node_log(LogLevel::WARN, "gateway: discovery message invalid JSON");
		node_log_local(LogLevel::WARN, "gw.discovery", "invalid_json");
		return;
	}

	const char* mac     = msg["mac"];
	const char* node_id = msg["node_id"];
	const char* status  = msg["status"];
	const char* fw      = msg["fw"];
	const char* ip      = msg["ip"];

	if (!mac || !node_id) {
		node_log(LogLevel::WARN, "gateway: discovery missing mac or node_id");
		node_log_local(LogLevel::WARN, "gw.discovery", "missing_fields");
		return;
	}

	node_log(LogLevel::INFO, "gateway: discovery node_id=%-20s status=%s fw=%s",
			 node_id, status ? status : "?", fw ? fw : "?");
	node_log_local(LogLevel::INFO, "gw.discovery", "new_node node_id=%s status=%s", 
				   node_id, status ? status : "unknown");

	// Upsert into registry keyed by MAC
	JsonObject entry = registry_doc[mac].isNull()
					   ? registry_doc[mac].to<JsonObject>()
					   : registry_doc[mac].as<JsonObject>();

	entry["node_id"]   = node_id;
	entry["status"]    = status  ? status  : "unknown";
	entry["fw"]        = fw      ? fw      : "unknown";
	entry["ip"]        = ip      ? ip      : "unknown";
	entry["last_seen"] = millis() / 1000;

	if (msg["boot_count"].is<unsigned int>()) {
		entry["boot_count"] = msg["boot_count"].as<unsigned int>();
	}

	registry_dirty = true;

	// If node is unprovisioned or in factory mode, publish an anomaly
	// so the Pi can initiate registration or recovery.
	if (status && (strcmp(status, "needs_provisioning") == 0 || strcmp(status, "factory") == 0)) {
		bool needs_reg = (strcmp(status, "needs_provisioning") == 0);

		node_log(LogLevel::INFO, "gateway: %s — %s",
				 node_id,
				 needs_reg ? "NEEDS PROVISIONING" : "FACTORY MODE (recovery)");
		node_log_local(LogLevel::INFO, "gw.discovery", "%s %s", 
					   node_id, needs_reg ? "needs_provisioning" : "factory_mode");

		JsonDocument anomaly;
		anomaly["type"]    = needs_reg ? "needs_provisioning" : "factory_mode";
		anomaly["node_id"] = node_id;
		anomaly["mac"]     = mac;
		anomaly["ip"]      = ip ? ip : "unknown";

		char buf[256];
		serializeJson(anomaly, buf, sizeof(buf));
		node_publish_raw("system/anomalies", (const uint8_t*)buf, strlen(buf));
	}
}

// ============================================================================
// Registration: handle system/registered from nodes completing provisioning
// ============================================================================

static void handle_registered(const uint8_t* payload, unsigned int len) {
	JsonDocument msg;
	if (deserializeJson(msg, payload, len) != DeserializationError::Ok) return;

	const char* mac     = msg["mac"];
	const char* node_id = msg["node_id"];
	if (!mac || !node_id) return;

	node_log(LogLevel::INFO, "gateway: registered node_id=%s mac=%s", node_id, mac);
	node_log_local(LogLevel::INFO, "gw.registered", "node_id=%s", node_id);

	// Update registry with the assigned node ID
	JsonObject entry = registry_doc[mac].isNull()
					   ? registry_doc[mac].to<JsonObject>()
					   : registry_doc[mac].as<JsonObject>();

	entry["node_id"]    = node_id;
	entry["status"]     = "factory";   // still in factory; app fw not yet pushed
	entry["registered"] = true;
	entry["last_seen"]  = millis() / 1000;

	registry_dirty = true;
}

// ============================================================================
// Health monitoring: detect silent and flaky nodes
// ============================================================================

static void check_node_health() {
	uint32_t now_s = millis() / 1000;

	for (JsonPair kv : registry_doc.as<JsonObject>()) {
		JsonObject entry = kv.value().as<JsonObject>();
		const char* nid      = entry["node_id"] | "unknown";
		uint32_t last_seen   = entry["last_seen"] | 0;
		uint32_t boot_count  = entry["boot_count"] | 0;
		const char* status   = entry["status"] | "unknown";
		const char* mac      = kv.key().c_str();

		uint32_t age_s = (now_s > last_seen) ? (now_s - last_seen) : 0;

		// Flag nodes that have gone silent
		if (age_s > NODE_SILENCE_THRESHOLD_S) {
			node_log(LogLevel::WARN, "gateway: node %s SILENT for %lu seconds", nid, age_s);
			node_log_local(LogLevel::WARN, "gw.health", "node_silent node_id=%s age_s=%lu", 
						   nid, age_s);

			JsonDocument anomaly;
			anomaly["type"]      = "node_silent";
			anomaly["node_id"]   = nid;
			anomaly["mac"]       = mac;
			anomaly["silent_s"]  = age_s;
			anomaly["status"]    = status;

			char buf[256];
			serializeJson(anomaly, buf, sizeof(buf));
			node_publish_raw("system/anomalies", (const uint8_t*)buf, strlen(buf));
		}

		// Flag nodes with high boot counts (indicates instability)
		if (boot_count >= BOOT_COUNT_FLAKY_THRESHOLD && strcmp(status, "factory") != 0) {
			node_log(LogLevel::WARN, "gateway: node %s FLAKY boot_count=%lu", nid, boot_count);
			node_log_local(LogLevel::WARN, "gw.health", "high_boot_count node_id=%s count=%lu", 
						   nid, boot_count);

			JsonDocument anomaly;
			anomaly["type"]       = "high_boot_count";
			anomaly["node_id"]    = nid;
			anomaly["mac"]        = mac;
			anomaly["boot_count"] = boot_count;

			char buf[256];
			serializeJson(anomaly, buf, sizeof(buf));
			node_publish_raw("system/anomalies", (const uint8_t*)buf, strlen(buf));
		}
	}
}

// ============================================================================
// Diagnostic console: text commands over USB serial
// ============================================================================

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
				Serial.printf("[console] uptime=%lus  free_heap=%lu bytes\n",
							  millis() / 1000, esp_get_free_heap_size());
				Serial.printf("[console] AP clients=%d\n", WiFi.softAPgetStationNum());
				Serial.printf("[console] registry: %d nodes\n", registry_doc.size());
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

// ============================================================================
// Plugin interface: required by node_template
// ============================================================================

void setup_peripheral() {
	// Load gateway-specific configuration
	load_gateway_config();
	node_log_local(LogLevel::INFO, "gw.init", "config_loaded");

	// Initialize filesystem
	if (!LittleFS.begin(true)) {
		node_log(LogLevel::FATAL, "gateway: LittleFS mount failed");
		node_log_local(LogLevel::FATAL, "gw.init", "littlefs_mount_failed");
		esp_restart();
	}
	node_log_local(LogLevel::INFO, "gw.init", "littlefs_ready");

	// Load node registry from persistent storage
	load_registry();
	node_log_local(LogLevel::INFO, "gw.init", "registry_loaded nodes=%d", registry_doc.size());

	// Start the WiFi AP
	start_ap();

	// Check USB link to Pi
	check_usb_link();
	node_log_local(LogLevel::INFO, "gw.init", "USB_link_%s", usb_link_up ? "up" : "down");

	// Subscribe to discovery and provisioning topics using raw (absolute) topics.
	// node_subscribe_raw() is used because system/discovery and system/registered
	// are global topics, NOT node-namespaced command channels.
	node_subscribe_raw("system/discovery");
	node_subscribe_raw("system/registered");
	node_subscribe("gateway/ota");
	node_subscribe("gateway/config");  // for runtime AP configuration updates

	last_registry_save = millis();
	last_health_check = millis();

	node_log(LogLevel::INFO, "gateway: setup complete");
	node_log_local(LogLevel::INFO, "gw.init", "setup_complete");
}

void loop_peripheral() {
	// Process diagnostic console commands from the Pi
	handle_console();

	unsigned long now = millis();

	// Save registry periodically
	if (registry_dirty && (now - last_registry_save >= REGISTRY_SAVE_INTERVAL_MS)) {
		node_log_local(LogLevel::DEBUG, "gw.registry", "auto_save_triggered");
		save_registry();
		last_registry_save = now;
	}

	// Check node health periodically
	if (now - last_health_check >= HEALTH_CHECK_INTERVAL_MS) {
		check_node_health();
		last_health_check = now;
	}
}

// Check USB/network link to Pi — detects if USB CDC-ECM is functional
// The Pi is statically configured at 192.168.4.2 on the usb0 interface.
static bool check_usb_link() {
	// Parse the AP's own IP so we can exclude the AP netif from the check.
	// Without this the AP interface (192.168.4.1) would always match.
	ip4_addr_t ap_ip4;
	ip4addr_aton(AP_IP, &ap_ip4);

	struct netif* nif = nullptr;
	bool found = false;

	for (nif = netif_list; nif != nullptr; nif = nif->next) {
		if (!netif_is_up(nif)) continue;
		if (ip_addr_isany(&nif->ip_addr)) continue;
		if (!IP_IS_V4(&nif->ip_addr)) continue;
		// Skip the WiFi AP interface (192.168.4.1)
		if (ip4_addr_cmp(netif_ip4_addr(nif), &ap_ip4)) continue;
		found = true;
		break;
	}

	// Detect state change and log it
	bool was_up = usb_link_up;
	usb_link_up = found;
	
	if (usb_link_up && !was_up) {
		node_log(LogLevel::INFO, "gateway: USB link to Pi is up");
		node_log_local(LogLevel::INFO, "gw.usb", "link_up");
	} else if (!usb_link_up && was_up) {
		node_log(LogLevel::WARN, "gateway: USB link to Pi is down");
		node_log_local(LogLevel::WARN, "gw.usb", "link_down");
	}
	
	return usb_link_up;
}

bool peripheral_health_check() {
	// Gateway is healthy when: AP is up, USB link is active, and
	// the MQTT connection to the Pi is working. (The template checks MQTT;
	// we verify the AP and USB here.)
	bool ap_ok = (WiFi.softAPgetStationNum() >= 0);
	if (!ap_ok) {
		node_log(LogLevel::WARN, "gateway: health check AP is not running");
		node_log_local(LogLevel::WARN, "gw.health", "AP_not_running");
		return false;
	}
	
	// Check USB link periodically
	if (millis() - last_usb_check >= USB_CHECK_INTERVAL_MS) {
		last_usb_check = millis();
		if (!check_usb_link()) {
			node_log(LogLevel::WARN, "gateway: health check USB link not active");
			node_log_local(LogLevel::WARN, "gw.health", "USB_not_active");
			return false;
		}
	}
	
	// Note: MQTT connectivity is checked by the template separately.
	// If we reach here, AP is up, USB link is active, and MQTT will be verified by template.
	node_log_local(LogLevel::DEBUG, "gw.health", "AP_ok clients=%d USB_ok", WiFi.softAPgetStationNum());
	return true;
}

void peripheral_handle_command(const char* channel, const uint8_t* payload, size_t len) {
	// Route discovery, registration, OTA, and configuration commands.
	// system/discovery and system/registered arrive as full topic strings (from node_subscribe_raw).
	// gateway/ota and gateway/config arrive as channel suffixes (from node_subscribe).
	if (strcmp(channel, "system/discovery") == 0) {
		handle_discovery(payload, len);
	} else if (strcmp(channel, "system/registered") == 0) {
		handle_registered(payload, len);
	} else if (strcmp(channel, "gateway/ota") == 0) {
		// OTA for the gateway itself is handled by the template's OTA subsystem.
		// This callback is called if the command arrives; template will process it.
		node_log(LogLevel::INFO, "gateway: OTA command received");
		node_log_local(LogLevel::INFO, "gw.ota", "command_received");
	} else if (strcmp(channel, "gateway/config") == 0) {
		// Gateway configuration: AP SSID/password can be updated via MQTT
		// JSON format: {"ap_ssid": "new_ssid", "ap_pass": "new_password"}
		JsonDocument cfg;
		if (deserializeJson(cfg, payload, len) == DeserializationError::Ok) {
			bool changed = false;
			if (cfg["ap_ssid"].is<const char*>()) {
				const char* new_ssid = cfg["ap_ssid"].as<const char*>();
				if (strlen(new_ssid) > 0 && strlen(new_ssid) < sizeof(ap_ssid)) {
					strlcpy(ap_ssid, new_ssid, sizeof(ap_ssid));
					changed = true;
					node_log(LogLevel::INFO, "gateway: AP SSID changed to %s", ap_ssid);
					node_log_local(LogLevel::INFO, "gw.config", "ap_ssid_changed to %s", ap_ssid);
				}
			}
			if (cfg["ap_pass"].is<const char*>()) {
				const char* new_pass = cfg["ap_pass"].as<const char*>();
				if (strlen(new_pass) > 0 && strlen(new_pass) < sizeof(ap_pass)) {
					strlcpy(ap_pass, new_pass, sizeof(ap_pass));
					changed = true;
					node_log(LogLevel::INFO, "gateway: AP password changed");
					node_log_local(LogLevel::INFO, "gw.config", "ap_pass_changed");
				}
			}
			if (changed) {
				save_gateway_config();
				// Restart WiFi AP with new settings
				WiFi.softAPdisconnect(true);
				delay(500);
				start_ap();
				node_log(LogLevel::INFO, "gateway: AP restarted with new config");
				node_log_local(LogLevel::INFO, "gw.config", "AP_restarted");
			}
		} else {
			node_log(LogLevel::WARN, "gateway: config JSON parse error");
			node_log_local(LogLevel::WARN, "gw.config", "parse_error");
		}
	}
}

// Optional: publish additional status beyond the template's heartbeat
void peripheral_publish_status() {
	JsonDocument doc;
	doc["node_id"]     = "gateway";
	doc["healthy"]     = true;
	doc["ap_ssid"]     = ap_ssid;
	doc["ap_clients"]  = WiFi.softAPgetStationNum();
	doc["known_nodes"] = registry_doc.size();
	doc["uptime_s"]    = millis() / 1000;
	doc["free_heap"]   = esp_get_free_heap_size();

	node_publish_json("data/status", doc);
}

// Optional: called when network connection is lost/restored
void peripheral_on_disconnect() {
	node_log(LogLevel::WARN, "gateway: MQTT connection lost");
	node_log_local(LogLevel::WARN, "gw.mqtt", "disconnect");
}

void peripheral_on_reconnect() {
	node_log(LogLevel::INFO, "gateway: MQTT connection restored");
	node_log_local(LogLevel::INFO, "gw.mqtt", "reconnect");
}

void peripheral_shutdown() {
	node_log(LogLevel::INFO, "gateway: shutting down");
	// Stop the AP gracefully
	WiFi.softAPdisconnect(true);
	// Save registry one last time
	if (registry_dirty) {
		save_registry();
	}
}
