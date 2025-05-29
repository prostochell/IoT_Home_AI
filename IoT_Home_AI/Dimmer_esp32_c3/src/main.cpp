/********************************************************************
 *  Wi-Fi MQTT Dimmer · ESP32-C3 · Zero-Cross + TRIAC (новий HAL)
 *******************************************************************/
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <cmath>

/* ---------- 1. NET CONFIG ---------- */
#define WIFI_SSID  "ReLab_2.4G"
#define WIFI_PASS  "relab2024"

#define MQTT_HOST  "192.168.50.175"
#define MQTT_PORT  1883
#define MQTT_USER  ""
#define MQTT_PASS  ""

#define MQTT_TOPIC "home/light/brightness"

/* ---------- 2. DIMMER PINS ---------- */
#define TRIAC_PIN  6          // → оптосимістор
#define ZC_PIN     7          // → детектор «нуль-переходу»

/* ---------- 3. SMOOTHNESS ---------- */
const float SMOOTH_STEP = 0.5f;  // % за цикл loop()
const int   LOOP_DELAY  = 5;     // мс

/* ---------- 4. GLOBALS ---------- */
WiFiClient   net;
PubSubClient mqtt(net);

volatile float  curBrightness = 0;   // 0…100 %
volatile float  setBrightness = 0;   // із MQTT

volatile uint16_t delayUs = 8500;    // фаза
volatile bool     fullOn  = false;

hw_timer_t* timer = nullptr;

/* ---------- 5. TRIAC PULSE & ZC ---------- */
void IRAM_ATTR fireTriac()
{
  digitalWrite(TRIAC_PIN, HIGH);
  delayMicroseconds(fullOn ? 300 : 50);
  digitalWrite(TRIAC_PIN, LOW);
}

void IRAM_ATTR onZC()
{
  timerAlarm(timer, fullOn ? 100 : delayUs, false, 0); 
}

inline uint16_t pctToDelay(float p)
{
  if (p >= 98.0f) return 100;                          // практично повний синус
  return 8500 - uint16_t(p * (8500 - 250) / 98.0f);    // 250…8500 µs
}

/* ---------- 6. MQTT CALLBACK ---------- */
void onMqtt(char* topic, byte* payload, unsigned len)
{
  if (strcmp(topic, MQTT_TOPIC)) return;

  char buf[8]{};  memcpy(buf, payload, min(len, 7u));
  setBrightness = constrain(atoi(buf), 0, 100);        // 0…100 %
}

/* ---------- 7. CONNECT HELPERS ---------- */
bool wifiReady = false;

void connectWiFi()
{
  if (wifiReady && WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Wi-Fi…");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(300); Serial.print('.');
  }
  wifiReady = (WiFi.status() == WL_CONNECTED);
  Serial.println(wifiReady ? " OK" : " FAIL");
  if (wifiReady)
    Serial.printf("IP  %s\n", WiFi.localIP().toString().c_str());
}

bool connectMQTT()
{
  if (!wifiReady) return false;
  if (mqtt.connected()) return true;

  String cid = "dimmer_" + String((uint32_t)ESP.getEfuseMac(), 16);
  Serial.print("MQTT…");
  if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS)) {
    mqtt.subscribe(MQTT_TOPIC, 1);
    Serial.println(" OK");
    return true;
  }
  Serial.printf(" FAIL (%d)\n", mqtt.state());
  return false;
}

/* ---------- 8. SETUP ---------- */
void setup()
{
  pinMode(TRIAC_PIN, OUTPUT);
  pinMode(ZC_PIN,    INPUT_PULLUP);
  digitalWrite(TRIAC_PIN, LOW);

  Serial.begin(9600);

  connectWiFi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setKeepAlive(30);

  /* --- HAL-таймер: 1 МГц (1 µs/тик) --- */
  timer = timerBegin(1'000'000);          // frequency = 1 MHz
  timerAttachInterrupt(timer, fireTriac); // ISR після alarm

  attachInterrupt(digitalPinToInterrupt(ZC_PIN), onZC, CHANGE);
}

/* ---------- 9. LOOP ---------- */
void loop()
{
  connectWiFi();
  connectMQTT();
  mqtt.loop();

  /* плавне наближення до setBrightness */
  if (fabs(setBrightness - curBrightness) > 0.01f) {
    if (SMOOTH_STEP <= 0.0f) {
      curBrightness = setBrightness;
    } else {
      float step = SMOOTH_STEP * ((setBrightness > curBrightness) ? 1 : -1);
      curBrightness += step;
      if ((step > 0 && curBrightness > setBrightness) ||
          (step < 0 && curBrightness < setBrightness))
        curBrightness = setBrightness;
    }

    noInterrupts();
    delayUs = pctToDelay(curBrightness);
    fullOn  = (curBrightness >= 98.0f);
    interrupts();

    Serial.printf("%.1f %% → delay %4u µs  full=%d\n",
                  curBrightness, delayUs, fullOn);
  }

  vTaskDelay(pdMS_TO_TICKS(LOOP_DELAY));
}
