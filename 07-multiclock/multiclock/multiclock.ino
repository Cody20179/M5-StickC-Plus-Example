/*
 * M5StickC Plus - multi page instrument
 *
 *   BtnA (front)  : next page
 *   BtnB (side)   : page action (see footer)
 *   hold BtnB 1s  : backlight 100 -> 75 -> 50 -> 25 %
 *
 *   1 CLOCK   time, full date, weekday, battery
 *   2 ENV     chip temperature, battery detail, vibration + cumulative dose
 *   3 IMU     6-axis raw, orientation, bubble level
 *   4 SOUND   level meter + octave-band spectrum from the PDM microphone
 *   5 INFO    system, RTC, and what this board cannot sense
 *
 * Serial: n=next  1..5=jump  b=brightness  r=reset page counters  s=status
 */
#include <M5StickCPlus.h>
#include <ESP_I2S.h>
#include <arduinoFFT.h>
#include <math.h>

#define SCR_W 240
#define SCR_H 135

#define C_BG     0x0000
#define C_DIM    0x8410
#define C_LABEL  0x7BEF
#define C_TEXT   0xFFFF
#define C_TIME   0xFFFF
#define C_TEMP   0xFD20
#define C_OK     0x07E0
#define C_WARN   0xFFE0
#define C_BAD    0xF800
#define C_ACC    0x07FF
#define C_PEAK   0xF81F
#define C_TITLE  0xFD20
#define C_GRID   0x2124

#define PIN_MIC_CLK  0
#define PIN_MIC_DATA 34

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

const uint8_t PAGE_COUNT = 5;
uint8_t page = 0;

uint8_t brightnessIdx = 0;
const uint8_t BRIGHTNESS[] = {100, 75, 50, 25};

bool btnBHandled = false;
uint32_t lastDrawMs = 0, lastSerialMs = 0;

// ---------------- vibration ----------------
const uint32_t VIB_WINDOW_MS = 250;
float    gravityBaseline = 1.0f;
double   vibSumSq = 0.0;
uint32_t vibCount = 0;
uint32_t vibWindowStart = 0;
float    vibRmsMg = 0, vibPeakMg = 0, vibBarMg = 0;
double   vibDose = 0.0;          // cumulative mg-seconds since reset
uint32_t vibDoseStartMs = 0;
uint32_t lastImuMs = 0;
const uint32_t IMU_PERIOD_MS = 5;
const float VIB_FULLSCALE_MG = 500.0f;

// ---------------- IMU page ----------------
float accX = 0, accY = 0, accZ = 0;
float gyrX = 0, gyrY = 0, gyrZ = 0;
float pitchDeg = 0, rollDeg = 0;
float levelZeroPitch = 0, levelZeroRoll = 0;

// ---------------- microphone / FFT ----------------
I2SClass i2s;
bool micOk = false;

const uint16_t FFT_N       = 512;
const float    SAMPLE_RATE = 16000.0f;
float vReal[FFT_N];
float vImag[FFT_N];
int16_t micRaw[FFT_N];
ArduinoFFT<float> FFT(vReal, vImag, FFT_N, SAMPLE_RATE);

// Octave bands. Showing every FFT bin on a 240 px screen is noise; the ear
// works in ratios, so one bar per octave is both readable and honest.
const uint8_t BAND_COUNT = 8;
const float BAND_EDGE[BAND_COUNT + 1] = {
  22, 44, 88, 177, 354, 707, 1414, 2828, 5657   // sqrt(2) around 31..4000 Hz
};
const char *BAND_LABEL[BAND_COUNT] = {"31", "63", "125", "250", "500", "1k", "2k", "4k"};
float bandDb[BAND_COUNT]     = {0};
float bandPeakDb[BAND_COUNT] = {0};
float micDbfs = -90.0f, micPeakDbfs = -90.0f;
float domFreq = 0;

// ================= helpers =================

float batPercent(float v) {
  float p = (v - 3.10f) / (4.18f - 3.10f) * 100.0f;
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}

// Zeller's congruence. The BM8563 has a weekday register but nothing here ever
// set it, so derive the day from the date instead of trusting stale hardware.
uint8_t weekdayOf(uint16_t y, uint8_t m, uint8_t d) {
  if (m < 3) { m += 12; y -= 1; }
  uint16_t k = y % 100, j = y / 100;
  int h = (d + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
  return (h + 6) % 7;                 // 0 = Sunday
}
const char *WDAY[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

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
  RTC_DateTypeDef nd; nd.WeekDay = weekdayOf(year, month, day);
  nd.Month = month; nd.Date = (uint8_t)day; nd.Year = (uint16_t)year;
  RTC_TimeTypeDef nt; nt.Hours = hh; nt.Minutes = mm; nt.Seconds = ss;
  M5.Rtc.SetDate(&nd);
  M5.Rtc.SetTime(&nt);
}

void sampleImu() {
  M5.Imu.getAccelData(&accX, &accY, &accZ);
  M5.Imu.getGyroData(&gyrX, &gyrY, &gyrZ);

  float mag = sqrtf(accX * accX + accY * accY + accZ * accZ);
  gravityBaseline += (mag - gravityBaseline) * 0.02f;
  float dyn = mag - gravityBaseline;
  vibSumSq += (double)dyn * dyn;
  vibCount++;

  // orientation from gravity
  pitchDeg = atan2f(-accX, sqrtf(accY * accY + accZ * accZ)) * 57.2958f;
  rollDeg  = atan2f(accY, accZ) * 57.2958f;

  uint32_t now = millis();
  if (now - vibWindowStart >= VIB_WINDOW_MS) {
    if (vibCount > 0) {
      vibRmsMg = sqrtf((float)(vibSumSq / vibCount)) * 1000.0f;
      if (vibRmsMg > vibPeakMg) vibPeakMg = vibRmsMg;
      vibBarMg += (vibRmsMg - vibBarMg) * 0.5f;
      vibDose  += (double)vibRmsMg * (now - vibWindowStart) / 1000.0;
    }
    vibSumSq = 0; vibCount = 0; vibWindowStart = now;
  }
}

void initMic() {
  i2s.setPinsPdmRx(PIN_MIC_CLK, PIN_MIC_DATA);
  micOk = i2s.begin(I2S_MODE_PDM_RX, (uint32_t)SAMPLE_RATE,
                    I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  Serial.printf("# mic PDM %s\n", micOk ? "OK" : "FAILED");
}

void sampleSpectrum() {
  if (!micOk) return;
  size_t got = i2s.readBytes((char *)micRaw, sizeof(micRaw));
  size_t n = got / sizeof(int16_t);
  if (n < FFT_N) return;

  double sum = 0;
  for (uint16_t i = 0; i < FFT_N; i++) sum += micRaw[i];
  float mean = sum / FFT_N;

  double sq = 0;
  for (uint16_t i = 0; i < FFT_N; i++) {
    float s = micRaw[i] - mean;      // strip the DC the PDM decimator leaves
    vReal[i] = s;
    vImag[i] = 0.0f;
    sq += (double)s * s;
  }

  float rms = sqrt(sq / FFT_N);
  micDbfs = 20.0f * log10f((rms + 1e-6f) / 32768.0f);
  if (micDbfs > micPeakDbfs) micPeakDbfs = micDbfs;
  else micPeakDbfs -= 0.25f;         // slow fall-back so peaks stay readable

  FFT.windowing(FFTWindow::Hann, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();
  domFreq = FFT.majorPeak();

  const float binHz = SAMPLE_RATE / FFT_N;
  for (uint8_t b = 0; b < BAND_COUNT; b++) {
    uint16_t lo = (uint16_t)(BAND_EDGE[b] / binHz);
    uint16_t hi = (uint16_t)(BAND_EDGE[b + 1] / binHz);
    if (lo < 1) lo = 1;
    if (hi > FFT_N / 2) hi = FFT_N / 2;
    if (hi <= lo) hi = lo + 1;
    double acc = 0;
    for (uint16_t i = lo; i < hi; i++) acc += vReal[i];
    float avg = acc / (hi - lo);
    float db  = 20.0f * log10f(avg + 1e-3f);
    bandDb[b] += (db - bandDb[b]) * 0.4f;          // light smoothing
    if (bandDb[b] > bandPeakDb[b]) bandPeakDb[b] = bandDb[b];
    else bandPeakDb[b] -= 0.4f;
  }
}

// ================= drawing =================

void header(const char *title) {
  spr.fillSprite(C_BG);
  spr.setTextFont(1);
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(C_TITLE, C_BG);
  spr.drawString(title, 4, 3);

  // battery always visible, top right
  float v = M5.Axp.GetBatVoltage();
  float pct = batPercent(v);
  bool usb = M5.Axp.GetVBusVoltage() > 4.0f;
  uint16_t c = usb ? C_ACC : (pct > 50 ? C_OK : (pct > 20 ? C_WARN : C_BAD));
  spr.setTextDatum(TR_DATUM);
  spr.setTextSize(1);
  spr.setTextColor(c, C_BG);
  spr.drawString(String((int)(pct + 0.5f)) + "%" + (usb ? " CHG" : ""), 236, 4);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(String(page + 1) + "/" + String(PAGE_COUNT), 236, 14);

  spr.drawFastHLine(0, 24, SCR_W, C_DIM);
  spr.setTextDatum(TL_DATUM);
}

void footer(const char *hint) {
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.setTextDatum(BL_DATUM);
  spr.drawString(hint, 4, SCR_H - 1);
  spr.setTextDatum(TL_DATUM);
}

void bar(int x, int y, int w, int h, float v, float fs, uint16_t col) {
  spr.drawRect(x, y, w, h, C_DIM);
  int inner = w - 4;
  int f = (int)(inner * (v / fs));
  if (f < 0) f = 0;
  if (f > inner) f = inner;
  if (f > 0) spr.fillRect(x + 2, y + 2, f, h - 4, col);
}

// ---- page 1: clock ----
void pageClock() {
  RTC_TimeTypeDef t; RTC_DateTypeDef d;
  M5.Rtc.GetTime(&t); M5.Rtc.GetDate(&d);

  spr.fillSprite(C_BG);

  // hh:mm in the 7-segment font, seconds beside it
  spr.setTextDatum(TL_DATUM);
  spr.setTextFont(7);
  spr.setTextSize(1);
  spr.setTextColor(C_TIME, C_BG);
  char hm[8]; snprintf(hm, sizeof(hm), "%02d:%02d", t.Hours, t.Minutes);
  spr.drawString(hm, 6, 14);

  spr.setTextFont(1);
  spr.setTextSize(2);
  spr.setTextColor(C_ACC, C_BG);
  char ss[4]; snprintf(ss, sizeof(ss), "%02d", t.Seconds);
  spr.drawString(ss, 186, 44);

  // full date + weekday
  spr.setTextSize(2);
  spr.setTextColor(C_TEXT, C_BG);
  char ds[16];
  snprintf(ds, sizeof(ds), "%04d-%02d-%02d", d.Year, d.Month, d.Date);
  spr.drawString(ds, 6, 76);

  spr.setTextSize(2);
  spr.setTextColor(C_TEMP, C_BG);
  spr.drawString(WDAY[weekdayOf(d.Year, d.Month, d.Date)], 176, 76);

  spr.drawFastHLine(0, 98, SCR_W, C_DIM);

  // battery, spelled out on the home page
  float v   = M5.Axp.GetBatVoltage();
  float i   = M5.Axp.GetBatCurrent();
  float pct = batPercent(v);
  bool  usb = M5.Axp.GetVBusVoltage() > 4.0f;
  uint16_t c = usb ? C_ACC : (pct > 50 ? C_OK : (pct > 20 ? C_WARN : C_BAD));

  int bx = 6, by = 104, bw = 74, bh = 22;
  spr.drawRect(bx, by, bw, bh, C_DIM);
  spr.fillRect(bx + bw, by + 7, 3, 8, C_DIM);
  int fill = (int)((bw - 4) * pct / 100.0f);
  if (fill > 0) spr.fillRect(bx + 2, by + 2, fill, bh - 4, c);

  spr.setTextSize(2);
  spr.setTextColor(c, C_BG);
  spr.drawString(String((int)(pct + 0.5f)) + "%", 92, 106);

  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(String(v, 2) + "V " + String(i, 0) + "mA", 236, 106);
  spr.drawString(usb ? "USB CHARGING" : "ON BATTERY", 236, 118);
  spr.setTextDatum(TL_DATUM);

  spr.pushSprite(0, 0);
}

// ---- page 2: environment ----
void pageEnv() {
  header("ENV");

  float tempC; M5.Imu.getTempData(&tempC);
  float v = M5.Axp.GetBatVoltage();
  float i = M5.Axp.GetBatCurrent();
  float pct = batPercent(v);

  spr.setTextSize(1);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("TEMP (die)", 4, 30);
  spr.setTextColor(C_TEMP, C_BG);
  spr.setTextSize(3);
  spr.drawString(String(tempC, 1), 4, 42);
  spr.setTextSize(1);
  spr.drawString("C", 78, 50);

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("BATT", 120, 30);
  uint16_t c = pct > 50 ? C_OK : (pct > 20 ? C_WARN : C_BAD);
  spr.setTextColor(c, C_BG);
  spr.setTextSize(3);
  spr.drawString(String(v, 2), 120, 42);
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("V  " + String(i, 0) + "mA  " + String((int)pct) + "%", 120, 68);

  spr.drawFastHLine(0, 78, SCR_W, C_DIM);

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("VIB", 4, 82);
  spr.setTextColor(C_ACC, C_BG);
  spr.setTextSize(2);
  spr.drawString(String((int)(vibRmsMg + 0.5f)) + "mg", 30, 80);
  spr.setTextSize(1);
  spr.setTextColor(C_PEAK, C_BG);
  spr.drawString("pk " + String((int)(vibPeakMg + 0.5f)), 118, 82);

  // cumulative exposure - the number that actually matters if you are
  // watching a machine rather than glancing at an instant reading
  uint32_t mins = (millis() - vibDoseStartMs) / 60000;
  spr.setTextColor(C_DIM, C_BG);
  spr.setTextDatum(TR_DATUM);
  spr.drawString("sum " + String((uint32_t)(vibDose / 1000.0)) + "k mg-s / " +
                 String(mins) + "min", 236, 82);
  spr.setTextDatum(TL_DATUM);

  bar(4, 96, SCR_W - 8, 14, vibBarMg, VIB_FULLSCALE_MG, C_ACC);
  int pk = 6 + (int)((SCR_W - 12) * (vibPeakMg / VIB_FULLSCALE_MG));
  if (pk > SCR_W - 6) pk = SCR_W - 6;
  spr.drawFastVLine(pk, 98, 10, C_PEAK);

  footer("BtnA next   BtnB reset vib");
  spr.pushSprite(0, 0);
}

// ---- page 3: 6-axis ----
void pageImu() {
  header("IMU 6-AXIS");

  spr.setTextSize(1);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("ACC g", 4, 28);

  const char *ax[3] = {"X", "Y", "Z"};
  float av[3] = {accX, accY, accZ};
  for (int k = 0; k < 3; k++) {
    int y = 40 + k * 14;
    spr.setTextColor(C_LABEL, C_BG);
    spr.drawString(ax[k], 4, y + 2);
    // bipolar bar, centre = 0 g, full scale +-2 g
    int x0 = 16, w = 84, h = 11;
    spr.drawRect(x0, y, w, h, C_DIM);
    int mid = x0 + w / 2;
    spr.drawFastVLine(mid, y, h, C_GRID);
    int len = (int)((w / 2 - 2) * (av[k] / 2.0f));
    if (len > w / 2 - 2)  len = w / 2 - 2;
    if (len < -(w / 2 - 2)) len = -(w / 2 - 2);
    if (len >= 0) spr.fillRect(mid, y + 2, len, h - 4, C_ACC);
    else          spr.fillRect(mid + len, y + 2, -len, h - 4, C_ACC);
    spr.setTextColor(C_TEXT, C_BG);
    spr.drawString(String(av[k], 2), 104, y + 2);
  }

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("GYR dps", 4, 84);
  spr.setTextColor(C_TEXT, C_BG);
  char gs[40];
  snprintf(gs, sizeof(gs), "%+6.1f %+6.1f %+6.1f", gyrX, gyrY, gyrZ);
  spr.drawString(gs, 4, 96);

  spr.setTextColor(C_DIM, C_BG);
  snprintf(gs, sizeof(gs), "|a| %.2fg", sqrtf(accX*accX + accY*accY + accZ*accZ));
  spr.drawString(gs, 4, 110);

  // bubble level - the most useful thing a 6-axis chip does without maths
  int cx = 186, cy = 74, r = 34;
  spr.drawCircle(cx, cy, r, C_DIM);
  spr.drawCircle(cx, cy, r / 2, C_GRID);
  spr.drawFastHLine(cx - r, cy, 2 * r, C_GRID);
  spr.drawFastVLine(cx, cy - r, 2 * r, C_GRID);

  float p = pitchDeg - levelZeroPitch;
  float ro = rollDeg - levelZeroRoll;
  int bxp = cx + (int)(ro * r / 45.0f);
  int byp = cy + (int)(p  * r / 45.0f);
  int dx = bxp - cx, dy = byp - cy;
  float dist = sqrtf(dx * dx + dy * dy);
  if (dist > r - 4) { bxp = cx + dx * (r - 4) / dist; byp = cy + dy * (r - 4) / dist; }
  bool level = fabsf(p) < 1.5f && fabsf(ro) < 1.5f;
  spr.fillCircle(bxp, byp, 5, level ? C_OK : C_PEAK);

  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(C_LABEL, C_BG);
  snprintf(gs, sizeof(gs), "P%+.0f R%+.0f", p, ro);
  spr.drawString(gs, 236, 112);
  spr.setTextDatum(TL_DATUM);

  footer("BtnA next   BtnB zero level");
  spr.pushSprite(0, 0);
}

// ---- page 4: sound ----
void pageSound() {
  header("SOUND");

  if (!micOk) {
    spr.setTextColor(C_BAD, C_BG);
    spr.drawString("mic init failed", 4, 40);
    footer("BtnA next");
    spr.pushSprite(0, 0);
    return;
  }

  spr.setTextSize(1);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("LEVEL", 4, 28);
  spr.setTextColor(C_ACC, C_BG);
  spr.setTextSize(2);
  spr.drawString(String(micDbfs, 1), 44, 26);
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("dBFS", 110, 32);

  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(C_PEAK, C_BG);
  spr.drawString("pk " + String(micPeakDbfs, 0), 236, 28);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(String((int)domFreq) + " Hz", 236, 40);
  spr.setTextDatum(TL_DATUM);

  // octave bars. -80..-10 dB maps to the full height.
  const int top = 54, bot = 118, h = bot - top;
  const int bw = (SCR_W - 8) / BAND_COUNT;
  spr.drawFastHLine(4, bot, SCR_W - 8, C_DIM);

  for (uint8_t b = 0; b < BAND_COUNT; b++) {
    int x = 4 + b * bw;
    float norm = (bandDb[b] + 20.0f) / 70.0f;      // magnitude dB -> 0..1
    if (norm < 0) norm = 0;
    if (norm > 1) norm = 1;
    int bh = (int)(h * norm);
    uint16_t col = norm > 0.8f ? C_BAD : (norm > 0.55f ? C_WARN : C_ACC);
    if (bh > 0) spr.fillRect(x + 1, bot - bh, bw - 3, bh, col);

    float pn = (bandPeakDb[b] + 20.0f) / 70.0f;
    if (pn < 0) pn = 0;
    if (pn > 1) pn = 1;
    int py = bot - (int)(h * pn);
    spr.drawFastHLine(x + 1, py, bw - 3, C_PEAK);

    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(BAND_LABEL[b], x + 2, bot + 3);
  }

  footer("");
  spr.setTextDatum(BR_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("BtnB reset pk", 236, SCR_H - 1);
  spr.setTextDatum(TL_DATUM);
  spr.pushSprite(0, 0);
}

// ---- page 5: info ----
void pageInfo() {
  header("INFO");
  spr.setTextSize(1);

  RTC_TimeTypeDef t; RTC_DateTypeDef d;
  M5.Rtc.GetTime(&t); M5.Rtc.GetDate(&d);
  char l[48];

  spr.setTextColor(C_TEXT, C_BG);
  snprintf(l, sizeof(l), "%s rev%d %dMHz  %uMB",
           ESP.getChipModel(), ESP.getChipRevision(), ESP.getCpuFreqMHz(),
           ESP.getFlashChipSize() / (1024 * 1024));
  spr.drawString(l, 4, 28);

  snprintf(l, sizeof(l), "heap %u/%u KB   up %lus",
           ESP.getFreeHeap() / 1024, ESP.getHeapSize() / 1024, millis() / 1000);
  spr.drawString(l, 4, 40);

  snprintf(l, sizeof(l), "rtc  %04d-%02d-%02d %02d:%02d:%02d %s",
           d.Year, d.Month, d.Date, t.Hours, t.Minutes, t.Seconds,
           WDAY[weekdayOf(d.Year, d.Month, d.Date)]);
  spr.setTextColor(C_ACC, C_BG);
  spr.drawString(l, 4, 52);

  spr.drawFastHLine(0, 66, SCR_W, C_DIM);

  // Be explicit about what is NOT on this board - it saves the next person
  // from hunting for a compass that was never fitted.
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("NOT AVAILABLE ON THIS BOARD", 4, 70);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("magnetometer - MPU6886 is 6-axis", 4, 82);
  spr.drawString("  (accel+gyro only, no compass)", 4, 92);
  spr.drawString("hall sensor - removed in", 4, 102);
  spr.drawString("  arduino-esp32 core 3.x", 4, 112);

  footer("BtnA next");
  spr.pushSprite(0, 0);
}

void drawPage() {
  switch (page) {
    case 0: pageClock(); break;
    case 1: pageEnv();   break;
    case 2: pageImu();   break;
    case 3: pageSound(); break;
    case 4: pageInfo();  break;
  }
}

void pageAction() {
  switch (page) {
    case 1:
      vibPeakMg = 0; vibDose = 0; vibDoseStartMs = millis();
      Serial.println("# vibration peak and dose reset");
      break;
    case 2:
      levelZeroPitch = pitchDeg; levelZeroRoll = rollDeg;
      Serial.printf("# level zeroed at pitch %.1f roll %.1f\n", pitchDeg, rollDeg);
      break;
    case 3:
      micPeakDbfs = -90;
      for (uint8_t b = 0; b < BAND_COUNT; b++) bandPeakDb[b] = -20;
      Serial.println("# spectrum peaks reset");
      break;
    default: break;
  }
}

void statusReport() {
  RTC_TimeTypeDef t; RTC_DateTypeDef d;
  M5.Rtc.GetTime(&t); M5.Rtc.GetDate(&d);
  float v = M5.Axp.GetBatVoltage();
  float tempC; M5.Imu.getTempData(&tempC);
  Serial.println("\n# status");
  Serial.printf("#   page   %d/%d\n", page + 1, PAGE_COUNT);
  Serial.printf("#   rtc    %04d-%02d-%02d %02d:%02d:%02d %s\n",
                d.Year, d.Month, d.Date, t.Hours, t.Minutes, t.Seconds,
                WDAY[weekdayOf(d.Year, d.Month, d.Date)]);
  Serial.printf("#   batt   %.2f V  %.0f mA  %.0f%%\n",
                v, M5.Axp.GetBatCurrent(), batPercent(v));
  Serial.printf("#   temp   %.1f C (die)\n", tempC);
  Serial.printf("#   vib    %.0f mg now, %.0f peak, %.0f mg-s dose\n",
                vibRmsMg, vibPeakMg, vibDose);
  Serial.printf("#   imu    acc %.2f %.2f %.2f  gyr %.1f %.1f %.1f\n",
                accX, accY, accZ, gyrX, gyrY, gyrZ);
  Serial.printf("#   tilt   pitch %.1f  roll %.1f\n", pitchDeg, rollDeg);
  Serial.printf("#   mic    %s  %.1f dBFS  peak freq %.0f Hz\n",
                micOk ? "ok" : "FAILED", micDbfs, domFreq);
  if (micOk) {
    Serial.print("#   bands ");
    for (uint8_t b = 0; b < BAND_COUNT; b++)
      Serial.printf(" %s:%.0f", BAND_LABEL[b], bandDb[b]);
    Serial.println();
  }
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
  initMic();

  M5.Imu.getAccelData(&accX, &accY, &accZ);
  gravityBaseline = sqrtf(accX * accX + accY * accY + accZ * accZ);
  vibWindowStart = millis();
  vibDoseStartMs = millis();
  for (uint8_t b = 0; b < BAND_COUNT; b++) { bandDb[b] = -20; bandPeakDb[b] = -20; }

  Serial.println("\n=== M5StickC Plus multi page instrument ===");
  Serial.println("# BtnA next page, BtnB page action, hold BtnB brightness");
  Serial.println("# serial: n=next 1..5=jump b=brightness r=reset s=status");
  statusReport();

  drawPage();
}

void loop() {
  M5.update();
  uint32_t now = millis();

  while (Serial.available()) {
    char c = Serial.read();
    if (c >= '1' && c <= '0' + PAGE_COUNT) {
      page = c - '1';
      Serial.printf("# page %d\n", page + 1);
      drawPage();
    } else switch (c) {
      case 'n': page = (page + 1) % PAGE_COUNT; drawPage(); break;
      case 'b':
        brightnessIdx = (brightnessIdx + 1) % (sizeof(BRIGHTNESS) / sizeof(BRIGHTNESS[0]));
        M5.Axp.ScreenBreath(BRIGHTNESS[brightnessIdx]);
        Serial.printf("# brightness %d%%\n", BRIGHTNESS[brightnessIdx]);
        break;
      case 'r': pageAction();   break;
      case 's': statusReport(); break;
      default: break;
    }
  }

  if (now - lastImuMs >= IMU_PERIOD_MS) { lastImuMs = now; sampleImu(); }

  if (M5.BtnA.wasPressed()) {
    page = (page + 1) % PAGE_COUNT;
    Serial.printf("# page %d\n", page + 1);
    drawPage();
  }

  if (M5.BtnB.isPressed() && !btnBHandled && M5.BtnB.pressedFor(1000)) {
    brightnessIdx = (brightnessIdx + 1) % (sizeof(BRIGHTNESS) / sizeof(BRIGHTNESS[0]));
    M5.Axp.ScreenBreath(BRIGHTNESS[brightnessIdx]);
    Serial.printf("# brightness %d%%\n", BRIGHTNESS[brightnessIdx]);
    btnBHandled = true;
  }
  if (M5.BtnB.wasReleased()) {
    if (!btnBHandled) pageAction();
    btnBHandled = false;
  }

  // the FFT only runs while its page is showing - no point burning CPU on it
  if (page == 3) sampleSpectrum();

  if (now - lastDrawMs >= 100) { lastDrawMs = now; drawPage(); }

  if (now - lastSerialMs >= 5000) {
    lastSerialMs = now;
    RTC_TimeTypeDef t; M5.Rtc.GetTime(&t);
    float tempC; M5.Imu.getTempData(&tempC);
    float v = M5.Axp.GetBatVoltage();
    Serial.printf("%02d:%02d:%02d,p%d,%.1fC,%.2fV,%.0f%%,%.0fmg,%.1fdB\n",
                  t.Hours, t.Minutes, t.Seconds, page + 1,
                  tempC, v, batPercent(v), vibRmsMg, micDbfs);
  }
}
