/*
 * M5StickC Plus - 定時提醒
 *
 * 時鐘畫面 + 每隔固定分鐘數讓蜂鳴器響、紅色 LED 閃。
 * 預設每 5 分鐘（整點、05 分、10 分…），間隔可改，設定存在 NVS，
 * 重開機不會忘記。
 *
 *   BtnA : 提醒開 / 關
 *   BtnB : 試響（響的時候按任一鍵停止）
 *
 * Serial:
 *   s          狀態
 *   t          試響
 *   e          開 / 關
 *   I5         設定間隔為 5 分鐘（I 後接 1~2 位數，1~60）
 *   T...       對時，格式 T20260918212018
 */
#include <M5StickCPlus.h>
#include <Preferences.h>

#define SCR_W 240
#define SCR_H 135

#define C_BG     0x0000
#define C_DIM    0x8410
#define C_LABEL  0x7BEF
#define C_TEXT   0xFFFF
#define C_OK     0x07E0
#define C_WARN   0xFFE0
#define C_BAD    0xF800
#define C_ACC    0x07FF

// 不要用 M5.Beep：它的 tone() 把「通道」傳給 ledcWriteTone()，
// 而 arduino-esp32 3.x 的那個參數已經改成「腳位」，結果是對 GPIO0 寫入，
// 蜂鳴器在 GPIO2，永遠不會有聲音。直接驅動腳位。
#define PIN_BUZZER 2
#define PIN_LED    10          // 低電位點亮

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);
Preferences prefs;

uint8_t intervalMin = 5;       // 每幾分鐘響一次
bool    alarmOn     = true;

// 同一分鐘只響一次。用「日期 + 當天第幾分鐘」當鍵值，
// 這樣即使在那一分鐘內重開機或對時，也不會連續觸發兩次。
long lastFiredKey = -1;

// 響鈴用非阻塞狀態機跑，這樣時鐘還會繼續走、按鍵也還能停它
bool     ringing    = false;
uint8_t  ringStep   = 0;
uint32_t ringNextMs = 0;
const uint8_t RING_BEEPS = 3;  // 每次響三短聲

uint32_t lastDrawMs = 0;

const char *WDAY[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

uint8_t weekdayOf(uint16_t y, uint8_t m, uint8_t d) {
  if (m < 3) { m += 12; y -= 1; }
  uint16_t k = y % 100, j = y / 100;
  int h = (d + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
  return (h + 6) % 7;
}

float batPercent(float v) {
  float p = (v - 3.10f) / (4.18f - 3.10f) * 100.0f;
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}

long minuteKey() {
  RTC_TimeTypeDef t; RTC_DateTypeDef d;
  M5.Rtc.GetTime(&t); M5.Rtc.GetDate(&d);
  long ymd = (long)d.Year * 10000 + d.Month * 100 + d.Date;
  return ymd * 1440L + t.Hours * 60 + t.Minutes;
}

// ---------------------------------------------------------------- 設定存取

void loadSettings() {
  prefs.begin("alarm", true);
  intervalMin = prefs.getUChar("iv", 5);
  alarmOn     = prefs.getBool("on", true);
  prefs.end();
  if (intervalMin < 1 || intervalMin > 60) intervalMin = 5;
}

void saveSettings() {
  prefs.begin("alarm", false);
  prefs.putUChar("iv", intervalMin);
  prefs.putBool("on", alarmOn);
  prefs.end();
}

// ------------------------------------------------------------------ 響鈴

void buzzerOn(uint32_t freq) {
  ledcAttach(PIN_BUZZER, freq, 10);
  ledcWriteTone(PIN_BUZZER, freq);
}

void buzzerOff() {
  ledcWriteTone(PIN_BUZZER, 0);
  ledcDetach(PIN_BUZZER);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);   // 閒置拉低，不要浮接
}

void ringStart() {
  ringing    = true;
  ringStep   = 0;
  ringNextMs = millis();
  Serial.println("# ring");
}

void ringStop() {
  ringing = false;
  buzzerOff();
  digitalWrite(PIN_LED, HIGH);     // 熄滅
}

// 偶數步開、奇數步關，靠 millis() 推進，不佔用主迴圈
void ringTick() {
  if (!ringing) return;
  uint32_t now = millis();
  if (now < ringNextMs) return;

  if (ringStep >= RING_BEEPS * 2) { ringStop(); return; }

  if ((ringStep % 2) == 0) {
    buzzerOn(4000);                // 4 kHz 接近這顆被動蜂鳴器的共振點，最大聲
    digitalWrite(PIN_LED, LOW);    // 亮
    ringNextMs = now + 150;
  } else {
    buzzerOff();
    digitalWrite(PIN_LED, HIGH);   // 暗
    ringNextMs = now + 180;
  }
  ringStep++;
}

// ------------------------------------------------------------------ 檢查

void checkAlarm() {
  if (!alarmOn || ringing) return;

  RTC_TimeTypeDef t;
  M5.Rtc.GetTime(&t);
  if (t.Minutes % intervalMin != 0) return;

  long key = minuteKey();
  if (key == lastFiredKey) return;   // 這一分鐘已經響過了

  lastFiredKey = key;
  Serial.printf("# ALARM at %02d:%02d\n", t.Hours, t.Minutes);
  ringStart();
}

// 距離下次響還有幾秒
long secondsToNext() {
  RTC_TimeTypeDef t;
  M5.Rtc.GetTime(&t);
  long nowMin  = (long)t.Hours * 60 + t.Minutes;
  long nextMin = (nowMin / intervalMin + 1) * intervalMin;
  return nextMin * 60 - (nowMin * 60 + t.Seconds);
}

// --------------------------------------------------------------- 序列指令

/*
 * 讀取一串數字參數。
 *
 * 用 peek() 而不是 read() 來判斷結束：遇到不屬於這個參數的字元時必須把它
 * 留在緩衝區裡，否則會把「下一個指令的第一個字元」吃掉。實際踩過 ——
 * 連續送 "I5" 再送 "T2026..."，I 的讀取器在等第二位數字時吃掉了那個 T，
 * 結果後面整串日期被主迴圈當成一堆無效指令丟掉，對時靜默失敗。
 */
uint8_t readDigits(char *buf, uint8_t maxLen, uint32_t timeoutMs) {
  uint8_t n = 0;
  uint32_t t0 = millis();
  while (n < maxLen && millis() - t0 < timeoutMs) {
    if (!Serial.available()) continue;
    int c = Serial.peek();
    if (c >= '0' && c <= '9') {
      buf[n++] = (char)Serial.read();
    } else if (c == '\r' || c == '\n') {
      Serial.read();                  // 結束符，吃掉
      if (n > 0) break;
    } else {
      break;                          // 別人的指令，留著
    }
  }
  buf[n] = '\0';
  return n;
}

void setRtcFromSerial() {
  char buf[16];
  uint8_t n = readDigits(buf, 14, 2000);
  if (n != 14) { Serial.println("# T needs YYYYMMDDHHMMSS"); return; }
  buf[14] = '\0';

  auto num = [&](uint8_t off, uint8_t len) {
    int v = 0;
    for (uint8_t i = 0; i < len; i++) v = v * 10 + (buf[off + i] - '0');
    return v;
  };
  int yy = num(0, 4), mo = num(4, 2), dd = num(6, 2);
  int hh = num(8, 2), mi = num(10, 2), ss = num(12, 2);
  if (yy < 2024 || yy > 2099 || mo < 1 || mo > 12 || dd < 1 || dd > 31 ||
      hh > 23 || mi > 59 || ss > 59) {
    Serial.println("# T rejected: out of range");
    return;
  }

  RTC_DateTypeDef nd;
  nd.WeekDay = weekdayOf(yy, mo, dd);
  nd.Month = mo; nd.Date = dd; nd.Year = yy;
  RTC_TimeTypeDef nt;
  nt.Hours = hh; nt.Minutes = mi; nt.Seconds = ss;
  M5.Rtc.SetDate(&nd);
  M5.Rtc.SetTime(&nt);

  // 對完時若正好落在該響的那一分鐘，別因為「這分鐘還沒響過」而立刻炸出來
  lastFiredKey = (mi % intervalMin == 0) ? minuteKey() : -1;

  Serial.printf("# RTC set to %04d-%02d-%02d %02d:%02d:%02d %s\n",
                yy, mo, dd, hh, mi, ss, WDAY[nd.WeekDay]);
}

void setIntervalFromSerial() {
  char buf[4];
  uint8_t n = readDigits(buf, 2, 600);
  if (n == 0) { Serial.println("# I needs 1-60, e.g. I5"); return; }
  int v = buf[0] - '0';
  if (n == 2) v = v * 10 + (buf[1] - '0');
  if (v < 1 || v > 60) { Serial.printf("# I rejected: %d (1-60)\n", v); return; }

  intervalMin = (uint8_t)v;
  lastFiredKey = -1;
  saveSettings();
  Serial.printf("# interval = every %d min\n", intervalMin);
}

void statusReport() {
  RTC_TimeTypeDef t; RTC_DateTypeDef d;
  M5.Rtc.GetTime(&t); M5.Rtc.GetDate(&d);
  long s = secondsToNext();
  float v = M5.Axp.GetBatVoltage();
  Serial.println("\n# status");
  Serial.printf("#   rtc      %04d-%02d-%02d %02d:%02d:%02d %s\n",
                d.Year, d.Month, d.Date, t.Hours, t.Minutes, t.Seconds,
                WDAY[weekdayOf(d.Year, d.Month, d.Date)]);
  Serial.printf("#   interval every %d min  %s\n",
                intervalMin, alarmOn ? "ON" : "OFF");
  Serial.printf("#   next in  %ldm %02lds\n", s / 60, s % 60);
  Serial.printf("#   ringing  %s\n", ringing ? "yes" : "no");
  Serial.printf("#   battery  %.2f V  %.0f%%\n", v, batPercent(v));
  Serial.printf("#   buzzer   GPIO%d   led GPIO%d\n", PIN_BUZZER, PIN_LED);
}

// ------------------------------------------------------------------ 畫面

void render() {
  RTC_TimeTypeDef t; RTC_DateTypeDef d;
  M5.Rtc.GetTime(&t); M5.Rtc.GetDate(&d);

  spr.fillSprite(C_BG);
  spr.setTextDatum(TL_DATUM);

  // 時分，7-seg 大字；響的時候整個轉紅
  spr.setTextFont(7);
  spr.setTextSize(1);
  spr.setTextColor(ringing ? C_BAD : C_TEXT, C_BG);
  char hm[8]; snprintf(hm, sizeof(hm), "%02d:%02d", t.Hours, t.Minutes);
  spr.drawString(hm, 6, 6);

  spr.setTextFont(1);
  spr.setTextSize(2);
  spr.setTextColor(C_ACC, C_BG);
  char ss[4]; snprintf(ss, sizeof(ss), "%02d", t.Seconds);
  spr.drawString(ss, 190, 36);

  spr.setTextSize(1);
  spr.setTextColor(C_LABEL, C_BG);
  char ds[24];
  snprintf(ds, sizeof(ds), "%04d-%02d-%02d  %s",
           d.Year, d.Month, d.Date, WDAY[weekdayOf(d.Year, d.Month, d.Date)]);
  spr.drawString(ds, 6, 60);

  spr.drawFastHLine(0, 74, SCR_W, C_DIM);

  // 間隔設定
  spr.setTextSize(1);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("EVERY", 6, 80);

  spr.setTextSize(3);
  spr.setTextColor(alarmOn ? C_ACC : C_DIM, C_BG);
  spr.drawString(String(intervalMin), 6, 92);
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("min", 6 + 18 * String(intervalMin).length() + 4, 108);

  spr.setTextSize(2);
  spr.setTextColor(alarmOn ? C_OK : C_DIM, C_BG);
  spr.drawString(alarmOn ? "ON" : "OFF", 96, 96);

  // 倒數 / 響鈴狀態
  spr.setTextDatum(TR_DATUM);
  spr.setTextSize(1);
  if (ringing) {
    spr.setTextColor(C_BAD, C_BG);
    spr.drawString("RINGING", 234, 84);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString("any key = stop", 234, 96);
  } else if (alarmOn) {
    long s = secondsToNext();
    char nx[24];
    snprintf(nx, sizeof(nx), "next in %ldm %02lds", s / 60, s % 60);
    spr.setTextColor(C_DIM, C_BG);
    spr.drawString(nx, 234, 90);
  }

  float v = M5.Axp.GetBatVoltage();
  float pct = batPercent(v);
  bool usb = M5.Axp.GetVBusVoltage() > 4.0f;
  uint16_t bc = usb ? C_ACC : (pct > 50 ? C_OK : (pct > 20 ? C_WARN : C_BAD));
  spr.setTextColor(bc, C_BG);
  spr.drawString(String((int)(pct + 0.5f)) + "%" + (usb ? " CHG" : ""), 234, 110);

  spr.setTextDatum(BL_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("A=on/off  B=test", 6, SCR_H - 1);
  spr.setTextDatum(TL_DATUM);

  spr.pushSprite(0, 0);
}

// ------------------------------------------------------------------ 主流程

void setup() {
  M5.begin();
  Serial.begin(115200);

  M5.Lcd.setRotation(3);
  M5.Axp.ScreenBreath(100);

  spr.setColorDepth(16);
  spr.createSprite(SCR_W, SCR_H);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);     // 熄滅
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  loadSettings();

  // 開機當下若正好落在該響的那一分鐘，不要立刻響
  RTC_TimeTypeDef t;
  M5.Rtc.GetTime(&t);
  if (t.Minutes % intervalMin == 0) lastFiredKey = minuteKey();

  Serial.println("\n=== M5StickC Plus interval alarm ===");
  Serial.println("# s=status t=test e=on/off I5=every 5 min T...=set clock");
  statusReport();

  render();
}

void loop() {
  M5.update();

  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 's': statusReport(); break;
      case 't': ringStart();    break;
      case 'e':
        alarmOn = !alarmOn;
        saveSettings();
        Serial.printf("# alarm %s\n", alarmOn ? "ON" : "OFF");
        break;
      case 'I': setIntervalFromSerial(); break;
      case 'T': setRtcFromSerial();      break;
      default: break;
    }
  }

  // 響的時候任一鍵都是「停」，不要順便把提醒關掉
  bool a = M5.BtnA.wasPressed();
  bool b = M5.BtnB.wasPressed();
  if (a || b) {
    if (ringing) {
      ringStop();
      Serial.println("# stopped");
    } else if (a) {
      alarmOn = !alarmOn;
      saveSettings();
      Serial.printf("# alarm %s\n", alarmOn ? "ON" : "OFF");
    } else {
      ringStart();
    }
  }

  checkAlarm();
  ringTick();

  uint32_t now = millis();
  if (now - lastDrawMs >= 200) { lastDrawMs = now; render(); }
}
