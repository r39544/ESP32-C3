// ============================================================
//  WiFi & OpenWeatherMap 配置
// ============================================================

// ---- WiFi 网络（可添加多个） ----
#define WIFI_SSID1       "TJCNC"
#define WIFI_PASSWORD1   "27473212"

// #define WIFI_SSID2     "BackupWiFi"
// #define WIFI_PASSWORD2 "BackupPassword"

// ---- OpenWeatherMap API ----
// 免费注册: https://openweathermap.org/api
#define OWM_API_KEY      "3bfa3845a0a3d063e9b972497969fc44"
#define OWM_CITY         "Beijing"         // 城市名（英文）
#define OWM_LANG         "zh_cn"            // 语言: zh_cn / en / ja ...

// ---- 中文字体支持 ----
// 如果 OWM_LANG 设为 "zh_cn"，取消下行注释以显示中文天气描述。
// 需要 U8g2 内嵌 WenQuanYi 12px 中文字体（增加 ~12KB flash 占用）。
#define USE_CHINESE_FONT

// ---- 更新间隔 ----
#define WEATHER_UPDATE_INTERVAL  30  // 分钟（最小 10）

// ---- OLED I2C 引脚 (ESP32-C3) ----
#define OLED_SDA   4
#define OLED_SCL   5
#define OLED_ADDR  0x3C

// ---- NTP 服务器（用于显示时间） ----
#define NTP_SERVER1  "ntp.aliyun.com"
#define NTP_SERVER2  "pool.ntp.org"
#define TZ_OFFSET    28800   // 北京时间 UTC+8 = 8*3600
