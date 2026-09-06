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
    char topic[64];
    char nicks[12][16];
    uint8_t nick_count;
    bool pinned;
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
enum AppMode { MODE_CHAT, MODE_WIFI, MODE_BOUNCER, MODE_SETTINGS, MODE_NAVIGATOR };
volatile AppMode current_app_mode = MODE_CHAT;
int menu_selection_idx = 0;
int nav_server_select_idx = 0;
int nav_channel_select_idx = 0;
int nav_focus_column = 0; // 0=LEFT server, 1=RIGHT channel - stealth navigator focus
char nav_filter[16] = {0};
bool nav_filter_active = false;
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
void IRAM_ATTR kb_isr() { kb_interrupt_fired = true; last_user_keyboard_input_tick = millis(); }
unsigned long last_input_time = 0;
bool rotation_locked = false;
uint8_t locked_rotation = 1;
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
            digitalWrite(38, HIGH); 
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
    // Tie LED to display brightness with gamma 2.2 (stop forcing pin 38 HIGH)
    {
        float br = g_backlight_level / 255.0f;
        if (br < 0) br = 0; if (br > 1) br = 1;
        br = powf(br, 2.2f);
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
    file.printf("current_tz_idx=%d\nuse_12_hour_format=%d\nbnc_host=%s\n", current_tz_idx, use_12_hour_format, bnc_host);
    file.printf("bnc_port=%d\nbnc_user=%s\nbnc_pass=%s\n", bnc_port, bnc_user, bnc_pass);
    file.close();
    if (sd_mutex) xSemaphoreGive(sd_mutex);
    Serial.println("[STORAGE-SYNC] New nick permanently synchronized to micro-SD config.");
}

void purge_old_logs() {
    if (safe_mode_active) return;
    Serial.println("[STORAGE] Launching automated 7-day log cleanup pass...");
    
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
    if (safe_mode_active || channel_log_enabled != 1) return;
    
    // Create direct structural folder path variables safely
    char file_path[64] = {0};
    // Strip channel prefix hash markers to keep file naming conventions completely FAT32 safe
    const char* clean_name = (tab_name[0] == '#') ? (tab_name + 1) : tab_name;
    snprintf(file_path, sizeof(file_path), "/irc/logs/%s.log", clean_name);
    
    // Open in append-mode to write text chunks without wiping historical rows
    File log_file = SD.open(file_path, FILE_APPEND);
    if (!log_file) return;
    
    // Export raw text payload strings cleanly inside a non-blocking snapshot lock pass
    log_file.printf("[00:00] <%s> %s\n", nick, message);
    log_file.close();
}


// Lightweight 512-byte static line cache for high-speed log write caching
static char log_sector_cache[512] = {0};
static int log_sector_cache_len = 0;
static char log_sector_current_path[128] = {0};

void flush_log_cache() {
    if (log_sector_cache_len == 0 || log_sector_current_path[0] == '\0') return;
    // Use sd_mutex if available
    bool sd_locked = false;
    if (sd_mutex) sd_locked = (xSemaphoreTake(sd_mutex, pdMS_TO_TICKS(50)) == pdTRUE);
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
    flush_log_cache();
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
    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        int target_idx = (gTabCount > 0 && current_tab_index < gTabCount) ? current_tab_index : 0;
        Tab &t = gTabs[target_idx];
        
        // Auto-Scrolling Rolling Viewport Shifter Engine
        if (t.line_count >= MSG_BUFFER_SIZE) {
            // Shift every single text row upward by one index offset slot
            for (int m = 1; m < MSG_BUFFER_SIZE; m++) {
                t.lines[m - 1] = t.lines[m];
            }
            t.line_count = MSG_BUFFER_SIZE - 1; // Open up the absolute bottom slot row for our incoming text
        }
        
        ChatLine &cl = t.lines[t.line_count];
        if (timeStr) strncpy(cl.timeStr, timeStr, sizeof(cl.timeStr)-1);
        else strncpy(cl.timeStr, "00:00", sizeof(cl.timeStr)-1);
        strncpy(cl.nick, source, sizeof(cl.nick)-1);
        strncpy(cl.message, msg, sizeof(cl.message)-1);
        cl.color = color;
        // Fixed: case-insensitive word-boundary mention, not server noise
        cl.is_highlight = is_mention(msg, irc_nick) && strcasecmp(source, "server")!=0 && strcasecmp(source, "ClientCore")!=0;
        t.line_count++;
        if (!is_scrollback_active && !scrollback_mode_active) ui_scroll_y_interpolation = 12.0f;

    // Dynamic Session Log Rotator with High-Speed Write Caching (512-byte sector cache)
    if (channel_log_enabled == 1 && safe_mode_active == false) {
        unsigned long current_sync_sec = (millis() / 1000) + adj_time;
        uint32_t active_yr  = 2026; 
        uint32_t active_mon = ((current_sync_sec / 2629743) % 12) + 1; 
        uint32_t active_day = ((current_sync_sec / 86400) % 31) + 1;   

        String safe_server = String(t.server);
        safe_server.replace("/", "_"); safe_server.replace("\\", "_"); safe_server.trim();
        String safe_room = String(t.name);
        safe_room.replace("/", "_"); safe_room.replace("\\", "_"); safe_room.trim();

        char dir_path_buffer[64] = {0};
        snprintf(dir_path_buffer, sizeof(dir_path_buffer), "/irc/logs/%s", safe_server.c_str());
        if (!SD.exists(dir_path_buffer)) SD.mkdir(dir_path_buffer);
        char file_path_buffer[128] = {0};
        snprintf(file_path_buffer, sizeof(file_path_buffer), "/irc/logs/%s/%s_%04d_%02d_%02d.log", safe_server.c_str(), safe_room.c_str(), active_yr, active_mon, active_day);

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
                for (int i = 1; i < MSG_BUFFER_SIZE; i++) mentions_tab.lines[i-1] = mentions_tab.lines[i];
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
bool is_mention(const char* msg, const char* nick) {
    if (!msg || !nick || !*nick) return false;
    size_t nlen = strlen(nick);
    if (nlen==0) return false;
    for (const char* p = msg; *p; p++) {
        if (strncasecmp(p, nick, nlen)==0) {
            bool preOk = (p==msg) || !isalnum((unsigned char)p[-1]);
            bool postOk = p[nlen]=='\0' || !isalnum((unsigned char)p[nlen]);
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
    if (!ui_needs_redraw) return;
    // Fluid geometry anchored to active rotation (135x240 vertical vs 240x135 landscape)
    int display_width = M5Cardputer.Display.width();
    int display_height = M5Cardputer.Display.height();
    bool is_vertical = (display_width < display_height);
    // Spec tight clamp: if rotation reports 1 or 3 treat as vertical too (fallback)
    uint8_t rot = M5Cardputer.Display.getRotation();
    if (rot == 1 || rot == 3) {
        // Honor spec: Orientation 1 or 3 = vertical 135x240 (overrides width check if driver remaps)
        if (display_width == 240 && display_height == 135) is_vertical = true;
    }
    int navbar_clamp_x = is_vertical ? 65 : 115;
    int rssi_anchor_x = is_vertical ? 75 : 120;
    int battery_anchor_x = is_vertical ? 110 : 208;
    int wire_y = is_vertical ? 224 : 121;
    int input_box_y = is_vertical ? 225 : 121;
    int baseline_y = is_vertical ? 208 : (97 + (int)ui_scroll_y_interpolation);
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
    if (current_app_mode == MODE_SETTINGS || current_app_mode == MODE_BOUNCER || current_app_mode == MODE_WIFI) {
        canvas.fillSprite(0x0000); 
        canvas.setTextColor(0xFD20); canvas.setCursor(10, 8);
        if (current_app_mode == MODE_SETTINGS) {
            canvas.print("--- SYSTEM CONFIGURATIONS ---"); canvas.setTextColor(0xFFFF);
            canvas.setCursor(10, 26); canvas.printf("%s Brightness Level: %d", (menu_selection_idx == 0 ? ">" : " "), screen_brightness);
            canvas.setCursor(10, 40); canvas.printf("%s Timezone Index: %d", (menu_selection_idx == 1 ? ">" : " "), current_tz_idx);
            canvas.setCursor(10, 54); canvas.printf("%s Format Layer: %s", (menu_selection_idx == 2 ? ">" : " "), use_12_hour_format ? "12-HR" : "24-HR");
            canvas.setCursor(10, 68); canvas.printf("%s Storage Logging: %s", (menu_selection_idx == 3 ? ">" : " "), channel_log_enabled ? "ON" : "OFF");
        } else if (current_app_mode == MODE_BOUNCER) {
            canvas.print("--- BOUNCER CONNECTION SCHEMAS ---"); canvas.setTextColor(0xFFFF);
            canvas.setCursor(10, 26); canvas.printf("%s Server Host: %s", (menu_selection_idx == 0 ? ">" : " "), (const char*)bnc_host);
            canvas.setCursor(10, 40); canvas.printf("%s Port Address: %d", (menu_selection_idx == 1 ? ">" : " "), bnc_port);
            canvas.setCursor(10, 54); canvas.printf("%s Username Key: %s", (menu_selection_idx == 2 ? ">" : " "), (const char*)bnc_user);
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
    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        canvas.fillSprite(0x0000);
        Tab &t = gTabs[current_tab_index];
        int current_y = baseline_y; 
        int starting_index = (t.line_count - 1) - scrollback_offset;
        if (starting_index < 0) starting_index = 0;
        scrollback_offset_idx = scrollback_offset;
        is_scrollback_active = scrollback_mode_active;

        for (int i = starting_index; i >= 0; i--) {
            if (current_y < viewport_top_y) break;
            if (current_y > wire_y - 9) { current_y -= 12; continue; }
            
            uint16_t row_bg = (i % 2 == 0) ? 0x0000 : 0x0841;
            int text_start_x = is_vertical ? 45 : ((current_tab_index == 0) ? 110 : 70);
            String msg_text = t.lines[i].message;
            int current_char_pos = 0;
            bool first_line_pass = true;
            while (current_char_pos < msg_text.length()) {
                int active_render_x = first_line_pass ? text_start_x : (text_start_x + 6);
                int dynamic_chars_budget;
                if (is_vertical) {
                    dynamic_chars_budget = 15;
                } else {
                    int dynamic_max_width = display_width - 4 - active_render_x;
                    dynamic_chars_budget = dynamic_max_width / 6;
                }
                if (dynamic_chars_budget <= 0) break;
                String sub_line = msg_text.substring(current_char_pos, current_char_pos + dynamic_chars_budget);
                canvas.fillRect(0, current_y - 2, display_width, 12, row_bg);
                
                if (first_line_pass) {
                    int vline_h = is_vertical ? wire_y : 120;
                    canvas.drawFastVLine(40, 0, vline_h, 0x7BEF); 
                    char shortened_nick[9] = {0};
                    if (strlen(t.lines[i].nick) > 8) {
                        strncpy(shortened_nick, t.lines[i].nick, 6);
                        strcat(shortened_nick, "..");
                    } else {
                        strncpy(shortened_nick, t.lines[i].nick, 8);
                    }
                    if (t.lines[i].is_highlight) { 
                        canvas.fillRect(2, current_y - 1, 36, 11, 0xFD20); canvas.setTextColor(0xFFFF); 
                    } else { 
                        canvas.setTextColor(get_nick_palette_color(t.lines[i].nick)); 
                    }
                    canvas.setCursor(2, current_y);
                    if (current_tab_index == 0) canvas.printf("%s", shortened_nick);
                    else canvas.printf("<%s>", shortened_nick);
                    first_line_pass = false;
                }
                
                canvas.setTextColor(t.lines[i].color); 
                canvas.setCursor(active_render_x, current_y);
                canvas.print(sub_line);
                current_y -= 12; 
                current_char_pos += dynamic_chars_budget;
            }
        }
        // Topic line per-tab (no navbar clutter) - display below top edge if set
        if (t.topic[0] && !is_scrollback_active && current_y < wire_y - 14) {
            canvas.setTextColor(0x7BEF);
            canvas.setCursor(42, 14);
            // truncate to display_width
            int maxTopic = is_vertical ? 14 : 30;
            char topic_disp[32]={0}; strncpy(topic_disp, t.topic, maxTopic);
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
                canvas.setTextColor(get_nick_palette_color(t.nicks[n]));
                canvas.setCursor(drawer_x+4, 26+n*10);
                char nd[12]={0}; strncpy(nd, t.nicks[n], 10);
                canvas.print(nd);
            }
            canvas.setTextColor(0x7BEF); canvas.setCursor(drawer_x+4, wire_y-10); canvas.print("Ent:close");
        }
        // TextBox Workspace Integrity: Y = 225 to 240 must remain empty - ensure cleared
        if (is_vertical) {
            // Reserve 225-240 completely empty wide-open for typed text across 135px portrait glass
            canvas.fillRect(0, 225, display_width, display_height - 225, 0x0000);
        }
        xSemaphoreGive(irc_mutex);
    }
    canvas.pushSprite(0, 0);

    // NAVBAR RENDERING SYSTEM - fluid to display_width
    M5Cardputer.Display.fillRect(0, 0, display_width, 12, 0x0841);
    String nav_server_str = String(gTabs[current_tab_index].server);
    String nav_chan_str = String(gTabs[current_tab_index].name);
    String full_nav_str = "[" + nav_server_str + "] " + nav_chan_str;
    int nav_text_width = canvas.textWidth(full_nav_str.c_str());
    String display_server = nav_server_str;
    if (nav_text_width > navbar_clamp_x) {
        int server_len = nav_server_str.length();
        if (server_len > 6) display_server = nav_server_str.substring(0, 4) + "..." + nav_server_str.substring(server_len - 2);
        else if (server_len > 4) display_server = nav_server_str.substring(0, 2) + "..." + nav_server_str.substring(server_len - 2);
        full_nav_str = "[" + display_server + "] " + nav_chan_str;
        nav_text_width = canvas.textWidth(full_nav_str.c_str());
        if (nav_text_width > navbar_clamp_x) {
            int max_chan = is_vertical ? 6 : 10;
            if ((int)nav_chan_str.length() > max_chan) nav_chan_str = nav_chan_str.substring(0, max_chan - 3) + "...";
        }
    }
    M5Cardputer.Display.setTextColor(0x7BEF, 0x0841); M5Cardputer.Display.setCursor(2, 2);
    M5Cardputer.Display.printf("[%s]", display_server.c_str());
    int chan_x = is_vertical ? 45 : 54;
    M5Cardputer.Display.setTextColor(0xFFFF, 0x0841); M5Cardputer.Display.setCursor(chan_x, 2);
    char truncated_name[12] = {0}; strncpy(truncated_name, nav_chan_str.c_str(), is_vertical ? 6 : 8);
    M5Cardputer.Display.print(truncated_name);

    // Connection indicator
    int conn_x = is_vertical ? 75 : 142;
    // RSSI anchor already at rssi_anchor_x, but keep conn indicator near RSSI for compact vertical
    if (!is_vertical) {
        M5Cardputer.Display.setCursor(conn_x, 2);
        if (WiFi.status() != WL_CONNECTED) { M5Cardputer.Display.setTextColor(0xF800, 0x0841); M5Cardputer.Display.print("D"); }
        else { M5Cardputer.Display.setTextColor(0x07E0, 0x0841); M5Cardputer.Display.print("W"); }
    }
    // RSSI sparkline 8-step ( -90→-50 ) update history
    {
        int8_t cur = (WiFi.status()==WL_CONNECTED) ? (int8_t)WiFi.RSSI() : -127;
        static unsigned long last_hist = 0;
        if (millis() - last_hist > 500) { last_hist = millis(); rssi_history[rssi_history_idx]=cur; rssi_history_idx=(rssi_history_idx+1)%8; }
    }
    if (WiFi.status() != WL_CONNECTED) {
        M5Cardputer.Display.setTextColor(0x7BEF, 0x0841); M5Cardputer.Display.setCursor(rssi_anchor_x, 2); M5Cardputer.Display.print("---");
    } else {
        // 8 bars anchored at rssi_anchor_x, 1px wide 2px gap, height 1-6
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
    // Vertical also needs W/D indicator near RSSI if not drawn above
    if (is_vertical) {
        M5Cardputer.Display.setCursor(90, 2);
        if (WiFi.status() != WL_CONNECTED) { M5Cardputer.Display.setTextColor(0xF800, 0x0841); M5Cardputer.Display.print("D"); }
        else { M5Cardputer.Display.setTextColor(0x07E0, 0x0841); M5Cardputer.Display.print("W"); }
    }
    // Rotation lock indicator l/u (unicode lock fails on builtin font)
    M5Cardputer.Display.setCursor(battery_anchor_x - 10, 2);
    if (rotation_locked) { M5Cardputer.Display.setTextColor(0xF800, 0x0841); M5Cardputer.Display.print("L"); }
    else { M5Cardputer.Display.setTextColor(0x7BEF, 0x0841); M5Cardputer.Display.print("U"); }
    M5Cardputer.Display.setCursor(battery_anchor_x, 2); 
    M5Cardputer.Display.setTextColor(0xFFFF, 0x0841);
    M5Cardputer.Display.printf("%d%%", (int)get_calibrated_battery_percentage());

    // Low-Profile Navbar Activity Indicator Dots Engine
    for (int t = 1; t < gTabCount; t++) {
        if (t == current_tab_index) continue;
        int dot_x = 2 + (t * 6);
        int dot_limit = is_vertical ? 60 : 130;
        if (dot_x > dot_limit) break;
        bool has_unread_highlight = false;
        bool has_unread_msg = (gTabs[t].line_count > 0);
        for (int l = 0; l < gTabs[t].line_count; l++) if (gTabs[t].lines[l].is_highlight) { has_unread_highlight = true; break; }
        if (has_unread_highlight) M5Cardputer.Display.fillRect(dot_x, 10, 3, 2, 0xFD20);
        else if (has_unread_msg) M5Cardputer.Display.fillRect(dot_x, 10, 3, 2, 0x07FF);
    }

    // LOWER INPUT BOX - fluid to display_width/display_height, textbox integrity 225-240 vertical
    M5Cardputer.Display.fillRect(0, input_box_y, display_width, textbox_height, 0x0000);
    M5Cardputer.Display.drawFastHLine(0, wire_y, display_width, 0x7BEF);
    M5Cardputer.Display.setTextColor(0xFD20, 0x0000);
    int input_cursor_y = is_vertical ? 227 : 124;
    M5Cardputer.Display.setCursor(4, input_cursor_y);
    M5Cardputer.Display.print("> ");
    M5Cardputer.Display.setTextColor(0xFFFF, 0x0000);
    // For vertical 135px width, wrap input display if needed but keep single line preview truncated
    String disp_input = input_buffer;
    if (is_vertical && disp_input.length() > 18) disp_input = disp_input.substring(disp_input.length() - 18);
    M5Cardputer.Display.print(disp_input.c_str());
    // Textbox counter at far-right, red at >390
    {
        int rem = 400 - (int)input_buffer.length();
        int cnt_x = display_width - 26;
        M5Cardputer.Display.setCursor(cnt_x, input_cursor_y);
        if ((int)input_buffer.length() >= 390) M5Cardputer.Display.setTextColor(0xF800, 0x0000);
        else M5Cardputer.Display.setTextColor(0x7BEF, 0x0000);
        M5Cardputer.Display.printf("%2d", rem);
    }
    ui_needs_redraw = false;
}

void handle_keyboard_inputs() {
    if (M5Cardputer.Keyboard.isPressed()) {
        last_user_keyboard_input_tick = millis(); // Refresh activity timer anchor
        // Instant restore from 35% partial dimmer: full operational brightness 100% duty, pin HIGH, refresh canvas
        if (current_app_mode == MODE_CHAT) {
            g_backlight_level = 255; M5Cardputer.Display.setBrightness(255);
            // pin 38 tied to backlight via scaling (no forced HIGH)
            ui_needs_redraw = true;
        }
    }
    if (!M5Cardputer.Keyboard.isPressed()) return;
    
    // Explicit 150ms hardware bounce filter guard to stop character double-chatter
    if (millis() - last_keypress_debounce < 150) return;
    last_keypress_debounce = millis();
    last_input_time = millis(); // Fresh backlight dim timer
    
    auto status = M5Cardputer.Keyboard.keysState();
    bool is_alt = M5Cardputer.Keyboard.isKeyPressed(KEY_LEFT_ALT);
    bool is_fn  = M5Cardputer.Keyboard.isKeyPressed(KEY_FN);
    
    // Hotkey Intercept A: Toggle Multi-Network Channel Navigator Hub (Fn + P)
    if (is_fn && M5Cardputer.Keyboard.isKeyPressed('p')) {
        current_app_mode = (current_app_mode == MODE_NAVIGATOR) ? MODE_CHAT : MODE_NAVIGATOR;
        menu_selection_idx = 0; nav_server_select_idx = 0; nav_channel_select_idx = 0;
        ui_needs_redraw = true;
        set_led_mode(18);
        return;
    }

    // Hotkey Intercept B: Cycle Hardware & Bouncer Configuration Menus (Fn + O)
    if (is_fn && M5Cardputer.Keyboard.isKeyPressed('o')) {
        if (current_app_mode == MODE_CHAT)       { current_app_mode = MODE_SETTINGS; }
        else if (current_app_mode == MODE_SETTINGS) { current_app_mode = MODE_BOUNCER; }
        else if (current_app_mode == MODE_BOUNCER)  { current_app_mode = MODE_WIFI; }
        else                                      { current_app_mode = MODE_CHAT; }
        menu_selection_idx = 0;
        ui_needs_redraw = true;
        set_led_mode(18);
        return;
    }


    
    // Layer B: Master Emergency Escape Back to Main Chat Workspace
    // Captures (Alt + Backspace [0x08]) OR the literal physical Esc key char mapping ('`')
    if ((is_alt && M5Cardputer.Keyboard.isKeyPressed(0x08)) || M5Cardputer.Keyboard.isKeyPressed('`')) {
        // Auto-export updated parameters safely to card storage upon form exit
        if (current_app_mode == MODE_SETTINGS || current_app_mode == MODE_BOUNCER || current_app_mode == MODE_WIFI) {
            if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                File file = SD.open("/irc/config.txt", FILE_WRITE);
                if (file) {
                    file.printf("wifi_ssid=%s\nwifi_pass=%s\nirc_nick=%s\n", wifi_ssid, wifi_pass, irc_nick);
                    file.printf("bnc_host=%s\nbnc_port=%d\nbnc_user=%s\nbnc_pass=%s\n", bnc_host, bnc_port, bnc_user, bnc_pass);
                    file.printf("channel_log_enabled=%d\nscreen_brightness=%d\n", channel_log_enabled, screen_brightness);
                    file.printf("current_tz_idx=%d\nuse_12_hour_format=%d\n", current_tz_idx, use_12_hour_format);
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
    // Global Scrollback Panic Reset (Esc or `) - restores real-time view from any mode
    if (M5Cardputer.Keyboard.isKeyPressed('`') || M5Cardputer.Keyboard.isKeyPressed(0x1B)) {
        scrollback_offset = 0;
        scrollback_offset_idx = 0;
        is_scrollback_active = false;
        scrollback_mode_active = false;
        ui_needs_redraw = true;
    }

    // Layer C: Critical Hardware Intercept for Fn+Arrow Punctuation Codes
    // On the Cardputer layout, Fn+Arrows outputs direct character values:
    // Fn+Left = ';' | Fn+Right = '/' | Fn+Up = ',' | Fn+Down = '.'
    if (current_app_mode == MODE_CHAT) {
        // Rotation Lock Toggle (Fn + Space) + Auto-Rotate
        if (is_fn && M5Cardputer.Keyboard.isKeyPressed(' ')) {
            rotation_locked = !rotation_locked;
            if (rotation_locked) locked_rotation = M5Cardputer.Display.getRotation();
            queueLed(40, 500); // Mode 40 lock blip
            // Visual lock persists in navbar, no canvas resize needed on lock alone
            g_backlight_level = 255; M5Cardputer.Display.setBrightness(255);
            // pin 38 tied to backlight via scaling (no forced HIGH)
            last_user_keyboard_input_tick = millis();
            last_input_time = millis();
            ui_needs_redraw = true;
            return;
        }
        if (is_fn && M5Cardputer.Keyboard.isKeyPressed(';')) { // Fn + UP Arrow
            Tab &t = gTabs[current_tab_index];
            is_scrollback_active = true;
            scrollback_mode_active = true;
            if (scrollback_offset < t.line_count - 1) scrollback_offset++;
            if (scrollback_offset_idx < t.line_count - 1) scrollback_offset_idx++;
            ui_needs_redraw = true;
            return;
        }
        if (is_fn && M5Cardputer.Keyboard.isKeyPressed('.')) { // Fn + DOWN Arrow
            if (scrollback_offset > 0) scrollback_offset--;
            if (scrollback_offset_idx > 0) scrollback_offset_idx--;
            if (scrollback_offset == 0) is_scrollback_active = false;
            if (scrollback_offset_idx == 0) scrollback_mode_active = false;
            ui_needs_redraw = true;
            return;
        }
        // Fn + Backspace Panic Flush Macro
        if (is_fn && M5Cardputer.Keyboard.isKeyPressed(0x08)) {
            Tab &t = gTabs[current_tab_index];
            if (xNetworkTaskHandle != NULL) vTaskSuspend(xNetworkTaskHandle);
            t.line_count = 0;
            scrollback_offset = 0;
            scrollback_offset_idx = 0;
            is_scrollback_active = false;
            scrollback_mode_active = false;
            memset(t.lines, 0, sizeof(t.lines));
            if (xNetworkTaskHandle != NULL) vTaskResume(xNetworkTaskHandle);
            ui_needs_redraw = true;
            return;
        }
        if ((is_alt && M5Cardputer.Keyboard.isKeyPressed(0x08)) || M5Cardputer.Keyboard.isKeyPressed('`')) {
            scrollback_offset = 0;
            scrollback_offset_idx = 0;
            is_scrollback_active = false;
            scrollback_mode_active = false;
            ui_needs_redraw = true;
        }

        // Horizontal Quick-Tab Swapper (Fn + Comma = Previous Tab | Fn + Slash = Next Tab)
        if (is_fn && M5Cardputer.Keyboard.isKeyPressed(',')) { // Previous Tab
            if (gTabCount > 1) {
                current_tab_index = (current_tab_index - 1 + gTabCount) % gTabCount;
                scrollback_offset_idx = 0;
                ui_needs_redraw = true;
                // Non-intrusive session state append with 2s debounce, keep Y=120 empty
                save_session_state();
                // Trigger auto-cleaning on quick-swapping left
                if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    Tab &current_view_tab = gTabs[current_tab_index];
                    for (int l = 0; l < current_view_tab.line_count; l++) current_view_tab.lines[l].is_highlight = false;
                    xSemaphoreGive(irc_mutex);
                }
            }
            return;
        }
        if (is_fn && M5Cardputer.Keyboard.isKeyPressed('/')) { // Next Tab
            if (gTabCount > 1) {
                current_tab_index = (current_tab_index + 1) % gTabCount;
                scrollback_offset_idx = 0;
                ui_needs_redraw = true;
                // Non-intrusive session state append with 2s debounce, keep Y=120 empty
                save_session_state();
                // Trigger auto-cleaning on quick-swapping right
                if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    Tab &current_view_tab = gTabs[current_tab_index];
                    for (int l = 0; l < current_view_tab.line_count; l++) current_view_tab.lines[l].is_highlight = false;
                    xSemaphoreGive(irc_mutex);
                }
            }
            return;
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
                    if (nav_server_select_idx < (int)discovered_network_count - 1) nav_server_select_idx++;
                    // clamp channel index when server changes
                    nav_channel_select_idx = 0;
                } else {
                    // count channels for selected server
                    int ch_cnt = 0;
                    for (int i = 0; i < gTabCount; i++) if (strcmp(gTabs[i].server, discovered_networks[nav_server_select_idx]) == 0) ch_cnt++;
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
                if (menu_selection_idx == 0) { // Screen Backlight Scaler
                    screen_brightness += forward ? 30 : -30;
                    if (screen_brightness > 255) screen_brightness = 255;
                    if (screen_brightness < 15)  screen_brightness = 15;
                }
                else if (menu_selection_idx == 1) { // Timezone Offset Index
                    current_tz_idx += forward ? 1 : -1;
                    if (current_tz_idx > 14) current_tz_idx = -12;
                    if (current_tz_idx < -12) current_tz_idx = 14;
                }
                else if (menu_selection_idx == 2) { use_12_hour_format = !use_12_hour_format; }
                else if (menu_selection_idx == 3) { channel_log_enabled = !channel_log_enabled; }
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
    // Layer E: Configuration Menu Form Row Value-Changing Handlers
    if (current_app_mode == MODE_SETTINGS && status.enter) {
        if (menu_selection_idx == 0) { // Row 1: Cycle Brightness Level (Off -> Dim -> Med -> Max)
            if (screen_brightness == 10) screen_brightness = 60;
            else if (screen_brightness == 60) screen_brightness = 120;
            else if (screen_brightness == 120) screen_brightness = 200;
            else if (screen_brightness == 200) screen_brightness = 255;
            else screen_brightness = 10;
            M5Cardputer.Display.setBrightness(screen_brightness);
        }
        else if (menu_selection_idx == 1) { // Row 2: Shift Timezone Indicator Offset index
            current_tz_idx = (current_tz_idx + 1);
            if (current_tz_idx > 14) current_tz_idx = -12; // Wrap across international date lines safely
        }
        else if (menu_selection_idx == 2) { // Row 3: Toggle 12/24hr Display Format Layer
            use_12_hour_format = !use_12_hour_format;
        }
        else if (menu_selection_idx == 3) { // Row 4: Toggle Local Channel Log File Recording
            channel_log_enabled = !channel_log_enabled;
        }
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
            }
        }

    // Nicklist drawer toggle: Fn+Enter in chat (does not send)
    if (current_app_mode == MODE_CHAT && is_fn && status.enter) {
        // Delay to ensure Fn+Space orientation switcher already handled
        if (!M5Cardputer.Keyboard.isKeyPressed(' ')) {
            show_nicklist = !show_nicklist;
            ui_needs_redraw = true;
            set_led_mode(18);
            return;
        }
    }

    // History recall with plain ;/. (non-Fn) while in chat - separate from Fn+;/ scrollback
    if (current_app_mode == MODE_CHAT && !is_fn) {
        if (M5Cardputer.Keyboard.isKeyPressed(';') && input_history_len>0) {
            // Up = older
            if (input_history_pos==-1) input_history_pos = (input_history_head -1 +10)%10;
            else {
                // move one older if not at oldest
                int oldest = (input_history_head - input_history_len +10)%10;
                if (input_history_pos != oldest) input_history_pos = (input_history_pos -1 +10)%10;
            }
            input_buffer = String(input_history[input_history_pos]);
            ui_needs_redraw = true;
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('.') && input_history_len>0 && input_history_pos!=-1) {
            // Down = newer, -1 wraps to live
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
        // Layer G: Tab Autocomplete Key Intercept Matrix (Physical Tab = 0xB9)
        if (M5Cardputer.Keyboard.isKeyPressed(0xB9) && input_buffer.length() > 0) {
            int last_space = input_buffer.lastIndexOf(' ');
            String partial_token = (last_space == -1) ? input_buffer : input_buffer.substring(last_space + 1);
            partial_token.toLowerCase();

            Tab &active_tab = gTabs[current_tab_index];
            String discovered_match = "";

            // Scan backwards through recent channel lines to find a matching nickname handle
            for (int line_scan = active_tab.line_count - 1; line_scan >= 0; line_scan--) {
                String candidate_nick = String(active_tab.lines[line_scan].nick);
                String lookup_lower = candidate_nick;
                lookup_lower.toLowerCase();

                if (lookup_lower.startsWith(partial_token) && candidate_nick != String(irc_nick)) {
                    discovered_match = candidate_nick;
                    break;
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
        // Handle Backspace deletions safely with synchronous empty telemetry triggers
        if (status.del) { 
            if (input_buffer.length() > 0) {
                input_buffer.remove(input_buffer.length() - 1); 
                ui_needs_redraw = true; 
            } else {
                // Tapping backspace on an empty textbox triggers our hardware-level red warning alert
                set_led_mode(5); 
            }
        }
        
        // Append standard printable characters into the buffer
        for (auto c : status.word) {
            if (is_fn && (c == ';' || c == '/' || c == 'p' || c == 's' || c == 'o')) continue;
            if (input_buffer.length() < 400) { 
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

                    if (cmd == "JOIN" && args.length() > 0) {
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
    static bool master_scan_complete = false;
    static WiFiClientSecure master_client;

    while (true) {
        yield(); vTaskDelay(pdMS_TO_TICKS(50)); // Prevent core starvation
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
        if (!master_scan_complete) {
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
                    master_client.stop(); // Close the discovery probe safely
                    master_scan_complete = true;
                    Serial.printf("[NET] Dynamic discovery complete. Isolated %d networks from bouncer.\n", discovered_network_count);
                }
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
                
                // Rate-Limiter Shield: Force a strict 5000ms cooldown wait period before retrying a dropped socket connection
                if (millis() - network_reconnect_cooldown[i] < 5000) {
                    continue; // Skip this network index slot until its 5-second connection cooldown expires
                }
                
                network_reconnect_cooldown[i] = millis(); 

                // Predictive Heap Compaction: Evaluate largest free block before TLS re-alloc (outside mutex)
                if (ESP.getMinFreeHeap() < 45 * 1024) {
                    Serial.println("[HEAP] Low heap detected (<45KB), recycling old channel history buffers");
                    for (int t_idx = 0; t_idx < gTabCount; t_idx++) {
                        if (gTabs[t_idx].line_count > 10) {
                            int keep = 10;
                            for (int m = 0; m < keep; m++) {
                                gTabs[t_idx].lines[m] = gTabs[t_idx].lines[gTabs[t_idx].line_count - keep + m];
                            }
                            for (int m = keep; m < gTabs[t_idx].line_count; m++) {
                                memset(&gTabs[t_idx].lines[m], 0, sizeof(ChatLine));
                            }
                            gTabs[t_idx].line_count = keep;
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
                            
                            if (t.line_count >= MSG_BUFFER_SIZE) {
                                for (int m = 1; m < MSG_BUFFER_SIZE; m++) t.lines[m-1] = t.lines[m];
                                t.line_count = MSG_BUFFER_SIZE - 1;
                            }
                            
                            ChatLine &cl = t.lines[t.line_count];
                            strncpy(cl.timeStr, "00:00", sizeof(cl.timeStr)-1);
                            strncpy(cl.nick, sender_nick.c_str(), sizeof(cl.nick)-1);
                            strncpy(cl.message, actual_msg.c_str(), sizeof(cl.message)-1);
                            cl.color = 0xFFFF;
                            cl.is_highlight = is_mention(actual_msg.c_str(), irc_nick);
                            t.line_count++;
                            if (!is_scrollback_active && !scrollback_mode_active) ui_scroll_y_interpolation = 12.0f;
                            
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
        // Auto-rotate with lock: Fn+Space locks, tilt only when !rotation_locked
        {
            static unsigned long last_tilt_poll = 0;
            static unsigned long tilt_hold_start = 0;
            static uint8_t target_rot = 0;
            if (millis() - last_tilt_poll >= 200) {
                last_tilt_poll = millis();
                if (rotation_locked) { tilt_hold_start = 0; }
                else {
                    float ax, ay, az;
                    if (M5.Imu.getAccel(&ax, &ay, &az)) {
                        uint8_t desired = M5Cardputer.Display.getRotation();
                        if (ay < -7.0f) desired = 0; // portrait 90
                        else if (ay > 7.0f) desired = 2; // portrait 270
                        else if (ax < -7.0f) desired = 3; // landscape inverted
                        else if (ax > 7.0f) desired = 1; // landscape
                        else { tilt_hold_start = 0; }
                        if (desired != M5Cardputer.Display.getRotation()) {
                            if (tilt_hold_start==0 || target_rot != desired) { tilt_hold_start = millis(); target_rot = desired; }
                            if (millis() - tilt_hold_start >= 1000) {
                                M5Cardputer.Display.setRotation(desired);
                                {
                                    int dw = M5Cardputer.Display.width();
                                    int dh = M5Cardputer.Display.height();
                                    canvas.deleteSprite();
                                    canvas.createSprite(dw, dh);
                                    canvas.fillSprite(0x0000);
                                }
                                ui_needs_redraw = true;
                                queueLed(40, 500);
                                tilt_hold_start = 0;
                            }
                        } else {
                            tilt_hold_start = 0;
                        }
                    }
                }
            }
        }
        // Interrupt-driven idle check consumes kb_interrupt_fired flag to avoid polling churn
        if (kb_interrupt_fired) { kb_interrupt_fired = false; last_input_time = millis(); last_user_keyboard_input_tick = millis(); }
        
        // --- REFINED 60-SECOND PARTIAL AUTO-DIMMER + UNIFIED HARDWARE POWER & TELEMETRY ENGINE ---
        float current_battery_pct = get_calibrated_battery_percentage();
        int target_backlight_level = screen_brightness;
        static bool was_dimmed = false;
        // Always enforce Pin 38 High by default to keep NeoPixel rail alive for Mode 15/9 blips
        // pin 38 tied to backlight via scaling (no forced HIGH)
        if (current_battery_pct <= 5.0f) {
            // CRITICAL GUARD: Drop backlight to 10% minimal draw, turn off power switch to cut LED drain completely
            digitalWrite(38, LOW);
            target_backlight_level = 10;
            flush_log_cache();
            was_dimmed = false;
        } else if (current_battery_pct <= 20.0f) {
            target_backlight_level = 30;
            was_dimmed = false;
        } else {
            bool is_chat_idle_60s = (current_app_mode == MODE_CHAT && (millis() - last_user_keyboard_input_tick >= 60000));
            if (is_chat_idle_60s) {
                // Partial power-saving step: 35% brightness (89/255) readable floor, Pin 38 stays HIGH for NeoPixel
                target_backlight_level = 89;
                // pin 38 tied to backlight via scaling (no forced HIGH)
                was_dimmed = true;
            } else {
                // Instant restore on any key matrix fire: full operational brightness 100% duty cycle
                if (was_dimmed) {
                    target_backlight_level = 255; // 100% duty cycle instant restore
                    // pin 38 tied to backlight via scaling (no forced HIGH)
                    ui_needs_redraw = true;
                    was_dimmed = false;
                } else {
                    if (screen_brightness < 15) screen_brightness = 15;
                    target_backlight_level = screen_brightness;
                    // pin 38 tied to backlight via scaling (no forced HIGH)
                }
            }
        }
        g_backlight_level = target_backlight_level;
        M5Cardputer.Display.setBrightness(target_backlight_level);

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
            } else if (input_buffer.length() >= 390) {
                target_led_mode = 6;  // Buffer Shield: Flash Indigo near character cap
            }
        }

        uint8_t q = popQueuedLed();
        if (q != 255) target_led_mode = q;
        set_led_mode(target_led_mode);

        // Hardware Frame-Rate Governor Shield
        // Locks sprite writes to exactly 50 FPS (20ms intervals) to completely eliminate diagonal CRT screen tearing!
        static unsigned long last_hardware_frame_tick = 0;
        
        if (millis() - last_hardware_frame_tick >= 20) {
            last_hardware_frame_tick = millis();
            // Fractional scroll interpolation decay step (12.0f -> 0)
            if (ui_scroll_y_interpolation > 0.0f) {
                ui_scroll_y_interpolation -= 2.0f;
                if (ui_scroll_y_interpolation < 0.0f) ui_scroll_y_interpolation = 0.0f;
                ui_needs_redraw = true;
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
    M5Cardputer.begin(cfg, true); // Initialize display and keyboard matrix cleanly
    M5Cardputer.Display.setRotation(1);
    // Create canvas sprite for middle viewport (240x109) - must be done after Display init
    // Without this, pushSprite operates on 0x0 buffer -> middle stays on splash / appears frozen
    canvas.createSprite(240, 135); 
    canvas.fillSprite(0x0000);
    
    // BMI270 Wire1 tilt sensor init (SDA 2 / SCL 1 on StampS3 is handled by M5Unified, fallback Wire1 direct)
    Wire1.begin(2, 1);
    M5.Imu.init();
    // Interrupt-driven idle dimmer wait gate: hardware FALLING edge on keyboard matrix rows wakes display
    // Cardputer matrix rows map to GPIOs 8,9,10,46 via internal shift register; we attach to exposed row pins
    pinMode(8, INPUT_PULLUP);
    pinMode(46, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(8), kb_isr, FALLING);
    attachInterrupt(digitalPinToInterrupt(46), kb_isr, FALLING);

    // UN-LOCKABLE WIRE I2C REGISTER TIMEOUT PROTECTION CODES
    Wire.setTimeOut(50);
    
    // Activate Stamp module built-in NeoPixel line switch rail power output
    pinMode(38, OUTPUT);
    // pin 38 tied to backlight via scaling (no forced HIGH)
    
    // Open clean cooperative SPI bus lane pipelines
    // Cardputer SD uses SCK=40, MISO=39, MOSI=14, CS=12 per official example
    SPI.begin(40, 39, 14, 12);
    SD.begin(12, SPI, 10000000);
    // SD Wi-Fi Vault boot ingestion: parse /irc/wifi_cache.txt into transient vault array
    load_wifi_vault_from_sd();
    if (wifi_vault_count > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(wifi_vault_ssid[0], wifi_vault_pass[0]);
        Serial.printf("[VAULT] Boot auth attempt slot0: %s\n", wifi_vault_ssid[0]);
        // Keep Y=120 empty - no canvas draw here
    }
    // TWDT 4s init (panic true) - tasks registered after creation
    esp_task_wdt_init(4, true);
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
        if (M5Cardputer.Keyboard.isKeyPressed(0x08)) { // Safely catches held backspace clicks on cold boot
            safe_mode_active = true;
        }
    }
    
    load_settings_from_sd();
    if (!safe_mode_active) purge_old_logs();

    irc_mutex = xSemaphoreCreateMutex();
    sd_mutex = xSemaphoreCreateMutex();
    gLogQueue = xQueueCreate(20, sizeof(char) * 128);
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

    // Setup complete. Instantly unblock Core 1 graphics engine to draw status lines
    system_booted = true; 
    
    xTaskCreatePinnedToCore(irc_network_task, "NetworkTask", 8192, NULL, 1, &xNetworkTaskHandle, 0);
    xTaskCreatePinnedToCore(custom_ui_loop_task, "CustomUITask", 16384, NULL, 1, &xUITaskHandle, 1);
    // Register core tasks into TWDT (4s timeout, panic + emergency flush)
    esp_task_wdt_add(xNetworkTaskHandle);
    esp_task_wdt_add(xUITaskHandle);
}

void loop() {
    vTaskDelete(NULL); // Force terminate default loop task to free up loop system registers entirely
}
