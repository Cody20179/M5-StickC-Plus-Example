/*
 * M5StickC Plus - BLE HID controller
 *
 * Pairs with the PC over Bluetooth LE and shows up as a keyboard + mouse.
 *
 *   BtnA (front, big)  : primary action
 *   BtnB (side)        : secondary action
 *   hold BtnA 1s       : switch profile
 *
 *   profile CLICK   A = ENTER          B = mouse LEFT click
 *   profile SLIDES  A = RIGHT arrow    B = LEFT arrow
 *   profile SCROLL  A = mouse LEFT     B = scroll down
 *
 * Serial commands (so it can be driven without touching the device):
 *   a = fire A   b = fire B   p = next profile   s = status
 *
 * Pairing on Windows: Settings > Bluetooth & devices > Add device > Bluetooth,
 * pick "M5Stick Ctrl". No PIN. It reconnects by itself afterwards.
 */
#include <M5StickCPlus.h>
#include <ESP32BLECombo.h>

// ESP32BLECombo::press() maps codes >= 136 straight to a HID usage (code - 136),
// the same convention as ESP32-BLE-Keyboard. ASCII goes through its own table,
// so '\n' already means Enter.
#define KEY_RETURN      0xB0   // 176 - 136 = 0x28 Enter
#define KEY_ESC         0xB1
#define KEY_RIGHT_ARROW 0xD7   // 215 - 136 = 0x4F
#define KEY_LEFT_ARROW  0xD8   // 216 - 136 = 0x50
#define KEY_DOWN_ARROW  0xD9
#define KEY_UP_ARROW    0xDA
#define KEY_PAGE_UP     0xD3
#define KEY_PAGE_DOWN   0xD6

#define SCR_W 240
#define SCR_H 135

#define C_BG     0x0000
#define C_DIM    0x8410
#define C_LABEL  0x7BEF
#define C_TEXT   0xFFFF
#define C_LIVE   0x07E0   // green - connected
#define C_WAIT   0xFFE0   // yellow - advertising
#define C_ACC    0x07FF
#define C_TITLE  0xFD20
#define C_FIRE   0xF81F

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);
ESP32BLECombo ble;

enum ActionKind { ACT_KEY, ACT_MOUSE_BTN, ACT_SCROLL };

// ---- air mouse ----
// The gyro gives angular rate in deg/s. Integrating it into cursor deltas is
// what every wand-style remote does; the accelerometer is useless for this
// because gravity swamps the signal the moment you tilt the device.
//
// The MPU6886 has a large, temperature-dependent zero offset (this unit reads
// roughly -5 / -12 / -9 dps at rest), so the bias MUST be measured and removed
// or the cursor crawls off on its own.
float gyroBiasX = 0, gyroBiasY = 0, gyroBiasZ = 0;
bool  airCalibrated = false;

// Which gyro axis drives which cursor axis depends entirely on how the device
// is held, so both are runtime-selectable (0=gx 1=gy 2=gz) and measured with
// the 'T' capture command rather than guessed.
uint8_t airAxisX = 2;       // gz -> cursor X
uint8_t airAxisY = 1;       // gy -> cursor Y
const char *AXIS_NAME[3] = {"gx", "gy", "gz"};

float airSens    = 0.45f;   // dps -> pixels per 20 ms tick, horizontal
float airSensY   = 0.45f;   // vertical, tuned separately - the usable wrist
                            // travel up/down is much smaller than left/right
float airDead    = 1.8f;    // dps, ignore anything smaller (hand tremor + drift)
int8_t airFlipX  = -1;      // flip these if the cursor goes the wrong way
int8_t airFlipY  = -1;
/*
 * Vertical control source.
 *
 * A gyro only sees ROTATION. Moving the device straight up and down produces
 * essentially nothing, which is exactly what the gesture capture showed here:
 * the "up/down" gesture had no signature of its own, it just leaked into gz
 * like the left/right gesture did.
 *
 * So vertical uses the accelerometer instead, as an absolute tilt angle
 * against gravity. Tilt the nose up and the cursor climbs at a steady rate,
 * level it off and it stops - a joystick, not a mouse. That needs no large
 * motion, cannot drift, and does not care how vigorously you move.
 */
enum VertSource : uint8_t { VS_TILT_X, VS_TILT_Y, VS_GYRO };
VertSource vertSource = VS_TILT_X;
const char *VS_NAME[3] = {"tiltX", "tiltY", "gyro"};

float tiltNeutral = 0.0f;    // degrees, captured while the device sits still
float tiltFilt    = 0.0f;    // low-passed current angle
float tiltGain    = 1.1f;    // pixels per degree of deflection, per 20 ms tick
float tiltDead    = 4.0f;    // degrees of slop around neutral
float tiltLastDeg = 0.0f;

float airAccX = 0, airAccY = 0;   // sub-pixel remainder carried between reports
bool  airFrozen = false;          // motion paused
float airLastDx = 0, airLastDy = 0;

uint32_t lastAirMs = 0;
const uint32_t AIR_PERIOD_MS = 20;   // 50 Hz reports

void calibrateGyro();     // defined below, after the profile table
float currentTiltDeg();

struct Action {
  ActionKind kind;
  uint8_t    code;    // key code, mouse button mask, or scroll magnitude
  int8_t     amount;  // scroll direction
  const char *label;
};

struct Profile {
  const char *name;
  Action a;
  Action b;
};

const Profile PROFILES[] = {
  { "CLICK",
    { ACT_KEY,       KEY_RETURN,                 0, "ENTER"      },
    { ACT_MOUSE_BTN, ESP32BLECombo::MOUSE_LEFT,  0, "MOUSE LEFT" } },
  { "SLIDES",
    { ACT_KEY,       KEY_RIGHT_ARROW,            0, "NEXT >"     },
    { ACT_KEY,       KEY_LEFT_ARROW,             0, "< PREV"     } },
  { "SCROLL",
    { ACT_MOUSE_BTN, ESP32BLECombo::MOUSE_LEFT,  0, "MOUSE LEFT" },
    { ACT_SCROLL,    0,                         -3, "SCROLL DN"  } },
  { "AIRMOUSE",
    { ACT_MOUSE_BTN, ESP32BLECombo::MOUSE_LEFT,  0, "L CLICK"    },
    { ACT_MOUSE_BTN, ESP32BLECombo::MOUSE_RIGHT, 0, "R CLICK"    } },
};
const uint8_t PROFILE_AIRMOUSE = 3;
const uint8_t PROFILE_COUNT = sizeof(PROFILES) / sizeof(PROFILES[0]);

uint8_t  profileIdx  = 0;
uint32_t fireCount   = 0;
String   lastAction  = "-";
uint32_t lastFireMs  = 0;
bool     wasConnected = false;
bool     btnAHandled = false;   // long press already consumed this hold
bool     btnBHandled = false;

uint32_t lastDrawMs = 0, lastBattMs = 0;

float batPercent(float v) {
  float p = (v - 3.10f) / (4.18f - 3.10f) * 100.0f;
  if (p < 0)   p = 0;
  if (p > 100) p = 100;
  return p;
}

void fire(const Action &act) {
  if (!ble.isConnected()) {
    Serial.println("# not connected, action dropped");
    lastAction = "NOT CONNECTED";
    lastFireMs = millis();
    return;
  }

  switch (act.kind) {
    case ACT_KEY:
      ble.press(act.code);
      delay(15);
      ble.release(act.code);
      break;
    case ACT_MOUSE_BTN:
      ble.mouseClick(act.code);
      break;
    case ACT_SCROLL:
      ble.mouseScroll(act.amount);
      break;
  }

  fireCount++;
  lastAction = act.label;
  lastFireMs = millis();
  Serial.printf("# fired %s (total %lu)\n", act.label, (unsigned long)fireCount);
}

/*
 * Gesture capture. Move the device in ONE direction only (e.g. tilt up and
 * down repeatedly) for the capture window; whichever gyro axis shows the
 * largest peak is the axis that gesture actually drives. Beats guessing the
 * IMU orientation from how someone happens to hold the thing.
 */
void captureGesture(uint32_t ms) {
  Serial.printf("\n# CAPTURE: move the device in ONE direction for %lu s...\n",
                (unsigned long)(ms / 1000));
  float peak[3] = {0, 0, 0};
  double area[3] = {0, 0, 0};
  uint32_t n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    float g[3];
    M5.Imu.getGyroData(&g[0], &g[1], &g[2]);
    g[0] -= gyroBiasX; g[1] -= gyroBiasY; g[2] -= gyroBiasZ;
    for (int i = 0; i < 3; i++) {
      float a = fabsf(g[i]);
      if (a > peak[i]) peak[i] = a;
      area[i] += a;
    }
    n++;
    delay(10);
  }
  int best = 0;
  for (int i = 1; i < 3; i++) if (area[i] > area[best]) best = i;

  Serial.println("#   axis   peak dps   mean dps");
  for (int i = 0; i < 3; i++)
    Serial.printf("#   %s     %7.1f    %7.1f%s\n", AXIS_NAME[i], peak[i],
                  n ? area[i] / n : 0.0, i == best ? "   <-- dominant" : "");
  Serial.printf("# dominant axis for that gesture: %s\n", AXIS_NAME[best]);
  Serial.printf("# to use it: 'X' cycles cursor-X axis, 'Y' cycles cursor-Y axis\n");
}

/*
 * Tilt angle against gravity, in degrees.
 *
 * Which body axis counts as "nose up" depends on how the device is held, so
 * both candidates are available and 'V' switches between them:
 *   tiltX - pitch about the short axis (nose of the stick up/down)
 *   tiltY - pitch about the long axis  (top edge of the screen up/down)
 */
float currentTiltDeg() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  if (vertSource == VS_TILT_Y)
    return atan2f(-ay, sqrtf(ax * ax + az * az)) * 57.2958f;
  return atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2958f;
}

void calibrateGyro() {
  Serial.println("# calibrating gyro - hold the device still...");
  const int N = 300;
  double sx = 0, sy = 0, sz = 0;
  for (int i = 0; i < N; i++) {
    float gx, gy, gz;
    M5.Imu.getGyroData(&gx, &gy, &gz);
    sx += gx; sy += gy; sz += gz;
    delay(3);
  }
  gyroBiasX = sx / N;
  gyroBiasY = sy / N;
  gyroBiasZ = sz / N;
  airCalibrated = true;
  Serial.printf("# gyro bias: %.2f %.2f %.2f dps\n", gyroBiasX, gyroBiasY, gyroBiasZ);

  // whatever angle it is sitting at right now becomes "cursor stays put"
  tiltNeutral = currentTiltDeg();
  tiltFilt    = tiltNeutral;
  Serial.printf("# tilt neutral (%s): %.1f deg\n", VS_NAME[vertSource], tiltNeutral);
}

void nextProfile() {
  profileIdx = (profileIdx + 1) % PROFILE_COUNT;
  Serial.printf("# profile -> %s\n", PROFILES[profileIdx].name);
  if (profileIdx == PROFILE_AIRMOUSE) {
    airAccX = airAccY = 0;
    airFrozen = false;
    if (!airCalibrated) calibrateGyro();
  }
}

// Turn angular rate into cursor movement. Called at 50 Hz while the AIRMOUSE
// profile is active and the link is up.
void airMouseTick() {
  float g[3];
  M5.Imu.getGyroData(&g[0], &g[1], &g[2]);
  g[0] -= gyroBiasX; g[1] -= gyroBiasY; g[2] -= gyroBiasZ;

  // ---- horizontal: gyro yaw rate (this part already works well) ----
  float rx = g[airAxisX];
  if (fabsf(rx) < airDead) rx = 0;
  airLastDx = rx;

  // ---- vertical ----
  float vy = 0;
  if (vertSource == VS_GYRO) {
    vy = g[airAxisY];
    if (fabsf(vy) < airDead) vy = 0;
    airLastDy = vy;
    vy *= airSensY;
  } else {
    float deg = currentTiltDeg();
    tiltFilt += (deg - tiltFilt) * 0.25f;        // accel is noisy while moving
    float defl = tiltFilt - tiltNeutral;
    tiltLastDeg = defl;
    airLastDy = defl;

    if (fabsf(defl) < tiltDead) defl = 0;
    else defl -= (defl > 0 ? tiltDead : -tiltDead);   // no step at the edge

    vy = defl * tiltGain;
  }

  if (airFrozen) return;

  airAccX += rx * airSens * airFlipX;
  airAccY += vy * airFlipY;

  int dx = (int)airAccX;
  int dy = (int)airAccY;
  airAccX -= dx;        // keep the fraction so slow movement still registers
  airAccY -= dy;

  if (dx != 0 || dy != 0) ble.mouseMove(dx, dy);
}

void statusReport() {
  const Profile &p = PROFILES[profileIdx];
  float v = M5.Axp.GetBatVoltage();
  Serial.println("\n# status");
  Serial.printf("#   ble      %s\n", ble.isConnected() ? "CONNECTED" : "advertising");
  Serial.printf("#   profile  %s\n", p.name);
  Serial.printf("#   BtnA     %s\n", p.a.label);
  Serial.printf("#   BtnB     %s\n", p.b.label);
  Serial.printf("#   fired    %lu\n", (unsigned long)fireCount);
  Serial.printf("#   battery  %.2f V  %.0f%%\n", v, batPercent(v));
  Serial.printf("#   air      %s, dead %.1f dps, %s\n",
                airCalibrated ? "calibrated" : "NOT calibrated",
                airDead, airFrozen ? "frozen" : "tracking");
  Serial.printf("#   mapping  cursorX <- gyro %s (sens %.2f, flip %d)\n",
                AXIS_NAME[airAxisX], airSens, airFlipX);
  if (vertSource == VS_GYRO)
    Serial.printf("#            cursorY <- gyro %s (sens %.2f, flip %d)\n",
                  AXIS_NAME[airAxisY], airSensY, airFlipY);
  else
    Serial.printf("#            cursorY <- %s (gain %.1f px/deg, dead %.0f deg, "
                  "neutral %.1f deg, now %+.1f deg, flip %d)\n",
                  VS_NAME[vertSource], tiltGain, tiltDead, tiltNeutral,
                  tiltLastDeg, airFlipY);
  Serial.printf("#   bias     %.2f %.2f %.2f dps\n", gyroBiasX, gyroBiasY, gyroBiasZ);
}

void render() {
  const Profile &p = PROFILES[profileIdx];
  bool conn = ble.isConnected();
  float v = M5.Axp.GetBatVoltage();
  float pct = batPercent(v);

  spr.fillSprite(C_BG);
  spr.setTextFont(1);
  spr.setTextDatum(TL_DATUM);

  // title + link state
  spr.setTextSize(2);
  spr.setTextColor(C_TITLE, C_BG);
  spr.drawString("M5 CTRL", 4, 4);

  spr.setTextDatum(TR_DATUM);
  spr.setTextSize(2);
  spr.setTextColor(conn ? C_LIVE : C_WAIT, C_BG);
  spr.drawString(conn ? "LINKED" : "PAIRING", 236, 4);

  spr.drawFastHLine(0, 26, SCR_W, C_DIM);

  // profile
  spr.setTextDatum(TL_DATUM);
  spr.setTextSize(1);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("PROFILE", 4, 32);
  spr.setTextSize(2);
  spr.setTextColor(C_ACC, C_BG);
  spr.drawString(p.name, 62, 29);

  // mapping
  spr.setTextSize(1);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("BtnA", 4, 54);
  spr.drawString("BtnB", 4, 72);
  spr.setTextSize(2);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(p.a.label, 46, 50);
  spr.drawString(p.b.label, 46, 68);

  spr.drawFastHLine(0, 90, SCR_W, C_DIM);

  // last action, flashes briefly after each press
  bool fresh = (millis() - lastFireMs) < 400;
  spr.setTextSize(1);
  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("last", 4, 96);
  spr.setTextColor(fresh ? C_FIRE : C_TEXT, C_BG);
  spr.drawString(lastAction, 34, 96);
  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString("n=" + String(fireCount), 236, 96);

  // air mouse readout replaces the "last action" counter area
  if (profileIdx == PROFILE_AIRMOUSE) {
    spr.setTextDatum(TL_DATUM);
    spr.setTextSize(1);
    spr.setTextColor(airFrozen ? C_WAIT : C_LIVE, C_BG);
    spr.drawString(airFrozen ? "FROZEN" : "TRACKING", 4, 96);
    spr.setTextColor(C_DIM, C_BG);
    char g[32];
    if (vertSource == VS_GYRO)
      snprintf(g, sizeof(g), "%s%+.0f %s%+.0f", AXIS_NAME[airAxisX], airLastDx,
               AXIS_NAME[airAxisY], airLastDy);
    else
      snprintf(g, sizeof(g), "yaw%+.0f  tilt%+.0f deg", airLastDx, tiltLastDeg);
    spr.setTextDatum(TR_DATUM);
    spr.drawString(g, 236, 96);
  }

  // footer
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(C_DIM, C_BG);
  spr.drawString(profileIdx == PROFILE_AIRMOUSE ? "hold B 1s = freeze"
                                                : "hold A 1s = profile", 4, 112);
  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(pct > 20 ? C_DIM : C_WAIT, C_BG);
  spr.drawString(String((int)(pct + 0.5f)) + "%", 236, 112);

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

  ESP32BLEComboConfig cfg;
  cfg.mode         = ESP32BLEComboMode::KEYBOARD_MOUSE;
  cfg.deviceName   = "M5Stick Ctrl";
  cfg.manufacturer = "M5Stack";
  cfg.appearance   = ESP32BLEComboAppearance::KEYBOARD;
  cfg.batteryLevel = (uint8_t)batPercent(M5.Axp.GetBatVoltage());

  Serial.println("\n=== M5StickC Plus BLE controller ===");
  bool ok = ble.begin(cfg);
  Serial.printf("# BLE begin: %s\n", ok ? "OK, advertising as \"M5Stick Ctrl\"" : "FAILED");
  Serial.println("# pair from Windows: Settings > Bluetooth > Add device > Bluetooth");
  Serial.println("# commands: a=fire A  b=fire B  p=profile  s=status");
  statusReport();

  render();
}

void loop() {
  M5.update();
  uint32_t now = millis();

  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'a': fire(PROFILES[profileIdx].a); break;
      case 'b': fire(PROFILES[profileIdx].b); break;
      case 'p': nextProfile();                break;
      case 's': statusReport();               break;
      case 'z': calibrateGyro();              break;
      case 'x': airFlipX = -airFlipX; Serial.printf("# flipX -> %d\n", airFlipX); break;
      case 'y': airFlipY = -airFlipY; Serial.printf("# flipY -> %d\n", airFlipY); break;
      case '1': airSens  = max(0.05f, airSens  - 0.1f); Serial.printf("# sensX %.2f\n", airSens);  break;
      case '2': airSens  = min(3.0f,  airSens  + 0.1f); Serial.printf("# sensX %.2f\n", airSens);  break;
      case '3':
        if (vertSource == VS_GYRO) { airSensY = max(0.05f, airSensY - 0.1f); Serial.printf("# sensY %.2f\n", airSensY); }
        else { tiltGain = max(0.1f, tiltGain - 0.2f); Serial.printf("# tiltGain %.1f px/deg\n", tiltGain); }
        break;
      case '4':
        if (vertSource == VS_GYRO) { airSensY = min(3.0f, airSensY + 0.1f); Serial.printf("# sensY %.2f\n", airSensY); }
        else { tiltGain = min(6.0f, tiltGain + 0.2f); Serial.printf("# tiltGain %.1f px/deg\n", tiltGain); }
        break;
      case 'V':
        vertSource = (VertSource)((vertSource + 1) % 3);
        tiltNeutral = currentTiltDeg();
        tiltFilt = tiltNeutral;
        airAccY = 0;
        Serial.printf("# vertical source -> %s (neutral %.1f deg)\n",
                      VS_NAME[vertSource], tiltNeutral);
        break;
      case 'n':
        tiltNeutral = currentTiltDeg();
        tiltFilt = tiltNeutral;
        Serial.printf("# tilt neutral re-zeroed at %.1f deg\n", tiltNeutral);
        break;
      case 'd':
        tiltDead = (tiltDead >= 10.0f) ? 2.0f : tiltDead + 2.0f;
        Serial.printf("# tilt deadzone %.0f deg\n", tiltDead);
        break;
      case 'X': airAxisX = (airAxisX + 1) % 3; Serial.printf("# cursor X <- %s\n", AXIS_NAME[airAxisX]); break;
      case 'Y': airAxisY = (airAxisY + 1) % 3; Serial.printf("# cursor Y <- %s\n", AXIS_NAME[airAxisY]); break;
      case 'T': captureGesture(6000); break;
      case 'f': airFrozen = !airFrozen; Serial.printf("# motion %s\n", airFrozen ? "FROZEN" : "live"); break;
      case 'g': {
        float gx, gy, gz;
        M5.Imu.getGyroData(&gx, &gy, &gz);
        Serial.printf("# gyro raw %.1f %.1f %.1f  corrected %.1f %.1f %.1f dps\n",
                      gx, gy, gz, gx - gyroBiasX, gy - gyroBiasY, gz - gyroBiasZ);
        break;
      }
      case 'M':   // jump straight to the air mouse profile
        profileIdx = PROFILE_AIRMOUSE;
        airAccX = airAccY = 0; airFrozen = false;
        if (!airCalibrated) calibrateGyro();
        Serial.printf("# profile -> %s\n", PROFILES[profileIdx].name);
        break;
      default: break;
    }
  }

  // BtnA: hold 1s switches profile, a short press fires the action
  if (M5.BtnA.isPressed() && !btnAHandled && M5.BtnA.pressedFor(1000)) {
    nextProfile();
    btnAHandled = true;
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnAHandled) fire(PROFILES[profileIdx].a);
    btnAHandled = false;
  }

  // BtnB: short press fires the action; in air mouse mode holding it 1s
  // freezes the cursor so you can put the device down without it wandering.
  if (profileIdx == PROFILE_AIRMOUSE) {
    if (M5.BtnB.isPressed() && !btnBHandled && M5.BtnB.pressedFor(1000)) {
      airFrozen = !airFrozen;
      Serial.printf("# motion %s\n", airFrozen ? "FROZEN" : "live");
      btnBHandled = true;
    }
    if (M5.BtnB.wasReleased()) {
      if (!btnBHandled) fire(PROFILES[profileIdx].b);
      btnBHandled = false;
    }
  } else {
    if (M5.BtnB.wasPressed()) fire(PROFILES[profileIdx].b);
  }

  if (profileIdx == PROFILE_AIRMOUSE && ble.isConnected() &&
      now - lastAirMs >= AIR_PERIOD_MS) {
    lastAirMs = now;
    airMouseTick();
  }

  bool conn = ble.isConnected();
  if (conn != wasConnected) {
    Serial.printf("# BLE %s\n", conn ? "connected" : "disconnected, advertising again");
    wasConnected = conn;
  }

  if (now - lastBattMs >= 30000) {
    lastBattMs = now;
    ble.setBatteryLevel((uint8_t)batPercent(M5.Axp.GetBatVoltage()));
  }

  if (now - lastDrawMs >= 100) {
    lastDrawMs = now;
    render();
  }
}
