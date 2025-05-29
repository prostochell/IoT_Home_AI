
/*********************************************************************
 *  ESP32-S3 HOST PANEL  –  Wi-Fi, MQTT, LVGL, OpenWeather, AI
 *  (Arduino core 3.1.1, LVGL 8.3)
 *********************************************************************/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

#include <lvgl.h>
#include <lv_png.h>

#include "esp_bsp.h"
#include "display.h"
#include "lv_port.h"

#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "SD_MMC.h"

#include <freertos/FreeRTOS.h>  // vTaskDelay
#include <cmath>                // fabs, log10

/* ---------- 1. Wi-Fi / MQTT ---------- */
#define WIFI_SSID "ReLab_2.4G"
#define WIFI_PASS "relab2024"
#define MQTT_HOST "192.168.50.175"
#define MQTT_PORT 1883
#define ICON_BUF_SIZE 8192
//#define PIC_FOLDER "/weather"
//#define DEMO_PIC 2
#define DEMO_MJPEG 3
#define MJPEG_FOLDER "/weather"
#define SD_MMC_D0 13
#define SD_MMC_CLK 12
#define SD_MMC_CMD 11

/* ---------- 2. AI / Weather ---------- */
#define PUTER_API_KEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ0IjoicyIsInYiOiIwLjAuMCIsInUiOiJtN2IzUDQyeFJZSzVSblc1K1ZlaHp3PT0iLCJ1dSI6IjcvbTVOc0NMVElxbW82Q3Z2N2lxK1E9PSIsImlhdCI6MTc0ODUyMTc2M30.-7nUBqEOGGc9fP3-w_DzjC81F5tgEt-ZizR-VOsZuvc"
#define OPENWX_KEY "f902e67ec7cdd18226193f26192df592"
#define AI_ENDPOINT "https://api.puter.com/v1/chat/completions"
#define AI_MODEL "gpt-4o-mini"
#define IAQ_MAX 200  // ← межа червоної зони

#include <ESP32_JPEG_Library.h>

#define WEATHER_JPEG_MAX (480 * 320 * 2)     // 300 KiB
static uint16_t* weather_img_buf = nullptr;  // RGB565 frame
static lv_img_dsc_t weather_img_dsc = {
  // LVGL дескриптор
  .header = { .cf = LV_IMG_CF_TRUE_COLOR, .always_zero = 0, .reserved = 0, .w = 480, .h = 320 },
  .data_size = WEATHER_JPEG_MAX,
  .data = nullptr,
};

static lv_obj_t* iaqArc = nullptr;   // дуга-індикатор
static lv_obj_t* iaqInfo = nullptr;  // текст усередині
/* ---------- 3. Глобальні об’єкти ---------- */
static uint8_t iconBuf[ICON_BUF_SIZE];
static uint32_t iconSize = 0;
static lv_img_dsc_t png_dsc;

WiFiClient net;
PubSubClient mqtt(net);

static uint32_t lastAI = 0;
static uint32_t nextWeather = 0;
static bool needWeather = false;

/* ---- індикатори на SCR_CONTROL ---- */
static lv_obj_t* lblTempIn = nullptr;
static lv_obj_t* lblHumIn = nullptr;
static lv_obj_t* lbliaqIn = nullptr;

/* ---------- 4. LVGL екрани ---------- */
enum ScreenID { SCR_ROOT,
                SCR_LIGHT,
                SCR_CLIMAT,
                SCR_CONTROL,
                SCR_AI,
                SCR_CNT };
static lv_obj_t* scr[SCR_CNT]{ nullptr };

static lv_obj_t *lblClim, *imgIcon, *lblIcon;
static lv_obj_t* aiAdviceLabel;
static bool aiJobRunning = false;
static TaskHandle_t aiTaskHandle = nullptr;

/* ---------- 5. Дані з датчиків (он-лайн) ---------- */
float tOut = 0, tIn = 0;
int co2 = 0, pm25 = 0;
bool acOn = false;
String wOut;

/* ========== 6. Мережеві функції ========== */
void wifiConnect() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(250);
}

bool mqttConnect() {
  if (mqtt.connected()) return true;
  String cid = "disp_" + String((uint32_t)ESP.getEfuseMac(), 16);
  if (mqtt.connect(cid.c_str())) {
    mqtt.subscribe("home/#", 1);
    return true;
  }
  return false;
}

/* MQTT callback */


void setIaq(uint16_t iaq) {
  if (!lblTempIn || !lblHumIn || !iaqArc || !iaqInfo) return;  // страхівка

  /* --- колір дуги --- */
  uint16_t val = lv_map(iaq, 0, IAQ_MAX, 0, 100);
  lv_arc_set_value(iaqArc, val);

  lv_color_t c = lv_color_hex(
    (iaq < 50) ? 0x00c853 : (iaq < 100) ? 0xcddc39
                          : (iaq < 150) ? 0xffc107
                          : (iaq < 200) ? 0xff6d00
                                        : 0xd50000);
  lv_obj_set_style_arc_color(iaqArc, c, LV_PART_INDICATOR);

  /* --- оновлення підписів --- */
  char buf[64];
  const char* t_txt = lv_label_get_text(lblTempIn);
  const char* h_txt = lv_label_get_text(lblHumIn);
  snprintf(buf, sizeof(buf),
           "Temp: %s\nHum : %s\nIAQ : %u",
           (t_txt && strlen(t_txt) > 6) ? t_txt + 7 : "--.-",
           (h_txt && strlen(h_txt) > 6) ? h_txt + 7 : "--",
           iaq);
  lv_label_set_text(iaqInfo, buf);
}




/* ---------- MQTT callback ---------- */
void onMqtt(char* topic, byte* payload, unsigned len) {
  /* копіюємо payload у локальний буфер */
  char msg[len + 1];
  memcpy(msg, payload, len);
  msg[len] = 0;

  if (!strcmp(topic, "home/climat/temp") && lblTempIn) {
    tIn = atof(msg);
    char buf[32];
    snprintf(buf, sizeof buf, "Temp : %.1f", atof(msg));
    if (bsp_display_lock(0)) {
      lv_label_set_text(lblTempIn, buf);
      bsp_display_unlock();
    }
  } else if (!strcmp(topic, "home/climat/hum") && lblHumIn) {
    char buf[32];
    snprintf(buf, sizeof buf, "Hum  : %.0f%", atof(msg));
    if (bsp_display_lock(0)) {
      lv_label_set_text(lblHumIn, buf);
      bsp_display_unlock();
    }
  } else if (!strcmp(topic, "home/climat/iaq") && lbliaqIn) {
    uint16_t iaq = uint16_t(atof(msg));
    char buf[32];
    snprintf(buf, sizeof buf, "IAQ  : %u", iaq);
    if (bsp_display_lock(0)) {
      lv_label_set_text(lbliaqIn, buf);
      setIaq(iaq);  // ← одна точка оновлення
      bsp_display_unlock();
    }
  }
}


/* ========== 7. OpenWeather ========== */
struct Weather {
  float temp = 0;
  int hum = 0;
  int pres = 0;
  char icon[4]{};
  char desc[20]{};
  bool ok = false;
};
static Weather fetchWeather();
void updateClimatUI(const Weather& w);

static Weather fetchWeather() {
  Weather w;
  WiFiClientSecure cli;
  cli.setInsecure();

  String url = String(F("https://api.openweathermap.org/data/2.5/weather"
                        "?lat=50.4501&lon=30.5234&units=metric&appid="))
               + OPENWX_KEY;

  HTTPClient http;
  if (!http.begin(cli, url)) return w;
  if (http.GET() != 200) return w;

  StaticJsonDocument<768> j;
  if (deserializeJson(j, http.getString()) != DeserializationError::Ok) return w;

  w.temp = j["main"]["temp"] | 0.0;
  w.hum = j["main"]["humidity"] | 0;
  w.pres = j["main"]["pressure"] | 0;
  strncpy(w.icon, j["weather"][0]["icon"] | "01d", 3);
  strncpy(w.desc, j["weather"][0]["main"] | "", 19);
  w.ok = true;
  return w;
}

/* ========== 8. LVGL UI ========== */
static const char* BTN_TXT[4] = { "Light", "Weather", "Climat\nin room", "  AI\nAdvice" };



void createLeftPanel(lv_obj_t* parent) {
  lv_obj_t* box = lv_obj_create(parent);
  lv_obj_set_size(box, 140, 300);
  lv_obj_align(box, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);
  lv_obj_set_style_pad_column(box, 14, 0);

  for (int i = 0; i < 4; ++i) {
    lv_obj_t* btn = lv_btn_create(box);
    lv_obj_set_size(btn, 110, 60);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_BLUE), 0);

    lv_obj_t* l = lv_label_create(btn);
    lv_label_set_text(l, BTN_TXT[i]);
    lv_obj_center(l);

    lv_obj_add_event_cb(btn, on_nav_btn, LV_EVENT_CLICKED, (void*)(intptr_t)(i + 1));
  }
}

/* ROOT */
void buildRootScreen() {
  lv_obj_t* r = lv_scr_act();
  scr[SCR_ROOT] = r;
  lv_obj_set_style_bg_color(r, lv_color_hex(0x0d1117), 0);
  createLeftPanel(r);
}

/* LIGHT SCREEN (без змін) ------------------------------------- */
static void on_arc_change(lv_event_t* e) {
  int v = lv_arc_get_value(lv_event_get_target(e));
  static uint32_t last_ms = 0;
  if (millis() - last_ms < 120) return;
  last_ms = millis();

  mqtt.publish("home/light/brightness", String(v).c_str(), true);

  lv_obj_t* lbl = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
  char buf[24];
  snprintf(buf, sizeof buf, "Brightness: %d%%", v);
  lv_label_set_text(lbl, buf);
}

void buildLightScreen() {
  lv_obj_t* s = lv_obj_create(NULL);
  scr[SCR_LIGHT] = s;
  lv_obj_set_style_bg_color(s, lv_color_hex(0x0d1117), 0);
  createLeftPanel(s);

  constexpr uint16_t ARC_ST = 135, ARC_SW = 270, ARC_TH = 20;
  static int ARC_VAL = 40;

  lv_obj_t* arc = lv_arc_create(s);
  lv_obj_set_size(arc, 220, 220);
  lv_obj_align(arc, LV_ALIGN_RIGHT_MID, -20, 0);
  lv_arc_set_bg_angles(arc, 0, ARC_SW);
  lv_arc_set_rotation(arc, ARC_ST);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, ARC_VAL);
  lv_obj_set_style_arc_width(arc, ARC_TH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);

  lv_obj_t* lbl = lv_label_create(s);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 90, 0);

  char buf[24];
  snprintf(buf, sizeof buf, "Brightness: %d%%", ARC_VAL);
  lv_label_set_text(lbl, buf);

  lv_obj_add_event_cb(arc, on_arc_change, LV_EVENT_VALUE_CHANGED, lbl);
}

/* CLIMAT */
void buildClimatScreen() {
  lv_obj_t* s = lv_obj_create(NULL);
  scr[SCR_CLIMAT] = s;
  lv_obj_set_style_bg_color(s, lv_color_hex(0x0d1117), 0);
  createLeftPanel(s);

  imgIcon = lv_img_create(s);
  lv_img_set_src(imgIcon, &weather_img_dsc);  // пустий спочатку
  lv_obj_align(imgIcon, LV_ALIGN_CENTER, 90, -40);

  lblClim = lv_label_create(s);
  lv_obj_set_style_text_color(lblClim, lv_color_hex(0xffffff), 0);
  lv_obj_align(lblClim, LV_ALIGN_CENTER, 90, 50);
}

static bool loadWeatherJpg(const char* fname) {
  if (!weather_img_buf) return false;  // SD або RAM немає
  File f = SD_MMC.open(fname, "r");
  if (!f) {
    Serial.println("icon not found");
    return false;
  }

  // читаємо файл у RAM по 1 кБ та декодуємо “на льоту”
  jpeg_dec_config_t cfg = { .output_type = JPEG_RAW_TYPE_RGB565_BE,
                            .rotate = JPEG_ROTATE_0D };
  jpeg_dec_handle_t* jd = jpeg_dec_open(&cfg);
  jpeg_dec_io_t io;
  jpeg_dec_header_info_t info;
  uint8_t readBuf[1024];

  io.inbuf = readBuf;
  io.outbuf = (uint8_t*)weather_img_buf;

  size_t totalIn = 0;
  jpeg_error_t err;  // ← нова змінна для коду помилки
  while (f.available()) {
    io.inbuf_len = f.read(readBuf, sizeof(readBuf));
    if (totalIn == 0) {  // перший шматок → парсимо header
      err = jpeg_dec_parse_header(jd, &io, &info);
      if (err != JPEG_ERR_OK) {
        Serial.printf("parse_header error %d\n", (int)err);
        break;
      }
    }
    err = jpeg_dec_process(jd, &io);
    if (err != JPEG_ERR_OK) {
      Serial.printf("process error %d\n", (int)err);
      break;
    }
    totalIn += io.inbuf_len;
  }
  jpeg_dec_close(jd);
  f.close();

  weather_img_dsc.header.w = info.width;
  weather_img_dsc.header.h = info.height;
  Serial.println("   ...fail");

  return (totalIn > 0);
}



void updateClimatUI(const Weather& w) {
  char buf[64];
  snprintf(buf, sizeof buf, "Temp: %.1f°C\nPs: %d hPa\n%s",
           w.temp, w.pres, w.desc);
  lv_label_set_text(lblClim, buf);
  char path[32];
  snprintf(path, sizeof path, "/weather/%.3s.jpg", w.icon);  // "01d" → /weather/01d.jpg
  Serial.printf(">> try open %s\n", path);

  if (loadWeatherJpg(path) && bsp_display_lock(0)) {
    lv_img_set_src(imgIcon, &weather_img_dsc);
    lv_obj_center(imgIcon);
    bsp_display_unlock();
  }
}

/* CONTROL */
void buildControlScreen() {
  lv_obj_t* s = lv_obj_create(NULL);
  scr[SCR_CONTROL] = s;
  lv_obj_set_style_bg_color(s, lv_color_hex(0x0d1117), 0);
  createLeftPanel(s);

  /* ---------- 1. навігаційні лейбли ---------- */
  lblTempIn = lv_label_create(s);
  lv_obj_set_style_text_color(lblTempIn, lv_color_hex(0xffffff), 0);
  lv_obj_align(lblTempIn, LV_ALIGN_CENTER, 90, -20);
  lv_label_set_text(lblTempIn, "Temp : --.-");

  lblHumIn = lv_label_create(s);
  lv_obj_set_style_text_color(lblHumIn, lv_color_hex(0xffffff), 0);
  lv_obj_align(lblHumIn, LV_ALIGN_CENTER, 90, 0);
  lv_label_set_text(lblHumIn, "Hum  : --");

  lbliaqIn = lv_label_create(s);
  lv_obj_set_style_text_color(lbliaqIn, lv_color_hex(0xffffff), 0);
  lv_obj_align(lbliaqIn, LV_ALIGN_CENTER, 90, 20);
  lv_label_set_text(lbliaqIn, "IAQ  : --");

  /* ---------- 2. IAQ дуга ---------- */
  constexpr uint16_t ARC_ST = 135, ARC_SW = 270, ARC_TH = 20;
  iaqArc = lv_arc_create(s);
  lv_obj_set_size(iaqArc, 220, 220);
  lv_obj_align(iaqArc, LV_ALIGN_CENTER, 90, 10);
  lv_arc_set_bg_angles(iaqArc, 0, ARC_SW);
  lv_arc_set_rotation(iaqArc, ARC_ST);
  lv_arc_set_range(iaqArc, 0, 100);
  lv_obj_set_style_arc_width(iaqArc, ARC_TH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(iaqArc, ARC_TH, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(iaqArc, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_arc_color(iaqArc, lv_color_hex(0x2e2e2e), LV_PART_MAIN);
  lv_obj_clear_flag(iaqArc, LV_OBJ_FLAG_CLICKABLE);

  /* ---------- IAQ info label усередині дуги ---------- */
  iaqInfo = lv_label_create(iaqArc);  // ≤— новий лейбл!
  lv_obj_center(iaqInfo);
  lv_label_set_recolor(iaqInfo, true);  // необов'язково: дозволяє #RRGGBB всередині тексту
                                        // lv_label_set_text(iaqInfo, "Temp: --.-\nHum : --\nIAQ : 0");

  /* тепер можна ініціалізувати індикатор */
  setIaq(0);
}

/* =========  AI helper  ========= */
static String askAI(const String& prompt) {

  WiFiClientSecure cli;
  cli.setInsecure();  // Puter має валідний сертифікат

  HTTPClient http;
  if (!http.begin(cli, AI_ENDPOINT)) return "AI connect error";

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " PUTER_API_KEY);
  /* тіло POST */
  StaticJsonDocument<512> jreq;
  jreq["model"] = AI_MODEL;
  JsonArray msgs = jreq.createNestedArray("messages");
  JsonObject sys = msgs.createNestedObject();
  sys["role"] = "system";
  sys["content"] =
    "You are a smart home assistant that gives VERY SHORT (<=30 words) "
    "advice on improving indoor climate.";

  JsonObject usr = msgs.createNestedObject();
  usr["role"] = "user";
  usr["content"] = prompt;

  String body;
  serializeJson(jreq, body);

  int code = http.POST(body);
  Serial.printf("[AI] HTTP code = %d\n", code);  // ← DEBUG
  Serial.printf("[AI] Resp. len = %d B\n", http.getSize());
  if (code != 200) return "HTTP " + String(code);

  /* читаємо ~ 1 kB JSON-відповідь */
  StaticJsonDocument<768> jres;
  DeserializationError e = deserializeJson(jres, http.getString());
  if (e) return "JSON error";

  const char* txt = jres["choices"][0]["message"]["content"] | "No answer";
  return String(txt);
}

void on_nav_btn(lv_event_t* e) {
  auto id = static_cast<ScreenID>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
  if (!scr[id]) return;
  lv_scr_load(scr[id]);
  if (id == SCR_CLIMAT) needWeather = true;

  if (id == SCR_AI && !aiJobRunning) {
    lv_label_set_text(aiAdviceLabel, "Querying AI…");
    aiJobRunning = true;

    xTaskCreatePinnedToCore([](void*) {
      /* 1. Формуємо prompt з live-даних */
      char pbuf[160];
      snprintf(pbuf, sizeof pbuf,
               "Indoor T %.1f°C, RH %.0f%%, IAQ %d; "
               "Outdoor T %.1f°C, Weather '%s'. "
               "Give actionable advice.",
               tIn, atof(lv_label_get_text(lblHumIn) + 7),
               atoi(lv_label_get_text(lbliaqIn) + 6),
               tOut, wOut.c_str());

      String reply = askAI(String(pbuf));

      /* 2. Передаємо текст у GUI-потік */
      lv_async_call([](void* p) {
        lv_label_set_text(aiAdviceLabel, (const char*)p);
        free(p);  // виділили нижче → звільняємо тут
        aiJobRunning = false;
      },
                    strdup(reply.c_str()));  // копія під LVGL

      vTaskDelete(nullptr);
    },
                            "aiJob", 8192, nullptr, 2, &aiTaskHandle, 1);
  }
}
/* AI-ADVICE (без змін) ---------------------------------------- */
void buildAiAdviceScreen() {
  lv_obj_t* s = lv_obj_create(NULL);
  scr[SCR_AI] = s;
  lv_obj_set_style_bg_color(s, lv_color_hex(0x0d1117), 0);
  createLeftPanel(s);

  lv_obj_t* cont = lv_obj_create(s);
  lv_obj_set_size(cont, 300, 90);
  lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_pad_all(cont, 4, 0);

  aiAdviceLabel = lv_label_create(cont);
  lv_obj_set_width(aiAdviceLabel, 280);
  lv_label_set_long_mode(aiAdviceLabel, LV_LABEL_LONG_WRAP);
  lv_label_set_text(aiAdviceLabel, "Waiting…");
}

/* ========== 9. setup() ========== */
void setup() {
  Serial.begin(115200);

  /* 1. Графічний інтерфейс ПЕРШИМ */
  bsp_display_cfg_t cfg = {
    .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
    .buffer_size = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
    .rotate = LV_DISP_ROT_90
  };
  bsp_display_start_with_config(&cfg);
  bsp_display_backlight_on();

  bsp_display_lock(0);
  buildRootScreen();
  buildLightScreen();
  buildClimatScreen();
  buildControlScreen();
  buildAiAdviceScreen();
  bsp_display_unlock();

  /* 2. Потім мережа */
  wifiConnect();  // з тайм-аутом!
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  while (time(nullptr) < 1'700'000'000) delay(200);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);

  if (!SD_MMC.begin("/sdmmc", true, false, 20000)) {
    Serial.println("SD mount failed – weather icons будуть відсутні");
  } else {
    weather_img_buf = (uint16_t*)heap_caps_aligned_alloc(16,
                                                         WEATHER_JPEG_MAX, MALLOC_CAP_SPIRAM);
    weather_img_dsc.data = (uint8_t*)weather_img_buf;
  }
  File root = SD_MMC.open("/weather");
  while (File f = root.openNextFile()) {
    Serial.println(f.path());
    f.close();
  }
  root.close();
}

/* ========== 10. loop() ========== */
void loop() {
  wifiConnect();
  mqttConnect();
  mqtt.loop();

  /* --- погода --- */
  if (((lv_scr_act() == scr[SCR_CLIMAT]) && millis() > nextWeather) || needWeather) {
    Weather w = fetchWeather();
    if (w.ok) {
      updateClimatUI(w);
      tOut = w.temp;  //  ← нове
      wOut = w.desc;
      needWeather = false;
      nextWeather = millis() + 600000UL;  // 10 хв
    }
  }

  /* --- AI кожні 10 хв або при натисканні --- */
  if (millis() - lastAI > 600000UL && !aiJobRunning) {
    needWeather = true;  // prompt потребує свіжих даних
    lastAI = millis();
  }

  lv_timer_handler();
  vTaskDelay(pdMS_TO_TICKS(5));
}
