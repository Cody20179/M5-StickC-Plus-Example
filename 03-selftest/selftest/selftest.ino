/*
 * M5StickC Plus - Hardware self test
 *
 *   BtnA (front) : next page
 *   BtnB (side)  : run the action on the current page
 *
 *   Page 0  I2C SCAN   internal bus (21/22) + Grove bus (32/33)
 *   Page 1  SYSTEM     chip / flash / heap / MAC / reset reason
 *   Page 2  WIFI       BtnB = scan, shows strongest APs
 *   Page 3  MIC        live level from the SPM1423 PDM microphone
 *   Page 4  OUTPUTS    BtnB = buzzer + red LED + IR LED
 */
#include <M5StickCPlus.h>
#include <WiFi.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <esp_system.h>
#include <math.h>

#define SCR_W 240
#define SCR_H 135

#define C_BG    0x0000
#define C_DIM   0x8410
#define C_LABEL 0x7BEF
#define C_TEXT  0xFFFF
#define C_OK    0x07E0
#define C_WARN  0xFFE0
#define C_FAIL  0xF800
#define C_ACC   0x07FF
#define C_TITLE 0xFD20

#define PIN_LED    10  // red LED, active low
#define PIN_IR      9  // IR transmit LED
#define PIN_BUZZER  2  // passive buzzer
#define PIN_MIC_CLK  0
#define PIN_MIC_DATA 34

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

const uint8_t PAGE_COUNT = 5;
uint8_t page = 0;

// ---- I2C scan results ----
uint8_t intAddr[16], intCount = 0;
uint8_t extAddr[16], extCount = 0;

// ---- WiFi scan results ----
int      wifiCount = -1;   // -1 = not scanned yet
String   wifiSsid[4];
int32_t  wifiRssi[4];
uint8_t  wifiShown = 0;

// ---- microphone ----
I2SClass i2s;
bool  micOk      = false;
float micRms     = 0.0f;
float micPeak    = 0.0f;
int16_t micBuf[512];

// ---- output test state ----
String outStatus = "press BtnB";

uint32_t lastDrawMs = 0;

// WiFi.macAddress() returns all zeros until the WiFi driver starts,
// so read it straight out of the efuse instead.
String macStr() {
  uint64_t m = ESP.getEfuseMac();   // byte-reversed
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           (uint8_t)(m >> 0), (uint8_t)(m >> 8),  (uint8_t)(m >> 16),
           (uint8_t)(m >> 24), (uint8_t)(m >> 32), (uint8_t)(m >> 40));
  return String(buf);
}

const char *i2cName(uint8_t addr) {
  switch (addr) {
    case 0x34: return "AXP192 PMU";
    case 0x51: return "BM8563 RTC";
    case 0x68: return "MPU6886 IMU";
    case 0x44: return "SHT3x/SHT4x";
    case 0x70: return "SHT4x/mux";
    case 0x76: return "BMP/QMP688x";
    case 0x77: return "BMP280/BME280";
    default:   return "";
  }
}

void scanBus(TwoWire &bus, uint8_t *out, uint8_t &count) {
  count = 0;
  for (uint8_t a = 1; a < 127 && count < 16; a++) {
    bus.beginTransmission(a);
    if (bus.endTransmission() == 0) out[count++] = a;
  }
}

void doI2cScan() {
  Wire1.begin(21, 22, 100000);   // internal bus
  Wire.begin(32, 33, 100000);    // Grove / PORT.A
  scanBus(Wire1, intAddr, intCount);
  scanBus(Wire,  extAddr, extCount);

  Serial.printf("\n[I2C] internal (SDA21/SCL22): %d device(s)\n", intCount);
  for (uint8_t i = 0; i < intCount; i++)
    Serial.printf("      0x%02X  %s\n", intAddr[i], i2cName(intAddr[i]));
  Serial.printf("[I2C] Grove    (SDA32/SCL33): %d device(s)\n", extCount);
  for (uint8_t i = 0; i < extCount; i++)
    Serial.printf("      0x%02X  %s\n", extAddr[i], i2cName(extAddr[i]));
}

const char *resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXT PIN";
    case ESP_RST_SW:       return "SOFTWARE";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT WDT";
    case ESP_RST_TASK_WDT: return "TASK WDT";
    case ESP_RST_WDT:      return "OTHER WDT";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default:               return "UNKNOWN";
  }
}

void reportSystem() {
  Serial.println("\n[SYS]");
  Serial.printf("      chip        : %s rev %d, %d core(s) @ %d MHz\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("      flash       : %u bytes @ %u Hz\n",
                ESP.getFlashChipSize(), ESP.getFlashChipSpeed());
  Serial.printf("      sketch      : %u / %u bytes\n",
                ESP.getSketchSize(), ESP.getFreeSketchSpace());
  Serial.printf("      heap        : %u free / %u total, min %u\n",
                ESP.getFreeHeap(), ESP.getHeapSize(), ESP.getMinFreeHeap());
  Serial.printf("      psram       : %u bytes\n", ESP.getPsramSize());
  Serial.printf("      mac         : %s\n", macStr().c_str());
  Serial.printf("      reset reason: %s\n", resetReasonStr());
  Serial.printf("      sdk         : %s\n", ESP.getSdkVersion());
#ifdef SOC_TEMP_SENSOR_SUPPORTED
  Serial.printf("      soc temp    : %.1f C\n", temperatureRead());
#else
  Serial.println("      soc temp    : n/a (ESP32 classic has no usable sensor)");
#endif
}

void doWifiScan() {
  Serial.println("\n[WIFI] scanning...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  wifiCount = WiFi.scanNetworks();
  wifiShown = 0;
  for (int i = 0; i < wifiCount && wifiShown < 4; i++) {
    wifiSsid[wifiShown] = WiFi.SSID(i);
    if (wifiSsid[wifiShown].length() == 0) wifiSsid[wifiShown] = "<hidden>";
    wifiRssi[wifiShown] = WiFi.RSSI(i);
    wifiShown++;
  }
  Serial.printf("[WIFI] %d AP(s)\n", wifiCount);
  for (int i = 0; i < wifiCount; i++)
    Serial.printf("       %-24s %4d dBm  ch%2d\n",
                  WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  WiFi.scanDelete();
}

void initMic() {
  i2s.setPinsPdmRx(PIN_MIC_CLK, PIN_MIC_DATA);
  micOk = i2s.begin(I2S_MODE_PDM_RX, 16000,
                    I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  Serial.printf("\n[MIC] SPM1423 PDM init %s\n", micOk ? "OK" : "FAILED");
}

void sampleMic() {
  if (!micOk) return;
  size_t n = i2s.readBytes((char *)micBuf, sizeof(micBuf));
  size_t samples = n / sizeof(int16_t);
  if (samples == 0) return;

  double sum = 0, sumSq = 0;
  for (size_t i = 0; i < samples; i++) sum += micBuf[i];
  double mean = sum / samples;
  for (size_t i = 0; i < samples; i++) {
    double d = micBuf[i] - mean;
    sumSq += d * d;
  }
  micRms = sqrt(sumSq / samples);
  if (micRms > micPeak) micPeak = micRms;
  micPeak *= 0.995f;   // slow decay
}

/*
 * Do NOT use M5.Beep here.
 *
 * M5StickCPlus's SPEAKER::tone() calls ledcWriteTone(TONE_PIN_CHANNEL, f) with
 * TONE_PIN_CHANNEL == 0. On arduino-esp32 3.x ledcWriteTone() takes a PIN, not
 * a channel, so the library writes to GPIO0 while the buzzer sits on GPIO2 and
 * nothing is ever heard. SPEAKER::begin() also attaches at freq 0, which is
 * what prints "E ledc: freq_hz=0 duty_resolution=13" at boot.
 *
 * Drive the pin directly instead.
 */
void buzzerTone(uint32_t freq, uint32_t ms) {
  ledcAttach(PIN_BUZZER, freq, 10);
  ledcWriteTone(PIN_BUZZER, freq);   // 50% duty square wave
  delay(ms);
  ledcWriteTone(PIN_BUZZER, 0);
  ledcDetach(PIN_BUZZER);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);     // leave it idle low, not floating
}

void runBuzzerTest() {
  Serial.println("\n[OUT] buzzer sweep on GPIO2 (2k/3k/4k/5k Hz, 250ms each)...");
  outStatus = "buzzer...";
  drawPage();
  uint32_t freqs[] = {2000, 3000, 4000, 5000};
  for (int i = 0; i < 4; i++) {
    Serial.printf("      %lu Hz\n", freqs[i]);
    buzzerTone(freqs[i], 250);
    delay(120);
  }
  Serial.println("[OUT] buzzer done");
}

void runOutputTest() {
  runBuzzerTest();

  Serial.println("[OUT] red LED 5 blinks (GPIO10, active low)...");
  outStatus = "red LED...";
  drawPage();
  pinMode(PIN_LED, OUTPUT);
  for (int i = 0; i < 5; i++) {
    digitalWrite(PIN_LED, LOW);   // on
    delay(120);
    digitalWrite(PIN_LED, HIGH);  // off
    delay(120);
  }

  Serial.println("[OUT] IR LED 38kHz burst (GPIO9) - use a phone camera to see it...");
  outStatus = "IR burst...";
  drawPage();
  ledcAttach(PIN_IR, 38000, 8);
  for (int i = 0; i < 20; i++) {
    ledcWrite(PIN_IR, 128);   // 50% duty carrier
    delay(30);
    ledcWrite(PIN_IR, 0);
    delay(30);
  }
  ledcDetach(PIN_IR);
  pinMode(PIN_IR, OUTPUT);
  digitalWrite(PIN_IR, LOW);

  outStatus = "done - all fired";
  Serial.println("[OUT] done");
}

// ---------------- drawing ----------------

void header(const char *title) {
  spr.fillSprite(C_BG);
  spr.setTextDatum(TL_DATUM);
  spr.setTextFont(1);
  spr.setTextSize(2);
  spr.setTextColor(C_TITLE, C_BG);
  spr.drawString(title, 4, 3);

  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(String(page + 1) + "/" + String(PAGE_COUNT), 236, 8);
  spr.drawFastHLine(0, 22, SCR_W, C_DIM);
  spr.setTextDatum(TL_DATUM);
}

void footer(const char *hint) {
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.setTextDatum(BL_DATUM);
  spr.drawString(hint, 4, SCR_H - 2);
  spr.setTextDatum(TL_DATUM);
}

void pageI2c() {
  header("I2C SCAN");
  int y = 28;
  spr.setTextSize(1);

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("INTERNAL  SDA21 SCL22", 4, y); y += 12;
  for (uint8_t i = 0; i < intCount; i++) {
    spr.setTextColor(C_OK, C_BG);
    char line[40];
    snprintf(line, sizeof(line), "0x%02X  %s", intAddr[i], i2cName(intAddr[i]));
    spr.drawString(line, 12, y); y += 11;
  }
  if (intCount == 0) { spr.setTextColor(C_FAIL, C_BG); spr.drawString("none!", 12, y); y += 11; }

  y += 4;
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("GROVE     SDA32 SCL33", 4, y); y += 12;
  for (uint8_t i = 0; i < extCount; i++) {
    spr.setTextColor(C_OK, C_BG);
    char line[40];
    snprintf(line, sizeof(line), "0x%02X  %s", extAddr[i], i2cName(extAddr[i]));
    spr.drawString(line, 12, y); y += 11;
  }
  if (extCount == 0) {
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString("nothing plugged in", 12, y);
  }
  footer("BtnA next   BtnB rescan");
}

void pageSystem() {
  header("SYSTEM");
  int y = 28;
  spr.setTextSize(1);
  char l[48];

  spr.setTextColor(C_TEXT, C_BG);
  snprintf(l, sizeof(l), "%s rev%d  %dcore %dMHz",
           ESP.getChipModel(), ESP.getChipRevision(),
           ESP.getChipCores(), ESP.getCpuFreqMHz());
  spr.drawString(l, 4, y); y += 12;

  snprintf(l, sizeof(l), "flash  %u MB", ESP.getFlashChipSize() / (1024 * 1024));
  spr.drawString(l, 4, y); y += 12;

  snprintf(l, sizeof(l), "heap   %u / %u KB",
           ESP.getFreeHeap() / 1024, ESP.getHeapSize() / 1024);
  spr.drawString(l, 4, y); y += 12;

  snprintf(l, sizeof(l), "sketch %u KB", ESP.getSketchSize() / 1024);
  spr.drawString(l, 4, y); y += 12;

  spr.setTextColor(C_ACC, C_BG);
  spr.drawString(macStr(), 4, y); y += 12;

  spr.setTextColor(C_LABEL, C_BG);
  snprintf(l, sizeof(l), "reset  %s", resetReasonStr());
  spr.drawString(l, 4, y); y += 12;

  snprintf(l, sizeof(l), "uptime %lus", millis() / 1000);
  spr.drawString(l, 4, y);

  footer("BtnA next");
}

void pageWifi() {
  header("WIFI");
  int y = 30;
  spr.setTextSize(1);

  if (wifiCount < 0) {
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString("not scanned yet", 4, y);
  } else {
    spr.setTextColor(wifiCount > 0 ? C_OK : C_WARN, C_BG);
    spr.setTextSize(2);
    spr.drawString(String(wifiCount) + " AP", 4, y);
    spr.setTextSize(1);
    y += 22;
    for (uint8_t i = 0; i < wifiShown; i++) {
      spr.setTextColor(C_TEXT, C_BG);
      String s = wifiSsid[i];
      if (s.length() > 20) s = s.substring(0, 20);
      spr.drawString(s, 4, y);
      spr.setTextDatum(TR_DATUM);
      spr.setTextColor(wifiRssi[i] > -70 ? C_OK : C_WARN, C_BG);
      spr.drawString(String(wifiRssi[i]) + "dBm", 236, y);
      spr.setTextDatum(TL_DATUM);
      y += 12;
    }
  }
  footer("BtnA next   BtnB scan");
}

void pageMic() {
  header("MIC  SPM1423");
  int y = 28;
  spr.setTextSize(1);

  if (!micOk) {
    spr.setTextColor(C_FAIL, C_BG);
    spr.drawString("PDM init FAILED", 4, y);
    footer("BtnA next");
    return;
  }

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("live RMS level", 4, y); y += 14;

  spr.setTextColor(C_ACC, C_BG);
  spr.setTextSize(3);
  spr.drawString(String((int)micRms), 4, y);
  spr.setTextSize(1);

  // level bar, log-ish scale
  const float FS = 8000.0f;
  int bx = 4, by = 92, bw = SCR_W - 8, bh = 18;
  spr.drawRect(bx, by, bw, bh, C_DIM);
  int inner = bw - 4;
  int fill = (int)(inner * (micRms / FS));
  if (fill > inner) fill = inner;
  if (fill > 0) spr.fillRect(bx + 2, by + 2, fill, bh - 4, C_ACC);
  int pk = (int)(inner * (micPeak / FS));
  if (pk > inner) pk = inner;
  spr.drawFastVLine(bx + 2 + pk, by + 2, bh - 4, C_FAIL);

  footer("BtnA next   speak / tap to test");
}

void pageOutputs() {
  header("OUTPUTS");
  int y = 30;
  spr.setTextSize(1);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("buzzer   GPIO2", 4, y);  y += 13;
  spr.drawString("red LED  GPIO10", 4, y); y += 13;
  spr.drawString("IR LED   GPIO9", 4, y);  y += 18;

  spr.setTextColor(C_ACC, C_BG);
  spr.setTextSize(2);
  spr.drawString(outStatus, 4, y);
  spr.setTextSize(1);

  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("IR is invisible - point a", 4, 104);
  spr.drawString("phone camera at the top edge", 4, 115);

  footer("BtnA next   BtnB run");
}

void drawPage() {
  switch (page) {
    case 0: pageI2c();     break;
    case 1: pageSystem();  break;
    case 2: pageWifi();    break;
    case 3: pageMic();     break;
    case 4: pageOutputs(); break;
  }
  spr.pushSprite(0, 0);
}

void setup() {
  M5.begin();
  M5.Imu.Init();
  Serial.begin(115200);

  M5.Lcd.setRotation(3);
  M5.Axp.ScreenBreath(100);

  spr.setColorDepth(16);
  spr.createSprite(SCR_W, SCR_H);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);   // off
  pinMode(PIN_IR, OUTPUT);
  digitalWrite(PIN_IR, LOW);

  Serial.println("\n\n================================");
  Serial.println(" M5StickC Plus  hardware selftest");
  Serial.println("================================");

  doI2cScan();
  reportSystem();
  initMic();

  Serial.println("\n[RTC]");
  RTC_TimeTypeDef t; RTC_DateTypeDef d;
  M5.Rtc.GetTime(&t); M5.Rtc.GetDate(&d);
  Serial.printf("      %04d-%02d-%02d %02d:%02d:%02d\n",
                d.Year, d.Month, d.Date, t.Hours, t.Minutes, t.Seconds);

  Serial.println("\n[IMU]");
  float ax, ay, az, gx, gy, gz, tc;
  M5.Imu.getAccelData(&ax, &ay, &az);
  M5.Imu.getGyroData(&gx, &gy, &gz);
  M5.Imu.getTempData(&tc);
  Serial.printf("      accel %.2f %.2f %.2f g\n", ax, ay, az);
  Serial.printf("      gyro  %.1f %.1f %.1f dps\n", gx, gy, gz);
  Serial.printf("      temp  %.1f C (die temp, not ambient)\n", tc);

  Serial.println("\n[AXP192]");
  Serial.printf("      bat   %.2f V  %.0f mA\n",
                M5.Axp.GetBatVoltage(), M5.Axp.GetBatCurrent());
  Serial.printf("      vbus  %.2f V  %.0f mA\n",
                M5.Axp.GetVBusVoltage(), M5.Axp.GetVBusCurrent());
  Serial.printf("      axp   %.1f C internal\n", M5.Axp.GetTempInAXP192());

  Serial.println("\nBtnA = next page, BtnB = run action\n");

  drawPage();
}

// Same actions as the buttons, driven over the serial port so the whole
// self test can be run without touching the device.
//   n = next page   i = i2c rescan   w = wifi scan
//   o = output test m = mic reading  s = system report  ? = help
void handleSerialCommand(char c) {
  switch (c) {
    case 'n':
      page = (page + 1) % PAGE_COUNT;
      if (page == 4) outStatus = "press BtnB";
      Serial.printf("# page %d\n", page + 1);
      drawPage();
      break;
    case 'i': doI2cScan();     drawPage(); break;
    case 'w': doWifiScan();    drawPage(); break;
    case 'o': runOutputTest(); drawPage(); break;
    case 'b': runBuzzerTest();  drawPage(); break;
    case 's': reportSystem();  break;
    case 'm': {
      if (!micOk) { Serial.println("[MIC] not initialised"); break; }
      float peak = 0;
      for (int i = 0; i < 20; i++) { sampleMic(); if (micRms > peak) peak = micRms; }
      Serial.printf("[MIC] rms %.0f  peak over 20 blocks %.0f\n", micRms, peak);
      break;
    }
    case '?':
      Serial.println("# n=next page  i=i2c  w=wifi  o=outputs  b=buzzer  m=mic  s=system");
      break;
    default: break;
  }
}

void loop() {
  M5.update();

  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\r' && c != '\n') handleSerialCommand(c);
  }

  if (M5.BtnA.wasPressed()) {
    page = (page + 1) % PAGE_COUNT;
    if (page == 4) outStatus = "press BtnB";
    Serial.printf("# page %d\n", page + 1);
    drawPage();
  }

  if (M5.BtnB.wasPressed()) {
    switch (page) {
      case 0: doI2cScan();     break;
      case 2: doWifiScan();    break;
      case 4: runOutputTest(); break;
      default: break;
    }
    drawPage();
  }

  if (page == 3) sampleMic();

  uint32_t now = millis();
  if (now - lastDrawMs >= 100) {
    lastDrawMs = now;
    if (page == 1 || page == 3) drawPage();   // live pages
  }
}
