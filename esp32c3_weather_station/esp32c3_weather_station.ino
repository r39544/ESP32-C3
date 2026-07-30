/**
 * ESP32-C3 + AHT20 + BMP280 + SSD1306 温湿度大气压+时钟显示器
 *
 * 接线 (I2C):
 *   ESP32-C3    ->  AHT20 / BMP280 / SSD1306
 *   GPIO5 (SCL) -> SCL (共用)
 *   GPIO4 (SDA) -> SDA (共用)
 *   3.3V        -> VCC
 *   GND         -> GND
 *
 * I2C 地址:
 *   AHT20   0x38
 *   BMP280  0x76
 *   SSD1306 0x3C
 *
 * 按键: 按 BOOT 按钮 (GPIO9) 切换页面 (传感器→时钟→网络→循环)
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <time.h>

// ======== WiFi 配置 (修改为你自己的 WiFi) ========
const char* ssid     = "SEMI-PACK";
const char* password = "Wpa8Aes6Psk";

// ======== IIS 推送配置 ========
const char* serverHost = "127.0.0.1";  // IIS 服务器地址
const int   serverPort = 80;                     // HTTP 端口
const char* serverPath = "/api/weather";          // 接收端路径
const unsigned long PUSH_INTERVAL = 60000;         // 推送间隔 60 秒

// ======== NTP 配置 ========
const char* ntpServer = "ntp3.aliyun.com";
const long  gmtOffset_sec = 8 * 3600;     // UTC+8 中国时区
const int   daylightOffset_sec = 0;

// ======== I2C 引脚配置 (ESP32-C3) ========
#define I2C_SDA  4
#define I2C_SCL  5

// ======== 按钮配置 (BOOT 键切换页面) ========
const int BTN_PIN = 9;       // ESP32-C3 BOOT 按钮
const int DEBOUNCE_MS = 300; // 防抖

// ======== OLED 配置 ========
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ======== 传感器对象 ========
Adafruit_AHTX0  aht20;
Adafruit_BMP280 bmp280;

// ======== 定时器 ========
unsigned long lastUpdate = 0;
unsigned long lastBtnTime = 0;
bool lastBtnState = HIGH;
unsigned long lastNtpSync = 0;
unsigned long lastPageSwitch = 0;
unsigned long lastPush = 0;
const unsigned long UPDATE_INTERVAL = 1000;   // 显示刷新间隔 1秒
const unsigned long NTP_INTERVAL = 3600000;    // NTP 重新同步间隔 1小时
const unsigned long PAGE_ROTATE_INTERVAL = 5000; // 页面自动轮换间隔 5秒

// ======== 状态标记 ========
bool aht20OK = false;
bool bmp280OK = false;
bool oledOK = false;
bool wifiConnected = false;
bool ntpSynced = false;
int currentPage = 0;   // 0=传感器, 1=时钟, 2=网络

// ======== 掉线缓存 ========
const char* BACKLOG_FILE = "/backlog.csv";
int backlogCount = 0;

// ======== 星期名称 ========
const char* weekDays[] = {"7", "1", "2", "3", "4", "5", "6"};
const char* weekDaysEn[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// ======== 最新传感器值 (供显示和推送共用) ========
float lastTemp = NAN, lastHum = NAN, lastPressure = NAN, lastAltitude = NAN;

// ======== 设备标识 ========
String deviceId;

// ======== WiFi 信号图标 (右下角) ========
void drawWiFiIcon() {
  int x = 118, y = 0;
  if (wifiConnected) {
    // 3 层弧形 (用上半圆表示信号强度)
    display.drawCircleHelper(x, y, 3, 9, SSD1306_WHITE);
    display.drawCircleHelper(x, y, 6, 9, SSD1306_WHITE);
    display.drawCircleHelper(x, y, 9, 9, SSD1306_WHITE);
    // 底部小圆点 (设备)
    display.fillCircle(x, y, 1, SSD1306_WHITE);
  } else {
    // 未连接: 画 X
    display.drawLine(x - 2, y - 2, x + 2, y + 2, SSD1306_WHITE);
    display.drawLine(x + 2, y - 2, x - 2, y + 2, SSD1306_WHITE);
  }
}

// ======== 页面0: 传感器数据显示 ========
void drawSensorPage(float temp, float hum, float pressure, float altitude) {
  display.clearDisplay();

  // 第1行: 海拔
  display.setTextSize(2);
  display.setCursor(0, 0);
  if (!isnan(altitude)) {
    display.print(altitude, 1);
    display.print(F(" m"));
  } else {
    display.print(F("---.- m"));
  }

  // 第2行: 温度
  display.setCursor(0, 16);
  display.setTextSize(2);
  if (!isnan(temp)) {
    display.print(temp, 1);
    display.print(F(" C"));
  } else {
    display.print(F("--.-C"));
  }

  // 第3行: 湿度
  display.setTextSize(2);
  display.setCursor(0, 32);
  if (!isnan(hum)) {
    display.print(hum, 2);
    display.print(F(" %"));
  } else {
    display.print(F("--.- %"));
  }

  // 第4行: 气压
  display.setCursor(0, 50);
  display.setTextSize(2);
  if (!isnan(pressure)) {
    display.print(pressure, 2);
    display.print(F("hPa"));
  } else {
    display.print(F("----.- hPa"));
  }


  drawWiFiIcon();
  display.display();
}

// ======== 页面1: 时钟显示 ========
void drawClockPage() {
  display.clearDisplay();

  struct tm timeinfo;
  bool gotTime = getLocalTime(&timeinfo);

  if (gotTime) {
    int16_t x1, y1;
    uint16_t w, h;

    // 第1行: 日期 (居中)
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    char dateStr[16];
    snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d",
      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    display.getTextBounds(dateStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 2);
    display.print(dateStr);

    // 分隔线
    display.drawFastHLine(0, 11, SCREEN_WIDTH, SSD1306_WHITE);

    // 第2行: HH:MM:SS 同排 (HH:MM 用3号, :SS 用2号, 总宽126px刚好)
    char timeStr[6];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    char secStr[4];
    snprintf(secStr, sizeof(secStr), ":%02d", timeinfo.tm_sec);

    display.setTextSize(3);
    display.setCursor(1, 16);
    display.print(timeStr);

    display.setTextSize(2);
    display.setCursor(91, 24);
    display.print(secStr);

    // 第3行: 英文星期几 (在时间下方居中)
    display.setTextSize(1);
    const char* dayEn = weekDaysEn[timeinfo.tm_wday];
    display.getTextBounds(dayEn, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 43);
    display.print(dayEn);

  } else {
    // 未获取到时间
    display.setTextSize(2);
    display.setCursor(4, 10);
    display.println(F("Syncing"));
    display.setCursor(10, 30);
    display.println(F("Time..."));
  }

  // 底部: NTP/WiFi 状态
  display.setTextSize(1);
  display.setCursor(0, 56);
  if (wifiConnected && ntpSynced && gotTime) {
    display.print(F("NTP synced"));
  } else if (wifiConnected && !ntpSynced) {
    display.print(F("NTP syncing..."));
  } else {
    display.print(F("WiFi disconnected"));
  }


  drawWiFiIcon();
  display.display();
}

// ======== 页面2: 网络信息显示 ========
void drawNetworkPage() {
  display.clearDisplay();

  if (wifiConnected) {
    int rssi = WiFi.RSSI();
    int pct = constrain(2 * (rssi + 100), 0, 100);

    // 第1行: WiFi 名称 + 信号强度百分比
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(F("SSID:"));
    display.print(WiFi.SSID());
    display.setCursor(100, 0);
    display.print(pct);
    display.print(F("%"));

    // 分隔线
    display.drawFastHLine(0, 9, SCREEN_WIDTH, SSD1306_WHITE);

    // 第2行: IP 地址
    display.setCursor(0, 16);
    display.print(F("IP:"));
    display.println(WiFi.localIP());

    // 第3行: 网关
    display.setCursor(0, 24);
    display.print(F("GW:"));
    display.println(WiFi.gatewayIP());

    // 第4行: 子网掩码
    display.setCursor(0, 32);
    display.print(F("MASK:"));
    display.println(WiFi.subnetMask());

    // 第5行: DNS
    display.setCursor(0, 40);
    display.print(F("DNS:"));
    display.println(WiFi.dnsIP());

    // 第6行: 蓝牙 MAC 地址 (ESP32-C3 上同 WiFi STA MAC)
    display.setCursor(0, 48);
    display.print(F("BT:"));
    display.println(WiFi.macAddress());

    // 第7行: RSSI + 缓存状态
    display.setCursor(0, 56);
    display.print(F("RSSI:"));
    display.print(rssi);
    display.print(F("dBm"));
    if (backlogCount > 0) {
      display.setCursor(76, 56);
      display.print(F("BUF:"));
      display.print(backlogCount);
    }

  } else {
    // 未连接 WiFi
    display.setTextSize(2);
    display.setCursor(10, 10);
    display.println(F("WiFi"));
    display.setCursor(10, 30);
    display.println(F("Disconnected"));
  }

  display.display();
}

// ======== 缓存 & 推送 ========

// 发送单条数据, true = 服务器已收到(含4xx)
bool sendOne(float temp, float hum, float pressure, float alt) {
  HTTPClient http;
  char url[128];
  snprintf(url, sizeof(url), "http://%s:%d%s", serverHost, serverPort, serverPath);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  char payload[256];
  snprintf(payload, sizeof(payload),
    "{\"device\":\"%s\",\"temp\":%.1f,\"humidity\":%.1f,\"pressure\":%.1f,\"altitude\":%.1f}",
    deviceId.c_str(), temp, hum, pressure, alt);

  int code = http.POST(payload);
  http.end();
  if (code > 0) {
    Serial.printf("[HTTP] POST %d\n", code);
    return true;       // 服务器有响应就算送达
  }
  return false;        // 连接失败
}

// 缓存到 LittleFS
void saveToBacklog(float temp, float hum, float pressure, float alt) {
  if (!LittleFS.begin(true)) return;
  File f = LittleFS.open(BACKLOG_FILE, FILE_APPEND);
  if (!f) return;
  f.printf("%.1f,%.1f,%.1f,%.1f\n", temp, hum, pressure, alt);
  f.close();
  backlogCount++;
  Serial.printf("[BUF] Saved to backlog (%d)\n", backlogCount);
}

// 补发缓存 (每次最多发5条, 不阻塞显示太久)
void flushBacklog() {
  if (!LittleFS.begin(true)) return;
  if (!LittleFS.exists(BACKLOG_FILE)) return;

  File f = LittleFS.open(BACKLOG_FILE, FILE_READ);
  if (!f) return;

  String content = f.readString();
  f.close();

  if (content.length() == 0) {
    LittleFS.remove(BACKLOG_FILE);
    backlogCount = 0;
    return;
  }

  // 解析行, 逐条补发, 最多 5 条
  int pos = 0, lastPos = 0, sent = 0, kept = 0, batch = 5;
  String remaining;

  while (pos < content.length() && batch > 0) {
    int nl = content.indexOf('\n', pos);
    if (nl < 0) break;
    String line = content.substring(pos, nl);
    pos = nl + 1;
    line.trim();
    if (line.length() == 0) continue;

    // 解析 CSV
    char buf[64];
    strncpy(buf, line.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* t = strtok(buf, ",");
    char* h = strtok(NULL, ",");
    char* p = strtok(NULL, ",");
    char* a = strtok(NULL, ",");
    if (!t || !h || !p || !a) { kept++; continue; }

    float ft = atof(t), fh = atof(h), fp = atof(p), fa = atof(a);
    if (sendOne(ft, fh, fp, fa)) {
      sent++;
      batch--;
    } else {
      remaining += line + "\n";
      kept++;
      break;  // 连不上就停, 下次再试
    }
  }

  // 把剩下的行写回去
  remaining += content.substring(pos);

  if (remaining.length() == 0) {
    LittleFS.remove(BACKLOG_FILE);
    backlogCount = 0;
    Serial.printf("[BUF] Flushed all backlog (%d sent)\n", sent);
  } else {
    File w = LittleFS.open(BACKLOG_FILE, FILE_WRITE);
    if (w) { w.print(remaining); w.close(); }
    // 简单估算剩余条数
    int c = 0;
    for (int i = 0; i < remaining.length(); i++) if (remaining[i] == '\n') c++;
    backlogCount = c;
    Serial.printf("[BUF] Flushed %d, %d remain in backlog\n", sent, backlogCount);
  }
}

// 主推送入口
void pushToServer(float temp, float hum, float pressure, float alt) {
  // 先补发缓存
  flushBacklog();

  // 再发实时数据
  if (!wifiConnected || isnan(temp)) return;

  if (!sendOne(temp, hum, pressure, alt)) {
    saveToBacklog(temp, hum, pressure, alt);
  }
}

// ======== 连接 WiFi ========
void connectWiFi() {
  if (strcmp(ssid, "your_wifi_ssid") == 0) {
    Serial.println("[WARN] 请先在代码中修改 ssid 和 password!");
    return;
  }

  Serial.print("[WiFi] 连接 ");
  Serial.print(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println(" OK");
    Serial.print("  IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" 失败");
  }
}

// ======== 同步 NTP 时间 ========
void syncNTP() {
  if (!wifiConnected) {
    ntpSynced = false;
    return;
  }

  Serial.print("[NTP] 同步 ");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // 等待 NTP 获取时间
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 20) {
    delay(500);
    attempts++;
  }

  if (attempts < 20) {
    ntpSynced = true;
    lastNtpSync = millis();
    Serial.println("OK");
    Serial.print("  Time: ");
    Serial.printf("%04d-%02d-%02d %02d:%02d:%02d\n",
      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  } else {
    Serial.println("失败");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n==================================");
  Serial.println("ESP32-C3 气象站 + NTP 时钟");
  Serial.println("==================================\n");

  // 生成设备 ID (取 MAC 去冒号)
  String macStr = WiFi.macAddress();
  macStr.replace(":", "");
  deviceId = macStr;
  Serial.printf("[DEVICE] ID: %s\n", deviceId.c_str());

  // 初始化 LittleFS (掉线缓存用)
  if (LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS OK");
    if (LittleFS.exists(BACKLOG_FILE)) {
      File f = LittleFS.open(BACKLOG_FILE, FILE_READ);
      if (f) {
        String c = f.readString();
        int nl = 0;
        for (int i = 0; i < c.length(); i++) if (c[i] == '\n') nl++;
        backlogCount = nl;
        f.close();
        if (backlogCount > 0)
          Serial.printf("[FS] Backlog file exists, %d records pending\n", backlogCount);
      }
    }
  } else {
    Serial.println("[FS] LittleFS mount FAIL");
  }

  // 初始化按钮
  pinMode(BTN_PIN, INPUT_PULLUP);

  // 初始化 I2C
  Wire.begin(I2C_SDA, I2C_SCL);

  // ---------- 初始化 OLED ----------
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledOK = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("  Initializing..."));
    display.display();
    Serial.println("[OK] SSD1306 OLED");
  } else {
    Serial.println("[FAIL] SSD1306 OLED not found at 0x3C");
  }

  // ---------- 初始化 AHT20 ----------
  if (aht20.begin()) {
    aht20OK = true;
    Serial.println("[OK] AHT20 温湿度传感器");
  } else {
    Serial.println("[FAIL] AHT20 not found at 0x38");
  }

  // ---------- 初始化 BMP280 (自动尝试 0x76 和 0x77) ----------
  uint8_t bmpAddr = 0x76;
  if (!bmp280.begin(0x76)) {
    if (bmp280.begin(0x77)) {
      bmpAddr = 0x77;
      bmp280OK = true;
    }
  } else {
    bmp280OK = true;
  }

  if (bmp280OK) {
    bmp280.setSampling(Adafruit_BMP280::MODE_NORMAL,
                       Adafruit_BMP280::SAMPLING_X2,
                       Adafruit_BMP280::SAMPLING_X16,
                       Adafruit_BMP280::FILTER_X16,
                       Adafruit_BMP280::STANDBY_MS_500);
    Serial.print("[OK] BMP280 at 0x");
    Serial.println(bmpAddr, HEX);
  } else {
    Serial.println("[FAIL] BMP280 not found at 0x76 or 0x77");
  }

  // 显示初始化状态
  if (oledOK) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F(" AHT20 BMP280 SSD1306"));
    display.println();
    display.print(F("  AHT20: ")); display.println(aht20OK ? F(" OK") : F("FAIL"));
    display.print(F("  BMP280: ")); display.println(bmp280OK ? F("OK") : F("FAIL"));
    display.print(F("  OLED:  "));  display.println(F(" OK"));
    display.display();
    delay(1500);
  }

  // ---------- 连接 WiFi ----------
  if (oledOK) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("  Connecting WiFi..."));
    display.display();
  }
  connectWiFi();

  // ---------- 同步 NTP ----------
  if (oledOK) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("  Syncing NTP..."));
    display.display();
  }
  syncNTP();

  Serial.println("\n--- 开始循环 ---\n");
}

void loop() {
  unsigned long now = millis();

  // ========== 检测按钮切换页面 ==========
  bool btnRead = digitalRead(BTN_PIN);
  if (btnRead == LOW && lastBtnState == HIGH && (now - lastBtnTime > DEBOUNCE_MS)) {
    lastBtnTime = now;
    lastPageSwitch = now;           // 手动切换后重置自动轮换计时
    currentPage = (currentPage + 1) % 3;  // 切换 0→1→2→0
    Serial.print("[Page] 切换到 ");
    switch (currentPage) {
      case 0: Serial.println(F("传感器")); break;
      case 1: Serial.println(F("时钟"));   break;
      case 2: Serial.println(F("网络"));   break;
    }
  }
  lastBtnState = btnRead;

  // ========== 定时 NTP 重新同步 ==========
  if (wifiConnected && ntpSynced && (now - lastNtpSync > NTP_INTERVAL)) {
    Serial.println("[NTP] 定时重新同步...");
    syncNTP();
  }

  // ========== 页面自动轮换 (每5秒, 仅传感器↔时钟) ==========
  if (now - lastPageSwitch >= PAGE_ROTATE_INTERVAL) {
    lastPageSwitch = now;
    currentPage = (currentPage == 0) ? 1 : 0;
    Serial.print("[AutoPage] 切换到 ");
    Serial.println(currentPage == 0 ? F("传感器") : F("时钟"));
  }

  // ========== 定时刷新生效 ==========
  if (now - lastUpdate >= UPDATE_INTERVAL) {
    lastUpdate = now;

    // ---------- 读取传感器 ----------
    float tempAHT = NAN, hum = NAN;
    if (aht20OK) {
      sensors_event_t humidity, temperature;
      aht20.getEvent(&humidity, &temperature);
      tempAHT = temperature.temperature;
      hum = humidity.relative_humidity;
      Serial.print("[AHT20] 温度: "); Serial.print(tempAHT, 1);
      Serial.print(" C\t湿度: "); Serial.print(hum, 1); Serial.println(" %");
    }

    float tempBMP = NAN, pressure = NAN, altitude = NAN;
    if (bmp280OK) {
      tempBMP = bmp280.readTemperature();
      pressure = bmp280.readPressure() / 100.0F;
      altitude = bmp280.readAltitude(1013.25);
      Serial.print("[BMP280] 温度: "); Serial.print(tempBMP, 1);
      Serial.print(" C\t气压: "); Serial.print(pressure, 1);
      Serial.print(" hPa\t海拔: "); Serial.print(altitude, 1); Serial.println(" m");
    }

    lastTemp = !isnan(tempAHT) ? tempAHT : tempBMP;
    lastHum = hum;
    lastPressure = pressure;
    lastAltitude = altitude;

    // ---------- 显示当前页面 ----------
    if (oledOK) {
      if (currentPage == 0) {
        drawSensorPage(lastTemp, lastHum, lastPressure, lastAltitude);
      } else if (currentPage == 1) {
        drawClockPage();
      } else {
        drawNetworkPage();
      }
    }

    Serial.println();
  }

  // ========== 定时推送到 IIS (每5秒, 不阻塞显示) ==========
  if (now - lastPush >= PUSH_INTERVAL) {
    lastPush = now;
    pushToServer(lastTemp, lastHum, lastPressure, lastAltitude);
  }
}
