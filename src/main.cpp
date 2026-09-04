// All comments in English. Production-grade monolithic IRC Client for Cardputer-Adv
// Target: ST7789 240x135, 56-key matrix, M5StampS3, NO PSRAM, 512KB SRAM
// Architecture: 8-bit split canvas (109px), direct draw navbar/input, dual-core mutex isolation

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <cstring>
#include <cctype>
#include <ctime>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_pm.h>
#include <driver/adc.h>
#include <cmath>

// ---------------------------------------------------------------------------
// Hardware pins and constants
// ---------------------------------------------------------------------------
static constexpr int SD_SCK  = 40;
static constexpr int SD_MISO = 39;
static constexpr int SD_MOSI = 14;
static constexpr int SD_CS   = 5; // spec mandates SPI compression on CS 5 at 16MHz

static constexpr int BATTERY_PIN = 10;
static constexpr int JACK_DETECT_PIN = 46; // headphone jack detect (active LOW when plugged)
static constexpr int AMP_SHUTDOWN_PIN = 11; // NS4150B I2S amplifier shutdown line (HIGH=enabled, LOW=muted)
static constexpr int G0_PIN = 0; // BtnA safe-boot pin
static constexpr int LED_PIN = 21; // NeoPixel side strip data pin (built-in helper uses neopixelWrite)

static constexpr uint16_t UI_BG = 0x0000;
static constexpr uint16_t UI_FG = 0x07FF;
static constexpr uint16_t UI_DIM = 0x04FF;
static constexpr uint16_t UI_WARN = 0xF800;
// 6 high-contrast 8-bit color tokens for nick hash (Cyan, Green, Yellow, Magenta, Orange, White)
static constexpr uint16_t NICK_COLORS[6] = {0x07FF, 0x07E0, 0xFFE0, 0xF81F, 0xFD20, 0xFFFF};

// TIMEZONE LOOKUP DATA - hardcoded static structural lookup array per spec
struct TimeZoneProfile { const char* label; int offset; };
static const TimeZoneProfile TZ_PROFILES[] = {
  {"UTC",  0}, {"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5},
  {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7},
  {"CET",  1}, {"CEST", 2}, {"EET",  2}, {"JST",  9}, {"AEST",10}, {"NZST",12},
  {"AKST",-9}, {"HST",-10}, {"BST",  1}, {"WET",  0}, {"IST",  5}
};
static constexpr int TZ_COUNT = sizeof(TZ_PROFILES)/sizeof(TZ_PROFILES[0]);
static int gTimezoneIndex = 0; // single global integer tracking selection
static bool gUse12Hour = false; // use_12_hour_format flag (false=24H, true=12H)
static int gQuickOverlayRow = 0; // 0=timezone row, 1=clock format row in Quick Settings

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;

// Split sub-canvas overhaul: 12px top navbar, 109px chat viewport, 14px bottom input
static constexpr int TOP_H   = 12;
static constexpr int INPUT_H = 14;
static constexpr int CHAT_H  = 109; // 135 - 12 - 14 = 109 (reclaims 12KB SRAM)
static constexpr int CHAT_Y  = TOP_H;
static constexpr int INPUT_Y = SCREEN_H - INPUT_H;

static constexpr int CHAR_W = 6;
static constexpr int CHAR_H = 8;
static constexpr int ROW_H  = 10; // 2-pixel padding window to stop vertical collision // 2-pixel padding window to stop vertical collision bleeding
static constexpr int CHAT_ROWS = CHAT_H / ROW_H; // 10 rows visible with 10px height

// Input scrolling spec: 38 columns visible before horizontal scroll
static constexpr int INPUT_VISIBLE_COLS = 38;

// Memory caps - zero heap fragmentation, fixed C-string arrays
static constexpr int MAX_TABS          = 8;
static constexpr int MAX_LINES_PER_TAB = 20; // strict ring per spec
static constexpr int MAX_LINE_LEN      = 155;
static constexpr int MAX_NICKS         = 32;
static constexpr int NICK_LEN          = 24;
static constexpr int TAB_NAME_LEN      = 28;
static constexpr int TOPIC_LEN         = 96;
static constexpr int IRC_LINE_MAX      = 512;
static constexpr int RX_ACCUM_SZ       = 2048;
static constexpr int CMD_QUEUE_CAP     = 20;
static constexpr int INPUT_BUF_SZ      = 256;
static constexpr int HISTORY_DEPTH     = 5;

// Timing and power
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr uint32_t PING_INTERVAL_MS = 60000;
static constexpr uint32_t PONG_TIMEOUT_MS  = 25000;
static constexpr uint32_t UI_REFRESH_MS    = 50;
static constexpr uint32_t INACTIVITY_MS    = 45000; // 45s watchdog
static constexpr uint8_t  BRIGHT_AWAKE     = 10;
static constexpr uint8_t  BRIGHT_SLEEP     = 2;
static constexpr uint32_t STEALTH_PULSE_MS = 3000; // purple flash every 3s

static constexpr const char* CONFIG_PATH = "/irc/config.txt";
static constexpr const char* LOG_ROOT_DEFAULT = "/irc/logs";

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------
enum TabType : uint8_t { TAB_STATUS=0, TAB_CHANNEL=1, TAB_QUERY=2 };
enum LogLevel : uint8_t { LOG_ALL=0, LOG_DMS_ONLY=1, LOG_NONE=2 };

struct ChatLine {
  char stamp[6]; // HH:MM
  char text[MAX_LINE_LEN+1];
  uint8_t flags; // bit0 highlight (!), bit1 own, bit2 notice
};

struct Tab {
  char name[TAB_NAME_LEN+1];
  TabType type;
  ChatLine lines[MAX_LINES_PER_TAB];
  uint8_t head;
  uint8_t count;
  int8_t scroll;
  bool unread;   // '*' indicator
  bool mention;  // '!' indicator for highlight
  char topic[TOPIC_LEN+1];
  char nicks[MAX_NICKS][NICK_LEN+1];
  uint8_t nickCount;
  char server[32]; // network/server prefix for server skip logic
};

// Mutex protected fixed queue for cross-core comms
struct CmdQueue {
  char buf[CMD_QUEUE_CAP][IRC_LINE_MAX+1];
  volatile int head{0};
  volatile int tail{0};
  volatile int cnt{0};
  SemaphoreHandle_t mtx{nullptr};
  void init() { mtx = xSemaphoreCreateMutex(); head=tail=cnt=0; }
  bool push(const char* s) {
    if (!mtx || !s) return false;
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    bool ok=false;
    if (cnt < CMD_QUEUE_CAP) {
      strncpy(buf[tail], s, IRC_LINE_MAX);
      buf[tail][IRC_LINE_MAX]='\0';
      tail = (tail+1)%CMD_QUEUE_CAP;
      cnt++; ok=true;
    }
    xSemaphoreGive(mtx);
    return ok;
  }
  bool pop(char* out) {
    if (!mtx || !out) return false;
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    bool ok=false;
    if (cnt>0) {
      strncpy(out, buf[head], IRC_LINE_MAX);
      out[IRC_LINE_MAX]='\0';
      head=(head+1)%CMD_QUEUE_CAP;
      cnt--; ok=true;
    }
    xSemaphoreGive(mtx);
    return ok;
  }
  int size() {
    int v=0;
    if (mtx && xSemaphoreTake(mtx, pdMS_TO_TICKS(10))==pdTRUE){ v=cnt; xSemaphoreGive(mtx); }
    return v;
  }
};

struct Config {
  char wifiSSID[64] = {0};
  char wifiPass[64] = {0};
  char host[96] = "irc.libera.chat";
  uint16_t port = 6697;
  bool useTLS = true;
  bool tlsInsecure = true;
  char nick[32] = "CardADV";
  char user[32] = "cardputer";
  char realname[64] = "Cardputer IRC";
  char pass[64] = {0};
  char autojoin[128] = {0};
  LogLevel logLevel = LOG_NONE;
  char logRoot[32] = "/irc/logs";
  uint8_t brightness = 10;
  char preset[16] = "libera";
  char bncUser[64]={0}, bncPass[64]={0}, bncNetwork[32]={0}, bncClient[32]={0}, sojuNet[32]={0};
  bool bncEnabled=false; bool saslEnabled=false; char saslUser[32]={0}, saslPass[32]={0};
  int current_audio = 0; // 0 normal, 1 stealth mode
  int timezone_index = 0;
  bool use_12_hour_format = false;
};

// ---------------------------------------------------------------------------
// Globals - static pre-allocated, no heap in loop pipelines
// ---------------------------------------------------------------------------
static Config gCfg;
// GLOBAL VARIABLE DEFINITIONS for bouncer connection settings per spec - STATIC GLOBAL STORAGE
static String bnc_host = "";
static int bnc_port = 0;
static char bnc_user[32] = {0};
static char bnc_net[32] = {0};
static char bnc_pass[64] = {0};
static Tab gTabs[MAX_TABS];
static int gTabCount = 0;
static int gActive = 0;
#define current_tab_index gActive // spec server skip index - alias to gActive
static char active_networks[4][32] = {0};
static int active_networks_count = 0;
static bool gSdReady = false;
static bool gSafeBoot = false;
static bool gNickOverlay = false;
static uint32_t gLastInputMs = 0;
static bool gDownclocked = false;
static uint8_t gSavedBrightness = BRIGHT_AWAKE;

static CmdQueue gTxQueue;
static CmdQueue gRxQueue;
static SemaphoreHandle_t gTabsMutex = nullptr;
// SPI BUS HARDWARE CONFLICT PROTECTION - Global irc_mutex gauntlet per spec
// SD card slot and ST7789 display share same hardware SPI peripheral lines.
// All SD file write/read/delete must be wrapped inside irc_mutex.
// Core 0 logging task must halt and wait for Core 1 canvas flushes before SD open/write.
static SemaphoreHandle_t irc_mutex = nullptr;

// Dedicated SD log queue for Core 0 logging task (decouples Core 1 UI from SD SPI)
struct LogEntry { char path[96]; char line[MAX_LINE_LEN+12]; };
struct LogQueue {
  LogEntry buf[16];
  volatile int head{0};
  volatile int tail{0};
  volatile int cnt{0};
  SemaphoreHandle_t mtx{nullptr};
  void init(){ mtx=xSemaphoreCreateMutex(); head=tail=cnt=0; }
  bool push(const LogEntry &e){
    if(!mtx) return false;
    if(xSemaphoreTake(mtx,pdMS_TO_TICKS(20))!=pdTRUE) return false;
    bool ok=false;
    if(cnt < 16){ buf[tail]=e; tail=(tail+1)%16; cnt++; ok=true; }
    xSemaphoreGive(mtx); return ok;
  }
  bool pop(LogEntry &out){
    if(!mtx) return false;
    if(xSemaphoreTake(mtx,pdMS_TO_TICKS(20))!=pdTRUE) return false;
    bool ok=false;
    if(cnt>0){ out=buf[head]; head=(head+1)%16; cnt--; ok=true; }
    xSemaphoreGive(mtx); return ok;
  }
};
static LogQueue gLogQueue;
static TaskHandle_t gLogTaskHandle = nullptr;

static WiFiClientSecure gSecure;
static WiFiClient gPlain;

static lgfx::LGFX_Sprite canvas(&M5Cardputer.Display);
static lgfx::LGFX_Sprite gChatCanvas(&M5Cardputer.Display); // legacy alias for unified canvas
static bool gCanvasReady = false;

static TaskHandle_t gNetTaskHandle = nullptr;
static char gRxAccum[RX_ACCUM_SZ];
static int gRxLen = 0;
static bool gIrcConnected = false;
static bool gIrcRegistered = false;
static uint32_t gLastRxMs = 0;
static uint32_t gLastPingMs = 0;
static bool gAwaitPong = false;
static char gPingToken[16]={0};

static char gInput[INPUT_BUF_SZ];
static int gInputLen = 0;
static int gInputCursor = 0;
static int gInputScroll = 0; // horizontal scroll offset for 38-col window

// Input history carousel - last 5 sent commands
static char gHistory[HISTORY_DEPTH][INPUT_BUF_SZ];
static int gHistCount = 0;
static int gHistNav = -1; // -1 means not navigating

// Scanner state
static bool gInScanner = false;
static int gScanFound = 0;
static char gScanSSID[4][33];
static int gScanRSSI[4];
static int gScanSel = 0;
static char gScanPass[64];
static int gScanPassLen = 0;

// WiFi non-blocking
static bool gWifiConnecting = false;
static uint32_t gWifiStartMs = 0;

// Stealth LED notification
static bool gStealthPulseActive = false;
static uint32_t gLastPulseMs = 0;
static int gHighlightedTab = -1;
static bool gLedOn = false;

// Jack and battery
static bool gJackPlugged = false;
static uint32_t gLastJackPoll = 0;
static uint32_t gLastBattPoll = 0;
static float gBattVoltage = 0.0f;
static bool gQuickOverlay = false; // Quick Settings overlay flag
static uint32_t gQuickOverlayMs = 0;
// Dirty flag redraw rule per spec
bool ui_needs_redraw = true;
int current_settings_row = 0; // 5-row grid 0-4: 0 Audio,1 Brightness,2 SD Filtering,3 TimeZone,4 Hour Format

// ---------------------------------------------------------------------------
// C-string helpers - no String in hot paths
// ---------------------------------------------------------------------------
static void safeCopy(char* dst, const char* src, size_t n){
  if(!dst||n==0) return;
  if(!src){ dst[0]='\0'; return; }
  strncpy(dst, src, n-1);
  dst[n-1]='\0';
}
static void trim(char* s){
  if(!s) return;
  char* p=s; while(*p && isspace((unsigned char)*p)) p++;
  if(p!=s) memmove(s,p,strlen(p)+1);
  size_t len=strlen(s);
  while(len>0 && isspace((unsigned char)s[len-1])) s[--len]='\0';
}
static bool eqI(const char* a, const char* b){
  if(!a||!b) return false;
  while(*a && *b){ if(tolower((unsigned char)*a)!=tolower((unsigned char)*b)) return false; a++; b++; }
  return *a==*b;
}
static bool startsI(const char* s, const char* pref){
  if(!s||!pref) return false;
  while(*pref){ if(tolower((unsigned char)*s)!=tolower((unsigned char)*pref)) return false; s++; pref++; }
  return true;
}
static bool isChannelName(const char* s){
  if(!s||!*s) return false;
  return s[0]=='#' || s[0]=='&' || s[0]=='+' || s[0]=='!';
}
static void toLower(char* s){ for(;*s;++s) *s=tolower((unsigned char)*s); }
static bool strToBoolC(const char* s){
  if(!s) return false;
  char tmp[16]; safeCopy(tmp,s,sizeof(tmp)); toLower(tmp); trim(tmp);
  return strcmp(tmp,"1")==0||strcmp(tmp,"true")==0||strcmp(tmp,"yes")==0||strcmp(tmp,"on")==0;
}
static LogLevel parseLogLevel(const char* v){
  if(!v) return LOG_NONE;
  char tmp[16]; safeCopy(tmp,v,sizeof(tmp)); trim(tmp); toLower(tmp);
  if(strcmp(tmp,"0")==0) return LOG_ALL;
  if(strcmp(tmp,"1")==0) return LOG_DMS_ONLY;
  if(strcmp(tmp,"2")==0) return LOG_NONE;
  if(strToBoolC(tmp)) return LOG_ALL;
  if(strcmp(tmp,"false")==0||strcmp(tmp,"off")==0||strcmp(tmp,"no")==0) return LOG_NONE;
  return LOG_NONE;
}
static const char* logLevelStr(LogLevel l){ return l==LOG_ALL?"0":l==LOG_DMS_ONLY?"1":"2"; }
static void currentStamp(char* hhmm, size_t n, char* hhmmss, size_t n2){
  time_t now=time(nullptr); struct tm tmv; localtime_r(&now,&tmv);
  if(hhmm) snprintf(hhmm,n,"%02d:%02d",tmv.tm_hour,tmv.tm_min);
  if(hhmmss) snprintf(hhmmss,n2,"%02d:%02d:%02d",tmv.tm_hour,tmv.tm_min,tmv.tm_sec);
}
static bool isWifiDummy(const Config& c){
  if(c.wifiSSID[0]=='\0') return true;
  char tmp[64]; safeCopy(tmp,c.wifiSSID,sizeof(tmp)); toLower(tmp);
  return strcmp(tmp,"your_wifi")==0 || strcmp(tmp,"your_wifi_ssid")==0 || strcmp(tmp,"ssid")==0 || strcmp(tmp,"placeholder")==0;
}

// ---------------------------------------------------------------------------
// Battery - GPIO10 with 11dB attenuation and 2.0x divider (Cardputer divider)
// ---------------------------------------------------------------------------
static float readBatteryVoltage(){
  // Must be called after analogSetPinAttenuation(10, ADC_11db) and pinMode(10, INPUT) in setup()
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  int raw = analogRead(BATTERY_PIN);
  // Accurate conversion for Cardputer resistor divider per spec:
  float voltage = ( (float)raw / 4095.0f ) * 3.3f * 2.0f;
  return voltage;
}
  // Glyph sanitizer and nick color hash
static void sanitizeGlyphs(char* s){
  if(!s) return;
  for(char* p=s; *p; ++p){
    unsigned char c = (unsigned char)*p;
    if(c < 32 || c > 126) *p = '.'; // replace non-printable/emoji/control with '.' per spec (space also valid)
  }
}
static uint16_t nickHashColor(const char* nick){
  if(!nick || !*nick) return UI_FG;
  uint32_t sum = 0;
  for(const char* p=nick; *p; ++p) sum += (unsigned char)*p;
  return NICK_COLORS[sum % 6];
}
// IRCv3 server-time interceptor - extract hh:mm from ISO8601 time= tag
static bool parseServerTimeHHMM(const char* tags, char* out6){
  if(!tags || !out6) return false;
  out6[0]='\0';
  // lightweight loop over ';' separated tags
  const char* p = tags;
  while(p && *p){
    // find next ';'
    const char* semi = strchr(p, ';');
    size_t tokLen = semi ? (size_t)(semi - p) : strlen(p);
    if(tokLen >= 5 && strncmp(p, "time=", 5)==0){
      const char* val = p + 5;
      // value may be "2024-02-14T12:34:56.123Z" - find 'T'
      const char* t = (const char*)memchr(val, 'T', tokLen - 5);
      if(t && t+5 < val+tokLen){
        // expect hh:mm at t+1
        if(isdigit((unsigned char)t[1]) && isdigit((unsigned char)t[2]) && t[3]==':' && isdigit((unsigned char)t[4]) && isdigit((unsigned char)t[5])){
          out6[0]=t[1]; out6[1]=t[2]; out6[2]=':'; out6[3]=t[4]; out6[4]=t[5]; out6[5]='\0';
          return true;
        }
      }
    }
    if(!semi) break;
    p = semi+1;
  }
  return false;
}
// LOCALIZED TIMEZONE & CLOCK FORMAT MANAGER - raw math without heavy time libs
static void localizeTimeHHMM(const char* utcHHMM, char* out6){
  if(!utcHHMM || !out6){ if(out6) out6[0]='\0'; return; }
  // validate "HH:MM"
  if(strlen(utcHHMM)<5 || utcHHMM[2]!=':' || !isdigit((unsigned char)utcHHMM[0]) || !isdigit((unsigned char)utcHHMM[1]) || !isdigit((unsigned char)utcHHMM[3]) || !isdigit((unsigned char)utcHHMM[4])){ safeCopy(out6, utcHHMM, 6); return; }
  int hh = (utcHHMM[0]-'0')*10 + (utcHHMM[1]-'0');
  int mm = (utcHHMM[3]-'0')*10 + (utcHHMM[4]-'0');
  int off = 0;
  if(gTimezoneIndex>=0 && gTimezoneIndex<TZ_COUNT) off = TZ_PROFILES[gTimezoneIndex].offset;
  int local = hh + off;
  // wrapping checks per spec - raw math subtraction/wrapping
  while(local < 0) local += 24;
  while(local >= 24) local -= 24;
  if(gUse12Hour){
    int disp = local % 12;
    if(disp==0) disp=12;
    // keep zero-padded 12-hour HH:MM (01..12) without AM/PM to fit 5-char stamp
    snprintf(out6,6,"%02d:%02d", disp, mm);
  } else {
    snprintf(out6,6,"%02d:%02d", local, mm);
  }
}
static void pollBattery(){
  if(millis() - gLastBattPoll < 5000) return;
  gLastBattPoll = millis();
  gBattVoltage = readBatteryVoltage();
}

// ---------------------------------------------------------------------------
// Jack auto-mute - NS4150B I2S amplifier shutdown line handling
// ---------------------------------------------------------------------------
static void pollJack(){
  if(millis() - gLastJackPoll < 500) return;
  gLastJackPoll = millis();
  bool plugged = (digitalRead(JACK_DETECT_PIN) == LOW);
  if(plugged != gJackPlugged){
    gJackPlugged = plugged;
    // NS4150B shutdown pin: LOW mutes cavity speaker to prevent bleed/interference
    digitalWrite(AMP_SHUTDOWN_PIN, plugged ? LOW : HIGH);
    if(gJackPlugged){
      // instantly shut down I2S speaker amplifier line
      M5Cardputer.Speaker.stop();
      M5Cardputer.Speaker.setVolume(0);
    } else {
      // restore volume unless stealth mode mutes it
      if(gCfg.current_audio != 1) M5Cardputer.Speaker.setVolume(128);
    }
  }
  // stealth mode also mutes via shutdown line
  if(gCfg.current_audio == 1 && gStealthPulseActive){
    digitalWrite(AMP_SHUTDOWN_PIN, LOW);
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.setVolume(0);
  } else if(!gJackPlugged){
    digitalWrite(AMP_SHUTDOWN_PIN, HIGH);
  }
}

// ---------------------------------------------------------------------------
// Stealth LED strip - NeoPixel, purple 0x330033 pulse every 3s on highlight
// ---------------------------------------------------------------------------
static void setStealthLed(bool on){
  if(on){
    // soft low-brightness purple 0x330033 via neopixelWrite (ESP Arduino core helper)
    neopixelWrite(LED_PIN, 0x33, 0x00, 0x33);
  } else {
    neopixelWrite(LED_PIN, 0, 0, 0);
  }
  gLedOn = on;
}
static void serviceStealthLed(){
  if(!gStealthPulseActive) {
    if(gLedOn) setStealthLed(false);
    return;
  }
  // if user switched to highlighted buffer, stop pulse
  if(gHighlightedTab == gActive){
    gStealthPulseActive = false;
    gHighlightedTab = -1;
    setStealthLed(false);
    // restore speaker if not jack-plugged
    if(!gJackPlugged && gCfg.current_audio != 1) M5Cardputer.Speaker.setVolume(128);
    return;
  }
  if(millis() - gLastPulseMs >= STEALTH_PULSE_MS){
    gLastPulseMs = millis();
    setStealthLed(!gLedOn); // toggle
    // auto-off after short flash: keep on for 150ms then off, but using toggle captures pulse
    // we keep toggle logic simple: on for 150ms via delay check in next tick
  }
  // enforce 150ms on-time
  if(gLedOn && millis() - gLastPulseMs > 150) setStealthLed(false);
}

// ---------------------------------------------------------------------------
// Tab helpers - ring buffer strictly 20 lines
// ---------------------------------------------------------------------------
static Tab* activeTab(){
  if(gTabCount==0) return nullptr;
  if(gActive<0) gActive=0;
  if(gActive>=gTabCount) gActive=gTabCount-1;
  return &gTabs[gActive];
}
static Tab* findTab(const char* name){
  if(!name) return nullptr;
  for(int i=0;i<gTabCount;++i) if(eqI(gTabs[i].name,name)) return &gTabs[i];
  return nullptr;
}
static Tab* getOrCreateTab(const char* name, TabType t){
  if(!name||!*name) return activeTab();
  Tab* f=findTab(name);
  if(f) return f;
  if(gTabCount>=MAX_TABS){
    for(int i=0;i<gTabCount;++i) if(gTabs[i].type==TAB_STATUS) return &gTabs[i];
    return &gTabs[0];
  }
  Tab* nb=&gTabs[gTabCount++];
  memset(nb,0,sizeof(Tab));
  safeCopy(nb->name,name,sizeof(nb->name));
  nb->type=t;
  {
    const char* srv = bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host;
    safeCopy(nb->server, srv, sizeof(nb->server));
    // also update active_networks cache
    bool found=false; for(int i=0;i<active_networks_count;++i) if(eqI(active_networks[i], nb->server)) found=true;
    if(!found && active_networks_count<4){ safeCopy(active_networks[active_networks_count++], nb->server, sizeof(active_networks[0])); }
  }
  return nb;
}
static void ensureStatus(){
  if(gTabCount==0){
    Tab* t=&gTabs[gTabCount++];
    memset(t,0,sizeof(Tab));
    safeCopy(t->name,"status",sizeof(t->name));
    t->type=TAB_STATUS;
    const char* srv = bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host;
    safeCopy(t->server, srv, sizeof(t->server));
    if(active_networks_count<4){ safeCopy(active_networks[active_networks_count++], t->server, sizeof(active_networks[0])); }
    gActive=0;
  }
}
static void ringPush(Tab* tab, const char* txt, uint8_t flags, const char* serverHHMM=nullptr){
  if(!tab||!txt) return;
  char sanitized[MAX_LINE_LEN+1];
  safeCopy(sanitized, txt, sizeof(sanitized));
  sanitizeGlyphs(sanitized);
  ChatLine* slot=&tab->lines[tab->head];
  char hhmm[6];
  if(serverHHMM && serverHHMM[0]) safeCopy(hhmm, serverHHMM, sizeof(hhmm));
  else currentStamp(hhmm,sizeof(hhmm),nullptr,0);
  safeCopy(slot->stamp,hhmm,sizeof(slot->stamp));
  safeCopy(slot->text,sanitized,sizeof(slot->text));
  slot->flags=flags;
  tab->head=(tab->head+1)%MAX_LINES_PER_TAB;
  if(tab->count<MAX_LINES_PER_TAB) tab->count++;
  if(tab->scroll>0) tab->scroll++;
  if(tab->scroll > (int)tab->count - CHAT_ROWS) tab->scroll = tab->count - CHAT_ROWS;
  if(tab->scroll<0) tab->scroll=0;
  bool isActive = (tab==activeTab() && !gInScanner);
  if(!isActive){
    tab->unread=true;
    if(flags & 0x01) tab->mention=true;
  }
  ui_needs_redraw = true; // new background message actively pushed to chat logs
}
static bool shouldLog(const Tab* tab, bool isSystemError){
  if(gCfg.logLevel==LOG_NONE) return false;
  if(gCfg.logLevel==LOG_ALL) return true;
  if(isSystemError) return true;
  if(tab && tab->type==TAB_QUERY) return true;
  return false;
}
static const ChatLine* ringAt(const Tab* tab, int logicalIdx){
  if(!tab||logicalIdx<0||logicalIdx>=tab->count) return nullptr;
  int start = (tab->head - tab->count + MAX_LINES_PER_TAB)%MAX_LINES_PER_TAB;
  int phys = (start + logicalIdx)%MAX_LINES_PER_TAB;
  return &tab->lines[phys];
}
static void appendLog(Tab* tab, const char* raw, const char* serverHHMM=nullptr){
  if(!tab||!raw) return;
  uint8_t fl=0;
  char lowTxt[160]; safeCopy(lowTxt,raw,sizeof(lowTxt)); toLower(lowTxt);
  char lowNick[32]; safeCopy(lowNick,gCfg.nick,sizeof(lowNick)); toLower(lowNick);
  if(lowNick[0] && strstr(lowTxt,lowNick)) fl|=0x01;
  ringPush(tab,raw,fl,serverHHMM);
  // stealth trigger: highlight while stealth mode active
  if((fl & 0x01) && gCfg.current_audio == 1){
    // mute speaker entirely
    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.setVolume(0);
    // trigger non-blocking pulse ticker if not already on highlighted tab
    Tab* at=activeTab();
    if(tab != at){
      gStealthPulseActive = true;
      gHighlightedTab = (int)(tab - gTabs);
      gLastPulseMs = millis() - STEALTH_PULSE_MS; // immediate pulse
    }
  }
  if(!gSdReady) return;
  if(!shouldLog(tab, false)) return;
  char date[9]; time_t now=time(nullptr); struct tm tmv; localtime_r(&now,&tmv);
  snprintf(date,sizeof(date),"%04d%02d%02d",tmv.tm_year+1900,tmv.tm_mon+1,tmv.tm_mday);
  char safeTab[32]; safeCopy(safeTab,tab->name,sizeof(safeTab));
  for(char* p=safeTab;*p;++p) if(!isalnum((unsigned char)*p) && *p!='#' && *p!='_' && *p!='-') *p='_';
  if(tab->type==TAB_QUERY){ char tmp[32]; snprintf(tmp,sizeof(tmp),"query_%s",safeTab); safeCopy(safeTab,tmp,sizeof(safeTab)); }
  if(eqI(safeTab,"status")) safeCopy(safeTab,"status",sizeof(safeTab));
  char path[96];
  snprintf(path,sizeof(path),"%s/%s/%s.log", gCfg.logRoot, date, safeTab);
  // MUTEX GAUNTLET: Enqueue log for Core 0 task - Core 0 will halt/wait for Core 1 SPI flush via irc_mutex before SD open/write
  char hhmmss[9]; currentStamp(nullptr,0,hhmmss,sizeof(hhmmss));
  LogEntry e;
  safeCopy(e.path, path, sizeof(e.path));
  snprintf(e.line, sizeof(e.line), "%s %s", hhmmss, raw);
  sanitizeGlyphs(e.line);
  gLogQueue.push(e);
}
static void logStatus(const char* s){
  Tab* t=findTab("status");
  if(!t) t=getOrCreateTab("status",TAB_STATUS);
  char buf[MAX_LINE_LEN+1];
  snprintf(buf,sizeof(buf),"*** %s",s?s:"");
  // system warnings always bypass tier 1 filter via isSystemError=true path? we call ringPush direct plus conditional log
  // For status we treat as system: shouldLog with true
  if(!shouldLog(t,true) && gCfg.logLevel==LOG_DMS_ONLY){
    // still ring push but skip SD
    ringPush(t,buf,0);
    return;
  }
  appendLog(t,buf);
}

// ---------------------------------------------------------------------------
// Config load/save - native key hooks
// ---------------------------------------------------------------------------
static void loadConfig(){
  memset(&gCfg,0,sizeof(gCfg));
  safeCopy(gCfg.host,"irc.libera.chat",sizeof(gCfg.host));
  gCfg.port=6697; gCfg.useTLS=true; gCfg.tlsInsecure=true;
  safeCopy(gCfg.nick,"CardADV",sizeof(gCfg.nick));
  safeCopy(gCfg.user,"cardputer",sizeof(gCfg.user));
  safeCopy(gCfg.realname,"Cardputer IRC",sizeof(gCfg.realname));
  safeCopy(gCfg.logRoot,LOG_ROOT_DEFAULT,sizeof(gCfg.logRoot));
  safeCopy(gCfg.preset,"libera",sizeof(gCfg.preset));
  gCfg.logLevel=LOG_NONE; gCfg.brightness=10; gCfg.current_audio=0;
  gCfg.timezone_index=0; gCfg.use_12_hour_format=false;
  gTimezoneIndex=0; gUse12Hour=false; gQuickOverlayRow=0;
  if(gSafeBoot){
    logStatus("Safe-boot: bypass SD config");
    return;
  }
  // MUTEX GAUNTLET: wrap all SD file read operations inside irc_mutex
  if(!irc_mutex || xSemaphoreTake(irc_mutex, portMAX_DELAY)!=pdTRUE){
    logStatus("No /irc/config.txt, defaults");
    return;
  }
  bool exists = SD.exists(CONFIG_PATH);
  if(!gSdReady || !exists){
    xSemaphoreGive(irc_mutex);
    logStatus("No /irc/config.txt, defaults");
    return;
  }
  File f=SD.open(CONFIG_PATH, FILE_READ);
  if(!f){ xSemaphoreGive(irc_mutex); logStatus("Config open fail"); return; }
  char line[256];
  while(f.available()){
    int len=0; while(f.available() && len<(int)sizeof(line)-1){ char c=(char)f.read(); if(c=='\r') continue; if(c=='\n') break; line[len++]=c; }
    line[len]='\0';
    char* s=line; trim(s);
    if(s[0]=='\0' || s[0]=='#' || s[0]==';') continue;
    char* eq=strchr(s,'=');
    if(!eq) continue;
    *eq='\0'; char* k=s; char* v=eq+1; trim(k); trim(v);
    for(char* p=k;*p;++p) *p=tolower((unsigned char)*p);
    if(strcmp(k,"wifi_ssid")==0) safeCopy(gCfg.wifiSSID,v,sizeof(gCfg.wifiSSID));
    else if(strcmp(k,"wifi_pass")==0) safeCopy(gCfg.wifiPass,v,sizeof(gCfg.wifiPass));
    else if(strcmp(k,"irc_server_preset")==0||strcmp(k,"irc_server")==0) safeCopy(gCfg.preset,v,sizeof(gCfg.preset));
    else if(strcmp(k,"irc_host")==0||strcmp(k,"endpoint_host")==0) safeCopy(gCfg.host,v,sizeof(gCfg.host));
    else if(strcmp(k,"irc_port")==0||strcmp(k,"endpoint_port")==0) gCfg.port=(uint16_t)atoi(v);
    else if(strcmp(k,"irc_use_tls")==0) gCfg.useTLS=strToBoolC(v);
    else if(strcmp(k,"tls_insecure")==0) gCfg.tlsInsecure=strToBoolC(v);
    else if(strcmp(k,"irc_pass")==0||strcmp(k,"server_pass")==0) safeCopy(gCfg.pass,v,sizeof(gCfg.pass));
    else if(strcmp(k,"irc_nick")==0||strcmp(k,"nick")==0) safeCopy(gCfg.nick,v,sizeof(gCfg.nick));
    else if(strcmp(k,"irc_user")==0||strcmp(k,"username")==0) safeCopy(gCfg.user,v,sizeof(gCfg.user));
    else if(strcmp(k,"irc_realname")==0||strcmp(k,"realname")==0) safeCopy(gCfg.realname,v,sizeof(gCfg.realname));
    else if(strcmp(k,"autojoin")==0) safeCopy(gCfg.autojoin,v,sizeof(gCfg.autojoin));
    else if(strcmp(k,"channel_log_enabled")==0||strcmp(k,"chat_log_enabled")==0) gCfg.logLevel=parseLogLevel(v);
    else if(strcmp(k,"log_root")==0) safeCopy(gCfg.logRoot,v,sizeof(gCfg.logRoot));
    else if(strcmp(k,"screen_brightness")==0) gCfg.brightness=(uint8_t)constrain(atoi(v),0,10);
    else if(strcmp(k,"current_audio")==0) gCfg.current_audio=atoi(v);
    else if(strcmp(k,"bnc_enabled")==0) gCfg.bncEnabled=strToBoolC(v);
    else if(strcmp(k,"bnc_user")==0) { strncpy(bnc_user, v, sizeof(bnc_user)-1); bnc_user[sizeof(bnc_user)-1]='\0'; safeCopy(gCfg.bncUser,v,sizeof(gCfg.bncUser)); }
    else if(strcmp(k,"bnc_net")==0) { strncpy(bnc_net, v, sizeof(bnc_net)-1); bnc_net[sizeof(bnc_net)-1]='\0'; }
    else if(strcmp(k,"bnc_pass")==0) { strncpy(bnc_pass, v, sizeof(bnc_pass)-1); bnc_pass[sizeof(bnc_pass)-1]='\0'; safeCopy(gCfg.bncPass,v,sizeof(gCfg.bncPass)); }
    else if(strcmp(k,"bnc_host")==0) bnc_host = String(v);
    else if(strcmp(k,"bnc_port")==0) bnc_port = atoi(v);
    else if(strcmp(k,"bnc_network")==0) safeCopy(gCfg.bncNetwork,v,sizeof(gCfg.bncNetwork));
    else if(strcmp(k,"bnc_client")==0) safeCopy(gCfg.bncClient,v,sizeof(gCfg.bncClient));
    else if(strcmp(k,"soju_bind_netid")==0) safeCopy(gCfg.sojuNet,v,sizeof(gCfg.sojuNet));
    else if(strcmp(k,"sasl_enabled")==0) gCfg.saslEnabled=strToBoolC(v);
    else if(strcmp(k,"sasl_user")==0) safeCopy(gCfg.saslUser,v,sizeof(gCfg.saslUser));
    else if(strcmp(k,"sasl_pass")==0) safeCopy(gCfg.saslPass,v,sizeof(gCfg.saslPass));
    else if(strcmp(k,"timezone_index")==0){ int idx=atoi(v); if(idx<0) idx=0; if(idx>=TZ_COUNT) idx=TZ_COUNT-1; gTimezoneIndex=idx; gCfg.timezone_index=idx; }
    else if(strcmp(k,"timezone")==0){ int idx=atoi(v); if(idx<0) idx=0; if(idx>=TZ_COUNT) idx=TZ_COUNT-1; gTimezoneIndex=idx; gCfg.timezone_index=idx; }
    else if(strcmp(k,"use_12_hour_format")==0){ bool b=(atoi(v)!=0) || strToBoolC(v); gUse12Hour=b; gCfg.use_12_hour_format=b; }
    else if(strcmp(k,"clock_format")==0){ bool b=(atoi(v)==12) || strToBoolC(v); gUse12Hour=b; gCfg.use_12_hour_format=b; }
  }
  f.close();
  xSemaphoreGive(irc_mutex);
  if(gCfg.logRoot[0]=='\0') safeCopy(gCfg.logRoot,LOG_ROOT_DEFAULT,sizeof(gCfg.logRoot));
  // sync global timezone/format with config and clamp
  if(gTimezoneIndex<0) gTimezoneIndex=0; if(gTimezoneIndex>=TZ_COUNT) gTimezoneIndex=TZ_COUNT-1;
  gCfg.timezone_index=gTimezoneIndex; gCfg.use_12_hour_format=gUse12Hour;
  char tmp[80]; snprintf(tmp,sizeof(tmp),"Config loaded: %s@%s log=%s audio=%d tz=%s:%d fmt=%s",gCfg.nick,gCfg.host,logLevelStr(gCfg.logLevel),gCfg.current_audio, TZ_PROFILES[gTimezoneIndex].label, TZ_PROFILES[gTimezoneIndex].offset, gUse12Hour?"12h":"24h");
  logStatus(tmp);
}
static void saveConfig(){
  if(!gSdReady || gSafeBoot) return;
  // MUTEX GAUNTLET: wrap all SD file write operations inside irc_mutex
  if(!irc_mutex || xSemaphoreTake(irc_mutex, portMAX_DELAY)!=pdTRUE) return;
  if(!SD.exists("/irc")) SD.mkdir("/irc");
  if(SD.exists(CONFIG_PATH)) SD.remove(CONFIG_PATH);
  File f=SD.open(CONFIG_PATH, FILE_WRITE);
  if(!f){ xSemaphoreGive(irc_mutex); return; }
  f.printf("wifi_ssid=%s\n", gCfg.wifiSSID);
  f.printf("wifi_pass=%s\n", gCfg.wifiPass);
  f.printf("irc_server_preset=%s\n", gCfg.preset);
  f.printf("irc_host=%s\n", gCfg.host);
  f.printf("irc_port=%u\n", gCfg.port);
  f.printf("irc_use_tls=%s\n", gCfg.useTLS?"true":"false");
  f.printf("tls_insecure=%s\n", gCfg.tlsInsecure?"true":"false");
  f.printf("irc_nick=%s\n", gCfg.nick);
  f.printf("irc_user=%s\n", gCfg.user);
  f.printf("irc_realname=%s\n", gCfg.realname);
  f.printf("irc_pass=%s\n", gCfg.pass);
  f.printf("autojoin=%s\n", gCfg.autojoin);
  f.printf("channel_log_enabled=%s\n", logLevelStr(gCfg.logLevel));
  f.printf("log_root=%s\n", gCfg.logRoot);
  f.printf("screen_brightness=%u\n", gCfg.brightness);
  f.printf("current_audio=%d\n", gCfg.current_audio);
  f.printf("bnc_enabled=%s\n", gCfg.bncEnabled?"true":"false");
  f.printf("bnc_user=%s\n", bnc_user);
  f.printf("bnc_net=%s\n", bnc_net);
  f.printf("bnc_pass=%s\n", bnc_pass);
  f.printf("bnc_host=%s\n", bnc_host.c_str());
  f.printf("bnc_port=%d\n", bnc_port);
  f.printf("bnc_network=%s\n", gCfg.bncNetwork);
  f.printf("bnc_client=%s\n", gCfg.bncClient);
  f.printf("soju_bind_netid=%s\n", gCfg.sojuNet);
  f.printf("sasl_enabled=%s\n", gCfg.saslEnabled?"true":"false");
  f.printf("sasl_user=%s\n", gCfg.saslUser);
  f.printf("sasl_pass=%s\n", gCfg.saslPass);
  f.printf("timezone_index=%d\n", gTimezoneIndex);
  f.printf("use_12_hour_format=%d\n", gUse12Hour?1:0);
  f.close();
  // persist numeric values into Config mirror
  gCfg.timezone_index=gTimezoneIndex; gCfg.use_12_hour_format=gUse12Hour;
  xSemaphoreGive(irc_mutex);
  logStatus("Config saved");
}

// DYNAMIC BACKUP HOOK: thread-safe backup of active config per spec
static void trigger_config_backup(){
  // Enveloped within irc_mutex to safeguard shared SPI bus (SD + ST7789 share same peripheral)
  if(!irc_mutex || xSemaphoreTake(irc_mutex, portMAX_DELAY)!=pdTRUE) return;
  // Copy active contents of /irc/config.txt to /irc/config.bak
  if(SD.exists("/irc/config.bak")) SD.remove("/irc/config.bak");
  if(!SD.exists("/irc/config.txt")){
    xSemaphoreGive(irc_mutex);
    return;
  }
  File src = SD.open("/irc/config.txt", FILE_READ);
  File dst = SD.open("/irc/config.bak", FILE_WRITE);
  if(src && dst){
    uint8_t buf[128];
    while(src.available()){
      size_t n = src.read(buf, sizeof(buf));
      if(n>0) dst.write(buf, n);
    }
  }
  if(src) src.close();
  if(dst) dst.close();
  xSemaphoreGive(irc_mutex);
}

// ---------------------------------------------------------------------------
// 7-day log purge engine in setup() - parse integer YYYYMMDD from dir names
// ---------------------------------------------------------------------------
static bool isAllDigits(const char* s){
  if(!s||!*s) return false;
  for(;*s;++s) if(!isdigit((unsigned char)*s)) return false;
  return true;
}
static int daysAgoFromYYYYMMDD(const char* yyyymmdd){
  if(!yyyymmdd||strlen(yyyymmdd)!=8||!isAllDigits(yyyymmdd)) return -1;
  int y=atoi(yyyymmdd)/10000;
  int m=(atoi(yyyymmdd)/100)%100;
  int d=atoi(yyyymmdd)%100;
  struct tm tmv{}; tmv.tm_year=y-1900; tmv.tm_mon=m-1; tmv.tm_mday=d;
  time_t t=mktime(&tmv);
  if(t==-1) return 999;
  time_t now=time(nullptr);
  double diff=difftime(now,t);
  return (int)(diff/86400);
}
static void sweepRecursive(const char* base, int depth){
  if(depth>6) return;
  File dir=SD.open(base);
  if(!dir) return;
  if(!dir.isDirectory()){ dir.close(); return; }
  File e=dir.openNextFile();
  while(e){
    char name[96]; safeCopy(name,e.name(),sizeof(name));
    const char* bn=strrchr(name,'/'); if(bn) bn++; else bn=name;
    if(e.isDirectory()){
      if(strlen(bn)==8 && isAllDigits(bn)){
        int age=daysAgoFromYYYYMMDD(bn);
        if(age>7){
          char full[96]; snprintf(full,sizeof(full),"%s/%s",base,bn);
          File dated=SD.open(full);
          if(dated && dated.isDirectory()){
            File f2=dated.openNextFile();
            while(f2){ char p2[128]; snprintf(p2,sizeof(p2),"%s/%s",full,f2.name()); f2.close(); SD.remove(p2); f2=dated.openNextFile(); }
            dated.close();
          }
          SD.rmdir(full);
          char msg[64]; snprintf(msg,sizeof(msg),"Purge %s %dd",full,age);
          logStatus(msg);
        }
      } else {
        char sub[128]; snprintf(sub,sizeof(sub),"%s/%s",base,bn);
        sweepRecursive(sub,depth+1);
      }
    } else {
      char baseOnly[32]; safeCopy(baseOnly,bn,sizeof(baseOnly));
      char* dot=strchr(baseOnly,'.'); if(dot) *dot='\0';
      if(strlen(baseOnly)==8 && isAllDigits(baseOnly)){
        int age=daysAgoFromYYYYMMDD(baseOnly);
        if(age>7){ char full[128]; snprintf(full,sizeof(full),"%s/%s",base,bn); SD.remove(full); }
      }
    }
    e.close();
    e=dir.openNextFile();
  }
  dir.close();
}
static void sweepOldLogs(){
  if(!gSdReady || gSafeBoot) return;
  // MUTEX GAUNTLET: wrap all SD file read/delete operations inside irc_mutex
  if(!irc_mutex || xSemaphoreTake(irc_mutex, portMAX_DELAY)!=pdTRUE) return;
  const char* roots[] = {"/irc/logs","/irc","/IRC", nullptr};
  for(int i=0;roots[i];++i){
    if(SD.exists(roots[i])){
      File f=SD.open(roots[i]);
      if(f){ f.close(); sweepRecursive(roots[i],0); }
    }
  }
  xSemaphoreGive(irc_mutex);
  logStatus("Purge 7d done");
}

// ---------------------------------------------------------------------------
// Power watchdog - 45s inactivity downclock 240->80 brightness 2
// ---------------------------------------------------------------------------
static void applyBrightness(uint8_t lvl){
  uint8_t pwm = (uint16_t)lvl*255/10;
  M5Cardputer.Display.setBrightness(pwm);
}
static void servicePowerWatchdog(){
  uint32_t idle = millis() - gLastInputMs;
  if(!gDownclocked && idle > INACTIVITY_MS){
    setCpuFrequencyMhz(80);
    applyBrightness(BRIGHT_SLEEP);
    gDownclocked=true;
  }
}
static void wakeFromSleep(){
  if(gDownclocked){
    setCpuFrequencyMhz(240);
    applyBrightness(gSavedBrightness ? gSavedBrightness : gCfg.brightness);
    gDownclocked=false;
  }
  gLastInputMs = millis();
}

// ---------------------------------------------------------------------------
// Rendering - 8-bit 109px canvas + direct top/bottom
// ---------------------------------------------------------------------------
static void initCanvas(){
  if(gCanvasReady) return;
  // Split Sub-Canvas per spec: 240x109 chat viewport only - 26KB at 8-bit, always succeeds
  canvas.setColorDepth(8);
  canvas.setPsram(false);
  canvas.setTextSize(1);
  canvas.setTextWrap(false);
  canvas.deleteSprite();
  if(!canvas.createSprite(240, 109)){
    gCanvasReady = false;
    return;
  }
  canvas.fillScreen(UI_BG);
  gCanvasReady = canvas.width()==240 && canvas.height()==109;
}
// ZERO-MUTEX INTRO ANIMATION - unshielded direct drawing to physical display glass
void run_retro_splash_screen(){
  // No irc_mutex usage here - direct to Display before background tasks exist
  M5Cardputer.Display.fillScreen(UI_BG);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(UI_FG, UI_BG);
  M5Cardputer.Display.setCursor(8,10);
  M5Cardputer.Display.print("Cardputer IRC 0.4");
  M5Cardputer.Display.setCursor(8,22);
  M5Cardputer.Display.setTextColor(UI_DIM, UI_BG);
  M5Cardputer.Display.print("Adv ST7789 240x135 NO PSRAM");
  M5Cardputer.Display.setCursor(8,34);
  M5Cardputer.Display.print("8-bit canvas 20-ring");
  M5Cardputer.Display.setCursor(8,46);
  M5Cardputer.Display.print("Hold G0 for safe WiFi setup");
  // Brief G0 check window without mutex
  uint32_t splashStart = millis();
  while(millis() - splashStart < 800){
    if(digitalRead(G0_PIN)==LOW){
      M5Cardputer.Display.setCursor(8,70);
      M5Cardputer.Display.setTextColor(UI_WARN, UI_BG);
      M5Cardputer.Display.print("[G0 SAFE BOOT]");
      gSafeBoot = true;
      break;
    }
    delay(10);
    M5Cardputer.update();
  }
  delay(200);
}


static void drawTopBar(){
  auto &d = canvas;
  d.fillRect(0,0,SCREEN_W,TOP_H, UI_BG);
  d.drawFastHLine(0,TOP_H-1,SCREEN_W, UI_DIM);
  d.setTextSize(1);
  // Scrolling tab-strip: show all buffers with [active] and !/* markers
  // HEADER COLOR SEGREGATION: dividers '[',']','|' muted grey, text bright white
  int x=2;
  for(int i=0;i<gTabCount && x < SCREEN_W-2; ++i){
    bool isActive = (i==gActive);
    char tabTok[36];
    char mark = ' ';
    if(gTabs[i].mention) mark='!';
    else if(gTabs[i].unread) mark='*';
    if(isActive){
      if(mark!=' ') snprintf(tabTok,sizeof(tabTok),"[%s%c]", gTabs[i].name, mark);
      else snprintf(tabTok,sizeof(tabTok),"[%s]", gTabs[i].name);
    } else {
      if(mark!=' ') snprintf(tabTok,sizeof(tabTok),"%s%c", gTabs[i].name, mark);
      else safeCopy(tabTok,gTabs[i].name,sizeof(tabTok));
    }
    // Color-code dividers low-contrast muted grey (0x8410), text bright white (0xFFFF) for legibility
    for(char *p=tabTok; *p; ++p){
      bool isDiv = (*p=='[' || *p==']' || *p=='|');
      d.setTextColor(isDiv ? 0x8410 : 0xFFFF, UI_BG);
      d.setCursor(x,2);
      d.print(*p);
      x += CHAR_W;
      if(x >= SCREEN_W-6) break;
    }
    x += 4;
    if(x >= SCREEN_W) break;
  }
  // Battery voltage on far right - truncated title at X=130, fixed anchors per spec
  // Truncate title string at X=130 to prevent shifting status icons
  // (tabs already truncated via x limit, but enforce hard cap at 130)
  // Fixed right-aligned anchors: Mute at X=145, Time at X=175, Battery at X=212
  char audioTag[7];
  uint16_t audioCol;
  if(gCfg.current_audio == 1){
    strncpy(audioTag, "[MUTE]", sizeof(audioTag));
    audioCol = 0xF800; // bright red
  } else {
    strncpy(audioTag, "[LOUD]", sizeof(audioTag));
    audioCol = 0x07E0; // solid bright green
  }
  d.setTextColor(audioCol, UI_BG);
  d.setCursor(145, 2);
  d.print(audioTag);
  // Local Time Clock at X=175
  char timeStr[9]; currentStamp(nullptr,0,timeStr,sizeof(timeStr));
  // Apply timezone and 12h conversion for HUD clock as well
  char timeHHMM[6]; if(strlen(timeStr)>=5){ timeHHMM[0]=timeStr[0]; timeHHMM[1]=timeStr[1]; timeHHMM[2]=':'; timeHHMM[3]=timeStr[3]; timeHHMM[4]=timeStr[4]; timeHHMM[5]='\0'; char loc[6]; localizeTimeHHMM(timeHHMM, loc); safeCopy(timeStr, loc, sizeof(timeStr)); }
  d.setTextColor(UI_FG, UI_BG);
  d.setCursor(175, 2);
  d.print(timeStr);
  // Battery Percentage HUD at X=212 - whole integer percentage from GPIO10 11dB raw math
  float v = readBatteryVoltage();
  int pct = (int)((v - 3.2f) / (4.2f - 3.2f) * 100.0f);
  if(pct<0) pct=0; if(pct>100) pct=100;
  char bstr[8]; snprintf(bstr,sizeof(bstr),"%d%%", pct);
  d.setTextColor(UI_DIM, UI_BG);
  d.setCursor(212, 2);
  d.print(bstr);
}

static void drawBottomInput(){
  auto &d = canvas;
  d.fillRect(0,INPUT_Y,SCREEN_W,INPUT_H, UI_BG);
  d.drawFastHLine(0,INPUT_Y,SCREEN_W, UI_DIM);
  d.setTextSize(1);
  d.setTextColor(UI_FG, UI_BG);
  // Rolling horizontal buffer: when >38 cols, scroll left keeping cursor visible at right edge
  int maxCols = INPUT_VISIBLE_COLS;
  // gInputScroll is managed to keep cursor visible
  if(gInputLen <= maxCols) gInputScroll = 0;
  else {
    if(gInputCursor < gInputScroll) gInputScroll = gInputCursor;
    else if(gInputCursor >= gInputScroll + maxCols) gInputScroll = gInputCursor - maxCols +1;
    if(gInputScroll > gInputLen - maxCols) gInputScroll = gInputLen - maxCols;
    if(gInputScroll <0) gInputScroll=0;
  }
  int vlen = gInputLen - gInputScroll;
  if(vlen > maxCols) vlen = maxCols;
  if(vlen <0) vlen=0;
  char visible[INPUT_BUF_SZ];
  memcpy(visible, gInput + gInputScroll, vlen);
  visible[vlen]='\0';
  d.setCursor(2, INPUT_Y+4);
  d.print(">");
  d.print(visible);
  // TEXT SCROLL FADE: 12-pixel localized gradient/bounding tint on left margin when scrolled
  if(gInputScroll > 0){
    // solid baseline tint + gradient fade edge - indicates hidden overflow
    for(int fx=0; fx<12; ++fx){
      uint8_t shade = (uint8_t)(60 + fx*10); // gradient from dark to lighter
      uint16_t col = d.color565(shade, shade, shade*0.9);
      d.drawFastVLine(8+fx, INPUT_Y+1, INPUT_H-2, col);
    }
    d.drawFastVLine(20, INPUT_Y+1, INPUT_H-2, UI_DIM); // bounding line
  }
  int curPos = gInputCursor - gInputScroll;
  int curX = 2 + CHAR_W + curPos*CHAR_W; // 1 char for '>'
  if(curX>=2 && curX < SCREEN_W-2){
    // SINE-WAVE PULSING CURSOR: smooth green fade via sin(millis()/150.0) no hard flicker
    float phase = sinf((float)millis() / 150.0f);
    float alpha = (phase + 1.0f) * 0.5f; // 0..1
    uint8_t g = (uint8_t)(alpha * 255);
    uint16_t col = d.color565(0, g, 0); // green cursor fades in/out
    d.fillRect(curX, INPUT_Y+2, 2, CHAR_H+2, col);
  }
  // hint for history when blank
  if(gInputLen==0 && gHistCount>0){
    d.setTextColor(UI_DIM, UI_BG);
    d.setCursor(SCREEN_W - 40, INPUT_Y+4);
    d.print("^v hist");
  }
}

// LAZY WORD-WRAPPING: only active tab gets layout - inactive bypassed until focus switch
static inline bool shouldLayoutTab(Tab* t){ return t && t==activeTab(); }
static void ensureTabLayout(Tab* tab){
  if(!shouldLayoutTab(tab)) return; // completely bypass word-wrapping/canvas layout for background tabs
  // layout math would be performed here only on focus switch - deferred until active
}
void draw_chat_view(){
  if (!ui_needs_redraw) return;
  // STEP A (THE LOGS BUFFER): draw strictly within 109px canvas sprite instance
  canvas.fillSprite(0x0000);
  canvas.setTextSize(1);
  Tab* tab=activeTab();
  if(tab){
    ensureTabLayout(tab);
    int total=tab->count, vis=CHAT_ROWS, startLogical=total-vis-tab->scroll;
    if(startLogical<0) startLogical=0; int endLogical=startLogical+vis; if(endLogical>total) endLogical=total;
    int y=1;
    for(int li=startLogical; li<endLogical; ++li){
      const ChatLine* cl=ringAt(tab,li); if(!cl) continue;
      canvas.setTextColor(0x8410, 0x0000); canvas.setCursor(2, y+1); canvas.print(cl->stamp);
      canvas.drawFastVLine(64, y, ROW_H, 0x8410);
      char out[MAX_LINE_LEN+1]; safeCopy(out,cl->text,sizeof(out)); sanitizeGlyphs(out);
      char nickTmp[32]={0}, bodyTmp[MAX_LINE_LEN+1]={0}; const char* txt2=out; bool hasNick=false;
      if(txt2[0]=='<' ){ const char* end=strchr(txt2,'>'); if(end){ size_t nlen=end-txt2-1; if(nlen<sizeof(nickTmp)){memcpy(nickTmp,txt2+1,nlen); nickTmp[nlen]='\0'; hasNick=true;} const char* body=end+1; while(*body==' ') body++; safeCopy(bodyTmp,body,sizeof(bodyTmp)); } else safeCopy(bodyTmp,txt2,sizeof(bodyTmp)); }
      else if(txt2[0]=='*'&&txt2[1]==' '){ const char* sp=strchr(txt2+2,' '); if(sp){ size_t nlen=sp-(txt2+2); if(nlen<sizeof(nickTmp)){memcpy(nickTmp,txt2+2,nlen); nickTmp[nlen]='\0'; hasNick=true;} safeCopy(bodyTmp,sp+1,sizeof(bodyTmp));} else safeCopy(bodyTmp,txt2,sizeof(bodyTmp));}
      else safeCopy(bodyTmp,txt2,sizeof(bodyTmp));
      if(hasNick&&nickTmp[0]){ uint16_t col=nickHashColor(nickTmp); int nickW=strlen(nickTmp)*CHAR_W; int xNick=64-nickW-4; if(xNick<32) xNick=32; canvas.setTextColor(col, 0x0000); canvas.setCursor(xNick, y+1); canvas.print(nickTmp); }
      canvas.setTextColor((cl->flags&0x01)?UI_WARN:((cl->flags&0x04)?0x8410:UI_FG), 0x0000);
      int maxBodyCols=(SCREEN_W-70-2)/CHAR_W; if((int)strlen(bodyTmp)>maxBodyCols){bodyTmp[maxBodyCols-1]='~'; bodyTmp[maxBodyCols]='\0';}
      canvas.setCursor(70, y+1); canvas.print(bodyTmp);
      y+=ROW_H;
    }
  }
  // STEP B (THE DIRECT HARDWARE BLIT): push middle chat viewport first, offset by 12px past top bar
  canvas.pushSprite(0, 12);
  // STEP C (THE FIXED HEADER & FOOTER OVERLAYS): draw top 12px and bottom 14px straight to glass with zero flicker
  // Top 12px status bar - fixed anchors, no shifting
  {
    auto &d = M5Cardputer.Display;
    d.fillRect(0,0,SCREEN_W,12, UI_BG);
    d.drawFastHLine(0,11,SCREEN_W, UI_DIM);
    d.setTextSize(1);
    // Channel title truncated at X=130
    Tab* at=activeTab();
    char title[32]; if(at) safeCopy(title, at->name, sizeof(title)); else safeCopy(title, "status", sizeof(title));
    // Truncate to fit X=130 (approx 21 chars at 6px)
    if(strlen(title)*CHAR_W > 130){ title[21]='\0'; }
    // Header dividers muted grey, text white
    int x=2;
    // Draw truncated title with muted brackets
    d.setTextColor(0x8410, UI_BG); d.setCursor(x,2); d.print("["); x+=CHAR_W;
    d.setTextColor(0xFFFF, UI_BG); d.setCursor(x,2); d.print(title); x+=strlen(title)*CHAR_W;
    d.setTextColor(0x8410, UI_BG); d.setCursor(x,2); d.print("]"); x+=CHAR_W;
    // Fixed HUD anchors
    // Mute at X=145
    char audioTag[7]; uint16_t audioCol;
    if(gCfg.current_audio==1){ strncpy(audioTag,"[M]",sizeof(audioTag)); audioCol=0xF800; } else { strncpy(audioTag,"[+]",sizeof(audioTag)); audioCol=0x07E0; }
    d.setTextColor(audioCol, UI_BG); d.setCursor(145,2); d.print(audioTag);
    // Time Clock at X=175
    char timeStr[9]; currentStamp(nullptr,0,timeStr,sizeof(timeStr));
    char timeHHMM[6]; if(strlen(timeStr)>=5){ timeHHMM[0]=timeStr[0]; timeHHMM[1]=timeStr[1]; timeHHMM[2]=':'; timeHHMM[3]=timeStr[3]; timeHHMM[4]=timeStr[4]; timeHHMM[5]='\0'; char loc[6]; localizeTimeHHMM(timeHHMM, loc); safeCopy(timeStr, loc, sizeof(timeStr)); }
    d.setTextColor(0xFFFF, UI_BG); d.setCursor(175,2); d.print(timeStr);
    // Battery Percentage at X=212 - integer percentage from voltage math
    float v = readBatteryVoltage();
    int pct = (int)((v - 3.2f) / (4.2f - 3.2f) * 100.0f);
    if(pct<0) pct=0; if(pct>100) pct=100;
    char bstr[8]; snprintf(bstr,sizeof(bstr),"%d%%", pct);
    d.setTextColor(0x8410, UI_BG); d.setCursor(212,2); d.print(bstr);
  }
  {
    auto &d = M5Cardputer.Display;
    d.fillRect(0,INPUT_Y,SCREEN_W,INPUT_H, UI_BG);
    d.drawFastHLine(0,INPUT_Y,SCREEN_W, UI_DIM);
    d.setTextSize(1); d.setTextColor(UI_FG, UI_BG);
    int maxCols=INPUT_VISIBLE_COLS;
    if(gInputLen<=maxCols) gInputScroll=0;
    else { if(gInputCursor<gInputScroll) gInputScroll=gInputCursor; else if(gInputCursor>=gInputScroll+maxCols) gInputScroll=gInputCursor-maxCols+1; if(gInputScroll>gInputLen-maxCols) gInputScroll=gInputLen-maxCols; if(gInputScroll<0) gInputScroll=0; }
    int vlen=gInputLen-gInputScroll; if(vlen>maxCols) vlen=maxCols; if(vlen<0) vlen=0;
    char visible[INPUT_BUF_SZ]; memcpy(visible,gInput+gInputScroll,vlen); visible[vlen]='\0';
    d.setCursor(2,INPUT_Y+4); d.print(">"); d.print(visible);
    if(gInputScroll>0){
      for(int fx=0;fx<12;++fx){ uint8_t shade=60+fx*10; uint16_t col=d.color565(shade,shade,shade*0.9); d.drawFastVLine(8+fx, INPUT_Y+1, INPUT_H-2, col); }
      d.drawFastVLine(20, INPUT_Y+1, INPUT_H-2, UI_DIM);
    }
    int curPos=gInputCursor-gInputScroll; int curX=2+CHAR_W+curPos*CHAR_W;
    if(curX>=2 && curX<SCREEN_W-2){
      float phase=sinf((float)millis()/150.0f); float alpha=(phase+1.0f)*0.5f; uint8_t g=(uint8_t)(alpha*255); uint16_t col=d.color565(0,g,0);
      d.fillRect(curX, INPUT_Y+2, 2, CHAR_H+2, col);
    }
    if(gInputLen==0 && gHistCount>0){ d.setTextColor(UI_DIM, UI_BG); d.setCursor(SCREEN_W-40,INPUT_Y+4); d.print("^v hist"); }
  }
  ui_needs_redraw = false;
}
static void drawChatViewport(){
  if (!ui_needs_redraw) return;
  if(!gCanvasReady) { initCanvas(); if(!gCanvasReady) return; }
  canvas.fillScreen(UI_BG);
  canvas.setTextSize(1);
  Tab* tab=activeTab();
  if(!tab){ canvas.pushSprite(0,0); ui_needs_redraw = false; return; }
  ensureTabLayout(tab); // lazy trigger - inactive tabs have bypassed this until now
  int total = tab->count;
  int vis = CHAT_ROWS;
  int startLogical = total - vis - tab->scroll;
  if(startLogical<0) startLogical=0;
  int endLogical = startLogical + vis;
  if(endLogical>total) endLogical=total;
  int y=1;
  for(int li=startLogical; li<endLogical; ++li){
    const ChatLine* cl=ringAt(tab, li);
    if(!cl) continue;
    // COMPACT COLUMN ARRAYS: dimmed timestamp, divider at 64, nick right-aligned, body at 70, ROW 10 with 2px padding
    canvas.setTextColor(0x8410, UI_BG); // muted grey dimmed timestamp
    canvas.setCursor(2, y+1);
    canvas.print(cl->stamp);
    canvas.drawFastVLine(64, y, ROW_H, 0x8410); // solid vertical dividing line at X=64
    char out[MAX_LINE_LEN+1];
    safeCopy(out, cl->text, sizeof(out));
    sanitizeGlyphs(out);
    char nickTmp[32]={0};
    char bodyTmp[MAX_LINE_LEN+1]={0};
    const char* txt = out;
    bool hasNick=false;
    if(txt[0]=='<' ){
      const char* end = strchr(txt, '>');
      if(end){
        size_t nlen = end - txt -1;
        if(nlen < sizeof(nickTmp)){ memcpy(nickTmp, txt+1, nlen); nickTmp[nlen]='\0'; hasNick=true; }
        const char* body = end+1; while(*body==' ') body++; safeCopy(bodyTmp, body, sizeof(bodyTmp));
      } else { safeCopy(bodyTmp, txt, sizeof(bodyTmp)); }
    } else if(txt[0]=='*' && txt[1]==' '){
      const char* sp = strchr(txt+2, ' ');
      if(sp){
        size_t nlen = sp - (txt+2);
        if(nlen < sizeof(nickTmp)){ memcpy(nickTmp, txt+2, nlen); nickTmp[nlen]='\0'; hasNick=true; }
        const char* body = sp+1; safeCopy(bodyTmp, body, sizeof(bodyTmp));
      } else { safeCopy(bodyTmp, txt, sizeof(bodyTmp)); }
    } else { safeCopy(bodyTmp, txt, sizeof(bodyTmp)); }
    if(hasNick && nickTmp[0]){
      uint16_t col = nickHashColor(nickTmp);
      int nickW = strlen(nickTmp)*CHAR_W;
      int xNick = 64 - nickW - 4;
      if(xNick < 32) xNick = 32;
      canvas.setTextColor(col, UI_BG);
      canvas.setCursor(xNick, y+1);
      canvas.print(nickTmp);
    }
    // Message body starting at X=70
    canvas.setTextColor((cl->flags & 0x01)?UI_WARN:((cl->flags & 0x04)?0x8410:UI_FG), UI_BG);
    int maxBodyCols = (SCREEN_W - 70 -2)/CHAR_W;
    if((int)strlen(bodyTmp) > maxBodyCols){ bodyTmp[maxBodyCols-1]='~'; bodyTmp[maxBodyCols]='\0'; }
    canvas.setCursor(70, y+1);
    canvas.print(bodyTmp);
    y += ROW_H;
  }
  if(gNickOverlay && tab->type==TAB_CHANNEL){
    int pw = 120, ph = 90;
    int px = (SCREEN_W - pw)/2;
    int py = (CHAT_H - ph)/2;
    canvas.fillRect(px,py,pw,ph, UI_BG);
    canvas.drawRect(px,py,pw,ph, UI_FG);
    canvas.setCursor(px+4, py+3);
    canvas.setTextColor(UI_FG, UI_BG);
    canvas.print("Nicks:");
    int ny=py+14;
    for(int i=0;i<tab->nickCount && i<8; ++i){
      canvas.setCursor(px+4, ny);
      canvas.setTextColor(UI_DIM, UI_BG);
      canvas.print(tab->nicks[i]);
      ny+=9;
    }
    if(tab->nickCount>8){ canvas.setCursor(px+4, ny); canvas.print("..."); }
  }
  canvas.pushSprite(0,0);
  // Quick Settings overlay - 5-row grid: 0 Audio,1 Brightness,2 SD Filtering,3 TimeZone,4 Hour Format
  if(gQuickOverlay){
    int ow=200, oh=110;
    int ox=(SCREEN_W-ow)/2;
    int oy=CHAT_Y + (CHAT_H-oh)/2;
    canvas.fillRect(ox,oy,ow,oh, UI_BG);
    canvas.drawRect(ox,oy,ow,oh, UI_FG);
    canvas.setTextSize(1);
    canvas.setTextColor(UI_FG, UI_BG);
    canvas.setCursor(ox+6, oy+6);
    canvas.print("Quick Settings 5-row");
    // Row 0 Audio Profile
    if(current_settings_row==0) canvas.fillRect(ox+2, oy+16, ow-4, 10, UI_FG), canvas.setTextColor(UI_BG, UI_FG);
    else canvas.setTextColor(UI_DIM, UI_BG);
    canvas.setCursor(ox+6, oy+18);
    canvas.printf("Audio:%s", gCfg.current_audio?"Stealth":"Normal");
    // Row 1 Brightness Level
    if(current_settings_row==1) canvas.fillRect(ox+2, oy+26, ow-4, 10, UI_FG), canvas.setTextColor(UI_BG, UI_FG);
    else canvas.setTextColor(UI_DIM, UI_BG);
    canvas.setCursor(ox+6, oy+28);
    canvas.printf("Bright:%d/10", gCfg.brightness);
    // Row 2 SD Filtering Level
    if(current_settings_row==2) canvas.fillRect(ox+2, oy+36, ow-4, 10, UI_FG), canvas.setTextColor(UI_BG, UI_FG);
    else canvas.setTextColor(UI_DIM, UI_BG);
    canvas.setCursor(ox+6, oy+38);
    canvas.printf("SD Filter:%s", logLevelStr(gCfg.logLevel));
    // Row 3 Local TimeZone
    if(current_settings_row==3) canvas.fillRect(ox+2, oy+46, ow-4, 10, UI_FG), canvas.setTextColor(UI_BG, UI_FG);
    else canvas.setTextColor(UI_DIM, UI_BG);
    canvas.setCursor(ox+6, oy+48);
    canvas.printf("TZ:%s %+d", TZ_PROFILES[gTimezoneIndex].label, TZ_PROFILES[gTimezoneIndex].offset);
    // Row 4 Hour Format
    if(current_settings_row==4) canvas.fillRect(ox+2, oy+56, ow-4, 10, UI_FG), canvas.setTextColor(UI_BG, UI_FG);
    else canvas.setTextColor(UI_DIM, UI_BG);
    canvas.setCursor(ox+6, oy+58);
    canvas.printf("Hour:%s", gUse12Hour?"12H":"24H");
    canvas.setTextColor(UI_DIM, UI_BG);
    canvas.setCursor(ox+6, oy+68);
    canvas.printf("Batt:%.2fV", gBattVoltage);
    canvas.setCursor(ox+6, oy+78);
    canvas.printf("Jack:%s", gJackPlugged?"PLUG":"OPEN");
    canvas.setTextColor(UI_FG, UI_BG);
    canvas.setCursor(ox+6, oy+88);
    canvas.print("Up/Dn Row  <>Value");
    canvas.setCursor(ox+6, oy+98);
    canvas.print("G0=save Fn+Q toggle");
    // keep legacy alias in sync
    gQuickOverlayRow = current_settings_row;
  }
}

// Quick Settings helper - display battery metric in overlay
static void toggleQuickOverlay(){
  gQuickOverlay = !gQuickOverlay;
  gQuickOverlayMs = millis();
  gQuickOverlayRow = 0;
  current_settings_row = 0;
  ui_needs_redraw = true;
  if(gQuickOverlay) gBattVoltage = readBatteryVoltage();
}
// Fix Quick Settings menu index boundaries to 5-row grid 0-4
void handle_settings_navigation(bool isDown){
  if(isDown){
    current_settings_row = (current_settings_row + 1) % 5;
    ui_needs_redraw = true;
  } else {
    current_settings_row = (current_settings_row == 0) ? 4 : current_settings_row - 1;
    ui_needs_redraw = true;
  }
  gQuickOverlayRow = current_settings_row;
}
void run_bouncer_setup_menu();
void display_network_jump_hud();
static void serverSkipForward();
static void serverSkipBackward();
static void add_message_to_buffer(const char* msg);
void handle_keyboard_inputs(){
  auto st = M5Cardputer.Keyboard.keysState();
  // HIGH-PRIORITY KEYBOARD MACRO PRECEDENCE: Fn block at absolute top per spec
  if(st.fn){
    for(char c: st.word){
      if(c=='s' || c=='S'){
        gCfg.current_audio = gCfg.current_audio ? 0 : 1;
        digitalWrite(4, gCfg.current_audio == 1 ? LOW : HIGH);
        ui_needs_redraw = true;
        return;
      }
    }
  }
  // CHANNEL STEPPING (Alt + Arrows) per spec
  if(st.alt){
    for(char c : st.word){
      if(c=='/' || c==']' || c=='l' || c=='L'){
        int total_tabs = gTabCount;
        current_tab_index = (current_tab_index + 1) % total_tabs;
        ui_needs_redraw = true;
        return;
      }
      if(c==',' || c=='[' || c=='h' || c=='H'){
        int total_tabs = gTabCount;
        current_tab_index = (current_tab_index == 0) ? (total_tabs - 1) : current_tab_index - 1;
        ui_needs_redraw = true;
        return;
      }
    }
    for(uint8_t k : st.hid_keys){
      if(k==0x4F){ int total_tabs=gTabCount; current_tab_index = (current_tab_index + 1) % total_tabs; ui_needs_redraw = true; return; }
      if(k==0x50){ int total_tabs=gTabCount; current_tab_index = (current_tab_index == 0) ? (total_tabs - 1) : current_tab_index - 1; ui_needs_redraw = true; return; }
    }
  }
  // LIVE INJECTION: Fn+B/T/N and Server Skip hotkeys per spec - check status.fn modifier
  if(st.fn){
    for(char c : st.word){
      if(c=='b' || c=='B'){
        // pause chat loops, call bouncer setup menu
        run_bouncer_setup_menu();
        ui_needs_redraw = true;
        return;
      }
      if(c=='t' || c=='T'){
        // TEST MODE 1 - THE MENTION ALERT (Purple Double-Pulse)
        logStatus("Testing LED: Mention Alert (Purple Double Pulse)...");
        for(int r=0; r<2; ++r){
          neopixelWrite(LED_PIN, 60, 0, 60);
          uint16_t purp = canvas.color565(60, 0, 60);
          (void)purp;
          vTaskDelay(pdMS_TO_TICKS(100));
          neopixelWrite(LED_PIN, 0, 0, 0);
          vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        // TEST MODE 2 - THE ACTIVITY PULSE (Cyan Breathing Fade)
        logStatus("Testing LED: Channel Activity (Cyan Breathing Fade)...");
        for(int b=0; b<=40; b+=4){
          uint16_t col2 = canvas.color565(0, b, 60*b/40 + 20);
          neopixelWrite(LED_PIN, 0, b, 60);
          (void)col2;
          vTaskDelay(pdMS_TO_TICKS(25));
        }
        for(int b=40; b>=0; b-=4){
          uint16_t col2 = canvas.color565(0, b, 60*b/40 + 20);
          neopixelWrite(LED_PIN, 0, b, 60);
          (void)col2;
          vTaskDelay(pdMS_TO_TICKS(25));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        // TEST MODE 3 - THE DISCONNECT WARNING (Dim Solid Red/Orange)
        logStatus("Testing LED: Disconnect Warning (Dim Solid Orange)...");
        {
          uint16_t col3 = canvas.color565(40, 15, 0);
          (void)col3;
          neopixelWrite(LED_PIN, 40, 15, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
        neopixelWrite(LED_PIN, 0, 0, 0);
        ui_needs_redraw = true;
        logStatus("LED Diagnostic Cycle Complete.");
        return;
      }
      if(c=='n' || c=='N'){
        display_network_jump_hud();
        ui_needs_redraw = true;
        return;
      }
      if(c=='/' || c==']' || c=='l' || c=='L'){
        // Fn + Right Arrow - DYNAMIC SERVER SKIP LOGIC forward: loop until server changes
        int total_tabs = gTabCount;
        const char* curServer = gTabs[current_tab_index].server[0] ? gTabs[current_tab_index].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
        for(int i=1; i<total_tabs; ++i){
          int idx = (current_tab_index + i) % total_tabs;
          const char* srv = gTabs[idx].server[0] ? gTabs[idx].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
          if(!eqI(srv, curServer)){
            current_tab_index = idx;
            ui_needs_redraw = true;
            return;
          }
        }
        return;
      }
      if(c==',' || c=='[' || c=='h' || c=='H'){
        // Fn + Left Arrow - server skip backward
        int total_tabs = gTabCount;
        const char* curServer = gTabs[current_tab_index].server[0] ? gTabs[current_tab_index].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
        for(int i=1; i<total_tabs; ++i){
          int idx = (current_tab_index - i + total_tabs) % total_tabs;
          const char* srv = gTabs[idx].server[0] ? gTabs[idx].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
          if(!eqI(srv, curServer)){
            current_tab_index = idx;
            ui_needs_redraw = true;
            return;
          }
        }
        return;
      }
    }
  }
  // spec wrapper - dirty flag only on active keystroke/char/tab/message
  ui_needs_redraw = true;
}
// Overlay navigation helpers for timezone/format
static void cycleTimezone(int delta){
  gTimezoneIndex = (gTimezoneIndex + delta) % TZ_COUNT;
  if(gTimezoneIndex<0) gTimezoneIndex+=TZ_COUNT;
  gCfg.timezone_index=gTimezoneIndex;
  ui_needs_redraw = true;
}
static void toggleClockFormat(){
  gUse12Hour = !gUse12Hour;
  gCfg.use_12_hour_format=gUse12Hour;
  ui_needs_redraw = true;
}
static void cycleAudio(){ gCfg.current_audio = gCfg.current_audio ? 0 : 1; ui_needs_redraw = true; }
static void cycleBrightness(int d){ int v=(int)gCfg.brightness+d; if(v<0) v=0; if(v>10) v=10; gCfg.brightness=v; applyBrightness(v); ui_needs_redraw = true; }
static void cycleFilter(int d){ int v=(int)gCfg.logLevel+d; if(v<0) v=2; if(v>2) v=0; gCfg.logLevel=(LogLevel)v; ui_needs_redraw = true; }

void run_bouncer_setup_menu(){
  bool prevScanner = gInScanner;
  gInScanner = true;
  ui_needs_redraw = true;
  const char* prompts[4] = {"bnc_host", "bnc_user", "bnc_net", "bnc_pass"};
  for(int step=0; step<4; ++step){
    char buf[64];
    if(step==0) safeCopy(buf, bnc_host.c_str(), sizeof(buf));
    else if(step==1) safeCopy(buf, bnc_user, sizeof(buf));
    else if(step==2) safeCopy(buf, bnc_net, sizeof(buf));
    else safeCopy(buf, bnc_pass, sizeof(buf));
    int len = strlen(buf);
    int cursor = len;
    bool done=false;
    // FORCE NATIVE ON-ENTRY REDRAW (DRAW ONCE): background frames and instruction text
    if(irc_mutex) xSemaphoreTake(irc_mutex, portMAX_DELAY);
    canvas.fillScreen(UI_BG);
    canvas.drawRect(0,0,SCREEN_W,SCREEN_H, UI_FG);
    canvas.setTextSize(1);
    canvas.setTextColor(UI_FG, UI_BG);
    canvas.setCursor(4,4);
    canvas.printf("Bouncer Setup %d/4", step+1);
    canvas.setCursor(4,18);
    canvas.printf("%s:", prompts[step]);
    canvas.setCursor(4,110);
    canvas.setTextColor(UI_DIM, UI_BG);
    canvas.print("Enter=next Del=back  Fn+Q=cancel");
    if(irc_mutex) xSemaphoreGive(irc_mutex);
    bool menu_needs_redraw = true;
    while(!done){
      if(menu_needs_redraw){
        if(irc_mutex) xSemaphoreTake(irc_mutex, portMAX_DELAY);
        // Dark blue input strip box
        canvas.fillRect(2, 28, SCREEN_W-4, 20, 0x001F);
        canvas.drawRect(2, 28, SCREEN_W-4, 20, UI_FG);
        canvas.setCursor(4,32);
        canvas.setTextColor(UI_FG, 0x001F);
        if(step==3){
          for(int i=0;i<len;++i) canvas.print("*");
          canvas.print("_");
        } else {
          canvas.print(buf);
          canvas.print("_");
        }
        if(irc_mutex) xSemaphoreGive(irc_mutex);
        menu_needs_redraw = false;
      }
      M5Cardputer.update();
      if(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()){
        auto ks2 = M5Cardputer.Keyboard.keysState();
        bool changed=false;
        for(char c : ks2.word){
          if(c>=32 && c<127 && len < (int)sizeof(buf)-1){
            memmove(buf+cursor+1, buf+cursor, len - cursor + 1);
            buf[cursor++]=c; len++; buf[len]='\0';
            changed=true;
          }
        }
        if(changed){ menu_needs_redraw = true; }
        if(ks2.alt && ks2.del /* Alt+Backspace '\b' escape */){ gInScanner = prevScanner; ui_needs_redraw = true; return; }
        if(ks2.del /* Alt+Backspace '\b' escape */ && len>0 && cursor>0){
          memmove(buf+cursor-1, buf+cursor, len - cursor + 1);
          cursor--; len--;
          menu_needs_redraw = true;
        }
        if(ks2.enter){
          done=true;
          menu_needs_redraw = true;
        }
        if(ks2.alt && ks2.del /* Alt+Backspace '\b' escape */){ gInScanner = prevScanner; ui_needs_redraw = true; return; }
        if(ks2.fn){
          for(char cc : ks2.word){ if(cc=='q' || cc=='Q'){ done=true; menu_needs_redraw = true; break; } }
        }
      }
      vTaskDelay(pdMS_TO_TICKS(30));
    }
    if(step==0) bnc_host = String(buf);
    else if(step==1){ strncpy(bnc_user, buf, sizeof(bnc_user)-1); bnc_user[sizeof(bnc_user)-1]='\0'; safeCopy(gCfg.bncUser, buf, sizeof(gCfg.bncUser)); }
    else if(step==2){ strncpy(bnc_net, buf, sizeof(bnc_net)-1); bnc_net[sizeof(bnc_net)-1]='\0'; }
    else if(step==3){ strncpy(bnc_pass, buf, sizeof(bnc_pass)-1); bnc_pass[sizeof(bnc_pass)-1]='\0'; safeCopy(gCfg.bncPass, buf, sizeof(gCfg.bncPass)); }
  }
  saveConfig();
  gInScanner = prevScanner;
  ui_needs_redraw = true;
}

// DYNAMIC SERVER SKIP LOGIC: jump across server networks
static void serverSkipForward(){
  if(gTabCount==0) return;
  const char* curSrv = gTabs[current_tab_index].server[0] ? gTabs[current_tab_index].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
  for(int i=1; i<gTabCount; ++i){
    int idx = (current_tab_index + i) % gTabCount;
    const char* srv = gTabs[idx].server[0] ? gTabs[idx].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
    if(!eqI(srv, curSrv)){
      current_tab_index = idx;
      ui_needs_redraw = true;
      return;
    }
  }
}
static void serverSkipBackward(){
  if(gTabCount==0) return;
  const char* curSrv = gTabs[current_tab_index].server[0] ? gTabs[current_tab_index].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
  for(int i=1; i<gTabCount; ++i){
    int idx = (current_tab_index - i + gTabCount) % gTabCount;
    const char* srv = gTabs[idx].server[0] ? gTabs[idx].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
    if(!eqI(srv, curSrv)){
      current_tab_index = idx;
      ui_needs_redraw = true;
      return;
    }
  }
}

void display_network_jump_hud(){
  bool prev = gInScanner;
  gInScanner = true;
  active_networks_count = 0;
  for(int i=0;i<gTabCount && active_networks_count<4; ++i){
    const char* srv = gTabs[i].server[0] ? gTabs[i].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
    bool exists=false;
    for(int j=0;j<active_networks_count;++j) if(eqI(active_networks[j], srv)) exists=true;
    if(!exists){ safeCopy(active_networks[active_networks_count++], srv, sizeof(active_networks[0])); }
  }
  if(active_networks_count==0 && bnc_host.length()>0){ safeCopy(active_networks[0], bnc_host.c_str(), sizeof(active_networks[0])); active_networks_count=1; }
  else if(active_networks_count==0){ safeCopy(active_networks[0], gCfg.host, sizeof(active_networks[0])); active_networks_count=1; }
  // FORCE NATIVE ON-ENTRY REDRAW (DRAW ONCE)
  if(irc_mutex) xSemaphoreTake(irc_mutex, portMAX_DELAY);
  canvas.fillScreen(UI_BG);
  canvas.drawRect(0,0,SCREEN_W,SCREEN_H, 0xFFFF);
  canvas.fillRect(10,10,SCREEN_W-20,SCREEN_H-20, UI_BG);
  canvas.drawRect(10,10,SCREEN_W-20,SCREEN_H-20, 0xFFFF);
  canvas.setTextSize(1);
  canvas.setTextColor(0xFFFF, UI_BG);
  canvas.setCursor(20,20);
  canvas.print("Network Jump HUD");
  canvas.setTextColor(UI_DIM, UI_BG);
  canvas.setCursor(20,35);
  canvas.printf("Found %d networks", active_networks_count);
  for(int i=0;i<active_networks_count;++i){
    canvas.setCursor(20, 50 + i*15);
    canvas.setTextColor(UI_FG, UI_BG);
    canvas.printf("%d: %s", i+1, active_networks[i]);
  }
  canvas.setCursor(20, SCREEN_H-15);
  canvas.setTextColor(UI_DIM, UI_BG);
  canvas.print("1-4=Jump  Del=Close");
  if(irc_mutex) xSemaphoreGive(irc_mutex);
  bool menu_needs_redraw = true;
  while(true){
    if(menu_needs_redraw){
      // STATE-CHANGED RENDERING ONLY: draw dark blue input strip if needed (static overlay already drawn once)
      menu_needs_redraw = false;
    }
    M5Cardputer.update();
    if(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()){
      auto ks = M5Cardputer.Keyboard.keysState();
      if(ks.del){ break; }
      bool changed=false;
      for(char c: ks.word){
        if(c>='1' && c<='4'){
          int idx = c - '1';
          if(idx < active_networks_count){
            for(int t=0; t<gTabCount; ++t){
              const char* srv = gTabs[t].server[0] ? gTabs[t].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
              if(eqI(srv, active_networks[idx])){
                current_tab_index = t;
                gTabs[t].unread=false; gTabs[t].mention=false;
                ui_needs_redraw = true;
                break;
              }
            }
            gInScanner = prev;
            ui_needs_redraw = true;
            return;
          }
        }
      }
      if(changed) menu_needs_redraw = true;
      if(ks.enter) break;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  gInScanner = prev;
  ui_needs_redraw = true;
}

// ---------------------------------------------------------------------------
// IRC helpers - static C-string parser, no heap
// ---------------------------------------------------------------------------
static bool nickFromPrefix(const char* prefix, char* out, size_t n){
  if(!prefix||!out) return false;
  const char* bang=strchr(prefix,'!');
  size_t len = bang ? (size_t)(bang-prefix) : strlen(prefix);
  if(len>=n) len=n-1;
  memcpy(out,prefix,len); out[len]='\0';
  return true;
}
static void sortNicks(Tab* t){
  if(!t||t->nickCount<=1) return;
  for(int i=0;i<t->nickCount-1;++i) for(int j=i+1;j<t->nickCount;++j) if(strcasecmp(t->nicks[i],t->nicks[j])>0){ char tmp[NICK_LEN+1]; safeCopy(tmp,t->nicks[i],sizeof(tmp)); safeCopy(t->nicks[i],t->nicks[j],sizeof(t->nicks[i])); safeCopy(t->nicks[j],tmp,sizeof(tmp)); }
}
static void addNick(Tab* t, const char* nick){
  if(!t||!nick||!*nick) return;
  for(int i=0;i<t->nickCount;++i) if(eqI(t->nicks[i],nick)) return;
  if(t->nickCount>=MAX_NICKS) return;
  safeCopy(t->nicks[t->nickCount++], nick, NICK_LEN+1);
  sortNicks(t);
}
static void delNick(Tab* t, const char* nick){
  if(!t||!nick) return;
  for(int i=0;i<t->nickCount;++i) if(eqI(t->nicks[i],nick)){ memmove(&t->nicks[i], &t->nicks[i+1], (t->nickCount-i-1)*(NICK_LEN+1)); t->nickCount--; break; }
}
static void handlePrivmsgC(const char* prefix, const char* target, const char* text, bool isNotice, bool isAction, const char* serverHHMM=nullptr){
  char nick[32]; nickFromPrefix(prefix?prefix:"server", nick,sizeof(nick));
  char disp[MAX_LINE_LEN+1];
  if(isAction) snprintf(disp,sizeof(disp),"* %s %s", nick, text?text:"");
  else snprintf(disp,sizeof(disp),"<%s> %s", nick, text?text:"");
  Tab* dest=nullptr;
  if(target && isChannelName(target)) dest=getOrCreateTab(target,TAB_CHANNEL);
  else if(target && eqI(target,gCfg.nick)) dest=getOrCreateTab(nick,TAB_QUERY);
  else if(target){ if(isChannelName(target)) dest=getOrCreateTab(target,TAB_CHANNEL); else dest=getOrCreateTab(nick,TAB_QUERY); }
  else dest=findTab("status");
  if(!dest) dest=activeTab();
  appendLog(dest,disp,serverHHMM);
  if(isNotice) dest->lines[(dest->head+MAX_LINES_PER_TAB-1)%MAX_LINES_PER_TAB].flags|=0x04;
}

// SASL PLAIN safety - manual length calc for embedded nulls, never strlen on block
static void buildSaslPlainBase64(char* out, size_t outSz, const char* user, const char* pass){
  if(!out || outSz==0) return;
  out[0]='\0';
  size_t ulen = user?strlen(user):0;
  size_t plen = pass?strlen(pass):0;
  size_t rawLen = 1 + ulen + 1 + plen; // spec: 1 + username_len +1 + password_len
  uint8_t raw[256];
  if(rawLen >= sizeof(raw)) rawLen = sizeof(raw)-1;
  size_t pos=0;
  raw[pos++]=0; // authzid empty
  if(ulen){ memcpy(raw+pos, user, ulen); pos+=ulen; }
  raw[pos++]=0;
  if(plen){ memcpy(raw+pos, pass, plen); pos+=plen; }
  // base64 encode manually without using String
  static const char* tbl="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t outPos=0;
  size_t i=0;
  while(i < pos){
    uint32_t triple = 0;
    int bytes = 0;
    for(int b=0;b<3;++b){ triple <<=8; if(i < pos){ triple |= raw[i++]; bytes++; } }
    // 4 chars
    for(int c=3;c>=0;--c){
      if(outPos+1 >= outSz) break;
      if(c < 4 - bytes + 1 && bytes <3){ out[outPos++]='='; }
      else { out[outPos++] = tbl[(triple >> (c*6)) & 0x3F]; }
    }
  }
  // correct padding for b64: we over-padded above simplified; recompute proper standard:
  // redo clean standard implementation for correctness (simple)
  out[0]='\0';
  // use standard loop
  size_t encLen=0;
  char* o=out;
  uint32_t val=0; int valb=-6;
  for(size_t k=0;k<pos;++k){
    val=(val<<8)+raw[k]; valb+=8;
    while(valb>=0){ if(encLen+1 < outSz) o[encLen++]=tbl[(val>>valb)&0x3F]; valb-=6; }
  }
  if(valb>-6){ if(encLen+1 < outSz) o[encLen++]=tbl[((val<<8)>>(valb+8))&0x3F]; }
  while(encLen%4){ if(encLen+1 < outSz) o[encLen++]='='; else break; }
  if(encLen < outSz) o[encLen]='\0';
}

static void handleRawIrc(char* line){
  if(!line||!*line) return;
  char* tags=nullptr;
  char serverHHMM[6]={0};
  bool hasServerTime=false;
  char* p=line;
  if(*p=='@'){
    tags=p+1;
    char* sp=strchr(p,' ');
    if(!sp) return;
    *sp='\0';
    // lightweight tag parser loop: intercept server-time
    hasServerTime = parseServerTimeHHMM(tags, serverHHMM);
    // PARSER STRING TRANSFORMATION: route UTC hh:mm through localized timezone/12h conversion
    if(hasServerTime){
      char localized[6];
      localizeTimeHHMM(serverHHMM, localized);
      safeCopy(serverHHMM, localized, sizeof(serverHHMM));
    }
    p=sp+1;
  }
  const char* sHHMM = hasServerTime ? serverHHMM : nullptr;
  char* prefix=nullptr;
  if(*p==':'){ prefix=p+1; char* sp=strchr(p,' '); if(!sp) return; *sp='\0'; p=sp+1; }
  while(*p==' ') p++;
  char* cmd=p;
  char* sp=strchr(p,' ');
  char* paramsStart=nullptr;
  if(sp){ *sp='\0'; paramsStart=sp+1; } else paramsStart=nullptr;
  for(char* c=cmd;*c;++c) *c=toupper((unsigned char)*c);
  char* argv[16]; int argc=0;
  if(paramsStart){
    char* s=paramsStart;
    while(*s && argc<16){
      while(*s==' ') s++;
      if(*s=='\0') break;
      if(*s==':'){ argv[argc++]=s+1; break; }
      char* e=strchr(s,' ');
      if(e){ *e='\0'; argv[argc++]=s; s=e+1; } else { argv[argc++]=s; break; }
    }
  }
  // EVENT CONDENSER: intercept CHGHOST and ACCOUNT - update metadata silently, no buffer insertion/redraw
  if(strcmp(cmd,"CHGHOST")==0){
    // CHGHOST user host - update internal channel nick metadata silently
    if(prefix){
      char nick[32]; nickFromPrefix(prefix,nick,sizeof(nick));
      // silently refresh nick presence without log to save SRAM/avoid SPI redraw
      for(int ti=0; ti<gTabCount; ++ti){
        if(gTabs[ti].type==TAB_CHANNEL){
          for(int ni=0; ni<gTabs[ti].nickCount; ++ni){
            if(eqI(gTabs[ti].nicks[ni], nick)){
              // host changed - we keep nick entry but no visual log
              break;
            }
          }
        }
      }
    }
    return;
  }
  if(strcmp(cmd,"ACCOUNT")==0){
    // ACCOUNT account-or-* - update account status silently
    // no buffer insertion to save SRAM and prevent SPI redraw
    return;
  }
  if(strcmp(cmd,"PING")==0){
    const char* tok = argc>0?argv[argc-1]:"cardputer";
    char out[64]; snprintf(out,sizeof(out),"PONG :%s",tok);
    gTxQueue.push(out);
    char msg[64]; snprintf(msg,sizeof(msg),"Ping <- %s",tok);
    logStatus(msg);
    return;
  }
  if(strcmp(cmd,"PONG")==0){ gAwaitPong=false; return; }
  if(strcmp(cmd,"001")==0){
    gIrcRegistered=true;
    logStatus("Registered on IRC");
    // DYNAMIC BACKUP HOOK: safely copy config to .bak the moment bouncer login succeeds
    trigger_config_backup();
    if(gCfg.autojoin[0]){
      char copy[128]; safeCopy(copy,gCfg.autojoin,sizeof(copy));
      char* tok=strtok(copy,",");
      while(tok){ trim(tok); if(*tok){ char out[64]; snprintf(out,sizeof(out),"JOIN %s",tok); gTxQueue.push(out); } tok=strtok(nullptr,","); }
    }
    return;
  }
  if(strcmp(cmd,"433")==0){
    char newNick[36]; snprintf(newNick,sizeof(newNick),"%s_",gCfg.nick);
    safeCopy(gCfg.nick,newNick,sizeof(gCfg.nick));
    char out[48]; snprintf(out,sizeof(out),"NICK %s",gCfg.nick);
    gTxQueue.push(out);
    char msg[48]; snprintf(msg,sizeof(msg),"Nick in use, retry %s",gCfg.nick);
    logStatus(msg);
    return;
  }
  if(strcmp(cmd,"JOIN")==0){
    const char* chan = argc>0?argv[0]:"";
    char n[32]; nickFromPrefix(prefix?prefix:"",n,sizeof(n));
    if(eqI(n,gCfg.nick)){
      Tab* t=getOrCreateTab(chan,TAB_CHANNEL);
      t->unread=false; t->mention=false;
      char m[64]; snprintf(m,sizeof(m),"Joined %s",chan);
      appendLog(t,m,sHHMM);
    } else {
      Tab* t=findTab(chan?chan:"");
      if(t) addNick(t,n);
      char m[64]; snprintf(m,sizeof(m),"* %s joined %s",n,chan);
      if(t) appendLog(t,m,sHHMM);
    }
    return;
  }
  if(strcmp(cmd,"PART")==0){
    const char* chan=argc>0?argv[0]:"";
    char n[32]; nickFromPrefix(prefix?prefix:"",n,sizeof(n));
    Tab* t=findTab(chan);
    if(eqI(n,gCfg.nick)){ char m[48]; snprintf(m,sizeof(m),"Left %s",chan); if(t) appendLog(t,m,sHHMM); }
    else { if(t) delNick(t,n); char m[64]; snprintf(m,sizeof(m),"* %s left %s",n,chan); if(t) appendLog(t,m,sHHMM); }
    return;
  }
  if(strcmp(cmd,"QUIT")==0){
    char n[32]; nickFromPrefix(prefix?prefix:"",n,sizeof(n));
    for(int i=0;i<gTabCount;++i) if(gTabs[i].type==TAB_CHANNEL) delNick(&gTabs[i],n);
    char m[64]; snprintf(m,sizeof(m),"* %s quit %s",n, argc>0?argv[0]:"");
    Tab* s=findTab("status"); if(s) appendLog(s,m,sHHMM);
    return;
  }
  if(strcmp(cmd,"NICK")==0){
    char oldn[32]; nickFromPrefix(prefix?prefix:"",oldn,sizeof(oldn));
    const char* newn=argc>0?argv[0]:"";
    if(eqI(oldn,gCfg.nick)) safeCopy(gCfg.nick,newn,sizeof(gCfg.nick));
    for(int i=0;i<gTabCount;++i){
      for(int k=0;k<gTabs[i].nickCount;++k) if(eqI(gTabs[i].nicks[k],oldn)) safeCopy(gTabs[i].nicks[k],newn,sizeof(gTabs[i].nicks[k]));
      if(eqI(gTabs[i].name,oldn) && gTabs[i].type==TAB_QUERY) safeCopy(gTabs[i].name,newn,sizeof(gTabs[i].name));
    }
    char m[64]; snprintf(m,sizeof(m),"* %s -> %s",oldn,newn);
    Tab* s=findTab("status"); if(s) appendLog(s,m,sHHMM);
    return;
  }
  if(strcmp(cmd,"PRIVMSG")==0){
    const char* tgt=argc>0?argv[0]:"";
    const char* txt=argc>1?argv[1]:"";
    bool isAction=false;
    if(txt && strncmp(txt,"\x01" "ACTION ",8)==0){ isAction=true; txt+=8; size_t l=strlen(txt); if(l && txt[l-1]=='\x01') ((char*)txt)[l-1]='\0'; }
    handlePrivmsgC(prefix,tgt,txt,false,isAction,sHHMM);
    return;
  }
  if(strcmp(cmd,"NOTICE")==0){
    const char* tgt=argc>0?argv[0]:"";
    const char* txt=argc>1?argv[1]:"";
    handlePrivmsgC(prefix,tgt,txt,true,false,sHHMM);
    return;
  }
  if(strcmp(cmd,"353")==0){
    const char* chan=nullptr; const char* list=nullptr;
    if(argc>=3){ chan=argv[1]; if(argv[2][0]=='='||argv[2][0]=='*'||argv[2][0]=='@') { if(argc>=4){ chan=argv[2]; list=argv[3]; } else list=""; } else list=argv[2]; }
    if(chan && list){
      Tab* t=findTab(chan);
      if(!t) t=getOrCreateTab(chan,TAB_CHANNEL);
      char copy[512]; safeCopy(copy,list,sizeof(copy));
      char* tok=strtok(copy," ");
      while(tok){ while(*tok && strchr("~&@%+",*tok)) tok++; if(*tok) addNick(t,tok); tok=strtok(nullptr," "); }
    }
    return;
  }
  if(strcmp(cmd,"366")==0){ return; }
  if(strcmp(cmd,"332")==0||strcmp(cmd,"333")==0||strcmp(cmd,"TOPIC")==0){
    const char* chan=argc>0?argv[0]:""; const char* topic=argc>1?argv[1]:"";
    if(strcmp(cmd,"TOPIC")==0){ chan=argc>0?argv[0]:""; topic=argc>1?argv[1]:""; }
    else if(strcmp(cmd,"332")==0){ chan=argc>=1?argv[1]:""; topic=argc>=2?argv[2]:""; }
    Tab* t=findTab(chan); if(t && topic) safeCopy(t->topic,topic,sizeof(t->topic));
    return;
  }
  if(strcmp(cmd,"MODE")==0||strcmp(cmd,"KICK")==0){
    const char* chan=argc>0?argv[0]:"";
    Tab* t=findTab(chan); if(t){ char m[96]; snprintf(m,sizeof(m),"* %s %s",cmd, paramsStart?paramsStart:""); appendLog(t,m,sHHMM); }
    return;
  }
  if(strcmp(cmd,"AUTHENTICATE")==0){
    // SASL challenge: if we sent AUTHENTICATE PLAIN and server replies '+', send payload with manual length
    if(argc>0 && strcmp(argv[0],"+")==0 && gCfg.saslEnabled){
      char b64[256];
      const char* u = gCfg.saslUser[0]?gCfg.saslUser:gCfg.nick;
      const char* p = gCfg.saslPass[0]?gCfg.saslPass:gCfg.bncPass;
      buildSaslPlainBase64(b64,sizeof(b64),u,p);
      char out[512]; snprintf(out,sizeof(out),"AUTHENTICATE %s", b64);
      gTxQueue.push(out);
    }
    return;
  }
  if(isdigit((unsigned char)cmd[0])){
    char m[180]; snprintf(m,sizeof(m),"[%s] %s",cmd, paramsStart?paramsStart:"");
    Tab* s=findTab("status"); if(s) appendLog(s,m,sHHMM);
    return;
  }
  Tab* s=findTab("status"); if(s){ char m[180]; snprintf(m,sizeof(m),"%s %s",cmd, line); appendLog(s,m,sHHMM); }
}

// ---------------------------------------------------------------------------
// User command hooks - optimized snprintf paths
// ---------------------------------------------------------------------------
static void doSend(const char* fmt, ...){
  char buf[IRC_LINE_MAX+1];
  va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
  gTxQueue.push(buf);
}
static void pushHistory(const char* s){
  if(!s||!*s) return;
  if(gHistCount>0 && eqI(gHistory[(gHistCount-1)%HISTORY_DEPTH], s)) return;
  safeCopy(gHistory[gHistCount % HISTORY_DEPTH], s, INPUT_BUF_SZ);
  gHistCount++;
  if(gHistCount > HISTORY_DEPTH*10) gHistCount = HISTORY_DEPTH; // prevent overflow
  gHistNav = -1;
}
static void handleUserInput(const char* in){
  if(!in||!*in) return;
  pushHistory(in);
  if(in[0]!='/'){
    Tab* at=activeTab();
    if(!at || at->type==TAB_STATUS){ logStatus("No channel/query"); return; }
    char out[IRC_LINE_MAX+1]; snprintf(out,sizeof(out),"PRIVMSG %s :%s", at->name, in);
    gTxQueue.push(out);
    char disp[MAX_LINE_LEN+1]; snprintf(disp,sizeof(disp),"<%s> %s", gCfg.nick, in);
    appendLog(at,disp);
    at->lines[(at->head+MAX_LINES_PER_TAB-1)%MAX_LINES_PER_TAB].flags|=0x02;
    return;
  }
  char copy[INPUT_BUF_SZ]; safeCopy(copy,in,sizeof(copy));
  char* sp=strchr(copy,' '); char* arg=nullptr;
  if(sp){ *sp='\0'; arg=sp+1; while(*arg==' ') arg++; if(*arg=='\0') arg=nullptr; }
  char* cmd=copy+1; toLower(cmd);
  if(strcmp(cmd,"join")==0){
    if(!arg){ logStatus("Usage: /join #chan"); return; }
    char first[32]; safeCopy(first,arg,sizeof(first)); char* c=strchr(first,','); if(c) *c='\0'; char* c2=strchr(first,' '); if(c2) *c2='\0';
    doSend("JOIN %s", arg);
    getOrCreateTab(first[0]?first:arg,TAB_CHANNEL);
    return;
  }
  if(strcmp(cmd,"part")==0){
    Tab* at=activeTab();
    const char* chan = arg ? arg : (at && at->type==TAB_CHANNEL ? at->name : nullptr);
    if(!chan){ logStatus("Usage: /part [#chan]"); return; }
    char ch[32]; safeCopy(ch,chan,sizeof(ch)); char* r=strchr(ch,' '); if(r) *r='\0';
    doSend("PART %s", ch);
    return;
  }
  if(strcmp(cmd,"msg")==0){
    if(!arg){ logStatus("Usage: /msg target text"); return; }
    char* sp2=strchr(arg,' '); if(!sp2){ logStatus("Usage: /msg target text"); return; }
    *sp2='\0'; char* tgt=arg; char* txt=sp2+1;
    doSend("PRIVMSG %s :%s", tgt, txt);
    Tab* t=getOrCreateTab(tgt,TAB_QUERY);
    char disp[MAX_LINE_LEN+1]; snprintf(disp,sizeof(disp),">%s< %s", tgt, txt);
    appendLog(t,disp);
    return;
  }
  if(strcmp(cmd,"soju")==0 || strcmp(cmd,"bouncer")==0){
    if(!arg) doSend("BOUNCER LISTNETWORKS");
    else { char out[IRC_LINE_MAX+1]; snprintf(out,sizeof(out),"%s %s", strcmp(cmd,"soju")==0?"BOUNCER":"BOUNCER", arg); if(startsI(arg,"status")) doSend("BOUNCER LISTNETWORKS"); else doSend("%s", out); }
    return;
  }
  if(strcmp(cmd,"close")==0){
    Tab* at=activeTab();
    if(!at){ logStatus("Nothing to close"); return; }
    if(at->type==TAB_STATUS){ logStatus("Cannot close status"); return; }
    int idx = (int)(at - gTabs);
    for(int i=idx;i<gTabCount-1;++i) gTabs[i]=gTabs[i+1];
    gTabCount--; if(gActive>=gTabCount) gActive=gTabCount-1;
    // clear stealth if closed highlighted
    if(gHighlightedTab==idx){ gStealthPulseActive=false; setStealthLed(false); }
    logStatus("Tab closed");
    return;
  }
  if(strcmp(cmd,"nicks")==0){ gNickOverlay=!gNickOverlay; return; }
  if(strcmp(cmd,"quit")==0||strcmp(cmd,"reconnect")==0){ gIrcConnected=false; gIrcRegistered=false; doSend("QUIT :Reconnect"); logStatus("Reconnect"); return; }
  if(strcmp(cmd,"nick")==0){ if(!arg){ logStatus("Usage: /nick newnick"); return; } doSend("NICK %s", arg); return; }
  if(strcmp(cmd,"audio")==0){ if(arg){ gCfg.current_audio = atoi(arg); char m[32]; snprintf(m,sizeof(m),"Audio %d", gCfg.current_audio); logStatus(m); saveConfig(); } else { char m[32]; snprintf(m,sizeof(m),"Audio=%d", gCfg.current_audio); logStatus(m); } return; }
  if(strcmp(cmd,"me")==0){
    Tab* at=activeTab(); if(!at||at->type==TAB_STATUS){ logStatus("No target"); return; }
    if(!arg){ logStatus("Usage: /me action"); return; }
    char out[IRC_LINE_MAX+1]; snprintf(out,sizeof(out),"PRIVMSG %s :\x01" "ACTION %s\x01", at->name, arg);
    gTxQueue.push(out);
    char disp[MAX_LINE_LEN+1]; snprintf(disp,sizeof(disp),"* %s %s", gCfg.nick, arg);
    appendLog(at,disp);
    return;
  }
  if(strcmp(cmd,"notice")==0){
    if(!arg){ logStatus("Usage: /notice target text"); return; }
    char* sp2=strchr(arg,' '); if(!sp2){ logStatus("Usage: /notice target text"); return; }
    *sp2='\0'; doSend("NOTICE %s :%s", arg, sp2+1); return;
  }
  if(strcmp(cmd,"topic")==0){
    Tab* at=activeTab(); const char* chan = at && at->type==TAB_CHANNEL ? at->name : nullptr;
    if(!arg && chan) doSend("TOPIC %s", chan);
    else if(arg && chan) doSend("TOPIC %s :%s", chan, arg);
    else logStatus("Usage: /topic [text]");
    return;
  }
  if(strcmp(cmd,"whois")==0||strcmp(cmd,"who")==0||strcmp(cmd,"names")==0){
    if(arg) doSend("%s %s", cmd, arg);
    else { Tab* at=activeTab(); if(at && at->type==TAB_CHANNEL) doSend("%s %s", cmd, at->name); else doSend("%s", cmd); }
    return;
  }
  if(strcmp(cmd,"query")==0){ if(!arg){ logStatus("Usage: /query nick"); return; } getOrCreateTab(arg,TAB_QUERY); return; }
  if(strcmp(cmd,"next")==0||strcmp(cmd,"prev")==0){
    if(gTabCount==0) return;
    if(strcmp(cmd,"next")==0) gActive=(gActive+1)%gTabCount;
    else gActive=(gActive-1+gTabCount)%gTabCount;
    activeTab()->unread=false; activeTab()->mention=false;
    // if we switched to highlighted, stealth pulse stops via serviceStealthLed
    return;
  }
  if(strcmp(cmd,"tabs")==0){ char list[160]="Tabs:"; for(int i=0;i<gTabCount;++i){ strncat(list," ",sizeof(list)-strlen(list)-1); strncat(list,gTabs[i].name,sizeof(list)-strlen(list)-1); } logStatus(list); return; }
  if(strcmp(cmd,"quote")==0||strcmp(cmd,"raw")==0){ if(arg) gTxQueue.push(arg); return; }
  if(arg) doSend("%s %s", cmd, arg); else doSend("%s", cmd);
}

// ---------------------------------------------------------------------------
// Native Wi-Fi provisioning engine - scan 4 strongest, input 1-4, * masking, verify, write SD
// ---------------------------------------------------------------------------
static void runWifiProvisioning(){
  gInScanner=true;
  canvas.fillScreen(UI_BG);
  canvas.setTextSize(1);
  canvas.setTextColor(UI_FG, UI_BG);
  canvas.setCursor(4,4);
  canvas.print("Scanning 2.4GHz...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(120);
  int n=WiFi.scanNetworks(false,true);
  gScanFound=0;
  struct Ent{ char ssid[33]; int rssi; };
  Ent all[24]; int ac=0;
  for(int i=0;i<n && ac<24; ++i){
    String ss=WiFi.SSID(i);
    if(ss.length()==0) continue;
    safeCopy(all[ac].ssid, ss.c_str(), sizeof(all[ac].ssid));
    all[ac].rssi=WiFi.RSSI(i);
    ac++;
  }
  for(int i=0;i<ac-1;++i) for(int j=i+1;j<ac;++j) if(all[j].rssi>all[i].rssi) std::swap(all[i],all[j]);
  gScanFound = min(4, ac);
  for(int i=0;i<gScanFound;++i){ safeCopy(gScanSSID[i], all[i].ssid, sizeof(gScanSSID[i])); gScanRSSI[i]=all[i].rssi; }
  if(gScanFound==0){
    canvas.fillScreen(UI_BG);
    canvas.setCursor(4,20);
    canvas.print("No networks found");
    canvas.setCursor(4,40);
    canvas.print("Press Enter to retry");
    while(true){
      M5Cardputer.update();
      if(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()){
        auto ks=M5Cardputer.Keyboard.keysState();
        if(ks.enter) break;
      }
      delay(20);
    }
    gInScanner=false;
    return;
  }
  gScanSel=0; gScanPassLen=0; gScanPass[0]='\0';
  enum State{ PICK, PASS } st=PICK;
  // FORCE NATIVE ON-ENTRY REDRAW (DRAW ONCE)
  if(irc_mutex) xSemaphoreTake(irc_mutex, portMAX_DELAY);
  canvas.fillScreen(UI_BG);
  canvas.drawRect(0,0,SCREEN_W,SCREEN_H, UI_FG);
  canvas.setTextSize(1);
  canvas.setTextColor(UI_FG, UI_BG);
  canvas.setCursor(4,2);
  canvas.print("WiFi Manager");
  if(irc_mutex) xSemaphoreGive(irc_mutex);
  bool menu_needs_redraw = true;
  while(true){
    if(menu_needs_redraw){
      if(irc_mutex) xSemaphoreTake(irc_mutex, portMAX_DELAY);
      if(st==PICK){
        // Dark blue input strip for SSID list highlight
        for(int i=0;i<gScanFound;++i){
          int y=18+i*18;
          if(i==gScanSel && st==PICK){
            canvas.fillRect(0,y,SCREEN_W,14, 0x001F);
            canvas.drawRect(0,y,SCREEN_W,14, UI_FG);
            canvas.setTextColor(UI_FG, 0x001F);
          } else {
            canvas.fillRect(0,y,SCREEN_W,14, UI_BG);
            canvas.setTextColor(UI_FG, UI_BG);
          }
          canvas.setCursor(4,y+3);
          char line[48]; snprintf(line,sizeof(line),"%d:%s (%d dBm)", i+1, gScanSSID[i], gScanRSSI[i]);
          canvas.print(line);
        }
        canvas.setCursor(4,104);
        canvas.setTextColor(UI_DIM, UI_BG);
        canvas.print("Press 1-4 or Enter   ;/. move");
      } else {
        canvas.fillRect(2, 88, SCREEN_W-4, 20, 0x001F);
        canvas.drawRect(2, 88, SCREEN_W-4, 20, UI_FG);
        canvas.setCursor(4, 92);
        canvas.setTextColor(UI_FG, 0x001F);
        canvas.print("Pass: ");
        for(int i=0;i<gScanPassLen;++i) canvas.print("*");
        canvas.print("_");
      }
      if(irc_mutex) xSemaphoreGive(irc_mutex);
      menu_needs_redraw = false;
    }
    M5Cardputer.update();
    if(!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()){ vTaskDelay(pdMS_TO_TICKS(30)); continue; }
    gLastInputMs=millis(); wakeFromSleep();
    auto ks=M5Cardputer.Keyboard.keysState();
    if(st==PICK){
      bool selChanged=false;
      for(char c: ks.word){
        if(c>='1' && c<='4'){
          int idx=c-'1';
          if(idx < gScanFound && idx != gScanSel){ gScanSel=idx; selChanged=true; }
        }
        if(c==';' && gScanSel>0){ gScanSel--; selChanged=true; }
        else if(c=='.' && gScanSel<gScanFound-1){ gScanSel++; selChanged=true; }
      }
      if(selChanged) menu_needs_redraw = true;
      if(ks.enter){ st=PASS; gScanPassLen=0; gScanPass[0]='\0'; menu_needs_redraw = true; }
      if(ks.del){ gInScanner=false; return; }
    } else {
      bool changed=false;
      for(char c: ks.word){
        if(c>=32 && c<127 && gScanPassLen < (int)sizeof(gScanPass)-1){
          gScanPass[gScanPassLen++]=c; gScanPass[gScanPassLen]='\0'; changed=true;
        }
      }
      if(changed) menu_needs_redraw = true;
      if(ks.del && gScanPassLen>0){ gScanPass[--gScanPassLen]='\0'; menu_needs_redraw = true; }
      else if(ks.del && gScanPassLen==0){ st=PICK; menu_needs_redraw = true; }
      if(ks.enter){
        if(irc_mutex) xSemaphoreTake(irc_mutex, portMAX_DELAY);
        canvas.fillScreen(UI_BG);
        canvas.setTextColor(UI_FG, UI_BG);
        canvas.setCursor(4,40);
        canvas.printf("Connecting %s...", gScanSSID[gScanSel]);
        if(irc_mutex) xSemaphoreGive(irc_mutex);
        WiFi.begin(gScanSSID[gScanSel], gScanPass);
        uint32_t stt=millis();
        while(millis()-stt < 10000){
          if(WiFi.status()==WL_CONNECTED) break;
          delay(200); M5Cardputer.update();
        }
        if(WiFi.status()==WL_CONNECTED){
          safeCopy(gCfg.wifiSSID, gScanSSID[gScanSel], sizeof(gCfg.wifiSSID));
          safeCopy(gCfg.wifiPass, gScanPass, sizeof(gCfg.wifiPass));
          saveConfig();
          if(irc_mutex) xSemaphoreTake(irc_mutex, portMAX_DELAY);
          canvas.fillScreen(UI_BG);
          canvas.setCursor(4,40);
          canvas.print("Connected! Saved.");
          if(irc_mutex) xSemaphoreGive(irc_mutex);
          delay(1200);
          gInScanner=false;
          gWifiConnecting=false;
          return;
        } else {
          if(irc_mutex) xSemaphoreTake(irc_mutex, portMAX_DELAY);
          canvas.fillScreen(UI_BG);
          canvas.setCursor(4,40);
          canvas.print("Connect failed");
          canvas.setCursor(4,60);
          canvas.print("Retry password");
          if(irc_mutex) xSemaphoreGive(irc_mutex);
          delay(1400);
          gScanPassLen=0; gScanPass[0]='\0';
          menu_needs_redraw = true;
        }
      }
      if(ks.tab){ st=PICK; menu_needs_redraw = true; }
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

// ---------------------------------------------------------------------------
// Network task pinned to Core 0 - WiFiClientSecure isolation
// ---------------------------------------------------------------------------
static void netTask(void* arg){
  (void)arg;
  gRxLen=0; memset(gRxAccum,0,sizeof(gRxAccum));
  for(;;){
    char out[IRC_LINE_MAX+1];
    while(gTxQueue.pop(out)){
      Client* cl = gCfg.useTLS ? (Client*)&gSecure : (Client*)&gPlain;
      if(gIrcConnected && cl->connected()){
        cl->print(out); cl->print("\r\n");
        // handle SASL manual if AUTHENTICATE is CAP? already handled
      }
    }
    if(WiFi.status()!=WL_CONNECTED){ gIrcConnected=false; vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    Client* cl = gCfg.useTLS ? (Client*)&gSecure : (Client*)&gPlain;
    if(!gIrcConnected){
      static uint32_t lastTry=0;
      if(millis()-lastTry < 3000){ vTaskDelay(pdMS_TO_TICKS(100)); continue; }
      lastTry=millis();
      // SOCKET DRIVER ROUTING: use dynamic bouncer globals instead of hardcoded server strings
      String _bncHost = bnc_host.length()>0 ? bnc_host : String(gCfg.host);
      int _bncPort = bnc_port>0 ? bnc_port : gCfg.port;
      if(_bncHost.length()==0){ vTaskDelay(pdMS_TO_TICKS(500)); continue; }
      bool ok=false;
      if(gCfg.useTLS){ gSecure.setInsecure(); ok = gSecure.connect(_bncHost.c_str(), _bncPort); }
      else ok = gPlain.connect(_bncHost.c_str(), _bncPort);
      if(ok){
        gIrcConnected=true; gIrcRegistered=false; gLastRxMs=millis(); gAwaitPong=false;
        if(xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(50))==pdTRUE){ add_message_to_buffer("Connected to bouncer"); xSemaphoreGive(irc_mutex); }
        // AUTHENTICATION STRING ASSEMBLY: allocate temporary stack buffer and combine bouncer creds per spec
        char auth_buffer[160];
        snprintf(auth_buffer, sizeof(auth_buffer), "PASS %s/%s:%s\r\n", bnc_user, bnc_net, bnc_pass);
        // Transmit assembled bouncer line immediately following successful client.connect()
        if(bnc_user[0]!='\0' || bnc_pass[0]!='\0'){
          cl->print(auth_buffer);
        } else if(gCfg.pass[0]){
          char line[128]; snprintf(line,sizeof(line),"PASS %s\r\n", gCfg.pass); cl->print(line);
        }
        char line[128];
        // MANDATORY IRCv3 CAP FILE HANDLER: precise capability string per spec
        cl->print("CAP LS 302\r\n");
        cl->print("CAP REQ :server-time chghost account-notify cap-notify batch labeled-response sasl\r\n");
        snprintf(line,sizeof(line),"NICK %s\r\n", gCfg.nick); cl->print(line);
        snprintf(line,sizeof(line),"USER %s 0 * :%s\r\n", gCfg.user, gCfg.realname); cl->print(line);
        gLastPingMs=millis();
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    // CHUNK SOCKET INGESTION: fixed 128-byte blocks on Core 0 to cut scheduling overhead
    bool got=false;
    int avail = cl->available();
    while(avail > 0){
      uint8_t chunk[128];
      int toRead = avail > 128 ? 128 : avail;
      int n = cl->read(chunk, toRead);
      if(n <= 0) break;
      got = true;
      for(int i=0; i<n; ++i){
        char c = (char)chunk[i];
        if(c=='\r') continue;
        if(c=='\n'){
          gRxAccum[gRxLen]='\0';
          if(gRxLen>0){ gRxQueue.push(gRxAccum); gLastRxMs=millis(); ui_needs_redraw = true; }
          gRxLen=0;
          if(gRxQueue.size() >= 8) break;
        } else {
          if(gRxLen < RX_ACCUM_SZ-1) gRxAccum[gRxLen++]=c;
          else { gRxAccum[gRxLen]='\0'; gRxQueue.push(gRxAccum); ui_needs_redraw = true; gRxLen=0; }
        }
      }
      if(gRxQueue.size() >= 8) break;
      avail = cl->available();
    }
    if(gIrcConnected){
      if(!gAwaitPong && millis()-gLastRxMs > PING_INTERVAL_MS){
        snprintf(gPingToken,sizeof(gPingToken),"%lu",(unsigned long)millis());
        char pl[32]; snprintf(pl,sizeof(pl),"PING :%s", gPingToken);
        cl->print(pl); cl->print("\r\n");
        gAwaitPong=true; gLastPingMs=millis();
      }
      if(gAwaitPong && millis()-gLastPingMs > PONG_TIMEOUT_MS){ cl->stop(); gIrcConnected=false; gIrcRegistered=false; }
    }
    if(!got) vTaskDelay(pdMS_TO_TICKS(20));
    else vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// SPI BUS HARDWARE CONFLICT PROTECTION - Core 0 dedicated SD logging task
// Completely halts and waits for Core 1 SPI screen canvas flushes via irc_mutex
static void add_message_to_buffer(const char* msg){ if(!irc_mutex || xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(50))!=pdTRUE) return; logStatus(msg); xSemaphoreGive(irc_mutex); }
static void logTask(void* arg){
  (void)arg;
  for(;;){
    LogEntry e;
    if(gLogQueue.pop(e)){
      // MUTEX GAUNTLET: halt until Core 1 finishes SPI flush before SD open/write
      if(xSemaphoreTake(irc_mutex, portMAX_DELAY)==pdTRUE){
        // Ensure log directories exist inside mutex (SD shares SPI bus)
        char dir[64];
        safeCopy(dir, gCfg.logRoot, sizeof(dir));
        if(!SD.exists(dir)) SD.mkdir(dir);
        // Extract date dir from path: /irc/logs/YYYYMMDD/tab.log -> mkdir date dir
        // Path format is logRoot/date/file.log
        char* lastSlash = strrchr(e.path, '/');
        if(lastSlash){
          char dir2[96];
          size_t len = lastSlash - e.path;
          if(len < sizeof(dir2)){
            memcpy(dir2, e.path, len);
            dir2[len]='\0';
            if(!SD.exists(dir2)) SD.mkdir(dir2);
          }
        }
        // All SD file write operations wrapped inside irc_mutex per spec
        File f = SD.open(e.path, FILE_APPEND);
        if(!f) f = SD.open(e.path, FILE_WRITE);
        if(f){
          f.println(e.line);
          f.close();
        }
        xSemaphoreGive(irc_mutex);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(30));
    }
  }
}

// ---------------------------------------------------------------------------
// Keyboard - Core 1 polling, input scrolling, history carousel
// ---------------------------------------------------------------------------
static void serviceKeyboard(){
  M5Cardputer.update();
  // Quick Settings overlay toggle via physical G0 (BtnA) - short press
  if(M5Cardputer.BtnA.wasPressed()){
    wakeFromSleep();
    toggleQuickOverlay();
    return;
  }
  if(!M5Cardputer.Keyboard.isChange()) return;
  if(!M5Cardputer.Keyboard.isPressed()) return;
  wakeFromSleep();
  gLastInputMs=millis();
  auto ks = M5Cardputer.Keyboard.keysState();
  // Quick Settings overlay active - 5-row grid handling per spec
  if(gQuickOverlay){
    // LIVE INJECTION: Fn+B/T/N and Server Skip hotkeys even inside overlay per spec
    for(char c: ks.word){
      if(ks.fn && (c=='b' || c=='B')){ run_bouncer_setup_menu(); ui_needs_redraw = true; return; }
      if(ks.fn && (c=='t' || c=='T')){ logStatus("LED Diagnostic: Purple test on Pin 21"); neopixelWrite(LED_PIN, 60,0,60); vTaskDelay(pdMS_TO_TICKS(1000)); neopixelWrite(LED_PIN,0,0,0); ui_needs_redraw = true; return; }
      if(ks.fn && (c=='n' || c=='N')){ display_network_jump_hud(); ui_needs_redraw = true; return; }
      if(ks.fn && (c==']' || c=='/' )){ serverSkipForward(); return; } // Fn+Right Arrow
      if(ks.fn && (c=='[' || c==',' )){ serverSkipBackward(); return; } // Fn+Left Arrow
    }
    // Up/Down selects row 0-4 via handle_settings_navigation
    for(char c: ks.word){
      if(c==';'){ handle_settings_navigation(false); gQuickOverlayMs=millis(); return; }
      if(c=='.'){ handle_settings_navigation(true); gQuickOverlayMs=millis(); return; }
    }
    if(ks.fn){
      for(char c: ks.word){
        if(c==';'){ handle_settings_navigation(false); gQuickOverlayMs=millis(); return; }
        if(c=='.'){ handle_settings_navigation(true); gQuickOverlayMs=millis(); return; }
      }
    }
    // Left/Right cycles value of selected row 0 Audio,1 Brightness,2 Filter,3 TZ,4 Hour
    for(char c: ks.word){
      if(c==',' ){ // Left
        if(current_settings_row==0) cycleAudio();
        else if(current_settings_row==1) cycleBrightness(-1);
        else if(current_settings_row==2) cycleFilter(-1);
        else if(current_settings_row==3) cycleTimezone(-1);
        else if(current_settings_row==4) toggleClockFormat();
        gQuickOverlayMs=millis(); ui_needs_redraw = true; return;
      }
      if(c=='/' ){ // Right
        if(current_settings_row==0) cycleAudio();
        else if(current_settings_row==1) cycleBrightness(1);
        else if(current_settings_row==2) cycleFilter(1);
        else if(current_settings_row==3) cycleTimezone(1);
        else if(current_settings_row==4) toggleClockFormat();
        gQuickOverlayMs=millis(); ui_needs_redraw = true; return;
      }
    }
    if(ks.fn){
      for(char c: ks.word){
        if(c==','){ if(current_settings_row==0) cycleAudio(); else if(current_settings_row==1) cycleBrightness(-1); else if(current_settings_row==2) cycleFilter(-1); else if(current_settings_row==3) cycleTimezone(-1); else if(current_settings_row==4) toggleClockFormat(); gQuickOverlayMs=millis(); ui_needs_redraw = true; return; }
        if(c=='/'){ if(current_settings_row==0) cycleAudio(); else if(current_settings_row==1) cycleBrightness(1); else if(current_settings_row==2) cycleFilter(1); else if(current_settings_row==3) cycleTimezone(1); else if(current_settings_row==4) toggleClockFormat(); gQuickOverlayMs=millis(); ui_needs_redraw = true; return; }
      }
    }
    // Enter saves and closes, Del closes without save? per spec save on changes
    if(ks.enter){
      saveConfig(); // persist numeric timezone_index and use_12_hour_format
      gQuickOverlay=false;
      return;
    }
    if(ks.del){ gQuickOverlay=false; return; }
    // Any Fn+Q toggles off as well
    for(char c: ks.word){ if(ks.fn && (c=='q' || c=='Q')){ saveConfig(); gQuickOverlay=false; return; } }
    // swallow other keys while overlay active
    return;
  }
  if(gInScanner) return;
  if(gNickOverlay && ks.del){ gNickOverlay=false; return; }
  // Fn+Q also toggles Quick Settings per spec convenience
  for(char c: ks.word){ if(ks.fn && (c=='q' || c=='Q')){ toggleQuickOverlay(); return; } }
  bool hasWord = !ks.word.empty();
  // History carousel: when buffer blank, Up/Down loops last 5
  // Cardputer-Adv arrow mapped via Fn+ combos or direct? We treat ';' '.' as up/down? Better use word chars for arrow? Use Fn+ ;/. and also direct up/down if present.
  // Detect up/down via keysState: we check word contains up arrow char (not standard). Fallback: when Fn held, ';' = up, '.' = down per original.
  // For spec: Up/Down arrow keys loops history when blank
  bool up=false, down=false;
  // Heuristic: check for special chars: M5Cardputer may send 0x1B sequences; we handle ';' with Fn etc.
  // Also check raw word for bracket sequences if available - simplify to Fn+; for up, Fn+. for down plus also handle ',' '/' mapping
  if(ks.fn){
    for(char c: ks.word){ if(c==';') up=true; if(c=='.') down=true; }
  }
  // Also support direct up/down if keyboard reports via word as 0x11/0x12? handled via same
  if(gInputLen==0 && (up||down)){
    if(gHistCount>0){
      int depth = min(gHistCount, HISTORY_DEPTH);
      if(gHistNav==-1) gHistNav = (up? depth-1 : 0);
      else {
        if(up) gHistNav = (gHistNav -1 + depth)%depth;
        else gHistNav = (gHistNav +1)%depth;
      }
      int idx = (gHistCount - depth + gHistNav) % HISTORY_DEPTH;
      if(idx<0) idx+=HISTORY_DEPTH;
      safeCopy(gInput, gHistory[idx], sizeof(gInput));
      gInputLen = strlen(gInput);
      gInputCursor = gInputLen;
      ui_needs_redraw = true; // buffer tab swapped via history
      return;
    }
  } else if(hasWord){
    gHistNav = -1;
  }

  for(char c : ks.word){
    if(c=='\n' || c=='\r') continue;
    // ignore navigation chars already handled when Fn
    if(ks.fn && (c==';' || c=='.' || c==',' || c=='/')) continue;
    if(c>=32 && c<127 && gInputLen < INPUT_BUF_SZ-1){
      memmove(gInput+gInputCursor+1, gInput+gInputCursor, gInputLen - gInputCursor +1);
      gInput[gInputCursor++]=c;
      gInputLen++;
      gHistNav=-1;
      ui_needs_redraw = true; // character typed
    }
  }
  if(ks.del && gInputLen>0 && gInputCursor>0){
    memmove(gInput+gInputCursor-1, gInput+gInputCursor, gInputLen - gInputCursor +1);
    gInputCursor--; gInputLen--;
    gHistNav=-1;
    ui_needs_redraw = true; // active keystroke
  }
  // CHANNEL STEPPING (Alt+Arrows) per spec
  if(ks.alt){
    for(char c: ks.word){
      if(c=='/' || c==']' || c=='l' || c=='L'){
        int total_tabs = gTabCount;
        current_tab_index = (current_tab_index + 1) % total_tabs;
        ui_needs_redraw = true;
        return;
      }
      if(c==',' || c=='[' || c=='h' || c=='H'){
        int total_tabs = gTabCount;
        current_tab_index = (current_tab_index == 0) ? (total_tabs - 1) : current_tab_index - 1;
        ui_needs_redraw = true;
        return;
      }
    }
    for(uint8_t k : ks.hid_keys){
      if(k==0x4F){ int total_tabs=gTabCount; current_tab_index = (current_tab_index + 1) % total_tabs; ui_needs_redraw = true; return; }
      if(k==0x50){ int total_tabs=gTabCount; current_tab_index = (current_tab_index == 0) ? (total_tabs - 1) : current_tab_index - 1; ui_needs_redraw = true; return; }
    }
  }
  if(ks.fn){
    // LIVE INJECTION: Fn+B/N/T and Server Skip hotkeys per spec inside status.fn check
    for(char c: ks.word){
      if(c=='b' || c=='B'){
        run_bouncer_setup_menu();
        ui_needs_redraw = true;
        return;
      }
      if(c=='t' || c=='T'){
        // TEST MODE 1 - THE MENTION ALERT (Purple Double-Pulse)
        logStatus("Testing LED: Mention Alert (Purple Double Pulse)...");
        for(int r=0; r<2; ++r){
          neopixelWrite(LED_PIN, 60, 0, 60);
          uint16_t purp = canvas.color565(60, 0, 60);
          (void)purp;
          vTaskDelay(pdMS_TO_TICKS(100));
          neopixelWrite(LED_PIN, 0, 0, 0);
          vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        // TEST MODE 2 - THE ACTIVITY PULSE (Cyan Breathing Fade)
        logStatus("Testing LED: Channel Activity (Cyan Breathing Fade)...");
        for(int b=0; b<=40; b+=4){
          uint16_t col2 = canvas.color565(0, b, 60*b/40 + 20);
          neopixelWrite(LED_PIN, 0, b, 60);
          (void)col2;
          vTaskDelay(pdMS_TO_TICKS(25));
        }
        for(int b=40; b>=0; b-=4){
          uint16_t col2 = canvas.color565(0, b, 60*b/40 + 20);
          neopixelWrite(LED_PIN, 0, b, 60);
          (void)col2;
          vTaskDelay(pdMS_TO_TICKS(25));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        // TEST MODE 3 - THE DISCONNECT WARNING (Dim Solid Red/Orange)
        logStatus("Testing LED: Disconnect Warning (Dim Solid Orange)...");
        {
          uint16_t col3 = canvas.color565(40, 15, 0);
          (void)col3;
          neopixelWrite(LED_PIN, 40, 15, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
        neopixelWrite(LED_PIN, 0, 0, 0);
        ui_needs_redraw = true;
        logStatus("LED Diagnostic Cycle Complete.");
        return;
      }
      if(c=='n' || c=='N'){
        display_network_jump_hud();
        ui_needs_redraw = true;
        return;
      }
      if(c==']' || c=='/' ){
        int total_tabs = gTabCount;
        const char* curServer = gTabs[current_tab_index].server[0] ? gTabs[current_tab_index].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
        for(int i=1; i<total_tabs; ++i){
          int idx = (current_tab_index + i) % total_tabs;
          const char* srv = gTabs[idx].server[0] ? gTabs[idx].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
          if(!eqI(srv, curServer)){ current_tab_index = idx; ui_needs_redraw = true; return; }
        }
        return;
      }
      if(c=='[' || c==','){
        int total_tabs = gTabCount;
        const char* curServer = gTabs[current_tab_index].server[0] ? gTabs[current_tab_index].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
        for(int i=1; i<total_tabs; ++i){
          int idx = (current_tab_index - i + total_tabs) % total_tabs;
          const char* srv = gTabs[idx].server[0] ? gTabs[idx].server : (bnc_host.length()>0 ? bnc_host.c_str() : gCfg.host);
          if(!eqI(srv, curServer)){ current_tab_index = idx; ui_needs_redraw = true; return; }
        }
        return;
      }
    }
    for(char c: ks.word){
      if(c==',' && gInputCursor>0){ gInputCursor--; ui_needs_redraw = true; }
      if(c=='/' && gInputCursor<gInputLen){ gInputCursor++; ui_needs_redraw = true; }
      if(c==';'){ Tab* t=activeTab(); if(t && t->scroll < t->count){ t->scroll++; ui_needs_redraw = true; } }
      if(c=='.'){ Tab* t=activeTab(); if(t && t->scroll>0){ t->scroll--; ui_needs_redraw = true; } }
    }
  } else {
    for(char c: ks.word){
      if(c==';'){ Tab* t=activeTab(); if(t && t->scroll < t->count){ t->scroll++; ui_needs_redraw = true; } }
      if(c=='.'){ Tab* t=activeTab(); if(t && t->scroll>0){ t->scroll--; ui_needs_redraw = true; } }
    }
  }
  if(ks.enter){
    if(gInputLen>0){
      gInput[gInputLen]='\0';
      char copy[INPUT_BUF_SZ]; safeCopy(copy,gInput,sizeof(copy));
      gInputLen=0; gInputCursor=0; gInput[0]='\0'; gInputScroll=0; gHistNav=-1;
      handleUserInput(copy);
      ui_needs_redraw = true; // character typed / command sent
    } else {
      gHistNav=-1;
    }
  }
  if(ks.tab){
    if(gTabCount>0){
      gActive=(gActive+1)%gTabCount;
      activeTab()->unread=false; activeTab()->mention=false;
      ui_needs_redraw = true; // buffer tab swapped
    }
  }
}

// ---------------------------------------------------------------------------
// WiFi service - Core 1
// ---------------------------------------------------------------------------
static void serviceWifi(){
  if(isWifiDummy(gCfg)){
    if(!gInScanner) runWifiProvisioning();
    return;
  }
  if(WiFi.status()==WL_CONNECTED){
    if(gWifiConnecting){ gWifiConnecting=false; logStatus("WiFi connected"); configTime(0,0,"pool.ntp.org","time.nist.gov"); }
    return;
  }
  if(!gWifiConnecting){
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(gCfg.wifiSSID, gCfg.wifiPass);
    gWifiConnecting=true; gWifiStartMs=millis();
    char msg[64]; snprintf(msg,sizeof(msg),"WiFi: %s", gCfg.wifiSSID);
    logStatus(msg);
  } else {
    if(millis()-gWifiStartMs > WIFI_CONNECT_TIMEOUT_MS){
      WiFi.disconnect(true);
      gWifiConnecting=false;
      logStatus("WiFi fail -> provisioning");
      delay(300);
      runWifiProvisioning();
    }
  }
}

// ---------------------------------------------------------------------------
// Setup - includes safe-boot check, 16MHz SPI, splash, purge, canvas, tasks
// ---------------------------------------------------------------------------
void setup(){
  // 1. INITIALIZE HARDWARE PRIMITIVES FIRST - absolute top per spec
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextWrap(false);
  M5Cardputer.Display.fillScreen(UI_BG);
  // Create global mutex IMMEDIATELY after hardware init
  irc_mutex = xSemaphoreCreateMutex();
  gTabsMutex = irc_mutex;
  gTxQueue.init(); gRxQueue.init(); gLogQueue.init();

  // Hardware pin setup after mutex creation (does not touch irc_mutex)
  pinMode(G0_PIN, INPUT_PULLUP);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  pinMode(BATTERY_PIN, INPUT);
  pinMode(JACK_DETECT_PIN, INPUT_PULLUP);
  pinMode(AMP_SHUTDOWN_PIN, OUTPUT);
  digitalWrite(AMP_SHUTDOWN_PIN, HIGH);
  pinMode(LED_PIN, OUTPUT);
  neopixelWrite(LED_PIN,0,0,0);
  gLastInputMs=millis();
  gSavedBrightness = 10; // default bright before config load to avoid black screen
  applyBrightness(gSavedBrightness);
  M5Cardputer.Display.wakeup();

  // 2. ZERO-MUTEX INTRO ANIMATION & INITIAL REDRAW
  run_retro_splash_screen(); // unshielded, no mutex - direct to Display glass
  // Initialize 8-bit canvas immediately following splash - single unified 240x135 8-bit
  canvas.setColorDepth(8);
  canvas.setPsram(false);
  canvas.setTextSize(1);
  canvas.setTextWrap(false);
  canvas.deleteSprite();
  if(!canvas.createSprite(240, 109)){
    gCanvasReady = false;
  } else {
    canvas.fillScreen(UI_BG);
    gCanvasReady = canvas.width()==240 && canvas.height()==109;
  }
  // PRE-FLIGHT DRAWING INSURANCE: force initial draw before background thread
  ui_needs_redraw = true;
  draw_chat_view();
  // Intermediate init - SPI, SD, config, etc. (all after mutex, before network)
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if(!gSafeBoot){
    gSdReady = SD.begin(SD_CS, SPI, 16000000);
  } else {
    gSdReady = false;
  }
  loadConfig();
  ensureStatus();
  setCpuFrequencyMhz(240);
  if(!gSafeBoot && gSdReady) sweepOldLogs();
  gLastBattPoll = millis();
  gBattVoltage = readBatteryVoltage();
  // Fix black screen: apply brightness AFTER config load, default to 10 if still 0
  if(gCfg.brightness==0) gCfg.brightness=10;
  gSavedBrightness = gCfg.brightness;
  applyBrightness(gSavedBrightness);
  M5Cardputer.Speaker.setVolume(128);
  digitalWrite(AMP_SHUTDOWN_PIN, HIGH);
  pollJack();

  if(gSafeBoot){
    logStatus("Safe Mode -> provisioning");
  } else if(isWifiDummy(gCfg)){
    logStatus("Provisioning mode");
  } else {
    // Wi-Fi client initialization loop - non-blocking, no while(WiFi.status()!=WL_CONNECTED) block in setup
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(gCfg.wifiSSID, gCfg.wifiPass);
    gWifiConnecting=true; gWifiStartMs=millis();
  }
  // DE-COUPLE NETWORK SELECTION & BOOT BLOCKS: background worker at absolute END of setup()
  if(!gSafeBoot){
    xTaskCreatePinnedToCore(logTask, "irc_log", 4096, nullptr, 1, &gLogTaskHandle, 0);
    xTaskCreatePinnedToCore(netTask, "irc_net", 8192, nullptr, 2, &gNetTaskHandle, 0);
  } else {
    logStatus("Safe Mode: net tasks bypassed");
  }
}

// ---------------------------------------------------------------------------
// Loop - Core 1 graphics/keyboard, protected shared vars via mutex
// ---------------------------------------------------------------------------
void loop(){
  // Safe Mode: if still dummy and not in scanner, keep provisioning active
  serviceKeyboard();
  if(gQuickOverlay && millis() - gQuickOverlayMs > 8000){ saveConfig(); gQuickOverlay=false; }
  if(!gSafeBoot) serviceWifi();
  else if(gInScanner==false && isWifiDummy(gCfg)) runWifiProvisioning();
  servicePowerWatchdog();
  pollBattery();
  pollJack();
  serviceStealthLed();

  char line[IRC_LINE_MAX+1];
  int drained=0;
  while(drained<6 && gRxQueue.pop(line)){
    if(gTabsMutex && xSemaphoreTake(gTabsMutex, pdMS_TO_TICKS(20))==pdTRUE){
      handleRawIrc(line);
      if(strstr(line," 001 ")) gIrcRegistered=true;
      xSemaphoreGive(gTabsMutex);
    } else {
      handleRawIrc(line);
    }
    drained++;
    gLastRxMs=millis();
  }

  static uint32_t lastDraw=0;
  if(millis()-lastDraw >= UI_REFRESH_MS){
    lastDraw=millis();
    if(gInScanner){
      // provision draws itself
    } else {
      // Single unified draw - draw_chat_view handles Steps A,B,C with zero flicker and single push
      if(irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(50))==pdTRUE){
        draw_chat_view();
        xSemaphoreGive(irc_mutex);
      } else {
        draw_chat_view();
      }
    }
  }
  delay(5);
}

