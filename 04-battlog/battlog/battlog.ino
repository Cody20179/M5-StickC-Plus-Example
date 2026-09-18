/*
 * M5StickC Plus - Dashboard + battery runtime logger
 *
 * The point of this build: you cannot measure battery life over USB, because
 * USB keeps charging the pack. So every sample is written to internal flash
 * (LittleFS, the 896 KB "spiffs" partition). Unplug, let it run flat, plug
 * back in, then dump the log over serial.
 *
 *   BtnA : reset vibration peak
 *   BtnB : cycle backlight brightness (recorded in the log - it dominates runtime)
 *
 * Serial commands:
 *   d  dump /batt.csv        c  clear the log
 *   s  status                +/-  brightness up/down
 */
#include <M5StickCPlus.h>
#include <LittleFS.h>
#include <math.h>

#define SCR_W 240
#define SCR_H 135

#define C_BG      0x0000
#define C_DIM     0x8410
#define C_LABEL   0x7BEF
#define C_TIME    0xFFFF
#define C_TEMP    0xFD20
#define C_BATT_OK 0x07E0
#define C_BATT_MD 0xFFE0
#define C_BATT_LO 0xF800
#define C_VIB     0x07FF
#define C_PEAK    0xF81F
#define C_LOG     0x07FF

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

const char *LOG_PATH = "/batt.csv";
const uint32_t LOG_PERIOD_MS = 10000;   // one sample every 10 s

bool     fsOk        = false;
uint32_t logRows     = 0;
uint32_t lastLogMs   = 0;
bool     usbPrev     = true;
uint32_t dischargeStartMs = 0;   // millis() when USB was removed
bool     discharging = false;

// ---- vibration ----
const uint32_t VIB_WINDOW_MS = 250;
float    gravityBaseline = 1.0f;
double   vibSumSq        = 0.0;
uint32_t vibCount        = 0;
uint32_t vibWindowStart  = 0;
float    vibRmsMg = 0.0f, vibPeakMg = 0.0f, vibBarMg = 0.0f;
const float VIB_FULLSCALE_MG = 500.0f;

uint32_t lastImuMs = 0, lastDrawMs = 0, lastSerialMs = 0;
const uint32_t IMU_PERIOD_MS = 5, DRAW_PERIOD_MS = 100, SERIAL_PERIOD_MS = 1000;

uint8_t brightnessIdx = 0;
const uint8_t BRIGHTNESS[] = {100, 75, 50, 25};

float batPercent(float v) {
  float p = (v - 3.10f) / (4.18f - 3.10f) * 100.0f;
  if (p < 0)   p = 0;
  if (p > 100) p = 100;
  return p;
}

void seedRtcFromBuildTime() {
  RTC_DateTypeDef d;
  M5.Rtc.GetDate(&d);
  if (d.Year >= 2024 && d.Year <= 2099) return;

  const char *mon[] = {"Jan","Feb","Mar","Apr","May","Jun",
                       "Jul","Aug","Sep","Oct","Nov","Dec"};
  char mstr[4] = {0};
  int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
  sscanf(__DATE__, "%3s %d %d", mstr, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
  uint8_t month = 1;
  for (uint8_t i = 0; i < 12; i++)
    if (strncmp(mstr, mon[i], 3) == 0) { month = i + 1; break; }

  RTC_DateTypeDef nd; nd.WeekDay = 0; nd.Month = month;
  nd.Date = (uint8_t)day; nd.Year = (uint16_t)year;
  RTC_TimeTypeDef nt; nt.Hours = (uint8_t)hh; nt.Minutes = (uint8_t)mm; nt.Seconds = (uint8_t)ss;
  M5.Rtc.SetDate(&nd);
  M5.Rtc.SetTime(&nt);
}

String rtcStamp() {
  RTC_TimeTypeDef t; RTC_DateTypeDef d;
  M5.Rtc.GetTime(&t); M5.Rtc.GetDate(&d);
  char b[24];
  snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d",
           d.Year, d.Month, d.Date, t.Hours, t.Minutes, t.Seconds);
  return String(b);
}

uint32_t countRows() {
  if (!fsOk) return 0;
  File f = LittleFS.open(LOG_PATH, "r");
  if (!f) return 0;
  uint32_t n = 0;
  while (f.available()) if (f.read() == '\n') n++;
  f.close();
  return n;
}

// Append one line, closing the file each time. Slower than holding it open,
// but the battery dying mid-test must not take the whole log with it.
void logLine(const String &line) {
  if (!fsOk) return;
  File f = LittleFS.open(LOG_PATH, "a");
  if (!f) return;
  f.println(line);
  f.close();
  logRows++;
}

void logSample() {
  float batV  = M5.Axp.GetBatVoltage();
  float batI  = M5.Axp.GetBatCurrent();
  float vbus  = M5.Axp.GetVBusVoltage();
  float tempC; M5.Imu.getTempData(&tempC);
  float coul  = M5.Axp.GetCoulombData();
  bool  usbIn = (vbus > 4.0f);

  uint32_t elapsed = discharging ? (millis() - dischargeStartMs) / 1000 : 0;

  char line[128];
  snprintf(line, sizeof(line), "%s,%lu,%.3f,%.1f,%.1f,%.2f,%d,%.1f,%.2f,%d",
           rtcStamp().c_str(), (unsigned long)elapsed,
           batV, batI, batPercent(batV), vbus, usbIn ? 1 : 0,
           tempC, coul, BRIGHTNESS[brightnessIdx]);
  logLine(String(line));
  Serial.printf("LOG %s\n", line);
}

void dumpLog() {
  if (!fsOk) { Serial.println("# FS not mounted"); return; }
  File f = LittleFS.open(LOG_PATH, "r");
  if (!f) { Serial.println("# no log file"); return; }
  Serial.printf("\n### BEGIN %s  (%u bytes)\n", LOG_PATH, (unsigned)f.size());
  Serial.println("# rtc,elapsed_s,batV,batI_mA,batPct,vbusV,usb,tempC,coulomb_mAh,brightness");
  while (f.available()) Serial.write(f.read());
  f.close();
  Serial.println("### END\n");
}

void clearLog() {
  if (!fsOk) return;
  LittleFS.remove(LOG_PATH);
  logRows = 0;
  Serial.println("# log cleared");
}

void statusReport() {
  float batV = M5.Axp.GetBatVoltage();
  float vbus = M5.Axp.GetVBusVoltage();
  Serial.printf("\n# status\n");
  Serial.printf("#   rtc        %s\n", rtcStamp().c_str());
  Serial.printf("#   battery    %.3f V  %.1f mA  %.0f%%\n",
                batV, M5.Axp.GetBatCurrent(), batPercent(batV));
  Serial.printf("#   vbus       %.2f V  (%s)\n", vbus,
                vbus > 4.0f ? "USB connected - charging" : "on battery");
  Serial.printf("#   coulomb    %.2f mAh\n", M5.Axp.GetCoulombData());
  Serial.printf("#   brightness %d%%\n", BRIGHTNESS[brightnessIdx]);
  Serial.printf("#   discharge  %s", discharging ? "" : "not started\n");
  if (discharging) Serial.printf("%lu s elapsed\n", (millis() - dischargeStartMs) / 1000);
  Serial.printf("#   log rows   %lu\n", (unsigned long)logRows);
  if (fsOk) Serial.printf("#   fs         %u / %u bytes used\n",
                          (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  else      Serial.printf("#   fs         NOT MOUNTED\n");
}

void setup() {
  M5.begin();
  M5.Imu.Init();
  Serial.begin(115200);

  M5.Lcd.setRotation(3);
  M5.Axp.ScreenBreath(BRIGHTNESS[brightnessIdx]);
  M5.Axp.EnableCoulombcounter();

  spr.setColorDepth(16);
  spr.createSprite(SCR_W, SCR_H);

  seedRtcFromBuildTime();

  fsOk = LittleFS.begin(true);   // format on first run
  logRows = countRows();

  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  gravityBaseline = sqrtf(ax * ax + ay * ay + az * az);
  vibWindowStart = millis();

  usbPrev = (M5.Axp.GetVBusVoltage() > 4.0f);

  Serial.println("\n=== M5StickC Plus battery logger ===");
  Serial.printf("# fs %s, %lu existing rows\n",
                fsOk ? "mounted" : "FAILED", (unsigned long)logRows);
  Serial.println("# commands: d=dump  c=clear  s=status  +/-=brightness");
  logLine("# BOOT " + rtcStamp() + " brightness=" + String(BRIGHTNESS[brightnessIdx]));
  statusReport();

  lastLogMs = millis() - LOG_PERIOD_MS;   // log one sample immediately
}

void sampleImu() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  gravityBaseline += (mag - gravityBaseline) * 0.02f;
  float dyn = mag - gravityBaseline;
  vibSumSq += (double)dyn * dyn;
  vibCount++;

  uint32_t now = millis();
  if (now - vibWindowStart >= VIB_WINDOW_MS) {
    if (vibCount > 0) {
      vibRmsMg = sqrtf((float)(vibSumSq / vibCount)) * 1000.0f;
      if (vibRmsMg > vibPeakMg) vibPeakMg = vibRmsMg;
      vibBarMg += (vibRmsMg - vibBarMg) * 0.5f;
    }
    vibSumSq = 0.0; vibCount = 0; vibWindowStart = now;
  }
}

void drawBar(int x, int y, int w, int h, float value, float fs, uint16_t color) {
  spr.drawRect(x, y, w, h, C_DIM);
  int inner = w - 4;
  int filled = (int)(inner * (value / fs));
  if (filled < 0) filled = 0;
  if (filled > inner) filled = inner;
  if (filled > 0) spr.fillRect(x + 2, y + 2, filled, h - 4, color);
}

void render() {
  RTC_TimeTypeDef t; RTC_DateTypeDef d;
  M5.Rtc.GetTime(&t); M5.Rtc.GetDate(&d);

  float tempC; M5.Imu.getTempData(&tempC);
  float batV = M5.Axp.GetBatVoltage();
  float batI = M5.Axp.GetBatCurrent();
  float vbus = M5.Axp.GetVBusVoltage();
  float batPct = batPercent(batV);
  bool  usbIn = (vbus > 4.0f);
  uint16_t battColor = (batPct > 50) ? C_BATT_OK : (batPct > 20 ? C_BATT_MD : C_BATT_LO);

  spr.fillSprite(C_BG);

  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_TIME, C_BG);
  spr.setTextSize(1);
  spr.setTextFont(7);
  char hm[8]; snprintf(hm, sizeof(hm), "%02d:%02d", t.Hours, t.Minutes);
  spr.drawString(hm, 4, 2);

  spr.setTextFont(1);
  spr.setTextSize(2);
  spr.setTextColor(C_DIM, C_BG);
  char ss[4]; snprintf(ss, sizeof(ss), "%02d", t.Seconds);
  spr.drawString(ss, 100, 26);

  spr.setTextSize(1);
  char ds[16]; snprintf(ds, sizeof(ds), "%04d-%02d-%02d", d.Year, d.Month, d.Date);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString(ds, 100, 4);

  int bx = 176, by = 4, bw = 52, bh = 22;
  spr.drawRect(bx, by, bw, bh, C_DIM);
  spr.fillRect(bx + bw, by + 6, 3, 10, C_DIM);
  int fill = (int)((bw - 4) * batPct / 100.0f);
  if (fill > 0) spr.fillRect(bx + 2, by + 2, fill, bh - 4, battColor);

  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(C_TIME, C_BG);
  spr.setTextSize(2);
  spr.drawString(String((int)(batPct + 0.5f)) + "%", 232, 28);
  spr.setTextSize(1);
  spr.setTextColor(usbIn ? C_BATT_OK : C_DIM, C_BG);
  spr.drawString(usbIn ? "USB CHG" : "BATT", 232, 46);

  spr.drawFastHLine(0, 58, SCR_W, C_DIM);

  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("TEMP", 4, 62);
  spr.setTextColor(C_TEMP, C_BG);
  spr.setTextSize(3);
  spr.drawString(String(tempC, 1), 4, 74);
  spr.setTextSize(1);
  spr.drawString("C", 76, 82);

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("BATT", 100, 62);
  spr.setTextColor(battColor, C_BG);
  spr.setTextSize(2);
  spr.drawString(String(batV, 2) + "V", 100, 74);
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(String(batI, 0) + "mA", 100, 92);

  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("VIB mg", 232, 62);
  spr.setTextColor(C_VIB, C_BG);
  spr.setTextSize(2);
  spr.drawString(String((int)(vibRmsMg + 0.5f)), 232, 74);
  spr.setTextSize(1);
  spr.setTextColor(C_PEAK, C_BG);
  spr.drawString("pk " + String((int)(vibPeakMg + 0.5f)), 232, 92);

  drawBar(4, 104, SCR_W - 8, 11, vibBarMg, VIB_FULLSCALE_MG, C_VIB);

  // ---- logger status line ----
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
  if (!fsOk) {
    spr.setTextColor(C_BATT_LO, C_BG);
    spr.drawString("LOG FS FAILED", 4, 121);
  } else if (discharging) {
    uint32_t s = (millis() - dischargeStartMs) / 1000;
    char b[40];
    snprintf(b, sizeof(b), "DISCHG %02lu:%02lu:%02lu  n=%lu  %d%%",
             (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60),
             (unsigned long)(s % 60), (unsigned long)logRows,
             BRIGHTNESS[brightnessIdx]);
    spr.setTextColor(C_LOG, C_BG);
    spr.drawString(b, 4, 121);
  } else {
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString("logging on USB  n=" + String(logRows) +
                   "  unplug to start", 4, 121);
  }

  spr.pushSprite(0, 0);
}

void handleSerial(char c) {
  switch (c) {
    case 'd': dumpLog();      break;
    case 'c': clearLog();     break;
    case 's': statusReport(); break;
    case '+':
      if (brightnessIdx > 0) brightnessIdx--;
      M5.Axp.ScreenBreath(BRIGHTNESS[brightnessIdx]);
      Serial.printf("# brightness %d%%\n", BRIGHTNESS[brightnessIdx]);
      break;
    case '-':
      if (brightnessIdx < sizeof(BRIGHTNESS) / sizeof(BRIGHTNESS[0]) - 1) brightnessIdx++;
      M5.Axp.ScreenBreath(BRIGHTNESS[brightnessIdx]);
      Serial.printf("# brightness %d%%\n", BRIGHTNESS[brightnessIdx]);
      break;
    default: break;
  }
}

void loop() {
  M5.update();
  uint32_t now = millis();

  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\r' && c != '\n') handleSerial(c);
  }

  if (now - lastImuMs >= IMU_PERIOD_MS) { lastImuMs = now; sampleImu(); }

  if (M5.BtnA.wasPressed()) {
    vibPeakMg = 0.0f;
    Serial.println("# vibration peak reset");
  }
  if (M5.BtnB.wasPressed()) {
    brightnessIdx = (brightnessIdx + 1) % (sizeof(BRIGHTNESS) / sizeof(BRIGHTNESS[0]));
    M5.Axp.ScreenBreath(BRIGHTNESS[brightnessIdx]);
    logLine("# BRIGHTNESS " + rtcStamp() + " -> " + String(BRIGHTNESS[brightnessIdx]));
    Serial.printf("# brightness %d%%\n", BRIGHTNESS[brightnessIdx]);
  }

  // USB plug/unplug marks the start and end of a discharge run
  bool usbNow = (M5.Axp.GetVBusVoltage() > 4.0f);
  if (usbNow != usbPrev) {
    if (!usbNow) {
      discharging = true;
      dischargeStartMs = now;
      M5.Axp.ClearCoulombcounter();
      logLine("# UNPLUGGED " + rtcStamp() + " brightness=" +
              String(BRIGHTNESS[brightnessIdx]));
      Serial.println("# USB removed - discharge test started");
    } else {
      uint32_t s = discharging ? (now - dischargeStartMs) / 1000 : 0;
      logLine("# PLUGGED " + rtcStamp() + " ran_for_s=" + String(s));
      Serial.printf("# USB back - ran %lu s on battery\n", (unsigned long)s);
      discharging = false;
    }
    usbPrev = usbNow;
  }

  if (now - lastLogMs >= LOG_PERIOD_MS) { lastLogMs = now; logSample(); }

  if (now - lastDrawMs >= DRAW_PERIOD_MS) { lastDrawMs = now; render(); }

  if (now - lastSerialMs >= SERIAL_PERIOD_MS) {
    lastSerialMs = now;
    RTC_TimeTypeDef t; M5.Rtc.GetTime(&t);
    float tempC; M5.Imu.getTempData(&tempC);
    float batV = M5.Axp.GetBatVoltage();
    Serial.printf("%02d:%02d:%02d,%.1f,%.3f,%.0f,%.0f,%.0f\n",
                  t.Hours, t.Minutes, t.Seconds, tempC, batV,
                  batPercent(batV), vibRmsMg, vibPeakMg);
  }
}
