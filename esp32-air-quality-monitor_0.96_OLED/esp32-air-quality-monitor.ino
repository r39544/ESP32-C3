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
#include <LittleFS.h>
#include <math.h>

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

// --- Sensor read intervals -------------------------------------------
#define READ_INTERVAL_MS  1000   // slow tasks (AHT21, time, display): every 1 s
#define ENS160_POLL_MS    200    // ENS160 non-blocking poll (~5×/s; new data ~1×/s)

// --- NTP time --------------------------------------------------------
#define TZ_OFFSET_SEC  (8 * 3600)   // UTC+8 (China) — change for your zone
#define NTP_SERVER1    "ntp2.aliyun.com"
#define NTP_SERVER2    "ntp3.aliyun.com"

// --- Data logging (LittleFS; requires "No OTA (2MB APP/2MB SPIFFS)" ---
// --- partition scheme in Arduino IDE) --------------------------------
#define LOG_DIR            "/log"
#define LOG_INTERVAL_S     60           // append a record every minute
#define LOG_RETENTION_DAYS 30           // keep the last N days (ring buffer)
#define HISTORY_MAX_BINS   1200         // max curve points returned by /history
#define CLEAR_PASSWORD     "1234"       // password required to clear history (plaintext)

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
  uint32_t last_read_ms     = 0;  // slow task (1 s)
  uint32_t last_ens_poll_ms = 0;  // ENS160 non-blocking poll (~200 ms)
  uint32_t last_log_ms      = 0;  // data log record (every LOG_INTERVAL_S)
  uint32_t last_prune_ms    = 0;  // log retention cleanup (hourly)
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
    <a class="export-btn" href="/export" download="aq_log.csv">⬇ 导出 CSV</a>
    <button class="export-btn danger" onclick="clearHistory()">🗑 清除历史</button>
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

    cctx.beginPath();
    pts.forEach((p, i) => i ? cctx.lineTo(sc.X(p[0]), sc.Y(p[1])) : cctx.moveTo(sc.X(p[0]), sc.Y(p[1])));
    cctx.strokeStyle = M.color; cctx.lineWidth = 2;
    cctx.lineJoin = 'round'; cctx.lineCap = 'round';
    cctx.stroke();
    cctx.lineTo(sc.X(sc.t1), sc.Y(sc.lo)); cctx.lineTo(sc.X(sc.t0), sc.Y(sc.lo)); cctx.closePath();
    cctx.fillStyle = M.color; cctx.globalAlpha = .12; cctx.fill(); cctx.globalAlpha = 1;
  }

  function renderChart() {
    const M = METRICS[chartMetric];
    const pts = (chartData && chartData.points) ? chartData.points : [];
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
  time_t cutoff = mktime(&tn) - (time_t)(LOG_RETENTION_DAYS - 1) * 86400L;

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
    if (days < 1 || days > LOG_RETENTION_DAYS) days = 1;
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
  int days = LOG_RETENTION_DAYS;
  if (server.hasArg("days")) {
    days = server.arg("days").toInt();
    if (days < 1 || days > LOG_RETENTION_DAYS) days = LOG_RETENTION_DAYS;
  }
  time_t now = time(nullptr);
  time_t start = 0;
  if (data.time_synced && now >= 1700000000)
    start = now - (time_t)days * 86400L;

  // Collect day-files and sort them oldest → newest (YYYYMMDD sorts lexically).
  String files[LOG_RETENTION_DAYS + 1];
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
  if (!server.hasArg("pwd") || server.arg("pwd") != String(CLEAR_PASSWORD)) {
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

  // Line 5: IP address
  display.setCursor(0, 56);
  display.print("IP ");
  if (WiFi.status() == WL_CONNECTED) {
    display.print(WiFi.localIP());
  } else {
    display.print("--");
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

  // --- LittleFS data logging ---------------------------------------
  initStorage();

  // --- Web server (built-in, no external libs) --------------------
  server.on("/",        handleRoot);
  server.on("/data",    handleData);
  server.on("/ip",      handleIP);
  server.on("/history", handleHistory);
  server.on("/export",  handleExport);
  server.on("/clear",   handleClear);
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
  // Serve web requests FIRST. Keeping this running continuously (never
  // blocked by a sensor read) is what makes the dashboard refresh at ~1 Hz.
  server.handleClient();

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
    updateDisplay();

    // Append a log record every LOG_INTERVAL_S (only once time is synced
    // and both sensors have produced valid data).
    if (data.time_synced && data.aht_ok && data.ens160_ok &&
        now - data.last_log_ms >= LOG_INTERVAL_S * 1000UL) {
      data.last_log_ms = now;
      appendLogRecord();
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
