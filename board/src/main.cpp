#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef DEVICE_KEY
#define DEVICE_KEY ""
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""
#define WIFI_CHANNEL 6
#endif

// ---------- CHANGE THESE ----------
const char* API_URL = "http://iot-toni.northcentralus.cloudapp.azure.com";   // no trailing slash, e.g. http://iot-jbrown.northcentralus.cloudapp.azure.com
const char* MQTT_NAME = "<name>";           // your Tutorial 9 name, lowercase letters and digits
const int DHT_PIN = 15;                     // check diagram.json: the pin the DHT22's SDA goes to
const int FAN_LED_PIN = 2;                  // check diagram.json: the pin the fan LED goes to
// ----------------------------------

const char* MQTT_HOST = "135.232.203.144";
const int MQTT_PORT = 1883;

const unsigned long READ_INTERVAL_MS = 10000;   // B1: every 10 s
const unsigned long WIFI_TIMEOUT_MS = 15000;
const unsigned long RETRY_INTERVAL_MS = 5000;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
DHT dht(DHT_PIN, DHT22);

String mac;
String statusTopic;
String settingsTopic;
String stateTopic;

// Settings from fan/settings. Until the first valid message: fan off (B4).
bool haveSettings = false;
String fanMode = "off";
float maxHumidity = 100;

float lastHumidity = NAN;
bool fanOn = false;
String publishedState = "";      // what we last put on fan/state

unsigned long lastRead = 0;
unsigned long lastAttempt = 0;
bool stopPosting = false;        // set on 401/403: a wrong key stays wrong

// ---------- Wi-Fi (as in Tutorial 6) ----------
bool connectWiFi() {
  Serial.print("Connecting to ");
  Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println(" no Wi-Fi after 15 s.");
      return false;
    }
    delay(250);
    Serial.print(".");
  }
  Serial.println(" connected");
  mac = WiFi.macAddress();
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("MAC address: ");
  Serial.println(mac);
  return true;
}

// ---------- B5: publish fan/state only when it changes ----------
void publishState() {
  String state = fanOn ? "on" : "off";
  if (state == publishedState) return;     // nothing changed
  if (!mqtt.connected()) return;           // try again after reconnect
  if (mqtt.publish(stateTopic.c_str(), state.c_str(), true)) {
    publishedState = state;
    Serial.println(String("fan/state -> ") + state);
  }
}

// ---------- B4: decide the fan LED ----------
void updateFan() {
  bool want = false;
  if (haveSettings) {
    if (fanMode == "on") {
      want = true;
    } else if (fanMode == "auto") {
      want = !isnan(lastHumidity) && lastHumidity > maxHumidity;
    }
  }
  if (want != fanOn) {
    fanOn = want;
    digitalWrite(FAN_LED_PIN, fanOn ? HIGH : LOW);
    Serial.println(fanOn ? "Fan ON" : "Fan OFF");
  }
  publishState();
}

// ---------- B3: read fan/settings ----------
void onMessage(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != settingsTopic) return;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.println("Refused settings: not JSON");
    return;
  }

  JsonVariant modeVar = doc["mode"];
  JsonVariant maxVar = doc["max_humidity"];
  if (!modeVar.is<const char*>() || !(maxVar.is<float>() || maxVar.is<long>())) {
    Serial.println("Refused settings: a field is missing");
    return;
  }

  String newMode = modeVar.as<const char*>();
  if (newMode != "auto" && newMode != "on" && newMode != "off") {
    Serial.println(String("Refused settings: bad mode ") + newMode);
    return;
  }

  fanMode = newMode;
  maxHumidity = maxVar.as<float>();
  haveSettings = true;
  Serial.printf("Settings: mode=%s max_humidity=%.1f\n", fanMode.c_str(), maxHumidity);
  updateFan();   // react now, not at the next reading
}

// ---------- B2: connect with a last will ----------
bool connectMqtt() {
  String clientId = String("ecse3038-") + MQTT_NAME + "-" + mac;
  clientId.replace(":", "");
  bool ok = mqtt.connect(clientId.c_str(), statusTopic.c_str(), 1, true, "offline");
  if (!ok) {
    Serial.print("Broker said no, state ");
    Serial.println(mqtt.state());
    return false;
  }
  Serial.println("Connected to broker");
  mqtt.publish(statusTopic.c_str(), "online", true);
  mqtt.subscribe(settingsTopic.c_str(), 1);   // subscribe again on every connect
  publishState();                             // catch up if the fan changed while offline
  return true;
}

// ---------- B1: POST a reading ----------
void postReading(float humidity) {
  JsonDocument body;
  body["mac"] = mac;
  body["humidity"] = humidity;
  String json;
  serializeJson(body, json);

  HTTPClient http;
  http.begin(String(API_URL) + "/readings");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", DEVICE_KEY);
  int status = http.POST(json);
  String response = "";
  if (status > 0) {
    response = http.getString();
  }
  http.end();

  Serial.print("POST /readings ");
  Serial.print(json);
  Serial.print(" -> ");
  Serial.println(status);

  if (status == 401 || status == 403) {
    Serial.println(response);
    Serial.println("Stopped posting. Register this MAC with POST /fans and put the key in include/secrets.h:");
    Serial.println(mac);
    stopPosting = true;
  } else if (status < 0) {
    Serial.println(HTTPClient::errorToString(status));
  }
}

void readAndSend() {
  float humidity = dht.readHumidity();
  if (isnan(humidity)) {
    Serial.println("DHT22 read failed");
    return;
  }
  lastHumidity = humidity;
  Serial.printf("Humidity: %.1f %%\n", humidity);
  updateFan();

  if (!stopPosting && WiFi.status() == WL_CONNECTED) {
    postReading(humidity);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(FAN_LED_PIN, OUTPUT);
  digitalWrite(FAN_LED_PIN, LOW);           // fan off until settings arrive
  dht.begin();

  String base = String("ecse3038/") + MQTT_NAME;
  statusTopic = base + "/status";
  settingsTopic = base + "/fan/settings";
  stateTopic = base + "/fan/state";

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(15);

  if (connectWiFi()) {
    connectMqtt();
  }
  lastAttempt = millis();
}

void loop() {
  // B2: reconnect, at most one try every 5 s (as in the MQTT lecture)
  if (WiFi.status() != WL_CONNECTED || !mqtt.connected()) {
    if (millis() - lastAttempt >= RETRY_INTERVAL_MS) {
      lastAttempt = millis();
      if (WiFi.status() == WL_CONNECTED || connectWiFi()) {
        connectMqtt();
      }
    }
  } else {
    mqtt.loop();   // delivers fan/settings to onMessage()
  }

  // B1: every 10 s, read and POST
  if (lastRead == 0 || millis() - lastRead >= READ_INTERVAL_MS) {
    lastRead = millis();
    readAndSend();
  }
}
