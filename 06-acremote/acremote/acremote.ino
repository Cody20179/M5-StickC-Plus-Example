/*
 * M5StickC Plus - Hitachi air conditioner power remote
 *
 *   BtnA        : ON   (cool, 25 C, fan auto)
 *   BtnB        : OFF
 *   hold BtnB 1s: next Hitachi frame variant
 *
 * Serial: 1=on  0=off  v=next variant  A=scan every variant  s=status
 *
 * Why a variant selector: an AC remote does not send a "power" code, it sends
 * the WHOLE machine state in one long frame, and Hitachi has used at least
 * five different frame lengths over the years. The StickC Plus has an IR
 * transmitter but NO receiver, so the unit's own remote cannot be captured to
 * find out which one it speaks - the only way is to send each and watch for
 * the AC to beep. 'A' does exactly that, one variant per two seconds.
 */
#include <M5StickCPlus.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Hitachi.h>

#define IR_PIN 9          // IR LED on the M5StickC Plus
#define AC_TEMP 25

#define SCR_W 240
#define SCR_H 135

#define C_BG    0x0000
#define C_DIM   0x8410
#define C_LABEL 0x7BEF
#define C_TEXT  0xFFFF
#define C_ON    0x07E0
#define C_OFF   0xF800
#define C_ACC   0x07FF
#define C_TITLE 0xFD20

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

IRHitachiAc    ac224(IR_PIN);
IRHitachiAc1   ac104(IR_PIN);
IRHitachiAc424 ac424(IR_PIN);
IRHitachiAc344 ac344(IR_PIN);
IRHitachiAc264 ac264(IR_PIN);

enum Variant : uint8_t { V224, V104, V424, V344, V264, VARIANT_COUNT };
const char *VNAME[VARIANT_COUNT] = {
  "HITACHI_AC",      // 224 bit, the classic one
  "HITACHI_AC1",     // 104 bit
  "HITACHI_AC424",   // 424 bit
  "HITACHI_AC344",   // 344 bit
  "HITACHI_AC264",   // 264 bit
};
const char *VBITS[VARIANT_COUNT] = {"224 bit", "104 bit", "424 bit", "344 bit", "264 bit"};

uint8_t  variant     = V424;
bool     lastPowerOn = false;
String   lastSent    = "-";
uint32_t lastSentMs  = 0;
uint32_t sentCount   = 0;
bool     btnBHandled = false;
bool     scanning    = false;

uint32_t lastDrawMs = 0;

void sendPower(bool on) {
  switch (variant) {
    case V224:
      ac224.setMode(ac224.convertMode(stdAc::opmode_t::kCool));
      ac224.setTemp(AC_TEMP);
      ac224.setFan(ac224.convertFan(stdAc::fanspeed_t::kAuto));
      ac224.setPower(on);
      ac224.send();
      break;
    case V104:
      ac104.setMode(ac104.convertMode(stdAc::opmode_t::kCool));
      ac104.setTemp(AC_TEMP);
      ac104.setFan(ac104.convertFan(stdAc::fanspeed_t::kAuto));
      ac104.setPower(on);
      ac104.send();
      break;
    case V424:
      ac424.setMode(ac424.convertMode(stdAc::opmode_t::kCool));
      ac424.setTemp(AC_TEMP);
      ac424.setFan(ac424.convertFan(stdAc::fanspeed_t::kAuto));
      ac424.setPower(on);
      ac424.send();
      break;
    case V344:
      ac344.setMode(ac344.convertMode(stdAc::opmode_t::kCool));
      ac344.setTemp(AC_TEMP);
      ac344.setFan(ac344.convertFan(stdAc::fanspeed_t::kAuto));
      ac344.setPower(on);
      ac344.send();
      break;
    case V264:
      ac264.setMode(ac264.convertMode(stdAc::opmode_t::kCool));
      ac264.setTemp(AC_TEMP);
      ac264.setFan(ac264.convertFan(stdAc::fanspeed_t::kAuto));
      ac264.setPower(on);
      ac264.send();
      break;
  }

  lastPowerOn = on;
  lastSent    = String(on ? "ON " : "OFF ") + VBITS[variant];
  lastSentMs  = millis();
  sentCount++;
  Serial.printf("# sent %s via %s (%s), total %lu\n",
                on ? "ON" : "OFF", VNAME[variant], VBITS[variant],
                (unsigned long)sentCount);
}

void nextVariant() {
  variant = (variant + 1) % VARIANT_COUNT;
  Serial.printf("# variant -> %s (%s)\n", VNAME[variant], VBITS[variant]);
}

void render();   // forward

// Fire an ON frame for every variant in turn. Whichever one makes the AC
// chirp is the protocol this unit speaks.
void scanAllVariants(bool on) {
  scanning = true;
  Serial.printf("\n# SCAN: sending %s for all %d variants, 2 s apart\n",
                on ? "ON" : "OFF", VARIANT_COUNT);
  Serial.println("# point the device at the AC and note which one it answers");
  uint8_t saved = variant;
  for (uint8_t i = 0; i < VARIANT_COUNT; i++) {
    variant = i;
    Serial.printf("#  [%d/%d] %s ...\n", i + 1, VARIANT_COUNT, VNAME[i]);
    render();
    sendPower(on);
    delay(2000);
  }
  variant = saved;
  scanning = false;
  Serial.println("# SCAN done");
}

void statusReport() {
  Serial.println("\n# status");
  Serial.printf("#   variant   %s (%s)  [%d/%d]\n",
                VNAME[variant], VBITS[variant], variant + 1, VARIANT_COUNT);
  Serial.printf("#   last sent %s\n", lastSent.c_str());
  Serial.printf("#   frames    %lu\n", (unsigned long)sentCount);
  Serial.printf("#   preset    COOL %d C, fan auto\n", AC_TEMP);
  Serial.printf("#   IR pin    GPIO%d (transmit only, no receiver on this board)\n", IR_PIN);
}

void render() {
  spr.fillSprite(C_BG);
  spr.setTextFont(1);

  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(C_TITLE, C_BG);
  spr.drawString("HITACHI IR", 4, 4);

  spr.setTextDatum(TR_DATUM);
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(String(variant + 1) + "/" + String(VARIANT_COUNT), 236, 10);

  spr.drawFastHLine(0, 26, SCR_W, C_DIM);

  // which frame variant is armed
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(C_ACC, C_BG);
  spr.drawString(VNAME[variant], 4, 32);
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(VBITS[variant], 4, 52);

  // last command, big
  bool fresh = (millis() - lastSentMs) < 600;
  spr.setTextSize(3);
  spr.setTextColor(lastSentMs == 0 ? C_DIM : (lastPowerOn ? C_ON : C_OFF), C_BG);
  spr.drawString(lastSentMs == 0 ? "READY" : (lastPowerOn ? "ON" : "OFF"), 4, 68);

  if (fresh) {
    spr.setTextSize(1);
    spr.setTextColor(C_TEXT, C_BG);
    spr.drawString("SENT", 110, 76);
  }

  spr.setTextDatum(TR_DATUM);
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("COOL " + String(AC_TEMP) + "C", 236, 68);
  spr.drawString("n=" + String(sentCount), 236, 80);

  spr.drawFastHLine(0, 100, SCR_W, C_DIM);

  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(C_ON, C_BG);
  spr.drawString("A=ON", 4, 106);
  spr.setTextColor(C_OFF, C_BG);
  spr.drawString("B=OFF", 86, 106);
  spr.setTextSize(1);
  spr.setTextColor(C_DIM, C_BG);
  spr.setTextDatum(TR_DATUM);
  spr.drawString("hold B = protocol", 236, 112);

  spr.pushSprite(0, 0);
}

void setup() {
  M5.begin();
  Serial.begin(115200);

  M5.Lcd.setRotation(3);
  M5.Axp.ScreenBreath(100);

  spr.setColorDepth(16);
  spr.createSprite(SCR_W, SCR_H);

  ac224.begin();
  ac104.begin();
  ac424.begin();
  ac344.begin();
  ac264.begin();

  Serial.println("\n=== M5StickC Plus - Hitachi AC remote ===");
  Serial.println("# BtnA = ON, BtnB = OFF, hold BtnB = next variant");
  Serial.println("# serial: 1=on 0=off v=variant A=scan all s=status");
  statusReport();

  render();
}

void loop() {
  M5.update();

  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case '1': sendPower(true);      break;
      case '0': sendPower(false);     break;
      case 'v': nextVariant();        break;
      case 'A': scanAllVariants(true);  break;
      case 'O': scanAllVariants(false); break;
      case 's': statusReport();       break;
      default: break;
    }
  }

  if (M5.BtnA.wasPressed()) sendPower(true);

  if (M5.BtnB.isPressed() && !btnBHandled && M5.BtnB.pressedFor(1000)) {
    nextVariant();
    btnBHandled = true;
  }
  if (M5.BtnB.wasReleased()) {
    if (!btnBHandled) sendPower(false);
    btnBHandled = false;
  }

  uint32_t now = millis();
  if (!scanning && now - lastDrawMs >= 100) {
    lastDrawMs = now;
    render();
  }
}
