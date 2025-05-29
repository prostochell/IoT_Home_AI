/*********************************************************************
 *  esp32c3_bme680_pub.ino – T/H + псевдо-IAQ → MQTT
 *********************************************************************/
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_BME680.h>
#include <ArduinoJson.h>

/* ---------- 1. Wi-Fi / MQTT ---------- */
#define WIFI_SSID  "ReLab_2.4G"
#define WIFI_PASS  "relab2024"
#define MQTT_HOST  "192.168.50.175"
#define MQTT_PORT  1883
#define PUBLISH_MS 5000

/* ---------- 2. Об'єкти ---------- */
WiFiClient      net;
PubSubClient    mqtt(net);
Adafruit_BME680 bme;               // I²C (SDA 8, SCL 9)

volatile bool wifiReady = false;
unsigned long  tLastPub = 0;

/* ---------- 3. IAQ-евристика ---------- */
uint16_t calcIAQ(float gas_kOhm, float hum)
{
  float gasScore = constrain((log10(gas_kOhm) - 3.0f) * 100.0f, 0.0f, 350.0f);
  float humScore = constrain(fabs(hum - 40.0f) * 2.5f,            0.0f, 150.0f);
  return uint16_t(constrain(gasScore + humScore, 0.0f, 500.0f));
}

/* ---------- 4. Wi-Fi події ---------- */
void WiFiEvent(WiFiEvent_t ev, arduino_event_info_t info)
{
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("\n✔ Wi-Fi %s  IP=%s  RSSI=%ddBm\n",
             WiFi.SSID().c_str(),
             WiFi.localIP().toString().c_str(),
             WiFi.RSSI());
      wifiReady = true;
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("✖ Wi-Fi lost (reason %d) → retry\n",
                    info.wifi_sta_disconnected.reason);
      wifiReady = false;

      /* коректний цикл перепідключення */
      WiFi.disconnect(true,   /*wifioff=*/false);
      delay(200);                         // дати радіо «охолонути»
      WiFi.begin(WIFI_SSID, WIFI_PASS);   // нова спроба
      break;

    default:
      break;
  }
}

/* ---------- 5. MQTT ---------- */
bool connectMQTT()
{
  if (!wifiReady) return false;
  if (mqtt.connected()) return true;

  String cid = "c3_bme680_" + String((uint32_t)ESP.getEfuseMac(), 16);
  Serial.print("MQTT… ");
  if (mqtt.connect(cid.c_str())) {
    Serial.println("OK");
    return true;
  }
  Serial.printf("FAIL (%d)\n", mqtt.state());
  return false;
}

/* ---------- 6. Публікація ---------- */
void publishReading()
{
  if (!mqtt.connected()) return;
  if (!bme.performReading()) {
    Serial.println("BME680 read error");
    return;
  }

  float temp = bme.temperature;                  // °C
  float hum  = bme.humidity;                    // %
  float gas  = bme.gas_resistance / 1000.0;     // kΩ
  uint16_t iaq = calcIAQ(gas, hum);             // 0…500

  /* 6.1 окремі retain-топіки */
  mqtt.publish("home/climat/temp", String(temp, 1).c_str(), true);
  mqtt.publish("home/climat/hum",  String(hum,  0).c_str(), true);
  mqtt.publish("home/climat/iaq",  String(iaq).c_str(),     true);

  /* 6.2 JSON для логування */
  StaticJsonDocument<128> doc;
  doc["t"]   = temp;
  doc["h"]   = hum;
  doc["iaq"] = iaq;

  char buf[128];
  serializeJson(doc, buf);
  mqtt.publish("sensors/bme680", buf);          // без явної довжини

  Serial.printf("PUB: %s\n", buf);
}

/* ---------- 7. Setup ---------- */
void setup()
{
  Serial.begin(9600);
  WiFi.onEvent(WiFiEvent);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
while (WiFi.status() != WL_CONNECTED) {  // чекаємо максимум 10 с
    delay(250);
    Serial.print('.');
    if (millis() > 10000) break;
}

if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nIP  %s  GW  %s  RSSI %d dBm\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.RSSI());
} else {
    Serial.println("\n!!! Wi-Fi NOT connected");
}
Serial.println("---------------");
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  Wire.begin(8, 9, 400000);
  if (!bme.begin()) {
    Serial.println("× BME680 not found");
    while (true) delay(1000);
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling   (BME680_OS_2X);
  bme.setPressureOversampling   (BME680_OS_4X);
  bme.setGasHeater(320, 150);
}

/* ---------- 8. Loop ---------- */
void loop()
{
  connectMQTT();
  mqtt.loop();

  unsigned long now = millis();
  if (now - tLastPub >= PUBLISH_MS) {
    tLastPub = now;
    publishReading();
  }
  vTaskDelay(pdMS_TO_TICKS(20));
}
