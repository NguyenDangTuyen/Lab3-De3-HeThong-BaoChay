// Lab 3 - De 3: Mach bao chay gia dinh (MQ2 + DHT22) - ESP32 + FreeRTOS + MQTT
#include <WiFi.h>
#include <PubSubClient.h>
#include "DHT.h"

// ---------- Chan ket noi ----------
#define DHT_PIN    15
#define GAS_PIN    34   // AOUT cua MQ2 (ADC1)
#define FAN_PIN    26   // Quat
#define PUMP_PIN   27   // May bom
#define BUZZER_PIN 25   // Coi

// ---------- Cau hinh ----------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;

const char* TOPIC_DATA = "Nguyen Dang Tuyen-231432390/firealarm/data";
const char* TOPIC_CMD  = "Nguyen Dang Tuyen-231432390/firealarm/cmd";

const int   GAS_THRESHOLD  = 3000;  // ppm: vuot nguong -> bat quat
const float TEMP_THRESHOLD = 50.0;  // do C: vuot nguong -> bat may bom

DHT dht(DHT_PIN, DHT22);
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---------- Du lieu & dong bo ----------
struct SensorData { float temp; int gas; };

QueueHandle_t     dataQueue;   // day du -> Task truyen data
QueueHandle_t     alertQueue;  // canh bao -> Task canh bao
SemaphoreHandle_t stateMutex;  // bao ve trang thai thiet bi dung chung giua 2 core

// Trang thai thiet bi (truy cap qua stateMutex)
bool fanManual = false,  fanCmd = false;
bool pumpManual = false, pumpCmd = false;
bool fanOn = false, pumpOn = false, alarmOn = false;

// ---------- Nhan lenh tu web ----------
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  Serial.printf("[MQTT] Nhan lenh: %s\n", msg.c_str());

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if      (msg == "FAN_ON")   { fanManual = true;  fanCmd = true;  }
  else if (msg == "FAN_OFF")  { fanManual = true;  fanCmd = false; }
  else if (msg == "PUMP_ON")  { pumpManual = true; pumpCmd = true; }
  else if (msg == "PUMP_OFF") { pumpManual = true; pumpCmd = false; }
  else if (msg == "AUTO")     { fanManual = false; pumpManual = false; }
  xSemaphoreGive(stateMutex);
}

// ================= TASK TRUYEN DATA (Core 0) =================
void taskTransmit(void* pv) {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);

  for (;;) {
    // 1. Ket noi WiFi
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Dang ket noi...");
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    // 2. Ket noi MQTT
    if (!mqtt.connected()) {
      String id = "esp32-fire-" + String((uint32_t)esp_random(), HEX);
      if (mqtt.connect(id.c_str())) {
        mqtt.subscribe(TOPIC_CMD);
        Serial.println("[MQTT] Da ket noi broker");
      } else {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
    }
    mqtt.loop();

    // 3. WiFi + MQTT OK -> doc dataQueue va gui len broker
    SensorData d;
    if (xQueueReceive(dataQueue, &d, pdMS_TO_TICKS(100)) == pdTRUE) {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      bool f = fanOn, p = pumpOn, a = alarmOn;
      const char* mode = (fanManual || pumpManual) ? "MANUAL" : "AUTO";
      xSemaphoreGive(stateMutex);

      char json[160];
      snprintf(json, sizeof(json),
        "{\"temp\":%.1f,\"gas\":%d,\"fan\":%d,\"pump\":%d,\"alarm\":%d,\"mode\":\"%s\"}",
        d.temp, d.gas, f, p, a, mode);
      mqtt.publish(TOPIC_DATA, json);
      Serial.printf("[Core %d] Da gui: %s\n", xPortGetCoreID(), json);
    }
  }
}

// ================= TASK DOC DATA (Core 1) =================
void taskRead(void* pv) {
  for (;;) {
    float t = dht.readTemperature();
    int raw = analogRead(GAS_PIN);
    int ppm = map(raw, 0, 4095, 200, 10000);   // quy doi gan dung ADC -> ppm

    if (!isnan(t)) {
      SensorData d = { t, ppm };
      Serial.printf("[Core %d] Nhiet do: %.1f C | Gas: %d ppm\n", xPortGetCoreID(), t, ppm);
      xQueueSend(dataQueue, &d, 0);    // day du -> Task truyen data
      xQueueSend(alertQueue, &d, 0);   // canh bao -> Task canh bao
    } else {
      Serial.println("[DHT22] Loi doc cam bien");
    }
    vTaskDelay(pdMS_TO_TICKS(2000));   // DHT22 can >= 2s
  }
}

// ================= TASK CANH BAO (Core 1) =================
void taskAlert(void* pv) {
  SensorData d = { 25.0, 0 };
  for (;;) {
    SensorData n;
    if (xQueueReceive(alertQueue, &n, pdMS_TO_TICKS(200)) == pdTRUE) d = n;

    bool gasHigh  = d.gas  > GAS_THRESHOLD;
    bool tempHigh = d.temp > TEMP_THRESHOLD;

    xSemaphoreTake(stateMutex, portMAX_DELAY);
    fanOn   = fanManual  ? fanCmd  : gasHigh;    // Auto: quat theo khi gas
    pumpOn  = pumpManual ? pumpCmd : tempHigh;   // Auto: bom theo nhiet do
    alarmOn = gasHigh || tempHigh;
    bool f = fanOn, p = pumpOn, a = alarmOn;
    xSemaphoreGive(stateMutex);

    digitalWrite(FAN_PIN, f);
    digitalWrite(PUMP_PIN, p);
    if (a) tone(BUZZER_PIN, 2000); else noTone(BUZZER_PIN);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  dht.begin();

  dataQueue  = xQueueCreate(5, sizeof(SensorData));
  alertQueue = xQueueCreate(5, sizeof(SensorData));
  stateMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(taskTransmit, "Task truyen data", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskRead,     "Task doc data",    4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskAlert,    "Task canh bao",    4096, NULL, 2, NULL, 1);
}

void loop() { vTaskDelete(NULL); }
