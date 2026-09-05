// Production-grade monolithic IRC Client for Cardputer-Adv - ground-up rewrite
// Target: M5Stack Cardputer-Adv ST7789 240x135, TCA8418 56-key matrix, M5StampS3, NO PSRAM 512KB SRAM
// All comments in English.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <cstring>
#include <cctype>
#include <ctime>
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <driver/adc.h>

#ifndef KEY_RIGHT
#define KEY_RIGHT 0x4F
#endif
#ifndef KEY_LEFT
#define KEY_LEFT 0x50
#endif
#ifndef KEY_DOWN
#define KEY_DOWN 0x51
#endif
#ifndef KEY_UP
#define KEY_UP 0x52
#endif

// ---------------------------------------------------------------------------
// Hardware pins and display geometry
// ---------------------------------------------------------------------------
static constexpr int SD_SCK = 40;
static constexpr int SD_MISO = 39;
static constexpr int SD_MOSI = 14;
static constexpr int SD_CS = 12;
static constexpr int BATTERY_PIN = 10;
static constexpr int AMP_ENABLE_PIN = 4;
static constexpr int LED_PIN = 21;
static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;
static constexpr int TOP_H = 12;
static constexpr int INPUT_H = 14;
static constexpr int CHAT_H = 109;
static constexpr int CHAT_Y = TOP_H;
static constexpr int INPUT_Y = SCREEN_H - INPUT_H;
static constexpr int CHAR_W = 6;
static constexpr int ROW_H = 10;
static constexpr int CHAT_ROWS = CHAT_H / ROW_H;
static constexpr int INPUT_VISIBLE_COLS = 38;
static constexpr int MAX_TABS = 8;
static constexpr int MAX_LINES_PER_TAB = 20;
static constexpr int MAX_LINE_LEN = 155;
static constexpr int MAX_NICKS = 32;
static constexpr int NICK_LEN = 24;
static constexpr int TAB_NAME_LEN = 28;
static constexpr int TOPIC_LEN = 96;
static constexpr int IRC_LINE_MAX = 512;
static constexpr int RX_ACCUM_SZ = 2048;
static constexpr int INPUT_BUF_SZ = 256;
static constexpr int HISTORY_DEPTH = 5;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------
enum TabType : uint8_t { TAB_STATUS = 0, TAB_CHANNEL = 1, TAB_QUERY = 2 };
enum LogLevel : uint8_t { LOG_ALL = 0, LOG_DMS_ONLY = 1, LOG_NONE = 2 };

struct TimeZoneProfile { const char* label; int offset; };
static const TimeZoneProfile TZ_PROFILES[] = {
  {"UTC",0},{"EST",-5},{"EDT",-4},{"CST",-6},{"CDT",-5},{"MST",-7},{"MDT",-6},{"PST",-8},{"PDT",-7},
  {"CET",1},{"CEST",2},{"EET",2},{"JST",9},{"AEST",10},{"NZST",12},{"AKST",-9},{"HST",-10},{"BST",1},{"WET",0},{"IST",5}
};
static constexpr int TZ_COUNT = sizeof(TZ_PROFILES)/sizeof(TZ_PROFILES[0]);

struct ChatLine {
  char stamp[6];
  char text[MAX_LINE_LEN+1];
  uint8_t flags;
  bool is_highlight;
};

struct Tab {
  char name[TAB_NAME_LEN+1];
  TabType type;
  ChatLine lines[MAX_LINES_PER_TAB];
  uint8_t head;
  uint8_t count;
  int8_t scroll;
  bool unread;
  bool mention;
  char topic[TOPIC_LEN+1];
  char nicks[MAX_NICKS][NICK_LEN+1];
  uint8_t nickCount;
  char server[32];
};

struct Config {
  char wifiSSID[64] = {0};
  char wifiPass[64] = {0};
  char host[96] = "irc.libera.chat";
  uint16_t port = 6697;
  bool useTLS = true;
  char nick[32] = "CardADV";
  char user[32] = "cardputer";
  char realname[64] = "Cardputer IRC";
  char pass[64] = {0};
  char autojoin[128] = {0};
  LogLevel logLevel = LOG_DMS_ONLY;
  char logRoot[32] = "/irc/logs";
  uint8_t brightness = 10;
  char bncUser[64] = {0};
  char bncPass[64] = {0};
  char bncHost[96] = {0};
  uint16_t bncPort = 0;
  bool bncEnabled = false;
  bool saslEnabled = false;
  char saslUser[32] = {0};
  char saslPass[32] = {0};
  int current_audio = 0;
  int timezone_index = 0;
  bool use_12_hour_format = false;
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
volatile bool safe_mode_active = false;
static Config gCfg;
char irc_nick[32] = "CardADV";
static Tab gTabs[MAX_TABS];
static int gTabCount = 0;
static int gActive = 0;
#define current_tab_index gActive
static bool gSdReady = false;
static SemaphoreHandle_t irc_mutex = nullptr;
static lgfx::LGFX_Sprite canvas(&M5Cardputer.Display);
static bool gCanvasReady = false;
static bool ui_needs_redraw = true;
static WiFiClientSecure gSecure;
static WiFiClient gPlain;
static char gRxAccum[RX_ACCUM_SZ];
static int gRxLen = 0;
static char gInlineServerHHMM[6] = {0};
static bool gIrcConnected = false;
static bool gIrcRegistered = false;
static uint32_t gLastRxMs = 0;
static char gInput[INPUT_BUF_SZ] = {0};
static int gInputLen = 0;
static int gInputCursor = 0;
static int gInputScroll = 0;
static char gHistory[HISTORY_DEPTH][INPUT_BUF_SZ];
static int gHistCount = 0;
static int gHistNav = -1;
static int gTimezoneIndex = 0;
static bool gUse12Hour = false;
static uint32_t gGreenBurstMs = 0;
static bool gGreenBurstActive = false;
static uint32_t gYellowBlipMs = 0;
static uint32_t gRedFlashMs = 0;
static uint32_t gWhiteSparkMs = 0;
static uint32_t gMagentaTintMs = 0;
static uint32_t gPurpleFlashMs = 0;
static uint8_t gPurpleFlashPhase = 0;
static float gBattVoltage = 0.0f;
static uint32_t gLastBattPoll = 0;
static TaskHandle_t gNetTaskHandle = nullptr;
static TaskHandle_t gUiTaskHandle = nullptr;
unsigned long last_input_time = 0;
int screen_brightness = 10;
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
    // QUEUE OVERFLOW DROP VALVE: if queue fully maxed out, drop oldest unread line packet from front
    if(cnt >= 16){
      head=(head+1)%16; cnt--;
    }
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
  int size(){
    int v=0;
    if(mtx && xSemaphoreTake(mtx,pdMS_TO_TICKS(10))==pdTRUE){ v=cnt; xSemaphoreGive(mtx); }
    return v;
  }
};
static LogQueue gLogQueue;
static TaskHandle_t gLogTaskHandle = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void safeCopy(char* dst, const char* src, size_t n){ if(!dst||n==0) return; if(!src){dst[0]=0;return;} strncpy(dst,src,n-1); dst[n-1]=0; }
static void trim(char* s){ if(!s) return; char* p=s; while(*p && isspace((unsigned char)*p)) p++; if(p!=s) memmove(s,p,strlen(p)+1); size_t l=strlen(s); while(l>0 && isspace((unsigned char)s[l-1])) s[--l]=0; }
static bool eqI(const char* a,const char* b){ if(!a||!b) return false; while(*a&&*b){ if(tolower((unsigned char)*a)!=tolower((unsigned char)*b)) return false; a++;b++; } return *a==*b; }
static bool startsI(const char* s,const char* pref){ if(!s||!pref) return false; while(*pref){ if(tolower((unsigned char)*s)!=tolower((unsigned char)*pref)) return false; s++;pref++; } return true; }
static bool isChannelName(const char* s){ return s && (s[0]=='#' || s[0]=='&' || s[0]=='+' || s[0]=='!'); }
static void toLower(char* s){ for(;*s;++s) *s=tolower((unsigned char)*s); }
static bool strToBoolC(const char* s){ if(!s) return false; char t[16]; safeCopy(t,s,sizeof(t)); toLower(t); trim(t); return !strcmp(t,"1")||!strcmp(t,"true")||!strcmp(t,"yes")||!strcmp(t,"on"); }
static LogLevel parseLogLevel(const char* v){ if(!v) return LOG_NONE; char t[16]; safeCopy(t,v,sizeof(t)); trim(t); toLower(t); if(!strcmp(t,"0")) return LOG_ALL; if(!strcmp(t,"1")) return LOG_DMS_ONLY; if(!strcmp(t,"2")) return LOG_NONE; if(strToBoolC(t)) return LOG_ALL; return LOG_NONE; }
static void sanitizeGlyphs(char* s){ if(!s) return; for(char* p=s;*p;++p){ unsigned char c=(unsigned char)*p; if(c<32||c>126) *p='.'; } }
static uint16_t nickHashColor(const char* nick){ static const uint16_t cols[6]={0x07FF,0x07E0,0xFFE0,0xF81F,0xFD20,0xFFFF}; if(!nick||!*nick) return 0x07FF; uint32_t sum=0; for(const char* p=nick;*p;++p) sum+=(unsigned char)*p; return cols[sum%6]; }

static void currentStamp(char* hhmm,size_t n,char* hhmmss,size_t n2){
  time_t now=time(nullptr); struct tm tv; localtime_r(&now,&tv);
  if(hhmm) snprintf(hhmm,n,"%02d:%02d",tv.tm_hour,tv.tm_min);
  if(hhmmss) snprintf(hhmmss,n2,"%02d:%02d:%02d",tv.tm_hour,tv.tm_min,tv.tm_sec);
}
static bool parseServerTimeHHMM(const char* tags,char* out6){
  if(!tags||!out6) return false; out6[0]=0; const char* p=tags;
  while(p && *p){ const char* semi=strchr(p,';'); size_t tokLen=semi?(size_t)(semi-p):strlen(p); if(tokLen>=5 && !strncmp(p,"time=",5)){ const char* val=p+5; const char* t=(const char*)memchr(val,'T',tokLen-5); if(t && t+5 < val+tokLen){ if(isdigit((unsigned char)t[1])&&isdigit((unsigned char)t[2])&&t[3]==':'&&isdigit((unsigned char)t[4])&&isdigit((unsigned char)t[5])){ out6[0]=t[1];out6[1]=t[2];out6[2]=':';out6[3]=t[4];out6[4]=t[5];out6[5]=0; return true; } } } if(!semi) break; p=semi+1; }
  return false;
}
static void localizeTimeHHMM(const char* utcHHMM,char* out6){
  if(!utcHHMM||!out6){ if(out6) out6[0]=0; return; }
  if(strlen(utcHHMM)<5||utcHHMM[2]!=':' || !isdigit((unsigned char)utcHHMM[0])){ safeCopy(out6,utcHHMM,6); return; }
  int hh=(utcHHMM[0]-'0')*10+(utcHHMM[1]-'0'); int mm=(utcHHMM[3]-'0')*10+(utcHHMM[4]-'0');
  int off=0; if(gTimezoneIndex>=0 && gTimezoneIndex<TZ_COUNT) off=TZ_PROFILES[gTimezoneIndex].offset;
  int local=hh+off; while(local<0) local+=24; while(local>=24) local-=24;
  if(gUse12Hour){ int disp=local%12; if(disp==0) disp=12; snprintf(out6,6,"%02d:%02d",disp,mm);} else snprintf(out6,6,"%02d:%02d",local,mm);
}
static float readBatteryVoltage(){
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  int raw=analogRead(BATTERY_PIN);
  return ((float)raw/4095.0f)*3.3f*2.0f;
}
float get_calibrated_battery_percentage() {
    // Read raw hardware state and convert to true cell voltage parameters
    float raw_volt = M5Cardputer.Power.getBatteryVoltage(); 
    
    // Standard lithium cell bounds for the Cardputer-Adv 1750mAh cell layout:
    // 4.2V is maximum full charge, 3.3V is safe empty boundary threshold limit
    if (raw_volt > 4.2f) raw_volt = 4.2f;
    if (raw_volt < 3.3f) raw_volt = 3.3f;
    
    float percentage = ((raw_volt - 3.3f) / (4.2f - 3.3f)) * 100.0f;
    
    // Smooth out read spikes using a non-blocking Exponential Moving Average (EMA) loop filter
    static float smoothed_pct = percentage;
    smoothed_pct = (smoothed_pct * 0.95f) + (percentage * 0.05f);
    
    return smoothed_pct;
}
static void pollBattery(){
  if(millis()-gLastBattPoll < 5000) return;
  gLastBattPoll=millis();
  gBattVoltage=readBatteryVoltage();
}
static inline void setDeepCrimson(){ neopixelWrite(LED_PIN,20,0,0); }

// 7-day log purging utility - parse storage directory and delete >7 days
static bool isAllDigits(const char* s){ if(!s||!*s) return false; for(;*s;++s) if(!isdigit((unsigned char)*s)) return false; return true; }
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
    yield(); vTaskDelay(pdMS_TO_TICKS(1));
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
            while(f2){ char p2[128]; snprintf(p2,sizeof(p2),"%s/%s",full,f2.name()); f2.close(); SD.remove(p2); f2=dated.openNextFile(); yield(); vTaskDelay(pdMS_TO_TICKS(1)); }
            dated.close();
          }
          SD.rmdir(full);
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
static void purge_old_logs(){
  if(!gSdReady) return;
  if(!irc_mutex || xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(50))!=pdTRUE) return;
  const char* roots[] = {"/irc/logs","/irc","/IRC", nullptr};
  for(int i=0;roots[i];++i){
    if(SD.exists(roots[i])){
      File f=SD.open(roots[i]);
      if(f){ f.close(); sweepRecursive(roots[i],0); }
    }
  }
  xSemaphoreGive(irc_mutex);
}
static void trigger_config_backup(){
  if(!gSdReady) return;
  if(!irc_mutex || xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(50))!=pdTRUE) return;
  // scrub any raw web symbols or '://' artifacts by not writing them - pure key=value
  if(SD.exists("/irc/config.bak")) SD.remove("/irc/config.bak");
  if(SD.exists("/irc/config.txt")){
    File src=SD.open("/irc/config.txt", FILE_READ);
    File dst=SD.open("/irc/config.bak", FILE_WRITE);
    if(src && dst){
      uint8_t buf[128];
      while(src.available()){
        yield(); vTaskDelay(pdMS_TO_TICKS(1));
        size_t n=src.read(buf,sizeof(buf));
        if(n>0) dst.write(buf,n);
      }
    }
    if(src) src.close();
    if(dst) dst.close();
  }
  xSemaphoreGive(irc_mutex);
  // overwrite /irc/config.txt using pure, un-spaced key-value pairs and scrub ://
  if(!irc_mutex || xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(50))!=pdTRUE) return;
  if(!SD.exists("/irc")) SD.mkdir("/irc");
  if(SD.exists("/irc/config.txt")) SD.remove("/irc/config.txt");
  File f=SD.open("/irc/config.txt", FILE_WRITE);
  if(f){
    auto scrub = [](const char* s)->String{ String t=s; t.replace("://",""); t.replace("http",""); t.replace("https",""); return t; };
    String hostScrub = scrub(gCfg.host);
    String bncHostScrub = scrub(gCfg.bncHost);
    f.printf("wifi_ssid=%s\n", gCfg.wifiSSID);
    f.printf("wifi_pass=%s\n", gCfg.wifiPass);
    f.printf("irc_host=%s\n", hostScrub.c_str());
    f.printf("irc_port=%u\n", gCfg.port);
    f.printf("irc_use_tls=%s\n", gCfg.useTLS?"true":"false");
    f.printf("irc_nick=%s\n", gCfg.nick);
    f.printf("irc_user=%s\n", gCfg.user);
    f.printf("irc_realname=%s\n", gCfg.realname);
    f.printf("irc_pass=%s\n", gCfg.pass);
    f.printf("autojoin=%s\n", gCfg.autojoin);
    f.printf("channel_log_enabled=%d\n", (int)gCfg.logLevel);
    f.printf("log_root=%s\n", gCfg.logRoot);
    f.printf("screen_brightness=%u\n", gCfg.brightness);
    f.printf("current_audio=%d\n", gCfg.current_audio);
    f.printf("bnc_host=%s\n", bncHostScrub.c_str());
    f.printf("bnc_port=%u\n", gCfg.bncPort);
    f.printf("bnc_user=%s\n", gCfg.bncUser);
    f.printf("bnc_pass=%s\n", gCfg.bncPass);
    f.printf("bnc_enabled=%s\n", gCfg.bncEnabled?"true":"false");
    f.printf("sasl_enabled=%s\n", gCfg.saslEnabled?"true":"false");
    f.printf("sasl_user=%s\n", gCfg.saslUser);
    f.printf("sasl_pass=%s\n", gCfg.saslPass);
    f.printf("timezone_index=%d\n", gTimezoneIndex);
    f.printf("use_12_hour_format=%d\n", gUse12Hour?1:0);
    f.close();
  }
  xSemaphoreGive(irc_mutex);
}
void sync_new_nick_to_sd(const char* new_nick) {
    // Only execute if our safe mode flag is inactive to protect file tables
    if (safe_mode_active) return;
    
    // Explicitly update our active application buffer right before writing
    strncpy(irc_nick, new_nick, sizeof(irc_nick) - 1);
    irc_nick[sizeof(irc_nick)-1]='\0';
    safeCopy(gCfg.nick, new_nick, sizeof(gCfg.nick));
    
    // Open the config file for pure truncation overwrite mode
    File file = SD.open("/irc/config.txt", FILE_WRITE);
    if (!file) return;
    
    // Export our exact configuration schema using clean un-spaced parameters
    file.printf("wifi_ssid=%s\n", gCfg.wifiSSID);
    file.printf("wifi_pass=%s\n", gCfg.wifiPass);
    file.printf("irc_nick=%s\n", irc_nick); // <-- Lock in the brand new name
    file.printf("channel_log_enabled=%d\n", (int)gCfg.logLevel);
    file.printf("current_audio=%d\n", gCfg.current_audio);
    file.printf("screen_brightness=%d\n", screen_brightness);
    file.printf("current_tz_idx=%d\n", gTimezoneIndex);
    file.printf("use_12_hour_format=%d\n", gUse12Hour?1:0);
    file.printf("bnc_host=%s\n", gCfg.bncHost);
    file.printf("bnc_port=%d\n", gCfg.bncPort);
    file.printf("bnc_user=%s\n", gCfg.bncUser);
    file.printf("bnc_pass=%s\n", gCfg.bncPass);
    
    file.close();
    Serial.println("[STORAGE-SYNC] New nick permanently synchronized to micro-SD config.");
}
static void logTask(void* pv){
  (void)pv;
  for(;;){
    LogEntry e;
    if(gLogQueue.pop(e)){
      if(irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(50))==pdTRUE){
        if(!SD.exists("/irc")) SD.mkdir("/irc");
        char dirPath[96]; snprintf(dirPath,sizeof(dirPath),"%s", e.path);
        char* slash=strrchr(dirPath,'/'); if(slash){ *slash='\0'; if(!SD.exists(dirPath)) SD.mkdir(dirPath); }
        File f=SD.open(e.path, FILE_APPEND);
        if(f){ f.print(e.line); f.print("\n"); f.close(); gGreenBurstMs=millis(); gGreenBurstActive=true; }
        xSemaphoreGive(irc_mutex);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

// 9-mode LED telemetry - non-blocking millis()
static void updateLedTelemetry(){
  uint32_t now=millis();
  if(!gIrcConnected || WiFi.status()!=WL_CONNECTED){ neopixelWrite(LED_PIN,65,20,0); return; }
  if(gPurpleFlashPhase>0){
    uint32_t e=now-gPurpleFlashMs;
    if(e<100 || (e>=200 && e<300)) { neopixelWrite(LED_PIN,25,0,55); return; }
    if(e<200 || (e>=300 && e<400)) { neopixelWrite(LED_PIN,0,0,0); return; }
    if(e>=400) gPurpleFlashPhase=0;
    else { neopixelWrite(LED_PIN,0,0,0); return; }
  }
  int bat_pct=(int)((gBattVoltage-3.2f)/(1.0f)*100); if(bat_pct<0) bat_pct=0; if(bat_pct>100) bat_pct=100;
  if(bat_pct<15){ neopixelWrite(LED_PIN,60,0,0); return; }
  if(now-gMagentaTintMs < 100){ neopixelWrite(LED_PIN,55,0,15); return; }
  if(WiFi.status()==WL_CONNECTED && WiFi.RSSI() < -80){ gMagentaTintMs=now; neopixelWrite(LED_PIN,55,0,15); return; }
  if(gGreenBurstActive && now-gGreenBurstMs < 200){ neopixelWrite(LED_PIN,0,30,0); return; } else gGreenBurstActive=false;
  if(now-gRedFlashMs < 80){ setDeepCrimson(); return; }
  if(now-gYellowBlipMs < 40){ neopixelWrite(LED_PIN,35,20,0); return; }
  if(now-gWhiteSparkMs < 15){ neopixelWrite(LED_PIN,45,45,45); return; }
  if(gIrcConnected){
    float phase = sinf((float)now / 350.0f);
    float a = (phase+1.0f)*0.5f;
    uint8_t r=0;
    uint8_t g=(uint8_t)(20 + a*25);
    uint8_t b=(uint8_t)(60 - a*15);
    neopixelWrite(LED_PIN,r,g,b);
    return;
  }
  neopixelWrite(LED_PIN,0,0,0);
}

// Tab helpers
static Tab* activeTab(){ if(gTabCount==0) return nullptr; if(gActive<0) gActive=0; if(gActive>=gTabCount) gActive=gTabCount-1; return &gTabs[gActive]; }
static Tab* findTab(const char* name){ if(!name) return nullptr; for(int i=0;i<gTabCount;++i) if(eqI(gTabs[i].name,name)) return &gTabs[i]; return nullptr; }
static Tab* getOrCreateTab(const char* name, TabType t){
  if(!name||!*name) return activeTab();
  Tab* f=findTab(name); if(f) return f;
  if(gTabCount>=MAX_TABS){
    for(int i=1;i<gTabCount;++i) if(!eqI(gTabs[i].name,"~mentions")) return &gTabs[i];
    return nullptr;
  }
  Tab* nb=&gTabs[gTabCount++]; memset(nb,0,sizeof(Tab)); safeCopy(nb->name,name,sizeof(nb->name)); nb->type=t;
  const char* srv=gCfg.bncHost[0]?gCfg.bncHost:gCfg.host; safeCopy(nb->server,srv,sizeof(nb->server));
  return nb;
}
static void ensureStatus(){
  if(gTabCount==0){
    memset(gTabs,0,sizeof(gTabs));
    safeCopy(gTabs[0].name,"~mentions",sizeof(gTabs[0].name));
    safeCopy(gTabs[0].server,"ClientCore",sizeof(gTabs[0].server));
    gTabs[0].type=TAB_STATUS;
    gTabCount=1; gActive=0;
    if(gTabCount<MAX_TABS){
      Tab* s=&gTabs[gTabCount++]; memset(s,0,sizeof(Tab)); safeCopy(s->name,"status",sizeof(s->name)); s->type=TAB_STATUS; safeCopy(s->server,gCfg.bncHost[0]?gCfg.bncHost:gCfg.host,sizeof(s->server));
    }
  } else {
    if(!eqI(gTabs[0].name,"~mentions")){ safeCopy(gTabs[0].name,"~mentions",sizeof(gTabs[0].name)); safeCopy(gTabs[0].server,"ClientCore",sizeof(gTabs[0].server)); gTabs[0].type=TAB_STATUS; }
  }
}
static const ChatLine* ringAt(const Tab* tab,int idx){ if(!tab||idx<0||idx>=tab->count) return nullptr; int start=(tab->head - tab->count + MAX_LINES_PER_TAB)%MAX_LINES_PER_TAB; int phys=(start+idx)%MAX_LINES_PER_TAB; return &tab->lines[phys]; }
static void ringPush(Tab* tab,const char* txt,uint8_t flags,const char* serverHHMM=nullptr){
  if(!tab||!txt) return;
  char sanitized[MAX_LINE_LEN+1]; safeCopy(sanitized,txt,sizeof(sanitized)); sanitizeGlyphs(sanitized);
  ChatLine* slot=&tab->lines[tab->head];
  char hhmm[6]; if(serverHHMM&&serverHHMM[0]) safeCopy(hhmm,serverHHMM,sizeof(hhmm)); else currentStamp(hhmm,sizeof(hhmm),nullptr,0);
  safeCopy(slot->stamp,hhmm,sizeof(slot->stamp)); safeCopy(slot->text,sanitized,sizeof(slot->text)); slot->flags=flags; slot->is_highlight=(flags&0x01)!=0;
  tab->head=(tab->head+1)%MAX_LINES_PER_TAB; if(tab->count<MAX_LINES_PER_TAB) tab->count++; if(tab->scroll>0) tab->scroll++; if(tab->scroll > (int)tab->count - CHAT_ROWS) tab->scroll=tab->count-CHAT_ROWS; if(tab->scroll<0) tab->scroll=0;
  bool isActive=(tab==activeTab()); if(!isActive){ tab->unread=true; if(flags&0x01) tab->mention=true; }
  ui_needs_redraw=true;
}
static bool shouldLog(const Tab* tab,bool isSystemError){
  if(gCfg.logLevel==LOG_NONE) return false;
  if(gCfg.logLevel==LOG_ALL) return true;
  if(isSystemError) return true;
  if(gCfg.logLevel==LOG_DMS_ONLY){ if(tab && tab->name[0]=='#') return false; return true; }
  return false;
}
static void appendLog(Tab* tab,const char* raw,const char* serverHHMM=nullptr){
  if(!tab||!raw) return;
  uint8_t fl=0;
  char lowTxt[160]; safeCopy(lowTxt,raw,sizeof(lowTxt)); toLower(lowTxt);
  char lowNick[32]; safeCopy(lowNick,irc_nick,sizeof(lowNick)); toLower(lowNick);
  if(lowNick[0] && strstr(lowTxt,lowNick)) fl|=0x01;
  ringPush(tab,raw,fl,serverHHMM);
  if(fl&0x01){ gPurpleFlashMs=millis(); gPurpleFlashPhase=2; }
  if(fl&0x01){
    if(irc_mutex && xSemaphoreTake(irc_mutex,pdMS_TO_TICKS(50))==pdTRUE){
      Tab* mentionTab=findTab("~mentions"); if(!mentionTab) mentionTab=&gTabs[0];
      if(mentionTab && eqI(mentionTab->name,"~mentions")){
        char prefixed[MAX_LINE_LEN+1];
        const char* origin=tab->server[0]?tab->server:(gCfg.bncHost[0]?gCfg.bncHost:gCfg.host);
        char cleanOrigin[32]; safeCopy(cleanOrigin,origin,sizeof(cleanOrigin)); char* colonPos=strchr(cleanOrigin,':'); if(colonPos) *colonPos=0;
        snprintf(prefixed,sizeof(prefixed),"[%s] %s",cleanOrigin,raw);
        ChatLine* mslot=&mentionTab->lines[mentionTab->head];
        char hhmm2[6]; if(serverHHMM&&serverHHMM[0]) safeCopy(hhmm2,serverHHMM,sizeof(hhmm2)); else currentStamp(hhmm2,sizeof(hhmm2),nullptr,0);
        safeCopy(mslot->stamp,hhmm2,sizeof(mslot->stamp));
        char sanitized2[MAX_LINE_LEN+1]; safeCopy(sanitized2,prefixed,sizeof(sanitized2)); sanitizeGlyphs(sanitized2);
        safeCopy(mslot->text,sanitized2,sizeof(mslot->text)); mslot->flags=fl; mslot->is_highlight=true;
        mentionTab->head=(mentionTab->head+1)%MAX_LINES_PER_TAB; if(mentionTab->count<MAX_LINES_PER_TAB) mentionTab->count++; if(mentionTab->scroll>0) mentionTab->scroll++; mentionTab->unread=true; mentionTab->mention=true; ui_needs_redraw=true;
      }
      xSemaphoreGive(irc_mutex);
    }
  }
  if(!gSdReady) return;
  if(!shouldLog(tab,false)) return;
  time_t now=time(nullptr); struct tm tmv; localtime_r(&now,&tmv);
  char date[9]; snprintf(date,sizeof(date),"%04d%02d%02d",tmv.tm_year+1900,tmv.tm_mon+1,tmv.tm_mday);
  char safeTab[32]; safeCopy(safeTab,tab->name,sizeof(safeTab)); for(char* p=safeTab;*p;++p) if(!isalnum((unsigned char)*p) && *p!='#' && *p!='_' && *p!='-') *p='_';
  if(tab->type==TAB_QUERY){ char tmp[32]; snprintf(tmp,sizeof(tmp),"query_%s",safeTab); safeCopy(safeTab,tmp,sizeof(safeTab)); }
  if(eqI(safeTab,"status")) safeCopy(safeTab,"status",sizeof(safeTab));
  char path[96]; snprintf(path,sizeof(path),"%s/%s/%s.log",gCfg.logRoot,date,safeTab);
  char hhmmss[9]; currentStamp(nullptr,0,hhmmss,sizeof(hhmmss));
  LogEntry e;
  safeCopy(e.path, path, sizeof(e.path));
  snprintf(e.line, sizeof(e.line), "%s %s", hhmmss, raw);
  sanitizeGlyphs(e.line);
  // QUEUE OVERFLOW DROP VALVE is handled inside gLogQueue.push (drops oldest if maxed)
  gLogQueue.push(e);
}
static void logStatus(const char* s){
  Tab* t=findTab("status"); if(!t) t=getOrCreateTab("status",TAB_STATUS);
  char buf[MAX_LINE_LEN+1]; snprintf(buf,sizeof(buf),"*** %s",s?s:"");
  if(!shouldLog(t,true)){ ringPush(t,buf,0); return; }
  appendLog(t,buf);
}
void add_message_to_buffer(Tab* tab, const char* text){
  if(!tab||!text) return;
  char lowTxt[160]; safeCopy(lowTxt, text, sizeof(lowTxt)); toLower(lowTxt);
  char lowLiveNick[32]; safeCopy(lowLiveNick, irc_nick, sizeof(lowLiveNick)); toLower(lowLiveNick);
  bool isLiveMention = lowLiveNick[0] && strstr(lowTxt, lowLiveNick) != nullptr;
  uint8_t fl = isLiveMention ? 0x01 : 0x00;
  // Use live irc_nick for highlighting, not static gCfg.nick
  ringPush(tab, text, fl);
  if(isLiveMention){ gPurpleFlashMs=millis(); gPurpleFlashPhase=2; }
  if(isLiveMention){
    if(irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(50))==pdTRUE){
      Tab* mentionTab=findTab("~mentions"); if(!mentionTab) mentionTab=&gTabs[0];
      if(mentionTab && eqI(mentionTab->name,"~mentions")){
        char prefixed[MAX_LINE_LEN+1];
        const char* origin=tab->server[0]?tab->server:(gCfg.bncHost[0]?gCfg.bncHost:gCfg.host);
        char cleanOrigin[32]; safeCopy(cleanOrigin,origin,sizeof(cleanOrigin)); char* colonPos=strchr(cleanOrigin,':'); if(colonPos) *colonPos=0;
        snprintf(prefixed,sizeof(prefixed),"[%s] %s",cleanOrigin,text);
        ChatLine* mslot=&mentionTab->lines[mentionTab->head];
        char hhmm2[6]; currentStamp(hhmm2,sizeof(hhmm2),nullptr,0);
        safeCopy(mslot->stamp,hhmm2,sizeof(mslot->stamp));
        char sanitized2[MAX_LINE_LEN+1]; safeCopy(sanitized2,prefixed,sizeof(sanitized2)); sanitizeGlyphs(sanitized2);
        safeCopy(mslot->text,sanitized2,sizeof(mslot->text)); mslot->flags=fl; mslot->is_highlight=true;
        mentionTab->head=(mentionTab->head+1)%MAX_LINES_PER_TAB; if(mentionTab->count<MAX_LINES_PER_TAB) mentionTab->count++; if(mentionTab->scroll>0) mentionTab->scroll++; mentionTab->unread=true; mentionTab->mention=true; ui_needs_redraw=true;
      }
      xSemaphoreGive(irc_mutex);
    }
  }
}

// Forward declarations
static void handleUserInput(const char* in);
static void run_bouncer_setup_menu();

// ---------------------------------------------------------------------------
// IRC parsing
// ---------------------------------------------------------------------------
static bool nickFromPrefix(const char* prefix,char* out,size_t n){
  if(!prefix||!out) return false; const char* bang=strchr(prefix,'!'); size_t len=bang?(size_t)(bang-prefix):strlen(prefix); if(len>=n) len=n-1; memcpy(out,prefix,len); out[len]=0; return true;
}
static void sortNicks(Tab* t){ if(!t||t->nickCount<=1) return; for(int i=0;i<t->nickCount-1;++i) for(int j=i+1;j<t->nickCount;++j) if(strcasecmp(t->nicks[i],t->nicks[j])>0){ char tmp[NICK_LEN+1]; safeCopy(tmp,t->nicks[i],sizeof(tmp)); safeCopy(t->nicks[i],t->nicks[j],sizeof(t->nicks[i])); safeCopy(t->nicks[j],tmp,sizeof(tmp)); } }
static void addNick(Tab* t,const char* nick){ if(!t||!nick||!*nick) return; for(int i=0;i<t->nickCount;++i) if(eqI(t->nicks[i],nick)) return; if(t->nickCount>=MAX_NICKS) return; safeCopy(t->nicks[t->nickCount++],nick,NICK_LEN+1); sortNicks(t); }
static void delNick(Tab* t,const char* nick){ if(!t||!nick) return; for(int i=0;i<t->nickCount;++i) if(eqI(t->nicks[i],nick)){ memmove(&t->nicks[i],&t->nicks[i+1],(t->nickCount-i-1)*(NICK_LEN+1)); t->nickCount--; break; } }
static void handlePrivmsgC(const char* prefix,const char* target,const char* text,bool isNotice,bool isAction,const char* serverHHMM=nullptr){
  char nick[32]; nickFromPrefix(prefix?prefix:"server",nick,sizeof(nick));
  char disp[MAX_LINE_LEN+1]; if(isAction) snprintf(disp,sizeof(disp),"* %s %s",nick,text?text:""); else snprintf(disp,sizeof(disp),"<%s> %s",nick,text?text:"");
  Tab* dest=nullptr; if(target && isChannelName(target)) dest=getOrCreateTab(target,TAB_CHANNEL); else if(target && (eqI(target,gCfg.nick) || eqI(target,irc_nick))) dest=getOrCreateTab(nick,TAB_QUERY); else if(target){ if(isChannelName(target)) dest=getOrCreateTab(target,TAB_CHANNEL); else dest=getOrCreateTab(nick,TAB_QUERY); } else dest=findTab("status"); if(!dest) dest=activeTab(); appendLog(dest,disp,serverHHMM); if(isNotice) dest->lines[(dest->head+MAX_LINES_PER_TAB-1)%MAX_LINES_PER_TAB].flags|=0x04;
}
static void handleRawIrc(char* line){
  if(!line||!*line) return;
  char* tags=nullptr; char serverHHMM[6]={0}; bool hasServerTime=false;
  if(gInlineServerHHMM[0] && strlen(gInlineServerHHMM)==5){ safeCopy(serverHHMM,gInlineServerHHMM,sizeof(serverHHMM)); hasServerTime=true; gInlineServerHHMM[0]=0; }
  char* p=line;
  if(*p=='@'){ tags=p+1; char* sp=strchr(p,' '); if(!sp) return; *sp=0; hasServerTime=parseServerTimeHHMM(tags,serverHHMM); if(hasServerTime){ char loc[6]; localizeTimeHHMM(serverHHMM,loc); safeCopy(serverHHMM,loc,sizeof(serverHHMM)); } p=sp+1; }
  const char* sHHMM=hasServerTime?serverHHMM:nullptr;
  char* prefix=nullptr; if(*p==':'){ prefix=p+1; char* sp=strchr(p,' '); if(!sp) return; *sp=0; p=sp+1; }
  while(*p==' ') p++; char* cmd=p; char* sp=strchr(p,' '); char* paramsStart=nullptr; if(sp){ *sp=0; paramsStart=sp+1; } else paramsStart=nullptr;
  for(char* c=cmd;*c;++c) *c=toupper((unsigned char)*c);
  char* argv[16]; int argc=0;
  if(paramsStart){ char* s=paramsStart; while(*s && argc<16){ while(*s==' ') s++; if(!*s) break; if(*s==':'){ argv[argc++]=s+1; break; } char* e=strchr(s,' '); if(e){ *e=0; argv[argc++]=s; s=e+1; } else { argv[argc++]=s; break; } } }
  if(!strcmp(cmd,"CHGHOST")||!strcmp(cmd,"ACCOUNT")) return;
  if(!strcmp(cmd,"PING")){ const char* tok=argc>0?argv[argc-1]:"cardputer"; char out[64]; snprintf(out,sizeof(out),"PONG :%s\r\n",tok); if(gIrcConnected){ WiFiClient* cl=gCfg.useTLS?(WiFiClient*)&gSecure:(WiFiClient*)&gPlain; if(cl&&cl->connected()) cl->print(out); } char m[64]; snprintf(m,sizeof(m),"Ping <- %s",tok); logStatus(m); return; }
  if(!strcmp(cmd,"PONG")){ gYellowBlipMs=millis(); return; }
  if(!strcmp(cmd,"001")){
    gIrcRegistered=true; logStatus("Registered on IRC");
    if(gCfg.autojoin[0]){ char copy[128]; safeCopy(copy,gCfg.autojoin,sizeof(copy)); char* tok=strtok(copy,","); while(tok){ trim(tok); if(*tok){ WiFiClient* cl=gCfg.useTLS?(WiFiClient*)&gSecure:(WiFiClient*)&gPlain; if(cl&&cl->connected()){ char out[64]; snprintf(out,sizeof(out),"JOIN %s\r\n",tok); cl->print(out);} } tok=strtok(nullptr,","); } }
    return;
  }
  if(!strcmp(cmd,"433")){ char nn[36]; snprintf(nn,sizeof(nn),"%s_",gCfg.nick); safeCopy(gCfg.nick,nn,sizeof(gCfg.nick)); safeCopy(irc_nick,gCfg.nick,sizeof(irc_nick)); WiFiClient* cl=gCfg.useTLS?(WiFiClient*)&gSecure:(WiFiClient*)&gPlain; if(cl&&cl->connected()){ char out[48]; snprintf(out,sizeof(out),"NICK %s\r\n",gCfg.nick); cl->print(out);} char m[48]; snprintf(m,sizeof(m),"Nick in use, retry %s",gCfg.nick); logStatus(m); return; }
  if(!strcmp(cmd,"JOIN")){
    const char* chan=argc>0?argv[0]:""; char n[32]; nickFromPrefix(prefix?prefix:"",n,sizeof(n));
    if(eqI(n,gCfg.nick)||eqI(n,irc_nick)){ Tab* t=getOrCreateTab(chan,TAB_CHANNEL); t->unread=false; t->mention=false; char m[64]; snprintf(m,sizeof(m),"Joined %s",chan); appendLog(t,m,sHHMM); }
    else { Tab* t=findTab(chan); if(t) addNick(t,n); char m[64]; snprintf(m,sizeof(m),"* %s joined %s",n,chan); if(t) appendLog(t,m,sHHMM); }
    return;
  }
  if(!strcmp(cmd,"PART")){
    const char* chan=argc>0?argv[0]:""; char n[32]; nickFromPrefix(prefix?prefix:"",n,sizeof(n)); Tab* t=findTab(chan);
    if(eqI(n,gCfg.nick)||eqI(n,irc_nick)){ char m[48]; snprintf(m,sizeof(m),"Left %s",chan); if(t) appendLog(t,m,sHHMM); } else { if(t) delNick(t,n); char m[64]; snprintf(m,sizeof(m),"* %s left %s",n,chan); if(t) appendLog(t,m,sHHMM); }
    return;
  }
  if(!strcmp(cmd,"QUIT")){ char n[32]; nickFromPrefix(prefix?prefix:"",n,sizeof(n)); for(int i=0;i<gTabCount;++i) if(gTabs[i].type==TAB_CHANNEL) delNick(&gTabs[i],n); char m[64]; snprintf(m,sizeof(m),"* %s quit %s",n,argc>0?argv[0]:""); Tab* s=findTab("status"); if(s) appendLog(s,m,sHHMM); return; }
  if(!strcmp(cmd,"NICK")){
    char oldn[32]; nickFromPrefix(prefix?prefix:"",oldn,sizeof(oldn)); const char* newn=argc>0?argv[0]:""; if(eqI(oldn,gCfg.nick) || eqI(oldn,irc_nick)){
        safeCopy(gCfg.nick,newn,sizeof(gCfg.nick));
        safeCopy(irc_nick,newn,sizeof(irc_nick));
        sync_new_nick_to_sd(newn);
        ui_needs_redraw = true;
    }
    for(int i=0;i<gTabCount;++i){ for(int k=0;k<gTabs[i].nickCount;++k) if(eqI(gTabs[i].nicks[k],oldn)) safeCopy(gTabs[i].nicks[k],newn,sizeof(gTabs[i].nicks[k])); if(eqI(gTabs[i].name,oldn) && gTabs[i].type==TAB_QUERY) safeCopy(gTabs[i].name,newn,sizeof(gTabs[i].name)); }
    char m[64]; snprintf(m,sizeof(m),"* %s -> %s",oldn,newn); Tab* s=findTab("status"); if(s) appendLog(s,m,sHHMM); return;
  }
  if(!strcmp(cmd,"PRIVMSG")){
    const char* tgt=argc>0?argv[0]:""; const char* txt=argc>1?argv[1]:""; bool isAction=false; if(txt && !strncmp(txt,"\x01""ACTION ",8)){ isAction=true; txt+=8; size_t l=strlen(txt); if(l && txt[l-1]=='\x01') ((char*)txt)[l-1]=0; } handlePrivmsgC(prefix,tgt,txt,false,isAction,sHHMM); return;
  }
  if(!strcmp(cmd,"NOTICE")){ const char* tgt=argc>0?argv[0]:""; const char* txt=argc>1?argv[1]:""; handlePrivmsgC(prefix,tgt,txt,true,false,sHHMM); return; }
  if(!strcmp(cmd,"353")){
    const char* chan=nullptr; const char* list=nullptr; if(argc>=3){ chan=argv[1]; if(argv[2][0]=='='||argv[2][0]=='*'||argv[2][0]=='@'){ if(argc>=4){ chan=argv[2]; list=argv[3]; } else list=""; } else list=argv[2]; }
    if(chan && list){ Tab* t=findTab(chan); if(!t) t=getOrCreateTab(chan,TAB_CHANNEL); char copy[512]; safeCopy(copy,list,sizeof(copy)); char* tok=strtok(copy," "); while(tok){ while(*tok && strchr("~&@%+",*tok)) tok++; if(*tok) addNick(t,tok); tok=strtok(nullptr," "); } }
    return;
  }
  if(!strcmp(cmd,"366")) return;
  if(!strcmp(cmd,"332")||!strcmp(cmd,"333")||!strcmp(cmd,"TOPIC")){
    const char* chan=argc>0?argv[0]:""; const char* topic=argc>1?argv[1]:""; if(!strcmp(cmd,"332")){ chan=argc>=1?argv[1]:""; topic=argc>=2?argv[2]:""; } Tab* t=findTab(chan); if(t&&topic) safeCopy(t->topic,topic,sizeof(t->topic)); return;
  }
  if(isdigit((unsigned char)cmd[0])){ char m[180]; snprintf(m,sizeof(m),"[%s] %s",cmd,paramsStart?paramsStart:""); Tab* s=findTab("status"); if(s) appendLog(s,m,sHHMM); return; }
  Tab* s=findTab("status"); if(s){ char m[180]; snprintf(m,sizeof(m),"%s %s",cmd,line); appendLog(s,m,sHHMM); }
}

// ---------------------------------------------------------------------------
// Rendering - zero-flicker
// ---------------------------------------------------------------------------
static void initCanvas(){
  if(gCanvasReady) return;
  canvas.setColorDepth(8);
  canvas.setPsram(false);
  canvas.setTextSize(1);
  canvas.setTextWrap(false);
  canvas.deleteSprite();
  if(!canvas.createSprite(240,109)){ gCanvasReady=false; return; }
  canvas.fillSprite(0x0000);
  gCanvasReady = canvas.width()==240 && canvas.height()==109;
}

void draw_chat_view(){
// 1. Defensively catch any unallocated or corrupted room states instantly
if (gTabCount <= 0 || gTabCount > MAX_TABS || current_tab_index < 0 || current_tab_index >= gTabCount) {
    
    // Explicitly heal the variables inside a rapid, safe mutex block
    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        gTabCount = 1;
        current_tab_index = 0;
        
        memset(&gTabs, 0, sizeof(gTabs));
        strncpy(gTabs[0].name, "~system", sizeof(gTabs[0].name) - 1);
        strncpy(gTabs[0].server, "Bouncer", sizeof(gTabs[0].server) - 1);
        
        xSemaphoreGive(irc_mutex);
    }
    
    // 2. Direct-render a clear system status frame without any conditional flags or loops
    canvas.fillSprite(0x0000);
    canvas.setTextColor(0xFD20, 0x0000); // Amber/Orange
    canvas.setCursor(10, 35);
    canvas.print("SYSTEM BUFFER ALIGNING...");
    canvas.setCursor(10, 55);
    canvas.print("Initializing ~system panel");
    
    // Direct hardware blit past any legacy checks
    canvas.pushSprite(0, 12);
    
    // Clear the redraw flag so it can render the newly initialized panel on the very next pass
    ui_needs_redraw = true; 
    vTaskDelay(pdMS_TO_TICKS(20));
    return;
}
  if(!ui_needs_redraw) return;
  // SAFEMODE DIRECT-DRAW GATE
  if(safe_mode_active){
    canvas.fillSprite(0x0000);
    canvas.setTextColor(0x5AEB,0x0000);
    canvas.setCursor(10,20);
    canvas.print("[!] SYSTEM INITIALIZATION:");
    canvas.setTextColor(0xFD20,0x0000);
    canvas.setCursor(10,40);
    canvas.print("~safemode console active");
    canvas.setCursor(10,55);
    canvas.print("press Alt+Backspace to exit");
    canvas.pushSprite(0,12);
    ui_needs_redraw=false;
    return;
  }
  if(!gCanvasReady){ initCanvas(); if(!gCanvasReady){ ui_needs_redraw=false; return; } }
  // Step A: canvas.fillSprite(0x0000); and draw the middle 109px text frames.
  if(xSemaphoreTake(irc_mutex,pdMS_TO_TICKS(5))==pdTRUE){
    canvas.fillSprite(0x0000);
    Tab* tab=activeTab();
    if(tab){
      int total=tab->count; int vis=CHAT_ROWS; int startLogical=total - vis - tab->scroll; if(startLogical<0) startLogical=0; int endLogical=startLogical+vis; if(endLogical>total) endLogical=total;
      int current_y = 1;
      for(int li=startLogical; li<endLogical; ++li){
        // LAZY SCROLLBACK BLITTING: skip off-viewport historical entries entirely above visible viewport (Y < 0)
        if(current_y < 0){
          current_y += ROW_H;
          continue;
        }
        if(current_y > 109) break;
        const ChatLine* cl=ringAt(tab,li); if(!cl){ current_y += ROW_H; continue; }
        // ALTERNATING ROW BACKGROUND TINTS: even 0x0000 pitch-black, odd 0x0841 charcoal grey
        uint16_t rowBg = (li % 2 == 0) ? 0x0000 : 0x0841;
        canvas.fillRect(0, current_y, SCREEN_W, ROW_H, rowBg);
        // DIMMED PUNCTUATION MUTING: divider strictly 0x7BEF slate grey
        canvas.drawFastVLine(64, current_y, ROW_H, 0x7BEF);
        canvas.setTextColor(0x7BEF, rowBg); canvas.setCursor(2, current_y+1); canvas.print(cl->stamp);
        char out[MAX_LINE_LEN+1]; safeCopy(out, cl->text, sizeof(out));
        // mIRC CONTROL STRIPPING: safe pointer scanner to strip Bold (\x02) and Underline (\x1F)
        {
          char *src = out, *dst = out;
          while(*src){
            if(*src == '\x02' || *src == '\x1F'){ src++; continue; }
            // also handle literal \x02/\x1F if they appear as single byte 0x02/0x1F
            if((unsigned char)*src == 0x02 || (unsigned char)*src == 0x1F){ src++; continue; }
            *dst++ = *src++;
          }
          *dst = '\0';
        }
        sanitizeGlyphs(out);
        char nickTmp[32]={0}, bodyTmp[MAX_LINE_LEN+1]={0}; const char* txt2=out; bool hasNick=false;
        if(txt2[0]=='<' ){ const char* end=strchr(txt2,'>'); if(end){ size_t nlen=end-txt2-1; if(nlen<sizeof(nickTmp)){ memcpy(nickTmp,txt2+1,nlen); nickTmp[nlen]=0; hasNick=true; } const char* body=end+1; while(*body==' ') body++; safeCopy(bodyTmp,body,sizeof(bodyTmp)); } else safeCopy(bodyTmp,txt2,sizeof(bodyTmp)); }
        else if(txt2[0]=='*'&&txt2[1]==' '){ const char* sp=strchr(txt2+2,' '); if(sp){ size_t nlen=sp-(txt2+2); if(nlen<sizeof(nickTmp)){ memcpy(nickTmp,txt2+2,nlen); nickTmp[nlen]=0; hasNick=true; } safeCopy(bodyTmp,sp+1,sizeof(bodyTmp)); } else safeCopy(bodyTmp,txt2,sizeof(bodyTmp)); }
        else safeCopy(bodyTmp,txt2,sizeof(bodyTmp));
        // strip controls from body as well
        {
          char *src = bodyTmp, *dst = bodyTmp;
          while(*src){ if((unsigned char)*src==0x02 || (unsigned char)*src==0x1F){ src++; continue; } *dst++=*src++; } *dst='\0';
        }
        // NICKNAME HANDLING: bright white scanning efficiency, reverse-highlight if local nick mentioned
        bool isMention = false;
        if(irc_nick[0]){
          char lowBody[170]; safeCopy(lowBody, bodyTmp, sizeof(lowBody)); toLower(lowBody);
          char lowNick[32]; safeCopy(lowNick, irc_nick, sizeof(lowNick)); toLower(lowNick);
          if(lowNick[0] && strstr(lowBody, lowNick)) isMention = true;
        }
        if(hasNick&&nickTmp[0]){
          int nickW=strlen(nickTmp)*CHAR_W; int xNick=64-nickW-4; if(xNick<32) xNick=32;
          if(isMention){
            // NICKNAME REVERSE-HIGHLIGHTING: White over solid Orange 0xFD20
            canvas.fillRect(xNick-1, current_y, nickW+2, ROW_H, 0xFD20);
            canvas.setTextColor(0xFFFF, 0xFD20); canvas.setCursor(xNick, current_y+1); canvas.print(nickTmp);
          } else {
            canvas.setTextColor(0xFFFF, rowBg); canvas.setCursor(xNick, current_y+1); canvas.print(nickTmp);
          }
        }
        // Message body bright white for scanning efficiency
        if(isMention && !hasNick){
          // body contains nick but no sender prefix - highlight fragment via reverse block is complex; highlight whole row background already handled via isMention check for nick, but also highlight body token
          // fallback: draw body with orange background for mention
          int maxBodyCols=(SCREEN_W-70-2)/CHAR_W; if((int)strlen(bodyTmp)>maxBodyCols){ bodyTmp[maxBodyCols-1]='~'; bodyTmp[maxBodyCols]='\0'; }
          // we cannot easily highlight substring without word-wrap; draw body normally then overlay mention highlight by checking substring position
          // simple: if mention, draw body white on orange strip
          canvas.fillRect(70-1, current_y, SCREEN_W-70+1, ROW_H, 0xFD20);
          canvas.setTextColor(0xFFFF, 0xFD20); canvas.setCursor(70, current_y+1); canvas.print(bodyTmp);
        } else {
          canvas.setTextColor(0xFFFF, rowBg);
          int maxBodyCols=(SCREEN_W-70-2)/CHAR_W; if((int)strlen(bodyTmp)>maxBodyCols){ bodyTmp[maxBodyCols-1]='~'; bodyTmp[maxBodyCols]='\0'; }
          canvas.setCursor(70, current_y+1); canvas.print(bodyTmp);
        }
        if(cl->is_highlight) canvas.drawFastHLine(0, current_y+ROW_H-1, SCREEN_W, 0xFD20);
        // VERTICAL CANVAS BOUNDARY SHIELD: absolute bounding check before updating coordinate tracker
        if (current_y + 10 > 109) break;
        current_y += ROW_H;
      }
    }
    xSemaphoreGive(irc_mutex);
  }
  // Step B: Push middle sprite safely past the top status bar: canvas.pushSprite(0, 12);
  canvas.setTextColor(0xFFFF,0x0000);
  if(gCanvasReady) canvas.pushSprite(0, 12);
  // Step C: Use direct display commands (M5Cardputer.Display.print) to draw top status row overlay and bottom input bar straight to hardware display glass.
  {
    auto &d=M5Cardputer.Display;
    d.fillRect(0,0,SCREEN_W,12,0x0000);
    d.drawFastHLine(0,11,SCREEN_W,0x7BEF);
    d.setTextSize(1);
    int x=2;
    // UNIVERSAL 7-BIT ASCII TABS: #, >, ~ prefixes guaranteed
    for(int i=0;i<gTabCount && i<3 && x<SCREEN_W-2; ++i){
      if(gTabCount>3 && i==2){ d.setTextColor(0x7BEF,0x0000); d.setCursor(120,2); d.print("..."); break; }
      bool isActive=(i==gActive);
      char mark=' '; if(gTabs[i].mention) mark='!'; else if(gTabs[i].unread) mark='*';
      char prefix='~'; if(gTabs[i].type==TAB_CHANNEL) prefix='#'; else if(gTabs[i].type==TAB_QUERY) prefix='>';
      char tabTok[36]; if(mark!=' ') snprintf(tabTok,sizeof(tabTok),"%c%s%c",prefix,gTabs[i].name,mark); else snprintf(tabTok,sizeof(tabTok),"%c%s",prefix,gTabs[i].name);
      for(char* p=tabTok; *p; ++p){
        if(*p=='!') d.setTextColor(((millis()/500)%2)?0xFD20:0x7BEF,0x0000);
        else d.setTextColor((*p=='['||*p==']'||*p=='|')?0x7BEF:0xFFFF,0x0000);
        d.setCursor(x,2); d.print(*p); x+=CHAR_W; if(x>=SCREEN_W-6) break;
      }
      x+=4; if(x>=SCREEN_W) break;
    }
    // NAVBAR HUD ANCHORS strictly right-aligned
    char audioTag[7]; uint16_t audioCol; if(gCfg.current_audio==1){ safeCopy(audioTag,"[MUTE]",sizeof(audioTag)); audioCol=0xF800; } else { safeCopy(audioTag,"[+]",sizeof(audioTag)); audioCol=0x07E0; }
    d.setTextColor(audioCol,0x0000); d.setCursor(145,2); d.print(audioTag);
    char timeStr[9]; currentStamp(nullptr,0,timeStr,sizeof(timeStr));
    char timeHHMM[6]; if(strlen(timeStr)>=5){ timeHHMM[0]=timeStr[0]; timeHHMM[1]=timeStr[1]; timeHHMM[2]=':'; timeHHMM[3]=timeStr[3]; timeHHMM[4]=timeStr[4]; timeHHMM[5]=0; char loc[6]; localizeTimeHHMM(timeHHMM,loc); safeCopy(timeStr,loc,sizeof(timeStr)); }
    d.setTextColor(0xFFFF,0x0000); d.setCursor(175,2); d.print(timeStr);
    int bat_pct = (int)get_calibrated_battery_percentage();
    d.setTextColor(0xFFFF,0x0000); d.setCursor(212,2); d.printf("[%d%%]", bat_pct);
  }
  {
    auto &d=M5Cardputer.Display;
    d.fillRect(0,INPUT_Y,SCREEN_W,INPUT_H,0x0000);
    d.drawFastHLine(0,INPUT_Y,SCREEN_W,0x7BEF);
    d.setTextSize(1); d.setTextColor(0xFFFF,0x0000);
    int maxCols=INPUT_VISIBLE_COLS; if(maxCols>38) maxCols=38;
    if(gInputLen<=maxCols) gInputScroll=0; else { if(gInputCursor<gInputScroll) gInputScroll=gInputCursor; else if(gInputCursor>=gInputScroll+maxCols) gInputScroll=gInputCursor-maxCols+1; if(gInputScroll>gInputLen-maxCols) gInputScroll=gInputLen-maxCols; if(gInputScroll<0) gInputScroll=0; }
    int vlen=gInputLen-gInputScroll; if(vlen>38) vlen=38; if(vlen>maxCols) vlen=maxCols; if(vlen<0) vlen=0;
    char visible[INPUT_BUF_SZ]; memcpy(visible,gInput+gInputScroll,vlen); visible[vlen]=0;
    d.setCursor(2,INPUT_Y+4); d.print(">"); d.print(visible);
    // HARD SCROLL MASK: small rectangular color overlay matching background tone over first 12 horizontal pixels as clean scroll fade boundary edge
    d.fillRect(0, INPUT_Y, 12, INPUT_H, 0x0000);
    int curPos=gInputCursor-gInputScroll; int curX=2+CHAR_W+curPos*CHAR_W;
    if(curX>=2 && curX<SCREEN_W-2){ float phase=sinf((float)millis()/150.0f); float alpha=(phase+1.0f)*0.5f; uint8_t g=(uint8_t)(alpha*255); uint16_t col=d.color565(0,g,0); d.fillRect(curX,123,6,9,col); }
    // PACKET COUNTDOWN METRICS: remaining against 400, dimmed slate grey 0x7BEF at trailing edge
    int remaining = 400 - gInputLen; if(remaining<0) remaining=0;
    char cntBuf[8]; snprintf(cntBuf,sizeof(cntBuf),"%d",remaining);
    d.setTextColor(0x7BEF,0x0000); d.setCursor(SCREEN_W - 28, INPUT_Y+4); d.print(cntBuf);
  }
  // Step D: Unconditionally set ui_needs_redraw = false;
  ui_needs_redraw = false;
}

// ---------------------------------------------------------------------------
// Keyboard - hotkeys at absolute top
// ---------------------------------------------------------------------------
static void pushHistory(const char* s){ if(!s||!*s) return; if(gHistCount>0 && eqI(gHistory[(gHistCount-1)%HISTORY_DEPTH],s)) return; safeCopy(gHistory[gHistCount%HISTORY_DEPTH],s,INPUT_BUF_SZ); gHistCount++; if(gHistCount>HISTORY_DEPTH*10) gHistCount=HISTORY_DEPTH; gHistNav=-1; }

void handle_keyboard_inputs(){
  if(!M5Cardputer.Keyboard.isPressed()) return;
  last_input_time = millis();
  auto status=M5Cardputer.Keyboard.keysState();
  // PHYSICAL KEYBOARD NAVIGATION CLUSTER MACROS - at absolute top, return instantly
  if(status.alt && M5Cardputer.Keyboard.isKeyPressed(KEY_RIGHT)){
    int total=gTabCount; if(total>0){ gActive=(gActive+1)%total; gTabs[gActive].unread=false; gTabs[gActive].mention=false; ui_needs_redraw=true; }
    return;
  }
  if(status.alt && M5Cardputer.Keyboard.isKeyPressed(KEY_LEFT)){
    int total=gTabCount; if(total>0){ gActive=(gActive==0)?total-1:gActive-1; gTabs[gActive].unread=false; gTabs[gActive].mention=false; ui_needs_redraw=true; }
    return;
  }
  if(status.fn && M5Cardputer.Keyboard.isKeyPressed(KEY_RIGHT)){
    int total=gTabCount; if(total>1){
      char curSrv[32]; safeCopy(curSrv,gTabs[gActive].server[0]?gTabs[gActive].server:(gCfg.bncHost[0]?gCfg.bncHost:gCfg.host),sizeof(curSrv));
      for(int i=1;i<total;++i){ int idx=(gActive+i)%total; char tgt[32]; safeCopy(tgt,gTabs[idx].server[0]?gTabs[idx].server:(gCfg.bncHost[0]?gCfg.bncHost:gCfg.host),sizeof(tgt)); if(!eqI(curSrv,tgt)){ gActive=idx; gTabs[gActive].unread=false; gTabs[gActive].mention=false; ui_needs_redraw=true; return; } }
    }
    ui_needs_redraw=true; return;
  }
  if(status.fn && M5Cardputer.Keyboard.isKeyPressed(KEY_LEFT)){
    int total=gTabCount; if(total>1){
      char curSrv[32]; safeCopy(curSrv,gTabs[gActive].server[0]?gTabs[gActive].server:(gCfg.bncHost[0]?gCfg.bncHost:gCfg.host),sizeof(curSrv));
      for(int i=1;i<total;++i){ int idx=(gActive - i + total)%total; char tgt[32]; safeCopy(tgt,gTabs[idx].server[0]?gTabs[idx].server:(gCfg.bncHost[0]?gCfg.bncHost:gCfg.host),sizeof(tgt)); if(!eqI(curSrv,tgt)){ gActive=idx; gTabs[gActive].unread=false; gTabs[gActive].mention=false; ui_needs_redraw=true; return; } }
    }
    ui_needs_redraw=true; return;
  }
  if(status.fn){
    for(char c: status.word){ if(c=='s'||c=='S'){ gCfg.current_audio=gCfg.current_audio?0:1; digitalWrite(AMP_ENABLE_PIN,gCfg.current_audio?LOW:HIGH); ui_needs_redraw=true; return; } }
  }
  if(status.alt && M5Cardputer.Keyboard.isKeyPressed('\b')){
    gInputLen=0; gInputCursor=0; gInput[0]=0; gInputScroll=0; gHistNav=-1; ui_needs_redraw=true; return;
  }
  if(status.fn && M5Cardputer.Keyboard.isKeyPressed(KEY_DOWN)){
    Tab* curTab=activeTab();
    if(curTab && curTab->nickCount>0 && gInputLen>0){
      int end=gInputCursor; int start=end; while(start>0 && gInput[start-1]!=' ' && gInput[start-1]!=':' && gInput[start-1]!='@') start--;
      int partialLen=end-start;
      if(partialLen>0 && partialLen<32){
        char partial[32]; memcpy(partial,gInput+start,partialLen); partial[partialLen]=0;
        char partialLower[32]; safeCopy(partialLower,partial,sizeof(partialLower)); toLower(partialLower);
        for(int n=0;n<curTab->nickCount;++n){
          char nickLower[32]; safeCopy(nickLower,curTab->nicks[n],sizeof(nickLower)); toLower(nickLower);
          if(!strncmp(nickLower,partialLower,partialLen)){
            char completed[64]; snprintf(completed,sizeof(completed),"%s: ",curTab->nicks[n]);
            int compLen=strlen(completed); int tailLen=gInputLen-end;
            if(start+compLen+tailLen < INPUT_BUF_SZ){ memmove(gInput+start+compLen,gInput+end,tailLen+1); memcpy(gInput+start,completed,compLen); gInputLen=gInputLen-partialLen+compLen; gInputCursor=start+compLen; gInputScroll=0; ui_needs_redraw=true; return; }
          }
        }
      }
    }
    ui_needs_redraw=true; return;
  }
  // remaining input handling - non-blocking, no while loops
  bool hasWord=!status.word.empty();
  bool up=false, down=false;
  if(status.fn){ for(char c: status.word){ if(c==';') up=true; if(c=='.') down=true; } }
  if(gInputLen==0 && (up||down)){
    if(gHistCount>0){
      int depth=gHistCount<HISTORY_DEPTH?gHistCount:HISTORY_DEPTH;
      if(gHistNav==-1) gHistNav=up?depth-1:0; else { if(up) gHistNav=(gHistNav-1+depth)%depth; else gHistNav=(gHistNav+1)%depth; }
      int idx=(gHistCount-depth+gHistNav)%HISTORY_DEPTH; if(idx<0) idx+=HISTORY_DEPTH;
      safeCopy(gInput,gHistory[idx],sizeof(gInput)); gInputLen=strlen(gInput); gInputCursor=gInputLen; ui_needs_redraw=true; return;
    }
  } else if(hasWord) gHistNav=-1;
  for(char c: status.word){
    if(c=='\n'||c=='\r') continue;
    if(status.fn && (c==';'||c=='.'||c==','||c=='/')) continue;
    if(c>=32 && c<127 && gInputLen < INPUT_BUF_SZ-1){ memmove(gInput+gInputCursor+1,gInput+gInputCursor,gInputLen-gInputCursor+1); gInput[gInputCursor++]=c; gInputLen++; gHistNav=-1; ui_needs_redraw=true; gWhiteSparkMs=millis(); }
  }
  if(status.del){
    if(gInputLen>0 && gInputCursor>0){ memmove(gInput+gInputCursor-1,gInput+gInputCursor,gInputLen-gInputCursor+1); gInputCursor--; gInputLen--; gHistNav=-1; ui_needs_redraw=true; }
    else if(gInputLen==0){ gRedFlashMs=millis(); }
  }
  if(status.fn){
    for(char c: status.word){
      if(c==',' && gInputCursor>0){ gInputCursor--; ui_needs_redraw=true; }
      if(c=='/' && gInputCursor<gInputLen){ gInputCursor++; ui_needs_redraw=true; }
      if(c==';'){ Tab* t=activeTab(); if(t && t->scroll < t->count){ t->scroll++; ui_needs_redraw=true; } }
      if(c=='.'){ Tab* t=activeTab(); if(t && t->scroll>0){ t->scroll--; ui_needs_redraw=true; } }
    }
  } else {
    for(char c: status.word){
      if(c==';'){ Tab* t=activeTab(); if(t && t->scroll < t->count){ t->scroll++; ui_needs_redraw=true; } }
      if(c=='.'){ Tab* t=activeTab(); if(t && t->scroll>0){ t->scroll--; ui_needs_redraw=true; } }
    }
  }
  if(status.enter){
    if(gInputLen>0){ gInput[gInputLen]=0; char copy[INPUT_BUF_SZ]; safeCopy(copy,gInput,sizeof(copy)); gInputLen=0; gInputCursor=0; gInput[0]=0; gInputScroll=0; gHistNav=-1; handleUserInput(copy); ui_needs_redraw=true; }
  }
  if(!status.word.empty() || status.del || status.enter) ui_needs_redraw=true;
}

static void handleUserInput(const char* in){
  if(!in||!*in) return; pushHistory(in);
  if(in[0]!='/'){
    Tab* at=activeTab(); if(!at || at->type==TAB_STATUS){ logStatus("No channel/query"); return; }
    WiFiClient* cl=gCfg.useTLS?(WiFiClient*)&gSecure:(WiFiClient*)&gPlain;
    if(cl&&cl->connected()){ char out[IRC_LINE_MAX+1]; snprintf(out,sizeof(out),"PRIVMSG %s :%s\r\n",at->name,in); cl->print(out); }
    char disp[MAX_LINE_LEN+1]; snprintf(disp,sizeof(disp),"<%s> %s",gCfg.nick,in); appendLog(at,disp); at->lines[(at->head+MAX_LINES_PER_TAB-1)%MAX_LINES_PER_TAB].flags|=0x02; return;
  }
  char copy[INPUT_BUF_SZ]; safeCopy(copy,in,sizeof(copy)); char* sp=strchr(copy,' '); char* arg=nullptr; if(sp){ *sp=0; arg=sp+1; while(*arg==' ') arg++; if(!*arg) arg=nullptr; } char* cmd=copy+1; toLower(cmd);
  WiFiClient* cl=gCfg.useTLS?(WiFiClient*)&gSecure:(WiFiClient*)&gPlain;
  auto doSend=[&](const char* fmt, ...){ char buf[IRC_LINE_MAX+1]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap); strncat(buf, "\r\n", sizeof(buf)-strlen(buf)-1); if(cl&&cl->connected()){ cl->print(buf); } };
  if(!strcmp(cmd,"join")){ if(!arg){ logStatus("Usage: /join #chan"); return; } char first[32]; safeCopy(first,arg,sizeof(first)); char* c=strchr(first,','); if(c) *c=0; char* c2=strchr(first,' '); if(c2) *c2=0; doSend("JOIN %s",arg); getOrCreateTab(first[0]?first:arg,TAB_CHANNEL); return; }
  if(!strcmp(cmd,"part")){ Tab* at=activeTab(); const char* chan=arg?(const char*)arg:(at&&at->type==TAB_CHANNEL?at->name:nullptr); if(!chan){ logStatus("Usage: /part [#chan]"); return; } doSend("PART %s",chan); return; }
  if(!strcmp(cmd,"msg")||!strcmp(cmd,"query")){ if(!arg){ logStatus("Usage: /msg <nick> <text>"); return; } char* sp2=strchr(arg,' '); if(!sp2){ getOrCreateTab(arg,TAB_QUERY); return; } *sp2=0; char* nick=arg; char* txt=sp2+1; while(*txt==' ') txt++; doSend("PRIVMSG %s :%s",nick,txt); Tab* t=getOrCreateTab(nick,TAB_QUERY); char disp[MAX_LINE_LEN+1]; snprintf(disp,sizeof(disp),"<%s> %s",gCfg.nick,txt); appendLog(t,disp); return; }
  if(!strcmp(cmd,"nick")){ if(!arg){ logStatus("Usage: /nick <newnick>"); return; } doSend("NICK %s",arg); return; }
  if(!strcmp(cmd,"quit")){ if(arg) doSend("QUIT :%s",arg); else doSend("QUIT"); return; }
  if(!strcmp(cmd,"me")){ Tab* at=activeTab(); if(!at||at->type==TAB_STATUS){ logStatus("No channel"); return; } if(!arg){ logStatus("Usage: /me <action>"); return; } doSend("PRIVMSG %s :\x01" "ACTION %s\x01",at->name,arg); char disp[MAX_LINE_LEN+1]; snprintf(disp,sizeof(disp),"* %s %s",gCfg.nick,arg); appendLog(at,disp); return; }
  if(!strcmp(cmd,"clear")){ Tab* at=activeTab(); if(at){ at->head=0; at->count=0; at->scroll=0; ui_needs_redraw=true; } return; }
  if(cl&&cl->connected()){ char out[IRC_LINE_MAX+1]; snprintf(out,sizeof(out),"%s%s%s\r\n",cmd+0,arg?" ":"",arg?arg:""); // raw fallback
    // reconstruct original without leading slash
    char raw[INPUT_BUF_SZ]; if(arg) snprintf(raw,sizeof(raw),"%s %s",cmd,arg); else safeCopy(raw,cmd,sizeof(raw)); for(char* p=raw;*p;++p) *p=toupper((unsigned char)*p); // keep uppercase for raw IRC
    // actually send as typed without slash uppercased? just send cmd arg
    char raw2[IRC_LINE_MAX+1]; if(arg) snprintf(raw2,sizeof(raw2),"%s %s\r\n",cmd,arg); else snprintf(raw2,sizeof(raw2),"%s\r\n",cmd); cl->print(raw2);
  }
}

static void run_bouncer_setup_menu(){
  bool done=false; char tmpHost[96]; safeCopy(tmpHost,gCfg.bncHost,sizeof(tmpHost));
  char tmpUser[64]; safeCopy(tmpUser,gCfg.bncUser,sizeof(tmpUser));
  char tmpPass[64]; safeCopy(tmpPass,gCfg.bncPass,sizeof(tmpPass));
  // simple single-screen prompt - use blocking but yield
  // For brevity reuse handle_keyboard_inputs flow via gInput - just open status log
  logStatus("Bouncer setup: edit /irc/config.txt bnc_host/bnc_user/bnc_pass");
}

// ---------------------------------------------------------------------------
// SD config
// ---------------------------------------------------------------------------
static void loadConfig(){
  memset(&gCfg,0,sizeof(gCfg));
  safeCopy(gCfg.host,"irc.libera.chat",sizeof(gCfg.host)); gCfg.port=6697; gCfg.useTLS=true;
  safeCopy(gCfg.nick,"CardADV",sizeof(gCfg.nick)); safeCopy(gCfg.user,"cardputer",sizeof(gCfg.user)); safeCopy(gCfg.realname,"Cardputer IRC",sizeof(gCfg.realname));
  safeCopy(gCfg.logRoot,"/irc/logs",sizeof(gCfg.logRoot)); gCfg.logLevel=LOG_DMS_ONLY; gCfg.brightness=10;
  gCfg.timezone_index=0; gCfg.use_12_hour_format=false; gTimezoneIndex=0; gUse12Hour=false;
  if(!gSdReady) return;
  if(irc_mutex && xSemaphoreTake(irc_mutex,pdMS_TO_TICKS(50))!=pdTRUE) return;
  if(!SD.exists("/irc/config.txt")){ xSemaphoreGive(irc_mutex); return; }
  File f=SD.open("/irc/config.txt",FILE_READ); if(!f){ xSemaphoreGive(irc_mutex); return; }
  char line[256];
  while(f.available()){
    yield(); vTaskDelay(pdMS_TO_TICKS(1));
    int len=0; while(f.available() && len<(int)sizeof(line)-1){ char c=(char)f.read(); if(c=='\r') continue; if(c=='\n') break; line[len++]=c; }
    line[len]=0; char* s=line; trim(s); if(!*s||*s=='#'||*s==';') continue; char* eq=strchr(s,'='); if(!eq) continue; *eq=0; char* k=s; char* v=eq+1; trim(k); trim(v); for(char* p=k;*p;++p) *p=tolower((unsigned char)*p);
    if(!strcmp(k,"wifi_ssid")) safeCopy(gCfg.wifiSSID,v,sizeof(gCfg.wifiSSID));
    else if(!strcmp(k,"wifi_pass")) safeCopy(gCfg.wifiPass,v,sizeof(gCfg.wifiPass));
    else if(!strcmp(k,"irc_host")) safeCopy(gCfg.host,v,sizeof(gCfg.host));
    else if(!strcmp(k,"irc_port")) gCfg.port=(uint16_t)atoi(v);
    else if(!strcmp(k,"irc_use_tls")) gCfg.useTLS=strToBoolC(v);
    else if(!strcmp(k,"irc_nick")||!strcmp(k,"nick")) safeCopy(gCfg.nick,v,sizeof(gCfg.nick));
    else if(!strcmp(k,"irc_user")||!strcmp(k,"username")) safeCopy(gCfg.user,v,sizeof(gCfg.user));
    else if(!strcmp(k,"irc_realname")||!strcmp(k,"realname")) safeCopy(gCfg.realname,v,sizeof(gCfg.realname));
    else if(!strcmp(k,"irc_pass")||!strcmp(k,"server_pass")) safeCopy(gCfg.pass,v,sizeof(gCfg.pass));
    else if(!strcmp(k,"autojoin")) safeCopy(gCfg.autojoin,v,sizeof(gCfg.autojoin));
    else if(!strcmp(k,"channel_log_enabled")||!strcmp(k,"chat_log_enabled")) gCfg.logLevel=parseLogLevel(v);
    else if(!strcmp(k,"log_root")) safeCopy(gCfg.logRoot,v,sizeof(gCfg.logRoot));
    else if(!strcmp(k,"screen_brightness")){ gCfg.brightness=(uint8_t)constrain(atoi(v),0,10); }
    else if(!strcmp(k,"current_audio")) gCfg.current_audio=atoi(v);
    else if(!strcmp(k,"bnc_host")) safeCopy(gCfg.bncHost,v,sizeof(gCfg.bncHost));
    else if(!strcmp(k,"bnc_port")) gCfg.bncPort=(uint16_t)atoi(v);
    else if(!strcmp(k,"bnc_user")) safeCopy(gCfg.bncUser,v,sizeof(gCfg.bncUser));
    else if(!strcmp(k,"bnc_pass")) safeCopy(gCfg.bncPass,v,sizeof(gCfg.bncPass));
    else if(!strcmp(k,"bnc_enabled")) gCfg.bncEnabled=strToBoolC(v);
    else if(!strcmp(k,"sasl_enabled")) gCfg.saslEnabled=strToBoolC(v);
    else if(!strcmp(k,"sasl_user")) safeCopy(gCfg.saslUser,v,sizeof(gCfg.saslUser));
    else if(!strcmp(k,"sasl_pass")) safeCopy(gCfg.saslPass,v,sizeof(gCfg.saslPass));
    else if(!strcmp(k,"timezone_index")){ int idx=atoi(v); if(idx<0) idx=0; if(idx>=TZ_COUNT) idx=TZ_COUNT-1; gTimezoneIndex=idx; gCfg.timezone_index=idx; }
    else if(!strcmp(k,"use_12_hour_format")){ bool b=atoi(v)!=0||strToBoolC(v); gUse12Hour=b; gCfg.use_12_hour_format=b; }
  }
  f.close(); xSemaphoreGive(irc_mutex);
  if(gTimezoneIndex<0) gTimezoneIndex=0; if(gTimezoneIndex>=TZ_COUNT) gTimezoneIndex=TZ_COUNT-1; gCfg.timezone_index=gTimezoneIndex; gCfg.use_12_hour_format=gUse12Hour;
}
static void saveConfig(){
  if(!gSdReady) return;
  if(irc_mutex && xSemaphoreTake(irc_mutex,pdMS_TO_TICKS(50))!=pdTRUE) return;
  if(!SD.exists("/irc")) SD.mkdir("/irc");
  if(SD.exists("/irc/config.txt")) SD.remove("/irc/config.txt");
  File f=SD.open("/irc/config.txt",FILE_WRITE); if(!f){ xSemaphoreGive(irc_mutex); return; }
  f.printf("wifi_ssid=%s\n",gCfg.wifiSSID);
  f.printf("wifi_pass=%s\n",gCfg.wifiPass);
  f.printf("irc_host=%s\n",gCfg.host);
  f.printf("irc_port=%u\n",gCfg.port);
  f.printf("irc_use_tls=%s\n",gCfg.useTLS?"true":"false");
  f.printf("irc_nick=%s\n",gCfg.nick);
  f.printf("irc_user=%s\n",gCfg.user);
  f.printf("irc_realname=%s\n",gCfg.realname);
  f.printf("irc_pass=%s\n",gCfg.pass);
  f.printf("autojoin=%s\n",gCfg.autojoin);
  f.printf("channel_log_enabled=%d\n",(int)gCfg.logLevel);
  f.printf("log_root=%s\n",gCfg.logRoot);
  f.printf("screen_brightness=%u\n",gCfg.brightness);
  f.printf("current_audio=%d\n",gCfg.current_audio);
  f.printf("bnc_host=%s\n",gCfg.bncHost);
  f.printf("bnc_port=%u\n",gCfg.bncPort);
  f.printf("bnc_user=%s\n",gCfg.bncUser);
  f.printf("bnc_pass=%s\n",gCfg.bncPass);
  f.printf("bnc_enabled=%s\n",gCfg.bncEnabled?"true":"false");
  f.printf("sasl_enabled=%s\n",gCfg.saslEnabled?"true":"false");
  f.printf("sasl_user=%s\n",gCfg.saslUser);
  f.printf("sasl_pass=%s\n",gCfg.saslPass);
  f.printf("timezone_index=%d\n",gTimezoneIndex);
  f.printf("use_12_hour_format=%d\n",gUse12Hour?1:0);
  f.close(); xSemaphoreGive(irc_mutex);
}

// ---------------------------------------------------------------------------
// WiFi helpers
// ---------------------------------------------------------------------------
static void ensureWiFi(){
  if(safe_mode_active) return;
  if(WiFi.status()==WL_CONNECTED) return;
  if(gCfg.wifiSSID[0]==0) return;
  WiFi.mode(WIFI_STA); WiFi.begin(gCfg.wifiSSID,gCfg.wifiPass);
  uint32_t start=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-start<12000){ vTaskDelay(pdMS_TO_TICKS(100)); yield(); }
}

// ---------------------------------------------------------------------------
// IRC network task - Core 0 strictly socket bytes, no display/keyboard/audio
// ---------------------------------------------------------------------------
void irc_network_task(void* pv){
  (void)pv;
  for(;;){
    if(safe_mode_active){ vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    ensureWiFi();
    if(WiFi.status()!=WL_CONNECTED){ gIrcConnected=false; vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    const char* host=gCfg.bncHost[0]?gCfg.bncHost:gCfg.host;
    uint16_t port=gCfg.bncHost[0] && gCfg.bncPort?gCfg.bncPort:gCfg.port;
    bool useTLS=gCfg.useTLS;
    WiFiClient* cl=useTLS?(WiFiClient*)&gSecure:(WiFiClient*)&gPlain;
    if(!cl->connected()){
      gIrcConnected=false; gIrcRegistered=false;
      bool ok=false;
      if(useTLS){ gSecure.setInsecure(); ok=gSecure.connect(host,port); } else ok=gPlain.connect(host,port);
      if(!ok){ vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
      gIrcConnected=true; gLastRxMs=millis();
      // IRCv3 handshake
      if(gCfg.bncUser[0] && gCfg.bncPass[0]){
        char passBuf[160]; snprintf(passBuf,sizeof(passBuf),"PASS %s:%s\r\n",gCfg.bncUser,gCfg.bncPass); cl->print(passBuf);
      } else if(gCfg.pass[0]){ char p[96]; snprintf(p,sizeof(p),"PASS %s\r\n",gCfg.pass); cl->print(p); }
      cl->print("CAP LS 302\r\n");
      cl->print("CAP REQ :cap-notify server-time away-notify account-notify extended-join\r\n");
      cl->print("CAP END\r\n");
      char nickCmd[64]; snprintf(nickCmd,sizeof(nickCmd),"NICK %s\r\n",gCfg.nick); cl->print(nickCmd);
      char userCmd[128]; snprintf(userCmd,sizeof(userCmd),"USER %s 0 * :%s\r\n",gCfg.user,gCfg.realname); cl->print(userCmd);
      gRxLen=0; gInlineServerHHMM[0]=0;
    }
    // read loop - ban display/keyboard/audio
    while(cl->connected()){
      if(safe_mode_active) break;
      int avail=cl->available();
      if(avail>0){
        char chunk[128]; int toRead= avail> (int)sizeof(chunk)-1 ? (int)sizeof(chunk)-1 : avail;
        int r=cl->readBytes(chunk,toRead); if(r<=0){ vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        for(int i=0;i<r;++i){
          char c=chunk[i];
          if(c=='\r') continue;
          if(c=='\n'){
            gRxAccum[gRxLen]=0;
            // ultra-lean server-time extraction before stripping tags: look for time= tag with T
            gInlineServerHHMM[0]=0;
            if(gRxAccum[0]=='@'){
              char* sp=strchr(gRxAccum,' '); if(sp){ *sp=0; char hhmm[6]; if(parseServerTimeHHMM(gRxAccum+1,hhmm)){ char loc[6]; localizeTimeHHMM(hhmm,loc); safeCopy(gInlineServerHHMM,loc,sizeof(gInlineServerHHMM)); } *sp=' '; }
            }
            // dynamic multi-network auto-discovery: inspect TARGET after parsing? do lightweight scan for #channel
            char lineCopy[IRC_LINE_MAX+1]; safeCopy(lineCopy,gRxAccum,sizeof(lineCopy));
            // check for JOIN/PRIVMSG channel to auto-create tab on-the-fly up to MAX_TABS
            if(!safe_mode_active && irc_mutex && xSemaphoreTake(irc_mutex,pdMS_TO_TICKS(50))==pdTRUE){
              // peek channel token
              char* p=lineCopy; if(*p=='@'){ char* s=strchr(p,' '); if(s) p=s+1; } if(*p==':'){ char* s=strchr(p,' '); if(s) p=s+1; } while(*p==' ') p++; // cmd
              char* cmdEnd=strchr(p,' '); if(cmdEnd) p=cmdEnd+1; while(*p==' ') p++;
              // param0 is target
              if(*p && *p!=':'){ char* tEnd=strchr(p,' '); if(tEnd) *tEnd=0; if(isChannelName(p)){ getOrCreateTab(p,TAB_CHANNEL); } else if(p[0]!='#' && p[0]!='~'){ /* maybe query - handled in handleRawIrc */ }
              }
              xSemaphoreGive(irc_mutex);
            }
            char ircLine[IRC_LINE_MAX+1]; safeCopy(ircLine,gRxAccum,sizeof(ircLine));
            handleRawIrc(ircLine);
            gRxLen=0; gLastRxMs=millis();
            if(gRxLen >= (int)sizeof(gRxAccum)-1) gRxLen=0;
          } else {
            if(gRxLen < (int)sizeof(gRxAccum)-1) gRxAccum[gRxLen++]=c; else { // overflow discard
              // discard until newline
              if(gRxLen>512){ gRxLen=0; gInlineServerHHMM[0]=0; }
            }
          }
        }
      } else {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      if(millis()-gLastRxMs > 300000){ cl->stop(); gIrcConnected=false; break; }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    gIrcConnected=false; gIrcRegistered=false; cl->stop();
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ---------------------------------------------------------------------------
// UI task - Core 1 handles update, keyboard, conditional draw, <5ms gate
// ---------------------------------------------------------------------------
void custom_ui_loop_task(void* pv){
  (void)pv;
  for(;;){
    pollBattery();
    updateLedTelemetry();
    M5Cardputer.update();
    handle_keyboard_inputs();
    if(ui_needs_redraw) draw_chat_view();
    if (millis() - last_input_time > 60000) { M5Cardputer.Display.setBrightness(10); } else { M5Cardputer.Display.setBrightness(screen_brightness); }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------------------------------------------------------------------------
// Setup / Loop
// ---------------------------------------------------------------------------
void setup(){
  safe_mode_active = false;
  M5Cardputer.begin(true, true, false, false); // Display=true, Keyboard=true, Speaker=false, Mic=false
  Wire.setTimeOut(50);
  SPI.begin();
  gSdReady = SD.begin(12, SPI, 10000000); // Enforce strict 10MHz clock speed to stop data cross-talk noise
  // Power gate Stamp-S3A NeoPixel rail
  pinMode(38, OUTPUT); digitalWrite(38, HIGH);
  pinMode(AMP_ENABLE_PIN, OUTPUT); digitalWrite(AMP_ENABLE_PIN, HIGH);
  pinMode(BATTERY_PIN, INPUT);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  neopixelWrite(LED_PIN,0,0,0);
  irc_mutex = xSemaphoreCreateMutex();
  gLogQueue.init();
  last_input_time = millis();
  screen_brightness = gCfg.brightness * 25;
  if(screen_brightness < 10) screen_brightness = 80;
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(0x0000);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(0xFFFF,0x0000);
  M5Cardputer.Display.setCursor(8,10); M5Cardputer.Display.print("Cardputer IRC 0.4");
  M5Cardputer.Display.setCursor(8,22); M5Cardputer.Display.setTextColor(0x8410,0x0000); M5Cardputer.Display.print("Adv ST7789 240x135 NO PSRAM");
  M5Cardputer.Display.setCursor(8,34); M5Cardputer.Display.print("8-bit canvas 20-ring");
  // Sub-canvas allocation matrix - inherit landscape naturally after setRotation
  if(esp_get_free_heap_size() < 30000){ /* low heap guard */ }
  canvas.setColorDepth(8);
  canvas.setPsram(false);
  canvas.createSprite(240,109);
  gCanvasReady = canvas.width()==240 && canvas.height()==109;
  if(gCanvasReady) canvas.fillSprite(0x0000);
  // Intro splash sits drawn, non-blocking 1500ms cold-start window - poll Backspace '\b' only
  uint32_t winStart=millis();
  bool sawBackspace=false;
  while(millis()-winStart < 1500){
    M5Cardputer.update();
    if(M5Cardputer.Keyboard.isKeyPressed('\b')) sawBackspace=true;
    vTaskDelay(pdMS_TO_TICKS(10)); yield();
  }
  if(sawBackspace) safe_mode_active=true; else safe_mode_active=false;
  // SD already mounted at 10MHz above - shared SPI bus stable
  loadConfig();
  safeCopy(irc_nick, gCfg.nick, sizeof(irc_nick));
  gTimezoneIndex=gCfg.timezone_index; gUse12Hour=gCfg.use_12_hour_format;
  if(gTimezoneIndex<0||gTimezoneIndex>=TZ_COUNT) gTimezoneIndex=0;
  screen_brightness = gCfg.brightness * 25; if(screen_brightness < 10) screen_brightness = 80; if(screen_brightness > 255) screen_brightness = 255;
  last_input_time = millis();
  purge_old_logs();
  ensureStatus();
  // trailing edge spawn - uniform FreeRTOS balancing
  xTaskCreatePinnedToCore(irc_network_task, "irc_network_task", 8192, nullptr, 1, &gNetTaskHandle, 0);
  xTaskCreatePinnedToCore(custom_ui_loop_task, "custom_ui_loop_task", 16384, nullptr, 1, &gUiTaskHandle, 1);
  xTaskCreatePinnedToCore(logTask, "logTask", 4096, nullptr, 1, &gLogTaskHandle, 0);
  ui_needs_redraw=true;
}

void loop(){
  vTaskDelete(NULL);
}
