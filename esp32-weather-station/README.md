# ESP32-C3 + SSD1306 天气预报站

基于 **ESP32-C3** 和 **0.96 寸 SSD1306 OLED** 的 WiFi 天气预报组件。
自动从 OpenWeatherMap 获取天气数据，每 30 分钟（可配置）刷新一次。

---

## 功能特性

- ✅ **天气图标** — 程序绘制太阳、云、雨、雪、闪电、雾等图标（无需位图数据）
- ✅ **当前温度** — 大字显示，支持 ℃
- ✅ **天气描述** — 支持中文/英文（通过 `config.h` 配置）
- ✅ **湿度 / 风速 / 气压**
- ✅ **城市 & 国家显示**
- ✅ **NTP 自动校时**
- ✅ **WiFi 信号强度条形指示**
- ✅ **多 WiFi 网络支持（备用网络）**
- ✅ **自动刷新（默认 30 分钟）**
- ✅ **异常处理 & 断线重连**

---

## 硬件接线

| ESP32-C3 | SSD1306 OLED |
|----------|--------------|
| 3.3V     | VCC          |
| GND      | GND          |
| GPIO 5   | SDA          |
| GPIO 6   | SCL          |

> 如果使用其他开发板，可在 `config.h` 中修改 `OLED_SDA` / `OLED_SCL` 引脚。

![连接示意图](https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/_static/esp32-c3-devkitm-1_pinmap.png)

*引脚图仅供参考，实际以你的开发板丝印为准。*

---

## 所需 Arduino 库

| 库名                | 版本     | 用途                   |
|---------------------|----------|------------------------|
| **U8g2**           | ≥ 2.34   | OLED 驱动 & 字体       |
| **ArduinoJson**    | ≥ 7.0    | JSON 解析              |
| **WiFi**           | 内置     | WiFi 连接              |
| **HTTPClient**     | 内置     | HTTP 请求              |

在 **Arduino IDE 库管理器** 中搜索安装 `U8g2` 和 `ArduinoJson`。

### PlatformIO (platformio.ini)

```ini
[env:esp32-c3-devkitm-1]
platform = espressif32
board = esp32-c3-devkitm-1
framework = arduino
lib_deps =
    olikraus/U8g2@^2.34.22
    bblanchon/ArduinoJson@^7.0.0
monitor_speed = 115200
```

---

## 使用步骤

### 1. 注册 OpenWeatherMap API

1. 访问 [openweathermap.org](https://openweathermap.org/api) → **Sign Up** 免费注册
2. 邮箱验证后，进入 [API Keys](https://home.openweathermap.org/api_keys) 页面
3. 复制你的 `API Key`（免费版每分钟 60 次，完全够用）

### 2. 配置 config.h

修改 `config.h`：

```cpp
#define WIFI_SSID1       "你的WiFi名称"
#define WIFI_PASSWORD1   "你的WiFi密码"
#define OWM_API_KEY      "你的API KEY"        // 从 openweathermap.org 获取
#define OWM_CITY         "Beijing"            // 城市英文名
#define OWM_LANG         "zh_cn"              // 中文显示
```

**城市名支持：**
- 国内：`Beijing`、`Shanghai`、`Shenzhen`、`Guangzhou`、`Chengdu` …
- 国外：`London`、`Tokyo`、`New York`、`Paris` …
- 完整列表参考 [OpenWeatherMap city list](http://bulk.openweathermap.org/sample/)

**语言选项：**
- `zh_cn` — 简体中文（如"晴"、"多云"）
- `en` — English
- `ja` — 日本語
- `fr`、`de`、`ru` …

**中文字体：** 如需 OLED 显示中文天气描述（"晴"、"多云"等），
取消 `config.h` 中的 `// #define USE_CHINESE_FONT` 注释：
```cpp
#define USE_CHINESE_FONT   // ← 去掉行首 //
```
这会启用 U8g2 内置的 WenQuanYi 12px 中文字体，额外占用约 12KB flash。

### 3. 烧录

- **Arduino IDE**: 选择 **ESP32-C3 Dev Module**，上传即可
- **PlatformIO**: `pio run --target upload`

### 4. 上电

OLED 依次显示：
1. 启动画面 "Weather Station"
2. 首次连接 WiFi + 获取数据（约 3-8 秒）
3. 天气主界面

---

## 屏幕布局

```
 ┌──────────────────────────────────┐
 │ [☀️]  +28°                       │  ← 天气图标 + 大字温度
 │       晴                         │  ← 天气描述
 │ Beijing, CN          14:32       │  ← 城市 & 时间
 │ Hum:45%  Wind:3.2  1013hPa ▂▄▆█ │  ← 详情 & WiFi 信号
 └──────────────────────────────────┘
```

---

## 低功耗优化（可选）

如需电池供电，可在 `loop()` 中添加 **light sleep** 模式：

```cpp
#include "esp_sleep.h"

// 在 delay(100) 之前插入
esp_light_sleep_start();  // ESP32-C3 支持 light sleep
```

或使用 **deep sleep** 模式（每 30 分钟唤醒一次）：

```cpp
esp_sleep_enable_timer_wakeup(fetchIntervalMs * 1000ULL);
esp_deep_sleep_start();
```

> Deep sleep 模式下 OLED 会断电，需要每次唤醒重新初始化。

---

## 常见问题

### ❌ OLED 不显示
- 检查 I2C 接线：SDA→GPIO5, SCL→GPIO6
- 尝试 I2C 地址扫描（很多库带示例 `i2c_scanner`）
- SSD1306 默认地址通常是 `0x3C` 或 `0x3D`

### ❌ WiFi 连不上
- 确认 SSID 和密码正确
- ESP32-C3 仅支持 **2.4 GHz** WiFi
- 检查路由器是否开启了 MAC 过滤

### ❌ 获取天气失败
- API Key 是否有效（有时需等待 1-2 小时激活）
- 检查串口输出中的 HTTP 错误码
- 城市名是否用英文（`Beijing` 而非 `北京`）

### ❌ 编译错误：JsonDocument 大小
- 如果出现 `ArduinoJson` 内存不足错误，可在代码中手动设置大小：
  ```cpp
  JsonDocument doc;  // 默认足够
  ```

---

## 项目结构

```
esp32-weather-station/
├── README.md                ← 本文件
├── esp32-weather-station.ino← 主程序（入口 & 全部逻辑）
├── config.h                 ← 配置（WiFi / API / 引脚）
└── platformio.ini           ←（可选）PIO 配置文件
```

---

## License

MIT
