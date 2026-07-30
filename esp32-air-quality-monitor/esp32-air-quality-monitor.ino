/*
 * ESP32-C3 Air Quality Monitor
 * ─────────────────────────────
 * Sensors:  ENS160 (VOC / eCO₂ / AQI)
 *            AHT21 (temperature / humidity)
 * Display:  SSD1306 OLED 128×64 (I²C)
 * Network:  WiFi + built-in WebServer (NO external libraries needed)
 *
 * Wiring (I²C bus — all three devices share the same SDA/SCL):
 *
 *   ESP32-C3 | ENS160 | AHT21 | SSD1306
 *   ─────────┼────────┼───────┼────────
 *   3V3      │ VIN    │ VIN   │ VCC
 *   GND      │ GND    │ GND   │ GND
 *   GPIO 4   │ SDA    │ SDA   │ SDA
 *   GPIO 5   │ SCL    │ SCL   │ SCL
 *
 *   I²C addresses:  ENS160 = 0x53
 *                   AHT21  = 0x38
 *                   SSD1306 = 0x3C
 *
 * ─── Board pin notes ────────────────
 *   Seeed XIAO C3:       SDA=4,  SCL=5   (selected below)
 *   ESP32-C3-DevKitM-1:  SDA=8,  SCL=9
 *   AirM2M C3:           SDA=6,  SCL=7
 *   Lolin C3 Mini:       SDA=8,  SCL=9
 *
 *   Change I2C_SDA / I2C_SCL below to match your board.
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_AHTX0.h>
#include <ScioSense_ENS160.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>

// ═══════════════════════════════════════════════════════════════════
//  USER CONFIGURATION
// ═══════════════════════════════════════════════════════════════════

// --- I²C pins (match your board) -----------------------------------
#define I2C_SDA   4
#define I2C_SCL   5

// --- OLED ----------------------------------------------------------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C

// --- WiFi (set credentials in config.h) ----------------------------
#include "config.h"

// --- Sensor read interval -------------------------------------------
#define READ_INTERVAL_MS  1000   // every 1 second

// --- NTP time --------------------------------------------------------
#define TZ_OFFSET_SEC  (8 * 3600)   // UTC+8 (China) — change for your zone
#define NTP_SERVER1    "ntp2.aliyun.com"
#define NTP_SERVER2    "ntp3.aliyun.com"

// ═══════════════════════════════════════════════════════════════════
//  GLOBALS
// ═══════════════════════════════════════════════════════════════════

// Display
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Sensors
Adafruit_AHTX0    aht;
ScioSense_ENS160  ens160(ENS160_I2CADDR_1);   // 0x53

// Web server (built-in — no external libraries needed)
WebServer server(80);

// Shared sensor snapshot (updated every READ_INTERVAL_MS)
struct {
  float    temperature = 0.0f;   // °C
  float    humidity    = 0.0f;   // % RH
  uint16_t tvoc        = 0;      // ppb
  uint16_t eco2        = 0;      // ppm
  uint8_t  aqi         = 0;      // 1–5 (0 = invalid)
  bool     aht_ok      = false;
  bool     ens160_ok   = false;
  uint32_t last_read_ms = 0;
  // Min/Max tracking
  float    temp_min = 999.0f,  temp_max = -999.0f;
  float    hum_min  = 999.0f,  hum_max  = -999.0f;
  uint16_t eco2_min = 65535,   eco2_max = 0;
  uint16_t tvoc_min = 65535,   tvoc_max = 0;
  // Timestamps (epoch seconds) when each min/max was set
  time_t   temp_min_time,  temp_max_time;
  time_t   hum_min_time,   hum_max_time;
  time_t   eco2_min_time,  eco2_max_time;
  time_t   tvoc_min_time,  tvoc_max_time;
  // Current time string (updated every read cycle)
  char     time_str[9] = "--:--:--";
  bool     time_synced = false;
} data;

// ═══════════════════════════════════════════════════════════════════
//  HTML / CSS / JS  (embedded — served as the web dashboard)
// ═══════════════════════════════════════════════════════════════════
//  Uses plain fetch() polling (no WebSocket, no external deps)

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<title>Air Quality Monitor</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    background: #0f0f1a; color: #e0e0e0; min-height: 100vh; display: flex;
    align-items: center; justify-content: center; padding: 16px;
  }
  .dashboard {
    width: 100%; max-width: 480px; display: flex; flex-direction: column; gap: 16px;
  }
  .aqi-card {
    background: linear-gradient(135deg, #1a1a2e, #16213e);
    border-radius: 24px; padding: 28px 24px 24px; text-align: center;
    box-shadow: 0 8px 32px rgba(0,0,0,.4);
  }
  .aqi-label { font-size: 14px; text-transform: uppercase; letter-spacing: 3px; opacity: .6; margin-bottom: 4px; }
  .aqi-value { font-size: 72px; font-weight: 800; line-height: 1; }
  .aqi-text  { font-size: 22px; font-weight: 600; margin-top: 4px; }
  .aqi-bar   { margin-top: 16px; height: 6px; border-radius: 3px;
               background: #333; overflow: hidden; }
  .aqi-fill  { height: 100%; width: 0%; border-radius: 3px;
               transition: width .6s ease, background .6s ease; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
  .card {
    background: #1a1a2e; border-radius: 20px; padding: 20px 16px 16px;
    box-shadow: 0 4px 20px rgba(0,0,0,.3);
  }
  .card-label { font-size: 12px; text-transform: uppercase; letter-spacing: 2px; opacity: .5; }
  .card-value { font-size: 28px; font-weight: 700; margin-top: 4px; }
  .card-unit  { font-size: 14px; font-weight: 400; opacity: .6; margin-left: 2px; }
  .card-minmax { font-size: 10px; line-height: 1.6; }
  .card-minmax .min { color: #4FC3F7; }
  .card-minmax .max { color: #FF8A65; }
  .card-minmax .min-time, .card-minmax .max-time { font-size: 9px; opacity: .45; margin: 0 2px 0 1px; }
  .time-display { font-size: 16px; font-weight: 600; letter-spacing: 1px; color: #80DEEA; margin-bottom: 6px; }
  .status {
    display: flex; align-items: center; justify-content: center;
    gap: 8px; font-size: 13px; opacity: .4;
  }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: #00E676;
         animation: pulse 2s infinite; }
  @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:.3} }
  .aq-1 { color: #00E676; }  .bg-1 { background: #00E676; }
  .aq-2 { color: #76FF03; }  .bg-2 { background: #76FF03; }
  .aq-3 { color: #FFD600; }  .bg-3 { background: #FFD600; }
  .aq-4 { color: #FF9100; }  .bg-4 { background: #FF9100; }
  .aq-5 { color: #FF1744; }  .bg-5 { background: #FF1744; }
</style>
</head>
<body>
<div class="dashboard">
  <div class="aqi-card">
    <div class="aqi-label">Air Quality Index</div>
    <div class="time-display" id="time">--:--:--</div>
    <div class="aqi-value" id="aqi-value">--</div>
    <div class="aqi-text"  id="aqi-text"></div>
    <div class="aqi-bar"><div class="aqi-fill" id="aqi-fill"></div></div>
  </div>
  <div class="grid">
    <div class="card">
      <div class="card-label">Temperature</div>
      <div class="card-value"><span id="temp">--</span><span class="card-unit">°C</span></div>
      <div class="card-minmax">↓<span class="min" id="temp-min">--</span> <span class="min-time" id="temp-min-t">--/-- --:--</span></div>
      <div class="card-minmax">↑<span class="max" id="temp-max">--</span> <span class="max-time" id="temp-max-t">--/-- --:--</span></div>
    </div>
    <div class="card">
      <div class="card-label">Humidity</div>
      <div class="card-value"><span id="hum">--</span><span class="card-unit">%</span></div>
      <div class="card-minmax">↓<span class="min" id="hum-min">--</span> <span class="min-time" id="hum-min-t">--/-- --:--</span></div>
      <div class="card-minmax">↑<span class="max" id="hum-max">--</span> <span class="max-time" id="hum-max-t">--/-- --:--</span></div>
    </div>
    <div class="card">
      <div class="card-label">eCO₂</div>
      <div class="card-value"><span id="eco2">--</span><span class="card-unit">ppm</span></div>
      <div class="card-minmax">↓<span class="min" id="eco2-min">--</span> <span class="min-time" id="eco2-min-t">--/-- --:--</span></div>
      <div class="card-minmax">↑<span class="max" id="eco2-max">--</span> <span class="max-time" id="eco2-max-t">--/-- --:--</span></div>
    </div>
    <div class="card">
      <div class="card-label">TVOC</div>
      <div class="card-value"><span id="tvoc">--</span><span class="card-unit">ppb</span></div>
      <div class="card-minmax">↓<span class="min" id="tvoc-min">--</span> <span class="min-time" id="tvoc-min-t">--/-- --:--</span></div>
      <div class="card-minmax">↑<span class="max" id="tvoc-max">--</span> <span class="max-time" id="tvoc-max-t">--/-- --:--</span></div>
    </div>
  </div>
  <div class="status">
    <span class="dot"></span><span id="status-text">Connected</span>
  </div>
</div>

<script>
  const AQI_NAMES = ["Excellent","Good","Moderate","Poor","Unhealthy"];

  function paint(d) {
    const v = d.aqi;
    if (v >= 1 && v <= 5) {
      document.getElementById('aqi-value').textContent = v;
      document.getElementById('aqi-value').className = 'aqi-value aq-' + v;
      document.getElementById('aqi-text').textContent = AQI_NAMES[v-1];
      document.getElementById('aqi-text').className = 'aqi-text aq-' + v;
      document.getElementById('aqi-fill').style.width = (v/5*100) + '%';
      document.getElementById('aqi-fill').className = 'aqi-fill bg-' + v;
    } else {
      document.getElementById('aqi-value').textContent = '--';
      document.getElementById('aqi-value').className = 'aqi-value';
      document.getElementById('aqi-text').textContent = '';
      document.getElementById('aqi-fill').style.width = '0%';
    }
    document.getElementById('time').textContent = d.time;
    document.getElementById('temp').textContent = d.temp;
    document.getElementById('temp-min').textContent = d.temp_min;
    document.getElementById('temp-max').textContent = d.temp_max;
    document.getElementById('temp-min-t').textContent = d.temp_min_time;
    document.getElementById('temp-max-t').textContent = d.temp_max_time;
    document.getElementById('hum').textContent  = d.hum;
    document.getElementById('hum-min').textContent = d.hum_min;
    document.getElementById('hum-max').textContent = d.hum_max;
    document.getElementById('hum-min-t').textContent = d.hum_min_time;
    document.getElementById('hum-max-t').textContent = d.hum_max_time;
    document.getElementById('eco2').textContent = d.eco2;
    document.getElementById('eco2-min').textContent = d.eco2_min;
    document.getElementById('eco2-max').textContent = d.eco2_max;
    document.getElementById('eco2-min-t').textContent = d.eco2_min_time;
    document.getElementById('eco2-max-t').textContent = d.eco2_max_time;
    document.getElementById('tvoc').textContent = d.tvoc;
    document.getElementById('tvoc-min').textContent = d.tvoc_min;
    document.getElementById('tvoc-max').textContent = d.tvoc_max;
    document.getElementById('tvoc-min-t').textContent = d.tvoc_min_time;
    document.getElementById('tvoc-max-t').textContent = d.tvoc_max_time;
    document.getElementById('status-text').textContent = 'Connected';
  }

  // Poll /data every 2 seconds — simple, reliable, no WebSocket needed
  async function poll() {
    try {
      const r = await fetch('/data');
      if (r.ok) {
        const txt = await r.text();
        let d;
        try { d = JSON.parse(txt); } catch(e) { console.error('JSON parse error:', txt); return; }
        paint(d);
      } else {
        console.warn('HTTP status:', r.status);
      }
    } catch(e) {
      console.warn('fetch error:', e);
    }
    setTimeout(poll, 1000);
  }
  poll();
</script>
</body>
</html>
)rawliteral";

// ═══════════════════════════════════════════════════════════════════
//  HTTP HANDLERS
// ═══════════════════════════════════════════════════════════════════

// Format a time_t value as "MM/DD HH:MM" string (or "--:--" if not synced)
String fmtTime(time_t t) {
  if (!data.time_synced || t == 0) return "--:--";
  struct tm *ti = localtime(&t);
  char buf[12];
  strftime(buf, sizeof(buf), "%m/%d %H:%M", ti);
  return String(buf);
}

void handleRoot() {
  // Force browser to always fetch fresh page (no cache)
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send_P(200, "text/html", index_html);
}

void handleData() {
  String json = "{\"aqi\":";
  json += String(data.aqi);
  json += ",\"eco2\":";
  json += String(data.eco2);
  json += ",\"eco2_min\":";
  json += String(data.eco2_min);
  json += ",\"eco2_max\":";
  json += String(data.eco2_max);
  json += ",\"eco2_min_time\":\"";
  json += fmtTime(data.eco2_min_time);
  json += "\",\"eco2_max_time\":\"";
  json += fmtTime(data.eco2_max_time);
  json += "\",\"tvoc\":";
  json += String(data.tvoc);
  json += ",\"tvoc_min\":";
  json += String(data.tvoc_min);
  json += ",\"tvoc_max\":";
  json += String(data.tvoc_max);
  json += ",\"tvoc_min_time\":\"";
  json += fmtTime(data.tvoc_min_time);
  json += "\",\"tvoc_max_time\":\"";
  json += fmtTime(data.tvoc_max_time);
  json += "\",\"temp\":\"";
  json += String(data.temperature, 1);
  json += "\",\"temp_min\":\"";
  json += String(data.temp_min, 1);
  json += "\",\"temp_max\":\"";
  json += String(data.temp_max, 1);
  json += "\",\"temp_min_time\":\"";
  json += fmtTime(data.temp_min_time);
  json += "\",\"temp_max_time\":\"";
  json += fmtTime(data.temp_max_time);
  json += "\",\"hum\":\"";
  json += String(data.humidity, 0);
  json += "\",\"hum_min\":\"";
  json += String(data.hum_min, 0);
  json += "\",\"hum_max\":\"";
  json += String(data.hum_max, 0);
  json += "\",\"hum_min_time\":\"";
  json += fmtTime(data.hum_min_time);
  json += "\",\"hum_max_time\":\"";
  json += fmtTime(data.hum_max_time);
  json += "\",\"time\":\"";
  json += String(data.time_str);
  json += "\"}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleIP() {
  server.send(200, "text/plain", WiFi.localIP().toString());
}

void handleNotFound() {
  server.send(404, "text/plain", "404");
}

// ═══════════════════════════════════════════════════════════════════
//  SENSOR HELPERS
// ═══════════════════════════════════════════════════════════════════

bool initAHT21() {
  if (!aht.begin()) return false;
  sensors_event_t hum, temp;
  aht.getEvent(&hum, &temp);
  return true;
}

bool initENS160() {
  if (!ens160.begin()) return false;
  // begin() already checks PART_ID
  // Set to standard (continuous) gas sensing mode
  ens160.setMode(ENS160_OPMODE_STD);
  // Warm-up: ~3 min to stabilise, readings self-correct
  return true;
}

bool readAHT21() {
  sensors_event_t hum_event, temp_event;
  if (!aht.getEvent(&hum_event, &temp_event)) return false;

  data.temperature = temp_event.temperature;
  data.humidity    = hum_event.relative_humidity;

  if (data.humidity < 0)   data.humidity = 0;
  if (data.humidity > 100) data.humidity = 100;

  // Update min/max with timestamps
  if (data.temperature < data.temp_min) {
    data.temp_min = data.temperature;
    data.temp_min_time = time(nullptr);
  }
  if (data.temperature > data.temp_max) {
    data.temp_max = data.temperature;
    data.temp_max_time = time(nullptr);
  }
  if (data.humidity < data.hum_min) {
    data.hum_min = data.humidity;
    data.hum_min_time = time(nullptr);
  }
  if (data.humidity > data.hum_max) {
    data.hum_max = data.humidity;
    data.hum_max_time = time(nullptr);
  }

  data.aht_ok = true;
  return true;
}

bool readENS160() {
  // Feed temperature / humidity compensation for best accuracy
  if (data.aht_ok) {
    ens160.set_envdata(data.temperature, data.humidity);
  }

  if (!ens160.measure(true)) return false;

  uint8_t aqi_val = ens160.getAQI();
  if (aqi_val == 0 || aqi_val > 5) return false;

  data.aqi       = aqi_val;
  data.tvoc      = ens160.getTVOC();
  data.eco2      = ens160.geteCO2();

  // Update min/max with timestamps
  if (data.eco2 < data.eco2_min) {
    data.eco2_min = data.eco2;
    data.eco2_min_time = time(nullptr);
  }
  if (data.eco2 > data.eco2_max) {
    data.eco2_max = data.eco2;
    data.eco2_max_time = time(nullptr);
  }
  if (data.tvoc < data.tvoc_min) {
    data.tvoc_min = data.tvoc;
    data.tvoc_min_time = time(nullptr);
  }
  if (data.tvoc > data.tvoc_max) {
    data.tvoc_max = data.tvoc;
    data.tvoc_max_time = time(nullptr);
  }

  data.ens160_ok = true;
  return true;
}

// ═══════════════════════════════════════════════════════════════════
//  DISPLAY
// ═══════════════════════════════════════════════════════════════════

static const char *AQI_LABEL[] = {"--","Excellent","Good","Moderate","Poor","Unhealthy"};

void updateDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Line 0: Time — centred, size 2
  display.setTextSize(2);
  display.setCursor((SCREEN_WIDTH - strlen(data.time_str) * 12) / 2, 0);
  display.print(data.time_str);

  // Line 1: AQI
  display.setTextSize(1);
  display.setCursor(0, 17);
  display.print("AQI: ");
  if (data.aqi >= 1 && data.aqi <= 5) {
    display.print(data.aqi);
    display.print(" ");
    display.print(AQI_LABEL[data.aqi]);
  } else {
    display.print("--");
  }

  // Line 2: eCO₂
  display.setCursor(0, 27);
  display.print("eCO2: ");
  if (data.ens160_ok) {
    display.print(data.eco2);
    display.print(" ppm");
  } else {
    display.print("--");
  }

  // Line 3: TVOC
  display.setCursor(0, 37);
  display.print("TVOC: ");
  if (data.ens160_ok) {
    display.print(data.tvoc);
    display.print(" ppb");
  } else {
    display.print("--");
  }

  // Line 4: Temperature & Humidity
  display.setCursor(0, 47);
  if (data.aht_ok) {
    display.printf("%.1fC %.0f%%RH", data.temperature, data.humidity);
  } else {
    display.print("AHT21 --");
  }

  display.display();
}

// ═══════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== ESP32-C3 Air Quality Monitor ===");

  // --- I²C bus ----------------------------------------------------
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  Serial.printf("I²C: SDA=%d  SCL=%d\n", I2C_SDA, I2C_SCL);

  // --- OLED -------------------------------------------------------
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("FAIL: SSD1306 not found");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("AQ Monitor");
    display.println("Booting...");
    display.display();
    Serial.println("OK:  SSD1306");
  }

  // --- AHT21 ------------------------------------------------------
  if (!initAHT21()) {
    Serial.println("FAIL: AHT21 not found");
    display.fillRect(0, 0, 128, 64, 0);
    display.setCursor(0, 0);
    display.println("AHT21 FAIL");
    display.display();
  } else {
    Serial.println("OK:  AHT21");
  }

  // --- ENS160 -----------------------------------------------------
  if (!initENS160()) {
    Serial.println("FAIL: ENS160 not found");
  } else {
    Serial.println("OK:  ENS160 (warming up...)");
  }

  // --- WiFi -------------------------------------------------------
  display.fillRect(0, 0, 128, 64, 0);
  display.setCursor(0, 0);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int dot = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    display.setCursor(0, 12);
    display.print("SSID: ");
    display.print(WIFI_SSID);
    display.setCursor(0, 24);
    display.print(dot % 4 == 0 ? "    " : dot % 4 == 1 ? ".   " :
                  dot % 4 == 2 ? "..  " : "... ");
    dot++;
    display.display();
    if (dot > 60) {
      display.fillRect(0, 0, 128, 64, 0);
      display.setCursor(0, 0);
      display.println("WiFi FAIL");
      display.println("Check credentials");
      display.display();
      Serial.println("\nFAIL: WiFi connection timeout");
      break;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("IP:  %s\nGW:  %s\nDNS: %s\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.dnsIP().toString().c_str());
    display.fillRect(0, 0, 128, 64, 0);
    display.setCursor(0, 0);
    display.print("IP ");
    display.print(WiFi.localIP());
    display.setCursor(0, 16);
    display.print("GW ");
    display.print(WiFi.gatewayIP());
    display.setCursor(0, 32);
    display.print("DNS ");
    display.print(WiFi.dnsIP());
    display.display();

    // --- NTP time sync (non-blocking: start SNTP, don't wait) -------
    Serial.println("NTP: started (background sync)");
    configTime(TZ_OFFSET_SEC, 0, NTP_SERVER1, NTP_SERVER2);
  }

  // --- Web server (built-in, no external libs) --------------------
  server.on("/",        handleRoot);
  server.on("/data",    handleData);
  server.on("/ip",      handleIP);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started");

  // --- Finish boot screen ------------------------------------------
  delay(1500);
  data.last_read_ms = millis();
}

// ═══════════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════════

void loop() {
  uint32_t now = millis();

  if (now - data.last_read_ms >= READ_INTERVAL_MS) {
    data.last_read_ms = now;

    // Refresh time string
    if (!data.time_synced) {
      if (time(nullptr) >= 1700000000) data.time_synced = true;
    }
    if (data.time_synced) {
      time_t t = time(nullptr);
      struct tm *ti = localtime(&t);
      strftime(data.time_str, sizeof(data.time_str), "%H:%M:%S", ti);
    }

    readAHT21();
    readENS160();

    updateDisplay();

    // Serial debug
    if (data.ens160_ok || data.aht_ok) {
      Serial.printf("[%s] AQI=%d  eCO₂=%d ppm  TVOC=%d ppb  "
                    "T=%.1f°C  RH=%.0f%%\n",
                    data.time_str, data.aqi, data.eco2, data.tvoc,
                    data.temperature, data.humidity);
    }
  }

  // Handle HTTP requests (built-in WebServer is non-blocking)
  server.handleClient();
}
