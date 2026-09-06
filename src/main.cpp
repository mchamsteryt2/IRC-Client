#include <Arduino.h>
#include <M5Cardputer.h>
#include <M5GFX.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <mbedtls/base64.h>
#include <esp_task_wdt.h>

// ==========================================
// ⚡ SYSTEM CONSTANTS & HARDWARE BUFFER BOUNDS
// ==========================================
#define MAX_TABS 10
#define MSG_BUFFER_SIZE 30
#define COLUMNS_MAX 38

struct ChatLine {
    char timeStr[6];
    char nick[16];
    char message[128];
    uint16_t color;
    bool is_highlight;
};

struct Tab {
    char name[32];
    char server[32];
    ChatLine lines[MSG_BUFFER_SIZE];
    int line_count;
    int head;
    char topic[64];
    char nicks[12][16];
    uint8_t nick_count;
    bool pinned;
    bool muted;
    char modes[16];
    bool nicks_away[12];
};
struct WhoisCache {
    char nick[32];
    char user[32];
    char host[32];
    char real[64];
    char server[32];
    char account[32];
    char channels[64];
    int idle;
    bool secure;
};

// ==========================================
// 🔏 REGISTERS (EXPLICIT VOLATILE STATE METRICS)
// ==========================================
volatile bool safe_mode_active = false;
volatile bool system_booted = false;
volatile bool ui_needs_redraw = true;
volatile uint8_t current_tab_index = 0;
volatile uint8_t gTabCount = 0;
int screen_brightness = 120;
int g_backlight_level = 120; // for LED scaling tie
struct LedEvent { uint8_t mode; unsigned long until; };
LedEvent ledQueue[4] = {{255,0},{255,0},{255,0},{255,0}};
void queueLed(uint8_t mode, uint16_t durationMs) {
    unsigned long now = millis();
    for(int i=0;i<4;i++) if(ledQueue[i].until <= now) { ledQueue[i].mode=mode; ledQueue[i].until=now+durationMs; return; }
    // if full, overwrite oldest
    ledQueue[0].mode=mode; ledQueue[0].until=now+durationMs;
}
uint8_t popQueuedLed() {
    unsigned long now=millis();
    for(int i=0;i<4;i++) if(ledQueue[i].until > now) return ledQueue[i].mode;
    return 255;
}

// ==========================================
// 🔀 INTERACTIVE APP-MODE STATE MACHINE
// ==========================================
enum AppMode { MODE_CHAT, MODE_WIFI, MODE_BOUNCER, MODE_SETTINGS, MODE_THEME, MODE_LOGS, MODE_WHOIS, MODE_NAVIGATOR };
volatile AppMode current_app_mode = MODE_CHAT;
int menu_selection_idx = 0;
int nav_server_select_idx = 0;
int nav_channel_select_idx = 0;
int nav_focus_column = 0; // 0=LEFT server, 1=RIGHT channel - stealth navigator focus
char nav_filter[16] = {0};
bool nav_filter_active = false;
char search_query[16] = {0};
bool search_active = false;
char ignore_list[8][16] = {{0}};
int ignore_count = 0;
char highlight_words[8][32] = {{0}};
int highlight_count = 0;
bool is_away = false;
bool show_dBm = false;
bool show_mentions_peek = false;
unsigned long last_away_tick = 0;
unsigned long last_keypress_debounce = 0; // Fixed key chatter metric

Tab gTabs[MAX_TABS];
SemaphoreHandle_t irc_mutex = NULL;
SemaphoreHandle_t sd_mutex = NULL;
QueueHandle_t gLogQueue = NULL;
M5Canvas canvas(&M5Cardputer.Display);

// Core Configuration Properties (Zero-Initialized, No Hardcoding)
char wifi_ssid[64] = {0};
char wifi_pass[64] = {0};
char wifi_ssid2[32] = {0}; // Fallback phone hotspot network name
char wifi_pass2[64] = {0}; // Fallback password
bool using_backup_ap = false;
unsigned long last_wifi_fail_tick = 0;
unsigned long last_user_keyboard_input_tick = 0; // Tracks live inactivity intervals
// SD Wi-Fi Vault: up to 5 SSID:PASS pairs in /irc/wifi_cache.txt (compact ASCII flat-file, NVS-free)
char wifi_vault_ssid[5][64] = {{0}};
char wifi_vault_pass[5][64] = {{0}};
int wifi_vault_count = 0;
unsigned long last_session_write_tick = 0; // 2s debounce for session_state.tmp
// QoL: Input history ring (10 entries, survives tab switches)
char input_history[10][128] = {{0}};
int input_history_head = 0;
int input_history_len = 0;
int input_history_pos = -1; // -1 = live buffer
bool show_nicklist = false;
int8_t rssi_history[8] = { -127,-127,-127,-127,-127,-127,-127,-127 };
uint8_t rssi_history_idx = 0;

volatile int scrollback_offset_idx = 0; // 0 = Live bottom-anchored feed, >0 = Looking at past lines
volatile bool scrollback_mode_active = false;
int scrollback_offset = 0;          // Tracking line offset for scrolling history up/down (roadmap alias)
bool is_scrollback_active = false;  // Disables live-snapping to bottom when true
unsigned long wifi_drop_timestamp = 0;
bool failover_in_progress = false;
TaskHandle_t xNetworkTaskHandle = NULL;
TaskHandle_t xUITaskHandle = NULL;
char irc_nick[64]  = {0};
char bnc_host[64]  = {0};
int bnc_port       = 0;
char bnc_user[64]  = {0};
char bnc_pass[64]  = {0};
int channel_log_enabled = 1;
int current_tz_idx      = 2;
int use_dst             = 0; // manual DST +1h override for wrong NTP zone
bool use_light_theme    = false;
int theme_accent        = 0; // 0=default,1=amber,2=cyan,3=purple
int text_scale          = 1; // 1 or 2
bool speaker_enabled    = false;
WhoisCache whois_cache = {0};
bool whois_pending = false;
int sound_profile = 1; // 0 off, 1 mention only, 2 all events
char alias_names[5][16] = {{0}};
char alias_cmds[5][64] = {{0}};
int alias_count = 0;
volatile bool request_network_reload = false;
volatile bool master_scan_complete_global = false;
char chan_list_cache[10][32] = {{0}};
int chan_list_count = 0;
unsigned long adj_time = 0; // Sync offset for wall-clock
int use_12_hour_format  = 1;

#define MAX_NETWORKS 5
char discovered_networks[MAX_NETWORKS][32] = {{0}};
volatile uint8_t discovered_network_count = 0;
WiFiClientSecure clients[MAX_NETWORKS];
bool network_authenticated[MAX_NETWORKS] = {false};
bool network_handshake_complete[MAX_NETWORKS] = {false};
unsigned long network_reconnect_cooldown[MAX_NETWORKS] = {0};
volatile float ui_scroll_y_interpolation = 0.0f;
volatile bool kb_interrupt_fired = false;
void IRAM_ATTR kb_isr() { kb_interrupt_fired = true; }
unsigned long last_input_time = 0;
unsigned long last_server_activity = 0;
String input_buffer;
int input_buffer_len = 0;
int input_buffer_cursor = 0;

// ==========================================
// 🔮 ASYNCHRONOUS ZERO-CPU LED TELEMETRY DESK
// ==========================================
void set_led_mode(uint8_t mode) {
    uint8_t r = 0, g = 0, b = 0;
    static unsigned long last_toggle[43] = {0};
    static bool flash_state[43] = {false};
    
    switch (mode) {
        case 0: // Mode 0: Inbound Private Message / Direct Query Alert (Flashing Cyan-White)
            if (millis() - last_toggle[mode] > 80) { flash_state[mode] = !flash_state[mode]; last_toggle[mode] = millis(); }
            if (flash_state[mode]) { r = 40; g = 95; b = 95; } // Piercing high-visibility flash completely separate from gold/orange/red rails
            break;
        case 1:  b = (sin(millis() / 400.0) + 1.0) * 12; g = b / 3; break; // Mode 1: Healthy Idle Cyan Heartbeat Wave (de-conflicted 400ms)
        case 2:  // Mode 2: Global Real-Time Mention (Purple Double-Flash Trigger)
            if (millis() - last_toggle[mode] > 150) { flash_state[mode] = !flash_state[mode]; last_toggle[mode] = millis(); }
            if (flash_state[mode]) { r = 40; b = 40; } break;
        case 3:  r = 30; g = 25; break; // Mode 3: Socket Packet Ingestion (Yellow Blip)
        case 4:  g = 40; break; // Mode 4: SD Storage Sector Append (Emerald Green Burst)
        case 5:  r = 95; g = 95; b = 95; break; // Mode 5: TextBox Underflow Boundary (High-Contrast Pure White Strobe)
        case 6:  // Mode 6: Battery Critical Alert (Crimson Breathe Wave)
            r = (sin(millis() / 200.0) + 1.0) * 20; break;
        case 7:  // Mode 7: Tab Memory Ceiling Barrier (Alternating Teal/Red Double Strobe) - de-conflicted
            if (millis() - last_toggle[mode] > 180) { flash_state[mode] = !flash_state[mode]; last_toggle[mode] = millis(); }
            if (flash_state[mode]) { b = 60; g = 65; r = 5; } else { r = 75; g = 5; b = 5; } break;
        case 8:  r = 40; g = 20; b = 40; break; // Mode 8: Pinned channel indicator
        case 9:  r = 90; g = 12; break; // Mode 9: Wi-Fi Disconnect Fault (Solid Sharp Orange)
        case 10: r = 0; g = 0; b = 0; break; // Mode 10: Privacy Stealth Blackout Mode (Zero Dark Panel)
        case 11: // Mode 11: SD Card Mount Failure Error
            if (millis() - last_toggle[mode] > 100) { flash_state[mode] = !flash_state[mode]; last_toggle[mode] = millis(); }
            if (flash_state[mode]) { r = 90; } break;
        case 12: r = 60; b = 60; break; // Mode 12: Bouncer Socket Disconnected State (Solid Magenta)
        case 13: // Mode 13: Bouncer Identification Sequence (Slow Ice Blue Pulse)
            b = (sin(millis() / 400.0) + 1.0) * 20; r = b / 4; break;
        case 14: b = 80; break; // Mode 14: Secure SSL Encryption Handshake (Deep Royal Blue Blip)
        case 15: // Mode 15: Unread Highlight Mention Alarm Strobe (Vibrant Pulsing Gold)
            r = (sin(millis() / 150.0) + 1.0) * 40; g = r / 1.5; b = 0; break;
        case 16: r = 25; g = 25; b = 25; break; // Mode 16: Server Connection Lag Warning (Soft White)
        case 17: // Mode 17: Active Handshake Channel Synchronization (Vibrant Flashing Neon Emerald Green)
            if (millis() - last_toggle[mode] > 100) { flash_state[mode] = !flash_state[mode]; last_toggle[mode] = millis(); }
            if (flash_state[mode]) { g = 95; r = 0; b = 0; } // Intense green flash cuts straight through cyan/blue washing completely
            break;
        case 18: r = 10; g = 80; b = 10; break; // Mode 18: System Mode / App State Tab Change (de-conflicted lime, solid not flash)
        case 19: g = 60; b = 20; break; // Mode 19: Configuration Auto-Saver File Trigger (Lime Green Blip)
        case 20: r = 60; g = 15; break; // Mode 20: Local Slash Command Macro Instruction (Warm Coral Strobe)
        case 21: r = 20; b = 50; break; // Mode 21: Tab Key Username Autocomplete Success (Cool Indigo Flare)
        case 22: // Mode 22: Critical RSSI Signal Drop Alert (Urgent Flashing Amber)
            if (millis() - last_toggle[mode] > 220) { flash_state[mode] = !flash_state[mode]; last_toggle[mode] = millis(); }
            if (flash_state[mode]) { r = 85; g = 55; b = 0; } break; // De-conflicted amber 220ms vs teal 180ms vs heartbeat 400ms
        case 23: r = 50; g = 50; b = 50; break; // Mode 23: Inbound Server Keep-Alive / CAP Sync Activity (Bright Pearl Strobe)
        case 24: r = 0; g = 40; b = 40; break; // Mode 24: Topic update (332/TOPIC) cyan blip
        case 25: r = 40; g = 0; b = 40; break; // Mode 25: Vault LRU write
        case 26: r = 25; g = 25; b = 25; break; // Mode 26: Session save
        case 27: r = 30; g = 20; b = 20; break; // Mode 27: Input history push
        case 28: r = 40; g = 30; b = 0; break; // Mode 28: Filter active steady gold
        case 29: r = 20; g = 35; b = 45; break; // Mode 29: Nicklist drawer
        case 30: r = 40; g = 25; b = 10; break; // Mode 30: History recall ;/.
        case 31: r = 20; g = 20; b = 20; break; // Mode 31: spare dim
        case 32: r = 30; g = 30; b = 30; break; // Mode 32: spare dim
        case 33: // Mode 33: Socket Connection Retry Backoff Interval (Paced Flashing Pure Red)
            if (millis() - last_toggle[mode] > 500) { flash_state[mode] = !flash_state[mode]; last_toggle[mode] = millis(); }
            if (flash_state[mode]) { r = 90; g = 0; b = 0; } break;
        case 34: r = 95; g = 15; b = 0; break; // Mode 34: Heap Memory Fragmentation Warning (Hot Burning Crimson Orange)
        case 35: r = 90; b = 50; g = 0; break; // Mode 35: Line Input Character Truncation (Bright Pink Flash)
        
        case 36: r = 40; g = 70; b = 70; break; // Mode 36: Socket Keep-Alive Dropped / Timeout (Solid Soft Cyan-White)
        
        case 55: // Mode 55: Low Battery Warning Override (Solid Pure Red)
            // Gently pulse GPIO 38 just long enough to blit the color register safely down the pixel line
            r = 95; g = 0; b = 0; 
            break;
        case 37: // Mode 37: Configuration Corrupt / Default Restore (Flashing Laser Red/Yellow)
            if (millis() - last_toggle[mode] > 150) { flash_state[mode] = !flash_state[mode]; last_toggle[mode] = millis(); }
            if (flash_state[mode]) { r = 90; g = 0; b = 0; } else { r = 70; g = 70; b = 0; } break;
        case 38: r = 90; g = 12; b = 0; break; // Mode 38: Vault passive scan blink (vs 9 solid)
        case 39: // Mode 39: WDT pre-warn (flash 200ms)
            if (millis() - last_toggle[mode] > 200) { flash_state[mode] = !flash_state[mode]; last_toggle[mode] = millis(); }
            if (flash_state[mode]) { r = 70; g = 0; b = 0; } break;
        case 40: r = 30; g = 20; b = 60; break; // Mode 40: Rotation lock / auto-rotate blip
        case 41: r = 0; g = 60; b = 0; break; // Mode 41: Vault scan match
        case 42: r = 20; g = 40; b = 40; break; // Mode 42: Session/history restore
            
        default: r = 30; g = 30; b = 30; break;
    }
    // LED on Cardputer Adv is hardware-tied to backlight rail (GPIO38). Hardware PWM at
    // 80/100/150 caused WS2812 VCC ripple -> flicker (even at normal 80). Keep hardware
    // rail at 255 for stable supply and do dimming in software via gamma, so heartbeat
    // stays smooth. Floors keep idle visible even when display is logically dimmed.
    {
        float br = g_backlight_level / 255.0f;
        if (br < 0) br = 0; if (br > 1) br = 1;
        br = powf(br, 2.2f);
        float floor = 0.40f;
        if (mode == 1) floor = 0.60f; // idle heartbeat
        if (mode == 6 || mode == 55 || mode == 9 || mode == 22 || mode == 7 || mode == 15) floor = 0.75f;
        if (br < floor) br = floor;
        r = (uint8_t)(r * br);
        g = (uint8_t)(g * br);
        b = (uint8_t)(b * br);
    }
    neopixelWrite(21, r, g, b); // Deliver bits down to physical Pin 21
}

// ==========================================
// 💾 FILE SYSTEM AND STREAM CONFIGURATION PARSER
// ==========================================
void update_config_string(char* destination, const char* source, size_t max_len);
bool is_mention(const char* msg, const char* nick);
void log_system(const char* fmt, ...);
void sync_ntp_timezone();
// SD Wi-Fi Vault helpers
void load_wifi_vault_from_sd();
void save_wifi_vault_lru(const char* ssid, const char* pass);
void handle_vault_scan_complete();
// Session state helpers
void save_session_state();
void load_session_state();
// TWDT emergency flush
void wdt_emergency_flush();

void load_settings_from_sd() {
    if (safe_mode_active) return;
    
    File file = SD.open("/irc/config.txt", FILE_READ);
    if (!file) {
        set_led_mode(37); // Flash Red/Yellow immediately to warn of missing or unreadable micro-SD configuration tables
        Serial.println("[ERROR] /irc/config.txt not found. Falling back to default flags.");
        return;
    }
    
    char lineBuf[128];
    while (file.available()) {
        yield(); // Crucial hardware watchdog protection inside file read loop
        int len = file.readBytesUntil('\n', lineBuf, sizeof(lineBuf)-1);
        if (len <= 0) {
            // consume single byte if readBytesUntil failed on empty line
            if (file.available()) file.read();
            continue;
        }
        lineBuf[len] = '\0';
        // strip trailing \r
        if (len > 0 && lineBuf[len-1] == '\r') lineBuf[len-1] = '\0';
        // trim leading whitespace
        char *start = lineBuf;
        while (*start==' '||*start=='\t') start++;
        if (*start=='\0' || *start=='#') continue;
        // trim trailing whitespace
        char *end = start + strlen(start) - 1;
        while (end > start && (*end==' '||*end=='\t')) { *end='\0'; end--; }
        char *eq = strchr(start, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = start;
        char *value = eq + 1;
        while (*key==' '||*key=='\t') key++;
        char *ke = key + strlen(key) - 1;
        while (ke >= key && (*ke==' '||*ke=='\t')) { *ke='\0'; ke--; }
        while (*value==' '||*value=='\t') value++;
        char *ve = value + strlen(value) - 1;
        while (ve >= value && (*ve==' '||*ve=='\t')) { *ve='\0'; ve--; }
        // Exact Token Matching Pipeline (no String heap)
        if (strcmp(key, "wifi_ssid")==0) update_config_string(wifi_ssid, value, sizeof(wifi_ssid));
        else if (strcmp(key, "wifi_pass")==0) update_config_string(wifi_pass, value, sizeof(wifi_pass));
        else if (strcmp(key, "irc_nick")==0) update_config_string(irc_nick, value, sizeof(irc_nick));
        else if (strcmp(key, "bnc_host")==0) update_config_string(bnc_host, value, sizeof(bnc_host));
        else if (strcmp(key, "bnc_port")==0) { bnc_port = atoi(value); }
        else if (strcmp(key, "bnc_user")==0) update_config_string(bnc_user, value, sizeof(bnc_user));
        else if (strcmp(key, "bnc_pass")==0) update_config_string(bnc_pass, value, sizeof(bnc_pass));
        else if (strcmp(key, "channel_log_enabled")==0) channel_log_enabled = atoi(value);
        else if (strcmp(key, "screen_brightness")==0) screen_brightness = atoi(value);
        else if (strcmp(key, "current_tz_idx")==0) current_tz_idx = atoi(value);
        else if (strcmp(key, "use_12_hour_format")==0) use_12_hour_format = atoi(value);
        else if (strcmp(key, "use_dst")==0) use_dst = atoi(value);
        else if (strcmp(key, "use_light_theme")==0) use_light_theme = atoi(value);
        else if (strcmp(key, "theme_accent")==0) theme_accent = atoi(value);
        else if (strcmp(key, "text_scale")==0) text_scale = atoi(value);
        else if (strcmp(key, "speaker_enabled")==0) speaker_enabled = atoi(value);
    }
    // Critical parameters validation – fire Mode 37 if bouncer host/port or wifi remains unassigned
    if (strlen(wifi_ssid) == 0 || strlen(bnc_host) == 0 || bnc_port == 0) {
        set_led_mode(37); // Configuration corrupt / default restore alarm
    }
    file.close();
    Serial.println("[STORAGE] Configuration fields successfully streamed and parsed from SD.");
}

void sync_new_nick_to_sd(const char* new_nick) {
    if (safe_mode_active) return;
    strncpy(irc_nick, new_nick, sizeof(irc_nick) - 1);
    bool sd_locked = false;
    if (sd_mutex) sd_locked = (xSemaphoreTake(sd_mutex, pdMS_TO_TICKS(100)) == pdTRUE);
    else sd_locked = true;
    if (!sd_locked) return;
    File file = SD.open("/irc/config.txt", FILE_WRITE);
    if (!file) { if (sd_mutex) xSemaphoreGive(sd_mutex); return; }
    file.printf("wifi_ssid=%s\nwifi_pass=%s\nirc_nick=%s\n", wifi_ssid, wifi_pass, irc_nick);
    file.printf("channel_log_enabled=%d\nscreen_brightness=%d\n", channel_log_enabled, screen_brightness);
    file.printf("current_tz_idx=%d\nuse_12_hour_format=%d\nuse_dst=%d\nuse_light_theme=%d\ntheme_accent=%d\ntext_scale=%d\nspeaker_enabled=%d\nbnc_host=%s\n", current_tz_idx, use_12_hour_format, use_dst, use_light_theme, theme_accent, text_scale, speaker_enabled, bnc_host);
    file.printf("bnc_port=%d\nbnc_user=%s\nbnc_pass=%s\n", bnc_port, bnc_user, bnc_pass);
    file.close();
    if (sd_mutex) xSemaphoreGive(sd_mutex);
    Serial.println("[STORAGE-SYNC] New nick permanently synchronized to micro-SD config.");
}

void purge_old_logs() {
    if (safe_mode_active) return;
    Serial.println("[STORAGE] Launching automated 7-day log cleanup pass...");
    // System logs 30d purge
    File sdir = SD.open("/irc/system");
    if (sdir && sdir.isDirectory()) {
        File sf = sdir.openNextFile();
        while(sf){ 
            // keep 30d: if file older than 30 days (approx via name date), or >512KB, remove
            if(!sf.isDirectory() && sf.size()>512000){ String p=String(sf.path()); sf.close(); SD.remove(p.c_str()); }
            else sf.close();
            sf = sdir.openNextFile();
        }
        sdir.close();
    }
    File dir = SD.open("/irc/logs");
    if (!dir || !dir.isDirectory()) {
        SD.mkdir("/irc/logs"); // Force self-heal and mount missing tracking folders on boot
        return;
    }
    
    File file = dir.openNextFile();
    while (file) {
        yield(); // Hardware watchdog timer starvation safeguard inside file loops
        
        // Simple linear rolling recovery check: if file tables reporting size footprints 
        // exceeding normal field limits, run manual truncation resets to free block sectors
        if (!file.isDirectory() && file.size() > 512000) { // Hard 500KB cap per channel window log
            String dead_path = file.path();
            file.close();
            SD.remove(dead_path.c_str()); // Erase maxed out log chunk cleanly
            Serial.printf("[PURGE] Erased maxed log file to reclaim card blocks: %s\n", dead_path.c_str());
        } else {
            file.close();
        }
        file = dir.openNextFile();
    }
    dir.close();
}

void append_line_to_sd_log(const char* tab_name, const char* nick, const char* message) {
    // Unified to 512B cache path /irc/logs/<server>/<room>_YYYY_MM_DD.log - keep as no-op to avoid duplicate collision
    (void)tab_name; (void)nick; (void)message;
    return;
}


// Lightweight 512-byte static line cache for high-speed log write caching
static char log_sector_cache[512] = {0};
static int log_sector_cache_len = 0;
static char log_sector_current_path[128] = {0};

void flush_log_cache() {
    if (log_sector_cache_len == 0 || log_sector_current_path[0] == '\0') return;
    // Use sd_mutex if available - tryTake defer to avoid 50ms stall during PRIVMSG flood
    bool sd_locked = false;
    if (sd_mutex) sd_locked = (xSemaphoreTake(sd_mutex, pdMS_TO_TICKS(5)) == pdTRUE);
    else sd_locked = true;
    if (!sd_locked) return; // defer to next loop
    else sd_locked = true;
    if (sd_locked) {
        File f = SD.open(log_sector_current_path, FILE_APPEND);
        if (f) {
            f.write((uint8_t*)log_sector_cache, log_sector_cache_len);
            f.close();
            set_led_mode(4);
        } else {
            set_led_mode(28);
        }
        if (sd_mutex) xSemaphoreGive(sd_mutex);
    }
    memset(log_sector_cache, 0, sizeof(log_sector_cache));
    log_sector_cache_len = 0;
}

// Emergency TWDT flush - called from watchdog ISR before hardware restart
void wdt_emergency_flush() {
    // Short non-blocking window, flush RAM cache to /irc/logs/ before S3 restart
    log_system("WDT panic flush");
    flush_log_cache();
}
void log_system(const char* fmt, ...) {
    // always log to SD, even in safe_mode (system logs are critical)
    char buf[160];
    va_list args; va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    unsigned long sec = millis()/1000 + adj_time;
    char path[64];
    struct tm ti;
    if (getLocalTime(&ti, 30)) {
        snprintf(path, sizeof(path), "/irc/system/system_%04d_%02d_%02d.log", ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday);
    } else {
        uint32_t yr=2026, mon=((sec/2629743)%12)+1, day=((sec/86400)%31)+1;
        snprintf(path, sizeof(path), "/irc/system/system_%04d_%02d_%02d.log", yr, mon, day);
    }
    bool sd_locked=false;
    if (sd_mutex) sd_locked=(xSemaphoreTake(sd_mutex, pdMS_TO_TICKS(50))==pdTRUE); else sd_locked=true;
    if (!sd_locked) return;
    SD.mkdir("/irc");
    SD.mkdir("/irc/system");
    File f=SD.open(path, FILE_APPEND);
    if(f){ f.printf("[%02d:%02d] %s\n", (int)(sec/3600)%24, (int)(sec%3600)/60, buf); f.close(); }
    if(sd_mutex) xSemaphoreGive(sd_mutex);
}

// ==========================================
// 📶 SD WI-FI VAULT ( /irc/wifi_cache.txt ) - 5x SSID:PASS, NVS-free
// ==========================================
void load_wifi_vault_from_sd() {
    wifi_vault_count = 0;
    for (int i=0;i<5;i++){ wifi_vault_ssid[i][0]='\0'; wifi_vault_pass[i][0]='\0'; }
    if (safe_mode_active) return;
    bool sd_locked = false;
    if (sd_mutex) sd_locked = (xSemaphoreTake(sd_mutex, pdMS_TO_TICKS(50)) == pdTRUE);
    else sd_locked = true;
    if (!sd_locked) return;
    File f = SD.open("/irc/wifi_cache.txt", FILE_READ);
    if (!f) { if (sd_mutex) xSemaphoreGive(sd_mutex); return; }
    char line[130];
    int idx=0;
    while (f.available() && idx<5) {
        int len = f.readBytesUntil('\n', line, sizeof(line)-1);
        if (len<=0){ if(f.available()) f.read(); continue; }
        line[len]='\0';
        if (len>0 && line[len-1]=='\r') line[len-1]='\0';
        char *p=line; while(*p==' '||*p=='\t') p++;
        if (*p=='\0' || *p=='#') continue;
        char *colon=strchr(p, ':');
        if (!colon) continue;
        *colon='\0';
        char *ssid=p; char *pass=colon+1;
        while(*ssid==' '||*ssid=='\t') ssid++;
        char *se=ssid+strlen(ssid)-1; while(se>=ssid && (*se==' '||*se=='\t')){*se='\0'; se--;}
        while(*pass==' '||*pass=='\t') pass++;
        char *pe=pass+strlen(pass)-1; while(pe>=pass && (*pe==' '||*pe=='\t')){*pe='\0'; pe--;}
        if (strlen(ssid)==0) continue;
        strncpy(wifi_vault_ssid[idx], ssid, 63); wifi_vault_ssid[idx][63]='\0';
        strncpy(wifi_vault_pass[idx], pass, 63); wifi_vault_pass[idx][63]='\0';
        idx++;
    }
    wifi_vault_count=idx;
    f.close();
    if (sd_mutex) xSemaphoreGive(sd_mutex);
    // Keep Y=120 wide-open: this is background SD-only, no canvas draw
}

void save_wifi_vault_lru(const char* ssid, const char* pass) {
    if (!ssid || strlen(ssid)==0) return;
    // Transient dedup + shift: new at slot 0, drop oldest 5th
    char tmp_ssid[5][64]={{0}};
    char tmp_pass[5][64]={{0}};
    int tmp_cnt=0;
    strncpy(tmp_ssid[tmp_cnt], ssid, 63);
    strncpy(tmp_pass[tmp_cnt], pass?pass:"", 63);
    tmp_cnt++;
    for(int i=0;i<wifi_vault_count && tmp_cnt<5;i++){
        if(strcmp(wifi_vault_ssid[i], ssid)==0) continue; // dedup
        strncpy(tmp_ssid[tmp_cnt], wifi_vault_ssid[i], 63);
        strncpy(tmp_pass[tmp_cnt], wifi_vault_pass[i], 63);
        tmp_cnt++;
    }
    // Short block window to protect against power fluctuations
    bool sd_locked=false;
    if(sd_mutex) sd_locked=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(100))==pdTRUE);
    else sd_locked=true;
    if(!sd_locked) return;
    SD.mkdir("/irc");
    File f=SD.open("/irc/wifi_cache.txt", FILE_WRITE);
    if(f){
        for(int i=0;i<tmp_cnt;i++){
            f.printf("%s:%s\n", tmp_ssid[i], tmp_pass[i]);
        }
        f.close();
    }
    if(sd_mutex) xSemaphoreGive(sd_mutex);
    queueLed(25, 600);
    // Refresh RAM copy
    load_wifi_vault_from_sd();
}

void handle_vault_scan_complete() {
    int n = WiFi.scanComplete();
    if (n < 0) return; // -2 scanning, -1 idle
    // Compare discovered SSIDs against vault history
    for(int i=0;i<wifi_vault_count;i++){
        for(int s=0;s<n;s++){
            String seen = WiFi.SSID(s);
            if(seen == wifi_vault_ssid[i]){
                Serial.printf("[VAULT-ROAM] Matched historical SSID: %s\n", wifi_vault_ssid[i]);
                WiFi.scanDelete();
                queueLed(41, 800);
                WiFi.begin(wifi_vault_ssid[i], wifi_vault_pass[i]);
                failover_in_progress=false;
                wifi_drop_timestamp=0;
                return;
            }
        }
    }
    WiFi.scanDelete();
}

// ==========================================
// 💾 SESSION STATE ( /irc/session_state.tmp ) - <12 bytes: net chan scrollFlag
// ==========================================
void save_session_state() {
    if (safe_mode_active) return;
    if (millis() - last_session_write_tick < 2000) return; // 2s debounce protects SPI bus while hotkey held
    last_session_write_tick = millis();
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d %d %d", (int)nav_server_select_idx, (int)current_tab_index, is_scrollback_active?1:0);
    if(n<=0 || n>= (int)sizeof(buf)) return;
    bool sd_locked=false;
    if(sd_mutex) sd_locked=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(50))==pdTRUE);
    else sd_locked=true;
    if(!sd_locked) return;
    SD.mkdir("/irc");
    File f=SD.open("/irc/session_state.tmp", FILE_WRITE);
    if(f){ f.print(buf); f.close(); queueLed(26, 400); }
    if(sd_mutex) xSemaphoreGive(sd_mutex);
    // Y=120-135 / 225-240 textbox left empty - background file op only
}
void load_session_state() {
    if (safe_mode_active) return;
    bool sd_locked=false;
    if(sd_mutex) sd_locked=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(50))==pdTRUE);
    else sd_locked=true;
    if(!sd_locked) return;
    File f=SD.open("/irc/session_state.tmp", FILE_READ);
    if(!f){ if(sd_mutex) xSemaphoreGive(sd_mutex); return; }
    char buf[16]={0};
    int len=f.readBytes(buf, sizeof(buf)-1);
    f.close();
    if(sd_mutex) xSemaphoreGive(sd_mutex);
    if(len<=0) return;
    buf[len]='\0';
    int netIdx=-1, chanIdx=-1, sFlag=-1;
    if(sscanf(buf, "%d %d %d", &netIdx, &chanIdx, &sFlag)!=3) return;
    // Safe boot interception: snap pointer indexes bypassing home
    if(netIdx>=0 && netIdx < 5) nav_server_select_idx=netIdx;
    if(chanIdx>=0 && chanIdx < MAX_TABS && chanIdx < gTabCount) current_tab_index=chanIdx;
    if(sFlag==0 || sFlag==1){ is_scrollback_active=(sFlag==1); scrollback_mode_active=is_scrollback_active; scrollback_offset=is_scrollback_active? scrollback_offset:0; scrollback_offset_idx=scrollback_offset; }
    ui_needs_redraw=true;
    queueLed(42, 600);
    Serial.printf("[SESSION] Restored net=%d chan=%d scroll=%d\n", netIdx, chanIdx, sFlag);
}

void sync_ntp_timezone() {
    if (WiFi.status() != WL_CONNECTED) return;
    long gmt = (long)current_tz_idx * 3600L;
    long dst = use_dst ? 3600L : 0L;
    configTime(gmt, dst, "pool.ntp.org", "time.nist.gov", "time.google.com");
}
void load_ignore_list() {
    ignore_count=0;
    bool sd_locked=false;
    if(sd_mutex) sd_locked=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(50))==pdTRUE); else sd_locked=true;
    if(!sd_locked) return;
    File f=SD.open("/irc/ignore.txt", FILE_READ);
    if(!f){ if(sd_mutex) xSemaphoreGive(sd_mutex); return; }
    char line[16];
    while(f.available() && ignore_count<8){
        int len=f.readBytesUntil('\n', line, sizeof(line)-1);
        if(len<=0){ if(f.available()) f.read(); continue; }
        line[len]='\0'; if(len>0 && line[len-1]=='\r') line[len-1]='\0';
        if(!*line) continue;
        strncpy(ignore_list[ignore_count], line,15); ignore_list[ignore_count][15]='\0';
        ignore_count++;
    }
    f.close(); if(sd_mutex) xSemaphoreGive(sd_mutex);
}
void save_ignore_list() {
    bool sd_locked=false;
    if(sd_mutex) sd_locked=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(50))==pdTRUE); else sd_locked=true;
    if(!sd_locked) return;
    SD.mkdir("/irc");
    File f=SD.open("/irc/ignore.txt", FILE_WRITE);
    if(f){ for(int i=0;i<ignore_count;i++) f.println(ignore_list[i]); f.close(); }
    if(sd_mutex) xSemaphoreGive(sd_mutex);
}
bool is_ignored(const char* nick){
    if(!nick) return false;
    for(int i=0;i<ignore_count;i++) if(strcasecmp(ignore_list[i], nick)==0) return true;
    return false;
}
bool is_highlight_word(const char* msg){
    if(!msg) return false;
    char lowMsg[128]; strncpy(lowMsg, msg,127); lowMsg[127]='\0'; for(char*p=lowMsg;*p;p++) *p=tolower(*p);
    for(int i=0;i<highlight_count;i++){
        const char* w=highlight_words[i];
        size_t wl=strlen(w);
        if(wl==0) continue;
        char lowW[32]; strncpy(lowW, w,31); lowW[31]='\0'; for(char*p=lowW;*p;p++) *p=tolower(*p);
        const char* star=strchr(lowW, '*');
        if(star){
            size_t pre=star-lowW;
            if(pre>0 && strncmp(lowMsg, lowW, pre)==0) return true;
            const char* suf=star+1;
            if(*suf && strstr(lowMsg, suf)) return true;
        } else {
            if(strstr(lowMsg, lowW)) return true;
        }
    }
    return false;
}
void load_highlight_list(){
    highlight_count=0;
    bool sd_locked=false;
    if(sd_mutex) sd_locked=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(50))==pdTRUE); else sd_locked=true;
    if(!sd_locked) return;
    File f=SD.open("/irc/highlight.txt", FILE_READ);
    if(!f){ if(sd_mutex) xSemaphoreGive(sd_mutex); return; }
    char line[32];
    while(f.available() && highlight_count<8){
        int len=f.readBytesUntil('\n', line, sizeof(line)-1);
        if(len<=0){ if(f.available()) f.read(); continue; }
        line[len]='\0'; if(len>0 && line[len-1]=='\r') line[len-1]='\0';
        if(!*line) continue;
        strncpy(highlight_words[highlight_count], line,31); highlight_words[highlight_count][31]='\0';
        highlight_count++;
    }
    f.close(); if(sd_mutex) xSemaphoreGive(sd_mutex);
}
void save_highlight_list(){
    bool sd_locked=false;
    if(sd_mutex) sd_locked=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(50))==pdTRUE); else sd_locked=true;
    if(!sd_locked) return;
    SD.mkdir("/irc");
    File f=SD.open("/irc/highlight.txt", FILE_WRITE);
    if(f){ for(int i=0;i<highlight_count;i++) f.println(highlight_words[i]); f.close(); }
    if(sd_mutex) xSemaphoreGive(sd_mutex);
}
void load_alias_list(){
    alias_count=0;
    bool sd_locked=false;
    if(sd_mutex) sd_locked=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(50))==pdTRUE); else sd_locked=true;
    if(!sd_locked) return;
    File f=SD.open("/irc/alias.txt", FILE_READ);
    if(!f){ if(sd_mutex) xSemaphoreGive(sd_mutex); return; }
    char line[96];
    while(f.available() && alias_count<5){
        int len=f.readBytesUntil('\n', line, sizeof(line)-1);
        if(len<=0){ if(f.available()) f.read(); continue; }
        line[len]='\0'; if(len>0 && line[len-1]=='\r') line[len-1]='\0';
        char *eq=strchr(line,'=');
        if(!eq) continue; *eq='\0';
        char *k=line; while(*k==' '||*k=='\t') k++; char *ke=k+strlen(k)-1; while(ke>=k && (*ke==' '||*ke=='\t')){*ke='\0'; ke--;}
        char *v=eq+1; while(*v==' '||*v=='\t') v++; char *ve=v+strlen(v)-1; while(ve>=v && (*ve==' '||*ve=='\t')){*ve='\0'; ve--;}
        if(!*k||!*v) continue;
        strncpy(alias_names[alias_count], k,15); alias_names[alias_count][15]='\0';
        strncpy(alias_cmds[alias_count], v,63); alias_cmds[alias_count][63]='\0';
        alias_count++;
    }
    f.close(); if(sd_mutex) xSemaphoreGive(sd_mutex);
}
void save_alias_list(){
    bool sd_locked=false;
    if(sd_mutex) sd_locked=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(50))==pdTRUE); else sd_locked=true;
    if(!sd_locked) return;
    SD.mkdir("/irc");
    File f=SD.open("/irc/alias.txt", FILE_WRITE);
    if(f){ for(int i=0;i<alias_count;i++) f.printf("%s=%s\n", alias_names[i], alias_cmds[i]); f.close(); }
    if(sd_mutex) xSemaphoreGive(sd_mutex);
}
const char* expand_alias(const char* cmd){
    for(int i=0;i<alias_count;i++) if(strcasecmp(alias_names[i], cmd)==0) return alias_cmds[i];
    return nullptr;
}
void push_input_history(const char* txt) {
    if (!txt || !*txt || txt[0]=='/') return; // skip slash commands and empty for cleaner recall
    // dedup consecutive
    int last = (input_history_head -1 +10)%10;
    if (input_history_len>0 && strcmp(input_history[last], txt)==0) return;
    strncpy(input_history[input_history_head], txt, 127); input_history[input_history_head][127]='\0';
    input_history_head = (input_history_head+1)%10;
    if (input_history_len<10) input_history_len++;
    input_history_pos=-1;
    // persist to SD for launcher survival - short window below Y=120
    bool sd_locked=false;
    if(sd_mutex) sd_locked=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(50))==pdTRUE); else sd_locked=true;
    if(sd_locked){
        SD.mkdir("/irc");
        File f=SD.open("/irc/history.txt", FILE_WRITE);
        if(f){
            for(int i=0;i<input_history_len;i++){
                int idx=(input_history_head - input_history_len + i +10)%10;
                f.println(input_history[idx]);
            }
            f.close(); queueLed(27, 300);
        }
        if(sd_mutex) xSemaphoreGive(sd_mutex);
    }
}
void load_input_history() {
    bool sd_locked=false;
    if(sd_mutex) sd_locked=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(50))==pdTRUE); else sd_locked=true;
    if(!sd_locked) return;
    File f=SD.open("/irc/history.txt", FILE_READ);
    if(!f){ if(sd_mutex) xSemaphoreGive(sd_mutex); return; }
    char line[128];
    input_history_len=0; input_history_head=0;
    while(f.available() && input_history_len<10){
        int len=f.readBytesUntil('\n', line, sizeof(line)-1);
        if(len<=0){ if(f.available()) f.read(); continue; }
        line[len]='\0'; if(len>0 && line[len-1]=='\r') line[len-1]='\0';
        if(!*line) continue;
        strncpy(input_history[input_history_head], line,127);
        input_history_head=(input_history_head+1)%10; input_history_len++;
    }
    f.close(); if(sd_mutex) xSemaphoreGive(sd_mutex);
    input_history_pos=-1;
}

void add_message_to_buffer(const char* source, const char* msg, uint16_t color, const char* timeStr = "00:00") {
    if (is_ignored(source)) return;
    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        int target_idx = (gTabCount > 0 && current_tab_index < gTabCount) ? current_tab_index : 0;
        Tab &t = gTabs[target_idx];
        
        // Auto-Scrolling Rolling Viewport Shifter Engine (memmove)
        if (t.line_count >= MSG_BUFFER_SIZE) {
            memmove(&t.lines[0], &t.lines[1], (MSG_BUFFER_SIZE-1)*sizeof(ChatLine));
            t.line_count = MSG_BUFFER_SIZE - 1; // Open up the absolute bottom slot row for our incoming text
        }
        // Heap guard: log largest free block <20KB
        if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 20000) {
            int largest=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            Serial.printf("[HEAP] largest free %d <20KB\n", largest);
            log_system("HEAP largest %d <20KB", largest);
            set_led_mode(34);
        }
        
        ChatLine &cl = t.lines[t.line_count];
        if (timeStr) strncpy(cl.timeStr, timeStr, sizeof(cl.timeStr)-1);
        else strncpy(cl.timeStr, "00:00", sizeof(cl.timeStr)-1);
        strncpy(cl.nick, source, sizeof(cl.nick)-1);
        strncpy(cl.message, msg, sizeof(cl.message)-1);
        cl.color = color;
        // Fixed: case-insensitive word-boundary mention, not server noise, muted suppress
        cl.is_highlight = is_mention(msg, irc_nick) && strcasecmp(source, "server")!=0 && strcasecmp(source, "ClientCore")!=0 && !is_ignored(source) && !t.muted;
        if (cl.is_highlight && speaker_enabled && sound_profile>=1) M5.Speaker.tone(800, 80);
        t.line_count++;
        // Preserve scrollback viewport: if user is scrolled back, keep view pinned by bumping offset
        if ((is_scrollback_active || scrollback_mode_active) && target_idx == current_tab_index) {
            int eff = t.line_count;
            if (eff > MSG_BUFFER_SIZE) eff = MSG_BUFFER_SIZE;
            // bump offset so new message doesn't shift visible window (unless already at live edge)
            if (scrollback_offset < eff - 1) {
                scrollback_offset++;
                scrollback_offset_idx = scrollback_offset;
            }
        } else {
            // no bounce animation - keep at 0 to avoid flicker on flood
            // ui_scroll_y_interpolation = 12.0f; // disabled for flicker
        }

    // Dynamic Session Log Rotator with High-Speed Write Caching (512-byte sector cache)
    if (channel_log_enabled == 1 && safe_mode_active == false) {
        unsigned long current_sync_sec = (millis() / 1000) + adj_time;
        uint32_t active_yr  = 2026; 
        uint32_t active_mon = ((current_sync_sec / 2629743) % 12) + 1; 
        uint32_t active_day = ((current_sync_sec / 86400) % 31) + 1;   

        char safe_server[32]={0}, safe_room[32]={0};
        strncpy(safe_server, t.server, 31); strncpy(safe_room, t.name, 31);
        for(char *p=safe_server;*p;p++) if(*p=='/'||*p=='\\'||*p=='#'||*p=='&'||*p=='~') *p='_';
        for(char *p=safe_room;*p;p++) if(*p=='/'||*p=='\\'||*p=='#'||*p=='&'||*p=='~') *p='_';
        // trim
        { char *s=safe_server; while(*s==' '||*s=='\t') s++; if(s!=safe_server) memmove(safe_server,s,strlen(s)+1);
          for(int i=strlen(safe_server)-1;i>=0 && (safe_server[i]==' '||safe_server[i]=='\t');i--) safe_server[i]='\0'; }
        { char *s=safe_room; while(*s==' '||*s=='\t') s++; if(s!=safe_room) memmove(safe_room,s,strlen(s)+1);
          for(int i=strlen(safe_room)-1;i>=0 && (safe_room[i]==' '||safe_room[i]=='\t');i--) safe_room[i]='\0'; }

        char dir_path_buffer[64] = {0};
        snprintf(dir_path_buffer, sizeof(dir_path_buffer), "/irc/logs/%s", safe_server);
        if (!SD.exists(dir_path_buffer)) SD.mkdir(dir_path_buffer);
        char file_path_buffer[128] = {0};
        snprintf(file_path_buffer, sizeof(file_path_buffer), "/irc/logs/%s/%s_%04d_%02d_%02d.log", safe_server, safe_room, active_yr, active_mon, active_day);

        // If path changed, flush previous cache first
        if (strcmp(log_sector_current_path, file_path_buffer) != 0) {
            if (log_sector_cache_len > 0) flush_log_cache();
            strncpy(log_sector_current_path, file_path_buffer, sizeof(log_sector_current_path)-1);
        }

        // Accumulate into 512-byte sector cache
        char line_buf[160] = {0};
        int line_len = snprintf(line_buf, sizeof(line_buf), "[%s] <%s>: %s\r\n", cl.timeStr, cl.nick, cl.message);
        if (log_sector_cache_len + line_len >= 512) {
            flush_log_cache();
            strncpy(log_sector_current_path, file_path_buffer, sizeof(log_sector_current_path)-1);
        }
        if (line_len < 512) {
            memcpy(log_sector_cache + log_sector_cache_len, line_buf, line_len);
            log_sector_cache_len += line_len;
        } else {
            // Fallback direct write for oversized line
            set_led_mode(4);
            File f = SD.open(file_path_buffer, FILE_APPEND);
            if (f) { f.write((uint8_t*)line_buf, line_len); f.close(); } else set_led_mode(28);
        }
        // Flush only on sector full, channel switch, or critical battery handled via external triggers
        if (log_sector_cache_len >= 512 - 160) flush_log_cache();
    }

        // Cross-Network Highlights Duplicator Pass - fixed: only true PRIVMSG mentions, not server/status noise
        if (is_mention(msg, irc_nick) && current_tab_index != 0 && strcasecmp(source, "server")!=0 && strcasecmp(source, "ClientCore")!=0) {
            Tab &mentions_tab = gTabs[0]; // Isolate Tab 0 explicitly
            if (mentions_tab.line_count >= MSG_BUFFER_SIZE) {
                memmove(&mentions_tab.lines[0], &mentions_tab.lines[1], (MSG_BUFFER_SIZE-1)*sizeof(ChatLine));
                mentions_tab.line_count = MSG_BUFFER_SIZE - 1;
            }
            ChatLine &ml = mentions_tab.lines[mentions_tab.line_count];
            if (timeStr) strncpy(ml.timeStr, timeStr, sizeof(ml.timeStr)-1);
            else strncpy(ml.timeStr, "00:00", sizeof(ml.timeStr)-1);
            // Prefix source context block clearly as: [Network]User
            snprintf(ml.nick, sizeof(ml.nick), "[%s]%s", t.server, source);
            strncpy(ml.message, msg, sizeof(ml.message)-1);
            ml.color = 0xFD20; // Pure high-visibility Amber
            ml.is_highlight = true;
            mentions_tab.line_count++;
        }
        append_line_to_sd_log(t.name, source, msg);
        xSemaphoreGive(irc_mutex);
        ui_needs_redraw = true;
    }

}

float get_calibrated_battery_percentage() {
    // Hardware API returns values directly in millivolts (e.g. 4200mV = Full, 3700mV = ~50%)
    int raw_mv = M5Cardputer.Power.getBatteryVoltage(); 
    
    if (raw_mv > 4200) raw_mv = 4200;
    if (raw_mv < 3300) raw_mv = 3300;
    
    // Calibrated millivolt discharge curve to perfectly track the Cardputer-Adv capacity parameters
    float percentage = 0.0f;
    if (raw_mv >= 4000) {
        percentage = 80.0f + (((float)(raw_mv - 4000) / (4200.0f - 4000.0f)) * 20.0f);
    } else if (raw_mv >= 3700) {
        percentage = 40.0f + (((float)(raw_mv - 3700) / (4000.0f - 3700.0f)) * 40.0f);
    } else if (raw_mv >= 3500) {
        percentage = 15.0f + (((float)(raw_mv - 3500) / (3700.0f - 3500.0f)) * 25.0f);
    } else {
        percentage = ((float)(raw_mv - 3300) / (3500.0f - 3300.0f)) * 15.0f;
    }
    
    // Smooth out micro-fluctuations over a running alpha-filter to prevent terminal display jitter
    static float smoothed_pct = percentage;
    smoothed_pct = (smoothed_pct * 0.90f) + (percentage * 0.10f);
    
    if (smoothed_pct > 100.0f) smoothed_pct = 100.0f;
    if (smoothed_pct < 0.0f) smoothed_pct = 0.0f;
    
    return smoothed_pct;
}

uint16_t get_nick_palette_color(const char* nick) {
    uint32_t hash = 5381;
    while (*nick) { hash = ((hash << 5) + hash) + *nick++; }
    const uint16_t palette[] = {0x07FF, 0xFDA0, 0xF81F, 0x07E0, 0xAFE5, 0xFED0, 0x867F}; 
    return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}
bool is_nick_char(char c){ return isalnum((unsigned char)c) || c=='_' || c=='-' || c=='[' || c==']' || c=='{' || c=='}' || c=='^' || c=='|' || c=='\\' || c=='`'; }
bool is_mention(const char* msg, const char* nick) {
    if (is_highlight_word(msg)) return true;
    if (!msg || !nick || !*nick) return false;
    size_t nlen = strlen(nick);
    if (nlen==0) return false;
    for (const char* p = msg; *p; p++) {
        if (strncasecmp(p, nick, nlen)==0) {
            bool preOk = (p==msg) || !is_nick_char(p[-1]);
            bool postOk = p[nlen]=='\0' || !is_nick_char(p[nlen]);
            if (preOk && postOk) return true;
        }
    }
    return false;
}

        // Explicit configuration safety assignment macro passes
        void update_config_string(char* destination, const char* source, size_t max_len) {
            if (destination == nullptr || source == nullptr || max_len == 0) return;
            strncpy(destination, source, max_len - 1);
            destination[max_len - 1] = '\0'; // Force a rigid null-terminator boundary safety seal
        }

// ==========================================
// 🎬 RETRO-TERMINAL GRAPHICS RENDERING ENGINE
// ==========================================
void draw_chat_view() {
    if (!ui_needs_redraw && ui_scroll_y_interpolation == 0.0f) return;
    canvas.setTextSize(text_scale);
    // Fluid geometry anchored to active rotation (135x240 vertical vs 240x135 landscape)
    int display_width = M5Cardputer.Display.width();
    int display_height = M5Cardputer.Display.height();
    bool is_vertical = (display_width < display_height);
    int navbar_clamp_x = is_vertical ? 65 : 115;
    int rssi_anchor_x = is_vertical ? 85 : 120;
    int battery_anchor_x = is_vertical ? 110 : 225;
    int wire_y = is_vertical ? 224 : 121;
    int input_box_y = is_vertical ? 225 : 121;
    // Disable scroll interpolation animation (was 97+interpolation causing 12px bounce flicker on every new message / flood)
    int baseline_y = is_vertical ? 208 : 97;
    int viewport_top_y = 12;
    int textbox_height = is_vertical ? (display_height - input_box_y) : 14;

    // ==========================================
    // STATE 1: WORKSPACE NAVIGATION HUB (Fn + P)
    // ==========================================
    if (current_app_mode == MODE_NAVIGATOR) {
        // Clear fluid frame
        canvas.fillSprite(0x0000); 
        int left_w = is_vertical ? (display_width/2 - 1) : 114;
        int right_x = is_vertical ? (display_width/2 + 1) : 117;
        int right_w = is_vertical ? (display_width - right_x) : 123;
        int split_x = is_vertical ? (display_width/2) : 116;
        canvas.fillRect(0, 0, left_w, display_height, 0x0841);
        canvas.fillRect(right_x, 0, right_w, display_height, 0x0000);
        canvas.drawFastVLine(split_x, 0, display_height, 0x7BEF);
        
        canvas.setTextColor(0x07E0); canvas.setCursor(6, 6); canvas.print("NETWORKS");
        
        canvas.setTextColor(nav_server_select_idx == 0 ? 0xFFFF : 0x7BEF);
        canvas.setCursor(6, 24); canvas.printf("%s [ALL NETS]", (nav_server_select_idx == 0 ? ">" : " "));
        for (int i = 0; i < discovered_network_count; i++) {
            canvas.setTextColor(nav_server_select_idx == (i + 1) ? 0xFFFF : 0x7BEF);
            canvas.setCursor(6, 38 + (i * 14));
            canvas.printf("%s %s", (nav_server_select_idx == (i + 1) ? ">" : " "), discovered_networks[i]);
            // status dot per network: green handshake complete, orange fault, dim idle
            int dot_x = left_w - 8;
            int dot_y = 40 + i*14;
            uint16_t dot_c = 0x7BEF;
            if (i < MAX_NETWORKS) {
                if (network_handshake_complete[i]) dot_c = 0x07E0;
                else if (WiFi.status()!=WL_CONNECTED) dot_c = 0xFD20;
                else dot_c = 0x4208;
            }
            canvas.fillRect(dot_x, dot_y, 3, 3, dot_c);
        }
        
        canvas.setTextColor(0xFD20); canvas.setCursor(right_x + 5, 6); canvas.print("ACTIVE ROOMS");
        
        int total_visible_rooms = 0;
        for (int i = 0; i < gTabCount; i++) {
            if (strcmp(gTabs[i].name, "~mentions")==0) continue;
            bool matched_visible = (nav_server_select_idx == 0) || 
                                   (strcasecmp(gTabs[i].server, discovered_networks[nav_server_select_idx - 1]) == 0);
            if (matched_visible && nav_filter[0]) {
                char ln[32]; strncpy(ln, gTabs[i].name,31); ln[31]='\0'; for(char*p=ln;*p;p++) *p=tolower(*p);
                char lf[16]; strncpy(lf, nav_filter,15); lf[15]='\0'; for(char*p=lf;*p;p++) *p=tolower(*p);
                char ls[32]; strncpy(ls, gTabs[i].server,31); ls[31]='\0'; for(char*p=ls;*p;p++) *p=tolower(*p);
                if (!strstr(ln, lf) && !strstr(ls, lf) && !strstr(gTabs[i].topic, lf)) matched_visible=false;
            }
            if (matched_visible) total_visible_rooms++;
        }
        int active_window_scroll_offset = 0;
        if (nav_channel_select_idx >= 7) active_window_scroll_offset = nav_channel_select_idx - 6;
        if (total_visible_rooms > 7) {
            canvas.setTextColor(0x7BEF);
            if (active_window_scroll_offset>0) { canvas.setCursor(right_x + right_w -10, 18); canvas.print("^"); }
            if (active_window_scroll_offset + 7 < total_visible_rooms) { canvas.setCursor(right_x + right_w -10, display_height-20); canvas.print("v"); }
        }
        int channel_print_counter = 0;
        int rendered_rows_count = 0;
        for (int i = 0; i < gTabCount; i++) {
            if (strcmp(gTabs[i].name, "~mentions")==0) continue;
            bool should_print = (nav_server_select_idx == 0) || 
                                   (strcasecmp(gTabs[i].server, discovered_networks[nav_server_select_idx - 1]) == 0);
            if (should_print && nav_filter[0]) {
                char ln[32]; strncpy(ln, gTabs[i].name,31); ln[31]='\0'; for(char*p=ln;*p;p++) *p=tolower(*p);
                char lf[16]; strncpy(lf, nav_filter,15); lf[15]='\0'; for(char*p=lf;*p;p++) *p=tolower(*p);
                char ls[32]; strncpy(ls, gTabs[i].server,31); ls[31]='\0'; for(char*p=ls;*p;p++) *p=tolower(*p);
                if (!strstr(ln, lf) && !strstr(ls, lf) && !strstr(gTabs[i].topic, lf)) should_print=false;
            }
            if (should_print) {
                if (channel_print_counter >= active_window_scroll_offset && rendered_rows_count < 7) {
                    int draw_y_row_coordinate = 24 + (rendered_rows_count * 14);
                    uint16_t txt_color = (nav_channel_select_idx == channel_print_counter) ? 0xFFFF : 0x7BEF;
                    canvas.setCursor(right_x + 5, draw_y_row_coordinate);
                    int local_mention_count = 0;
                    for (int l = 0; l < gTabs[i].line_count; l++) if (gTabs[i].lines[l].is_highlight) local_mention_count++;
                    int max_chars_allowed = is_vertical ? 8 : ((local_mention_count > 0) ? 8 : 12);
                    char truncated_chan_name[20] = {0};
                    if ((int)strlen(gTabs[i].name) > max_chars_allowed) {
                        strncpy(truncated_chan_name, gTabs[i].name, max_chars_allowed - 3);
                        strcat(truncated_chan_name, "...");
                    } else {
                        strncpy(truncated_chan_name, gTabs[i].name, max_chars_allowed);
                    }
                    canvas.setTextColor(txt_color);
                    if (nav_server_select_idx == 0 && i > 0) {
                        char micro_net[4] = {0}; strncpy(micro_net, gTabs[i].server, 3);
                        canvas.printf("%s [%s]%s", (nav_channel_select_idx == channel_print_counter ? "*" : " "), micro_net, truncated_chan_name);
                    } else {
                        canvas.printf("%s %s", (nav_channel_select_idx == channel_print_counter ? "*" : " "), truncated_chan_name);
                    }
                    if (local_mention_count > 0) {
                        canvas.setTextColor(0xFD20);
                        if (local_mention_count > 10) canvas.print(" (10+)");
                        else canvas.printf(" (%d)", local_mention_count);
                    }
                    // per-room metadata: nick count + pinned marker
                    if (gTabs[i].pinned) { canvas.setTextColor(0xFFE0); canvas.print(" P"); }
                    if (gTabs[i].nick_count>0) { canvas.setTextColor(0x7BEF); canvas.printf(" %d", gTabs[i].nick_count); }
                    // last-message preview dim below (6px offset, 12-char)
                    if (gTabs[i].line_count>0 && !is_vertical) {
                        String last = String(gTabs[i].lines[gTabs[i].line_count-1].message);
                        if (last.length()>12) last = last.substring(0,12);
                        canvas.setCursor(right_x+8, draw_y_row_coordinate+7);
                        canvas.setTextColor(0x4208);
                        canvas.print(last);
                    }
                    rendered_rows_count++;
                }
                channel_print_counter++;
            }
        }
        int bar_y = display_height - 13;
        canvas.fillRect(0, bar_y, display_width, 13, 0x0841);
        canvas.drawFastHLine(0, bar_y - 1, display_width, 0x7BEF);
        if (nav_filter_active) {
            canvas.setCursor(6, bar_y + 2); canvas.setTextColor(0xFFE0); canvas.printf("Find:%s_", nav_filter);
        } else {
            canvas.setCursor(6, bar_y + 2); canvas.setTextColor(0xFFFF); canvas.print(";/. Scroll | Enter:Open Fn+F Find Fn+S Pin");
        }
        canvas.pushSprite(0, 0); 
        ui_needs_redraw = false; 
        return; 
    }

    // ==========================================
    // STATE 2: CONFIGURATION MENUS (Fn + O)
    // ==========================================
    if (current_app_mode == MODE_SETTINGS || current_app_mode == MODE_BOUNCER || current_app_mode == MODE_THEME || current_app_mode == MODE_LOGS || current_app_mode == MODE_WHOIS || current_app_mode == MODE_WIFI) {
        canvas.fillSprite(0x0000); 
        // Theme palette: light vs dark background
        uint16_t bg = use_light_theme ? 0xFFFF : 0x0000;
        uint16_t fg = use_light_theme ? 0x0000 : 0xFFFF;
        if (current_app_mode == MODE_THEME) canvas.fillSprite(bg);
        canvas.setTextColor(0xFD20); canvas.setCursor(10, 8);
        if (current_app_mode == MODE_SETTINGS) {
            canvas.print("--- SYSTEM CONFIGURATIONS ---"); canvas.setTextColor(0xFFFF);
            {
                char utcLab[20]; snprintf(utcLab, sizeof(utcLab), "UTC%+d%s", current_tz_idx, use_dst ? " DST" : "");
                canvas.setCursor(10, 26); canvas.printf("%s Timezone: %s", (menu_selection_idx == 0 ? ">" : " "), utcLab);
            }
            canvas.setCursor(10, 40); canvas.printf("%s Format Layer: %s", (menu_selection_idx == 1 ? ">" : " "), use_12_hour_format ? "12-HR" : "24-HR");
            canvas.setCursor(10, 54); canvas.printf("%s DST Override: %s", (menu_selection_idx == 2 ? ">" : " "), use_dst ? "ON +1h" : "OFF");
            canvas.setCursor(10, 68); canvas.printf("%s Storage Logging: %s", (menu_selection_idx == 3 ? ">" : " "), channel_log_enabled ? "ON" : "OFF");
        } else if (current_app_mode == MODE_BOUNCER) {
            canvas.print("--- BOUNCER CONNECTION SCHEMAS ---"); canvas.setTextColor(0xFFFF);
            canvas.setCursor(10, 26); canvas.printf("%s Server Host: %s", (menu_selection_idx == 0 ? ">" : " "), (const char*)bnc_host);
            canvas.setCursor(10, 40); canvas.printf("%s Port Address: %d", (menu_selection_idx == 1 ? ">" : " "), bnc_port);
            canvas.setCursor(10, 54); canvas.printf("%s Username Key: %s", (menu_selection_idx == 2 ? ">" : " "), (const char*)bnc_user);
        } else if (current_app_mode == MODE_THEME) {
            canvas.print("--- THEME STUDIO ---"); canvas.setTextColor(use_light_theme ? 0x0000 : 0xFFFF);
            canvas.setCursor(10, 26); canvas.printf("%s Theme: %s", (menu_selection_idx == 0 ? ">" : " "), use_light_theme ? "LIGHT" : "DARK");
            const char* accNames[4]={"DEFAULT","AMBER","CYAN","PURPLE"};
            canvas.setCursor(10, 40); canvas.printf("%s Accent: %s", (menu_selection_idx == 1 ? ">" : " "), accNames[theme_accent%4]);
            canvas.setCursor(10, 54); canvas.printf("%s Text Scale: %dx", (menu_selection_idx == 2 ? ">" : " "), text_scale);
            canvas.setCursor(10, 68); canvas.printf("%s Speaker: %s", (menu_selection_idx == 3 ? ">" : " "), speaker_enabled ? "ON" : "OFF");
            const char* sndNames[3]={"OFF","MENTION","ALL"};
            canvas.setCursor(10, 82); canvas.printf("%s Sound: %s", (menu_selection_idx == 4 ? ">" : " "), sndNames[sound_profile%3]);
        } else if (current_app_mode == MODE_LOGS) {
            canvas.print("--- LOG BROWSER ---"); canvas.setTextColor(0xFFFF);
            // Hotfix: avoid SD open in 20ms draw (WDT black screen) - show cached count
            canvas.setCursor(10, 26); canvas.print("SD: /irc/logs");
            canvas.setCursor(10, 40); canvas.printf("Use Fn+L again to refresh");
            canvas.setCursor(10, 54); canvas.print("Enter: preview (no SD block)");
            // Actual listing moved to background task, not draw
            
        } else if (current_app_mode == MODE_WHOIS) {
            canvas.print("--- WHOIS ---"); canvas.setTextColor(0xFFFF);
            canvas.setCursor(10, 26); canvas.printf("Nick: %s", whois_cache.nick);
            canvas.setCursor(10, 40); canvas.printf("User: %s@%s", whois_cache.user, whois_cache.host);
            canvas.setCursor(10, 54); canvas.printf("Real: %s", whois_cache.real);
            canvas.setCursor(10, 68); canvas.printf("Server: %s", whois_cache.server);
            canvas.setCursor(10, 82); canvas.printf("Account: %s %s", whois_cache.account, whois_cache.secure?"(secure)":"");
            canvas.setCursor(10, 96); canvas.printf("Idle: %d", whois_cache.idle);
            canvas.setCursor(10, 110); canvas.printf("Chans: %.20s", whois_cache.channels);
        } else {
            canvas.print("--- WI-FI CONFIG MANAGER ---"); canvas.setTextColor(0xFFFF);
            canvas.setCursor(10, 26); canvas.printf("%s Primary SSID: %s", (menu_selection_idx == 0 ? ">" : " "), (const char*)wifi_ssid);
            canvas.setCursor(10, 40); canvas.printf("%s Primary Pass: [ **** ]", (menu_selection_idx == 1 ? ">" : " "));
            canvas.setCursor(10, 54); canvas.printf("%s Backup Hotspot: %s", (menu_selection_idx == 2 ? ">" : " "), (const char*)wifi_ssid2);
            canvas.setCursor(10, 68); canvas.printf("%s Backup Pass:  [ **** ]", (menu_selection_idx == 3 ? ">" : " "));
            canvas.setCursor(10, 86); canvas.printf("%s Scan Airwaves: [ RUN ]", (menu_selection_idx == 4 ? ">" : " "));
            // WiFi scan HUD quick-reconnect (below Y=100, keep textbox integrity)
            int sc = WiFi.scanComplete();
            if (sc == -2) {
                canvas.setTextColor(0x7BEF); canvas.setCursor(10, 102); canvas.print("Scanning...");
            } else if (sc > 0) {
                int n = sc > 5 ? 5 : sc;
                canvas.setTextColor(0xFD20); canvas.setCursor(10, 102); canvas.printf("Found %d:", sc);
                for(int i=0;i<n;i++){
                    int y = 114 + i*12;
                    bool sel = (menu_selection_idx == 5 + i);
                    canvas.setTextColor(sel ? 0xFFFF : 0x7BEF);
                    canvas.setCursor(10, y);
                    String ss = WiFi.SSID(i);
                    if (ss.length()>14) ss = ss.substring(0,12)+"..";
                    canvas.printf("%s %s %ddBm", sel?">":" ", ss.c_str(), WiFi.RSSI(i));
                }
                if (menu_selection_idx >=5) {
                    canvas.setTextColor(0x07E0); canvas.setCursor(10, 180); canvas.print("Enter:Connect vault");
                }
            } else if (sc == 0) {
                canvas.setTextColor(0xF800); canvas.setCursor(10, 102); canvas.print("No networks");
            }
        }
        int bar_y = display_height - 13;
        canvas.fillRect(0, bar_y, display_width, 13, 0x0841);
        canvas.drawFastHLine(0, bar_y - 1, display_width, 0x7BEF);
        canvas.setCursor(10, bar_y + 2); canvas.setTextColor(0xFFFF); canvas.print("Esc: Exit | ,/. Adjust Value");
        canvas.pushSprite(0, 0); 
        ui_needs_redraw = false; 
        return;
    }

    // ==========================================
    // STATE 3: LIVE TERMINAL CHAT VIEWPORT
    // ==========================================
    // Ensure canvas valid - no PSRAM: keep whatever size succeeded at boot (240x109) to avoid 64KB alloc failure on 320KB heap
    // PSRAM boards get 240x135 atomic full-screen; no-PSRAM keeps 240x109 middle viewport + separate navbar/input to save RAM
    if(canvas.width()==0 || canvas.height()==0){
        Serial.println("[GFX] canvas 0 in draw, recreating viewport");
        canvas.deleteSprite();
        bool psram = psramFound() && ESP.getPsramSize() > 0;
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        // Need ~65KB for 240x135*2 + overhead; require 70KB contiguous
        if(psram || largest >= 70000){
            if(!canvas.createSprite(240,135)) canvas.createSprite(240,109);
        } else {
            if(!canvas.createSprite(240,109)) canvas.createSprite(135,214);
        }
        canvas.setTextSize(text_scale);
    }
    bool is_fullscreen = (canvas.width()==240 && canvas.height()==135);
    // Snapshot chat state under mutex (short critical section) to avoid holding mutex during heavy rendering
    Tab snapTab;
    uint8_t snap_gTabCount = 0;
    uint8_t snap_current_tab = current_tab_index;
    bool snap_is_scrollback = is_scrollback_active;
    bool snap_scroll_active = scrollback_mode_active;
    int snap_scrollback = scrollback_offset;
    bool snap_has_data = false;
    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (current_tab_index >= MAX_TABS) current_tab_index = 0;
        if (current_tab_index >= gTabCount && gTabCount > 0) current_tab_index = gTabCount - 1;
        snap_current_tab = current_tab_index;
        snap_gTabCount = gTabCount;
        snap_is_scrollback = is_scrollback_active;
        snap_scroll_active = scrollback_mode_active;
        snap_scrollback = scrollback_offset;
        // copy current tab
        snapTab = gTabs[snap_current_tab];
        // copy highlight state for overlay/sidebar (small snapshot)
        snap_has_data = true;
        xSemaphoreGive(irc_mutex);
    } else {
        // mutex busy - skip frame to avoid tearing, will retry next 20ms
        return;
    }
    {
        canvas.fillSprite(0x0000);
        Tab &t = snapTab;
        int effective_count = t.line_count;
        if (effective_count > MSG_BUFFER_SIZE) effective_count = MSG_BUFFER_SIZE;
        if (effective_count < 0) effective_count = 0;
        // keep scrollback sticky - if new messages arrived while scrolled back, effective_count grew but snap_scrollback is old; view should stay, NEW separator will show
        int cur_scrollback = snap_scrollback;
        if (cur_scrollback >= effective_count) {
            cur_scrollback = effective_count > 0 ? effective_count - 1 : 0;
        }
        if (snap_scroll_active && cur_scrollback < 0) cur_scrollback = 0;
        // sync globals for other subsystems (navbar uses global)
        scrollback_offset = cur_scrollback;
        scrollback_offset_idx = cur_scrollback;
        is_scrollback_active = snap_is_scrollback;
        int current_y = baseline_y;
        int starting_index = (effective_count - 1) - cur_scrollback;
        if (starting_index < 0) starting_index = 0;
        if (starting_index >= effective_count) starting_index = effective_count - 1;
        if (effective_count == 0) starting_index = -1;

        for (int i = starting_index; i >= 0; i--) {
            if (current_y < viewport_top_y) break;
            if (current_y > wire_y - 9) { current_y -= 12*text_scale; continue; }
            
            uint16_t row_bg = (i % 2 == 0) ? 0x0000 : 0x0841;
            int text_start_x = is_vertical ? 45 : ((current_tab_index == 0) ? 110 : 70);
            ChatLine &curLine = t.lines[(t.head + i) % MSG_BUFFER_SIZE];
            const char* msg_text = curLine.message;
            int msg_len = strlen(msg_text);
            int current_char_pos = 0;
            bool first_line_pass = true;
            while (current_char_pos < msg_len) {
                int active_render_x = first_line_pass ? text_start_x : (text_start_x + 6);
                int dynamic_chars_budget;
                if (is_vertical) {
                    dynamic_chars_budget = 15 / text_scale;
                    if (dynamic_chars_budget < 5) dynamic_chars_budget = 5;
                } else {
                    int dynamic_max_width = display_width - 4 - active_render_x;
                    dynamic_chars_budget = dynamic_max_width / (6*text_scale);
                }
                if (dynamic_chars_budget <= 0) break;
                int chunk = dynamic_chars_budget;
                if (chunk > msg_len - current_char_pos) chunk = msg_len - current_char_pos;
                if (chunk > 31) chunk = 31;
                char sub_line[32] = {0};
                memcpy(sub_line, msg_text + current_char_pos, chunk);
                sub_line[chunk] = '\0';
                uint16_t cur_bg = row_bg;
                if (search_active && search_query[0]) {
                    char lowSub[32]; strncpy(lowSub, sub_line,31); lowSub[31]='\0'; for(char*p=lowSub;*p;p++) *p=tolower(*p);
                    char lowQ[16]; strncpy(lowQ, search_query,15); lowQ[15]='\0'; for(char*p=lowQ;*p;p++) *p=tolower(*p);
                    if (strstr(lowSub, lowQ)) cur_bg = 0xFFE0;
                }
                canvas.fillRect(0, current_y - 2, display_width, 12*text_scale, cur_bg);
                
                if (first_line_pass) {
                    int vline_h = is_vertical ? wire_y : 120;
                    canvas.drawFastVLine(40, 0, vline_h, 0x7BEF); 
                    char shortened_nick[9] = {0};
                    if (strlen(curLine.nick) > 8) {
                        strncpy(shortened_nick, curLine.nick, 6);
                        shortened_nick[6] = '\0';
                        strcat(shortened_nick, "..");
                    } else {
                        strncpy(shortened_nick, curLine.nick, 8);
                        shortened_nick[8] = '\0';
                    }
                    if (curLine.is_highlight) { 
                        canvas.fillRect(2, current_y - 1, 36, 11, 0xFD20); canvas.setTextColor(0xFFFF); 
                    } else { 
                        canvas.setTextColor(get_nick_palette_color(curLine.nick)); 
                    }
                    canvas.setCursor(2, current_y);
                    if (current_tab_index == 0) canvas.printf("%s", shortened_nick);
                    else canvas.printf("<%s>", shortened_nick);
                    first_line_pass = false;
                }
                
                canvas.setTextColor(curLine.color); 
                canvas.setCursor(active_render_x, current_y);
                canvas.print(sub_line);
                current_y -= 12 * text_scale; 
                current_char_pos += chunk;
            }
        }
        // Topic line per-tab + chan modes viewer (no navbar clutter)
        if ((t.topic[0] || t.modes[0]) && !is_scrollback_active && current_y < wire_y - 14) {
            canvas.setTextColor(0x7BEF);
            canvas.setCursor(42, 14);
            int maxTopic = is_vertical ? 10 : 20;
            char topic_disp[32]={0};
            if (t.modes[0]) snprintf(topic_disp, sizeof(topic_disp), "%.12s [%s]", t.topic, t.modes);
            else strncpy(topic_disp, t.topic, maxTopic);
            topic_disp[maxTopic]='\0';
            canvas.print(topic_disp);
        }
        // Unread jump bar left edge 2px strip
        {
            int unread = 0;
            for(int u=0;u<t.line_count;u++) if(t.lines[u].is_highlight) unread++;
            if (unread>0 && !is_scrollback_active) {
                int bar_h = unread>8? (is_vertical? 80: 40) : unread*5;
                canvas.fillRect(0, wire_y - bar_h, 2, bar_h, 0xFD20);
            }
            // NEW separator when scrolled back
            if (is_scrollback_active && scrollback_offset>0) {
                int sep_y = baseline_y - (t.line_count -1 - scrollback_offset)*12 - 6;
                if (sep_y>=12 && sep_y < wire_y) {
                    canvas.drawFastHLine(2, sep_y, display_width-4, 0xF800);
                    canvas.setTextColor(0xF800); canvas.setCursor(display_width-28, sep_y-4); canvas.print("NEW");
                }
            }
        }
        bool chat_highlight_active = false;
        int highlight_src_tab = -1;
        for (int tt = 0; tt < gTabCount; tt++) {
            for (int ll = 0; ll < gTabs[tt].line_count; ll++) if (gTabs[tt].lines[ll].is_highlight) { chat_highlight_active = true; highlight_src_tab = tt; break; }
            if (chat_highlight_active) break;
        }
        if (chat_highlight_active && highlight_src_tab != (int)current_tab_index && gTabCount > 1) {
            Tab &ht = gTabs[highlight_src_tab];
            ChatLine hl = ht.lines[ht.line_count - 1];
            for (int ll = ht.line_count - 1; ll >= 0; ll--) if (ht.lines[ll].is_highlight) { hl = ht.lines[ll]; break; }
            int overlay_y = is_vertical ? (display_height - 32) : 108;
            int overlay_w = display_width;
            canvas.fillRect(0, overlay_y, overlay_w, 12, 0xFD20);
            canvas.drawRect(0, overlay_y, overlay_w, 12, 0xFFFF);
            canvas.setTextColor(0x0000);
            canvas.setCursor(4, overlay_y + 2);
            char overlay_short[9] = {0}; strncpy(overlay_short, hl.nick, 8);
            canvas.printf("! [%s]%s: %.20s", ht.server, overlay_short, hl.message);
        }
        // Horizontal dividing wireframe cutoff line exactly at Y = wire_y
        canvas.drawFastHLine(0, wire_y, display_width, 0x7BEF);
        // Nicklist drawer (Fn+Enter) - 42px overlay from right
        if (show_nicklist && t.nick_count>0) {
            int drawer_w = 42;
            int drawer_x = display_width - drawer_w;
            canvas.fillRect(drawer_x, 12, drawer_w, wire_y-12, 0x0841);
            canvas.drawRect(drawer_x, 12, drawer_w, wire_y-12, 0x7BEF);
            canvas.setTextColor(0xFD20); canvas.setCursor(drawer_x+4, 14); canvas.print("NICKS");
            for(int n=0;n<t.nick_count && n<10; n++){
                if(t.nicks_away[n]) canvas.setTextColor(0x4208);
                else canvas.setTextColor(get_nick_palette_color(t.nicks[n]));
                canvas.setCursor(drawer_x+4, 26+n*10);
                char nd[12]={0}; strncpy(nd, t.nicks[n], 10);
                canvas.print(nd);
            }
            canvas.setTextColor(0x7BEF); canvas.setCursor(drawer_x+4, wire_y-10); canvas.print("Ent:close");
        }
        // Search bar at wire_y-12 when active
        if (search_active) {
            canvas.fillRect(0, wire_y-12, display_width-6, 12, 0x4208);
            canvas.drawRect(0, wire_y-12, display_width-6, 12, 0xFFE0);
            canvas.setTextColor(0xFFFF); canvas.setCursor(4, wire_y-10);
            canvas.printf("?%s_", search_query);
        }
        // Split mentions peek Fn+Q 3-line strip above wire_y
        if (show_mentions_peek && gTabs[0].line_count>0 && current_app_mode==MODE_CHAT) {
            int peek_h = 30;
            int peek_y = wire_y - peek_h - 2;
            if(peek_y < 12) peek_y = 12;
            canvas.fillRect(0, peek_y, display_width-6, peek_h, 0x0841);
            canvas.drawRect(0, peek_y, display_width-6, peek_h, 0x7BEF);
            canvas.setTextColor(0xFFE0); canvas.setCursor(4, peek_y+2); canvas.print("~mentions peek");
            for(int i=0;i<3 && i<gTabs[0].line_count;i++){
                int idx=gTabs[0].line_count-1-i;
                ChatLine &ml=gTabs[0].lines[idx];
                canvas.setCursor(4, peek_y+12+i*8);
                canvas.setTextColor(0xFFFF);
                char tmp[22]={0}; strncpy(tmp, ml.message,20);
                canvas.print(tmp);
            }
        }
        // Slim 6px sidebar vertical indicators (right edge, 12-wire_y)
        {
            int sb_x = display_width - 6;
            canvas.fillRect(sb_x, 12, 6, wire_y - 12, 0x0841);
            canvas.drawFastVLine(sb_x, 12, wire_y - 12, 0x7BEF);
            // WiFi dot top
            uint16_t wifi_c = (WiFi.status()==WL_CONNECTED) ? 0x07E0 : 0xF800;
            canvas.fillRect(sb_x+2, 14, 2, 2, wifi_c);
            // Battery vertical bar 4x20
            float bp = get_calibrated_battery_percentage();
            int bh = (int)(bp / 100.0f * 18);
            if (bh<1) bh=1; if (bh>18) bh=18;
            uint16_t bat_c = (bp <= 20) ? 0xF800 : (bp <= 40 ? 0xFD20 : 0x07E0);
            canvas.fillRect(sb_x+1, 20 + (18-bh), 4, bh, bat_c);
            canvas.drawRect(sb_x+1, 20, 4, 18, 0x7BEF);
            // Per-tab unread dots stacked
            int dot_y = 42;
            for(int ti=1; ti<gTabCount && dot_y < wire_y-12; ti++){
                bool hi=false; for(int li=0; li<gTabs[ti].line_count; li++) if(gTabs[ti].lines[li].is_highlight) {hi=true; break;}
                if (ti == current_tab_index) { canvas.fillRect(sb_x+2, dot_y, 2, 2, 0xFFFF); }
                else if (hi) { canvas.fillRect(sb_x+2, dot_y, 2, 2, 0xFD20); }
                else if (gTabs[ti].line_count>0) { canvas.fillRect(sb_x+2, dot_y, 2, 2, 0x07FF); }
                dot_y+=4;
                if(dot_y > wire_y-8) break;
            }
        }
        // TextBox Workspace Integrity: Y = 225 to 240 must remain empty - ensure cleared
        if (is_vertical) {
            canvas.fillRect(0, 225, display_width, display_height - 225, 0x0000);
        }
        if(is_fullscreen){
            // --- Fullscreen atomic path (PSRAM): navbar+input into canvas, single push at 0,0 eliminates flicker ---
            // RSSI history update (throttled 500ms, no draw yet)
            {
                int8_t cur = (WiFi.status()==WL_CONNECTED) ? (int8_t)WiFi.RSSI() : -127;
                static unsigned long last_hist = 0;
                if (millis() - last_hist > 500) { last_hist = millis(); rssi_history[rssi_history_idx]=cur; rssi_history_idx=(rssi_history_idx+1)%8; }
            }
            // Navbar (0,0,240,12) - drawn into canvas
            canvas.fillRect(0, 0, display_width, 12, 0x0841);
            {
                char nav_server_str[32]={0}, nav_chan_str[32]={0};
                strncpy(nav_server_str, snapTab.server,31); strncpy(nav_chan_str, snapTab.name,31);
                char full_nav_str[70]={0}; snprintf(full_nav_str,sizeof(full_nav_str),"[%s] %s",nav_server_str,nav_chan_str);
                int nav_text_width = canvas.textWidth(full_nav_str);
                char display_server[32]={0}; strncpy(display_server, nav_server_str,31);
                if (nav_text_width > navbar_clamp_x) {
                    int server_len = strlen(nav_server_str);
                    if (server_len > 6) { strncpy(display_server, nav_server_str,4); display_server[4]='\0'; strcat(display_server,"..."); strncat(display_server, nav_server_str+server_len-2,2); }
                    else if (server_len > 4) { strncpy(display_server, nav_server_str,2); display_server[2]='\0'; strcat(display_server,"..."); strncat(display_server, nav_server_str+server_len-2,2); }
                    snprintf(full_nav_str,sizeof(full_nav_str),"[%s] %s",display_server,nav_chan_str);
                    nav_text_width = canvas.textWidth(full_nav_str);
                    if (nav_text_width > navbar_clamp_x) {
                        int max_chan = is_vertical ? 6 : 10;
                        if ((int)strlen(nav_chan_str) > max_chan) { nav_chan_str[max_chan-3]='\0'; strcat(nav_chan_str,"..."); }
                    }
                }
                canvas.setTextColor(0x7BEF, 0x0841); canvas.setCursor(2, 2);
                canvas.printf("[%s]", display_server);
                int chan_x = is_vertical ? 45 : 54;
                canvas.setTextColor(0xFFFF, 0x0841); canvas.setCursor(chan_x, 2);
                char truncated_name[12] = {0}; strncpy(truncated_name, nav_chan_str, is_vertical ? 6 : 8);
                canvas.print(truncated_name);
                if (WiFi.status() != WL_CONNECTED) {
                    canvas.setTextColor(0x7BEF, 0x0841); canvas.setCursor(rssi_anchor_x, 2); canvas.print("---");
                } else {
                    for(int i=0;i<8;i++){
                        int idx = (rssi_history_idx + i) %8;
                        int8_t v = rssi_history[idx];
                        int h=0;
                        if(v != -127){
                            if(v >= -50) h=6;
                            else if(v >= -60) h=4;
                            else if(v >= -75) h=3;
                            else if(v >= -90) h=2;
                            else h=1;
                        }
                        int x = rssi_anchor_x + i*2;
                        int y0 = 10;
                        if(x < battery_anchor_x -2){
                            if(h>0) canvas.fillRect(x, y0 - h, 1, h, 0x07E0);
                            else canvas.fillRect(x, 9, 1, 1, 0x4208);
                        }
                    }
                }
                canvas.setTextColor(0xFFFF, 0x0841); canvas.setCursor(battery_anchor_x, 2);
                canvas.printf("%d%%", (int)get_calibrated_battery_percentage());
                if (!is_vertical) {
                    struct tm timeinfo;
                    int hh, mm;
                    if (getLocalTime(&timeinfo, 30)) { hh=timeinfo.tm_hour; mm=timeinfo.tm_min; }
                    else {
                        unsigned long sec = (millis()/1000 + adj_time) % 86400; hh=sec/3600; mm=(sec%3600)/60;
                    }
                    if (use_12_hour_format) { int h12 = hh%12; if(h12==0) h12=12; hh=h12; }
                    canvas.setTextColor(0x7BEF, 0x0841); canvas.setCursor(150, 2);
                    canvas.printf("%02d:%02d", hh, mm);
                    canvas.setTextColor(0xFFFF, 0x0841); canvas.setCursor(185, 2);
                    canvas.printf("%d/%d", (int)snap_current_tab+1, (int)snap_gTabCount);
                    if (snapTab.topic[0]) { canvas.setTextColor(0xFD20,0x0841); canvas.setCursor(205,2); canvas.print("*"); }
                }
                for (int t = 1; t < snap_gTabCount; t++) {
                    if (t == snap_current_tab) continue;
                    int dot_x = 2 + (t * 6);
                    int dot_limit = is_vertical ? 60 : 130;
                    if (dot_x > dot_limit) break;
                    bool has_unread_highlight = false;
                    bool has_unread_msg = false;
                    if (t < MAX_TABS) {
                        if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(2))==pdTRUE) {
                            has_unread_msg = (gTabs[t].line_count > 0);
                            for (int l = 0; l < gTabs[t].line_count; l++) if (gTabs[t].lines[l].is_highlight) { has_unread_highlight = true; break; }
                            xSemaphoreGive(irc_mutex);
                        } else {
                            has_unread_msg = true;
                        }
                    }
                    if (has_unread_highlight) canvas.fillRect(dot_x, 10, 3, 2, 0xFD20);
                    else if (has_unread_msg) canvas.fillRect(dot_x, 10, 3, 2, 0x07FF);
                }
            }
            // Input box - drawn into canvas
            {
                canvas.setTextSize(text_scale);
                canvas.fillRect(0, input_box_y, display_width, textbox_height, 0x0000);
                canvas.drawFastHLine(0, wire_y, display_width, 0x7BEF);
                canvas.setTextColor(0xFD20, 0x0000);
                int input_cursor_y = is_vertical ? (text_scale==2? 223 : 227) : (text_scale==2? 118 : 124);
                canvas.setCursor(4, input_cursor_y);
                canvas.print("> ");
                canvas.setTextColor(0xFFFF, 0x0000);
                { int l=input_buffer.length(); const char* s=input_buffer.c_str();
                  int off=0; if(l>31) off=l-31;
                  int dlen=l-off; if(is_vertical && dlen>18) off=l-18;
                  if (l>0) canvas.print(s+off);
                }
                canvas.setTextSize(1);
                {
                    int rem = 200 - (int)input_buffer.length();
                    int cnt_x = display_width - 26;
                    canvas.setCursor(cnt_x, input_cursor_y);
                    if ((int)input_buffer.length() >= 190) canvas.setTextColor(0xF800, 0x0000);
                    else canvas.setTextColor(0x7BEF, 0x0000);
                    canvas.printf("%2d", rem);
                }
                canvas.setTextSize(text_scale);
            }
        } else {
            // --- No-PSRAM fallback: middle viewport only, navbar/input via Display but with waitDisplay batching to reduce flicker ---
            // RSSI update
            {
                int8_t cur = (WiFi.status()==WL_CONNECTED) ? (int8_t)WiFi.RSSI() : -127;
                static unsigned long last_hist = 0;
                if (millis() - last_hist > 500) { last_hist = millis(); rssi_history[rssi_history_idx]=cur; rssi_history_idx=(rssi_history_idx+1)%8; }
            }
        }
    } // end snapshot block
    if(is_fullscreen){
        canvas.pushSprite(0, 0);
    } else {
        // push middle viewport at 0,12, then batch navbar/input via Display
        canvas.pushSprite(0, 12);
        // batch navbar+input in single startWrite to reduce SPI transactions
        M5Cardputer.Display.waitDisplay();
        M5Cardputer.Display.startWrite();
        // navbar
        M5Cardputer.Display.fillRect(0, 0, display_width, 12, 0x0841);
        {
            char nav_server_str[32]={0}, nav_chan_str[32]={0};
            strncpy(nav_server_str, snapTab.server,31); strncpy(nav_chan_str, snapTab.name,31);
            char full_nav_str[70]={0}; snprintf(full_nav_str,sizeof(full_nav_str),"[%s] %s",nav_server_str,nav_chan_str);
            int nav_text_width = canvas.textWidth(full_nav_str);
            char display_server[32]={0}; strncpy(display_server, nav_server_str,31);
            if (nav_text_width > navbar_clamp_x) {
                int server_len = strlen(nav_server_str);
                if (server_len > 6) { strncpy(display_server, nav_server_str,4); display_server[4]='\0'; strcat(display_server,"..."); strncat(display_server, nav_server_str+server_len-2,2); }
                else if (server_len > 4) { strncpy(display_server, nav_server_str,2); display_server[2]='\0'; strcat(display_server,"..."); strncat(display_server, nav_server_str+server_len-2,2); }
                snprintf(full_nav_str,sizeof(full_nav_str),"[%s] %s",display_server,nav_chan_str);
                nav_text_width = canvas.textWidth(full_nav_str);
                if (nav_text_width > navbar_clamp_x) {
                    int max_chan = is_vertical ? 6 : 10;
                    if ((int)strlen(nav_chan_str) > max_chan) { nav_chan_str[max_chan-3]='\0'; strcat(nav_chan_str,"..."); }
                }
            }
            M5Cardputer.Display.setTextColor(0x7BEF, 0x0841); M5Cardputer.Display.setCursor(2, 2);
            M5Cardputer.Display.printf("[%s]", display_server);
            int chan_x = is_vertical ? 45 : 54;
            M5Cardputer.Display.setTextColor(0xFFFF, 0x0841); M5Cardputer.Display.setCursor(chan_x, 2);
            char truncated_name[12] = {0}; strncpy(truncated_name, nav_chan_str, is_vertical ? 6 : 8);
            M5Cardputer.Display.print(truncated_name);
            if (WiFi.status() != WL_CONNECTED) {
                M5Cardputer.Display.setTextColor(0x7BEF, 0x0841); M5Cardputer.Display.setCursor(rssi_anchor_x, 2); M5Cardputer.Display.print("---");
            } else {
                for(int i=0;i<8;i++){
                    int idx = (rssi_history_idx + i) %8;
                    int8_t v = rssi_history[idx];
                    int h=0;
                    if(v != -127){
                        if(v >= -50) h=6;
                        else if(v >= -60) h=4;
                        else if(v >= -75) h=3;
                        else if(v >= -90) h=2;
                        else h=1;
                    }
                    int x = rssi_anchor_x + i*2;
                    int y0 = 10;
                    if(x < battery_anchor_x -2){
                        if(h>0) M5Cardputer.Display.fillRect(x, y0 - h, 1, h, 0x07E0);
                        else M5Cardputer.Display.fillRect(x, 9, 1, 1, 0x4208);
                    }
                }
            }
            M5Cardputer.Display.setTextColor(0xFFFF, 0x0841); M5Cardputer.Display.setCursor(battery_anchor_x, 2);
            M5Cardputer.Display.printf("%d%%", (int)get_calibrated_battery_percentage());
            if (!is_vertical) {
                struct tm timeinfo; int hh, mm;
                if (getLocalTime(&timeinfo, 30)) { hh=timeinfo.tm_hour; mm=timeinfo.tm_min; }
                else { unsigned long sec = (millis()/1000 + adj_time) % 86400; hh=sec/3600; mm=(sec%3600)/60; }
                if (use_12_hour_format) { int h12 = hh%12; if(h12==0) h12=12; hh=h12; }
                M5Cardputer.Display.setTextColor(0x7BEF, 0x0841); M5Cardputer.Display.setCursor(150, 2);
                M5Cardputer.Display.printf("%02d:%02d", hh, mm);
                M5Cardputer.Display.setTextColor(0xFFFF, 0x0841); M5Cardputer.Display.setCursor(185, 2);
                M5Cardputer.Display.printf("%d/%d", (int)snap_current_tab+1, (int)snap_gTabCount);
                if (snapTab.topic[0]) { M5Cardputer.Display.setTextColor(0xFD20,0x0841); M5Cardputer.Display.setCursor(205,2); M5Cardputer.Display.print("*"); }
            }
            for (int t = 1; t < snap_gTabCount; t++) {
                if (t == snap_current_tab) continue;
                int dot_x = 2 + (t * 6);
                int dot_limit = is_vertical ? 60 : 130;
                if (dot_x > dot_limit) break;
                bool hi=false; bool has=false;
                if (t < MAX_TABS) {
                    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(2))==pdTRUE) {
                        has = (gTabs[t].line_count > 0);
                        for (int l=0;l<gTabs[t].line_count;l++) if(gTabs[t].lines[l].is_highlight){hi=true;break;}
                        xSemaphoreGive(irc_mutex);
                    } else has=true;
                }
                if (hi) M5Cardputer.Display.fillRect(dot_x, 10, 3, 2, 0xFD20);
                else if (has) M5Cardputer.Display.fillRect(dot_x, 10, 3, 2, 0x07FF);
            }
        }
        // input box via Display
        {
            M5Cardputer.Display.setTextSize(text_scale);
            M5Cardputer.Display.fillRect(0, input_box_y, display_width, textbox_height, 0x0000);
            M5Cardputer.Display.drawFastHLine(0, wire_y, display_width, 0x7BEF);
            M5Cardputer.Display.setTextColor(0xFD20, 0x0000);
            int input_cursor_y = is_vertical ? (text_scale==2? 223 : 227) : (text_scale==2? 118 : 124);
            M5Cardputer.Display.setCursor(4, input_cursor_y);
            M5Cardputer.Display.print("> ");
            M5Cardputer.Display.setTextColor(0xFFFF, 0x0000);
            { int l=input_buffer.length(); const char* s=input_buffer.c_str();
              int off=0; if(l>31) off=l-31;
              int dlen=l-off; if(is_vertical && dlen>18) off=l-18;
              if(l>0) M5Cardputer.Display.print(s+off);
            }
            M5Cardputer.Display.setTextSize(1);
            {
                int rem = 200 - (int)input_buffer.length();
                int cnt_x = display_width - 26;
                M5Cardputer.Display.setCursor(cnt_x, input_cursor_y);
                if ((int)input_buffer.length() >= 190) M5Cardputer.Display.setTextColor(0xF800, 0x0000);
                else M5Cardputer.Display.setTextColor(0x7BEF, 0x0000);
                M5Cardputer.Display.printf("%2d", rem);
            }
            M5Cardputer.Display.setTextSize(text_scale);
        }
        M5Cardputer.Display.endWrite();
    }
    ui_needs_redraw = false;
}

void handle_keyboard_inputs() {
    static bool wasPressed = false;
    bool isPressed = M5Cardputer.Keyboard.isPressed();
    if (isPressed) {
        last_user_keyboard_input_tick = millis(); // Refresh activity timer anchor (always while held)
        // Throttled brightness restore: only on rising edge, not every 10ms while held (fixes flicker/SPI flood)
        if (!wasPressed && current_app_mode == MODE_CHAT && g_backlight_level != 255) {
            g_backlight_level = 255;
            ui_needs_redraw = true;
        }
        wasPressed = true;
    } else {
        wasPressed = false;
        return;
    }
    // Hardware bounce filter 35ms (was 180ms which blocked legitimate repeat and still left unconditional redraw)
    // Keep short so typematic repeat can be controlled by explicit hold guards below
    if (millis() - last_keypress_debounce < 35) { esp_task_wdt_reset(); return; }
    last_keypress_debounce = millis();
    last_input_time = millis(); // Fresh backlight dim timer
    // Fn edge-triggered mode switches already debounce via was_*_prev, no extra 300ms block
    // (previous 300ms window blocked rapid Fn+P/O presses and made them appear "does nothing")
    
    auto status = M5Cardputer.Keyboard.keysState();
    bool is_alt = M5Cardputer.Keyboard.isKeyPressed(KEY_LEFT_ALT);
    bool is_fn  = M5Cardputer.Keyboard.isKeyPressed(KEY_FN);
    // Hold repeat guard: throttled typematic 90ms
    {
        static String last_hold_word="";
        static unsigned long last_hold_ms=0;
        String cur;
        cur.reserve(status.word.size());
        for(auto c: status.word) cur += c;
        if (cur.length()>0 && cur == last_hold_word && millis() - last_hold_ms < 90) {
            esp_task_wdt_reset(); return;
        }
        if (cur.length()>0) { last_hold_word = cur; last_hold_ms = millis(); }
    }
    // Fn hold ghost guard: if only Fn held with no other key word/enter/del, avoid ghost matrix scan
    if (is_fn && status.word.empty() && !status.enter && !status.del) {
        bool anyFnCombo = M5Cardputer.Keyboard.isKeyPressed('p') || M5Cardputer.Keyboard.isKeyPressed('o') || M5Cardputer.Keyboard.isKeyPressed('l') || M5Cardputer.Keyboard.isKeyPressed('i') || M5Cardputer.Keyboard.isKeyPressed('q') || M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('c') || M5Cardputer.Keyboard.isKeyPressed('r') || M5Cardputer.Keyboard.isKeyPressed('b') || M5Cardputer.Keyboard.isKeyPressed('m') || M5Cardputer.Keyboard.isKeyPressed(';') || M5Cardputer.Keyboard.isKeyPressed('.') || M5Cardputer.Keyboard.isKeyPressed(',') || M5Cardputer.Keyboard.isKeyPressed('/') || M5Cardputer.Keyboard.isKeyPressed(' ') || M5Cardputer.Keyboard.isKeyPressed('f');
        if (!anyFnCombo) { esp_task_wdt_reset(); return; }
    }
    
    // Hotkey Intercept A: Toggle Multi-Network Channel Navigator Hub (Fn + P) - edge
    {
        static bool was_p_prev=false;
        bool cur_p = is_fn && M5Cardputer.Keyboard.isKeyPressed('p');
        if(cur_p && !was_p_prev){
            current_app_mode = (current_app_mode == MODE_NAVIGATOR) ? MODE_CHAT : MODE_NAVIGATOR;
            menu_selection_idx = 0; nav_server_select_idx = 0; nav_channel_select_idx = 0;
            ui_needs_redraw = true;
            set_led_mode(18);
            was_p_prev=true;
            return;
        }
        if(!cur_p) was_p_prev=false;
        if(cur_p) { esp_task_wdt_reset(); return; } // hold, don't fall through to other handlers
    }

    // Hotkey Intercept B: Cycle Hardware & Bouncer Configuration Menus (Fn + O) + Theme - edge triggered to prevent hold flicker
    {
        static bool was_o_prev=false;
        bool cur_o = is_fn && M5Cardputer.Keyboard.isKeyPressed('o');
        if(cur_o && !was_o_prev){
            if (current_app_mode == MODE_CHAT)       { current_app_mode = MODE_SETTINGS; }
            else if (current_app_mode == MODE_SETTINGS) { current_app_mode = MODE_BOUNCER; }
            else if (current_app_mode == MODE_BOUNCER)  { current_app_mode = MODE_THEME; }
            else if (current_app_mode == MODE_THEME)    { current_app_mode = MODE_WIFI; }
            else                                      { current_app_mode = MODE_CHAT; }
            menu_selection_idx = 0;
            ui_needs_redraw = true;
            set_led_mode(18);
            was_o_prev=true;
            return;
        }
        if(!cur_o) was_o_prev=false;
        if(cur_o) { esp_task_wdt_reset(); return; }
    }
    {
        static bool was_i_prev=false;
        bool cur_i = is_fn && M5Cardputer.Keyboard.isKeyPressed('i');
        if(cur_i && !was_i_prev){
            String who = "";
            if (input_buffer.length()>0) {
                int sp = input_buffer.lastIndexOf(' ');
                who = (sp==-1)? input_buffer : input_buffer.substring(sp+1);
                who.trim(); if(who.startsWith("/")) who="";
            }
            if (who.length()==0 && gTabs[current_tab_index].line_count>0) who = String(gTabs[current_tab_index].lines[gTabs[current_tab_index].line_count-1].nick);
            who.trim(); if(who.length()>0 && who!="server" && who!="ClientCore"){
                memset(&whois_cache,0,sizeof(whois_cache));
                whois_pending=true;
                const char* anet = gTabs[current_tab_index].server;
                for(int i=0;i<discovered_network_count;i++) if(strcmp(discovered_networks[i], anet)==0 && clients[i].connected()){
                    clients[i].printf("WHOIS %s\r\n", who.c_str());
                    clients[i].flush();
                    strncpy(whois_cache.nick, who.c_str(), sizeof(whois_cache.nick)-1);
                    break;
                }
                set_led_mode(18);
                ui_needs_redraw=true;
            }
            was_i_prev=true;
            return;
        }
        if(!cur_i) was_i_prev=false;
        if(cur_i) { esp_task_wdt_reset(); return; }
    }
    // Fn+B dBm toggle removed - keep sparkline only per user
    {
        static bool was_q_prev=false;
        bool cur_q = is_fn && M5Cardputer.Keyboard.isKeyPressed('q');
        if(cur_q && !was_q_prev){
            show_mentions_peek = !show_mentions_peek;
            ui_needs_redraw=true;
            was_q_prev=true;
            return;
        }
        if(!cur_q) was_q_prev=false;
        if(cur_q) { esp_task_wdt_reset(); return; }
    }
    {
        static bool was_l_prev=false;
        bool cur_l = is_fn && M5Cardputer.Keyboard.isKeyPressed('l');
        if(cur_l && !was_l_prev){
            current_app_mode = (current_app_mode == MODE_LOGS) ? MODE_CHAT : MODE_LOGS;
            menu_selection_idx = 0;
            ui_needs_redraw = true;
            set_led_mode(18);
            was_l_prev=true;
            return;
        }
        if(!cur_l) was_l_prev=false;
        if(cur_l) { esp_task_wdt_reset(); return; }
    }


    
    // Layer B: Master Emergency Escape Back to Main Chat Workspace
    // Captures (Alt + Backspace) OR the literal physical Esc key char mapping ('`')
    if ((is_alt && (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || status.del)) || M5Cardputer.Keyboard.isKeyPressed('`')) {
        // Auto-export updated parameters safely to card storage upon form exit
        if (current_app_mode == MODE_SETTINGS || current_app_mode == MODE_BOUNCER || current_app_mode == MODE_WIFI) {
            if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                File file = SD.open("/irc/config.txt", FILE_WRITE);
                if (file) {
                    file.printf("wifi_ssid=%s\nwifi_pass=%s\nirc_nick=%s\n", wifi_ssid, wifi_pass, irc_nick);
                    file.printf("bnc_host=%s\nbnc_port=%d\nbnc_user=%s\nbnc_pass=%s\n", bnc_host, bnc_port, bnc_user, bnc_pass);
                    file.printf("channel_log_enabled=%d\nscreen_brightness=%d\n", channel_log_enabled, screen_brightness);
                    file.printf("current_tz_idx=%d\nuse_12_hour_format=%d\nuse_dst=%d\nuse_light_theme=%d\ntheme_accent=%d\ntext_scale=%d\nspeaker_enabled=%d\n", current_tz_idx, use_12_hour_format, use_dst, use_light_theme, theme_accent, text_scale, speaker_enabled);
                    file.close();
                    Serial.println("[STORAGE-SYNC] Configuration parameters permanently synchronized to micro-SD card.");
                    set_led_mode(19); // Lime Green Blip for auto-saver
                }
                xSemaphoreGive(irc_mutex);
            }
        }
        current_app_mode = MODE_CHAT;
        input_buffer = ""; // Cleanly flush stray menu characters out of memory registers
        ui_needs_redraw = true;
        return;
    }
    // Fn+Esc quick ~mentions jump (faster than Fn+,/ swapper)
    if (is_fn && M5Cardputer.Keyboard.isKeyPressed('`')) {
        current_tab_index = 0;
        scrollback_offset = 0; scrollback_offset_idx = 0;
        is_scrollback_active = false; scrollback_mode_active = false;
        // clear highlight dots for mentions view
        if(irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5))==pdTRUE){
            for(int l=0;l<gTabs[0].line_count;l++) gTabs[0].lines[l].is_highlight=false;
            xSemaphoreGive(irc_mutex);
        }
        current_app_mode = MODE_CHAT;
        ui_needs_redraw = true;
        set_led_mode(18);
        return;
    }
    // Global Scrollback Panic Reset (Esc or `) - edge triggered 500ms
    {
        static unsigned long last_esc_ms=0;
        static bool was_esc=false;
        bool cur_esc = M5Cardputer.Keyboard.isKeyPressed('`');
        if (cur_esc && !was_esc) {
            if (millis() - last_esc_ms < 500) { was_esc=cur_esc; return; }
            last_esc_ms = millis();
            scrollback_offset = 0;
            scrollback_offset_idx = 0;
            is_scrollback_active = false;
            scrollback_mode_active = false;
            ui_needs_redraw = true;
        }
        was_esc = cur_esc;
    }

    // Layer C: Critical Hardware Intercept for Fn+Arrow Punctuation Codes
    // On the Cardputer layout, Fn+Arrows outputs direct character values:
    // Fn+Left = ';' | Fn+Right = '/' | Fn+Up = ',' | Fn+Down = '.'
    if (current_app_mode == MODE_CHAT) {
        // Scrollback up/down and tab swap are intentionally repeatable but throttled to 90ms to avoid SPI/UI flood
        {
            static unsigned long last_scroll_ms=0;
            if (is_fn && M5Cardputer.Keyboard.isKeyPressed(';')) { // Fn + UP Arrow
                if(millis() - last_scroll_ms < 90) { esp_task_wdt_reset(); return; }
                last_scroll_ms = millis();
                Tab &t = gTabs[current_tab_index];
                is_scrollback_active = true;
                scrollback_mode_active = true;
                {
                    int eff = t.line_count;
                    if (eff > MSG_BUFFER_SIZE) eff = MSG_BUFFER_SIZE;
                    if (scrollback_offset < eff - 1) scrollback_offset++;
                    if (scrollback_offset_idx < eff - 1) scrollback_offset_idx++;
                }
                ui_needs_redraw = true;
                return;
            }
            if (is_fn && M5Cardputer.Keyboard.isKeyPressed('.')) { // Fn + DOWN Arrow
                if(millis() - last_scroll_ms < 90) { esp_task_wdt_reset(); return; }
                last_scroll_ms = millis();
                if (scrollback_offset > 0) scrollback_offset--;
                if (scrollback_offset_idx > 0) scrollback_offset_idx--;
                if (scrollback_offset == 0) is_scrollback_active = false;
                if (scrollback_offset_idx == 0) scrollback_mode_active = false;
                ui_needs_redraw = true;
                return;
            }
        }
        // Fn + Backspace Panic Flush Macro
        if (is_fn && (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || status.del)) {
            Tab &t = gTabs[current_tab_index];
            if (xNetworkTaskHandle != NULL) vTaskSuspend(xNetworkTaskHandle);
            t.line_count = 0;
            t.head = 0;
            scrollback_offset = 0;
            scrollback_offset_idx = 0;
            is_scrollback_active = false;
            scrollback_mode_active = false;
            memset(t.lines, 0, sizeof(t.lines));
            if (xNetworkTaskHandle != NULL) vTaskResume(xNetworkTaskHandle);
            ui_needs_redraw = true;
            return;
        }
        if ((is_alt && (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || status.del)) || M5Cardputer.Keyboard.isKeyPressed('`')) {
            scrollback_offset = 0;
            scrollback_offset_idx = 0;
            is_scrollback_active = false;
            scrollback_mode_active = false;
            ui_needs_redraw = true;
        }

        // Horizontal Quick-Tab Swapper (Fn + Comma = Previous Tab | Fn + Slash = Next Tab) - throttled 120ms repeat
        {
            static unsigned long last_tab_ms=0;
            if (is_fn && M5Cardputer.Keyboard.isKeyPressed(',')) { // Previous Tab
                if(millis() - last_tab_ms < 120) { esp_task_wdt_reset(); return; }
                last_tab_ms = millis();
                if (gTabCount > 1) {
                    current_tab_index = (current_tab_index - 1 + gTabCount) % gTabCount;
                    scrollback_offset = 0;
                    scrollback_offset_idx = 0;
                    ui_needs_redraw = true;
                    save_session_state();
                    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        Tab &current_view_tab = gTabs[current_tab_index];
                        for (int l = 0; l < current_view_tab.line_count; l++) current_view_tab.lines[l].is_highlight = false;
                        xSemaphoreGive(irc_mutex);
                    }
                }
                return;
            }
            if (is_fn && M5Cardputer.Keyboard.isKeyPressed('/')) { // Next Tab
                if(millis() - last_tab_ms < 120) { esp_task_wdt_reset(); return; }
                last_tab_ms = millis();
                if (gTabCount > 1) {
                    current_tab_index = (current_tab_index + 1) % gTabCount;
                    scrollback_offset = 0;
                    scrollback_offset_idx = 0;
                    ui_needs_redraw = true;
                    save_session_state();
                    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        Tab &current_view_tab = gTabs[current_tab_index];
                        for (int l = 0; l < current_view_tab.line_count; l++) current_view_tab.lines[l].is_highlight = false;
                        xSemaphoreGive(irc_mutex);
                    }
                }
                return;
            }
        }
    } else {
        // Navigator Hub override: side-by-side Server & Channel navigation
        if (current_app_mode == MODE_NAVIGATOR) {
            // Left/Right switch focus between columns
            if (is_fn && M5Cardputer.Keyboard.isKeyPressed(';')) { // Fn+Left Arrow
                nav_focus_column = 0;
                ui_needs_redraw = true;
                return;
            }
            if (is_fn && M5Cardputer.Keyboard.isKeyPressed('/')) { // Fn+Right Arrow
                nav_focus_column = 1;
                ui_needs_redraw = true;
                return;
            }
            // Up/Down increment active column row pointer
            if (is_fn && M5Cardputer.Keyboard.isKeyPressed(',')) { // Fn+Up
                if (nav_focus_column == 0) {
                    if (nav_server_select_idx > 0) nav_server_select_idx--;
                } else {
                    if (nav_channel_select_idx > 0) nav_channel_select_idx--;
                }
                ui_needs_redraw = true;
                return;
            }
            if (is_fn && M5Cardputer.Keyboard.isKeyPressed('.')) { // Fn+Down
                if (nav_focus_column == 0) {
                    if (nav_server_select_idx < (int)discovered_network_count) nav_server_select_idx++;
                    // clamp channel index when server changes
                    nav_channel_select_idx = 0;
                } else {
                    // count channels for selected server (handle ALL case)
                    int ch_cnt = 0;
                    for (int i = 0; i < gTabCount; i++) {
                        if (strcmp(gTabs[i].name, "~mentions")==0) continue;
                        bool vis = (nav_server_select_idx == 0) || strcasecmp(gTabs[i].server, discovered_networks[nav_server_select_idx - 1]) == 0;
                        if (vis && nav_filter[0]) {
                            char ln[32]; strncpy(ln, gTabs[i].name,31); ln[31]='\0'; for(char*p=ln;*p;p++) *p=tolower(*p);
                            char lf[16]; strncpy(lf, nav_filter,15); lf[15]='\0'; for(char*p=lf;*p;p++) *p=tolower(*p);
                            char ls[32]; strncpy(ls, gTabs[i].server,31); ls[31]='\0'; for(char*p=ls;*p;p++) *p=tolower(*p);
                            if (!strstr(ln, lf) && !strstr(ls, lf)) vis=false;
                        }
                        if (vis) ch_cnt++;
                    }
                    if (nav_channel_select_idx < ch_cnt - 1) nav_channel_select_idx++;
                }
                ui_needs_redraw = true;
                return;
            }
            // Filter toggle Fn+F, pin toggle Fn+S
            if (is_fn && M5Cardputer.Keyboard.isKeyPressed('f')) {
                nav_filter_active = !nav_filter_active;
                nav_filter[0]='\0';
                ui_needs_redraw=true; return;
            }
            if (nav_filter_active) {
                if (status.del && strlen(nav_filter)>0) { nav_filter[strlen(nav_filter)-1]='\0'; ui_needs_redraw=true; return; }
                for(auto c: status.word) { if(strlen(nav_filter)<15){ size_t l=strlen(nav_filter); nav_filter[l]=c; nav_filter[l+1]='\0'; ui_needs_redraw=true; } }
                if (status.enter) { nav_filter_active=false; ui_needs_redraw=true; return; }
            }
            if (is_fn && M5Cardputer.Keyboard.isKeyPressed('s')) {
                int chan_cnt=0; int target=-1;
                for(int i=0;i<gTabCount;i++){ if(strcmp(gTabs[i].name,"~mentions")==0) continue; bool vis=(nav_server_select_idx==0)||strcasecmp(gTabs[i].server, discovered_networks[nav_server_select_idx-1])==0; if(nav_filter[0]){ char ln[32]; strncpy(ln,gTabs[i].name,31); for(char*p=ln;*p;p++) *p=tolower(*p); char lf[16]; strncpy(lf,nav_filter,15); for(char*p=lf;*p;p++) *p=tolower(*p); if(!strstr(ln,lf) && !strstr(gTabs[i].server,lf)) vis=false; } if(vis){ if(chan_cnt==nav_channel_select_idx) {target=i; break;} chan_cnt++; } } if(target>=0){ gTabs[target].pinned=!gTabs[target].pinned; ui_needs_redraw=true; } return;
            }
            if (is_fn && M5Cardputer.Keyboard.isKeyPressed('m')) {
                int chan_cnt2=0; int target2=-1;
                for(int i=0;i<gTabCount;i++){
                    if(strcmp(gTabs[i].name,"~mentions")==0) continue;
                    bool vis2=(nav_server_select_idx==0)||strcasecmp(gTabs[i].server, discovered_networks[nav_server_select_idx-1])==0;
                    if(vis2 && nav_filter[0]){
                        char ln2[32]; strncpy(ln2,gTabs[i].name,31); ln2[31]='\0'; for(char*p=ln2;*p;p++) *p=tolower(*p);
                        char lf2[16]; strncpy(lf2,nav_filter,15); lf2[15]='\0'; for(char*p=lf2;*p;p++) *p=tolower(*p);
                        char ls2[32]; strncpy(ls2,gTabs[i].server,31); ls2[31]='\0'; for(char*p=ls2;*p;p++) *p=tolower(*p);
                        if(!strstr(ln2,lf2) && !strstr(ls2,lf2)) vis2=false;
                    }
                    if(vis2){ if(chan_cnt2==nav_channel_select_idx) {target2=i; break;} chan_cnt2++; }
                }
                if(target2>=0){ gTabs[target2].muted = !gTabs[target2].muted; ui_needs_redraw=true; if(gTabs[target2].muted) queueLed(29,400); }
                return;
            }
            // Enter maps to current_tab_index
            if (status.enter) {
                int channel_print_counter = 0;
                for (int i = 0; i < gTabCount; i++) {
                    if (strcmp(gTabs[i].name, "~mentions")==0) continue;
                    if ( (nav_server_select_idx==0) || strcmp(gTabs[i].server, discovered_networks[nav_server_select_idx-1]) == 0) {
                        if (channel_print_counter == nav_channel_select_idx) {
                            current_tab_index = i;
                            current_app_mode = MODE_CHAT;
                            ui_needs_redraw = true;
                            set_led_mode(18);
                            // Clear highlights automatically upon entering or cycling active channel views
                            if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                                Tab &current_view_tab = gTabs[current_tab_index];
                                for (int l = 0; l < current_view_tab.line_count; l++) {
                                    current_view_tab.lines[l].is_highlight = false;
                                }
                                xSemaphoreGive(irc_mutex);
                            }
                            return;
                        }
                        channel_print_counter++;
                    }
                }
                return;
            }
        }
    // ==========================================
    // 🎮 TRUE HARDWARE PHYSICAL D-PAD LAYER (ACTIVE IN MENUS)
    // ==========================================
    if (current_app_mode != MODE_CHAT) {
        
        // 1. VERTICAL SCROLLING AXIS (Semicolon = UP | Period = DOWN)
        if (M5Cardputer.Keyboard.isKeyPressed(';')) { // Physical UP Key
            if (current_app_mode == MODE_NAVIGATOR) {
                if (nav_channel_select_idx > 0) { nav_channel_select_idx--; ui_needs_redraw = true; }
            } else {
                if (menu_selection_idx > 0) { menu_selection_idx--; ui_needs_redraw = true; }
            }
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('.')) { // Physical DOWN Key
            if (current_app_mode == MODE_NAVIGATOR) {
                int total_chans = 0;
                for (int i = 0; i < gTabCount; i++) {
                    if (strcmp(gTabs[i].name, "~mentions")==0) continue;
                    bool vis = (nav_server_select_idx == 0 || strcmp(gTabs[i].server, discovered_networks[nav_server_select_idx - 1]) == 0);
                    if (vis && nav_filter[0]) {
                        char ln[32]; strncpy(ln, gTabs[i].name,31); ln[31]='\0'; for(char*p=ln;*p;p++) *p=tolower(*p);
                        char lf[16]; strncpy(lf, nav_filter,15); lf[15]='\0'; for(char*p=lf;*p;p++) *p=tolower(*p);
                        char ls[32]; strncpy(ls, gTabs[i].server,31); ls[31]='\0'; for(char*p=ls;*p;p++) *p=tolower(*p);
                        if (!strstr(ln, lf) && !strstr(ls, lf) && !strstr(gTabs[i].topic, lf)) vis=false;
                    }
                    if (vis) total_chans++;
                }
                if (nav_channel_select_idx < total_chans - 1) { nav_channel_select_idx++; ui_needs_redraw = true; }
            } else {
                // Determine our menu navigation boundary max depending on active layout modes (WIFI expands with scan HUD)
                int max_limit;
                if (current_app_mode == MODE_WIFI) {
                    int sc = WiFi.scanComplete();
                    if (sc > 0) { if (sc>5) sc=5; max_limit = 4 + sc; } else max_limit = 4;
                } else if (current_app_mode == MODE_SETTINGS) max_limit = 3;
                else if (current_app_mode == MODE_THEME) max_limit = 4;
                else if (current_app_mode == MODE_LOGS) max_limit = 9;
                else if (current_app_mode == MODE_WHOIS) max_limit = 0;
                else max_limit = 2;
                if (menu_selection_idx < max_limit) { menu_selection_idx++; ui_needs_redraw = true; }
            }
            return;
        }

        // 2. HORIZONTAL ADJUSTMENT AXIS (Comma = LEFT / DECREMENT | Forward Slash = RIGHT / INCREMENT)
        if (M5Cardputer.Keyboard.isKeyPressed(',') || M5Cardputer.Keyboard.isKeyPressed('/')) {
            bool forward = M5Cardputer.Keyboard.isKeyPressed('/'); // Right = True, Left = False
            ui_needs_redraw = true;
            
            if (current_app_mode == MODE_NAVIGATOR) {
                nav_server_select_idx += forward ? 1 : -1;
                if (nav_server_select_idx > discovered_network_count) nav_server_select_idx = 0;
                if (nav_server_select_idx < 0) nav_server_select_idx = discovered_network_count;
                nav_channel_select_idx = 0; // Clear rows index step on server swap
            }
            else if (current_app_mode == MODE_SETTINGS) {
                if (menu_selection_idx == 0) { // Timezone Offset Index
                    current_tz_idx += forward ? 1 : -1;
                    if (current_tz_idx > 14) current_tz_idx = -12;
                    if (current_tz_idx < -12) current_tz_idx = 14;
                    sync_ntp_timezone();
                }
                else if (menu_selection_idx == 1) { use_12_hour_format = !use_12_hour_format; }
                else if (menu_selection_idx == 2) { use_dst = !use_dst; sync_ntp_timezone(); }
                else if (menu_selection_idx == 3) { channel_log_enabled = !channel_log_enabled; }
            } else if (current_app_mode == MODE_THEME) {
                if (menu_selection_idx == 0) { use_light_theme = !use_light_theme; }
                else if (menu_selection_idx == 1) { theme_accent = (theme_accent + (forward?1:-1) +4)%4; }
                else if (menu_selection_idx == 2) { text_scale = (text_scale==1?2:1); }
                else if (menu_selection_idx == 3) { speaker_enabled = !speaker_enabled; if(speaker_enabled) M5.Speaker.tone(800,80); }
                else if (menu_selection_idx == 4) { sound_profile = (sound_profile + (forward?1:-1) +3)%3; }
                else if (menu_selection_idx == 3) { speaker_enabled = !speaker_enabled; if(speaker_enabled) M5.Speaker.tone(800,80); }
            }
            return;
        }
                if (status.enter && current_app_mode == MODE_NAVIGATOR) { // Enter Selection Core Handler
            int chan_match_counter = 0;
            int targeted_index_slot = -1;

            for (int i = 0; i < gTabCount; i++) {
                if (strcmp(gTabs[i].name, "~mentions")==0) continue;
                // Precision Aligned Evaluation Gate matching our draw_chat_view screen layout rules
                bool is_room_visible = (nav_server_select_idx == 0) || 
                                       (strcmp(gTabs[i].server, discovered_networks[nav_server_select_idx - 1]) == 0);
                if (is_room_visible && nav_filter[0]) {
                    char ln[32]; strncpy(ln, gTabs[i].name,31); ln[31]='\0'; for(char*p=ln;*p;p++) *p=tolower(*p);
                    char lf[16]; strncpy(lf, nav_filter,15); lf[15]='\0'; for(char*p=lf;*p;p++) *p=tolower(*p);
                    char ls[32]; strncpy(ls, gTabs[i].server,31); ls[31]='\0'; for(char*p=ls;*p;p++) *p=tolower(*p);
                    if (!strstr(ln, lf) && !strstr(ls, lf) && !strstr(gTabs[i].topic, lf)) is_room_visible=false;
                }
                if (is_room_visible && nav_filter[0]) {
                    char ln[32]; strncpy(ln, gTabs[i].name,31); ln[31]='\0'; for(char*p=ln;*p;p++) *p=tolower(*p);
                    char lf[16]; strncpy(lf, nav_filter,15); lf[15]='\0'; for(char*p=lf;*p;p++) *p=tolower(*p);
                    char ls[32]; strncpy(ls, gTabs[i].server,31); ls[31]='\0'; for(char*p=ls;*p;p++) *p=tolower(*p);
                    if (!strstr(ln, lf) && !strstr(ls, lf) && !strstr(gTabs[i].topic, lf)) is_room_visible=false;
                }
                
                if (is_room_visible) {
                    if (chan_match_counter == nav_channel_select_idx) { 
                        targeted_index_slot = i; // Secure our clean absolute structural RAM position match
                        break; 
                    }
                    chan_match_counter++;
                }
            }
            
            // Assign the verified index pointer safely if a structural match was secured
            if (targeted_index_slot != -1 && targeted_index_slot < MAX_TABS) {
                current_tab_index = targeted_index_slot;
                scrollback_offset_idx = 0; // Reset scrollback offsets instantly on room swap
                network_handshake_complete[current_tab_index] = true; // Clear out stuck LED states instantly
                
                // Defensive Shield: Only claim thread locks if the target channel actually contains data lines
                if (gTabs[current_tab_index].line_count > 0 && irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    for (int l = 0; l < gTabs[current_tab_index].line_count; l++) {
                        gTabs[current_tab_index].lines[l].is_highlight = false; 
                    }
                    xSemaphoreGive(irc_mutex);
                }
            }
            
            current_app_mode = MODE_CHAT; 
            ui_needs_redraw = true; 
            return;
        }
    }
    }
    // Layer E: Configuration Menu Form Row Value-Changing Handlers (brightness fixed 80, no user row)
    if (current_app_mode == MODE_SETTINGS && status.enter) {
        if (menu_selection_idx == 0) { // Row 1: Shift Timezone Indicator Offset index
            current_tz_idx = (current_tz_idx + 1);
            if (current_tz_idx > 14) current_tz_idx = -12; // Wrap across international date lines safely
            sync_ntp_timezone();
        }
        else if (menu_selection_idx == 1) { // Row 2: Toggle 12/24hr Display Format Layer
            use_12_hour_format = !use_12_hour_format;
        }
        else if (menu_selection_idx == 2) { // Row 3: DST Override
            use_dst = !use_dst; sync_ntp_timezone();
        }
        else if (menu_selection_idx == 3) { // Row 4: Toggle Local Channel Log File Recording
            channel_log_enabled = !channel_log_enabled;
        }
        ui_needs_redraw = true;
        return;
    }
    if (current_app_mode == MODE_THEME && status.enter) {
        if (menu_selection_idx == 0) { use_light_theme = !use_light_theme; }
        else if (menu_selection_idx == 1) { theme_accent = (theme_accent+1)%4; }
        else if (menu_selection_idx == 2) { text_scale = (text_scale==1?2:1); canvas.setTextSize(text_scale); }
        else if (menu_selection_idx == 3) { speaker_enabled = !speaker_enabled; if(speaker_enabled) M5.Speaker.tone(800,80); }
        else if (menu_selection_idx == 4) { sound_profile = (sound_profile+1)%3; }
        else if (menu_selection_idx == 3) { speaker_enabled = !speaker_enabled; if(speaker_enabled) M5.Speaker.tone(800,80); }
        ui_needs_redraw = true;
        return;
    }

        if (status.enter) {
            // --- CORE STATE 1: WORKSPACE NAVIGATION HUB SELECTIONS ---
            if (current_app_mode == MODE_NAVIGATOR) {
                int chan_match_counter = 0;
                int targeted_index_slot = -1;
                for (int i = 0; i < gTabCount; i++) {
                    if (strcmp(gTabs[i].name, "~mentions")==0) continue;
                    bool is_room_visible = (nav_server_select_idx == 0) || 
                                           (strcmp(gTabs[i].server, discovered_networks[nav_server_select_idx - 1]) == 0);
                if (is_room_visible && nav_filter[0]) {
                    char ln[32]; strncpy(ln, gTabs[i].name,31); ln[31]='\0'; for(char*p=ln;*p;p++) *p=tolower(*p);
                    char lf[16]; strncpy(lf, nav_filter,15); lf[15]='\0'; for(char*p=lf;*p;p++) *p=tolower(*p);
                    char ls[32]; strncpy(ls, gTabs[i].server,31); ls[31]='\0'; for(char*p=ls;*p;p++) *p=tolower(*p);
                    if (!strstr(ln, lf) && !strstr(ls, lf) && !strstr(gTabs[i].topic, lf)) is_room_visible=false;
                }
                    if (is_room_visible) {
                        if (chan_match_counter == nav_channel_select_idx) { targeted_index_slot = i; break; }
                        chan_match_counter++;
                    }
                }
                if (targeted_index_slot != -1 && targeted_index_slot < MAX_TABS) {
                    current_tab_index = targeted_index_slot;
                    scrollback_offset_idx = 0;
                    network_handshake_complete[current_tab_index] = true;
                    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                        for (int l = 0; l < gTabs[current_tab_index].line_count; l++) gTabs[current_tab_index].lines[l].is_highlight = false;
                        xSemaphoreGive(irc_mutex);
                    }
                }
                current_app_mode = MODE_CHAT; ui_needs_redraw = true; return;
            }

            // --- CORE STATE 2: CONFIGURATION FIELD BUTTON TRIGGERS ---
            if (current_app_mode == MODE_WIFI || current_app_mode == MODE_SETTINGS || current_app_mode == MODE_BOUNCER) {
                if (current_app_mode == MODE_WIFI && menu_selection_idx == 4) { // Target the 'Scan Airwaves [ RUN ]' Row
                    Serial.println("[WIFI] Initializing non-blocking background async airwaves scan pass...");
                    
                    set_led_mode(19); // Flash Lime Green activity burst to confirm action receipt
                    
                    // Force an asynchronous, passive spectrum scan to prevent UI freezes and socket timeouts!
                    WiFi.scanNetworks(true, true); 
                    
                    ui_needs_redraw = true;
                    return;
                }
                if (current_app_mode == MODE_WIFI && menu_selection_idx >= 5) {
                    int sc = WiFi.scanComplete();
                    if (sc > 0) {
                        int sel = menu_selection_idx - 5;
                        if (sel >=0 && sel < sc) {
                            String ssid = WiFi.SSID(sel);
                            const char* pass = "";
                            for(int i=0;i<wifi_vault_count;i++) if(ssid == wifi_vault_ssid[i]) { pass = wifi_vault_pass[i]; break; }
                            if (strlen(pass)==0) {
                                if (ssid == String(wifi_ssid)) pass = wifi_pass;
                                else if (ssid == String(wifi_ssid2)) pass = wifi_pass2;
                            }
                            Serial.printf("[VAULT-HUD] Quick-reconnect %s\n", ssid.c_str());
                            WiFi.begin(ssid.c_str(), pass);
                            save_wifi_vault_lru(ssid.c_str(), pass);
                            set_led_mode(9);
                            WiFi.scanDelete();
                            ui_needs_redraw = true;
                            return;
                        }
                    }
                }
                if (current_app_mode == MODE_LOGS) {
                    bool sdL=false; if(sd_mutex) sdL=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(50))==pdTRUE); else sdL=true;
                    File dir = SD.open("/irc/logs");
                    if (dir && dir.isDirectory()) {
                        // collect up to 10 logs - char heap-safe
                        char logs[10][64]={0}; int cnt=0;
                        File f = dir.openNextFile();
                        while(f && cnt<10){ if(!f.isDirectory()){ strncpy(logs[cnt], f.path(),63); cnt++; } f.close(); f=dir.openNextFile(); esp_task_wdt_reset(); }
                        dir.close();
                        if(sdL && sd_mutex) xSemaphoreGive(sd_mutex);
                        if (menu_selection_idx < cnt) {
                            char p[64]; strncpy(p, logs[menu_selection_idx],63);
                            bool sdL2=false; if(sd_mutex) sdL2=(xSemaphoreTake(sd_mutex,pdMS_TO_TICKS(50))==pdTRUE); else sdL2=true;
                            File lf = SD.open(p);
                            if (lf) {
                                for(int i=0;i<5 && lf.available(); i++){
                                    char line[128]; int len=lf.readBytesUntil('\n', line, sizeof(line)-1);
                                    if(len<=0) break; line[len]='\0';
                                    add_message_to_buffer("LOG", line, 0x7BEF);
                                    esp_task_wdt_reset();
                                }
                                lf.close();
                            }
                            if(sdL2 && sd_mutex) xSemaphoreGive(sd_mutex);
                            current_app_mode = MODE_CHAT;
                            ui_needs_redraw = true;
                            return;
                        }
                    }
                }
            }
        }

    // Nicklist drawer toggle: Fn+Enter in chat (does not send) - edge triggered
    {
        static bool was_nick_prev=false;
        bool cur_nick = (current_app_mode == MODE_CHAT && is_fn && status.enter && !M5Cardputer.Keyboard.isKeyPressed(' '));
        if(cur_nick && !was_nick_prev){
            show_nicklist = !show_nicklist;
            ui_needs_redraw = true;
            set_led_mode(18);
            was_nick_prev=true;
            return;
        }
        if(!cur_nick) was_nick_prev=false;
        if(cur_nick) { esp_task_wdt_reset(); return; }
    }

    // Search bar Fn+S in chat, Copy last line Fn+C - edge triggered
    {
        static bool was_s_prev=false;
        bool cur_s = (current_app_mode == MODE_CHAT && is_fn && M5Cardputer.Keyboard.isKeyPressed('s'));
        if(cur_s && !was_s_prev){
            search_active = !search_active;
            if (!search_active) search_query[0]='\0';
            else search_query[0]='\0';
            ui_needs_redraw=true;
            was_s_prev=true;
            return;
        }
        if(!cur_s) was_s_prev=false;
        if(cur_s) { esp_task_wdt_reset(); return; }
    }
    if (search_active && current_app_mode == MODE_CHAT) {
        if (status.del && strlen(search_query)>0) { search_query[strlen(search_query)-1]='\0'; ui_needs_redraw=true; return; }
        for(auto c: status.word) { if(strlen(search_query)<15){ size_t l=strlen(search_query); search_query[l]=c; search_query[l+1]='\0'; ui_needs_redraw=true; } }
        if (status.enter) { search_active=false; ui_needs_redraw=true; return; }
        if (M5Cardputer.Keyboard.isKeyPressed('`')) { search_active=false; search_query[0]='\0'; ui_needs_redraw=true; return; }
        ui_needs_redraw=true; return;
    }
    {
        static bool was_r_prev=false;
        bool cur_r = (current_app_mode == MODE_CHAT && is_fn && M5Cardputer.Keyboard.isKeyPressed('r'));
        if(cur_r && !was_r_prev){
            request_network_reload = true;
            set_led_mode(13);
            add_message_to_buffer("ClientCore", "Reloading bouncer networks...", 0x7BEF);
            ui_needs_redraw=true;
            was_r_prev=true;
            return;
        }
        if(!cur_r) was_r_prev=false;
        if(cur_r) { esp_task_wdt_reset(); return; }
    }
    {
        static bool was_c_prev=false;
        bool cur_c = (current_app_mode == MODE_CHAT && is_fn && M5Cardputer.Keyboard.isKeyPressed('c'));
        if(cur_c && !was_c_prev){
            if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5))==pdTRUE) {
                Tab &t = gTabs[current_tab_index];
                if (t.line_count>0) {
                    const char* lastMsg = t.lines[t.line_count-1].message;
                    strncpy(search_query, lastMsg, 15);
                    input_buffer = String(lastMsg);
                    ui_needs_redraw=true;
                }
                xSemaphoreGive(irc_mutex);
            }
            set_led_mode(10);
            was_c_prev=true;
            return;
        }
        if(!cur_c) was_c_prev=false;
        if(cur_c) { esp_task_wdt_reset(); return; }
    }
    // History recall with plain ;/. (non-Fn) while in chat - throttled 120ms repeat
    if (current_app_mode == MODE_CHAT && !is_fn) {
        static unsigned long last_hist_ms=0;
        if (M5Cardputer.Keyboard.isKeyPressed(';') && input_history_len>0) {
            if(millis() - last_hist_ms < 120) { esp_task_wdt_reset(); return; }
            last_hist_ms = millis();
            if (input_history_pos==-1) input_history_pos = (input_history_head -1 +10)%10;
            else {
                int oldest = (input_history_head - input_history_len +10)%10;
                if (input_history_pos != oldest) input_history_pos = (input_history_pos -1 +10)%10;
            }
            input_buffer = String(input_history[input_history_pos]);
            ui_needs_redraw = true;
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('.') && input_history_len>0 && input_history_pos!=-1) {
            if(millis() - last_hist_ms < 120) { esp_task_wdt_reset(); return; }
            last_hist_ms = millis();
            int newest = (input_history_head -1 +10)%10;
            if (input_history_pos == newest) { input_history_pos=-1; input_buffer=""; }
            else { input_history_pos = (input_history_pos +1)%10; input_buffer = String(input_history[input_history_pos]); }
            ui_needs_redraw = true;
            return;
        }
    }

    // ==========================================
    // 💬 CHAT MODE PROCESSING PIPELINE (ONLY RUNS IF MODE_CHAT IS ACTIVE)
    // ==========================================
    if (current_app_mode == MODE_CHAT) {
        // Layer G: Tab Autocomplete Key Intercept Matrix (Physical Tab)
        if ((status.tab || M5Cardputer.Keyboard.isKeyPressed(KEY_TAB)) && input_buffer.length() > 0) {
            int last_space = input_buffer.lastIndexOf(' ');
            String partial_token = (last_space == -1) ? input_buffer : input_buffer.substring(last_space + 1);
            partial_token.toLowerCase();

            Tab &active_tab = gTabs[current_tab_index];
            String discovered_match = "";

            // Scan backwards through recent channel lines to find a matching nickname handle
            // Clamp line_count and use ring head for correct lookup
            {
                int eff = active_tab.line_count;
                if (eff > MSG_BUFFER_SIZE) eff = MSG_BUFFER_SIZE;
                for (int line_scan = eff - 1; line_scan >= 0; line_scan--) {
                    String candidate_nick = String(active_tab.lines[(active_tab.head + line_scan) % MSG_BUFFER_SIZE].nick);
                String lookup_lower = candidate_nick;
                lookup_lower.toLowerCase();

                if (lookup_lower.startsWith(partial_token) && candidate_nick != String(irc_nick)) {
                    discovered_match = candidate_nick;
                    break;
                }
                }
            }

            if (discovered_match.length() > 0) {
                // Strip the typed fragment and replace it with the true, completed nickname handle string
                if (last_space == -1) {
                    input_buffer = discovered_match + ": ";
                } else {
                    input_buffer = input_buffer.substring(0, last_space + 1) + discovered_match + ": ";
                }
                ui_needs_redraw = true;
                set_led_mode(21); // Cool Indigo Flare for autocomplete success
                return;
            }
        }
        // Handle Backspace deletions - throttled 80ms repeat to avoid hold flood
        if (status.del) { 
            static unsigned long last_del_ms=0;
            if(millis() - last_del_ms < 80) { esp_task_wdt_reset(); return; }
            last_del_ms = millis();
            if (input_buffer.length() > 0) {
                input_buffer.remove(input_buffer.length() - 1); 
                ui_needs_redraw = true; 
            } else {
                set_led_mode(5); 
            }
        }
        
        // Hold typematic: limit burst to 1 char, initial 350ms delay then 80ms repeat
        if (status.word.size() > 1) status.word.resize(1);
        static String last_hold_word2=""; static unsigned long last_hold_ms2=0;
        static bool first_repeat_done=false;
        String cur2;
        cur2.reserve(status.word.size());
        for(auto c: status.word) cur2 += c;
        if (cur2.length()>0 && cur2 == last_hold_word2) {
            unsigned long elapsed = millis() - last_hold_ms2;
            unsigned long threshold = first_repeat_done ? 80 : 350;
            if (elapsed < threshold) { esp_task_wdt_reset(); return; }
            first_repeat_done = true;
        } else {
            first_repeat_done = false;
        }
        if (cur2.length()>0) { last_hold_word2=cur2; last_hold_ms2=millis(); }
        // Append standard printable characters into the buffer
        for (auto c : status.word) {
            // Filter all Fn combos to avoid polluting input when Fn is held (Fn+P/O/I/Q/L/R/S/C/B/M/F etc plus arrows ,./;)
            if (is_fn && (c == ';' || c == '/' || c == ',' || c == '.' || c == 'p' || c == 'o' || c == 'i' || c == 'q' || c == 'l' || c == 'r' || c == 's' || c == 'c' || c == 'b' || c == 'm' || c == 'f')) continue;
            if (input_buffer.length() < 200) { 
                input_buffer += c; 
                input_history_pos=-1;
                ui_needs_redraw = true; 
            }
        }
        // Close nicklist drawer on plain Enter when open (no send)
        if (show_nicklist && status.enter && !is_fn && current_app_mode==MODE_CHAT) {
            show_nicklist=false;
            ui_needs_redraw=true;
            return;
        }
        
        if (status.enter && input_buffer.length() > 0) {
            const char* active_net = gTabs[current_tab_index].server;
            WiFiClientSecure* target_socket = nullptr;
            int current_net_idx = -1;

            // Isolate the active parallel network socket handle
            for (int i = 0; i < discovered_network_count; i++) {
                if (strcmp(discovered_networks[i], active_net) == 0) {
                    target_socket = &clients[i];
                    current_net_idx = i;
                    break;
                }
            }

            if (target_socket && target_socket->connected()) {
                if (input_buffer.startsWith("/")) {
                    // --- LOCAL SLASH COMMAND ENGINE ---
                    int space_idx = input_buffer.indexOf(' ');
                    String cmd = (space_idx == -1) ? input_buffer.substring(1) : input_buffer.substring(1, space_idx);
                    cmd.toUpperCase();
                    String args = (space_idx == -1) ? "" : input_buffer.substring(space_idx + 1);
                    args.trim();

                    const char* ali = expand_alias(cmd.c_str());
                    if (ali) {
                        String aliStr = String(ali);
                        int sp2 = aliStr.indexOf(' ');
                        String aliCmd = (sp2==-1)? aliStr : aliStr.substring(0, sp2);
                        String aliArgs = (sp2==-1)? "" : aliStr.substring(sp2+1);
                        String fullArgs = aliArgs + (args.length()? " "+args : "");
                        aliCmd.toUpperCase();
                        if (aliCmd == "JOIN" && fullArgs.length()>0) { target_socket->printf("JOIN %s\r\n", fullArgs.c_str()); target_socket->flush(); }
                        else if (aliCmd == "PART") { String tc = (fullArgs.length()>0)? fullArgs : gTabs[current_tab_index].name; target_socket->printf("PART %s\r\n", tc.c_str()); target_socket->flush(); }
                        else { target_socket->printf("%s %s\r\n", aliCmd.c_str(), fullArgs.c_str()); target_socket->flush(); }
                        add_message_to_buffer("ClientCore", ("Aliased "+ aliStr).c_str(), 0x07E0);
                        set_led_mode(20);
                    } else if (cmd == "JOIN" && args.length() > 0) {
                        target_socket->printf("JOIN %s\r\n", args.c_str());
                        target_socket->flush(); // Force immediate protocol delivery pass
                    }
                    else if (cmd == "PART") {
                        // If no argument is passed, part the channel you are currently viewing
                        String target_chan = (args.length() > 0) ? args : gTabs[current_tab_index].name;
                        target_socket->printf("PART %s\r\n", target_chan.c_str());
                        target_socket->flush();
                    }
                    else if (cmd == "NICK" && args.length() > 0) {
                        target_socket->printf("NICK %s\r\n", args.c_str());
                        target_socket->flush();
                        update_config_string(irc_nick, args.c_str(), sizeof(irc_nick)); // Dynamic RAM update
                    }
                    else if (cmd == "MSG" && args.length() > 0) {
                        // Direct Messaging format: /msg Nickname message text...
                        int msg_space = args.indexOf(' ');
                        if (msg_space != -1) {
                            String target_nick = args.substring(0, msg_space);
                            String private_txt = args.substring(msg_space + 1);
                            target_socket->printf("PRIVMSG %s :%s\r\n", target_nick.c_str(), private_txt.c_str());
                            target_socket->flush();
                            add_message_to_buffer(target_nick.c_str(), private_txt.c_str(), 0xF81F); // Render text in hot pink DM colors
                        }
                    }
                    else if (cmd == "ALIAS" && args.length() > 0) {
                        int sp = args.indexOf(' ');
                        if (sp==-1) { add_message_to_buffer("ClientCore", "Usage: /alias name=cmd or /alias name cmd", 0xF800); }
                        else {
                            String n = args.substring(0, sp); String v = args.substring(sp+1);
                            n.trim(); v.trim(); 
                            int eq = n.indexOf('=');
                            if(eq!=-1){ String nn=n.substring(0,eq); String vv=n.substring(eq+1); if(vv.length()) v=vv+" "+v; n=nn; n.trim(); v.trim(); }
                            if(n.length() && v.length()){
                                bool upd=false;
                                for(int i=0;i<alias_count;i++) if(strcasecmp(alias_names[i], n.c_str())==0){ strncpy(alias_cmds[i], v.c_str(),63); upd=true; break; }
                                if(!upd && alias_count<5){ strncpy(alias_names[alias_count], n.c_str(),15); strncpy(alias_cmds[alias_count], v.c_str(),63); alias_count++; }
                                save_alias_list();
                                add_message_to_buffer("ClientCore", (String("Alias ")+(upd?"updated":"added")+": "+n+"="+v).c_str(), 0x07E0);
                            }
                        }
                    }
                    else if (cmd == "LIST") {
                        chan_list_count=0;
                        target_socket->printf("LIST\r\n");
                        target_socket->flush();
                        add_message_to_buffer("ClientCore", "Listing channels (10 max)...", 0x07E0);
                    }
                    else if (cmd == "HIGHLIGHT" && args.length() > 0) {
                        String w = args; w.trim();
                        if (w.startsWith("add ")) w = w.substring(4);
                        else if (w.startsWith("del ")) {
                            String del = w.substring(4); del.trim();
                            for(int i=0;i<highlight_count;i++) if(strcasecmp(highlight_words[i], del.c_str())==0){
                                for(int j=i;j<highlight_count-1;j++) strncpy(highlight_words[j], highlight_words[j+1],32);
                                highlight_count--; save_highlight_list(); add_message_to_buffer("ClientCore", "Highlight removed", 0xFFFF); break;
                            }
                            w="";
                        }
                        w.trim();
                        if(w.length()>0 && w.length()<32 && highlight_count<8){
                            bool exists=false; for(int i=0;i<highlight_count;i++) if(strcasecmp(highlight_words[i], w.c_str())==0) exists=true;
                            if(!exists){ strncpy(highlight_words[highlight_count], w.c_str(),31); highlight_count++; save_highlight_list(); add_message_to_buffer("ClientCore", ("Highlight added: "+w).c_str(), 0x07E0); }
                        } else if(w.length()>=32) add_message_to_buffer("ClientCore", "Highlight too long", 0xF800);
                        else if(highlight_count>=8) add_message_to_buffer("ClientCore", "Highlight list full (8)", 0xF800);
                    }
                    else if (cmd == "IGNORE" && args.length() > 0) {
                        bool found=false; int idx=-1;
                        for(int i=0;i<ignore_count;i++) if(strcasecmp(ignore_list[i], args.c_str())==0){ found=true; idx=i; break; }
                        if(found){
                            for(int j=idx;j<ignore_count-1;j++) strncpy(ignore_list[j], ignore_list[j+1], 16);
                            ignore_count--; ignore_list[ignore_count][0]='\0';
                            add_message_to_buffer("ClientCore", "Unignored", 0xFFFF);
                        } else {
                            if(ignore_count < 8){ strncpy(ignore_list[ignore_count], args.c_str(), 15); ignore_list[ignore_count][15]='\0'; ignore_count++; add_message_to_buffer("ClientCore", "Ignored", 0xFFFF); }
                            else add_message_to_buffer("ClientCore", "Ignore list full (8)", 0xF800);
                        }
                        save_ignore_list();
                    }
                    else if (cmd == "RAW" && args.length() > 0) {
                        // Power User Escape Hatch: Transmit un-filtered protocol lines straight down the wire
                        target_socket->printf("%s\r\n", args.c_str());
                        target_socket->flush(); // Power User flush
                    }
                    else {
                        add_message_to_buffer("ClientCore", "Unknown local slash command protocol instruction.", 0xF800);
                    }
                    set_led_mode(20); // Warm Coral Strobe for slash macro
                } else {
                    // --- STANDARD CHAT TEXT TRANSMISSION PATH ---
                    target_socket->printf("PRIVMSG %s :%s\r\n", gTabs[current_tab_index].name, input_buffer.c_str());
                    target_socket->flush(); // Secure transmission delivery pipeline
                    add_message_to_buffer(irc_nick, input_buffer.c_str(), 0xFFFF);
                }
            }
            // QoL: push to history ring (skip slash, dedup) and SD
            push_input_history(input_buffer.c_str());
            input_buffer = "";
            input_history_pos=-1;
            ui_needs_redraw = true;
        }
    }
}

void irc_network_task(void* pvParameters) {
    static bool wifi_initialized = false;
    static WiFiClientSecure master_client;

    while (true) {
        yield(); vTaskDelay(pdMS_TO_TICKS(10)); // 10ms poll for low-latency bouncer relay (was 50ms -> long delays)
        if (request_network_reload) {
            request_network_reload=false;
            master_scan_complete_global=false;
            discovered_network_count=0;
            memset(discovered_networks,0,sizeof(discovered_networks));
            master_client.stop();
            for(int i=0;i<MAX_NETWORKS;i++){ clients[i].stop(); network_handshake_complete[i]=false; network_reconnect_cooldown[i]=0; }
            add_message_to_buffer("ClientCore", "Network cache cleared, rediscovering...", 0x7BEF);
            ui_needs_redraw=true;
            set_led_mode(13);
        }
        if (safe_mode_active || bnc_port == 0) continue;

        // STEP 0: ASYNCHRONOUS BACKGROUND WI-FI INITIALIZATION
        if (!wifi_initialized) {
            WiFi.disconnect(true); vTaskDelay(pdMS_TO_TICKS(100)); 
            WiFi.mode(WIFI_STA);
            if (strlen(wifi_ssid) > 0) {
                WiFi.begin((const char*)wifi_ssid, (const char*)wifi_pass);
                Serial.printf("[NET-INIT] Background wireless radio launched for SSID: %s\n", wifi_ssid);
            }
            wifi_initialized = true;
            continue;
        }

        // Non-blocking passive vault scan completion poll (SD vault history match)
        handle_vault_scan_complete();
        if (WiFi.status() != WL_CONNECTED) {
            set_led_mode(9); // Orange Network Fault Light
            if (current_app_mode == MODE_SETTINGS || current_app_mode == MODE_WIFI) {
                // Abort failover to protect active user configurations
            } else {
                if (last_wifi_fail_tick == 0) last_wifi_fail_tick = millis();
                if (millis() - last_wifi_fail_tick > 10000) {
                    int sc = WiFi.scanComplete();
                    if (sc == -2) {
                        // scan already in progress - wait
                    } else {
                        Serial.println("[VAULT-ROAM] 10s disconnect window - triggering async passive scan");
                        WiFi.scanNetworks(true, true);
                        set_led_mode(9);
                        last_wifi_fail_tick = millis();
                    }
                }
            }
            continue;
        }
        last_wifi_fail_tick = 0; // Reset ticker once connection settles healthy
        sync_ntp_timezone();
        // LRU cache rotator: on successful auth via Wi-Fi Manager, vault learns new AP
        {
            static String lastConnectedSSID = "";
            String cur = WiFi.SSID();
            if (cur.length()>0 && cur != lastConnectedSSID) {
                bool known=false;
                for(int i=0;i<wifi_vault_count;i++) if(cur == wifi_vault_ssid[i]) known=true;
                if(!known){
                    const char* pass = "";
                    // try to resolve pass from config or vault
                    if (cur == String(wifi_ssid)) pass = wifi_pass;
                    else if (cur == String(wifi_ssid2)) pass = wifi_pass2;
                    save_wifi_vault_lru(cur.c_str(), pass);
                    Serial.printf("[VAULT-LRU] Learned new AP %s\n", cur.c_str());
                }
                lastConnectedSSID = cur;
            }
        }

        // STEP 1: INITIAL PASS - EXTRACT NETWORKS DYNAMICALLY FROM THE BOUNCER
        if (!master_scan_complete_global) {
            if (!master_client.connected()) {
                master_client.setInsecure();
                if (master_client.connect(bnc_host, bnc_port)) {
                    // Send raw root credentials to trigger the bouncer's available network notice
                    master_client.printf("PASS %s:%s\r\n", bnc_user, bnc_pass);
                    master_client.printf("NICK %s\r\n", irc_nick);
                    master_client.printf("USER %s 0 * :M5 Discovery\r\n", bnc_user);
                }
            }

            if (master_client.connected() && master_client.available()) {
                String line = master_client.readStringUntil('\n');
                line.trim(); line.replace("\r", "");

                // Intercept the bouncer's active listing notification string at runtime
                int avail_idx = line.indexOf("Available: ");
                if (avail_idx != -1) {
                    String net_list = line.substring(avail_idx + 11);
                    
                    // Tokenize the server's text stream comma-by-comma on the fly
                    int start_pos = 0;
                    while (start_pos < net_list.length()) {
                        // Overflow Shield: Prevent bouncer from spilling past our MAX_NETWORKS memory boundaries
                        if (discovered_network_count >= MAX_NETWORKS) {
                            Serial.println("[WARN] Bouncer roster exceeds MAX_NETWORKS. Truncating allocations safely.");
                            break;
                        }
                        
                        int comma_idx = net_list.indexOf(',', start_pos);
                        String net_name = (comma_idx == -1) ? net_list.substring(start_pos) : net_list.substring(start_pos, comma_idx);
                        net_name.trim();
                        
                        if (net_name.length() > 0) { 
                            strncpy(discovered_networks[discovered_network_count], net_name.c_str(), 31); 
                            discovered_network_count++; 
                        }
                        if (comma_idx == -1) break; 
                        start_pos = comma_idx + 1;
                    }
                    if(discovered_network_count==0){
                        // Fallback: bouncer didn't send Available:, use bnc_host as single network
                        strncpy(discovered_networks[0], bnc_host, 31);
                        discovered_network_count=1;
                        Serial.println("[NET] No Available: line, fallback to bnc_host");
                    }
                    master_client.stop(); // Close the discovery probe safely
                    master_scan_complete_global = true;
                    Serial.printf("[NET] Dynamic discovery complete. Isolated %d networks from bouncer.\n", discovered_network_count);
                }
            }
            // Timeout fallback if bouncer never sends Available: (no channels populating)
            {
                static unsigned long scan_start_ms = 0;
                if (scan_start_ms==0) scan_start_ms=millis();
                if (!master_scan_complete_global && discovered_network_count==0 && millis()-scan_start_ms>3000) {
                    strncpy(discovered_networks[0], bnc_host, 31);
                    if (strlen(bnc_host)==0) strncpy(discovered_networks[0], "BNC", 31);
                    discovered_network_count=1;
                    master_scan_complete_global=true;
                    master_client.stop();
                    Serial.println("[NET] Available timeout fallback to bnc_host (3s)");
                    log_system("NET fallback bnc_host");
                }
                if (master_scan_complete_global) scan_start_ms=0;
            }
            continue;
        }

        // STEP 2: CONCURRENT PARALLEL SOCKETS ENGINE
        for (int i = 0; i < discovered_network_count; i++) {
            yield(); WiFiClientSecure &net_client = clients[i];
            
            // Re-authenticate and execute socket connections safely under rate-limited backoff guards
            if (!net_client.connected()) {
                network_authenticated[i] = false;
                network_handshake_complete[i] = false;
                
                // Rate-Limiter Shield: 2500ms cooldown (was 5000ms -> very long reconnect delays)
                if (millis() - network_reconnect_cooldown[i] < 2500) {
                    continue; // Skip this network index slot until its 5-second connection cooldown expires
                }
                
                network_reconnect_cooldown[i] = millis(); 

                // Predictive Heap Compaction: Evaluate largest free block before TLS re-alloc (outside mutex)
                if (ESP.getMinFreeHeap() < 45 * 1024) {
                    Serial.println("[HEAP] Low heap detected (<45KB), recycling old channel history buffers");
                    for (int t_idx = 0; t_idx < gTabCount; t_idx++) {
                        if (gTabs[t_idx].line_count > 15) {
                            int keep = 15;
                            ChatLine tmp[15];
                            for(int k=0;k<keep;k++) tmp[k]=gTabs[t_idx].lines[(gTabs[t_idx].head + gTabs[t_idx].line_count - keep + k) % MSG_BUFFER_SIZE];
                            for(int k=0;k<keep;k++) gTabs[t_idx].lines[k]=tmp[k];
                            for(int k=keep;k<MSG_BUFFER_SIZE;k++) memset(&gTabs[t_idx].lines[k],0,sizeof(ChatLine));
                            gTabs[t_idx].head=0;
                            gTabs[t_idx].line_count=keep;
                        }
                    }
                    heap_caps_check_integrity_all(true);
                } 
                
                net_client.stop(); // Forcefully purge old secure memory blocks first
                net_client.setInsecure();
                
                // Modern ESP32 Core 3.x Frame Stabilization: Set secure handshake window limits
                // to give heavy certificate chains like MansionNET plenty of time to clear sockets natively
                net_client.setHandshakeTimeout(15); 
                
                if (channel_log_enabled == 1) {
                    Serial.printf("[NET] Attempting secure link pass for slot: %s\n", discovered_networks[i]);
                }
                
                if (net_client.connect(bnc_host, bnc_port)) {
                    net_client.printf("PASS %s:%s\r\n", bnc_user, bnc_pass);
                    net_client.print("CAP REQ :server-time\r\n");
                    net_client.printf("NICK %s\r\n", irc_nick);
                    net_client.printf("USER %s/%s 0 * :M5 Client\r\n", bnc_user, discovered_networks[i]);
                }
            }

            // TWDT periodic feed for idle loop (ensures 4s watchdog never false-trips)
            esp_task_wdt_reset();
            if (net_client.connected() && net_client.available()) {
                // Secure a 0-millisecond execution timeout pass to forcefully kill read-stalls completely
                net_client.setTimeout(0);
                
                char packet_chunk[512] = {0};
                // Pull an entire data paragraph block into internal RAM in a single, atomic hardware operation
                int bytes_read = net_client.readBytesUntil('\n', packet_chunk, sizeof(packet_chunk) - 1);
                esp_task_wdt_reset(); // TWDT heartbeat for Core0 network task (4s)
                if (bytes_read <= 0) continue;
                packet_chunk[bytes_read] = '\0';
                
                String line = String(packet_chunk);
                line.trim(); line.replace("\r", "");
                if (line.length() == 0) continue;

                // SASL Authentication Handler: Intercept CAP negotiation
                if (line.indexOf("CAP LS 302") != -1) {
                    net_client.printf("CAP REQ :sasl\r\n");
                    net_client.flush();
                    continue;
                }
                if (line.indexOf("CAP ACK :sasl") != -1) {
                    String user = String(bnc_user);
                    String pass = String(bnc_pass);
                    int plain_len = 1 + user.length() + 1 + pass.length();
                    char plain[128] = {0};
                    int pos = 0;
                    plain[pos++] = '\0';
                    memcpy(plain+pos, user.c_str(), user.length()); pos += user.length();
                    plain[pos++] = '\0';
                    memcpy(plain+pos, pass.c_str(), pass.length()); pos += pass.length();
                    char b64[256] = {0};
                    size_t olen = 0;
                    mbedtls_base64_encode((unsigned char*)b64, sizeof(b64), &olen, (unsigned char*)plain, plain_len);
                    net_client.printf("AUTHENTICATE %s\r\n", b64);
                    net_client.flush();
                    memset(plain, 0, sizeof(plain));
                    memset(b64, 0, sizeof(b64));
                    user = ""; pass = "";
                    continue;
                }

                // --- ATOMIC DISCOVERY PROTOCOL TOKENIZER ---
                String discovered_room = "";
                String isolated_packet_server = String(discovered_networks[i]); // Secure default baseline

                if (line.indexOf(" PRIVMSG ") != -1) {
                    int priv_idx = line.indexOf(" PRIVMSG ");
                    int colon_idx = line.indexOf(" :", priv_idx);
                    if (priv_idx != -1 && colon_idx != -1) {
                        String target_recipient = line.substring(priv_idx + 9, colon_idx);
                        target_recipient.trim();
                        
                        int slash_idx = target_recipient.indexOf('/');
                        int hash_pos = target_recipient.indexOf('#');
                        int amp_pos = target_recipient.indexOf('&');
                        int symbol_pos = (hash_pos != -1) ? hash_pos : amp_pos;

                        // RIGID PROTECTION: Only strip a server prefix if it sits directly in front of a true channel hash symbol
                        if (slash_idx != -1 && symbol_pos != -1 && slash_idx < symbol_pos) {
                            isolated_packet_server = target_recipient.substring(0, slash_idx);
                            discovered_room = target_recipient.substring(symbol_pos);
                        } else if (symbol_pos != -1) {
                            discovered_room = target_recipient.substring(symbol_pos);
                        } else {
                            // This is a clean Private Query PM!
                            discovered_room = target_recipient;
                        }
                    }
                }
                isolated_packet_server.trim();
                discovered_room.trim();

                // Preserve JOIN and 353 handling for backward compatibility (also protected by slash blocker)
                if (discovered_room.length() == 0) {
                    if (line.indexOf(" JOIN ") != -1) {
                        int join_idx = line.indexOf(" JOIN ");
                        discovered_room = line.substring(join_idx + 6);
                    } else if (line.indexOf(" 353 ") != -1) {
                        int equal_idx = line.indexOf(" = ");
                        if (equal_idx != -1) {
                            int colon_idx = line.indexOf(" :", equal_idx);
                            if (colon_idx != -1) discovered_room = line.substring(equal_idx + 3, colon_idx);
                        }
                    }
                    discovered_room.trim();
                    if (discovered_room.startsWith(":")) discovered_room = discovered_room.substring(1);
                }

                // If a channel name was isolated, dynamically allocate its space in RAM
                if (discovered_room.length() > 0) {
                    if (discovered_room.startsWith(":")) discovered_room = discovered_room.substring(1);
                    if (discovered_room.startsWith("#") || discovered_room.startsWith("&")) {
                        if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            bool tab_exists = false;
                            for (int t = 0; t < gTabCount; t++) {
                                if (strcmp(gTabs[t].name, discovered_room.c_str()) == 0 && strcmp(gTabs[t].server, isolated_packet_server.c_str()) == 0) { tab_exists = true; break; }
                            }
                            if (!tab_exists && gTabCount < MAX_TABS) {
                                strncpy(gTabs[gTabCount].name, discovered_room.c_str(), sizeof(gTabs[gTabCount].name)-1);
                                strncpy(gTabs[gTabCount].server, isolated_packet_server.c_str(), sizeof(gTabs[gTabCount].server)-1);
                                gTabs[gTabCount].line_count = 0;
                                gTabCount++;
                            }
                            xSemaphoreGive(irc_mutex); ui_needs_redraw = true;
                        }
                    }
                }

                // WHOIS numeric cache (311/312/317/318/319/330/671)
                if (line.indexOf(" 311 ") != -1) {
                    int p311 = line.indexOf(" 311 ");
                    int s1 = line.indexOf(' ', p311+5); if(s1!=-1){ int s2=line.indexOf(' ', s1+1); if(s2!=-1){ int s3=line.indexOf(' ', s2+1); int s4=line.indexOf(' ', s3+1); int s5=line.indexOf(' ', s4+1); int colon=line.indexOf(" :", s5); 
                        String target = line.substring(s2+1, s3); target.trim();
                        String user = line.substring(s3+1, s4); String host = line.substring(s4+1, s5);
                        String real = (colon!=-1)? line.substring(colon+2) : "";
                        strncpy(whois_cache.nick, target.c_str(), sizeof(whois_cache.nick)-1);
                        strncpy(whois_cache.user, user.c_str(), sizeof(whois_cache.user)-1);
                        strncpy(whois_cache.host, host.c_str(), sizeof(whois_cache.host)-1);
                        strncpy(whois_cache.real, real.c_str(), sizeof(whois_cache.real)-1);
                        whois_pending=true;
                    }}
                } else if (line.indexOf(" 312 ") != -1) {
                    int p312=line.indexOf(" 312 "); int s1=line.indexOf(' ', p312+5); if(s1!=-1){ int s2=line.indexOf(' ', s1+1); if(s2!=-1){ int s3=line.indexOf(' ', s2+1); int s4=line.indexOf(" :", s3); String srv=(s4==-1)?line.substring(s3+1):line.substring(s3+1,s4); srv.trim(); strncpy(whois_cache.server, srv.c_str(), sizeof(whois_cache.server)-1); }}
                } else if (line.indexOf(" 317 ") != -1) {
                    int p317=line.indexOf(" 317 "); int s1=line.indexOf(' ', p317+5); if(s1!=-1){ int s2=line.indexOf(' ', s1+1); if(s2!=-1){ int s3=line.indexOf(' ', s2+1); String idle=line.substring(s3+1, line.indexOf(' ', s3+1)); idle.trim(); whois_cache.idle=idle.toInt(); }}
                } else if (line.indexOf(" 330 ") != -1) {
                    int p330=line.indexOf(" 330 "); int s1=line.indexOf(' ', p330+5); if(s1!=-1){ int s2=line.indexOf(' ', s1+1); if(s2!=-1){ int s3=line.indexOf(" :", s2+1); String acc=(s3==-1)?line.substring(s2+1):line.substring(s2+1,s3); acc.trim(); int sp=acc.lastIndexOf(' '); if(sp!=-1) acc=acc.substring(sp+1); strncpy(whois_cache.account, acc.c_str(), sizeof(whois_cache.account)-1); }}
                } else if (line.indexOf(" 671 ") != -1) { whois_cache.secure=true; }
                else if (line.indexOf(" 319 ") != -1) {
                    int p319=line.indexOf(" 319 "); int colon=line.indexOf(" :", p319); if(colon!=-1){ String chans=line.substring(colon+2); chans.trim(); strncpy(whois_cache.channels, chans.c_str(), sizeof(whois_cache.channels)-1); }
                } else if (line.indexOf(" 318 ") != -1) {
                    if(whois_pending){ whois_pending=false; current_app_mode=MODE_WHOIS; ui_needs_redraw=true; if(speaker_enabled && sound_profile>=1) M5.Speaker.tone(600,100); queueLed(18,500); }
                    continue;
                } else if (line.indexOf(" 301 ") != -1) {
                    int p301=line.indexOf(" 301 "); int s1=line.indexOf(' ', p301+5); if(s1!=-1){ int s2=line.indexOf(' ', s1+1); if(s2!=-1){ int s3=line.indexOf(' ', s2+1); String target=line.substring(s2+1,s3); target.trim();
                        if(target.length() && irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5))==pdTRUE){
                            for(int t=0;t<gTabCount;t++) for(int k=0;k<gTabs[t].nick_count;k++) if(strcasecmp(gTabs[t].nicks[k], target.c_str())==0) gTabs[t].nicks_away[k]=true;
                            xSemaphoreGive(irc_mutex); ui_needs_redraw=true;
                        }
                    }}
                } else if (line.indexOf(" 322 ") != -1) {
                    // Minimal chan list cache (10 max, light)
                    int p322=line.indexOf(" 322 "); int s1=line.indexOf(' ', p322+5); if(s1!=-1){ int s2=line.indexOf(' ', s1+1); if(s2!=-1){ int s3=line.indexOf(' ', s2+1); int s4=line.indexOf(' ', s3+1); String chan=(s4==-1)? line.substring(s3+1): line.substring(s3+1, s4); chan.trim(); if(chan.startsWith("#")||chan.startsWith("&")){ if(chan_list_count<10){ strncpy(chan_list_cache[chan_list_count], chan.c_str(),31); chan_list_count++; } add_message_to_buffer(chan.c_str(), line.substring(line.indexOf(" :", s4)+2).c_str(), 0x7BEF); }}
                } else if (line.indexOf(" 323 ") != -1) {
                    if(chan_list_count>0){
                        String sum="Channels: ";
                        for(int i=0;i<chan_list_count && i<5;i++){ if(i) sum+=" "; sum+=chan_list_cache[i]; }
                        add_message_to_buffer("ClientCore", sum.c_str(), 0x07E0);
                        // also push to log browser cache is already via SD, no extra
                    }
                    chan_list_count=0;
                }
                }
                // ==========================================
                // 🛑 PROTOCOL DROP SHIELD MASK + NICKLIST/TOPIC CAPTURE
                // ==========================================

                // Capture topic (332 / TOPIC) into per-tab storage without navbar clutter
                if (line.indexOf(" 332 ") != -1 || line.indexOf(" TOPIC ") != -1) {
                    int colon = line.indexOf(" :");
                    if (colon != -1 && discovered_room.length()>0) {
                        String topic = line.substring(colon+2);
                        topic.trim();
                        if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            for(int t=0;t<gTabCount;t++) if(strcmp(gTabs[t].name, discovered_room.c_str())==0 && strcmp(gTabs[t].server, isolated_packet_server.c_str())==0){
                                strncpy(gTabs[t].topic, topic.c_str(), sizeof(gTabs[t].topic)-1);
                                queueLed(24, 400);
                                break;
                            }
                            xSemaphoreGive(irc_mutex);
                        }
                    }
                    if (line.indexOf(" 332 ") != -1) continue; // don't flood chat with RPL_TOPIC
                }
                // Capture nicklist from 353 before dropping to feed drawer
                if (line.indexOf(" 353 ") != -1) {
                    int colon = line.lastIndexOf(" :");
                    if (colon != -1 && discovered_room.length()>0) {
                        String nickStr = line.substring(colon+2);
                        nickStr.trim();
                        if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                            for(int t=0;t<gTabCount;t++) if(strcmp(gTabs[t].name, discovered_room.c_str())==0 && strcmp(gTabs[t].server, isolated_packet_server.c_str())==0){
                                gTabs[t].nick_count=0;
                                int start=0;
                                while(start < (int)nickStr.length() && gTabs[t].nick_count < 12){
                                    int sp = nickStr.indexOf(' ', start);
                                    String tok = (sp==-1)? nickStr.substring(start) : nickStr.substring(start, sp);
                                    tok.trim();
                                    if(tok.length()>0){
                                        // strip @ + % modes
                                        if(tok[0]=='@' || tok[0]=='+' || tok[0]=='%' || tok[0]=='~' || tok[0]=='&') tok = tok.substring(1);
                                        strncpy(gTabs[t].nicks[gTabs[t].nick_count], tok.c_str(), 15);
                                        gTabs[t].nick_count++;
                                    }
                                    if(sp==-1) break;
                                    start = sp+1;
                                }
                                break;
                            }
                            xSemaphoreGive(irc_mutex);
                        }
                    }
                    continue;
                }
                if (line.indexOf(" 366 ") != -1) {
                    continue; 
                }

                // Process standard live traffic below this shield pass...
                // Recommended alternative: zero-init + explicit null guarantee (fixes char parsed_time[6]="00:00" truncation risk)
                char parsed_time[6] = {0};
                strncpy(parsed_time, "00:00", sizeof(parsed_time)-1);
                parsed_time[sizeof(parsed_time)-1] = '\0';
                if (line.startsWith("@")) {
                    int time_idx = line.indexOf("time=");
                    if (time_idx != -1) {
                        int t_start = line.indexOf('T', time_idx);
                        if (t_start != -1 && t_start + 6 < line.length()) {
                            String hh_mm = line.substring(t_start + 1, t_start + 6);
                            strncpy(parsed_time, hh_mm.c_str(), sizeof(parsed_time) - 1);
                            parsed_time[sizeof(parsed_time)-1] = '\0';
                        }
                    }
                    int msg_start = line.indexOf(' ');
                    if (msg_start != -1) {
                        line = line.substring(msg_start + 1);
                    }
                }
                
                if (line.startsWith("PING")) { 
                    net_client.printf("PONG %s\r\n", line.substring(5).c_str()); 
                    set_led_mode(23); // Bright Pearl Strobe for keep-alive
                    continue; 
                }

                // Intercept the Welcome token (001) or End of MOTD (376) to fire CAP END safely
                if (!network_handshake_complete[i] && (line.indexOf(" 001 ") != -1 || line.indexOf(" 376 ") != -1 || line.indexOf("CAP * ACK") != -1)) {
                    net_client.print("CAP END\r\n");
                    network_handshake_complete[i] = true;
                    Serial.printf("[NET-SYNC] Handshake finalized for network: %s. Releasing channels.\n", discovered_networks[i]);
                    continue;
                }
                // SASL 900/901/903/904 + WHO 352 cache (no flood)
                if (line.indexOf(" 900 ") != -1) { network_handshake_complete[i]=true; log_system("SASL 900 %s", discovered_networks[i]); Serial.printf("[SASL] 900 success %s\n", discovered_networks[i]); continue; }
                if (line.indexOf(" 901 ") != -1 || line.indexOf(" 903 ") != -1 || line.indexOf(" 904 ") != -1) { 
                    Serial.printf("[SASL] fail %s %s\n", discovered_networks[i], line.c_str()); 
                    log_system("SASL fail %s", discovered_networks[i]);
                    set_led_mode(37); 
                    network_reconnect_cooldown[i]=millis()+10000; // backoff 10s
                    net_client.stop();
                    continue; 
                }
                if (line.indexOf(" 352 ") != -1) {
                    // :server 352 mynick #chan user host server nick H :0 real
                    int p352=line.indexOf(" 352 "); int s1=line.indexOf(' ', p352+5); if(s1!=-1){ int s2=line.indexOf(' ', s1+1); if(s2!=-1){ int s3=line.indexOf(' ', s2+1); int s4=line.indexOf(' ', s3+1); int s5=line.indexOf(' ', s4+1); int s6=line.indexOf(' ', s5+1); String nick=line.substring(s6+1, line.indexOf(' ', s6+1)); nick.trim(); int colon=line.indexOf(" :", s6); String chan=line.substring(s3+1, s4); chan.trim(); if(chan.startsWith("#")||chan.startsWith("&")){ if(irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5))==pdTRUE){ for(int t=0;t<gTabCount;t++) if(strcmp(gTabs[t].name, chan.c_str())==0 && strcasecmp(gTabs[t].server, isolated_packet_server.c_str())==0){ bool exists=false; for(int k=0;k<gTabs[t].nick_count;k++) if(strcasecmp(gTabs[t].nicks[k], nick.c_str())==0) exists=true; if(!exists && gTabs[t].nick_count<12){ strncpy(gTabs[t].nicks[gTabs[t].nick_count], nick.c_str(),15); gTabs[t].nicks_away[gTabs[t].nick_count]=false; gTabs[t].nick_count++; } break; } xSemaphoreGive(irc_mutex); } } }}
                }

                // LAYER B: DYNAMIC CHANNEL SYNC - PART EVENT EXTRACTION (STATE MACHINE DELETION ENGINE)
                if (line.indexOf(" PART ") != -1) {
                    int part_idx = line.indexOf(" PART ");
                    // Extract channel target text out of packet (e.g., "#channel")
                    int chan_end = line.indexOf(' ', part_idx + 6);
                    String part_chan = (chan_end == -1) ? line.substring(part_idx + 6) : line.substring(part_idx + 6, chan_end);
                    part_chan.trim(); if (part_chan.startsWith(":")) part_chan = part_chan.substring(1);

                    // Verify if the part action came from our own active username register handle
                    int bang_idx = line.indexOf('!');
                    if (bang_idx != -1 && line.substring(1, bang_idx) == irc_nick) {
                        if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                            int target_delete_idx = -1;
                            
                            // Find the memory offset index matching this specific server/room pairing
                            for (int t = 1; t < gTabCount; t++) { // Skip Tab 0 (~mentions safeguard)
                                if (strcmp(gTabs[t].name, part_chan.c_str()) == 0 && strcmp(gTabs[t].server, discovered_networks[i]) == 0) {
                                    target_delete_idx = t; break;
                                }
                            }

                            // If matched, delete row and collapse array cleanly to maintain alignment bounds
                            if (target_delete_idx != -1) {
                                for (int d = target_delete_idx; d < gTabCount - 1; d++) {
                                    gTabs[d] = gTabs[d + 1];
                                }
                                gTabCount--;
                                if (current_tab_index >= gTabCount) current_tab_index = gTabCount - 1;
                            }
                            xSemaphoreGive(irc_mutex); ui_needs_redraw = true;
                        }
                    }
                }
                if (line.indexOf(" ACCOUNT ") != -1 || line.indexOf(" AWAY ") != -1) {
                    continue;
                }
                // Chan modes viewer: capture MODE #chan +nt etc
                if (line.indexOf(" MODE ") != -1) {
                    int mIdx = line.indexOf(" MODE ");
                    int chanStart = mIdx + 6;
                    int chanEnd = line.indexOf(' ', chanStart);
                    String chan = (chanEnd==-1) ? line.substring(chanStart) : line.substring(chanStart, chanEnd);
                    chan.trim(); if(chan.startsWith(":")) chan=chan.substring(1);
                    String modeStr = "";
                    if(chanEnd!=-1){
                        int sp = line.indexOf(' ', chanEnd+1);
                        if(sp!=-1) modeStr=line.substring(sp+1); else modeStr=line.substring(chanEnd+1);
                        modeStr.trim();
                    }
                    if((chan.startsWith("#")||chan.startsWith("&")) && modeStr.length()>0){
                        if(irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5))==pdTRUE){
                            for(int t=0;t<gTabCount;t++) if(strcmp(gTabs[t].name, chan.c_str())==0 && strcasecmp(gTabs[t].server, isolated_packet_server.c_str())==0){
                                strncpy(gTabs[t].modes, modeStr.c_str(), sizeof(gTabs[t].modes)-1);
                                break;
                            }
                            xSemaphoreGive(irc_mutex); ui_needs_redraw=true;
                        }
                    }
                }
                
                if (line.indexOf(" 001 ") != -1 || line.indexOf(" JOIN ") != -1) {
                    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        if (gTabCount == 1 && strcmp(gTabs[0].name, "~system") == 0) {
                            strncpy(gTabs[0].server, "BNC", sizeof(gTabs[0].server)-1);
                        }
                        xSemaphoreGive(irc_mutex);
                        ui_needs_redraw = true;
                    }
                }
                String network_context = String(discovered_networks[i]);
                if (network_context.length() == 0) network_context = "BNC";
                int slash_idx = line.indexOf('/');
                int colon_idx = line.indexOf(':');
                if (slash_idx != -1 && slash_idx < colon_idx) {
                    int space_idx = line.indexOf(' ', slash_idx);
                    if (space_idx != -1) {
                        network_context = line.substring(slash_idx + 1, space_idx);
                        network_context.trim();
                    }
                }

                // --- OPTIMIZED CHAT PAYLOAD ROUTING ENGINE ---
                if (line.indexOf(" PRIVMSG ") != -1) {
                    int priv_idx = line.indexOf(" PRIVMSG ");
                    int colon_idx = line.indexOf(" :", priv_idx);
                    
                    if (priv_idx != -1 && colon_idx != -1) {
                        String target_recipient = line.substring(priv_idx + 9, colon_idx);
                        target_recipient.trim();
                        
                        // EXPLICIT NETWORK EXTRACTOR: Isolate the true upstream bouncer network tag signature
                        String isolated_packet_server = "";
                        int network_slash_idx = target_recipient.indexOf('/');
                        int hash_pos = target_recipient.indexOf('#');
                        
                        if (network_slash_idx != -1 && (hash_pos == -1 || network_slash_idx < hash_pos)) {
                            isolated_packet_server = target_recipient.substring(0, network_slash_idx);
                        } else {
                            // Fallback: If no explicit slash exists, default directly to this socket slot's pre-saved name
                            isolated_packet_server = String(discovered_networks[i]);
                        }
                        isolated_packet_server.trim();
                        
                        String clean_target_room = (hash_pos != -1) ? target_recipient.substring(hash_pos) : target_recipient;
                        if (clean_target_room.indexOf('/') != -1 && hash_pos != -1) {
                            // Strip any trailing network tags trailing behind channel tags
                            int structural_slash = clean_target_room.indexOf('/');
                            clean_target_room = clean_target_room.substring(0, structural_slash);
                        }
                        clean_target_room.trim();
                        
                        int prefix_start_idx = line.lastIndexOf(':', priv_idx);
                        int bang_idx = line.indexOf('!', prefix_start_idx);
                        String sender_nick = "Server";
                        if (prefix_start_idx != -1 && bang_idx != -1 && bang_idx > prefix_start_idx) {
                            sender_nick = line.substring(prefix_start_idx + 1, bang_idx);
                        }
                        sender_nick.trim();
                        // clear away on activity
                        if(irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5))==pdTRUE){
                            for(int t=0;t<gTabCount;t++) for(int k=0;k<gTabs[t].nick_count;k++) if(strcasecmp(gTabs[t].nicks[k], sender_nick.c_str())==0) gTabs[t].nicks_away[k]=false;
                            xSemaphoreGive(irc_mutex);
                        }
                        String actual_msg = line.substring(colon_idx + 2);
                            // --- CTCP PROTOCOL INTERCEPT ENGINE ---
                            // Check if the message string contains hidden CTCP delimiter tags (\x01)
                            if (actual_msg.startsWith("\x01") && actual_msg.endsWith("\x01")) {
                                String ctcp_cmd = actual_msg.substring(1, actual_msg.length() - 1);
                                ctcp_cmd.trim();

                                if (ctcp_cmd.equalsIgnoreCase("VERSION")) {
                                    Serial.printf("[CTCP] Version query intercepted from user handle: %s\n", sender_nick.c_str());
                                    
                                    // Target our 42-mode diagnostic desk to flash a bright Pearl White Strobe (Mode 23)
                                    set_led_mode(23); 
                                    
                                    // Transmit a custom, sanitized identification string back down the secure socket
                                    net_client.printf("NOTICE %s :\x01VERSION Cardputer ADV IRC (ESP32-S3FN8)\x01\r\n", sender_nick.c_str());
                                    net_client.flush(); // Force immediate packet delivery down the wire
                                    
                                    continue; // Drop the packet out of processing execution so it never clutters chat logs
                                }
                            }

                        // Match Target Context FIRST completely free of loop variable confusion
                        int target_tab_slot = -1;
                        for (int t = 0; t < gTabCount; t++) {
                            if (strcmp(gTabs[t].name, clean_target_room.c_str()) == 0 && 
                                strcasecmp(gTabs[t].server, isolated_packet_server.c_str()) == 0) {
                                target_tab_slot = t;
                                break;
                            }
                        }

                        // Claim memory locks strictly to inject data into the verified absolute slot frame
                        if (target_tab_slot != -1 && irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                            Tab &t = gTabs[target_tab_slot];
                            
                            ChatLine *clp2;
                            if (t.line_count < MSG_BUFFER_SIZE) {
                                clp2 = &t.lines[(t.head + t.line_count) % MSG_BUFFER_SIZE];
                            } else {
                                t.head = (t.head + 1) % MSG_BUFFER_SIZE;
                                clp2 = &t.lines[(t.head + MSG_BUFFER_SIZE -1) % MSG_BUFFER_SIZE];
                            }
                            ChatLine &cl = *clp2;
                            strncpy(cl.timeStr, "00:00", sizeof(cl.timeStr)-1);
                            strncpy(cl.nick, sender_nick.c_str(), sizeof(cl.nick)-1);
                            if (is_ignored(sender_nick.c_str())) { xSemaphoreGive(irc_mutex); ui_needs_redraw=true; continue; }
                            strncpy(cl.message, actual_msg.c_str(), sizeof(cl.message)-1);
                            cl.color = 0xFFFF;
                            cl.is_highlight = is_mention(actual_msg.c_str(), irc_nick) && !t.muted;
                            if (cl.is_highlight && speaker_enabled && sound_profile>=1) M5.Speaker.tone(800, 80);
                            if (t.line_count < MSG_BUFFER_SIZE) t.line_count++;
                            // preserve scrollback viewport
                            if ((is_scrollback_active || scrollback_mode_active) && target_tab_slot == current_tab_index) {
                                int eff = t.line_count;
                                if (eff > MSG_BUFFER_SIZE) eff = MSG_BUFFER_SIZE;
                                if (scrollback_offset < eff - 1) {
                                    scrollback_offset++;
                                    scrollback_offset_idx = scrollback_offset;
                                }
                            } else {
                                // disabled bounce animation to avoid flood flicker
                            }
                            
                            xSemaphoreGive(irc_mutex);
                            ui_needs_redraw = true;
                        }
                    }
                }
                if (line.indexOf(" PRIVMSG ") == -1 && line.length() > 0) {
                    char dispNick[16] = "server";
                    if (line.charAt(0) == ':') {
                        int sp = line.indexOf(' ');
                        if (sp != -1) {
                            String pref = line.substring(1, sp);
                            int bang = pref.indexOf('!');
                            if (bang != -1) pref = pref.substring(0, bang);
                            strncpy(dispNick, pref.c_str(), sizeof(dispNick)-1);
                            dispNick[sizeof(dispNick)-1] = '\0';
                        }
                    }
                    add_message_to_buffer(dispNick, line.c_str(), 0xFFFF, parsed_time);
                    if (gLogQueue) {
                        if (uxQueueMessagesWaiting(gLogQueue) >= 20) {
                            char dummy[128];
                            xQueueReceive(gLogQueue, &dummy, 0);
                        }
                        char qLine[128];
                        strncpy(qLine, line.c_str(), sizeof(qLine)-1);
                        qLine[sizeof(qLine)-1] = '\0';
                        xQueueSend(gLogQueue, &qLine, 0);
                    }
                }
            }
        }
        esp_task_wdt_reset(); // idle heartbeat ensures 4s TWDT never fires spuriously
    }
}

void custom_ui_loop_task(void* pvParameters) {
    ui_needs_redraw = true;
    while (true) {
        yield();
        vTaskDelay(pdMS_TO_TICKS(10));
        
        // Sit idle and yield if the core setup sequence hasn't finished initializing memory
        if (!system_booted) continue; 
        
        M5Cardputer.update(); // Polling matrix registers over un-lockable bus lane
        handle_keyboard_inputs();
        // Auto-rotate and rotation lock (Fn+Space) removed - rotation fixed at boot (1).
        // Interrupt-driven idle check consumes kb_interrupt_fired flag to avoid polling churn
        if (kb_interrupt_fired) { kb_interrupt_fired = false; last_input_time = millis(); last_user_keyboard_input_tick = millis(); }
        
        // --- REFINED 60-SECOND PARTIAL AUTO-DIMMER + HARDWARE-TIED LED RAIL ---
        // Cardputer Adv: LED (GPIO21 neopixel) VCC is tied to backlight rail (GPIO38).
        // At display 80/89/30 the rail PWM avg is 31%/35%/12% -> WS2812 browns out, idle LED off even at normal 80.
        // Keep rail high enough for LED heartbeat: normal 180 (70%), idle 150 (59%), low-batt 100 (39%) still visible.
        float current_battery_pct = get_calibrated_battery_percentage();
        int target_backlight_level = screen_brightness;
        static bool was_dimmed = false;
        if (current_battery_pct <= 5.0f) {
            target_backlight_level = 100; // 39% keeps LED visible, was 30 (12% -> off)
            flush_log_cache();
            was_dimmed = false;
        } else if (current_battery_pct <= 20.0f) {
            target_backlight_level = 100;
            was_dimmed = false;
        } else {
            bool is_chat_idle_60s = (current_app_mode == MODE_CHAT && (millis() - last_user_keyboard_input_tick >= 60000));
            if (is_chat_idle_60s) {
                target_backlight_level = 150; // 59% idle floor, was 89 (35% -> LED off)
                was_dimmed = true;
            } else {
                if (was_dimmed) {
                    target_backlight_level = 255;
                    ui_needs_redraw = true;
                    was_dimmed = false;
                } else {
                    if (screen_brightness < 100) screen_brightness = 180;
                    target_backlight_level = screen_brightness;
                }
            }
        }
        g_backlight_level = target_backlight_level;
        // FIX: Keep hardware backlight rail at 255 (100% DC) to stop PWM flicker on both
        // display and hardware-tied LED (GPIO38). At 80/100/150 PWM the rail pulses and
        // WS2812 brown-out + display flicker. Dimming is now handled in software via
        // g_backlight_level gamma in set_led_mode() and via pixel dimming, not hardware PWM.
        { static bool hw_init=false;
          if(!hw_init){
              M5Cardputer.Display.setBrightness(255);
              hw_init=true;
          }
          // Intentionally do not drive Display.setBrightness(target) - would re-introduce flicker
          (void)target_backlight_level;
        }
        // Periodic log flush every 1s to make channel logs visible (was only on 512B full)
        {
            static unsigned long last_log_flush=0;
            if(channel_log_enabled==1 && millis()-last_log_flush>1000 && log_sector_cache_len>0){
                last_log_flush=millis();
                flush_log_cache();
            }
        }
        // Auto-away 5m idle -> AWAY :auto, Mode 10; key restores
        if (current_app_mode==MODE_CHAT && !is_away && millis() - last_user_keyboard_input_tick > 300000) {
            for(int i=0;i<discovered_network_count;i++) if(clients[i].connected()){ clients[i].printf("AWAY :auto\r\n"); clients[i].flush(); }
            is_away=true; last_away_tick=millis(); queueLed(10, 1000);
        }
        if (is_away && M5Cardputer.Keyboard.isPressed()) {
            for(int i=0;i<discovered_network_count;i++) if(clients[i].connected()){ clients[i].printf("AWAY\r\n"); clients[i].flush(); }
            is_away=false; queueLed(1, 500);
        }

        // Hierarchical 24-Mode Diagnostic LED Status Selector Matrix
        uint8_t target_led_mode = 1; // Default fallback to Mode 1 (Cyan Idle Heartbeat)

        if (safe_mode_active) {
            target_led_mode = 0;
        } else if (WiFi.status() != WL_CONNECTED) {
            target_led_mode = 9;  // Wi-Fi Disconnect Fault (Solid Sharp Orange)
        } else if (WiFi.status() == WL_CONNECTED && WiFi.RSSI() <= -85) {
            target_led_mode = 22; // Antenna Alert: Urgent Flashing Ruby Red if signal drops below -85dBm
        } else if (gTabCount >= MAX_TABS) {
            target_led_mode = 7;  // Memory Buffer Ceiling Fault (Flash Magenta)
        } else {
            bool highlight_active = false;
            if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                for (int t = 0; t < gTabCount; t++) {
                    if (gTabs[t].line_count > 0 && gTabs[t].lines[gTabs[t].line_count - 1].is_highlight) {
                        highlight_active = true; break;
                    }
                }
                xSemaphoreGive(irc_mutex);
            }

            if (highlight_active) {
                target_led_mode = 15; // Priority 1: Unread Highlight Mention Alarm Strobe (Pulsing Gold)
            } else if (current_battery_pct <= 20.0f) {
                target_led_mode = 55; // Priority 2: Low Battery Emergency Indicator (Pure Solid Red)
            } else if (current_app_mode == MODE_NAVIGATOR && nav_server_select_idx > 0 && !network_handshake_complete[nav_server_select_idx - 1]) {
                target_led_mode = 17; // Priority 3: Active Server Handshake Strobe (Neon Green)
            } else if (nav_filter_active) {
                target_led_mode = 28; // Filter active steady gold
            } else if (show_nicklist) {
                target_led_mode = 29; // Nicklist drawer teal
            } else if (input_buffer.length() >= 190) {
                target_led_mode = 6;  // Buffer Shield: Flash Indigo near character cap
            }
        }

        uint8_t q = popQueuedLed();
        if (q != 255) target_led_mode = q;
        set_led_mode(target_led_mode);

        // Hardware Frame-Rate Governor Shield - 30 FPS (33ms) reduces SPI load and flood flicker, atomic push eliminates tearing
        static unsigned long last_hardware_frame_tick = 0;
        // Coalesce flood: if many messages arrived, limit to 30fps; interpolation disabled (no bounce) to avoid extra frames
        if (millis() - last_hardware_frame_tick >= 33) {
            last_hardware_frame_tick = millis();
            // interpolation disabled - keep at 0
            if (ui_scroll_y_interpolation > 0.0f) {
                ui_scroll_y_interpolation = 0.0f;
            }
            if (ui_needs_redraw) draw_chat_view();
            esp_task_wdt_reset(); // TWDT heartbeat for Core1 UI task (4s)
        }

        // Background Smart Wi-Fi Hotspot Auto-Roaming Monitor (non-blocking vault-driven)
        handle_vault_scan_complete();
        if (current_app_mode == MODE_SETTINGS || current_app_mode == MODE_WIFI) {
            // Abort failover to protect active user configurations
        } else {
            if (WiFi.status() != WL_CONNECTED) {
                if (wifi_drop_timestamp == 0) wifi_drop_timestamp = millis();
                if (millis() - wifi_drop_timestamp > 10000 && !failover_in_progress) {
                    // Instead of blocking WiFi.begin, trigger async passive scan
                    if (WiFi.scanComplete() != -2) {
                        failover_in_progress = true;
                        set_led_mode(9); // Solid Orange
                        WiFi.scanNetworks(true, true);
                    }
                }
            } else {
                wifi_drop_timestamp = 0;
                if (failover_in_progress) {
                    failover_in_progress = false;
                    set_led_mode(1);
                }
                sync_ntp_timezone();
                // LRU cache rotator on successful auth (SD-only, <Y=120 open)
                {
                    static String lastVaultLearn = "";
                    String cur = WiFi.SSID();
                    if (cur.length()>0 && cur != lastVaultLearn) {
                        bool known=false;
                        for(int i=0;i<wifi_vault_count;i++) if(cur == wifi_vault_ssid[i]) known=true;
                        if(!known){
                            const char* pass = "";
                            if (cur == String(wifi_ssid)) pass = wifi_pass;
                            else if (cur == String(wifi_ssid2)) pass = wifi_pass2;
                            save_wifi_vault_lru(cur.c_str(), pass);
                        }
                        lastVaultLearn = cur;
                    }
                }
            }
        }
    }
}

// ==========================================
// 🔌 HARDWARE HANDSHAKES AND INITIAL COLD BOOT
// ==========================================
void setup() {
    safe_mode_active = false;
    
    // Safe initialization sequence passing native configurations to shield the Stamp-S3A
    auto cfg = m5::M5Unified::config();
    cfg.internal_imu = true;
    M5Cardputer.begin(cfg, true); // Initialize display and keyboard matrix cleanly
    M5Cardputer.Display.setRotation(1);
    use_light_theme=false; text_scale=1;
    // Create full-screen canvas (240x135) for atomic push - eliminates navbar/input flicker
    Serial.printf("[GFX] heap largest %d free %d\n", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), heap_caps_get_free_size(MALLOC_CAP_8BIT));
    if(!canvas.createSprite(240, 135)){
        Serial.println("[GFX] 240x135 sprite fail, trying 240x109 fallback");
        for(int t=0;t<MAX_TABS;t++){ gTabs[t].line_count=0; memset(gTabs[t].lines,0,sizeof(gTabs[t].lines)); }
        if(!canvas.createSprite(240, 109)){
            Serial.println("[GFX] 240x109 fail, trying 135x214");
            canvas.createSprite(135, 214);
        }
    }
    if(canvas.width()==0 || canvas.height()==0){
        Serial.println("[GFX] sprite still 0, retry 240x135 after heap trim");
        for(int t=0;t<gTabCount;t++) if(gTabs[t].line_count>5) gTabs[t].line_count=5;
        canvas.deleteSprite();
        canvas.createSprite(240,135);
    }
    Serial.printf("[GFX] sprite %dx%d ok\n", canvas.width(), canvas.height());
    canvas.fillSprite(0x0000);
    // Force visible brightness at boot (hardware LED rail needs >150 to keep idle heartbeat on)
    M5Cardputer.Display.setBrightness(180);
    g_backlight_level=180;
    
    // BMI270 Wire1 tilt sensor init - cfg.internal_imu already inits via M5Cardputer.begin, just ensure Wire1
    Wire1.begin(2, 1);
    // Interrupt-driven idle dimmer wait gate: hardware FALLING edge on keyboard matrix rows wakes display
    // Cardputer matrix rows map to GPIOs 8,9,10,46 via internal shift register; we attach to exposed row pins
    pinMode(8, INPUT_PULLUP);
    pinMode(46, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(8), kb_isr, FALLING);
    attachInterrupt(digitalPinToInterrupt(46), kb_isr, FALLING);

    // UN-LOCKABLE WIRE I2C REGISTER TIMEOUT PROTECTION CODES
    Wire.setTimeOut(50);
    
    // Activate Stamp module built-in NeoPixel line switch rail power output
    // pin 38 via Display.setBrightness (was NeoPixel rail, now tied LED/backlight)
    
    // Open clean cooperative SPI bus lane pipelines with SD self-heal 3x retry
    // Cardputer SD uses SCK=40, MISO=39, MOSI=14, CS=12 per official example
    SPI.begin(40, 39, 14, 12);
    bool sd_ok=false;
    for(int i=0;i<3;i++){ if(SD.begin(12, SPI, 10000000)){ sd_ok=true; break; } vTaskDelay(pdMS_TO_TICKS(200)); }
    if(!sd_ok){ safe_mode_active=true; set_led_mode(11); Serial.println("[STORAGE] SD self-heal failed 3x, safe_mode"); log_system("STORAGE SD self-heal fail 3x"); }
    // SD Wi-Fi Vault boot ingestion: parse /irc/wifi_cache.txt into transient vault array
    load_wifi_vault_from_sd();
    if (wifi_vault_count > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(wifi_vault_ssid[0], wifi_vault_pass[0]);
        Serial.printf("[VAULT] Boot auth attempt slot0: %s\n", wifi_vault_ssid[0]);
        // Keep Y=120 empty - no canvas draw here
    }
    // Ensure system log dir exists on SD (visible even before first log)
    if (SD.cardType() != CARD_NONE) {
        SD.mkdir("/irc");
        SD.mkdir("/irc/system");
    }
    // TWDT 4s init (panic true) - tasks registered after creation
    esp_task_wdt_init(8, true);
    esp_register_shutdown_handler(wdt_emergency_flush);
    
    // Paint intro splash logo horizontally
    M5Cardputer.Display.fillScreen(0x0000);
    M5Cardputer.Display.setTextColor(0xFD20);
    M5Cardputer.Display.setCursor(20, 55);
    M5Cardputer.Display.print("CRISP TERMINAL V3A ACTIVE");
    
    // 1500ms TIME-DELAY REGISTER WARMUP ISOLATION WINDOW
    unsigned long boot_scan_start = millis();
    while (millis() - boot_scan_start < 1500) {
        yield();
        vTaskDelay(pdMS_TO_TICKS(10));
        M5Cardputer.update();
        
        // Scan if clicky Backspace key is physically held down to activate safe mode safely
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) { // Safely catches held backspace clicks on cold boot
            safe_mode_active = true;
        }
    }
    
    load_settings_from_sd();
    screen_brightness = 180; g_backlight_level=180; // fixed 180 keeps LED rail alive (was 80 -> LED off due to hardware tie)
    if (screen_brightness < 100) { Serial.printf("[BRIGHT] bump %d->180\n", screen_brightness); screen_brightness = 180; g_backlight_level=180; sync_new_nick_to_sd(irc_nick); }
    if (!safe_mode_active) purge_old_logs();

    irc_mutex = xSemaphoreCreateMutex();
    sd_mutex = xSemaphoreCreateMutex();
    gLogQueue = xQueueCreate(20, sizeof(char) * 128);
    input_buffer.reserve(201);
    // Ensure system log dir after mutex will be created in log_system itself
    gTabCount = 1; current_tab_index = 0; memset(&gTabs, 0, sizeof(gTabs));
    strncpy(gTabs[0].name, "~mentions", sizeof(gTabs[0].name)-1);
    strncpy(gTabs[0].server, "CC", sizeof(gTabs[0].server)-1); // Clean local system node identifier
    if (irc_mutex) xSemaphoreGive(irc_mutex);
    // Safe boot interception: read /irc/session_state.tmp (<12 bytes) and snap pointer indexes bypassing home
    // Keep Y=120-135 (225-240 portrait) textbox completely empty isolated from background file ops
    load_session_state();
    // Refresh vault after mutex ready (short protected window)
    load_wifi_vault_from_sd();
    load_input_history();
    load_ignore_list();
    load_highlight_list();
    // System log boot entry (now mutex ready, ensures /irc/system visible)
    log_system("System boot ok heap %d", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    load_alias_list();
    load_highlight_list();

    // Force visible brightness at boot (180 keeps LED rail alive, was 80 -> LED off)
    M5Cardputer.Display.setBrightness(180);
    g_backlight_level=180;
    // Ensure canvas is valid before boot (full-screen 135)
    if(canvas.width()==0 || canvas.height()==0){
        canvas.deleteSprite();
        canvas.createSprite(240,135);
    }
    // Setup complete. Instantly unblock Core 1 graphics engine to draw status lines
    system_booted = true; 
    
    xTaskCreatePinnedToCore(irc_network_task, "NetworkTask", 16384, NULL, 1, &xNetworkTaskHandle, 0);
    xTaskCreatePinnedToCore(custom_ui_loop_task, "CustomUITask", 16384, NULL, 1, &xUITaskHandle, 1);
    // Register core tasks into TWDT (4s timeout, panic + emergency flush)
    esp_task_wdt_add(xNetworkTaskHandle);
    esp_task_wdt_add(xUITaskHandle);
}

void loop() {
    vTaskDelete(NULL); // Force terminate default loop task to free up loop system registers entirely
}
