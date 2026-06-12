#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define WIFI_SSID    "ParleyNet"
#define WIFI_PASS    "parley-secret"
#define MQTT_HOST    "192.168.4.1"
#define MQTT_PORT    1883

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

void setup() {
	Serial.begin(115200);
	delay(1000);
	Serial.println("\n\n=== Parley Factory Firmware ===");
	
	// Connect to WiFi
	Serial.print("Connecting to WiFi: ");
	Serial.println(WIFI_SSID);
	WiFi.mode(WIFI_STA);
	WiFi.begin(WIFI_SSID, WIFI_PASS);
	
	int attempts = 0;
	while (WiFi.status() != WL_CONNECTED && attempts < 40) {
		delay(500);
		Serial.print(".");
		attempts++;
	}
	
	if (WiFi.status() == WL_CONNECTED) {
		Serial.println("\nWiFi connected!");
		Serial.print("IP: ");
		Serial.println(WiFi.localIP());
	} else {
		Serial.println("\nWiFi connection failed - will retry...");
	}
}

void loop() {
	Serial.println("Factory firmware running...");
	delay(5000);
}


