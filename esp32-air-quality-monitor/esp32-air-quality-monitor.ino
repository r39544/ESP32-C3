/*
 * ESP32-C3 Air Quality Monitor
 * ─────────────────────────────
 * Sensors:  ENS160 (VOC / eCO₂ / AQI)
 *            AHT21 (temperature / humidity)
 * Display:  ST7789 TFT 240×320 (SPI)
 * Network:  WiFi + built-in WebServer (NO external libraries needed)
 *
 * Wiring — sensors on I²C bus (share SDA/SCL):
 *
 *   ESP32-C3 | ENS160 | AHT21
 *   ─────────┼────────┼───────
 *   3V3      │ VIN    │ VIN
 *   GND      │ GND    │ GND
 *   GPIO 4   │ SDA    │ SDA
 *   GPIO 5   │ SCL    │ SCL
 *
 * Wiring — ST7789 TFT on SPI (board silkscreen: BL CS DC RST SDA SCL VCC GND):
 *   (note: on this module "SDA" = SPI MOSI/DIN, "SCL" = SPI SCLK/CLK)
 *
 *   ESP32-C3 | ST7789 TFT
 *   ─────────┼─────────
 *   3V3      │ VCC
 *   GND      │ GND
 *   GPIO 8   │ SCL  (SCLK/CLK)
 *   GPIO 6   │ SDA  (MOSI/DIN)
 *   GPIO 10  │ CS
 *   GPIO 7   │ DC
 *   GPIO 9   │ RST
 *   GPIO 3   │ BL   (or tie BL to 3V3 and set TFT_BL to -1)
 *
 *   I²C addresses:  ENS160 = 0x53,   AHT21 = 0x38
 *
 * ─── Board pin notes ────────────────
 *   Wiring above is for Seeed XIAO ESP32-C3.
 *   Other boards use different GPIOs — change TFT_* and I2C_* to match.
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_AHTX0.h>
#include <ScioSense_ENS160.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>    // captive-portal DNS for the AP config hotspot
#include <ESPmDNS.h>      // advertise <device>.local so scanners see a friendly name
#include <HTTPClient.h>         // ThingSpeak cloud upload
#include <WiFiClientSecure.h>   // HTTPS for ThingSpeak
#include <time.h>
#include <LittleFS.h>
#include <math.h>

// ═══════════════════════════════════════════════════════════════════
//  USER CONFIGURATION
// ═══════════════════════════════════════════════════════════════════

// --- I²C pins (match your board) -----------------------------------
#define I2C_SDA   4
#define I2C_SCL   5

// --- TFT (ST7789 2.0" 240×320, SPI) --------------------------------
#define TFT_SCLK  8
#define TFT_MOSI  6
#define TFT_CS   10
#define TFT_DC    7
#define TFT_RST   9
#define TFT_BL    3    // -1 if the module ties backlight to 3V3
#define TFT_W   240
#define TFT_H   320
#define TFT_SELFTEST  1  // 1 = flash red/green/blue at boot to verify SPI path

// --- Card layout (perfectly centred) --------------------------------
// 2×2 metric cards: 112px each + 2px gap = 226px → 7px side margins.
#define CARD_L   7        // left  column x
#define CARD_R   121      // right column x (= 7 + 112 + 2)
// AQI banner (first row): centred, 2px wider than the old 220px.
#define AQI_X    9
#define AQI_W    (TFT_W - 18)   // 222

// --- WiFi (set credentials in config.h) ----------------------------
#include "config.h"

// --- Sensor read intervals -------------------------------------------
#define READ_INTERVAL_MS  1000   // slow tasks (AHT21, time, display): every 1 s
#define ENS160_POLL_MS    200    // ENS160 non-blocking poll (~5×/s; new data ~1×/s)

// --- Data logging (LittleFS; requires "No OTA (2MB APP/2MB SPIFFS)" ---
// --- partition scheme in Arduino IDE) --------------------------------
#define LOG_DIR            "/log"
#define HISTORY_MAX_BINS   1200         // max curve points returned by /history

// --- Runtime settings ---------------------------------------------------
// Stored in LittleFS /cfg.txt (see loadSettings/saveSettings). The
// WIFI_SSID / WIFI_PASSWORD macros from config.h act as factory defaults.
#define CFG_FILE            "/cfg.txt"
#define CFG_AP_SSID         "AirQuality-Config"
#define NTP_SERVER_DEFAULT  "ntp2.aliyun.com"
#define NTP_SERVER_FALLBACK "ntp3.aliyun.com"
#define MAX_RETENTION_DAYS  60          // hard cap for the export file array

struct Settings {
  char     wifi_ssid[33];
  char     wifi_pass[65];
  char     device_name[33];            // DHCP/mDNS hostname (scanner-friendly)
  char     clear_pwd[17];              // password for /clear
  char     ntp_server[65];
  int32_t  tz_sec;                     // timezone offset, seconds
  uint16_t log_interval_s;             // seconds between log records
  uint16_t log_boot_grace_s;           // seconds after boot before logging starts
  uint16_t log_retention_days;         // ring-buffer retention (calendar days)
  bool     alert_enabled;
  uint16_t alert_eco2;                 // ppm
  uint16_t alert_tvoc;                 // ppb
  uint8_t  alert_aqi;                  // 1..5
  // ThingSpeak cloud upload
  bool     cloud_enabled;              // master switch
  char     ts_api_key[17];             // ThingSpeak write API key (16 chars)
  char     ts_channel[9];              // ThingSpeak channel number
  uint16_t cloud_interval_s;           // upload interval (>= 15 s)
};

Settings cfg;
bool cfg_mode = false;   // true when WiFi failed → AP config hotspot is running

void setDefaults();
void loadSettings();
void saveSettings();
void reinitNTP();

// ═══════════════════════════════════════════════════════════════════
//  GLOBALS
// ═══════════════════════════════════════════════════════════════════

// Display (ST7789 240×320 over SPI)
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// Sensors
Adafruit_AHTX0    aht;
ScioSense_ENS160  ens160(ENS160_I2CADDR_1);   // 0x53

// Web server (built-in — no external libraries needed)
WebServer server(80);
DNSServer dns;                       // captive-portal DNS (AP config mode only)

// Shared sensor snapshot (updated every READ_INTERVAL_MS)
struct {
  float    temperature = 0.0f;   // °C
  float    humidity    = 0.0f;   // % RH
  uint16_t tvoc        = 0;      // ppb
  uint16_t eco2        = 0;      // ppm
  uint8_t  aqi         = 0;      // 1–5 (0 = invalid)
  bool     aht_ok      = false;
  bool     ens160_ok   = false;
  uint32_t last_read_ms     = 0;  // slow task (1 s)
  uint32_t last_ens_poll_ms = 0;  // ENS160 non-blocking poll (~200 ms)
  uint32_t last_log_ms      = 0;  // data log record (every LOG_INTERVAL_S)
  uint32_t last_prune_ms    = 0;  // log retention cleanup (hourly)
  uint32_t boot_ms          = 0;  // millis() at boot (log warm-up grace)
  uint32_t last_cloud_ms    = 0;  // ThingSpeak upload (every cloud_interval_s)
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
  bool     display_on  = true;    // TFT on/off (web toggle; panel sleep + backlight)
  bool     alert       = false;   // threshold alert active
  char     alert_msg[40] = "";    // e.g. "eCO₂ 1250/1000 ppm"
  // Last-rendered display state (change detection removes the blink)
  bool     d_valid   = false;     // false → force a full render next time
  char     d_date[16] = "";
  int      d_aqi     = -1;
  char     d_aqi_lbl[16] = "";
  char     d_e2[12], d_e2n[12], d_e2x[12];   // eco2 value/min/max
  char     d_tv[12], d_tvn[12], d_tvx[12];   // tvoc
  char     d_tp[12], d_tpn[12], d_tpx[12];   // temperature
  char     d_hm[12], d_hmn[12], d_hmx[12];   // humidity
  int      d_pct       = -1;                 // signal %
  int      d_wifi_state = -1;                // 0 off / 1 connected / 2 cfg
  char     d_ssid[33]  = "";
  char     d_ip[16]    = "";
} data;

// TFT on/off control — implemented in the DISPLAY section below.
void setDisplay(bool on);

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

  /* Clickable data cards (open history chart) */
  .card.clickable { cursor: pointer; transition: transform .15s ease, box-shadow .15s ease; }
  .card.clickable:active { transform: scale(.97); }
  .card.clickable:hover { box-shadow: 0 6px 24px rgba(0,0,0,.5); }
  .card.clickable:hover .card-label { opacity: .8; }

  /* History modal */
  .modal-overlay {
    position: fixed; inset: 0; background: rgba(4,4,12,.74);
    display: none; align-items: center; justify-content: center;
    padding: 16px; z-index: 50;
  }
  .modal-overlay.open { display: flex; }
  .modal {
    width: 100%; max-width: 520px; background: #14142b; border-radius: 24px;
    padding: 20px; box-shadow: 0 12px 40px rgba(0,0,0,.6);
    display: flex; flex-direction: column; gap: 12px;
  }
  .modal-head { display: flex; align-items: center; justify-content: space-between; }
  .modal-title { font-size: 18px; font-weight: 700; }
  .modal-close { background: none; border: none; color: #e0e0e0; font-size: 24px;
                 line-height: 1; cursor: pointer; padding: 4px 10px; border-radius: 8px; }
  .modal-close:hover { background: rgba(255,255,255,.08); }
  .range-btns { display: flex; gap: 8px; }
  .range-btn {
    background: rgba(255,255,255,.06); border: 1px solid rgba(255,255,255,.08);
    color: #e0e0e0; border-radius: 999px; padding: 6px 14px; font-size: 13px; cursor: pointer;
  }
  .range-btn.active { background: rgba(255,255,255,.16); border-color: rgba(255,255,255,.35); }
  .chart-stats { display: flex; gap: 14px; font-size: 12px; opacity: .75; flex-wrap: wrap; }
  .chart-stats b { font-size: 15px; }
  .chart-wrap { position: relative; width: 100%; height: 260px; }
  .chart-wrap canvas { width: 100%; height: 100%; display: block; }
  .chart-tip {
    position: absolute; display: none; pointer-events: none;
    background: rgba(0,0,0,.82); border: 1px solid rgba(255,255,255,.12);
    border-radius: 8px; padding: 6px 10px; font-size: 12px; white-space: nowrap;
  }
  .export-btn {
    display: inline-flex; align-items: center; justify-content: center; gap: 6px;
    background: rgba(255,255,255,.06); border: 1px solid rgba(255,255,255,.1);
    color: #e0e0e0; border-radius: 999px; padding: 8px 16px; font-size: 13px;
    text-decoration: none; align-self: center; transition: background .15s ease;
    cursor: pointer;
  }
  .export-btn:hover { background: rgba(255,255,255,.12); }
  .export-sm { padding: 6px 12px; font-size: 12px; margin-left: auto; }
  .export-btn.danger { border-color: rgba(255,82,82,.45); color: #ff8a8a; }
  .export-btn.danger:hover { background: rgba(255,82,82,.12); }
  .action-row { display: flex; justify-content: center; gap: 12px; }
  .alert-banner {
    display: none; align-items: center; justify-content: center; gap: 8px;
    background: rgba(255,23,68,.12); border: 1px solid rgba(255,23,68,.45);
    color: #ff8a8a; border-radius: 14px; padding: 10px 14px;
    font-size: 14px; font-weight: 600; text-align: center;
  }
  .alert-banner.show { display: flex; }
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
    <div class="card clickable" onclick="openHistory('temp')">
      <div class="card-label">Temperature</div>
      <div class="card-value"><span id="temp">--</span><span class="card-unit">°C</span></div>
      <div class="card-minmax">↓<span class="min" id="temp-min">--</span> <span class="min-time" id="temp-min-t">--/-- --:--</span></div>
      <div class="card-minmax">↑<span class="max" id="temp-max">--</span> <span class="max-time" id="temp-max-t">--/-- --:--</span></div>
    </div>
    <div class="card clickable" onclick="openHistory('hum')">
      <div class="card-label">Humidity</div>
      <div class="card-value"><span id="hum">--</span><span class="card-unit">%</span></div>
      <div class="card-minmax">↓<span class="min" id="hum-min">--</span> <span class="min-time" id="hum-min-t">--/-- --:--</span></div>
      <div class="card-minmax">↑<span class="max" id="hum-max">--</span> <span class="max-time" id="hum-max-t">--/-- --:--</span></div>
    </div>
    <div class="card clickable" onclick="openHistory('eco2')">
      <div class="card-label">eCO₂</div>
      <div class="card-value"><span id="eco2">--</span><span class="card-unit">ppm</span></div>
      <div class="card-minmax">↓<span class="min" id="eco2-min">--</span> <span class="min-time" id="eco2-min-t">--/-- --:--</span></div>
      <div class="card-minmax">↑<span class="max" id="eco2-max">--</span> <span class="max-time" id="eco2-max-t">--/-- --:--</span></div>
    </div>
    <div class="card clickable" onclick="openHistory('tvoc')">
      <div class="card-label">TVOC</div>
      <div class="card-value"><span id="tvoc">--</span><span class="card-unit">ppb</span></div>
      <div class="card-minmax">↓<span class="min" id="tvoc-min">--</span> <span class="min-time" id="tvoc-min-t">--/-- --:--</span></div>
      <div class="card-minmax">↑<span class="max" id="tvoc-max">--</span> <span class="max-time" id="tvoc-max-t">--/-- --:--</span></div>
    </div>
  </div>

  <!-- Threshold alert banner (hidden unless active) -->
  <div class="alert-banner" id="alert-banner"><span>⚠</span><span id="alert-text"></span></div>

  <!-- History chart modal -->
  <div class="modal-overlay" id="modal" onclick="if(event.target===this)closeHistory()">
    <div class="modal">
      <div class="modal-head">
        <div class="modal-title" id="modal-title">History</div>
        <button class="modal-close" onclick="closeHistory()">×</button>
      </div>
      <div class="range-btns">
        <button class="range-btn active" onclick="setRange(1)">24H</button>
        <button class="range-btn" onclick="setRange(7)">7D</button>
        <button class="range-btn" onclick="setRange(30)">30D</button>
        <a class="export-btn export-sm" id="export-range" href="/export?days=1" download="aq_log.csv">⬇ 导出该区间</a>
      </div>
      <div class="chart-stats" id="chart-stats"></div>
      <div class="chart-wrap">
        <canvas id="chart-canvas"></canvas>
        <div class="chart-tip" id="chart-tip"></div>
      </div>
    </div>
  </div>
  <div class="status">
    <span class="dot"></span><span id="status-text">Connected</span>
  </div>
  <div class="action-row">
    <a class="export-btn" href="/export" download="aq_log.csv">⬇ Export CSV</a>
    <button class="export-btn" id="display-btn" onclick="toggleDisplay()">⏻ Turn off screen
</button>
    <button class="export-btn danger" onclick="clearHistory()">🗑 Clear History
</button>
    <a class="export-btn" href="/config">⚙ Config</a>
  </div>
</div>

<script>
  const AQI_NAMES = ["Excellent","Good","Moderate","Poor","Unhealthy"];
  // Hide sentinel min/max values (initial or after clear) instead of "999"/"65535".
  const minTxt = (v, hi) => (v >= hi) ? '--' : v;
  const maxTxt = (v, lo) => (v <= lo) ? '--' : v;

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
    document.getElementById('temp-min').textContent = minTxt(d.temp_min, 990);
    document.getElementById('temp-max').textContent = maxTxt(d.temp_max, -990);
    document.getElementById('temp-min-t').textContent = d.temp_min_time;
    document.getElementById('temp-max-t').textContent = d.temp_max_time;
    document.getElementById('hum').textContent  = d.hum;
    document.getElementById('hum-min').textContent = minTxt(d.hum_min, 990);
    document.getElementById('hum-max').textContent = maxTxt(d.hum_max, -990);
    document.getElementById('hum-min-t').textContent = d.hum_min_time;
    document.getElementById('hum-max-t').textContent = d.hum_max_time;
    document.getElementById('eco2').textContent = d.eco2;
    document.getElementById('eco2-min').textContent = minTxt(d.eco2_min, 50000);
    document.getElementById('eco2-max').textContent = maxTxt(d.eco2_max, 0);
    document.getElementById('eco2-min-t').textContent = d.eco2_min_time;
    document.getElementById('eco2-max-t').textContent = d.eco2_max_time;
    document.getElementById('tvoc').textContent = d.tvoc;
    document.getElementById('tvoc-min').textContent = minTxt(d.tvoc_min, 50000);
    document.getElementById('tvoc-max').textContent = maxTxt(d.tvoc_max, 0);
    document.getElementById('tvoc-min-t').textContent = d.tvoc_min_time;
    document.getElementById('tvoc-max-t').textContent = d.tvoc_max_time;
    document.getElementById('status-text').textContent = 'Connected';

    // Reflect TFT on/off on the display toggle button.
    displayState = (d.display === 1);
    updateDisplayBtn();

    // Threshold alert banner.
    const ab = document.getElementById('alert-banner');
    if (d.alert) {
      ab.classList.add('show');
      document.getElementById('alert-text').textContent = '告警: ' + (d.alert_msg || '超出阈值');
    } else {
      ab.classList.remove('show');
    }
  }

  // ── TFT display on/off (web toggle) ──────────────────────────────
  function updateDisplayBtn() {
    const btn = document.getElementById('display-btn');
    if (!btn) return;
    if (displayState) {
      btn.textContent = '⏻ Turn off screen';
      btn.classList.remove('danger');
    } else {
      btn.textContent = '⏻ Turn on screen';
      btn.classList.add('danger');
    }
  }
  async function toggleDisplay() {
    const want = displayState ? 'off' : 'on';
    try {
      const r = await fetch('/display?state=' + want, { method: 'POST' });
      const d = await r.json();
      if (d.ok) { displayState = d.display === 'on'; updateDisplayBtn(); }
    } catch (e) { console.warn('display error:', e); }
  }

  // Poll /data every 1 second — simple, reliable, no WebSocket needed
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

  // ── History charts (hand-rolled <canvas>, no external libs) ─────────
  const METRICS = {
    temp: { label: 'Temperature', unit: '°C',  color: '#FF8A65', digits: 1 },
    hum:  { label: 'Humidity',    unit: '%',   color: '#4FC3F7', digits: 1 },
    eco2: { label: 'eCO₂',        unit: 'ppm', color: '#FFD600', digits: 0 },
    tvoc: { label: 'TVOC',        unit: 'ppb', color: '#00E676', digits: 0 },
  };
  const RANGES = [1, 7, 30];
  const PAD = { l: 44, r: 12, t: 12, b: 22 };

  let chartMetric = 'temp';
  let chartDays   = 1;
  let chartData   = null;
  let displayState = true;   // TFT on/off, synced from /data

  const modal  = document.getElementById('modal');
  const canvas = document.getElementById('chart-canvas');
  const cctx   = canvas.getContext('2d');
  const tip    = document.getElementById('chart-tip');

  function openHistory(m) {
    chartMetric = m;
    chartDays   = 1;
    document.getElementById('modal-title').textContent = METRICS[m].label + ' · History';
    document.querySelectorAll('.range-btn').forEach((b, i) => b.classList.toggle('active', i === 0));
    document.getElementById('export-range').href = '/export?days=' + chartDays;
    modal.classList.add('open');
    loadHistory();
  }
  function closeHistory() { modal.classList.remove('open'); }
  function setRange(days) {
    chartDays = days;
    document.querySelectorAll('.range-btn').forEach((b, i) =>
      b.classList.toggle('active', RANGES[i] === days));
    document.getElementById('export-range').href = '/export?days=' + chartDays;
    loadHistory();
  }

  async function loadHistory() {
    try {
      const r = await fetch('/history?metric=' + chartMetric + '&days=' + chartDays);
      if (!r.ok) throw new Error('HTTP ' + r.status);
      chartData = await r.json();
      renderChart();
    } catch (e) { console.warn('history error:', e); }
  }

  const pad2 = n => String(n).padStart(2, '0');
  function fmtT(t, mode) {
    const d = new Date(t * 1000);
    return mode === '24h'
      ? pad2(d.getHours()) + ':' + pad2(d.getMinutes())
      : pad2(d.getMonth() + 1) + '/' + pad2(d.getDate()) + ' ' + pad2(d.getHours()) + ':' + pad2(d.getMinutes());
  }

  // Light centered moving average (±2 neighbours) so short-term sensor
  // noise doesn't make the curve jagged. Neighbours are only averaged when
  // they are close in time, so real data gaps are never bridged.
  function smoothPts(pts) {
    const n = pts.length;
    if (n < 3) return pts.map(p => p.slice());
    const spac = [];
    for (let i = 1; i < n; i++) spac.push(pts[i][0] - pts[i - 1][0]);
    spac.sort((a, b) => a - b);
    const med = spac[Math.floor(spac.length / 2)] || 60;
    const maxSpan = Math.max(med * 5, 120);
    const out = pts.map(p => p.slice());
    for (let i = 0; i < n; i++) {
      let s = 0, c = 0;
      for (let j = Math.max(0, i - 2); j <= Math.min(n - 1, i + 2); j++) {
        if (Math.abs(pts[j][0] - pts[i][0]) <= maxSpan) { s += pts[j][1]; c++; }
      }
      if (c) out[i][1] = s / c;
    }
    return out;
  }

  function getScale(pts, pad) {
    const W = canvas.clientWidth, H = canvas.clientHeight;
    const pw = W - pad.l - pad.r, ph = H - pad.t - pad.b;
    let lo = Infinity, hi = -Infinity;
    for (const p of pts) { if (p[1] < lo) lo = p[1]; if (p[1] > hi) hi = p[1]; }
    if (lo === hi) { lo -= 1; hi += 1; }
    const span = hi - lo; lo -= span * 0.08; hi += span * 0.08;
    const t0 = pts[0][0], t1 = pts[pts.length - 1][0];
    return {
      W, H, pw, ph, lo, hi,
      X: t => pad.l + (t - t0) / ((t1 - t0) || 1) * pw,
      Y: v => pad.t + (hi - v) / (hi - lo) * ph,
    };
  }

  function drawBase(M, pts) {
    const dpr = window.devicePixelRatio || 1;
    canvas.width  = Math.round(canvas.clientWidth * dpr);
    canvas.height = Math.round(canvas.clientHeight * dpr);
    cctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    cctx.clearRect(0, 0, canvas.clientWidth, canvas.clientHeight);

    const sc = getScale(pts, PAD);
    const mode = chartDays === 1 ? '24h' : 'multi';

    cctx.font = '10px sans-serif';
    cctx.textAlign = 'right'; cctx.textBaseline = 'middle';
    for (let i = 0; i <= 4; i++) {
      const v = sc.hi - (sc.hi - sc.lo) * i / 4, y = sc.Y(v);
      cctx.strokeStyle = 'rgba(255,255,255,.07)';
      cctx.beginPath(); cctx.moveTo(PAD.l, y); cctx.lineTo(PAD.l + sc.pw, y); cctx.stroke();
      cctx.fillStyle = 'rgba(255,255,255,.45)';
      cctx.fillText(v.toFixed(M.digits), PAD.l - 6, y);
    }
    cctx.textAlign = 'center'; cctx.textBaseline = 'top';
    cctx.fillStyle = 'rgba(255,255,255,.45)';
    for (let i = 0; i <= 4; i++) {
      const t = sc.t0 + (sc.t1 - sc.t0) * i / 4;
      cctx.fillText(fmtT(Math.round(t), mode), sc.X(t), PAD.t + sc.ph + 6);
    }

    // Smooth curve through the points (Catmull-Rom spline as cubic Bézier).
    cctx.lineJoin = 'round'; cctx.lineCap = 'round';
    cctx.strokeStyle = M.color; cctx.lineWidth = 2;
    cctx.beginPath();
    cctx.moveTo(sc.X(pts[0][0]), sc.Y(pts[0][1]));
    for (let i = 0; i < pts.length - 1; i++) {
      const p0 = pts[Math.max(0, i - 1)];
      const p1 = pts[i];
      const p2 = pts[i + 1];
      const p3 = pts[Math.min(pts.length - 1, i + 2)];
      cctx.bezierCurveTo(
        sc.X(p1[0]) + (sc.X(p2[0]) - sc.X(p0[0])) / 6,
        sc.Y(p1[1]) + (sc.Y(p2[1]) - sc.Y(p0[1])) / 6,
        sc.X(p2[0]) - (sc.X(p3[0]) - sc.X(p1[0])) / 6,
        sc.Y(p2[1]) - (sc.Y(p3[1]) - sc.Y(p1[1])) / 6,
        sc.X(p2[0]), sc.Y(p2[1]));
    }
    cctx.stroke();
    // Area fill follows the same curve down to the baseline.
    cctx.lineTo(sc.X(pts[pts.length - 1][0]), sc.Y(sc.lo));
    cctx.lineTo(sc.X(pts[0][0]), sc.Y(sc.lo));
    cctx.closePath();
    cctx.fillStyle = M.color; cctx.globalAlpha = .12; cctx.fill(); cctx.globalAlpha = 1;
  }

  function renderChart() {
    const M = METRICS[chartMetric];
    const raw = (chartData && chartData.points) ? chartData.points : [];
    const pts = raw.length ? smoothPts(raw) : [];   // smoothed for display
    const stats = document.getElementById('chart-stats');
    if (!pts.length) {
      cctx.clearRect(0, 0, canvas.clientWidth, canvas.clientHeight);
      stats.textContent = '暂无数据 —— 等待下一次每分钟记录';
      return;
    }
    stats.innerHTML = '';
    const addStat = (l, v) => { const s = document.createElement('span'); s.innerHTML = l + ' <b>' + v + '</b>'; stats.appendChild(s); };
    addStat('min ', chartData.min);
    addStat('avg ', chartData.avg);
    addStat('max ', chartData.max);
    addStat('N ', chartData.count);

    drawBase(M, pts);
    canvas.onmousemove = e => hover(e, M, pts);
    canvas.onmouseleave = () => { tip.style.display = 'none'; if (pts.length) drawBase(M, pts); };
  }

  function hover(e, M, pts) {
    const sc = getScale(pts, PAD);
    const mode = chartDays === 1 ? '24h' : 'multi';
    const rect = canvas.getBoundingClientRect();
    const mx = e.clientX - rect.left;
    let best = 0, bd = 1e9;
    for (let i = 0; i < pts.length; i++) { const d = Math.abs(sc.X(pts[i][0]) - mx); if (d < bd) { bd = d; best = i; } }
    drawBase(M, pts);
    const p = pts[best], tx = sc.X(p[0]);
    cctx.strokeStyle = 'rgba(255,255,255,.25)';
    cctx.beginPath(); cctx.moveTo(tx, sc.Y(sc.lo)); cctx.lineTo(tx, sc.Y(sc.hi)); cctx.stroke();
    cctx.fillStyle = M.color;
    cctx.beginPath(); cctx.arc(tx, sc.Y(p[1]), 3, 0, 6.2832); cctx.fill();
    tip.style.display = 'block';
    tip.style.left = Math.min(Math.max(tx + 12, 0), sc.W - 90) + 'px';
    tip.style.top = Math.max(sc.Y(p[1]) - 24, 0) + 'px';
    tip.textContent = fmtT(p[0], mode) + '  ' + p[1].toFixed(M.digits) + ' ' + M.unit;
  }

  window.addEventListener('resize', () => { if (modal.classList.contains('open')) renderChart(); });
  document.addEventListener('keydown', e => { if (e.key === 'Escape') closeHistory(); });

  function clearHistory() {
    const pwd = prompt('输入密码以清除全部历史记录(此操作不可恢复):');
    if (pwd === null) return;
    fetch('/clear?pwd=' + encodeURIComponent(pwd), { method: 'POST' })
      .then(r => r.json())
      .then(d => {
        if (d.ok) {
          alert('已清除 ' + d.deleted + ' 个历史数据文件');
          location.reload();
        } else {
          alert('密码错误,未清除');
        }
      })
      .catch(() => alert('清除失败'));
  }
</script>
</body>
</html>
)rawliteral";

// Configuration page (dark theme, same look as the dashboard).
const char config_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<title>设置 · Air Quality Monitor</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
         background: #0f0f1a; color: #e0e0e0; min-height: 100vh; padding: 24px 16px; }
  .wrap { max-width: 480px; margin: 0 auto; display: flex; flex-direction: column; gap: 16px; }
  h1 { font-size: 20px; font-weight: 700; text-align: center; }
  .card { background: #1a1a2e; border-radius: 20px; padding: 20px; box-shadow: 0 4px 20px rgba(0,0,0,.3); }
  .sec { font-size: 12px; text-transform: uppercase; letter-spacing: 2px; opacity: .5; margin: 16px 0 10px; }
  .sec:first-child { margin-top: 0; }
  .row { display: flex; flex-direction: column; gap: 4px; margin-bottom: 14px; }
  .row label { font-size: 13px; opacity: .75; }
  .row input { background: #14142b; border: 1px solid #2c2c4a; color: #e0e0e0;
               border-radius: 10px; padding: 10px 12px; font-size: 15px; outline: none; }
  .row input:focus { border-color: #4FC3F7; }
  .row .hint { font-size: 11px; opacity: .45; }
  .chk { flex-direction: row; align-items: center; gap: 10px; }
  .chk input { width: 20px; height: 20px; accent-color: #4FC3F7; }
  .btn { background: #4FC3F7; color: #0a0a14; border: none; border-radius: 999px;
         padding: 14px; font-size: 16px; font-weight: 700; cursor: pointer; width: 100%; }
  .btn:active { transform: scale(.98); }
  #msg { text-align: center; font-size: 14px; min-height: 20px; }
  #msg.ok { color: #00E676; } #msg.err { color: #ff8a8a; }
  .back { text-align: center; font-size: 13px; opacity: .5; text-decoration: none; color: #80DEEA; }
</style>
</head>
<body>
<div class="wrap">
  <h1>⚙ 设备设置</h1>
  <form class="card" id="cfg">
    <div class="sec">WiFi</div>
    <div class="row"><label>SSID</label><input name="wifi_ssid" id="wifi_ssid" autocomplete="off" required></div>
    <div class="row"><label>密码</label><input name="wifi_pass" id="wifi_pass" type="password" autocomplete="off" placeholder="留空则保持当前密码">
      <span class="hint">留空 = 不修改当前 WiFi 密码</span></div>
    <div class="sec">系统</div>
    <div class="row"><label>设备名称</label><input name="device_name" id="device_name" maxlength="32">
      <span class="hint">字母/数字/中划线;用于 IP 扫描工具、路由器设备列表、mDNS(&lt;名称&gt;.local)</span></div>
    <div class="row"><label>清除历史密码</label><input name="clear_pwd" id="clear_pwd" autocomplete="off"></div>
    <div class="row"><label>NTP 服务器</label><input name="ntp_server" id="ntp_server"></div>
    <div class="row"><label>时区(UTC 偏移,小时)</label><input name="tz_hours" id="tz_hours" type="number" step="0.5" min="-14" max="14"></div>
    <div class="row"><label>日志记录间隔(秒)</label><input name="log_interval_s" id="log_interval_s" type="number" min="5" max="3600"></div>
    <div class="row"><label>启动后延迟记录(秒)</label><input name="log_boot_grace_s" id="log_boot_grace_s" type="number" min="0" max="3600">
      <span class="hint">开机后等待 N 秒再开始写日志,跳过传感器预热(ENS160 完全稳定约 180 秒)</span></div>
    <div class="row"><label>日志保留天数</label><input name="log_retention_days" id="log_retention_days" type="number" min="1" max="60"></div>
    <div class="sec">告警阈值</div>
    <div class="row chk"><label>启用告警</label><input name="alert_enabled" id="alert_enabled" type="checkbox"></div>
    <div class="row"><label>eCO₂ 阈值(ppm)</label><input name="alert_eco2" id="alert_eco2" type="number" min="0"></div>
    <div class="row"><label>TVOC 阈值(ppb)</label><input name="alert_tvoc" id="alert_tvoc" type="number" min="0"></div>
    <div class="row"><label>AQI 阈值(1-5)</label><input name="alert_aqi" id="alert_aqi" type="number" min="1" max="5"></div>
    <div class="sec">云上报 (ThingSpeak)</div>
    <div class="row chk"><label>启用云上报</label><input name="cloud_enabled" id="cloud_enabled" type="checkbox"></div>
    <div class="row"><label>Write API Key</label><input name="ts_api_key" id="ts_api_key" maxlength="16" autocomplete="off">
      <span class="hint">ThingSpeak 通道页的 Write API Key(16 位)</span></div>
    <div class="row"><label>通道号</label><input name="ts_channel" id="ts_channel" maxlength="8" autocomplete="off"></div>
    <div class="row"><label>上报间隔(秒)</label><input name="cloud_interval_s" id="cloud_interval_s" type="number" min="15" max="3600">
      <span class="hint">免费版最少 15 秒一次;需 WiFi 在线、传感器有效且过启动宽限后才上报</span></div>
    <button class="btn" type="submit">保存</button>
  </form>
  <div id="msg"></div>
  <a class="back" href="/">← 返回仪表盘</a>
</div>
<script>
  const msgEl = document.getElementById('msg');
  function msg(t, c) { msgEl.textContent = t; msgEl.className = c || ''; }

  async function loadCfg() {
    try {
      const d = await (await fetch('/config/data')).json();
      document.getElementById('wifi_ssid').value = d.wifi_ssid || '';
      document.getElementById('device_name').value = d.device_name || '';
      document.getElementById('clear_pwd').value = d.clear_pwd || '';
      document.getElementById('ntp_server').value = d.ntp_server || '';
      document.getElementById('tz_hours').value = d.tz_hours;
      document.getElementById('log_interval_s').value = d.log_interval_s;
      document.getElementById('log_boot_grace_s').value = d.log_boot_grace_s;
      document.getElementById('log_retention_days').value = d.log_retention_days;
      document.getElementById('alert_enabled').checked = !!d.alert_enabled;
      document.getElementById('alert_eco2').value = d.alert_eco2;
      document.getElementById('alert_tvoc').value = d.alert_tvoc;
      document.getElementById('alert_aqi').value = d.alert_aqi;
      document.getElementById('cloud_enabled').checked = !!d.cloud_enabled;
      document.getElementById('ts_api_key').value = d.ts_api_key || '';
      document.getElementById('ts_channel').value = d.ts_channel || '';
      document.getElementById('cloud_interval_s').value = d.cloud_interval_s;
    } catch (e) { msg('加载配置失败: ' + e, 'err'); }
  }

  document.getElementById('cfg').addEventListener('submit', async e => {
    e.preventDefault();
    // URL-encoded body (the ESP32 WebServer only parses this into args).
    const params = new URLSearchParams(new FormData(e.target));
    if (document.getElementById('wifi_pass').value.trim() === '') params.delete('wifi_pass');
    try {
      const r = await fetch('/config/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: params.toString(),
      });
      const d = await r.json();
      if (d.ok) {
        msg(d.reboot ? '已保存,设备正在重启…' : '已保存', 'ok');
      } else {
        msg(d.msg || '保存失败', 'err');
      }
    } catch (e) { msg('保存失败: ' + e, 'err'); }
  });

  loadCfg();
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
  // In AP config mode, the root path serves the settings page so a phone
  // that connects to the hotspot (or its captive-portal probe) lands
  // straight on the reconfiguration form.
  server.send_P(200, "text/html", cfg_mode ? config_html : index_html);
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
  json += "\",\"display\":";
  json += data.display_on ? "1" : "0";
  json += ",\"alert\":";
  json += data.alert ? "1" : "0";
  json += ",\"alert_msg\":\"";
  json += String(data.alert_msg);
  json += "\",\"time\":\"";
  json += String(data.time_str);
  json += "\"}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleIP() {
  if (cfg_mode) server.send(200, "text/plain", WiFi.softAPIP().toString());
  else          server.send(200, "text/plain", WiFi.localIP().toString());
}

void handleNotFound() {
  // Unknown paths → redirect. In AP config mode this makes OS captive-
  // portal probes (connectivitycheck / generate_204 …) land on the
  // settings page instead of a dead 404.
  server.sendHeader("Location", cfg_mode ? "/config" : "/");
  server.send(302, "text/plain", "");
}

// POST /display?state=on|off  — turn the TFT panel + backlight on/off
// from the web dashboard (the BOOT button is unusable: it shares GPIO9
// with TFT_RST on this board).
void handleDisplay() {
  if (!server.hasArg("state")) {
    server.send(400, "application/json; charset=utf-8",
                "{\"ok\":false,\"msg\":\"missing state\"}");
    return;
  }
  setDisplay(server.arg("state") == "on");
  String json = "{\"ok\":true,\"display\":\"";
  json += String(data.display_on ? "on" : "off");
  json += "\"}";
  server.send(200, "application/json; charset=utf-8", json);
}

// GET /config — configuration form page.
void handleConfigPage() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.send_P(200, "text/html", config_html);
}

// GET /config/data — current settings as JSON (prefills the form).
// NOTE: wifi_pass is intentionally never echoed back.
void handleConfigData() {
  String json;
  json.reserve(512);
  json = "{\"wifi_ssid\":\"";
  json += String(cfg.wifi_ssid);
  json += "\",\"device_name\":\"";
  json += String(cfg.device_name);
  json += "\",\"clear_pwd\":\"";
  json += String(cfg.clear_pwd);
  json += "\",\"ntp_server\":\"";
  json += String(cfg.ntp_server);
  json += "\",\"tz_hours\":";
  json += String((double)cfg.tz_sec / 3600.0, 1);
  json += ",\"log_interval_s\":";
  json += String(cfg.log_interval_s);
  json += ",\"log_boot_grace_s\":";
  json += String(cfg.log_boot_grace_s);
  json += ",\"log_retention_days\":";
  json += String(cfg.log_retention_days);
  json += ",\"alert_enabled\":";
  json += cfg.alert_enabled ? "true" : "false";
  json += ",\"alert_eco2\":";
  json += String(cfg.alert_eco2);
  json += ",\"alert_tvoc\":";
  json += String(cfg.alert_tvoc);
  json += ",\"alert_aqi\":";
  json += String(cfg.alert_aqi);
  json += ",\"cloud_enabled\":";
  json += cfg.cloud_enabled ? "true" : "false";
  json += ",\"ts_api_key\":\"";
  json += String(cfg.ts_api_key);
  json += "\",\"ts_channel\":\"";
  json += String(cfg.ts_channel);
  json += "\",\"cloud_interval_s\":";
  json += String(cfg.cloud_interval_s);
  json += "}";
  server.send(200, "application/json; charset=utf-8", json);
}

// POST /config/save — form-urlencoded body. An empty wifi_pass keeps the
// current password. WiFi changes trigger a reboot; all other parameters
// (NTP/timezone/log/alert) apply immediately.
void handleConfigSave() {
  bool wifi_changed = false;
  bool name_changed = false;

  if (server.hasArg("wifi_ssid")) {
    String s = server.arg("wifi_ssid");
    s.trim();
    if (s.length() == 0) {
      server.send(400, "application/json; charset=utf-8",
                  "{\"ok\":false,\"msg\":\"SSID 不能为空\"}");
      return;
    }
    if (s != String(cfg.wifi_ssid)) {
      strncpy(cfg.wifi_ssid, s.c_str(), sizeof(cfg.wifi_ssid) - 1);
      cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
      wifi_changed = true;
    }
  }
  if (server.hasArg("wifi_pass") && server.arg("wifi_pass").length() > 0) {
    String s = server.arg("wifi_pass");
    if (s != String(cfg.wifi_pass)) {
      strncpy(cfg.wifi_pass, s.c_str(), sizeof(cfg.wifi_pass) - 1);
      cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = '\0';
      wifi_changed = true;
    }
  }
  if (server.hasArg("device_name")) {
    String s = server.arg("device_name");
    s.trim();
    if (s.length() > 0 && validHostname(s.c_str()) && s != String(cfg.device_name)) {
      strncpy(cfg.device_name, s.c_str(), sizeof(cfg.device_name) - 1);
      cfg.device_name[sizeof(cfg.device_name) - 1] = '\0';
      name_changed = true;
    }
  }
  if (server.hasArg("clear_pwd")) {
    String s = server.arg("clear_pwd");
    s.trim();
    if (s.length() > 0) {
      strncpy(cfg.clear_pwd, s.c_str(), sizeof(cfg.clear_pwd) - 1);
      cfg.clear_pwd[sizeof(cfg.clear_pwd) - 1] = '\0';
    }
  }
  if (server.hasArg("ntp_server")) {
    String s = server.arg("ntp_server");
    s.trim();
    if (s.length() > 0) {
      strncpy(cfg.ntp_server, s.c_str(), sizeof(cfg.ntp_server) - 1);
      cfg.ntp_server[sizeof(cfg.ntp_server) - 1] = '\0';
    }
  }
  if (server.hasArg("tz_hours")) {
    float h = server.arg("tz_hours").toFloat();
    if (h >= -14.0f && h <= 14.0f) cfg.tz_sec = (int32_t)roundf(h * 3600.0f);
  }
  if (server.hasArg("log_interval_s")) {
    int v = server.arg("log_interval_s").toInt();
    if (v >= 5 && v <= 3600) cfg.log_interval_s = (uint16_t)v;
  }
  if (server.hasArg("log_boot_grace_s")) {
    int v = server.arg("log_boot_grace_s").toInt();
    if (v >= 0 && v <= 3600) cfg.log_boot_grace_s = (uint16_t)v;
  }
  if (server.hasArg("log_retention_days")) {
    int v = server.arg("log_retention_days").toInt();
    if (v >= 1 && v <= MAX_RETENTION_DAYS) cfg.log_retention_days = (uint16_t)v;
  }
  cfg.alert_enabled = server.hasArg("alert_enabled");
  if (server.hasArg("alert_eco2")) {
    int v = server.arg("alert_eco2").toInt();
    if (v >= 0 && v <= 20000) cfg.alert_eco2 = (uint16_t)v;
  }
  if (server.hasArg("alert_tvoc")) {
    int v = server.arg("alert_tvoc").toInt();
    if (v >= 0 && v <= 60000) cfg.alert_tvoc = (uint16_t)v;
  }
  if (server.hasArg("alert_aqi")) {
    int v = server.arg("alert_aqi").toInt();
    if (v >= 1 && v <= 5) cfg.alert_aqi = (uint8_t)v;
  }
  cfg.cloud_enabled = server.hasArg("cloud_enabled");
  if (server.hasArg("ts_api_key")) {
    String s = server.arg("ts_api_key");
    s.trim();
    if (s.length() <= 16) { strncpy(cfg.ts_api_key, s.c_str(), sizeof(cfg.ts_api_key) - 1); cfg.ts_api_key[sizeof(cfg.ts_api_key) - 1] = '\0'; }
  }
  if (server.hasArg("ts_channel")) {
    String s = server.arg("ts_channel");
    s.trim();
    if (s.length() <= 8) { strncpy(cfg.ts_channel, s.c_str(), sizeof(cfg.ts_channel) - 1); cfg.ts_channel[sizeof(cfg.ts_channel) - 1] = '\0'; }
  }
  if (server.hasArg("cloud_interval_s")) {
    int v = server.arg("cloud_interval_s").toInt();
    if (v >= 15 && v <= 3600) cfg.cloud_interval_s = (uint16_t)v;
  }

  saveSettings();

  if (wifi_changed || name_changed) {
    // Reboot so setup() re-applies the new credentials / DHCP hostname.
    server.sendHeader("Cache-Control", "no-cache");
    server.send(200, "application/json; charset=utf-8",
                wifi_changed
                  ? "{\"ok\":true,\"reboot\":true,\"msg\":\"WiFi 已更改,设备即将重启…\"}"
                  : "{\"ok\":true,\"reboot\":true,\"msg\":\"设备名称已更改,设备即将重启…\"}");
    delay(400);
    ESP.restart();
  } else {
    reinitNTP();   // apply NTP server / timezone immediately
    server.sendHeader("Cache-Control", "no-cache");
    server.send(200, "application/json; charset=utf-8",
                "{\"ok\":true,\"reboot\":false,\"msg\":\"已保存\"}");
  }
}

// ═══════════════════════════════════════════════════════════════════
//  DATA LOGGING (LittleFS)  — one binary file per calendar day
// ═══════════════════════════════════════════════════════════════════
//  Record (12 bytes, little-endian):
//    uint32_t t;     // unix epoch seconds
//    uint16_t eco2;  // ppm
//    uint16_t tvoc;  // ppb
//    int16_t  temp;  // °C × 10
//    uint16_t rh;    // % RH × 10
//  Ring buffer = keep only the last LOG_RETENTION_DAYS day-files.

#pragma pack(push, 1)
struct LogRec {
  uint32_t t;
  uint16_t eco2;
  uint16_t tvoc;
  int16_t  temp;
  uint16_t rh;
};
#pragma pack(pop)

// Path like "/log/20260731.bin" for a given epoch time.
String logPathForDay(time_t t) {
  struct tm *ti = localtime(&t);
  char buf[20];
  strftime(buf, sizeof(buf), "/log/%Y%m%d.bin", ti);
  return String(buf);
}

void initStorage() {
  if (!LittleFS.begin(true)) {              // format on first failure
    Serial.println("FAIL: LittleFS mount (check partition scheme)");
    return;
  }
  LittleFS.mkdir(LOG_DIR);
  Serial.println("OK:  LittleFS mounted");
  pruneLogs();
}

// Delete day files older than the retention window. Only runs once time
// is synced, so we never delete based on a bogus pre-NTP clock.
void pruneLogs() {
  if (!data.time_synced) return;

  // Cutoff = start (00:00 local) of (RETENTION-1) days ago, so we keep
  // exactly LOG_RETENTION_DAYS calendar-day files (today .. RETENTION-1 ago).
  time_t now = time(nullptr);
  struct tm tn;
  localtime_r(&now, &tn);
  tn.tm_hour = 0; tn.tm_min = 0; tn.tm_sec = 0;
  time_t cutoff = mktime(&tn) - (time_t)(cfg.log_retention_days - 1) * 86400L;

  File root = LittleFS.open(LOG_DIR);
  if (!root || !root.isDirectory()) return;

  File f = root.openNextFile();
  while (f) {
    String path = String(f.path());
    String name = String(f.name());         // e.g. "20260731.bin"
    f.close();                              // close before removing
    if (name.length() == 12 && name.endsWith(".bin")) {
      int y = name.substring(0, 4).toInt();
      int mo = name.substring(4, 6).toInt();
      int d  = name.substring(6, 8).toInt();
      if (y >= 2000) {
        struct tm tm0 = {0};
        tm0.tm_year = y - 1900;
        tm0.tm_mon  = mo - 1;
        tm0.tm_mday = d;
        time_t ft = mktime(&tm0);
        if (ft != (time_t)-1 && ft < cutoff) {
          LittleFS.remove(path);
          Serial.printf("LOG: pruned %s\n", name.c_str());
        }
      }
    }
    f = root.openNextFile();
  }
  root.close();
}

// Append one 12-byte record to today's file. Only called when time is
// synced and both sensors have valid data (see loop()).
void appendLogRecord() {
  LogRec r;
  r.t    = (uint32_t)time(nullptr);
  r.eco2 = data.eco2;
  r.tvoc = data.tvoc;
  r.temp = (int16_t)roundf(data.temperature * 10.0f);
  r.rh   = (uint16_t)roundf(data.humidity * 10.0f);

  File f = LittleFS.open(logPathForDay(r.t), FILE_APPEND);
  if (!f) {
    Serial.println("LOG: append open failed");
    return;
  }
  f.write((const uint8_t *)&r, sizeof(r));
  f.close();
}

// GET /history?metric={eco2|tvoc|temp|hum}&days={1..30}  (default days=1)
// Returns a downsampled time series over the last `days` days. Binning is
// done incrementally so RAM use is bounded regardless of how much data is
// stored (HISTORY_MAX_BINS × 16 B on the heap).
void handleHistory() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");

  // --- parse args ---------------------------------------------------
  char metric = 'e';
  if (server.hasArg("metric")) {
    String m = server.arg("metric");
    metric = m.length() ? m.charAt(0) : 'e';
    if (metric != 'e' && metric != 'v' && metric != 't' && metric != 'h') {
      server.send(400, "text/plain", "400: bad metric");
      return;
    }
  }
  int days = 1;
  if (server.hasArg("days")) {
    days = server.arg("days").toInt();
    if (days < 1 || days > (int)cfg.log_retention_days) days = 1;
  }

  // --- window -------------------------------------------------------
  time_t now = time(nullptr);
  if (!data.time_synced || now < 1700000000) {
    server.send(200, "application/json; charset=utf-8",
                "{\"metric\":\"\",\"unit\":\"\",\"digits\":0,\"points\":[],\"min\":0,\"max\":0,\"avg\":0,\"count\":0}");
    return;
  }
  uint32_t start = (uint32_t)(now - (time_t)days * 86400L);

  // --- metric metadata ----------------------------------------------
  String metricName; const char *unit; int digits;
  if      (metric == 'v') { metricName = "tvoc"; unit = "ppb"; digits = 0; }
  else if (metric == 't') { metricName = "temp"; unit = "°C";  digits = 1; }
  else if (metric == 'h') { metricName = "hum";  unit = "%RH"; digits = 1; }
  else                    { metricName = "eco2"; unit = "ppm"; digits = 0; }

  // --- incremental binning -------------------------------------------
  struct Bin { uint32_t t0; uint32_t count; double sum; };
  Bin *bins = (Bin *)calloc(HISTORY_MAX_BINS, sizeof(Bin));
  if (!bins) {
    server.send(500, "text/plain", "500: no memory");
    return;
  }
  int nbins = 0;

  double vsum = 0; uint32_t vcnt = 0;
  float  vmin = 1e30f, vmax = -1e30f;

  uint32_t binSec = ((uint32_t)days * 86400UL + HISTORY_MAX_BINS - 1) / HISTORY_MAX_BINS;
  if (binSec == 0) binSec = 1;

  // Read enough day-files to cover the rolling window (today .. days ago).
  for (int d = 0; d <= days; ++d) {
    time_t day = now - (time_t)d * 86400L;
    File f = LittleFS.open(logPathForDay(day), FILE_READ);
    if (!f) continue;
    LogRec r;
    while (f.read((uint8_t *)&r, sizeof(r)) == (int)sizeof(r)) {
      if (r.t < start || r.t > (uint32_t)now) continue;
      float v;
      switch (metric) {
        case 'e': v = (float)r.eco2; break;
        case 'v': v = (float)r.tvoc; break;
        case 't': v = r.temp / 10.0f; break;
        default:  v = r.rh   / 10.0f; break;
      }
      vsum += v; vcnt++;
      if (v < vmin) vmin = v;
      if (v > vmax) vmax = v;

      uint32_t idx = (r.t - start) / binSec;
      if ((int)idx >= HISTORY_MAX_BINS) idx = HISTORY_MAX_BINS - 1;
      if ((int)idx >= nbins) {
        for (int k = nbins; k <= (int)idx; ++k) {
          bins[k].t0    = start + k * binSec;
          bins[k].count = 0;
          bins[k].sum   = 0;
        }
        nbins = (int)idx + 1;
      }
      bins[idx].sum += v;
      bins[idx].count++;
    }
    f.close();
  }

  // --- build JSON ----------------------------------------------------
  String json;
  json.reserve(HISTORY_MAX_BINS * 24 + 64);
  json = "{\"metric\":\"" + metricName + "\",\"unit\":\"" + unit;
  json += "\",\"digits\":";
  json += digits;
  json += ",\"points\":[";
  bool first = true;
  for (int i = 0; i < nbins; ++i) {
    if (bins[i].count == 0) continue;
    if (!first) json += ",";
    first = false;
    json += "[";
    json += (unsigned long)bins[i].t0;
    json += ",";
    json += String(bins[i].sum / bins[i].count, digits);
    json += "]";
  }
  json += "],\"min\":";
  json += (vcnt ? String(vmin, digits) : String("0"));
  json += ",\"max\":";
  json += (vcnt ? String(vmax, digits) : String("0"));
  json += ",\"avg\":";
  json += (vcnt ? String(vsum / vcnt, digits) : String("0"));
  json += ",\"count\":";
  json += (unsigned long)vcnt;
  json += "}";

  free(bins);
  server.send(200, "application/json; charset=utf-8", json);
}

// GET /export[?days={1..30}]  — download all logged data as CSV.
// Streams via chunked transfer (chunkResponseBegin/Write/End) so the full
// 30-day history (~1.7 MB) can be served without building it all in RAM.
void handleExport() {
  int days = cfg.log_retention_days;
  if (server.hasArg("days")) {
    days = server.arg("days").toInt();
    if (days < 1 || days > (int)cfg.log_retention_days) days = cfg.log_retention_days;
  }
  time_t now = time(nullptr);
  time_t start = 0;
  if (data.time_synced && now >= 1700000000)
    start = now - (time_t)days * 86400L;

  // Collect day-files and sort them oldest → newest (YYYYMMDD sorts lexically).
  String files[MAX_RETENTION_DAYS + 1];
  int nfiles = 0;
  File root = LittleFS.open(LOG_DIR);
  if (root && root.isDirectory()) {
    File f = root.openNextFile();
    while (f && nfiles < (int)(sizeof(files) / sizeof(files[0]))) {
      String name = String(f.name());
      if (name.length() == 12 && name.endsWith(".bin")) files[nfiles++] = name;
      f = root.openNextFile();
    }
    root.close();
  }
  for (int i = 1; i < nfiles; i++) {          // insertion sort by name
    String key = files[i];
    int j = i - 1;
    while (j >= 0 && files[j] > key) { files[j + 1] = files[j]; j--; }
    files[j + 1] = key;
  }

  server.sendHeader("Content-Disposition", "attachment; filename=aq_log.csv");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.chunkResponseBegin("text/csv");

  // Small output buffer flushed to the client whenever it fills up.
  char buf[2048];
  int used = 0;
  auto flush = [&]() {
    if (used > 0) { server.chunkWrite(buf, used); used = 0; }
  };
  auto emit = [&](const char *s) {
    while (*s) {
      if (used == (int)sizeof(buf)) flush();
      buf[used++] = *s++;
    }
  };

  emit("timestamp_iso,timestamp_unix,eco2_ppm,tvoc_ppb,temp_c,hum_pct\r\n");

  char row[80];
  char iso[24];
  unsigned long rows = 0;
  for (int i = 0; i < nfiles; i++) {
    String path = String(LOG_DIR) + "/" + files[i];
    File f = LittleFS.open(path, FILE_READ);
    if (!f) continue;
    LogRec r;
    while (f.read((uint8_t *)&r, sizeof(r)) == (int)sizeof(r)) {
      if (r.t == 0 || (start && r.t < (uint32_t)start)) continue;
      time_t tt = (time_t)r.t;
      struct tm *ti = localtime(&tt);
      strftime(iso, sizeof(iso), "%Y-%m-%d %H:%M:%S", ti);
      int rlen = snprintf(row, sizeof(row),
                          "%s,%lu,%u,%u,%.1f,%.1f\r\n",
                          iso, (unsigned long)r.t, (unsigned)r.eco2,
                          (unsigned)r.tvoc, r.temp / 10.0f, r.rh / 10.0f);
      if (rlen >= (int)sizeof(row)) rlen = (int)sizeof(row) - 1;
      row[rlen] = '\0';
      emit(row);
      rows++;
    }
    f.close();
  }
  flush();
  server.chunkResponseEnd();
  Serial.printf("EXPORT: %lu rows\n", rows);
}

// GET/POST /clear?pwd=...  — delete ALL history files. Requires the
// CLEAR_PASSWORD. Also resets the in-RAM min/max tracking.
void handleClear() {
  if (!server.hasArg("pwd") || server.arg("pwd") != String(cfg.clear_pwd)) {
    server.send(403, "application/json; charset=utf-8",
                "{\"ok\":false,\"msg\":\"wrong password\"}");
    return;
  }

  int deleted = 0;
  File root = LittleFS.open(LOG_DIR);
  if (root && root.isDirectory()) {
    File f = root.openNextFile();
    while (f) {
      String path = String(f.path());
      f.close();
      LittleFS.remove(path);
      deleted++;
      f = root.openNextFile();
    }
    root.close();
  }

  // Reset min/max tracking so the dashboard starts fresh too.
  data.temp_min = 999.0f;  data.temp_max = -999.0f;
  data.hum_min  = 999.0f;  data.hum_max  = -999.0f;
  data.eco2_min = 65535;   data.eco2_max = 0;
  data.tvoc_min = 65535;   data.tvoc_max = 0;
  data.temp_min_time = data.temp_max_time = 0;
  data.hum_min_time  = data.hum_max_time  = 0;
  data.eco2_min_time = data.eco2_max_time = 0;
  data.tvoc_min_time = data.tvoc_max_time = 0;

  String json = "{\"ok\":true,\"deleted\":" + String(deleted) + "}";
  server.send(200, "application/json; charset=utf-8", json);
  Serial.printf("CLEAR: deleted %d files\n", deleted);
}

// ═══════════════════════════════════════════════════════════════════
//  CONFIG (LittleFS /cfg.txt)
// ═══════════════════════════════════════════════════════════════════
//  Simple "key=value" line file. WebServer::arg() already URL-decodes
//  form values, so no escaping needed beyond rejecting newlines.

void setDefaults() {
  strncpy(cfg.wifi_ssid, WIFI_SSID, sizeof(cfg.wifi_ssid) - 1);
  cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
  strncpy(cfg.wifi_pass, WIFI_PASSWORD, sizeof(cfg.wifi_pass) - 1);
  cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = '\0';
  strncpy(cfg.device_name, "AirQuality-Monitor", sizeof(cfg.device_name) - 1);
  cfg.device_name[sizeof(cfg.device_name) - 1] = '\0';
  strncpy(cfg.clear_pwd, "1234", sizeof(cfg.clear_pwd) - 1);
  cfg.clear_pwd[sizeof(cfg.clear_pwd) - 1] = '\0';
  strncpy(cfg.ntp_server, NTP_SERVER_DEFAULT, sizeof(cfg.ntp_server) - 1);
  cfg.ntp_server[sizeof(cfg.ntp_server) - 1] = '\0';
  cfg.tz_sec             = 8 * 3600;   // UTC+8
  cfg.log_interval_s     = 60;
  cfg.log_boot_grace_s   = 180;   // ENS160 takes ~3 min to fully stabilise
  cfg.log_retention_days = 30;
  cfg.alert_enabled      = true;
  cfg.alert_eco2         = 1000;
  cfg.alert_tvoc         = 1000;
  cfg.alert_aqi          = 3;
  cfg.cloud_enabled      = false;
  cfg.ts_api_key[0]      = '\0';
  cfg.ts_channel[0]      = '\0';
  cfg.cloud_interval_s   = 60;
}

// mDNS/DHCP hostname rules: 1–32 chars, letters / digits / hyphen only.
static bool validHostname(const char *s) {
  size_t n = strlen(s);
  if (n == 0 || n > 32) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

void loadSettings() {
  setDefaults();
  File f = LittleFS.open(CFG_FILE, FILE_READ);
  if (!f) {                       // first boot (or partition wiped) → defaults
    Serial.println("CFG: none found, writing defaults");
    saveSettings();
    return;
  }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();                   // drops \r and surrounding whitespace
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    if      (key == "wifi_ssid")          { strncpy(cfg.wifi_ssid, val.c_str(), sizeof(cfg.wifi_ssid) - 1); cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0'; }
    else if (key == "wifi_pass")          { strncpy(cfg.wifi_pass, val.c_str(), sizeof(cfg.wifi_pass) - 1); cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = '\0'; }
    else if (key == "device_name")        { strncpy(cfg.device_name, val.c_str(), sizeof(cfg.device_name) - 1); cfg.device_name[sizeof(cfg.device_name) - 1] = '\0'; }
    else if (key == "clear_pwd")          { strncpy(cfg.clear_pwd, val.c_str(), sizeof(cfg.clear_pwd) - 1); cfg.clear_pwd[sizeof(cfg.clear_pwd) - 1] = '\0'; }
    else if (key == "ntp_server")         { strncpy(cfg.ntp_server, val.c_str(), sizeof(cfg.ntp_server) - 1); cfg.ntp_server[sizeof(cfg.ntp_server) - 1] = '\0'; }
    else if (key == "tz_sec")             cfg.tz_sec             = val.toInt();
    else if (key == "log_interval_s")     cfg.log_interval_s     = (uint16_t)val.toInt();
    else if (key == "log_boot_grace_s")   cfg.log_boot_grace_s   = (uint16_t)val.toInt();
    else if (key == "log_retention_days") cfg.log_retention_days = (uint16_t)val.toInt();
    else if (key == "alert_enabled")      cfg.alert_enabled      = (val == "1");
    else if (key == "alert_eco2")         cfg.alert_eco2         = (uint16_t)val.toInt();
    else if (key == "alert_tvoc")         cfg.alert_tvoc         = (uint16_t)val.toInt();
    else if (key == "alert_aqi")          cfg.alert_aqi          = (uint8_t)val.toInt();
    else if (key == "cloud_enabled")      cfg.cloud_enabled      = (val == "1");
    else if (key == "ts_api_key")         { strncpy(cfg.ts_api_key, val.c_str(), sizeof(cfg.ts_api_key) - 1); cfg.ts_api_key[sizeof(cfg.ts_api_key) - 1] = '\0'; }
    else if (key == "ts_channel")         { strncpy(cfg.ts_channel, val.c_str(), sizeof(cfg.ts_channel) - 1); cfg.ts_channel[sizeof(cfg.ts_channel) - 1] = '\0'; }
    else if (key == "cloud_interval_s")   cfg.cloud_interval_s   = (uint16_t)val.toInt();
  }
  f.close();

  // sanity clamps so a hand-edited / bogus file can't break things
  if (cfg.log_interval_s < 5) cfg.log_interval_s = 5;
  if (cfg.log_boot_grace_s > 3600) cfg.log_boot_grace_s = 180;
  if (cfg.cloud_interval_s < 15 || cfg.cloud_interval_s > 3600) cfg.cloud_interval_s = 60;
  if (cfg.log_retention_days < 1 || cfg.log_retention_days > MAX_RETENTION_DAYS)
    cfg.log_retention_days = 30;
  if (cfg.alert_eco2 == 0) cfg.alert_eco2 = 1000;
  if (cfg.alert_tvoc == 0) cfg.alert_tvoc = 1000;
  if (cfg.alert_aqi < 1 || cfg.alert_aqi > 5) cfg.alert_aqi = 3;
  if (!validHostname(cfg.device_name))
    strncpy(cfg.device_name, "AirQuality-Monitor", sizeof(cfg.device_name) - 1);
  if (cfg.ntp_server[0] == '\0') strncpy(cfg.ntp_server, NTP_SERVER_DEFAULT, sizeof(cfg.ntp_server) - 1);
}

void saveSettings() {
  File f = LittleFS.open(CFG_FILE, FILE_WRITE);
  if (!f) { Serial.println("CFG: save FAILED"); return; }
  f.printf("wifi_ssid=%s\n", cfg.wifi_ssid);
  f.printf("wifi_pass=%s\n", cfg.wifi_pass);
  f.printf("device_name=%s\n", cfg.device_name);
  f.printf("clear_pwd=%s\n", cfg.clear_pwd);
  f.printf("ntp_server=%s\n", cfg.ntp_server);
  f.printf("tz_sec=%ld\n", (long)cfg.tz_sec);
  f.printf("log_interval_s=%u\n", (unsigned)cfg.log_interval_s);
  f.printf("log_boot_grace_s=%u\n", (unsigned)cfg.log_boot_grace_s);
  f.printf("log_retention_days=%u\n", (unsigned)cfg.log_retention_days);
  f.printf("alert_enabled=%d\n", cfg.alert_enabled ? 1 : 0);
  f.printf("alert_eco2=%u\n", (unsigned)cfg.alert_eco2);
  f.printf("alert_tvoc=%u\n", (unsigned)cfg.alert_tvoc);
  f.printf("alert_aqi=%u\n", (unsigned)cfg.alert_aqi);
  f.printf("cloud_enabled=%d\n", cfg.cloud_enabled ? 1 : 0);
  f.printf("ts_api_key=%s\n", cfg.ts_api_key);
  f.printf("ts_channel=%s\n", cfg.ts_channel);
  f.printf("cloud_interval_s=%u\n", (unsigned)cfg.cloud_interval_s);
  f.close();
  Serial.println("CFG: saved");
}

// Re-apply NTP server + timezone; force a re-sync with the new server.
void reinitNTP() {
  configTime(cfg.tz_sec, 0, cfg.ntp_server, NTP_SERVER_FALLBACK);
  data.time_synced = false;
}

// Recompute the threshold-alert flag + message (called every ~1 s).
void computeAlert() {
  data.alert = false;
  data.alert_msg[0] = '\0';
  if (!cfg.alert_enabled || !data.ens160_ok) return;
  if (data.eco2 > cfg.alert_eco2) {
    data.alert = true;
    snprintf(data.alert_msg, sizeof(data.alert_msg), "eCO₂ %u/%u ppm", data.eco2, cfg.alert_eco2);
  } else if (data.tvoc > cfg.alert_tvoc) {
    data.alert = true;
    snprintf(data.alert_msg, sizeof(data.alert_msg), "TVOC %u/%u ppb", data.tvoc, cfg.alert_tvoc);
  } else if (data.aqi >= cfg.alert_aqi) {
    data.alert = true;
    snprintf(data.alert_msg, sizeof(data.alert_msg), "AQI %u (阈值 %u)", data.aqi, cfg.alert_aqi);
  }
}

// Upload the latest readings to a ThingSpeak channel via HTTPS (blocking,
// ~1-5 s; free tier minimum is 15 s between updates). Field map:
//   field1=eCO₂ ppm  field2=TVOC ppb  field3=temp °C  field4=RH %  field5=AQI
void uploadToCloud() {
  if (cfg.ts_api_key[0] == '\0' || cfg.ts_channel[0] == '\0') return;

  WiFiClientSecure client;
  client.setInsecure();               // accept the server cert without pinning

  String url = "https://api.thingspeak.com/update?api_key=";
  url += cfg.ts_api_key;
  url += "&field1=";
  url += String(data.eco2);
  url += "&field2=";
  url += String(data.tvoc);
  url += "&field3=";
  url += String(data.temperature, 1);
  url += "&field4=";
  url += String(data.humidity, 0);
  url += "&field5=";
  url += String(data.aqi);

  Serial.printf("CLOUD: channel %s → upload\n", cfg.ts_channel);
  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("CLOUD: HTTP begin failed");
    return;
  }
  http.setTimeout(5000);
  int code = http.GET();
  if (code > 0) {
    String resp = http.getString();
    resp.trim();
    // On success ThingSpeak returns the new entry id; on failure it returns "0".
    if (code == 200 && resp.length() > 0 && resp != "0") {
      Serial.printf("CLOUD: OK entry=%s\n", resp.c_str());
    } else {
      Serial.printf("CLOUD: HTTP %d resp=%s\n", code, resp.c_str());
    }
  } else {
    Serial.printf("CLOUD: request error %d\n", code);
  }
  http.end();
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

  // Non-blocking: measure(false) returns true ONLY when a fresh sample is
  // available — it never waits. ENS160 in STD mode produces a new reading
  // ~1×/s, so we poll from loop() at ENS160_POLL_MS and the main loop (and
  // therefore the web server) never gets blocked by the sensor.
  if (!ens160.measure(false)) return false;

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

// RGB565 colour helper.
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static uint16_t aqiColor(uint8_t aqi) {
  switch (aqi) {
    case 1: return ST77XX_GREEN;
    case 2: return rgb565(160, 240, 0);
    case 3: return ST77XX_YELLOW;
    case 4: return ST77XX_ORANGE;
    case 5: return ST77XX_RED;
    default: return rgb565(140, 148, 168);
  }
}

// WiFi signal colour from strength percentage (0-100).
static uint16_t wifiColor(int pct) {
  if (pct >= 80) return ST77XX_GREEN;
  if (pct >= 60) return rgb565(160, 240, 0);
  if (pct >= 40) return ST77XX_YELLOW;
  if (pct >= 20) return ST77XX_ORANGE;
  return ST77XX_RED;
}

// One 2px-thick top arc drawn with short line segments (a basic primitive,
// guaranteed to render on any display).
void tftTopArc(int16_t cx, int16_t cy, int r, uint16_t color) {
  // upper-left quarter arc (90° top → 180° left) = 90° CCW from before
  for (int deg = 90; deg < 180; deg += 15) {
    float a1 = (float)deg * PI / 180.0f;
    float a2 = (float)(deg + 15) * PI / 180.0f;
    tft.drawLine(cx + (int)(r * cos(a1)), cy - (int)(r * sin(a1)),
                 cx + (int)(r * cos(a2)), cy - (int)(r * sin(a2)), color);
  }
}

// Classic three-segment WiFi icon (dot + 3 arc bands). The stronger the
// signal, the more bands are lit. Pass pct = -1 for a dim "off" icon.
void tftDrawWifiIcon(int16_t x, int16_t y, int pct, uint16_t color) {
  const uint16_t DIM = rgb565(70, 74, 90);
  int level = pct >= 70 ? 3 : pct >= 40 ? 2 : pct >= 15 ? 1 : 0;
  tft.fillCircle(x, y, 2, level > 0 ? color : DIM);
  tftTopArc(x, y, 3, level >= 1 ? color : DIM);
  tftTopArc(x, y, 7, level >= 2 ? color : DIM);
  tftTopArc(x, y, 11, level >= 3 ? color : DIM);
}

// Static frame of one 2×2 card (background, border, label) — drawn once.
void tftMetricCardFrame(int16_t x, int16_t y, const char *label) {
  const uint16_t CARD   = rgb565(22, 24, 38);
  const uint16_t BORDER = rgb565(52, 56, 76);
  const uint16_t LABEL  = rgb565(140, 148, 168);

  tft.fillRoundRect(x, y, 112, 64, 8, CARD);
  tft.drawRoundRect(x, y, 112, 64, 8, BORDER);
  tft.setTextSize(1);
  tft.setTextColor(LABEL);
  tft.setCursor(x + 8, y + 4);
  tft.print(label);
}

// Refresh the dynamic text of one card (value/unit + L-min H-max), but only
// when the content actually changed (force=true on first render / after wake).
// Text is drawn with an opaque background; a region is cleared only when the
// new text is shorter than the old — so steady same-length updates replace
// glyphs in place instead of blanking the card (that blanking was the blink).
void tftMetricCardValues(int16_t x, int16_t y, const char *unit,
                         const char *value, const char *mn, const char *mx,
                         bool force, char *cV, char *cN, char *cX) {
  const uint16_t CARD  = rgb565(22, 24, 38);
  const uint16_t LABEL = rgb565(140, 148, 168);
  const uint16_t MINC  = rgb565(120, 190, 240);
  const uint16_t MAXC  = rgb565(255, 150, 120);

  if (!force && strcmp(cV, value) == 0 &&
                strcmp(cN, mn)    == 0 &&
                strcmp(cX, mx)    == 0) return;   // nothing changed

  bool vshrink = force || strlen(value) < strlen(cV);
  if (vshrink) tft.fillRect(x + 6, y + 16, 100, 28, CARD);

  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE, CARD);
  tft.setCursor(x + 8, y + 18);
  tft.print(value);
  tft.setTextSize(1);
  tft.setTextColor(LABEL, CARD);
  tft.setCursor(x + 8 + strlen(value) * 12 + 2, y + 24);
  tft.print(unit);

  bool nshrink = force || strlen(mn) < strlen(cN) || strlen(mx) < strlen(cX);
  if (nshrink) tft.fillRect(x + 6, y + 44, 100, 10, CARD);
  tft.setTextSize(1);
  tft.setTextColor(MINC, CARD);
  tft.setCursor(x + 8, y + 46);
  tft.print("L");
  tft.setCursor(x + 16, y + 46);
  tft.print(mn);
  tft.setTextColor(MAXC, CARD);
  tft.setCursor(x + 58, y + 46);
  tft.print("H");
  tft.setCursor(x + 66, y + 46);
  tft.print(mx);

  strncpy(cV, value, 11); cV[11] = '\0';
  strncpy(cN, mn, 11);    cN[11] = '\0';
  strncpy(cX, mx, 11);    cX[11] = '\0';
}

// Draw the whole static frame once (background, cards, labels).
void tftDrawLayout() {
  const uint16_t BG     = ST77XX_BLACK;
  const uint16_t CARD   = rgb565(22, 24, 38);
  const uint16_t BORDER = rgb565(52, 56, 76);
  const uint16_t LABEL  = rgb565(140, 148, 168);

  tft.fillScreen(BG);

  tft.fillRoundRect(AQI_X, 56, AQI_W, 62, 10, CARD);
  tft.drawRoundRect(AQI_X, 56, AQI_W, 62, 10, BORDER);
  tft.setTextSize(1);
  tft.setTextColor(LABEL);
  tft.setCursor(AQI_X + (AQI_W - strlen("AIR QUALITY") * 6) / 2, 62);
  tft.print("AIR QUALITY");

  tftMetricCardFrame(CARD_L, 132, "eCO2");
  tftMetricCardFrame(CARD_R, 132, "TVOC");
  tftMetricCardFrame(CARD_L, 208, "TEMP");
  tftMetricCardFrame(CARD_R, 208, "HUM");
}

// Called every second. Change-detection: each region is only redrawn when
// its content actually changed, and text uses an opaque background so it is
// replaced in place instead of "clear-then-draw" — that is what caused the
// visible blink on the panel. Pass force (first render / after wake) to
// redraw everything.
void updateDisplay() {
  const uint16_t BG    = ST77XX_BLACK;
  const uint16_t CARD  = rgb565(22, 24, 38);
  const uint16_t LABEL = rgb565(140, 148, 168);

  bool force = !data.d_valid;

  // ── Time ─────────────────────────────────────────────────────
  // Always 8 chars ("--:--:--" or "HH:MM:SS") → fixed region; opaque-bg
  // text replaces it cleanly with no full-strip clear.
  tft.setTextSize(4);
  tft.setTextColor(data.alert ? ST77XX_RED : ST77XX_WHITE, BG);
  tft.setCursor((TFT_W - strlen(data.time_str) * 24) / 2, 6);
  tft.print(data.time_str);

  // ── Date ─────────────────────────────────────────────────────
  char ds[16] = "";
  if (data.time_synced) {
    time_t t = time(nullptr);
    struct tm *ti = localtime(&t);
    strftime(ds, sizeof(ds), "%Y-%m-%d", ti);
  }
  if (force || strcmp(data.d_date, ds) != 0) {
    strncpy(data.d_date, ds, sizeof(data.d_date) - 1);
    data.d_date[sizeof(data.d_date) - 1] = '\0';
    tft.fillRect(0, 40, TFT_W, 8, BG);
    if (ds[0]) {
      tft.setTextSize(1);
      tft.setTextColor(LABEL, BG);
      tft.setCursor((TFT_W - strlen(ds) * 6) / 2, 40);
      tft.print(ds);
    }
  }

  // ── AQI value + label (centred as a group in the banner) ─────
  const char *lbl = (data.aqi >= 1 && data.aqi <= 5) ? AQI_LABEL[data.aqi] : "";
  if (force || data.d_aqi != data.aqi || strcmp(data.d_aqi_lbl, lbl) != 0) {
    data.d_aqi = data.aqi;
    strncpy(data.d_aqi_lbl, lbl, sizeof(data.d_aqi_lbl) - 1);
    data.d_aqi_lbl[sizeof(data.d_aqi_lbl) - 1] = '\0';
    tft.fillRect(AQI_X, 74, AQI_W, 34, CARD);
    if (data.aqi >= 1 && data.aqi <= 5) {
      uint16_t c = aqiColor(data.aqi);
      int total = 24 + (int)strlen(lbl) * 18 + 8;   // digit(size4) + gap + label(size3)
      int x0 = AQI_X + (AQI_W - total) / 2;
      tft.setTextSize(4);
      tft.setTextColor(c, CARD);
      tft.setCursor(x0, 76);
      tft.print(data.aqi);
      tft.setTextSize(3);
      tft.setCursor(x0 + 32, 82);
      tft.print(lbl);
    } else {
      int x0 = AQI_X + (AQI_W - 48) / 2;   // "--" centred
      tft.setTextSize(4);
      tft.setTextColor(LABEL, CARD);
      tft.setCursor(x0, 76);
      tft.print("--");
    }
  }

  // ── metric values + min/max (change-detected) ──────────────────
  char b[12], mn[12], mx[12];

  if (data.ens160_ok) snprintf(b, sizeof(b), "%u", data.eco2); else strcpy(b, "--");
  snprintf(mn, sizeof(mn), "%u", data.eco2_min);
  snprintf(mx, sizeof(mx), "%u", data.eco2_max);
  tftMetricCardValues(CARD_L, 132, "ppm", b, mn, mx, force, data.d_e2, data.d_e2n, data.d_e2x);

  if (data.ens160_ok) snprintf(b, sizeof(b), "%u", data.tvoc); else strcpy(b, "--");
  snprintf(mn, sizeof(mn), "%u", data.tvoc_min);
  snprintf(mx, sizeof(mx), "%u", data.tvoc_max);
  tftMetricCardValues(CARD_R, 132, "ppb", b, mn, mx, force, data.d_tv, data.d_tvn, data.d_tvx);

  if (data.aht_ok) snprintf(b, sizeof(b), "%.1f", data.temperature); else strcpy(b, "--");
  snprintf(mn, sizeof(mn), data.temp_min > 990 ? "--" : "%.1f", data.temp_min);
  snprintf(mx, sizeof(mx), data.temp_max < -990 ? "--" : "%.1f", data.temp_max);
  tftMetricCardValues(CARD_L, 208, "C", b, mn, mx, force, data.d_tp, data.d_tpn, data.d_tpx);

  if (data.aht_ok) snprintf(b, sizeof(b), "%.0f", data.humidity); else strcpy(b, "--");
  snprintf(mn, sizeof(mn), data.hum_min > 990 ? "--" : "%.0f", data.hum_min);
  snprintf(mx, sizeof(mx), data.hum_max < -990 ? "--" : "%.0f", data.hum_max);
  tftMetricCardValues(CARD_R, 208, "%", b, mn, mx, force, data.d_hm, data.d_hmn, data.d_hmx);

  // ── Bottom area (whole strip redrawn only when its state/content changes)
  tft.setTextSize(1);
  int mode = (WiFi.status() == WL_CONNECTED) ? 1 : (cfg_mode ? 2 : 0);
  bool bottom_changed = force || data.d_wifi_state != mode;
  data.d_wifi_state = mode;

  if (mode == 1) {
    int rssi = WiFi.RSSI();
    int pct  = constrain(map(rssi, -90, -35, 0, 100), 0, 100);
    String ssid = WiFi.SSID();
    String ip   = WiFi.localIP().toString();
    bottom_changed = bottom_changed || data.d_pct != pct ||
                     strcmp(data.d_ssid, ssid.c_str()) != 0 ||
                     strcmp(data.d_ip, ip.c_str()) != 0;
    if (bottom_changed) {
      data.d_pct = pct;
      strncpy(data.d_ssid, ssid.c_str(), sizeof(data.d_ssid) - 1);
      data.d_ssid[sizeof(data.d_ssid) - 1] = '\0';
      strncpy(data.d_ip, ip.c_str(), sizeof(data.d_ip) - 1);
      data.d_ip[sizeof(data.d_ip) - 1] = '\0';
      uint16_t c = wifiColor(pct);
      tft.fillRect(0, 272, TFT_W, 36, BG);

      // RSSI in dBm (bottom-left, on the SSID row above the %)
      char rssibuf[12];
      snprintf(rssibuf, sizeof(rssibuf), "%ddBm", rssi);
      tft.setTextColor(c, BG);
      tft.setCursor(4, 278);
      tft.print(rssibuf);

      // SSID (centred, above the bottom row)
      tft.setTextColor(ST77XX_GREEN, BG);
      tft.setCursor((TFT_W - ssid.length() * 6) / 2, 278);
      tft.print(ssid);

      // IP (centred)
      tft.setTextColor(ST77XX_GREEN, BG);
      tft.setCursor((TFT_W - ip.length() * 6) / 2, 296);
      tft.print(ip);

      // signal % (bottom-left)
      tft.setTextColor(c, BG);
      tft.setCursor(4, 296);
      tft.print(pct);
      tft.print("%");

      // WiFi icon (bottom-right)
      tftDrawWifiIcon(TFT_W - 16, 302, pct, c);
    }
  } else if (mode == 2) {
    if (bottom_changed) {
      tft.fillRect(0, 272, TFT_W, 36, BG);
      // AP config hotspot — the device is reachable at 192.168.4.1
      tft.setTextColor(ST77XX_GREEN, BG);
      String ap = String("Config: ") + CFG_AP_SSID;
      tft.setCursor((TFT_W - ap.length() * 6) / 2, 278);
      tft.print(ap);
      String aip = WiFi.softAPIP().toString();
      tft.setCursor((TFT_W - aip.length() * 6) / 2, 296);
      tft.print(aip);
      tftDrawWifiIcon(TFT_W - 16, 302, 40, ST77XX_YELLOW);
    }
  } else {
    if (bottom_changed) {
      tft.fillRect(0, 272, TFT_W, 36, BG);
      const char *ns = "WiFi: no connection";
      tft.setTextColor(ST77XX_GREEN, BG);
      tft.setCursor((TFT_W - strlen(ns) * 6) / 2, 296);
      tft.print(ns);

      tft.setTextColor(rgb565(140, 148, 168), BG);
      tft.setCursor(4, 296);
      tft.print("--%");

      tftDrawWifiIcon(TFT_W - 16, 302, -1, rgb565(70, 74, 90));
    }
  }

  data.d_valid = true;
}

// Turn the TFT panel + backlight on/off from the web dashboard.
void setDisplay(bool on) {
  if (on == data.display_on) return;
  data.display_on = on;
  if (on) {
    // Wake sequence per ST7789 datasheet: SLPOUT → sleep-out time → DISPON.
    if (TFT_BL >= 0) digitalWrite(TFT_BL, HIGH);
    tft.enableSleep(false);      // ST77XX_SLPOUT
    delay(130);
    tft.enableDisplay(true);     // ST77XX_DISPON
    delay(20);
    tftDrawLayout();
    data.d_valid = false;        // force a full redraw after wake
    updateDisplay();
  } else {
    // Sleep sequence per datasheet: Display OFF first, then Sleep In.
    tft.enableDisplay(false);    // ST77XX_DISPOFF
    delay(20);
    tft.enableSleep(true);       // ST77XX_SLPIN
    delay(20);
    if (TFT_BL >= 0) digitalWrite(TFT_BL, LOW);
  }
  Serial.printf("DISPLAY: %s\n", on ? "ON" : "OFF");
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

  // --- TFT (ST7789 240×320, SPI) ----------------------------------
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(TFT_W, TFT_H);
  tft.setRotation(0);
  tft.setTextWrap(false);
  if (TFT_BL >= 0) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
  }
  tft.fillScreen(ST77XX_BLACK);
#if TFT_SELFTEST
  // Power-on self-test: flash full-screen colours to verify the SPI path.
  tft.fillScreen(ST77XX_RED);    delay(400);
  tft.fillScreen(ST77XX_GREEN);  delay(400);
  tft.fillScreen(ST77XX_BLUE);   delay(400);
  tft.fillScreen(ST77XX_BLACK);
#endif
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(28, 40);
  tft.print("AQ Monitor");
  tft.setTextSize(1);
  tft.setCursor(28, 72);
  tft.print("Booting...");
  Serial.println("OK:  TFT ST7789");

  // --- AHT21 ------------------------------------------------------
  if (!initAHT21()) {
    Serial.println("FAIL: AHT21 not found");
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_RED);
    tft.setCursor(8, 100);
    tft.print("AHT21 FAIL");
  } else {
    Serial.println("OK:  AHT21");
  }

  // --- ENS160 -----------------------------------------------------
  if (!initENS160()) {
    Serial.println("FAIL: ENS160 not found");
  } else {
    Serial.println("OK:  ENS160 (warming up...)");
  }

  // --- Config + storage (LittleFS must be mounted before WiFi) -------
  initStorage();
  loadSettings();

  // --- WiFi -------------------------------------------------------
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(8, 20);
  tft.print("Connecting WiFi...");

  // IMPORTANT: setHostname() MUST run before mode(WIFI_STA). The core's
  // mode() copies the hostname into the STA netif only at the moment STA
  // is enabled; setHostname() alone never re-applies it afterwards, so
  // doing it in the other order would leave the default "esp32c3-XXXX".
  WiFi.setHostname(cfg.device_name);   // DHCP: report friendly name to router
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);

  int dot = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    tft.setCursor(8, 36);
    tft.print("SSID: ");
    tft.print(cfg.wifi_ssid);
    tft.setCursor(8, 52);
    tft.print(dot % 4 == 0 ? "    " : dot % 4 == 1 ? ".   " :
              dot % 4 == 2 ? "..  " : "... ");
    dot++;
    if (dot > 60) {
      tft.fillScreen(ST77XX_BLACK);
      tft.setTextColor(ST77XX_RED);
      tft.setCursor(8, 40);
      tft.print("WiFi FAIL");
      tft.setTextColor(ST77XX_WHITE);
      tft.setCursor(8, 56);
      tft.print("Check credentials");
      Serial.println("\nFAIL: WiFi connection timeout");

      // Fall back to an AP config hotspot so the device can still be
      // reconfigured from a phone at http://192.168.4.1
      WiFi.disconnect(true);          // drop the failed STA attempt cleanly
      delay(100);
      WiFi.mode(WIFI_AP);
      delay(50);
      if (!WiFi.softAP(CFG_AP_SSID)) { // verify the AP actually came up
        Serial.println("AP: softAP failed, retrying…");
        delay(300);
        WiFi.softAP(CFG_AP_SSID);
      }
      cfg_mode = true;
      // Captive-portal DNS: answer EVERY domain with the device IP, so a
      // phone's connectivity probe / any typed URL lands on our config page.
      dns.start(53, "*", WiFi.softAPIP());
      Serial.printf("AP: SSID=%s IP=%s (config @ http://%s)\n",
                    CFG_AP_SSID, WiFi.softAPIP().toString().c_str(),
                    WiFi.softAPIP().toString().c_str());
      tft.fillScreen(ST77XX_BLACK);
      tft.setTextColor(ST77XX_WHITE);
      tft.setCursor(8, 16);
      tft.print("Config Mode");
      tft.setCursor(8, 40);
      tft.print("AP: ");
      tft.print(CFG_AP_SSID);
      tft.setCursor(8, 64);
      tft.print("IP ");
      tft.print(WiFi.softAPIP());
      break;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("IP:  %s\nGW:  %s\nDNS: %s\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.dnsIP().toString().c_str());
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(8, 16);
    tft.print("IP  ");
    tft.print(WiFi.localIP());
    tft.setCursor(8, 32);
    tft.print("GW  ");
    tft.print(WiFi.gatewayIP());
    tft.setCursor(8, 48);
    tft.print("DNS ");
    tft.print(WiFi.dnsIP());

    // --- mDNS: advertise as <device>.local (visible to scanner tools) ---
    if (MDNS.begin(cfg.device_name)) {
      Serial.printf("MDNS: %s.local\n", cfg.device_name);
      MDNS.addService("http", "tcp", 80);   // helps scanner tools (Fing etc.)
    } else {
      Serial.println("MDNS: begin failed");
    }

    // --- NTP time sync (non-blocking: start SNTP, don't wait) -------
    Serial.println("NTP: started (background sync)");
    reinitNTP();
  }

  // --- Web server (built-in, no external libs) --------------------
  server.on("/",        handleRoot);
  server.on("/data",    handleData);
  server.on("/ip",      handleIP);
  server.on("/history", handleHistory);
  server.on("/export",  handleExport);
  server.on("/clear",        handleClear);
  server.on("/display",      handleDisplay);
  server.on("/config",       handleConfigPage);
  server.on("/config/data",  handleConfigData);
  server.on("/config/save",  handleConfigSave);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started");

  // Draw the main UI frame once, then fill in current values.
  tftDrawLayout();
  updateDisplay();

  // --- Finish boot screen ------------------------------------------
  delay(1500);
  data.boot_ms = millis();          // start of the log warm-up grace period
  data.last_read_ms = millis();
}

// ═══════════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════════

void loop() {
  // Serve web requests FIRST. Keeping this running continuously (never
  // blocked by a sensor read) is what makes the dashboard refresh at ~1 Hz.
  server.handleClient();
  if (cfg_mode) dns.processNextRequest();   // captive-portal DNS lookups

  uint32_t now = millis();

  // ENS160 is polled non-blocking several times per second, so a fresh
  // reading is picked up within ~ENS160_POLL_MS while the loop stays
  // responsive to the web server.
  if (now - data.last_ens_poll_ms >= ENS160_POLL_MS) {
    data.last_ens_poll_ms = now;
    readENS160();
  }

  // Slow 1 s tasks: clock string, AHT21, display, serial
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
    computeAlert();
    if (data.display_on) updateDisplay();   // skip draws while TFT is off

    // Append a log record every LOG_INTERVAL_S — only once time is synced,
    // both sensors have produced valid data, AND the boot warm-up grace has
    // elapsed (skips ENS160's ~3 min stabilisation period).
    if (data.time_synced && data.aht_ok && data.ens160_ok &&
        now - data.boot_ms >= (uint32_t)cfg.log_boot_grace_s * 1000UL &&
        now - data.last_log_ms >= (uint32_t)cfg.log_interval_s * 1000UL) {
      data.last_log_ms = now;
      appendLogRecord();
    }

    // Cloud upload (ThingSpeak) — WiFi online, sensors valid, past the
    // warm-up grace, on the configured interval. Blocking (~1-5 s).
    if (cfg.cloud_enabled && WiFi.status() == WL_CONNECTED &&
        data.aht_ok && data.ens160_ok &&
        now - data.boot_ms >= (uint32_t)cfg.log_boot_grace_s * 1000UL &&
        now - data.last_cloud_ms >= (uint32_t)cfg.cloud_interval_s * 1000UL) {
      data.last_cloud_ms = now;
      uploadToCloud();
    }

    // Ring-buffer cleanup: run once right after NTP sync, then hourly.
    if (data.time_synced && (now - data.last_prune_ms >= 3600UL * 1000UL ||
                             data.last_prune_ms == 0)) {
      data.last_prune_ms = now;
      pruneLogs();
    }

    // Serial debug
    if (data.ens160_ok || data.aht_ok) {
      Serial.printf("[%s] AQI=%d  eCO₂=%d ppm  TVOC=%d ppb  "
                    "T=%.1f°C  RH=%.0f%%\n",
                    data.time_str, data.aqi, data.eco2, data.tvoc,
                    data.temperature, data.humidity);
    }
  }
}
