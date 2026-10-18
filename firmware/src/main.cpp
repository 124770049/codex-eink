/**
 * codex-eink ESP32 固件 · 微雪 ESP32-S3-RLCD-4.2 (ST7305, 300x400 全反射屏)
 *
 * 常驻模式(USB 供电): 启动 -> 连 WiFi (保持常连) -> 拉取当前页位图并绘制
 *   - KEY (GPIO18): 切换到下一页 (Codex / TRAE / HW) 并立即拉取
 *   - BOOT (GPIO0): 立即刷新当前页
 *   - 普通页每 5 分钟自动刷新, 硬件页每 15 秒刷新
 *
 * 实测记录(勿回退):
 * - u8g2 drawBitmap 在 ST7305 上输出空白, 必须逐位 drawPixel
 * - 面板极性: RAM 1=黑 0=白, 不取反即为白底黑字
 * - VSHP 源极电压: 保持默认 0x3C (4.8V) (u8x8_d_st7305.c), 接电池省电; 曾试过 0x44(5.28V) 提对比度但回退
 * - LPM(0x39) 会使面板丢内容, 保持 HPM
 * - 深睡时引脚悬空会复位面板清屏, 故不深睡(桌面 USB 供电)
 * - esptool 烧录后可能停在下载模式, 断电重插即可运行
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include "secrets.h"

// ---------- 配置 ----------
#define MAC_HOST       "192.168.5.12"   // Mac 局域网 IP (ipconfig getifaddr en0)
#define MAC_PORT       8090
#define REFRESH_MS     (5UL * 60UL * 1000UL)  // 自动刷新间隔(与 Mac 端 5 分钟抓取同步)
#define HW_REFRESH_MS  (15UL * 1000UL)         // 硬件页刷新间隔(墨水屏建议 ≥10s)
#define AUTO_SWITCH_MS (30UL * 1000UL)         // 自动轮播切换间隔(保护按键寿命)

#define ROTATION       U8G2_R1          // R1: 300x400 竖屏 -> 400x300 横屏
#define INVERT         0                // 实测 RAM 1=黑 0=白: 不取反即为白底黑字(Mac 位图 1=黑)

// ---------- 引脚 (ESP32-S3-RLCD-4.2 固定) ----------
#define RLCD_SCK  11
#define RLCD_MOSI 12
#define RLCD_DC   5
#define RLCD_CS   40
#define RLCD_RST  41
#define BOOT_BTN  0                    // BOOT 键: 立即刷新当前页
#define KEY_BTN   18                   // KEY 键: 切换页面(低电平有效, 微雪文档)
#define BATT_PIN  4                    // 电池电压检测 (GPIO4, 分压 1:3, Vbat=Vadc*3) 微雪官方
#define I2C_SDA   13                   // I2C 数据 (SHTC3 / PCF85063)
#define I2C_SCL   14                   // I2C 时钟
#define SHTC3_ADDR 0x70                // 温湿度传感器
#define RTC_ADDR   0x51                // PCF85063 实时时钟

#define CMD_HPM 0x38

#define BMP_W 400
#define BMP_H 300
#define BMP_BYTES (BMP_W * BMP_H / 8)  // 15000
#define ROW_BYTES (BMP_W / 8)          // 50

// ---------- 页面定义 (与 Mac 端 codex_eink.py 页序一致) ----------
// 前 3 页从 Mac 拉取位图, 第 4 页为本地环境页(时间/温湿度/WiFi)
const char *PAGE_NAMES[] = { "CODEX", "TRAE", "HW", "ENV" };
const char *PAGE_PATHS[] = { "/bitmap?page=0", "/bitmap?page=1", "/bitmap?page=2", "" };
#define PAGE_COUNT 4
#define PAGE_ENV  3   // 本地环境页索引

static U8G2_ST7305_300X400_F_4W_HW_SPI u8g2(ROTATION, RLCD_CS, RLCD_DC, RLCD_RST);
static uint8_t bmpBuf[BMP_BYTES];
static int curPage = 0;
static bool wifiOk = false;
static uint32_t lastRefresh = 0;   // 全屏刷新计时
static uint32_t lastSwitch = 0;    // 自动轮播计时

// 前向声明
static bool wifiConnect();
static bool syncNtpToRtc();

#define LOG(fmt, ...) do { Serial.printf("[ce] " fmt "\n", ##__VA_ARGS__); } while (0)

static void applyInvert() {
#if INVERT
  u8g2_t *u = u8g2.getU8g2();
  uint8_t *fb = u8g2_GetBufferPtr(u);
  size_t n = u8g2_GetBufferSize(u);
  for (size_t i = 0; i < n; i++) fb[i] ^= 0xFF;
#endif
}

// 读取电池电压 (V), 分压比 1:3 (Vadc = Vbat / 3, 故 Vbat = Vadc * 3) 微雪官方
static float readBatteryVoltage() {
  analogReadResolution(12);
  analogSetPinAttenuation(BATT_PIN, ADC_11db);
  uint32_t sum = 0;
  const int N = 8;
  for (int i = 0; i < N; i++) {
    sum += analogReadMilliVolts(BATT_PIN);
    delay(2);
  }
  float mv = (float)sum / N;
  return mv * 3.0f / 1000.0f;  // 分压 1:3 -> 实际电压
}

// LiPo 电量百分比 (官方校准: 2.5V->0%, 4.2V->100%)
static int batteryPercent(float v) {
  if (v >= 4.20f) return 100;
  if (v <= 2.50f) return 0;
  return (int)((v - 2.50f) / (4.20f - 2.50f) * 100.0f);
}

// 检测是否在充电: 优先用 USB 物理连接检测 (Serial.isPlugged 检测 VBUS),
// 其次用电压辅助判断 (插墙充时电压被充电器拉到 ~4.2V)
static bool isCharging(float vbat) {
  if (Serial.isPlugged()) return true;   // USB 物理连接 -> 充电中
  if (vbat >= 4.18f) return true;       // 电压被充电器顶高 -> 充电中
  return false;
}

// 在 u8g2 缓冲区顶部中间绘制电池图标 + 百分比 + 充电状态
// (在 showBitmap 复制完位图后、sendBuffer 前调用, 叠加在位图之上)
static void drawBatteryIndicator(u8g2_t *u) {
  float v = readBatteryVoltage();
  int pct = batteryPercent(v);
  bool charging = isCharging(v);

  // 电池图标尺寸
  const int BW = 34;   // 电池主体宽
  const int BH = 14;   // 电池主体高
  const int NW = 3;    // 极耳宽
  const int NH = 6;    // 极耳高

  // 布局: [电池] [百分比] [充电中?]
  char pctStr[8];
  snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
  const char *chgStr = charging ? "充电中" : "";
  // 百分比用 7x13 字体, 中文用文泉驿 12px 字体
  u8g2_SetFont(u, u8g2_font_7x13_tf);
  int tw = u8g2_GetStrWidth(u, pctStr);
  int cw = 0;
  if (charging) {
    u8g2_SetFont(u, u8g2_font_wqy12_t_gb2312);
    cw = u8g2_GetUTF8Width(u, chgStr);
  }
  int gap = 5;
  int totalW = BW + NW + gap + tw + (charging ? gap + cw : 0);
  int x0 = (BMP_W - totalW) / 2;   // 水平居中
  int y0 = 7;                       // 顶部垂直位置

  // 先清空该区域 (用白色覆盖位图), 避免与页面内容重叠
  u8g2_SetDrawColor(u, 0);
  u8g2_DrawBox(u, x0 - 2, y0 - 2, totalW + 4, BH + 4);
  u8g2_SetDrawColor(u, 1);

  // 电池极耳 (右侧小凸起)
  u8g2_DrawBox(u, x0 + BW, y0 + (BH - NH) / 2, NW, NH);
  // 电池主体边框
  u8g2_DrawFrame(u, x0, y0, BW, BH);

  // 电池内部填充 (留 2px 边距)
  int innerW = BW - 4;
  int innerH = BH - 4;
  int fillW = (innerW * pct) / 100;
  if (fillW > 0) {
    u8g2_DrawBox(u, x0 + 2, y0 + 2, fillW, innerH);
  }

  // 百分比文本
  u8g2_SetFont(u, u8g2_font_7x13_tf);
  int tx = x0 + BW + NW + gap;
  u8g2_DrawStr(u, tx, y0 + BH - 1, pctStr);

  // 充电中文字 (充电时显示在百分比右侧, 用中文字体, 必须用 UTF8 版本)
  if (charging) {
    u8g2_SetFont(u, u8g2_font_wqy12_t_gb2312);
    u8g2_DrawUTF8(u, tx + tw + gap, y0 + BH - 1, chgStr);
  }

  LOG("battery: %.2fV %d%% %s (usb=%d)", v, pct, charging ? "CHG" : "BAT", Serial.isPlugged());
}

// ---------- SHTC3 温湿度 ----------
// 返回 true 成功, 温度(°C)和湿度(%)写入出参
static bool readSHTC3(float &tempC, float &humPct) {
  // 唤醒 SHTC3 (默认休眠)
  Wire.beginTransmission(SHTC3_ADDR);
  Wire.write(0x35);
  Wire.write(0x17);
  uint8_t wake = Wire.endTransmission();
  if (wake != 0) {
    LOG("shtc3: wake fail err=%d", wake);
    return false;
  }
  delay(1);  // 唤醒后等待

  // 高精度测量, clock stretching: 0x7CA2 (用完整 stop, 不用 repeated start)
  Wire.beginTransmission(SHTC3_ADDR);
  Wire.write(0x7C);
  Wire.write(0xA2);
  uint8_t cmd = Wire.endTransmission();
  if (cmd != 0) {
    LOG("shtc3: cmd fail err=%d", cmd);
    return false;
  }
  delay(15);  // 等待测量完成 (高精度模式最长 15ms)

  uint8_t buf[6];
  uint8_t got = Wire.requestFrom((int)SHTC3_ADDR, 6);
  if (got != 6) {
    LOG("shtc3: short read got=%d", got);
    return false;
  }
  for (int i = 0; i < 6; i++) buf[i] = Wire.read();

  uint16_t rawT = (buf[0] << 8) | buf[1];
  uint16_t rawH = (buf[3] << 8) | buf[4];
  tempC = -45.0f + 175.0f * (rawT / 65535.0f);
  humPct = 100.0f * (rawH / 65535.0f);
  return true;
}

// ---------- PCF85063 RTC ----------
struct RtcTime {
  int year, month, day, weekday, hour, minute, second;
  bool valid;  // OS 标志位为 0 表示时间有效
};

static uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static RtcTime readRTC() {
  RtcTime t = {0, 0, 0, 0, 0, 0, 0, false};
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);  // 秒寄存器起始地址
  uint8_t err = Wire.endTransmission(false);
  if (err != 0) {
    LOG("rtc: addr fail err=%d", err);
    return t;
  }
  uint8_t got = Wire.requestFrom((int)RTC_ADDR, 7);
  if (got != 7) {
    LOG("rtc: short read got=%d", got);
    return t;
  }
  uint8_t b[7];
  for (int i = 0; i < 7; i++) b[i] = Wire.read();
  t.second  = bcd2dec(b[0] & 0x7F);
  t.minute  = bcd2dec(b[1] & 0x7F);
  t.hour    = bcd2dec(b[2] & 0x3F);
  t.day     = bcd2dec(b[3] & 0x3F);
  t.weekday = b[4] & 0x07;
  t.month   = bcd2dec(b[5] & 0x1F);
  t.year    = 2000 + bcd2dec(b[6]);
  t.valid   = !(b[0] & 0x80);  // OS=0 有效
  return t;
}

// ---------- 本地环境页面渲染 ----------
static void drawEnvPage() {
  // 确保 WiFi 连接以同步 NTP 时间 (非阻塞, 失败也不影响显示)
  wifiConnect();

  u8g2_t *u = u8g2.getU8g2();
  u8g2.clearBuffer();
  u8g2_SetDrawColor(u, 1);

  // 读取传感器数据
  float temp = 0, hum = 0;
  bool shtOk = readSHTC3(temp, hum);
  RtcTime rt = readRTC();

  // ---- 标题栏 ----
  u8g2_SetFont(u, u8g2_font_9x15B_tf);
  u8g2_DrawStr(u, 12, 24, "ENVIRONMENT");
  u8g2_DrawLine(u, 12, 30, 388, 30);

  // ---- 大号时间 ----
  char timeStr[16];
  if (rt.valid) {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", rt.hour, rt.minute, rt.second);
  } else {
    snprintf(timeStr, sizeof(timeStr), "--:--:--");
  }
  u8g2_SetFont(u, u8g2_font_inb38_mn);  // 大号数字字体
  int tw = u8g2_GetStrWidth(u, timeStr);
  u8g2_DrawStr(u, (BMP_W - tw) / 2, 88, timeStr);

  // ---- 日期 + 星期 ----
  const char *wknames[] = { "日", "一", "二", "三", "四", "五", "六" };
  char dateStr[32];
  if (rt.valid) {
    snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d  星期%s",
             rt.year, rt.month, rt.day, wknames[rt.weekday % 7]);
  } else {
    snprintf(dateStr, sizeof(dateStr), "RTC 未同步 (OS=1)");
  }
  u8g2_SetFont(u, u8g2_font_wqy12_t_gb2312);
  int dw = u8g2_GetUTF8Width(u, dateStr);
  u8g2_DrawUTF8(u, (BMP_W - dw) / 2, 112, dateStr);

  // ---- 温湿度卡片 ----
  int cardY = 138;
  int cardH = 90;
  int cardW = 170;
  int gapX = 16;

  // 温度卡片
  int tx = 20;
  u8g2_DrawFrame(u, tx, cardY, cardW, cardH);
  u8g2_SetFont(u, u8g2_font_wqy12_t_gb2312);
  u8g2_DrawUTF8(u, tx + 10, cardY + 20, "温度");
  char tStr[24];
  if (shtOk) snprintf(tStr, sizeof(tStr), "%.1f°C", temp);
  else       snprintf(tStr, sizeof(tStr), "--.-°C");
  u8g2_SetFont(u, u8g2_font_inb24_mn);
  u8g2_DrawStr(u, tx + 10, cardY + 62, tStr);

  // 湿度卡片
  int hx = tx + cardW + gapX;
  u8g2_DrawFrame(u, hx, cardY, cardW, cardH);
  u8g2_SetFont(u, u8g2_font_wqy12_t_gb2312);
  u8g2_DrawUTF8(u, hx + 10, cardY + 20, "湿度");
  char hStr[24];
  if (shtOk) snprintf(hStr, sizeof(hStr), "%.0f%%", hum);
  else       snprintf(hStr, sizeof(hStr), "--%%");
  u8g2_SetFont(u, u8g2_font_inb24_mn);
  u8g2_DrawStr(u, hx + 10, cardY + 62, hStr);

  // ---- 底部信息栏 ----
  int by = 252;
  u8g2_SetFont(u, u8g2_font_7x13_tf);

  // WiFi 信号
  char wifiStr[40];
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(wifiStr, sizeof(wifiStr), "WiFi:%s  %ddBm",
             WiFi.SSID().c_str(), WiFi.RSSI());
  } else {
    snprintf(wifiStr, sizeof(wifiStr), "WiFi: 未连接");
  }
  u8g2_DrawStr(u, 12, by, wifiStr);

  // IP 地址
  char ipStr[40];
  snprintf(ipStr, sizeof(ipStr), "IP:%s",
           WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "---");
  u8g2_DrawStr(u, 12, by + 16, ipStr);

  // 运行时长 (右侧)
  uint32_t up = millis() / 1000;
  int dd = up / 86400, hh = (up % 86400) / 3600, mm = (up % 3600) / 60;
  char upStr[32];
  snprintf(upStr, sizeof(upStr), "up %dd %02d:%02d", dd, hh, mm);
  int uw = u8g2_GetStrWidth(u, upStr);
  u8g2_DrawStr(u, BMP_W - uw - 12, by, upStr);

  // 芯片温度 (右下, 用英文避免 ASCII 字体中文乱码)
  float ct = temperatureRead();
  char ctStr[24];
  snprintf(ctStr, sizeof(ctStr), "Chip %.0fC", ct);
  int cw = u8g2_GetStrWidth(u, ctStr);
  u8g2_DrawStr(u, BMP_W - cw - 12, by + 16, ctStr);

  // 叠加电池电量 (顶部居中, 覆盖在标题下方)
  drawBatteryIndicator(u);

  applyInvert();
  u8g2.sendBuffer();

  LOG("env: t=%.1fC h=%.0f%% rtc=%d chip=%.0fC", temp, hum, rt.valid, ct);
}

// 环境页时间区域 tile 行范围 (y≈48~96, 覆盖 38px 大号数字)
#define ENV_TIME_TILE_START 6
#define ENV_TIME_TILE_END   11

// 仅刷新环境页的时间数字 (局部 tile 更新, 避免全屏闪烁和慢刷新)
static void updateEnvTimePartial() {
  u8g2_t *u = u8g2.getU8g2();
  u8x8_t *u8x8 = u8g2_GetU8x8(u);

  RtcTime rt = readRTC();
  char timeStr[16];
  if (rt.valid) {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", rt.hour, rt.minute, rt.second);
  } else {
    snprintf(timeStr, sizeof(timeStr), "--:--:--");
  }

  // 仅清除时间条带 (y=48..96), 保留标题/日期/卡片等其他区域
  u8g2_SetDrawColor(u, 0);
  u8g2_DrawBox(u, 0, 48, BMP_W, 48);
  u8g2_SetDrawColor(u, 1);

  u8g2_SetFont(u, u8g2_font_inb38_mn);
  int tw = u8g2_GetStrWidth(u, timeStr);
  u8g2_DrawStr(u, (BMP_W - tw) / 2, 88, timeStr);

  // 只发送时间区域的 tile 行到 ST7305
  uint8_t w = u8x8->display_info->tile_width;
  uint8_t *buf = u8g2_GetBufferPtr(u);
  for (int row = ENV_TIME_TILE_START; row <= ENV_TIME_TILE_END; row++) {
    uint8_t *ptr = buf + (uint32_t)row * w * 8;
    u8x8_DrawTile(u8x8, 0, row, w, ptr);
  }
}

static void showBitmap() {
  // 逐位画点(Mac 位图: MSB first, 1=黑), 绕开 drawBitmap 在 ST7305 上的问题
  u8g2_t *u = u8g2.getU8g2();
  u8g2.clearBuffer();
  u8g2_SetDrawColor(u, 1);
  for (int row = 0; row < BMP_H; row++) {
    const uint8_t *line = bmpBuf + row * ROW_BYTES;
    for (int cb = 0; cb < ROW_BYTES; cb++) {
      uint8_t v = line[cb];
      if (!v) continue;
      int x0 = cb * 8;
      for (int b = 0; b < 8; b++)
        if (v & (0x80 >> b)) u8g2_DrawPixel(u, x0 + b, row);
    }
  }
  // 叠加电池电量指示 (顶部居中)
  drawBatteryIndicator(u);
  applyInvert();
  u8g2.sendBuffer();
}

// 过渡/错误画面
static void splash(const char *line) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_9x15B_tf);
  u8g2.drawStr(20, 60, "QUOTA  EINK");
  u8g2.drawLine(20, 70, 380, 70);
  u8g2.setFont(u8g2_font_9x15_tf);
  u8g2.drawStr(20, 110, line);
  drawBatteryIndicator(u8g2.getU8g2());
  applyInvert();
  u8g2.sendBuffer();
}

static bool wifiConnect() {
  if (wifiOk && WiFi.status() == WL_CONNECTED) return true;
  LOG("wifi: connecting to %s", WIFI_SSID);
  splash("connecting wifi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > 15000) {
      LOG("wifi: TIMEOUT rssi=%d", WiFi.RSSI());
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_OFF);
      wifiOk = false;
      return false;
    }
    delay(50);
  }
  wifiOk = true;
  LOG("wifi: connected, ip=%s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // 首次连接成功后, 用 NTP 校准 RTC
  static bool ntpSynced = false;
  if (!ntpSynced) {
    ntpSynced = syncNtpToRtc();
  }
  return true;
}

// NTP 同步时间并写入 RTC (WiFi 已连接时调用)
static bool syncNtpToRtc() {
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");  // CST(UTC+8)
  struct tm tinfo;
  uint32_t t0 = millis();
  while (!getLocalTime(&tinfo, 0)) {
    if (millis() - t0 > 5000) {
      LOG("ntp: timeout");
      return false;
    }
    delay(100);
  }
  // 写入 PCF85063 (BCD 编码)
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);  // 秒寄存器
  Wire.write(((tinfo.tm_sec / 10) << 4) | (tinfo.tm_sec % 10));
  Wire.write(((tinfo.tm_min / 10) << 4) | (tinfo.tm_min % 10));
  Wire.write(((tinfo.tm_hour / 10) << 4) | (tinfo.tm_hour % 10));
  Wire.write(((tinfo.tm_mday / 10) << 4) | (tinfo.tm_mday % 10));
  Wire.write(tinfo.tm_wday & 0x07);
  Wire.write((((tinfo.tm_mon + 1) / 10) << 4) | ((tinfo.tm_mon + 1) % 10));
  Wire.write((((tinfo.tm_year % 100) / 10) << 4) | ((tinfo.tm_year % 100) % 10));
  Wire.endTransmission();
  LOG("ntp: synced %04d-%02d-%02d %02d:%02d:%02d -> RTC",
      tinfo.tm_year + 1900, tinfo.tm_mon + 1, tinfo.tm_mday,
      tinfo.tm_hour, tinfo.tm_min, tinfo.tm_sec);
  return true;
}

// 拉取当前页位图; WiFi 保持常连
static bool fetchBitmap(int page, char *err, size_t errLen) {
  if (!wifiConnect()) {
    snprintf(err, errLen, "wifi connect fail");
    return false;
  }

  splash("fetching...");
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(10000);
  bool ok = false;

  LOG("http: GET %s:%d%s", MAC_HOST, MAC_PORT, PAGE_PATHS[page]);
  if (!http.begin(String("http://") + MAC_HOST + ":" + MAC_PORT + PAGE_PATHS[page])) {
    snprintf(err, errLen, "HTTP begin fail");
    LOG("http: begin fail");
  } else {
    int code = http.GET();
    LOG("http: status %d", code);
    if (code != 200) {
      snprintf(err, errLen, "HTTP %d from %s", code, MAC_HOST);
    } else {
      int len = http.getSize();
      LOG("http: len %d", len);
      if (len != BMP_BYTES) {
        snprintf(err, errLen, "size %d != %d", len, BMP_BYTES);
      } else {
        WiFiClient *s = http.getStreamPtr();
        size_t got = 0;
        while (got < BMP_BYTES) {
          int n = s->readBytes(bmpBuf + got, BMP_BYTES - got);
          if (n <= 0) break;
          got += n;
        }
        LOG("http: read %u/%d", (unsigned)got, BMP_BYTES);
        if (got != BMP_BYTES) {
          snprintf(err, errLen, "short read %u/%d", (unsigned)got, BMP_BYTES);
        } else {
          ok = true;
        }
      }
    }
    http.end();
  }
  return ok;
}

static void refreshPage(int page) {
  // 本地环境页: 直接渲染, 不拉取位图
  if (page == PAGE_ENV) {
    drawEnvPage();
    return;
  }

  splash("loading page...");
  char err[96] = "";
  bool ok = fetchBitmap(page, err, sizeof(err));

  if (ok) {
    showBitmap();
    LOG("page %d (%s) drawn", page, PAGE_NAMES[page]);
  } else {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_9x15B_tf);
    u8g2.drawStr(20, 60, "QUOTA  EINK");
    u8g2.drawLine(20, 70, 380, 70);
    u8g2.setFont(u8g2_font_9x15_tf);
    char line1[48];
    snprintf(line1, sizeof(line1), "page %d (%s) failed:", page, PAGE_NAMES[page]);
    u8g2.drawStr(20, 105, line1);
    u8g2.drawStr(20, 130, err);
    u8g2.setFont(u8g2_font_7x13_tf);
    u8g2.drawStr(20, 260, "KEY: next page   BOOT: retry");
    drawBatteryIndicator(u8g2.getU8g2());
    applyInvert();
    u8g2.sendBuffer();
    LOG("error screen: %s", err);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // 等 USB CDC 主机连上, 保证日志可见
  LOG("boot");

  pinMode(BOOT_BTN, INPUT_PULLUP);
  pinMode(KEY_BTN, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);   // SHTC3 + PCF85063
  Wire.setClock(100000);
  Wire.setTimeOut(50);

  SPI.begin(RLCD_SCK, -1, RLCD_MOSI, RLCD_CS);
  u8g2.initDisplay();
  u8g2.clearDisplay();
  u8g2.setPowerSave(0);
  u8g2.setBusClock(24000000);
  u8g2.sendF("c", CMD_HPM);
  LOG("display init done");

  refreshPage(curPage);
  lastRefresh = millis();
  lastSwitch = millis();
}

void loop() {
  static uint32_t lastTimeTick = 0;    // 环境页时间局部刷新计时

  // KEY 键: 手动切换页面 (保留作为备用, 平时靠自动轮播)
  if (digitalRead(KEY_BTN) == LOW) {
    while (digitalRead(KEY_BTN) == LOW) delay(20);
    curPage = (curPage + 1) % PAGE_COUNT;
    LOG("key: switch to page %d (%s)", curPage, PAGE_NAMES[curPage]);
    refreshPage(curPage);
    lastSwitch = millis();   // 手动切换后重置自动轮播计时
    lastRefresh = millis();  // 同时重置全屏刷新计时
  }
  // BOOT 键: 立即刷新当前页
  if (digitalRead(BOOT_BTN) == LOW) {
    while (digitalRead(BOOT_BTN) == LOW) delay(20);
    LOG("boot key: refresh page %d", curPage);
    refreshPage(curPage);
    lastRefresh = millis();
    return;
  }

  // 自动轮播: 每隔 AUTO_SWITCH_MS 切换到下一页 (保护按键寿命)
  if (millis() - lastSwitch >= AUTO_SWITCH_MS) {
    lastSwitch = millis();
    curPage = (curPage + 1) % PAGE_COUNT;
    LOG("auto: switch to page %d (%s)", curPage, PAGE_NAMES[curPage]);
    refreshPage(curPage);
    lastRefresh = millis();  // 切页即刷新, 重置刷新计时
  }

  // 环境页: 每秒局部刷新时间数字 (快速, 不闪屏)
  if (curPage == PAGE_ENV && millis() - lastTimeTick >= 1000) {
    lastTimeTick = millis();
    updateEnvTimePartial();
  }

  // 定时全屏刷新当前页 (仅在当前页停留超过间隔时刷新)
  uint32_t interval;
  if (curPage == 2)             interval = HW_REFRESH_MS;
  else if (curPage == PAGE_ENV) interval = 60UL * 1000UL;  // 环境页 60s 全屏刷新
  else                          interval = REFRESH_MS;
  if (millis() - lastRefresh >= interval) {
    lastRefresh = millis();
    refreshPage(curPage);
  }
  delay(50);
}
