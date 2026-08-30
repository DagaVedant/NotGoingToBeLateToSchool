/*
  NotGoingToBeLateToSchool
  9 key alarm clock on a Seeed XIAO ESP32C3

  the display setup here comes from the blare firmware guide:
  https://blare.hackclub.com/docs/firmware

  what works right now:
    - wifi + ntp so it knows the time
    - clock face
    - 3x3 key matrix
    - set an alarm, it rings with the buzzer
    - screen fades up starting 15 min before the alarm
    - 3 snoozes max, then you have to type the code

  not done yet: ble spotify remote, iphone notifications, the info pages
*/

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include "secrets.h"

// ---------- pins ----------
// see the readme for the full pin map

#define TFT_SCLK 3   // D1, labeled SCL on the screen
#define TFT_MOSI 4   // D2, labeled SDA on the screen
#define TFT_DC   5   // D3
#define TFT_CS   6   // D4
#define TFT_RST  -1  // tied to 3v3 on the board, so -1 = not connected
// BL is tied to gnd, so the backlight is just always on

#define BUZZER 7     // D5

int rowPins[3] = {2, 9, 8};      // ROW1 ROW2 ROW3  =  D0 D9 D8
int colPins[3] = {21, 20, 10};   // COL1 COL2 COL3  =  D6 D7 D10

// key numbering, 0 to 8, left to right then top to bottom:
//   0 HOME   1 UP     2 BACK
//   3 LEFT   4 OK     5 RIGHT
//   6 ALARM  7 DOWN   8 SNOOZE
// in code entry the same keys are digits 1 to 9

#define KEY_HOME 0
#define KEY_UP 1
#define KEY_BACK 2
#define KEY_LEFT 3
#define KEY_OK 4
#define KEY_RIGHT 5
#define KEY_ALARM 6
#define KEY_DOWN 7
#define KEY_SNOOZE 8

// ---------- display ----------
// the plain Adafruit_ST7789 class hides setOffsets(), so this subclass
// exposes it. straight out of the blare guide.

class MyST7789 : public Adafruit_ST7789 {
public:
  MyST7789(int8_t cs, int8_t dc, int8_t mosi, int8_t sclk, int8_t rst)
    : Adafruit_ST7789(cs, dc, mosi, sclk, rst) {}
  void setOffsets(uint8_t col, uint8_t row) {
    _colstart = _colstart2 = col;
    _rowstart = _rowstart2 = row;
  }
};

MyST7789 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// ---------- state ----------

Preferences prefs;

int mode = 0;            // 0 clock, 1 ringing, 2 snoozed, 3 code entry, 4 setting alarm
int alarmHour = 7;
int alarmMin = 0;
bool alarmOn = false;

int snoozeCount = 0;
unsigned long snoozeUntil = 0;
int snoozeMinutes = 9;

int code[4] = {1, 2, 3, 4};
int typed[4];
int typedCount = 0;

bool keyDown[9];
bool keyWas[9];
int justPressed = -1;

unsigned long lastScan = 0;
unsigned long lastDraw = 0;
unsigned long lastBeep = 0;
bool beepOn = false;

int lastMinShown = -1;

// ---------- setup ----------

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 3; i++) {
    pinMode(rowPins[i], INPUT_PULLUP);
    pinMode(colPins[i], OUTPUT);
    digitalWrite(colPins[i], HIGH);
  }
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  tft.init(76, 284);          // our panel size, portrait
  tft.setOffsets(82, 18);     // offsets for the weird resolution
  tft.invertDisplay(false);
  tft.setRotation(1);         // landscape, use 3 if it comes out upside down
  tft.fillScreen(ST77XX_BLACK);
  Serial.println("TFT Initialized!");

  prefs.begin("clock", false);
  alarmHour = prefs.getInt("ah", 7);
  alarmMin = prefs.getInt("am", 0);
  alarmOn = prefs.getBool("aon", false);

  showMessage("connecting");
  startWifi();
}

void startWifi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org");
    setenv("TZ", TZ_STRING, 1);
    tzset();
    showMessage("syncing");
  } else {
    showMessage("no wifi");
  }
}

// ---------- key matrix ----------
// drive one column low at a time and read the three rows.
// a pressed key pulls its row low through its diode.

void scanKeys() {
  justPressed = -1;

  for (int c = 0; c < 3; c++) {
    digitalWrite(colPins[c], LOW);
    delayMicroseconds(50);

    for (int r = 0; r < 3; r++) {
      int key = r * 3 + c;
      keyDown[key] = (digitalRead(rowPins[r]) == LOW);
    }

    digitalWrite(colPins[c], HIGH);
  }

  for (int k = 0; k < 9; k++) {
    if (keyDown[k] && !keyWas[k]) justPressed = k;
    keyWas[k] = keyDown[k];
  }
}

// ---------- time helpers ----------

bool getNow(int &h, int &m, int &s) {
  struct tm t;
  if (!getLocalTime(&t, 100)) return false;
  h = t.tm_hour;
  m = t.tm_min;
  s = t.tm_sec;
  return true;
}

// how many minutes until the alarm, or -1 if the alarm is off
int minutesToAlarm(int h, int m) {
  if (!alarmOn) return -1;
  int now = h * 60 + m;
  int target = alarmHour * 60 + alarmMin;
  int diff = target - now;
  if (diff < 0) diff += 24 * 60;
  return diff;
}

// ---------- drawing ----------

void showMessage(const char *msg) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);
  tft.setCursor(0, 20);
  tft.print(msg);
}

// the sunrise ramp. the backlight is hardwired on, so instead of pwm
// we just draw in a brighter grey as the alarm gets closer.
uint16_t clockColour(int minsAway) {
  int level = 60;                       // normal brightness
  if (minsAway >= 0 && minsAway <= 15) {
    level = 60 + (15 - minsAway) * 13;  // fades up to about 255
    if (level > 255) level = 255;
  }
  return tft.color565(level, level, level);
}

void drawClock() {
  int h, m, s;
  if (!getNow(h, m, s)) {
    showMessage("syncing");
    return;
  }

  if (m == lastMinShown && mode == 0) return;   // only redraw when it changes
  lastMinShown = m;

  int away = minutesToAlarm(h, m);

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(clockColour(away));
  tft.setTextSize(6);
  tft.setCursor(4, 12);
  if (h < 10) tft.print("0");
  tft.print(h);
  tft.print(":");
  if (m < 10) tft.print("0");
  tft.print(m);

  if (alarmOn) {
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(200, 20);
    tft.print("alarm ");
    if (alarmHour < 10) tft.print("0");
    tft.print(alarmHour);
    tft.print(":");
    if (alarmMin < 10) tft.print("0");
    tft.print(alarmMin);
  }
}

void drawRinging() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(4);
  tft.setCursor(4, 6);
  tft.print("WAKE UP");
  tft.setTextSize(1);
  tft.setCursor(4, 50);
  if (snoozeCount < 2) {
    tft.print("OK = stop   SNOOZE = ");
    tft.print(snoozeMinutes);
    tft.print(" more min");
  } else {
    tft.print("OK = stop   no snoozes left");
  }
}

void drawCodeEntry() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(4, 6);
  tft.print("type the code");
  tft.setTextSize(4);
  tft.setCursor(4, 34);
  for (int i = 0; i < 4; i++) {
    if (i < typedCount) tft.print("*");
    else tft.print("_");
    tft.print(" ");
  }
}

void drawSetAlarm() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(4, 4);
  tft.print("set alarm");
  tft.setTextSize(4);
  tft.setCursor(4, 30);
  if (alarmHour < 10) tft.print("0");
  tft.print(alarmHour);
  tft.print(":");
  if (alarmMin < 10) tft.print("0");
  tft.print(alarmMin);
  tft.setTextSize(1);
  tft.setCursor(150, 40);
  tft.print(alarmOn ? "on" : "off");
}

// ---------- buzzer ----------
// gets faster the longer it rings

void updateBuzzer(unsigned long ringingFor) {
  int gap = 600;
  if (ringingFor > 30000) gap = 300;
  if (ringingFor > 60000) gap = 150;

  if (millis() - lastBeep > (unsigned long)gap) {
    lastBeep = millis();
    beepOn = !beepOn;
    if (beepOn) tone(BUZZER, 2000);
    else noTone(BUZZER);
  }
}

void stopBuzzer() {
  noTone(BUZZER);
  beepOn = false;
}

// ---------- main loop ----------

unsigned long ringStarted = 0;

void loop() {
  if (millis() - lastScan > 20) {
    lastScan = millis();
    scanKeys();
  }

  int h, m, s;
  bool haveTime = getNow(h, m, s);

  // should the alarm go off?
  if (mode == 0 && haveTime && alarmOn) {
    if (h == alarmHour && m == alarmMin && s < 2) {
      mode = 1;
      ringStarted = millis();
      snoozeCount = 0;
      drawRinging();
    }
  }

  // snooze finished
  if (mode == 2 && millis() > snoozeUntil) {
    mode = 1;
    ringStarted = millis();
    drawRinging();
  }

  if (mode == 1) {
    updateBuzzer(millis() - ringStarted);

    if (justPressed == KEY_OK) {
      stopBuzzer();
      mode = 0;
      snoozeCount = 0;
      lastMinShown = -1;
    }
    else if (justPressed == KEY_SNOOZE) {
      if (snoozeCount < 2) {
        snoozeCount++;
        stopBuzzer();
        snoozeUntil = millis() + (unsigned long)snoozeMinutes * 60000UL;
        mode = 2;
        showMessage("snoozed");
      } else {
        // third snooze, you have to type the code now
        typedCount = 0;
        mode = 3;
        drawCodeEntry();
      }
    }
  }

  else if (mode == 3) {
    updateBuzzer(millis() - ringStarted);

    if (justPressed >= 0) {
      typed[typedCount] = justPressed + 1;   // key 0 is digit 1
      typedCount++;
      drawCodeEntry();

      if (typedCount == 4) {
        bool ok = true;
        for (int i = 0; i < 4; i++) {
          if (typed[i] != code[i]) ok = false;
        }
        if (ok) {
          stopBuzzer();
          mode = 0;
          snoozeCount = 0;
          lastMinShown = -1;
        } else {
          typedCount = 0;
          showMessage("wrong");
          delay(600);
          drawCodeEntry();
        }
      }
    }
  }

  else if (mode == 4) {
    if (justPressed == KEY_UP) { alarmHour = (alarmHour + 1) % 24; drawSetAlarm(); }
    if (justPressed == KEY_DOWN) { alarmHour = (alarmHour + 23) % 24; drawSetAlarm(); }
    if (justPressed == KEY_RIGHT) { alarmMin = (alarmMin + 5) % 60; drawSetAlarm(); }
    if (justPressed == KEY_LEFT) { alarmMin = (alarmMin + 55) % 60; drawSetAlarm(); }
    if (justPressed == KEY_ALARM) { alarmOn = !alarmOn; drawSetAlarm(); }

    if (justPressed == KEY_OK || justPressed == KEY_HOME) {
      prefs.putInt("ah", alarmHour);
      prefs.putInt("am", alarmMin);
      prefs.putBool("aon", alarmOn);
      mode = 0;
      lastMinShown = -1;
    }
  }

  else {   // mode 0 or 2, normal clock
    if (justPressed == KEY_ALARM) {
      mode = 4;
      drawSetAlarm();
    }
    if (mode == 0 && millis() - lastDraw > 500) {
      lastDraw = millis();
      drawClock();
    }
  }
}
