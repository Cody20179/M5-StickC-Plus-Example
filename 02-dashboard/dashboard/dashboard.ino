/*
 * M5StickC Plus - Dashboard
 *   Time  : BM8563 RTC
 *   Temp  : MPU6886 on-die sensor
 *   Batt  : AXP192 PMU
 *   Vib   : MPU6886 accelerometer, high-passed RMS
 *
 *   BtnA : reset vibration peak
 *   BtnB : cycle backlight brightness
 */
#include <M5StickCPlus.h>
#include <math.h>

#define SCR_W 240
#define SCR_H 135

#define C_BG      0x0000  // black
#define C_DIM     0x8410  // grey
#define C_LABEL   0x7BEF  // light grey
#define C_TIME    0xFFFF  // white
#define C_TEMP    0xFD20  // orange
#define C_BATT_OK 0x07E0  // green
#define C_BATT_MD 0xFFE0  // yellow
#define C_BATT_LO 0xF800  // red
#define C_VIB     0x07FF  // cyan
#define C_PEAK    0xF81F  // magenta

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

// ---- vibration measurement ----
// |a| minus a slow-tracking gravity baseline (high-pass), RMS over a window.
const uint32_t VIB_WINDOW_MS = 250;
float    gravityBaseline = 1.0f;
double   vibSumSq        = 0.0;
uint32_t vibCount        = 0;
uint32_t vibWindowStart  = 0;
float    vibRmsMg        = 0.0f;
float    vibPeakMg       = 0.0f;
float    vibBarMg        = 0.0f;

const float VIB_FULLSCALE_MG = 500.0f;

uint32_t lastImuMs  = 0;
uint32_t lastDrawMs = 0;
uint32_t lastLogMs  = 0;

const uint32_t IMU_PERIOD_MS  = 5;     // 200 Hz
const uint32_t DRAW_PERIOD_MS = 100;   // 10 fps
const uint32_t LOG_PERIOD_MS  = 1000;

// AXP192::ScreenBreath() takes 0-100 (percent), mapped to 2500-3200 mV backlight.
uint8_t brightnessIdx = 0;
const uint8_t BRIGHTNESS[] = {100, 75, 50, 25};

float batPercent(float v) {
  float p = (v - 3.10f) / (4.18f - 3.10f) * 100.0f;
  if (p < 0)   p = 0;
  if (p > 100) p = 100;
  return p;
}

// If the RTC is not running (implausible year), seed it from build time.
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
  for (uint8_t i = 0; i < 12; i++) {
    if (strncmp(mstr, mon[i], 3) == 0) { month = i + 1; break; }
  }

  RTC_DateTypeDef nd;
  nd.WeekDay = 0;
  nd.Month   = month;
  nd.Date    = (uint8_t)day;
  nd.Year    = (uint16_t)year;

  RTC_TimeTypeDef nt;
  nt.Hours   = (uint8_t)hh;
  nt.Minutes = (uint8_t)mm;
  nt.Seconds = (uint8_t)ss;

  M5.Rtc.SetDate(&nd);
  M5.Rtc.SetTime(&nt);
  Serial.printf("RTC seeded from build time: %04d-%02d-%02d %02d:%02d:%02d\n",
                year, month, day, hh, mm, ss);
}

void setup() {
  M5.begin();
  M5.Imu.Init();
  Serial.begin(115200);

  M5.Lcd.setRotation(3);
  M5.Axp.ScreenBreath(BRIGHTNESS[brightnessIdx]);

  spr.setColorDepth(16);
  spr.createSprite(SCR_W, SCR_H);

  seedRtcFromBuildTime();

  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  gravityBaseline = sqrtf(ax * ax + ay * ay + az * az);

  vibWindowStart = millis();
  Serial.println("\n=== M5StickC Plus dashboard ===");
  Serial.println("time,tempC,batV,batPct,vibRms_mg,vibPeak_mg");
}

void sampleImu() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax * ax + ay * ay + az * az);

  // single-pole high pass: baseline slowly follows |a|, the residual is motion
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
    vibSumSq = 0.0;
    vibCount = 0;
    vibWindowStart = now;
  }
}

void drawBar(int x, int y, int w, int h, float value, float fullscale, uint16_t color) {
  spr.drawRect(x, y, w, h, C_DIM);
  int inner  = w - 4;
  int filled = (int)(inner * (value / fullscale));
  if (filled < 0)     filled = 0;
  if (filled > inner) filled = inner;
  if (filled > 0) spr.fillRect(x + 2, y + 2, filled, h - 4, color);
}

void render() {
  RTC_TimeTypeDef t;
  RTC_DateTypeDef d;
  M5.Rtc.GetTime(&t);
  M5.Rtc.GetDate(&d);

  float tempC;
  M5.Imu.getTempData(&tempC);

  float batV   = M5.Axp.GetBatVoltage();
  float batI   = M5.Axp.GetBatCurrent();
  float vbus   = M5.Axp.GetVBusVoltage();
  float batPct = batPercent(batV);
  bool  usbIn  = (vbus > 4.0f);

  uint16_t battColor = (batPct > 50) ? C_BATT_OK : (batPct > 20 ? C_BATT_MD : C_BATT_LO);

  spr.fillSprite(C_BG);

  // ---- top row: clock + battery ----
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_TIME, C_BG);
  spr.setTextSize(1);
  spr.setTextFont(7);
  char hm[8];
  snprintf(hm, sizeof(hm), "%02d:%02d", t.Hours, t.Minutes);
  spr.drawString(hm, 4, 2);

  spr.setTextFont(1);
  spr.setTextSize(2);
  spr.setTextColor(C_DIM, C_BG);
  char ss[4];
  snprintf(ss, sizeof(ss), "%02d", t.Seconds);
  spr.drawString(ss, 100, 30);

  spr.setTextSize(1);
  char ds[16];
  snprintf(ds, sizeof(ds), "%04d-%02d-%02d", d.Year, d.Month, d.Date);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString(ds, 100, 4);

  // battery glyph, top right
  int bx = 176, by = 4, bw = 52, bh = 22;
  spr.drawRect(bx, by, bw, bh, C_DIM);
  spr.fillRect(bx + bw, by + 6, 3, 10, C_DIM);
  int fill = (int)((bw - 4) * batPct / 100.0f);
  if (fill > 0) spr.fillRect(bx + 2, by + 2, fill, bh - 4, battColor);

  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(C_TIME, C_BG);
  spr.setTextSize(2);
  spr.drawString(String((int)(batPct + 0.5f)) + "%", 232, 32);
  spr.setTextSize(1);
  spr.setTextColor(usbIn ? C_BATT_OK : C_DIM, C_BG);
  spr.drawString(usbIn ? "USB CHG" : "BATT", 232, 50);

  spr.drawFastHLine(0, 64, SCR_W, C_DIM);

  // ---- temperature ----
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_LABEL, C_BG);
  spr.setTextSize(1);
  spr.drawString("TEMP", 4, 70);
  spr.setTextColor(C_TEMP, C_BG);
  spr.setTextSize(3);
  spr.drawString(String(tempC, 1), 4, 82);
  spr.setTextSize(1);
  spr.drawString("C", 76, 90);

  // ---- battery detail ----
  spr.setTextColor(C_LABEL, C_BG);
  spr.setTextSize(1);
  spr.drawString("BATT", 100, 70);
  spr.setTextColor(battColor, C_BG);
  spr.setTextSize(2);
  spr.drawString(String(batV, 2) + "V", 100, 82);
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(String(batI, 0) + "mA", 100, 100);

  // ---- vibration ----
  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(C_LABEL, C_BG);
  spr.setTextSize(1);
  spr.drawString("VIB mg", 232, 70);
  spr.setTextColor(C_VIB, C_BG);
  spr.setTextSize(2);
  spr.drawString(String((int)(vibRmsMg + 0.5f)), 232, 82);
  spr.setTextSize(1);
  spr.setTextColor(C_PEAK, C_BG);
  spr.drawString("pk " + String((int)(vibPeakMg + 0.5f)), 232, 100);

  drawBar(4, 116, SCR_W - 8, 15, vibBarMg, VIB_FULLSCALE_MG, C_VIB);
  int peakX = 6 + (int)((SCR_W - 12) * (vibPeakMg / VIB_FULLSCALE_MG));
  if (peakX > SCR_W - 6) peakX = SCR_W - 6;
  spr.drawFastVLine(peakX, 118, 11, C_PEAK);

  spr.pushSprite(0, 0);
}

void loop() {
  M5.update();

  uint32_t now = millis();

  if (now - lastImuMs >= IMU_PERIOD_MS) {
    lastImuMs = now;
    sampleImu();
  }

  if (M5.BtnA.wasPressed()) {
    vibPeakMg = 0.0f;
    Serial.println("# vibration peak reset");
  }
  if (M5.BtnB.wasPressed()) {
    brightnessIdx = (brightnessIdx + 1) % (sizeof(BRIGHTNESS) / sizeof(BRIGHTNESS[0]));
    M5.Axp.ScreenBreath(BRIGHTNESS[brightnessIdx]);
    Serial.printf("# brightness -> %d\n", BRIGHTNESS[brightnessIdx]);
  }

  if (now - lastDrawMs >= DRAW_PERIOD_MS) {
    lastDrawMs = now;
    render();
  }

  if (now - lastLogMs >= LOG_PERIOD_MS) {
    lastLogMs = now;
    RTC_TimeTypeDef t;
    M5.Rtc.GetTime(&t);
    float tempC;
    M5.Imu.getTempData(&tempC);
    float batV = M5.Axp.GetBatVoltage();
    Serial.printf("%02d:%02d:%02d,%.1f,%.2f,%.0f,%.0f,%.0f\n",
                  t.Hours, t.Minutes, t.Seconds,
                  tempC, batV, batPercent(batV), vibRmsMg, vibPeakMg);
  }
}
