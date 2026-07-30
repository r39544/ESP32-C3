// ============================================================
//  ESP32-C3 + SSD1306 OLED  天气预报站
//  -------------------------------------------------------
//  - 通过 WiFi 连接 OpenWeatherMap API
//  - 显示温度、天气图标、湿度、风速、气压
//  - 30 分钟自动刷新（可配置）
//  - NTP 时间同步
//  - 支持中文 / 英文天气描述
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include "config.h"

// ============================================================
//  全局对象 & 状态
// ============================================================
U8G2_SSD1306_128X64_NONAME_1_SW_I2C u8g2(U8G2_R0, OLED_SCL, OLED_SDA, U8X8_PIN_NONE);

static WiFiMulti wifiMulti;

// ---- 天气数据结构 ----
struct WeatherData {
  String   city;
  String   country;
  String   description;       // 例如 "晴" / "Clear"
  int      iconCode;          // OWM 图标数字部分: 01, 02 ... 50
  bool     isNight;           // true = 夜间图标（n 后缀）
  float    temperature;       // °C
  int      humidity;          // %
  float    windSpeed;         // m/s
  int      pressure;          // hPa
  bool     valid = false;
};

static WeatherData weather;
static unsigned long lastFetchMs = 0;
static unsigned long fetchIntervalMs = (WEATHER_UPDATE_INTERVAL * 60 * 1000UL);
static int wifiRssi = 0;

// ============================================================
//  本地时间缓存
// ============================================================
static int timeHour   = 0;
static int timeMinute = 0;
static bool timeSynced = false;

// ============================================================
//  天气图标 — 纯程序绘制（无需位图数据）
// ============================================================
namespace Icons {
  // ---- 太阳 ----
  static void drawSun(int cx, int cy, int r) {
    u8g2.drawDisc(cx, cy, r, U8G2_DRAW_ALL);
    // 8 条射线
    for (int a = 0; a < 8; a++) {
      float rad = a * PI / 4;
      int x1 = cx + (int)(cosf(rad) * (r + 2));
      int y1 = cy + (int)(sinf(rad) * (r + 2));
      int x2 = cx + (int)(cosf(rad) * (r + 5));
      int y2 = cy + (int)(sinf(rad) * (r + 5));
      u8g2.drawLine(x1, y1, x2, y2);
    }
  }

  // ---- 弯月 ----
  static void drawMoon(int cx, int cy, int r) {
    u8g2.drawDisc(cx, cy, r, U8G2_DRAW_ALL);
    // 用背景色挖去一块形成弯月
    u8g2.setDrawColor(0);
    u8g2.drawDisc(cx + 3, cy - 2, r - 1, U8G2_DRAW_ALL);
    u8g2.setDrawColor(1);
    // 星星点缀
    u8g2.drawPixel(cx - r - 1, cy - r + 2);
    u8g2.drawPixel(cx - r + 3, cy + r - 3);
    u8g2.drawPixel(cx + 1, cy - r - 1);
  }

  // ---- 云朵 ----
  static void drawCloud(int cx, int cy, int s) {
    // s ≈ 整体大小倍数
    u8g2.drawDisc(cx,      cy,      4 * s, U8G2_DRAW_ALL);
    u8g2.drawDisc(cx - 6 * s, cy + 2 * s, 3 * s, U8G2_DRAW_ALL);
    u8g2.drawDisc(cx + 7 * s, cy + 1 * s, 4 * s, U8G2_DRAW_ALL);
    u8g2.drawDisc(cx + 3 * s, cy - 3 * s, 3 * s, U8G2_DRAW_ALL);
    // 底部填平
    u8g2.drawBox(cx - 10 * s, cy, 20 * s, 4 * s);
  }

  // ---- 雨滴 ----
  static void drawRain(int cx, int cy) {
    for (int i = -1; i <= 1; i++) {
      int rx = cx + i * 8;
      u8g2.drawLine(rx, cy, rx - 1, cy + 5);
      u8g2.drawLine(rx + 1, cy + 6, rx, cy + 11);
    }
  }

  // ---- 雪花 ----
  static void drawSnow(int cx, int cy) {
    for (int i = -1; i <= 1; i++) {
      int sx = cx + i * 8, sy = cy;
      u8g2.drawPixel(sx, sy);
      u8g2.drawLine(sx - 1, sy - 2, sx + 1, sy + 2);
      u8g2.drawLine(sx - 1, sy + 2, sx + 1, sy - 2);
    }
  }

  // ---- 闪电 ----
  static void drawLightning(int cx, int cy) {
    u8g2.drawLine(cx - 2, cy - 6,  cx + 3, cy - 6);
    u8g2.drawLine(cx + 3, cy - 6,  cx - 1, cy + 1);
    u8g2.drawLine(cx - 1, cy + 1,  cx + 1, cy + 1);
    u8g2.drawLine(cx + 1, cy + 1,  cx - 3, cy + 8);
  }

  // ---- 雾 ----
  static void drawFog(int cx, int cy) {
    for (int row = 0; row < 3; row++) {
      int yy = cy + row * 5 - 5;
      u8g2.drawBox(cx - 10, yy, 20, 2);
      u8g2.drawBox(cx - 5, yy + 2, 15, 1);
    }
  }

  // ============================================================
  //  主入口：根据 OWM icon code 绘制完整图标 (24×24 区域)
  // ============================================================
  void draw(int x, int y, int code, bool night) {
    int cx = x + 12;  // 中心 x
    int cy = y + 12;  // 中心 y

    switch (code) {
      case 1:   // 晴
        if (night) drawMoon(cx, cy, 5);
        else       drawSun(cx, cy, 5);
        break;

      case 2:   // 少云
        if (night) drawMoon(cx - 6, cy - 2, 4);
        else       drawSun(cx - 6, cy - 2, 4);
        drawCloud(cx + 4, cy + 2, 1);
        break;

      case 3:   // 多云
      case 4:   // 阴
        drawCloud(cx, cy, 1);
        break;

      case 9:   // 阵雨
      case 10:  // 雨
        drawCloud(cx - 1, cy - 5, 1);
        drawRain(cx, cy + 5);
        break;

      case 11:  // 雷暴
        drawCloud(cx - 1, cy - 6, 1);
        drawLightning(cx, cy + 4);
        break;

      case 13:  // 雪
        drawCloud(cx - 1, cy - 5, 1);
        drawSnow(cx, cy + 6);
        break;

      case 50:  // 雾 / 霾
        drawFog(cx, cy);
        break;

      default:
        drawCloud(cx, cy, 1);
        break;
    }
  }
} // namespace Icons

// ============================================================
//  WiFi 连接
// ============================================================
static bool connectWiFi() {
  if (wifiMulti.run() == WL_CONNECTED) {
    wifiRssi = WiFi.RSSI();
    return true;
  }

  Serial.print("Connecting WiFi");
  unsigned long t0 = millis();
  wifiMulti.addAP(WIFI_SSID1, WIFI_PASSWORD1);
#ifdef WIFI_SSID2
  wifiMulti.addAP(WIFI_SSID2, WIFI_PASSWORD2);
#endif

  while (millis() - t0 < 10000) {  // 最多等 10s
    if (wifiMulti.run() == WL_CONNECTED) {
      wifiRssi = WiFi.RSSI();
      Serial.printf("\nWiFi OK  RSSI: %d dBm\n", wifiRssi);
      return true;
    }
    Serial.print(".");
    delay(200);
  }
  Serial.println("\nWiFi FAILED");
  return false;
}

// ============================================================
//  NTP 时间同步
// ============================================================
static void syncNTP() {
  configTime(TZ_OFFSET, 0, NTP_SERVER1, NTP_SERVER2);
  time_t now = time(nullptr);
  int tries = 0;
  while (now < 100000 && tries < 20) {
    delay(250);
    now = time(nullptr);
    tries++;
  }
  if (now > 100000) {
    struct tm *ti = localtime(&now);
    timeHour   = ti->tm_hour;
    timeMinute = ti->tm_min;
    timeSynced = true;
    Serial.printf("NTP: %02d:%02d\n", timeHour, timeMinute);
  } else {
    timeSynced = false;
    Serial.println("NTP FAILED");
  }
}

// ============================================================
//  获取天气数据
// ============================================================
static bool fetchWeather() {
  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/weather"
               "?q=" + String(OWM_CITY) +
               "&appid=" + String(OWM_API_KEY) +
               "&units=metric&lang=" + String(OWM_LANG);

  Serial.println("Fetching: " + url);
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode <= 0) {
    Serial.printf("HTTP error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }
  if (httpCode != 200) {
    Serial.printf("HTTP %d\n", httpCode);
    String body = http.getString();
    Serial.println(body.substring(0, 200));
    http.end();
    return false;
  }

  // 解析 JSON
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return false;
  }

  // ---- 提取数据 ----
  weather.city        = doc["name"].as<String>();
  weather.country     = doc["sys"]["country"].as<String>();
  weather.description = doc["weather"][0]["description"].as<String>();
  weather.temperature = doc["main"]["temp"].as<float>();
  weather.humidity    = doc["main"]["humidity"].as<int>();
  weather.windSpeed   = doc["wind"]["speed"].as<float>();
  weather.pressure    = doc["main"]["pressure"].as<int>();
  weather.valid       = true;

  // 解析 icon code
  String iconStr = doc["weather"][0]["icon"].as<String>();
  weather.iconCode = iconStr.substring(0, 2).toInt();
  weather.isNight  = iconStr.endsWith("n");

  Serial.printf("Weather: %s %.1f°C  %s  icon=%s\n",
                weather.city.c_str(), weather.temperature,
                weather.description.c_str(), iconStr.c_str());
  return true;
}

// ============================================================
//  显示界面
// ============================================================
static void drawSignalBars(int x, int y) {
  int bars;
  if (wifiRssi > -55)      bars = 4;
  else if (wifiRssi > -65) bars = 3;
  else if (wifiRssi > -78) bars = 2;
  else if (wifiRssi > -88) bars = 1;
  else                     bars = 0;
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    u8g2.drawFrame(x + i * 5, y - h, 3, h);
    if (i < bars) u8g2.drawBox(x + i * 5 + 1, y - h + 1, 1, h - 2);
  }
}

static void updateDisplay() {
  u8g2.firstPage();
  do {
    // ========== 第 1 行: 天气图标 + 温度 ==========
    if (weather.valid) {
      // 图标 (左上)
      Icons::draw(0, 0, weather.iconCode, weather.isNight);

      // 温度 (大号)
      u8g2.setFont(u8g2_font_logisoso24_tf);
      char tempStr[16];
      snprintf(tempStr, sizeof(tempStr), "%.0f\xb0", weather.temperature);
      u8g2.drawStr(28, 22, tempStr);

      // 天气描述（根据语言选择字体）
#ifdef USE_CHINESE_FONT
      u8g2.setFont(u8g2_font_wqy12_t_chinese1);
      u8g2.setFontDirection(0);
#else
      u8g2.setFont(u8g2_font_helvR08_tr);
#endif
      u8g2.drawStr(28, 34, weather.description.c_str());
    } else {
      // 无数据：显示 "---"
      u8g2.setFont(u8g2_font_logisoso24_tf);
      u8g2.drawStr(28, 22, "---");
      u8g2.setFont(u8g2_font_helvR08_tr);
      u8g2.drawStr(28, 34, "No data");
    }

    // ========== 第 2 行: 城市 & 时间 ==========
    u8g2.setFont(u8g2_font_helvR08_tr);
    char locStr[32];
    if (weather.valid) {
      snprintf(locStr, sizeof(locStr), "%s, %s",
               weather.city.c_str(), weather.country.c_str());
    } else {
      snprintf(locStr, sizeof(locStr), "%s", OWM_CITY);
    }
    u8g2.drawStr(0, 50, locStr);

    // 右上角时间
    if (timeSynced) {
      char timeStr[8];
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d", timeHour, timeMinute);
      int tw2 = u8g2.getStrWidth(timeStr);
      u8g2.drawStr(128 - tw2, 50, timeStr);
    }

    // ========== 第 3 行: 湿度 / 风速 / 气压 ==========
    u8g2.setFont(u8g2_font_helvR08_tr);
    char detailStr[64];
    if (weather.valid) {
      snprintf(detailStr, sizeof(detailStr),
               "%d%%  %.1fm/s  %dhPa",
               weather.humidity, weather.windSpeed, weather.pressure);
    } else {
      snprintf(detailStr, sizeof(detailStr),
               "Connecting...");
    }
    u8g2.drawStr(0, 62, detailStr);

    // ========== WiFi 信号条 ==========
    int tw3 = u8g2.getStrWidth(detailStr);
    if (wifiMulti.run() == WL_CONNECTED) {
      drawSignalBars(tw3 + 4, 62);
    }

  } while (u8g2.nextPage());
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-C3 Weather Station ===");

  // ---- 探测 I2C 设备 ----
  Wire.begin(OLED_SDA, OLED_SCL);
  Serial.printf("I2C: SDA=%d SCL=%d\n", OLED_SDA, OLED_SCL);
  Serial.print("Scanning I2C bus ...");
  byte error, addr;
  int nDevices = 0;
  for (addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    error = Wire.endTransmission();
    if (error == 0) {
      Serial.printf(" found 0x%02X", addr);
      nDevices++;
    }
  }
  if (nDevices == 0) Serial.print(" (none found)");
  Serial.println();

  // ---- 初始化 OLED ----
  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setContrast(200);
  u8g2.clearBuffer();

  // 显示测试图案 —— 确认 OLED 通信正常
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_helvR10_tr);
    u8g2.drawStr(12, 30, "Weather");
    u8g2.drawStr(12, 46, "Station");
    u8g2.setFont(u8g2_font_helvR08_tr);
    u8g2.drawStr(12, 60, "Connecting...");
  } while (u8g2.nextPage());
  Serial.println("OLED init done");

  // ---- WiFi ----
  connectWiFi();
  if (wifiMulti.run() == WL_CONNECTED) {
    syncNTP();
    fetchWeather();
  }

  updateDisplay();
  lastFetchMs = millis();
}

void loop() {
  unsigned long now = millis();
  bool needRedraw = false;

  // ---- 定时获取 ----
  if (now - lastFetchMs >= fetchIntervalMs) {
    lastFetchMs = now;

    if (wifiMulti.run() != WL_CONNECTED) {
      connectWiFi();
    }

    if (wifiMulti.run() == WL_CONNECTED) {
      // 同步时间（每 6 小时校准一次）
      static unsigned long lastNtpSync = 0;
      if (now - lastNtpSync > 6 * 3600 * 1000UL) {
        syncNTP();
        lastNtpSync = now;
      }
      // 刷新本地时间（即使 NTP 失败也保持走动）
      time_t t = time(nullptr);
      if (t > 100000) {
        struct tm *ti = localtime(&t);
        timeHour   = ti->tm_hour;
        timeMinute = ti->tm_min;
        timeSynced = true;
      }
      fetchWeather();
    }
    needRedraw = true;
  }

  // ---- 每分钟更新一次时间显示 ----
  static unsigned long lastMinuteCheck = 0;
  if (now - lastMinuteCheck >= 60000) {
    lastMinuteCheck = now;
    time_t t = time(nullptr);
    if (t > 100000) {
      struct tm *ti = localtime(&t);
      int newHour   = ti->tm_hour;
      int newMinute = ti->tm_min;
      if (newHour != timeHour || newMinute != timeMinute) {
        timeHour   = newHour;
        timeMinute = newMinute;
        needRedraw = true;
      }
    }
  }

  if (needRedraw) {
    updateDisplay();
  }

  // 低功耗等待
  delay(100);
}
