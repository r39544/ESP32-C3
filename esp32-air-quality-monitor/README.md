# ESP32-C3 空气质量监测仪

基于 ESP32-C3 的空气监测站:**ENS160**(eCO₂ / TVOC / AQI)+ **AHT21**(温湿度),数据实时显示在 **ST7789 2.0" 240×320 TFT** 上,并内置一个 **Web 仪表盘**(无需外部前端依赖)。支持**历史数据记录与曲线查看、CSV 导出**。

---

## 硬件接线

### 传感器(I²C 总线,共用 SDA/SCL)

| ESP32-C3 | ENS160 | AHT21 |
|----------|--------|-------|
| 3V3      | VIN    | VIN   |
| GND      | GND    | GND   |
| GPIO 4   | SDA    | SDA   |
| GPIO 5   | SCL    | SCL   |

I²C 地址:ENS160 = `0x53`、AHT21 = `0x38`。

### 显示屏(ST7789 240×320,SPI,板子丝印:BL CS DC RST SDA SCL VCC GND)

> ⚠️ 此模块上的 **SDA = SPI MOSI(DIN)**、**SCL = SPI SCLK(CLK)**,不是 I²C。

| ESP32-C3 | ST7789 TFT |
|----------|-----------|
| 3V3      | VCC        |
| GND      | GND        |
| GPIO 8   | SCL (SCLK) |
| GPIO 6   | SDA (MOSI) |
| GPIO 10  | CS         |
| GPIO 7   | DC         |
| GPIO 9   | RST        |
| GPIO 3   | BL         |

> 若模块的背光(BL)已直接接 3V3,把 `.ino` 中 `TFT_BL` 改成 `-1`。
> 接线针对 **Seeed XIAO ESP32-C3**;其它板子请改 `.ino` 里的 `TFT_*` 与 `I2C_*` 引脚。

---

## 功能

### 实时监测
- 每秒刷新一次:温度、湿度、eCO₂、TVOC、AQI。
- TFT 显示(240×320 卡片布局):大号时间/日期、**彩色 AQI 卡**(绿/黄/橙/红随等级变化)、eCO₂/TVOC/温度/湿度 2×2 卡片(含 L/H 最值),底部显示 **SSID / IP / 信号强度**(SSID 一行,IP 居中,左下信号百分比、右下三段式 WiFi 图标,弧段按强度点亮)。
- 屏幕采用**局部刷新**(只重画变化的数字),无整屏闪烁;开机有红/绿/蓝自检闪烁(`TFT_SELFTEST` 宏,可设为 0 关闭)。
- Web 仪表盘(`http://<设备IP>`),深色卡片式,1 秒轮询刷新,纯 `fetch()` + 手写 CSS/JS,无外部 CDN。
- Web 仪表盘底部「⏻ 关闭屏幕 / 开启屏幕」按钮:一键让 TFT 进入睡眠(面板 `SLPIN` + 背光关断,省电),再次点击唤醒(`SLPOUT` + `DISPON`)并重绘。说明:因 BOOT 按钮与 `TFT_RST` 共用 GPIO9,无法用硬件按钮控制屏幕,故用 Web 按钮代替。
- **告警**:当 eCO₂ / TVOC / AQI 超过配置的阈值时,Web 仪表盘出现红色告警条(显示具体超标的指标与数值),TFT 顶部时间变红。阈值在「⚙ 设置」页可调,也可一键关闭告警。

### 网页配置(存入 LittleFS)
- 仪表盘底部「⚙ 设置」进入配置页,可修改 **设备名称、WiFi SSID/密码、清除历史密码、NTP 服务器、时区、日志记录间隔、日志保留天数、告警阈值**。
- **设备名称**:同时用作 DHCP 主机名与 mDNS 名(`<名称>.local`),用 Fing、路由器设备列表等 IP 扫描工具可直接看到设备名而非随机名;设备名称变更后需重启生效。
- 配置以 `key=value` 文本存于 LittleFS 根目录 `/cfg.txt`,启动时读取;`config.h` 作为出厂默认值兜底。
- **WiFi 变更 → 保存后自动重启生效**;其余参数即时生效,无需重启。
- **WiFi 连不上时自动开启配置热点 `AirQuality-Config`**(IP `192.168.4.1`):设备同时充当**捕获门户**——DNS 把所有域名解析到自身,手机连上热点后会自动弹出配置页;也可手动打开 `http://192.168.4.1`。

### 历史数据记录(30 天环形)
- 每 **1 分钟**在 LittleFS 中追加一条 12 字节二进制记录(时间戳、eCO₂、TVOC、温度、湿度)。
- 按**日历日**存放:`/log/YYYYMMDD.bin`。
- **环形覆盖**:开机后 + 每小时清理,只保留最近 **30 天**,超过自动删除最旧的。
- 仅在 NTP 时间已同步、且两个传感器均有有效读数时记录(避免预热期垃圾数据)。

### Web 历史曲线
- 点击仪表盘上任意数据卡片(温度 / 湿度 / eCO₂ / TVOC)弹出历史曲线。
- **图表类型**:时间序列**平滑折线图**(5 点滑动平均 + Catmull-Rom 样条平滑,去除传感器短时噪声与折角)+ 半透明面积填充,手写绘制在 HTML5 `<canvas>` 上,**无第三方图表库**。平滑仅用于绘图,`/export` 导出的 CSV 仍是原始数据。
- **坐标轴**:X 轴为时间(24H 显示 `HH:MM`,7D/30D 显示 `MM/DD`),Y 轴为数值(5 档刻度)。
- 数据由 `/history` 接口按时间分桶**平均降采样**(最多 1200 点;30 天视图聚合为约每 36 分钟 1 点),响应体积有上限。
- 支持 **24H / 7D / 30D** 切换,鼠标悬停显示十字线与该点时间/数值,附 min / avg / max / 数据点数统计。

### CSV 导出
- 仪表盘底部「⬇ 导出 CSV」:导出全部(保留的 30 天)数据。
- 历史曲线弹窗内「⬇ 导出该区间」:只导出当前选择的范围(24H/7D/30D)。
- CSV 格式:`timestamp_iso,timestamp_unix,eco2_ppm,tvoc_ppb,temp_c,hum_pct`

### 清除历史(需密码)
- 仪表盘底部「🗑 清除历史」按钮,输入密码 `1234` 后删除全部历史记录,并重置 min/max 统计。
- 密码在固件中定义为明文(`CLEAR_PASSWORD` 宏),仅用于防止误操作,不是安全机制。

### HTTP 接口

| 路径 | 说明 |
|------|------|
| `/` | 仪表盘页面 |
| `/data` | 实时数据 JSON(1 秒轮询) |
| `/ip` | 返回 IP |
| `/history?metric={eco2\|tvoc\|temp\|hum}&days={1..30}` | 历史曲线数据(自动降采样) |
| `/export?days={1..30}` | 下载 CSV(默认全部) |
| `/clear?pwd={密码}` | 清除全部历史记录(需密码,默认 `1234`) |
| `/display?state={on\|off}` | 关闭/开启 TFT 屏幕(面板睡眠/唤醒) |
| `/config` | 设备设置页面 |
| `/config/data` | 当前配置 JSON(回显设置表单) |
| `/config/save` | 保存配置(POST,URL 编码表单) |

---

## 依赖库

需要先在 Arduino IDE **库管理器** 中安装:

| 库 | 用于 |
|----|------|
| **Adafruit ST7735 and ST7789 Library** | ST7789 TFT 驱动(库管理器里搜 "ST7735") |
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

> 完整流程:复制 `config.h.example` 为 `config.h` 并填入 WiFi 账号密码 → 打开 `esp32-air-quality-monitor.ino` → 按上述 1–3 设置 → 上传 → 串口监视器(115200)确认 `OK: TFT ST7789`、`OK: LittleFS mounted` 和 IP 地址;ENS160 约 3 分钟预热后开始输出读数。

---

## ⚠️ 注意事项

- **No OTA 分区会去掉 OTA 升级槽位**——改为此方案后只能用串口烧录,不能 OTA。
- **首次上电会自动格式化 LittleFS**(`LittleFS.begin(true)`),数据分区为空白。
- ENS160 **预热约 3 分钟**,预热期间显示 `--`;历史记录要等预热完成 + NTP 同步后才开始写。
- **WiFi 断连或 NTP 未同步时不记录数据**(没有可靠时间戳),恢复后继续追加。
- 30 天数据约 **506 KB**(12 B/分钟),数据分区 1.875 MB,余量充足;如需更久可调大 `LOG_RETENTION_DAYS`。
- `/export` 与 `/history` 大范围查询是**同步阻塞**的:导出 30 天全量时仪表盘会短暂停顿几秒,属正常。
- **`config.h` 已被 .gitignore 忽略**,不要提交 WiFi 密码到仓库。
- **WiFi 密码与清除历史密码以明文存于 LittleFS `/cfg.txt`**,仅供局域网使用,不要当作真正的安全防护。
- **配置热点 `AirQuality-Config` 无密码**,仅在 WiFi 连不上时自动开启,用于从手机重新配置;请勿在公共环境长时间暴露。
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
- **页面打不开** → 确认 TFT 屏幕底部显示的 IP,浏览器访问 `http://<IP>`。
- **屏幕白屏/不亮** → 检查 SPI 接线(SCLK/MOSI/CS/DC/RST)与 `TFT_*` 引脚定义是否匹配;背光 BL 未接的话把 `TFT_BL` 设为 `-1`。
- **历史曲线没数据** → 需要先等 NTP 时间同步 + ENS160 预热完成,再等约 1 分钟产生第一条记录。
