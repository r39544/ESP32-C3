# ESP32-C3 空气质量监测仪

基于 ESP32-C3 的空气监测站:**ENS160**(eCO₂ / TVOC / AQI)+ **AHT21**(温湿度),数据实时显示在 **SSD1306 OLED** 上,并内置一个 **Web 仪表盘**(无需外部前端依赖)。支持**历史数据记录与曲线查看、CSV 导出**。

---

## 硬件接线

三个 I²C 设备共用同一条 I²C 总线(SDA / SCL):

| ESP32-C3 | ENS160 | AHT21 | SSD1306 |
|----------|--------|-------|---------|
| 3V3      | VIN    | VIN   | VCC     |
| GND      | GND    | GND   | GND     |
| GPIO 4   | SDA    | SDA   | SDA     |
| GPIO 5   | SCL    | SCL   | SCL     |

I²C 地址:ENS160 = `0x53`、AHT21 = `0x38`、SSD1306 = `0x3C`。

> 其它常见板子引脚(修改 `.ino` 中 `I2C_SDA` / `I2C_SCL`):
> - Seeed XIAO C3: SDA=4, SCL=5(默认)
> - ESP32-C3-DevKitM-1: SDA=8, SCL=9
> - AirM2M C3: SDA=6, SCL=7
> - Lolin C3 Mini: SDA=8, SCL=9

---

## 功能

### 实时监测
- 每秒刷新一次:温度、湿度、eCO₂、TVOC、AQI。
- OLED 显示(5 行):时间 / AQI / eCO₂ / TVOC / 温湿度,最后一行显示 **IP 地址**。
- Web 仪表盘(`http://<设备IP>`),深色卡片式,1 秒轮询刷新,纯 `fetch()` + 手写 CSS/JS,无外部 CDN。

### 历史数据记录(30 天环形)
- 每 **1 分钟**在 LittleFS 中追加一条 12 字节二进制记录(时间戳、eCO₂、TVOC、温度、湿度)。
- 按**日历日**存放:`/log/YYYYMMDD.bin`。
- **环形覆盖**:开机后 + 每小时清理,只保留最近 **30 天**,超过自动删除最旧的。
- 仅在 NTP 时间已同步、且两个传感器均有有效读数时记录(避免预热期垃圾数据)。

### Web 历史曲线
- 点击仪表盘上任意数据卡片(温度 / 湿度 / eCO₂ / TVOC)弹出历史曲线。
- 支持 **24H / 7D / 30D** 切换,悬停显示该点时间与数值,附 min / avg / max 统计。
- 曲线为手写 `<canvas>` 折线图,无第三方图表库。

### CSV 导出
- 仪表盘底部「⬇ 导出 CSV」:导出全部(保留的 30 天)数据。
- 历史曲线弹窗内「⬇ 导出该区间」:只导出当前选择的范围(24H/7D/30D)。
- CSV 格式:`timestamp_iso,timestamp_unix,eco2_ppm,tvoc_ppb,temp_c,hum_pct`

### HTTP 接口

| 路径 | 说明 |
|------|------|
| `/` | 仪表盘页面 |
| `/data` | 实时数据 JSON(1 秒轮询) |
| `/ip` | 返回 IP |
| `/history?metric={eco2\|tvoc\|temp\|hum}&days={1..30}` | 历史曲线数据(自动降采样) |
| `/export?days={1..30}` | 下载 CSV(默认全部) |

---

## 依赖库

需要先在 Arduino IDE **库管理器** 中安装:

| 库 | 用于 |
|----|------|
| **Adafruit SSD1306** | OLED 驱动 |
| **Adafruit GFX Library** | 图形基础库 |
| **Adafruit AHTX0**(自动带 BusIO / Unified Sensor) | AHT21 温湿度 |
| **ENS160 - Adafruit Fork**(或 ScioSense ENS160) | ENS160 气体传感器(代码 `#include <ScioSense_ENS160.h>`) |

`WiFi`、`WebServer`、`LittleFS`、`Wire`、`time` 均为 ESP32 核心内置,无需单独安装。

---

## ⚠️ 上传前必做(Arduino IDE)

1. **工具 → 板**:`XIAO_ESP32C3`(或你实际用的 C3 板)
2. **工具 → Flash Size**:`4MB`
3. **工具 → Partition Scheme**:`No OTA (2MB APP/2MB SPIFFS)` ← **不选这个,`LittleFS.begin()` 会失败**
4. **上传**。首次会串口打印 `OK: LittleFS mounted`(LittleFS 自动格式化)

> 完整流程:复制 `config.h.example` 为 `config.h` 并填入 WiFi 账号密码 → 打开 `esp32-air-quality-monitor.ino` → 按上述 1–3 设置 → 上传 → 串口监视器(115200)确认 `OK: LittleFS mounted` 和 IP 地址;ENS160 约 3 分钟预热后开始输出读数。

---

## ⚠️ 注意事项

- **No OTA 分区会去掉 OTA 升级槽位**——改为此方案后只能用串口烧录,不能 OTA。
- **首次上电会自动格式化 LittleFS**(`LittleFS.begin(true)`),数据分区为空白。
- ENS160 **预热约 3 分钟**,预热期间显示 `--`;历史记录要等预热完成 + NTP 同步后才开始写。
- **WiFi 断连或 NTP 未同步时不记录数据**(没有可靠时间戳),恢复后继续追加。
- 30 天数据约 **506 KB**(12 B/分钟),数据分区 1.875 MB,余量充足;如需更久可调大 `LOG_RETENTION_DAYS`。
- `/export` 与 `/history` 大范围查询是**同步阻塞**的:导出 30 天全量时仪表盘会短暂停顿几秒,属正常。
- **`config.h` 已被 .gitignore 忽略**,不要提交 WiFi 密码到仓库。
- `platformio.ini` 为旧文件,本项目按 **Arduino IDE** 工程维护(PlatformIO 的依赖声明与 `src/` 布局未同步)。

---

## 目录结构

```
esp32-air-quality-monitor/
├── esp32-air-quality-monitor.ino   # 全部代码(含嵌入式 Web 页面)
├── config.h.example                # WiFi 配置模板 → 复制为 config.h
├── config.h                        # 本地 WiFi 凭据(不提交)
├── platformio.ini                  # 遗留文件,未维护
└── README.md
```

## 常见问题

- **串口打印 `FAIL: LittleFS mount`** → 未选择 "No OTA" 分区方案,或 Flash Size 不是 4MB。
- **页面打不开** → 确认 OLED 显示的 IP,浏览器访问 `http://<IP>`。
- **历史曲线没数据** → 需要先等 NTP 时间同步 + ENS160 预热完成,再等约 1 分钟产生第一条记录。
