/*
  NotGoingToBeLateToSchool
  9 key alarm clock on a Seeed XIAO ESP32C3

  the display setup here comes from the blare firmware guide:
  https://blare.hackclub.com/docs/firmware
  the spinning globe is the same animation as the oled on my hackpad.

  what works:
    - wifi + ntp so it knows the time
    - clock face with the spinning globe
    - 3x3 key matrix
    - 4 alarms, each with its own days of the week, all editable on the device
    - rings with an escalating buzzer
    - screen fades up starting 15 min before an alarm
    - 3 snoozes max, then you have to type the code
    - info pages: weather, hackatime, now playing, world clocks
    - all four pages come from one json off the relay, one request a minute
    - pages auto rotate, or arrow through them yourself

  ble is written but switched off, see ENABLE_BLE below. it needs the
  NimBLE-Arduino library and a real board to test on, i have neither yet.

  libraries: Adafruit GFX, Adafruit ST7735/ST7789, ArduinoJson 7
*/

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include "secrets.h"
#include "globe.h"

#define ENABLE_BLE 0    // 1 = build the spotify remote and iphone notifications

// ---------- pins ----------
// see the readme for the full pin map

#define TFT_SCLK 3   // D1, labeled SCL on the screen
#define TFT_MOSI 4   // D2, labeled SDA on the screen
#define TFT_DC   5   // D3
#define TFT_CS   6   // D4
#define TFT_RST  -1  // tied to 3v3 on the board, so -1 = not connected
// BL is tied to gnd, so the backlight is just always on
#define BUZZER 7     // D5

// these have to match the board. rows are D0 D10 D9, columns are D6 D7 D8.
int rowPins[3] = {2, 10, 9};     // top to bottom
int colPins[3] = {21, 20, 8};    // left to right

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

// where the globe sits on the clock face
#define GLOBE_X 8
#define GLOBE_Y 22

// ---------- modes ----------

#define MODE_CLOCK 0
#define MODE_RING 1
#define MODE_SNOOZE 2
#define MODE_CODE 3
#define MODE_LIST 4      // list of the 4 alarms
#define MODE_EDIT 5      // editing one alarm

int mode = MODE_CLOCK;

// ---------- alarms ----------
// days is a bit per weekday, bit 0 = sunday, same order as tm_wday.
// days == 0 means every day.

struct Alarm {
  uint8_t hour;
  uint8_t minute;
  uint8_t days;
  uint8_t on;
};

#define NALARMS 4
Alarm alarms[NALARMS];

int listSel = 0;     // which alarm the list is pointing at
int editSel = 0;     // which alarm we are editing
int editField = 0;   // 0 hour, 1 minute, 2 days, 3 on/off
int daySel = 0;      // which day the days field is pointing at
int ringingAlarm = -1;

const char *dayLetters[7] = {"S", "M", "T", "W", "T", "F", "S"};

Preferences prefs;

// ---------- snooze and code ----------

int snoozeCount = 0;
unsigned long snoozeUntil = 0;
int snoozeMinutes = 9;

int code[4] = {1, 2, 3, 4};
int typed[4];
int typedCount = 0;

// ---------- keys ----------

bool keyDown[9];
bool keyWas[9];
int justPressed = -1;

// ---------- timers ----------

unsigned long lastScan = 0;
unsigned long lastBeep = 0;
unsigned long lastGlobe = 0;
unsigned long lastFetch = 0;
unsigned long lastPageFlip = 0;
unsigned long lastKeyAt = 0;
unsigned long ringStarted = 0;
bool beepOn = false;

int globeFrame = 0;
int lastMinShown = -1;
int lastAwayShown = -999;

// ---------- info pages ----------
// 0 is the clock, the rest come off the relay

#define PAGE_CLOCK 0
#define PAGE_WEATHER 1
#define PAGE_HACKATIME 2
#define PAGE_PLAYING 3
#define PAGE_WORLD 4
#define NPAGES 5

int page = PAGE_CLOCK;
bool pageDirty = true;

// what the relay gave us last time
struct Info {
  bool ok;
  int   temp;
  int   hi;
  int   lo;
  char  sky[24];
  int   todayMin;
  int   weekMin;
  bool  playing;
  char  title[40];
  char  artist[40];
  char  cityName[3][10];
  int   cityOffset[3];   // minutes from utc
  int   cityCount;
  char  note[64];        // last notification, from ble
  bool  noteFresh;
};

Info info;

// ---------- forward declarations ----------

void showMessage(const char *msg);
void startWifi();
void loadAlarms();
void saveAlarms();
void drawList();
void drawEdit();
void drawRinging();
void drawCodeEntry();
bool bleConnected();
void blePlay();
void bleHookAncs();
#if ENABLE_BLE
void bleStart();
#endif

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

  memset(&info, 0, sizeof(info));
  loadAlarms();

  showMessage("connecting");
  startWifi();

#if ENABLE_BLE
  bleStart();
#endif

  pageDirty = true;
}

// ---------- saving ----------
// the four alarms go into one blob so it is a single read and write

void loadAlarms() {
  prefs.begin("clock", false);
  size_t got = prefs.getBytes("alarms", alarms, sizeof(alarms));
  if (got != sizeof(alarms)) {
    for (int i = 0; i < NALARMS; i++) {
      alarms[i].hour = 7;
      alarms[i].minute = 0;
      alarms[i].days = 0;
      alarms[i].on = 0;
    }
    alarms[0].hour = 6;
    alarms[0].minute = 30;
    alarms[0].days = 0b0111110;   // mon to fri
  }
  for (int i = 0; i < 4; i++) {
    char k[4] = {'c', (char)('0' + i), 0, 0};
    code[i] = prefs.getInt(k, code[i]);
  }
}

void saveAlarms() {
  prefs.putBytes("alarms", alarms, sizeof(alarms));
}

// ---------- wifi ----------

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
    if (keyDown[k] && !keyWas[k]) {
      justPressed = k;
      lastKeyAt = millis();
    }
    keyWas[k] = keyDown[k];
  }
}

// ---------- time helpers ----------

bool getNow(int &h, int &m, int &s, int &wday) {
  struct tm t;
  if (!getLocalTime(&t, 100)) return false;
  h = t.tm_hour;
  m = t.tm_min;
  s = t.tm_sec;
  wday = t.tm_wday;
  return true;
}

bool alarmRunsToday(const Alarm &a, int wday) {
  if (!a.on) return false;
  if (a.days == 0) return true;             // 0 means every day
  return (a.days >> wday) & 1;
}

// minutes until the soonest armed alarm, or -1 if none.
// only looks at today and tomorrow, which is all the sunrise ramp needs.
int minutesToAlarm(int h, int m, int wday) {
  int now = h * 60 + m;
  int best = -1;

  for (int d = 0; d < 2; d++) {
    int day = (wday + d) % 7;
    for (int i = 0; i < NALARMS; i++) {
      if (!alarmRunsToday(alarms[i], day)) continue;
      int t = alarms[i].hour * 60 + alarms[i].minute + d * 1440;
      int diff = t - now;
      if (diff < 0) continue;
      if (best < 0 || diff < best) best = diff;
    }
  }
  return best;
}

// which alarm should be going off right now, or -1
int alarmDueNow(int h, int m, int s, int wday) {
  if (s >= 2) return -1;
  for (int i = 0; i < NALARMS; i++) {
    if (!alarmRunsToday(alarms[i], wday)) continue;
    if (alarms[i].hour == h && alarms[i].minute == m) return i;
  }
  return -1;
}

// ---------- the relay ----------
// one https get a minute, one json back, everything on it. building four
// api calls into the clock would not fit in flash and would need four sets
// of keys on the device, so the relay does it and hands over the answer.
//
// what it has to send back:
// {
//   "weather":   {"temp":12,"hi":15,"lo":8,"sky":"light rain"},
//   "hackatime": {"today":143,"week":812},
//   "playing":   {"is":true,"title":"...","artist":"..."},
//   "clocks":    [{"name":"UTC","offset":0},{"name":"SF","offset":-480}]
// }
// temps in celsius, hackatime in minutes, clock offsets in minutes from utc.

void fetchInfo() {
  if (WiFi.status() != WL_CONNECTED) { info.ok = false; return; }

  WiFiClientSecure net;
  net.setInsecure();          // the relay is mine and it is read only
  HTTPClient http;
  http.setTimeout(6000);
  if (!http.begin(net, RELAY_URL)) { info.ok = false; return; }

  int codeHttp = http.GET();
  if (codeHttp != 200) { http.end(); info.ok = false; return; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { info.ok = false; return; }

  info.temp = doc["weather"]["temp"] | 0;
  info.hi   = doc["weather"]["hi"] | 0;
  info.lo   = doc["weather"]["lo"] | 0;
  strlcpy(info.sky, doc["weather"]["sky"] | "", sizeof(info.sky));

  info.todayMin = doc["hackatime"]["today"] | 0;
  info.weekMin  = doc["hackatime"]["week"] | 0;

  info.playing = doc["playing"]["is"] | false;
  strlcpy(info.title,  doc["playing"]["title"] | "", sizeof(info.title));
  strlcpy(info.artist, doc["playing"]["artist"] | "", sizeof(info.artist));

  info.cityCount = 0;
  for (JsonObject c : doc["clocks"].as<JsonArray>()) {
    if (info.cityCount >= 3) break;
    strlcpy(info.cityName[info.cityCount], c["name"] | "", 10);
    info.cityOffset[info.cityCount] = c["offset"] | 0;
    info.cityCount++;
  }

  info.ok = true;
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

void print2(int v) {
  if (v < 10) tft.print("0");
  tft.print(v);
}

// header used by all the info pages
void pageHeader(const char *name) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(tft.color565(90, 90, 90));
  tft.setCursor(6, 6);
  tft.print(name);
}

// only the globe moves most of the time, so it gets redrawn on its own
void drawGlobe(uint16_t colour) {
  tft.drawBitmap(GLOBE_X, GLOBE_Y, globe[globeFrame],
                 GLOBE_SIZE, GLOBE_SIZE, colour, ST77XX_BLACK);
}

void drawClockFace(bool full) {
  int h, m, s, wday;
  if (!getNow(h, m, s, wday)) {
    showMessage("syncing");
    return;
  }

  int away = minutesToAlarm(h, m, wday);
  uint16_t col = clockColour(away);

  // the whole face only gets repainted when the minute or the ramp changes
  if (full || m != lastMinShown || away != lastAwayShown) {
    lastMinShown = m;
    lastAwayShown = away;

    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(col);
    tft.setTextSize(6);
    tft.setCursor(50, 14);
    print2(h);
    tft.print(":");
    print2(m);

    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);

    int next = -1;
    for (int i = 0; i < NALARMS; i++) if (alarms[i].on) { next = i; break; }
    tft.setCursor(238, 20);
    if (next >= 0 && away >= 0) {
      print2(away / 60);
      tft.print("h");
      print2(away % 60);
      tft.setCursor(238, 34);
      tft.print("to go");
    } else {
      tft.print("no alarm");
    }

    if (info.noteFresh) {
      tft.setTextColor(tft.color565(255, 190, 60));
      tft.setCursor(50, 62);
      tft.print(info.note);
    }
  }

  drawGlobe(col);
}

void drawWeather() {
  pageHeader("weather");
  tft.setTextColor(ST77XX_WHITE);
  if (!info.ok) { tft.setTextSize(2); tft.setCursor(6, 32); tft.print("no data"); return; }

  tft.setTextSize(5);
  tft.setCursor(6, 22);
  tft.print(info.temp);
  tft.print("C");

  tft.setTextSize(1);
  tft.setCursor(150, 26);
  tft.print(info.sky);
  tft.setCursor(150, 42);
  tft.print("hi ");
  tft.print(info.hi);
  tft.print("  lo ");
  tft.print(info.lo);
}

void drawHackatime() {
  pageHeader("hackatime");
  tft.setTextColor(ST77XX_WHITE);
  if (!info.ok) { tft.setTextSize(2); tft.setCursor(6, 32); tft.print("no data"); return; }

  tft.setTextSize(5);
  tft.setCursor(6, 22);
  tft.print(info.todayMin / 60);
  tft.print("h");
  print2(info.todayMin % 60);

  tft.setTextSize(1);
  tft.setCursor(180, 26);
  tft.print("today");
  tft.setCursor(180, 42);
  tft.print("week ");
  tft.print(info.weekMin / 60);
  tft.print("h");
}

void drawPlaying() {
  pageHeader("now playing");
  tft.setTextColor(ST77XX_WHITE);
  if (!info.ok || !info.playing) {
    tft.setTextSize(2);
    tft.setCursor(6, 32);
    tft.print("nothing playing");
    return;
  }
  tft.setTextSize(2);
  tft.setCursor(6, 24);
  tft.print(info.title);
  tft.setTextSize(1);
  tft.setTextColor(tft.color565(140, 140, 140));
  tft.setCursor(6, 48);
  tft.print(info.artist);
}

void drawWorld() {
  pageHeader("world");
  tft.setTextColor(ST77XX_WHITE);

  time_t raw = time(nullptr);
  struct tm utc;
  gmtime_r(&raw, &utc);
  int utcMin = utc.tm_hour * 60 + utc.tm_min;

  if (!info.ok || info.cityCount == 0) {
    tft.setTextSize(2); tft.setCursor(6, 32); tft.print("no data"); return;
  }

  int x = 6;
  for (int i = 0; i < info.cityCount; i++) {
    int t = ((utcMin + info.cityOffset[i]) % 1440 + 1440) % 1440;
    tft.setTextSize(1);
    tft.setTextColor(tft.color565(140, 140, 140));
    tft.setCursor(x, 22);
    tft.print(info.cityName[i]);
    tft.setTextSize(3);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(x, 38);
    print2(t / 60);
    tft.print(":");
    print2(t % 60);
    x += 95;
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
#if ENABLE_BLE
  tft.setCursor(4, 62);
  tft.print(bleConnected() ? "phone is playing" : "phone not connected");
#endif
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

void drawDays(int x, int y, uint8_t days, int cursor) {
  tft.setTextSize(1);
  for (int d = 0; d < 7; d++) {
    bool set = (days == 0) || ((days >> d) & 1);
    tft.setTextColor(set ? ST77XX_WHITE : tft.color565(70, 70, 70));
    tft.setCursor(x + d * 12, y);
    tft.print(dayLetters[d]);
    if (d == cursor) tft.drawFastHLine(x + d * 12, y + 10, 7, ST77XX_WHITE);
  }
}

void drawList() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(tft.color565(90, 90, 90));
  tft.setCursor(6, 4);
  tft.print("alarms   OK edit   BACK done");

  for (int i = 0; i < NALARMS; i++) {
    int y = 18 + i * 14;
    tft.setTextColor(i == listSel ? ST77XX_WHITE : tft.color565(110, 110, 110));
    tft.setCursor(6, y);
    tft.print(i == listSel ? ">" : " ");
    tft.setCursor(18, y);
    print2(alarms[i].hour);
    tft.print(":");
    print2(alarms[i].minute);
    tft.setCursor(70, y);
    tft.print(alarms[i].on ? "on " : "off");
    drawDays(110, y, alarms[i].days, -1);
  }
}

void drawEdit() {
  Alarm &a = alarms[editSel];
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(tft.color565(90, 90, 90));
  tft.setCursor(6, 4);
  tft.print("LEFT RIGHT pick field   UP DOWN change   ALARM toggles a day   OK save");

  tft.setTextSize(4);
  tft.setTextColor(editField == 0 ? ST77XX_WHITE : tft.color565(110, 110, 110));
  tft.setCursor(6, 22);
  print2(a.hour);
  tft.setTextColor(tft.color565(110, 110, 110));
  tft.print(":");
  tft.setTextColor(editField == 1 ? ST77XX_WHITE : tft.color565(110, 110, 110));
  print2(a.minute);

  drawDays(140, 30, a.days, editField == 2 ? daySel : -1);

  tft.setTextSize(2);
  tft.setTextColor(editField == 3 ? ST77XX_WHITE : tft.color565(110, 110, 110));
  tft.setCursor(230, 26);
  tft.print(a.on ? "on" : "off");
}

void drawPage(bool full) {
  switch (page) {
    case PAGE_CLOCK:     drawClockFace(full); break;
    case PAGE_WEATHER:   drawWeather(); break;
    case PAGE_HACKATIME: drawHackatime(); break;
    case PAGE_PLAYING:   drawPlaying(); break;
    case PAGE_WORLD:     drawWorld(); break;
  }
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

// ---------- ble ----------
// the phone side of the clock. two jobs:
//   1. act as a bluetooth media remote so the alarm can hit play on spotify
//      instead of just screaming at me
//   2. read notifications off the iphone over ancs and put the last one on
//      the clock face
// both are off by default, see ENABLE_BLE at the top. this needs
// NimBLE-Arduino and a board to test on.

#if ENABLE_BLE
#include <NimBLEDevice.h>

// hid report map for a consumer control device, which is all we need to
// send play, pause, next and previous
static const uint8_t hidReportMap[] = {
  0x05, 0x0C,        // usage page: consumer
  0x09, 0x01,        // usage: consumer control
  0xA1, 0x01,        // collection: application
  0x85, 0x01,        //   report id 1
  0x15, 0x00,        //   logical min 0
  0x25, 0x01,        //   logical max 1
  0x75, 0x01,        //   report size 1
  0x95, 0x08,        //   report count 8
  0x09, 0xCD,        //   play / pause
  0x09, 0xB5,        //   next track
  0x09, 0xB6,        //   previous track
  0x09, 0xE9,        //   volume up
  0x09, 0xEA,        //   volume down
  0x09, 0xE2,        //   mute
  0x0A, 0x23, 0x02,  //   home
  0x0A, 0x24, 0x02,  //   back
  0x81, 0x02,        //   input: data, variable, absolute
  0xC0               // end collection
};

// apple notification center service, on the phone not on us
static NimBLEUUID ancsService("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static NimBLEUUID ancsNotifSrc("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static NimBLEUUID ancsCtrlPoint("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static NimBLEUUID ancsDataSrc("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");

static NimBLEHIDDevice *hid = nullptr;
static NimBLECharacteristic *hidInput = nullptr;
static NimBLEServer *server = nullptr;
static bool bleLinked = false;
static uint32_t pendingUid = 0;

bool bleConnected() { return bleLinked; }

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
    bleLinked = true;
    s->updateConnParams(info.getConnHandle(), 12, 24, 0, 200);
  }
  void onDisconnect(NimBLEServer *s, NimBLEConnInfo &info, int reason) override {
    bleLinked = false;
    NimBLEDevice::startAdvertising();
  }
};

// the notification source tells us an event happened and gives a uid.
// to get the actual text we ask the control point for the title and
// message, and the answer comes back on the data source.
static void onNotifSrc(NimBLERemoteCharacteristic *c, uint8_t *d, size_t len, bool isNotify) {
  if (len < 8) return;
  uint8_t event = d[0];              // 0 added, 1 modified, 2 removed
  if (event != 0) return;
  pendingUid = (uint32_t)d[4] | ((uint32_t)d[5] << 8) |
               ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);

  NimBLEClient *cl = NimBLEDevice::getClientByPeerAddress(c->getRemoteService()->getClient()->getPeerAddress());
  if (!cl) return;
  NimBLERemoteService *svc = cl->getService(ancsService);
  if (!svc) return;
  NimBLERemoteCharacteristic *cp = svc->getCharacteristic(ancsCtrlPoint);
  if (!cp) return;

  // command 0 = get notification attributes, ask for title and message
  uint8_t req[] = {
    0x00,
    (uint8_t)(pendingUid), (uint8_t)(pendingUid >> 8),
    (uint8_t)(pendingUid >> 16), (uint8_t)(pendingUid >> 24),
    0x01, 0x18, 0x00,   // title, up to 24 bytes
    0x03, 0x20, 0x00    // message, up to 32 bytes
  };
  cp->writeValue(req, sizeof(req), true);
}

// the reply is: command, uid, then for each attribute an id, a 16 bit
// length and that many bytes of utf8
static void onDataSrc(NimBLERemoteCharacteristic *c, uint8_t *d, size_t len, bool isNotify) {
  if (len < 5) return;
  size_t i = 5;
  char title[26] = {0};
  char body[34] = {0};

  while (i + 3 <= len) {
    uint8_t id = d[i];
    uint16_t l = (uint16_t)d[i + 1] | ((uint16_t)d[i + 2] << 8);
    i += 3;
    if (i + l > len) break;
    if (id == 0x01 && l < sizeof(title)) memcpy(title, d + i, l);
    if (id == 0x03 && l < sizeof(body)) memcpy(body, d + i, l);
    i += l;
  }

  if (title[0] || body[0]) {
    snprintf(info.note, sizeof(info.note), "%s %s", title, body);
    info.noteFresh = true;
    lastMinShown = -1;              // force the clock face to repaint
  }
}

// once the phone has bonded with us it exposes ancs, so go and subscribe
void bleHookAncs() {
  if (!bleLinked || !server) return;
  NimBLEClient *cl = NimBLEDevice::getClientByPeerAddress(
      server->getPeerInfo(0).getAddress());
  if (!cl) {
    cl = NimBLEDevice::createClient();
    if (!cl->connect(server->getPeerInfo(0).getAddress(), false)) return;
  }
  NimBLERemoteService *svc = cl->getService(ancsService);
  if (!svc) return;

  NimBLERemoteCharacteristic *ds = svc->getCharacteristic(ancsDataSrc);
  if (ds && ds->canNotify()) ds->subscribe(true, onDataSrc);

  NimBLERemoteCharacteristic *ns = svc->getCharacteristic(ancsNotifSrc);
  if (ns && ns->canNotify()) ns->subscribe(true, onNotifSrc);
}

void bleStart() {
  NimBLEDevice::init("NotLate Clock");
  NimBLEDevice::setSecurityAuth(true, true, true);   // bond, mitm, secure
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  hid = new NimBLEHIDDevice(server);
  hidInput = hid->getInputReport(1);
  hid->setManufacturer("vedant");
  hid->setPnp(0x02, 0xE502, 0xA111, 0x0210);
  hid->setHidInfo(0x00, 0x01);
  hid->setReportMap((uint8_t *)hidReportMap, sizeof(hidReportMap));
  hid->startServices();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(HID_KEYBOARD);
  adv->addServiceUUID(hid->getHidService()->getUUID());
  // asking the phone to show us its notification service
  adv->addServiceUUID(ancsService);
  adv->start();
}

// tap one of the consumer control bits, then release
void bleTap(uint8_t bit) {
  if (!bleLinked || !hidInput) return;
  uint8_t on = bit, off = 0;
  hidInput->setValue(&on, 1);
  hidInput->notify();
  delay(30);
  hidInput->setValue(&off, 1);
  hidInput->notify();
}

void blePlay() { bleTap(0x01); }   // play / pause is bit 0

#else
bool bleConnected() { return false; }
void blePlay() {}
void bleHookAncs() {}
#endif

// ---------- ringing ----------

void startRinging(int which) {
  ringingAlarm = which;
  mode = MODE_RING;
  ringStarted = millis();
  snoozeCount = 0;
  blePlay();            // wake up to music if the phone is connected
  drawRinging();
}

void stopRinging() {
  stopBuzzer();
  mode = MODE_CLOCK;
  page = PAGE_CLOCK;
  snoozeCount = 0;
  ringingAlarm = -1;
  lastMinShown = -1;
  pageDirty = true;
}

// ---------- main loop ----------

void loop() {
  if (millis() - lastScan > 20) {
    lastScan = millis();
    scanKeys();
  }

  int h, m, s, wday;
  bool haveTime = getNow(h, m, s, wday);

  // pull the relay once a minute, but never while the alarm is going
  if (mode == MODE_CLOCK && millis() - lastFetch > 60000UL) {
    lastFetch = millis();
    fetchInfo();
    if (page != PAGE_CLOCK) pageDirty = true;
  }

  // has an alarm come due
  if (mode == MODE_CLOCK && haveTime) {
    int due = alarmDueNow(h, m, s, wday);
    if (due >= 0) startRinging(due);
  }

  // snooze finished
  if (mode == MODE_SNOOZE && millis() > snoozeUntil) {
    mode = MODE_RING;
    ringStarted = millis();
    blePlay();
    drawRinging();
  }

  if (mode == MODE_RING) {
    updateBuzzer(millis() - ringStarted);

    if (justPressed == KEY_OK) {
      stopRinging();
    }
    else if (justPressed == KEY_SNOOZE) {
      if (snoozeCount < 2) {
        snoozeCount++;
        stopBuzzer();
        snoozeUntil = millis() + (unsigned long)snoozeMinutes * 60000UL;
        mode = MODE_SNOOZE;
        showMessage("snoozed");
      } else {
        // third snooze, you have to type the code now
        typedCount = 0;
        mode = MODE_CODE;
        drawCodeEntry();
      }
    }
  }

  else if (mode == MODE_CODE) {
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
          stopRinging();
        } else {
          typedCount = 0;
          showMessage("wrong");
          delay(600);
          drawCodeEntry();
        }
      }
    }
  }

  else if (mode == MODE_LIST) {
    if (justPressed == KEY_DOWN) { listSel = (listSel + 1) % NALARMS; drawList(); }
    if (justPressed == KEY_UP)   { listSel = (listSel + NALARMS - 1) % NALARMS; drawList(); }
    if (justPressed == KEY_ALARM) { alarms[listSel].on = !alarms[listSel].on; saveAlarms(); drawList(); }
    if (justPressed == KEY_OK)   { editSel = listSel; editField = 0; mode = MODE_EDIT; drawEdit(); }
    if (justPressed == KEY_BACK || justPressed == KEY_HOME) {
      saveAlarms();
      mode = MODE_CLOCK;
      page = PAGE_CLOCK;
      lastMinShown = -1;
      pageDirty = true;
    }
  }

  else if (mode == MODE_EDIT) {
    Alarm &a = alarms[editSel];
    bool touched = false;

    if (justPressed == KEY_RIGHT) { editField = (editField + 1) % 4; touched = true; }
    if (justPressed == KEY_LEFT)  { editField = (editField + 3) % 4; touched = true; }

    if (justPressed == KEY_UP || justPressed == KEY_DOWN) {
      int step = (justPressed == KEY_UP) ? 1 : -1;
      if (editField == 0) a.hour = (a.hour + 24 + step) % 24;
      if (editField == 1) a.minute = (a.minute + 60 + step * 5) % 60;
      if (editField == 2) daySel = (daySel + 7 + step) % 7;
      if (editField == 3) a.on = !a.on;
      touched = true;
    }

    // on the days field ALARM flips the day the cursor is under
    if (editField == 2 && justPressed == KEY_ALARM) {
      a.days ^= (1 << daySel);
      touched = true;
    }

    if (justPressed == KEY_OK) {
      saveAlarms();
      mode = MODE_LIST;
      drawList();
    } else if (touched) {
      drawEdit();
    }
  }

  else {   // MODE_CLOCK or MODE_SNOOZE, showing a page
    if (justPressed == KEY_ALARM) {
      listSel = 0;
      mode = MODE_LIST;
      drawList();
    }
    else if (justPressed == KEY_RIGHT) { page = (page + 1) % NPAGES; pageDirty = true; }
    else if (justPressed == KEY_LEFT)  { page = (page + NPAGES - 1) % NPAGES; pageDirty = true; }
    else if (justPressed == KEY_HOME)  { page = PAGE_CLOCK; pageDirty = true; info.noteFresh = false; }

    // auto rotate, but hold still for 20s after you touch a key
    if (millis() - lastKeyAt > 20000UL && millis() - lastPageFlip > 8000UL) {
      lastPageFlip = millis();
      page = (page + 1) % NPAGES;
      pageDirty = true;
    }

    if (page == PAGE_CLOCK) {
      // the globe keeps turning, the rest only repaints when it changes
      if (millis() - lastGlobe > 80) {
        lastGlobe = millis();
        globeFrame = (globeFrame + 1) % GLOBE_FRAMES;
        drawClockFace(pageDirty);
        pageDirty = false;
      }
    } else if (pageDirty) {
      drawPage(true);
      pageDirty = false;
    }
  }

#if ENABLE_BLE
  static unsigned long lastAncs = 0;
  if (bleLinked && millis() - lastAncs > 5000) {
    lastAncs = millis();
    bleHookAncs();
  }
#endif
}
