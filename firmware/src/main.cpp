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
#include <U8g2lib.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include "secrets.h"

// ---------- 配置 ----------
#define MAC_HOST       "192.168.5.12"   // Mac 局域网 IP (ipconfig getifaddr en0)
#define MAC_PORT       8090
#define REFRESH_MS     (5UL * 60UL * 1000UL)  // 自动刷新间隔(与 Mac 端 5 分钟抓取同步)
#define HW_REFRESH_MS  (15UL * 1000UL)         // 硬件页刷新间隔(墨水屏建议 ≥10s)

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

#define CMD_HPM 0x38

#define BMP_W 400
#define BMP_H 300
#define BMP_BYTES (BMP_W * BMP_H / 8)  // 15000
#define ROW_BYTES (BMP_W / 8)          // 50

// ---------- 页面定义 (与 Mac 端 codex_eink.py 页序一致) ----------
const char *PAGE_NAMES[] = { "CODEX", "TRAE", "HW" };
const char *PAGE_PATHS[] = { "/bitmap?page=0", "/bitmap?page=1", "/bitmap?page=2" };
#define PAGE_COUNT 3

static U8G2_ST7305_300X400_F_4W_HW_SPI u8g2(ROTATION, RLCD_CS, RLCD_DC, RLCD_RST);
static uint8_t bmpBuf[BMP_BYTES];
static int curPage = 0;
static bool wifiOk = false;

#define LOG(fmt, ...) do { Serial.printf("[ce] " fmt "\n", ##__VA_ARGS__); } while (0)

static void applyInvert() {
#if INVERT
  u8g2_t *u = u8g2.getU8g2();
  uint8_t *fb = u8g2_GetBufferPtr(u);
  size_t n = u8g2_GetBufferSize(u);
  for (size_t i = 0; i < n; i++) fb[i] ^= 0xFF;
#endif
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

  SPI.begin(RLCD_SCK, -1, RLCD_MOSI, RLCD_CS);
  u8g2.initDisplay();
  u8g2.clearDisplay();
  u8g2.setPowerSave(0);
  u8g2.setBusClock(24000000);
  u8g2.sendF("c", CMD_HPM);
  LOG("display init done");

  refreshPage(curPage);
}

void loop() {
  // KEY 键: 按下松开后切换页面并立即拉取
  if (digitalRead(KEY_BTN) == LOW) {
    while (digitalRead(KEY_BTN) == LOW) delay(20);
    curPage = (curPage + 1) % PAGE_COUNT;
    LOG("key: switch to page %d (%s)", curPage, PAGE_NAMES[curPage]);
    refreshPage(curPage);
  }
  // BOOT 键: 按下松开后立即刷新当前页
  if (digitalRead(BOOT_BTN) == LOW) {
    while (digitalRead(BOOT_BTN) == LOW) delay(20);
    LOG("boot key: refresh page %d", curPage);
    refreshPage(curPage);
    return;
  }
  // 定时刷新当前页 (硬件页用更短间隔)
  static uint32_t last = 0;
  uint32_t interval = (curPage == 2) ? HW_REFRESH_MS : REFRESH_MS;
  if (millis() - last >= interval) {
    last = millis();
    refreshPage(curPage);
  }
  delay(50);
}
