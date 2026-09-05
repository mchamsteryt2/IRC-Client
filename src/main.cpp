#include <Arduino.h>
#include <M5Cardputer.h>
#include <M5GFX.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>

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
};

// ==========================================
// 🔏 REGISTERS (EXPLICIT VOLATILE STATE METRICS)
// ==========================================
volatile bool safe_mode_active = false;
volatile bool system_booted = false;
volatile bool ui_needs_redraw = true;
volatile uint8_t current_tab_index = 0;
volatile uint8_t gTabCount = 0;
volatile int current_audio = 1;
int screen_brightness = 120;

// ==========================================
// 🔀 INTERACTIVE APP-MODE STATE MACHINE
// ==========================================
enum AppMode { MODE_CHAT, MODE_WIFI, MODE_BOUNCER, MODE_SETTINGS, MODE_NAVIGATOR };
volatile AppMode current_app_mode = MODE_CHAT;
int menu_selection_idx = 0;
int nav_server_select_idx = 0;
int nav_channel_select_idx = 0;
int nav_focus_column = 0; // 0=LEFT server, 1=RIGHT channel - stealth navigator focus
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

volatile int scrollback_offset_idx = 0; // 0 = Live bottom-anchored feed, >0 = Looking at past lines
volatile bool scrollback_mode_active = false;
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
    static unsigned long last_toggle = 0;
    static bool flash_state = false;
    
    switch (mode) {
        case 0: // Mode 0: Inbound Private Message / Direct Query Alert (Flashing Cyan-White)
            if (millis() - last_toggle > 80) { flash_state = !flash_state; last_toggle = millis(); }
            if (flash_state) { r = 40; g = 95; b = 95; } // Piercing high-visibility flash completely separate from gold/orange/red rails
            break;
        case 1:  b = (sin(millis() / 300.0) + 1.0) * 15; g = b / 2; break; // Mode 1: Healthy Idle Cyan Heartbeat Wave
        case 2:  // Mode 2: Global Real-Time Mention (Purple Double-Flash Trigger)
            if (millis() - last_toggle > 150) { flash_state = !flash_state; last_toggle = millis(); }
            if (flash_state) { r = 40; b = 40; } break;
        case 3:  r = 30; g = 25; break; // Mode 3: Socket Packet Ingestion (Yellow Blip)
        case 4:  g = 40; break; // Mode 4: SD Storage Sector Append (Emerald Green Burst)
        case 5:  r = 95; g = 95; b = 95; break; // Mode 5: TextBox Underflow Boundary (High-Contrast Pure White Strobe)
        case 6:  // Mode 6: Battery Critical Alert (Crimson Breathe Wave)
            r = (sin(millis() / 200.0) + 1.0) * 20; break;
        case 7:  // Mode 7: Tab Memory Ceiling Barrier (Alternating Teal/Red Double Strobe)
            if (millis() - last_toggle > 150) { flash_state = !flash_state; last_toggle = millis(); }
            if (flash_state) { b = 70; g = 70; r = 0; } else { r = 80; g = 0; b = 0; } break;
        case 8:  r = 0; g = 0; b = 0; break; // Mode 8: Empty / Unassigned (Kept Dark)
        case 9:  r = 90; g = 12; break; // Mode 9: Wi-Fi Disconnect Fault (Solid Sharp Orange)
        case 10: r = 0; g = 0; b = 0; break; // Mode 10: Privacy Stealth Blackout Mode (Zero Dark Panel)
        case 11: // Mode 11: SD Card Mount Failure Error
            if (millis() - last_toggle > 100) { flash_state = !flash_state; last_toggle = millis(); }
            if (flash_state) { r = 90; } break;
        case 12: r = 60; b = 60; break; // Mode 12: Bouncer Socket Disconnected State (Solid Magenta)
        case 13: // Mode 13: Bouncer Identification Sequence (Slow Ice Blue Pulse)
            b = (sin(millis() / 400.0) + 1.0) * 20; r = b / 4; break;
        case 14: b = 80; break; // Mode 14: Secure SSL Encryption Handshake (Deep Royal Blue Blip)
        case 15: // Mode 15: Unread Highlight Mention Alarm Strobe (Vibrant Pulsing Gold)
            r = (sin(millis() / 150.0) + 1.0) * 40; g = r / 1.5; b = 0; break;
        case 16: r = 25; g = 25; b = 25; break; // Mode 16: Server Connection Lag Warning (Soft White)
        case 17: // Mode 17: Active Handshake Channel Synchronization (Vibrant Flashing Neon Emerald Green)
            if (millis() - last_toggle > 100) { flash_state = !flash_state; last_toggle = millis(); }
            if (flash_state) { g = 95; r = 0; b = 0; } // Intense green flash cuts straight through cyan/blue washing completely
            break;
        case 18: r = 0; g = 95; b = 0; break; // Mode 18: System Mode / App State Tab Change (Sharp Lime Green Double-Blip)
        case 19: g = 60; b = 20; break; // Mode 19: Configuration Auto-Saver File Trigger (Lime Green Blip)
        case 20: r = 60; g = 15; break; // Mode 20: Local Slash Command Macro Instruction (Warm Coral Strobe)
        case 21: r = 20; b = 50; break; // Mode 21: Tab Key Username Autocomplete Success (Cool Indigo Flare)
        case 22: // Mode 22: Critical RSSI Signal Drop Alert (Urgent Flashing Electric Yellow)
            if (millis() - last_toggle > 100) { flash_state = !flash_state; last_toggle = millis(); }
            if (flash_state) { r = 90; g = 90; b = 0; } break; // Distinct yellow flash stays clear of orange/red rails Completely
        case 23: r = 50; g = 50; b = 50; break; // Mode 23: Inbound Server Keep-Alive / CAP Sync Activity (Bright Pearl Strobe)
        case 24: r = 30; g = 20; b = 40; break; // Mode 24: Reserved Expansion Slot
        case 25: r = 40; g = 30; b = 20; break; // Mode 25: Reserved Expansion Slot
        case 26: r = 20; g = 40; b = 30; break; // Mode 26: Reserved Expansion Slot
        case 27: r = 50; g = 20; b = 20; break; // Mode 27: Reserved Expansion Slot
        case 28: r = 20; g = 50; b = 20; break; // Mode 28: Reserved Expansion Slot
        case 29: r = 20; g = 20; b = 50; break; // Mode 29: Reserved Expansion Slot
        case 30: r = 60; g = 30; b = 0; break; // Mode 30: Reserved Expansion Slot
        case 31: r = 0; g = 60; b = 30; break; // Mode 31: Reserved Expansion Slot
        case 32: r = 30; g = 0; b = 60; break; // Mode 32: Reserved Expansion Slot
        case 33: // Mode 33: Socket Connection Retry Backoff Interval (Paced Flashing Pure Red)
            if (millis() - last_toggle > 500) { flash_state = !flash_state; last_toggle = millis(); }
            if (flash_state) { r = 90; g = 0; b = 0; } break;
        case 34: r = 95; g = 15; b = 0; break; // Mode 34: Heap Memory Fragmentation Warning (Hot Burning Crimson Orange)
        case 35: r = 90; b = 50; g = 0; break; // Mode 35: Line Input Character Truncation (Bright Pink Flash)
        
        case 36: r = 40; g = 70; b = 70; break; // Mode 36: Socket Keep-Alive Dropped / Timeout (Solid Soft Cyan-White)
        case 37: // Mode 37: Configuration Corrupt / Default Restore (Flashing Laser Red/Yellow)
            if (millis() - last_toggle > 150) { flash_state = !flash_state; last_toggle = millis(); }
            if (flash_state) { r = 90; g = 0; b = 0; } else { r = 70; g = 70; b = 0; } break;
            
        default: r = 30; g = 30; b = 30; break;
    }
    neopixelWrite(21, r, g, b); // Deliver bits down to physical Pin 21
}

// ==========================================
// 💾 FILE SYSTEM AND STREAM CONFIGURATION PARSER
// ==========================================
void update_config_string(char* destination, const char* source, size_t max_len);

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
    file.printf("channel_log_enabled=%d\ncurrent_audio=%d\nscreen_brightness=%d\n", channel_log_enabled, current_audio, screen_brightness);
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
        cl.is_highlight = (strstr(msg, irc_nick) != NULL);
        t.line_count++;

    // Dynamic Session Log Rotator and Auto-Cleaning Engine Pass
    if (channel_log_enabled == 1 && safe_mode_active == false) {
        unsigned long current_sync_sec = (millis() / 1000) + adj_time;
        uint32_t active_yr  = 2026; 
        uint32_t active_mon = ((current_sync_sec / 2629743) % 12) + 1; 
        uint32_t active_day = ((current_sync_sec / 86400) % 31) + 1;   

        // Isolate and sanitize server string handles to prevent nested subdirectory errors
        String safe_server = String(t.server);
        safe_server.replace("/", "_"); safe_server.replace("\\", "_"); safe_server.trim();
        
        String safe_room = String(t.name);
        safe_room.replace("/", "_"); safe_room.replace("\\", "_"); safe_room.trim();

        char dir_path_buffer[64] = {0};
        snprintf(dir_path_buffer, sizeof(dir_path_buffer), "/irc/logs/%s", safe_server.c_str());
        
        // Ensure the clean base server folder exists on the card first
        if (!SD.exists(dir_path_buffer)) {
            SD.mkdir(dir_path_buffer);
        }
        
        // Compile the absolute file location block using sanitized string structures
        char file_path_buffer[128] = {0};
        snprintf(file_path_buffer, sizeof(file_path_buffer), "/irc/logs/%s/%s_%04d_%02d_%02d.log", 
                 safe_server.c_str(), safe_room.c_str(), active_yr, active_mon, active_day);

        // Flash our Emerald Green storage indicator (Mode 4) to flag write execution
        set_led_mode(4);
        File channel_log_file = SD.open(file_path_buffer, FILE_APPEND);
        if (channel_log_file) {
            channel_log_file.printf("[%s] <%s>: %s\r\n", cl.timeStr, cl.nick, cl.message);
            channel_log_file.close();
        } else {
            set_led_mode(28); // Drop into Solid Yellow status if micro-SD write operation fails
        }
    }

        // Cross-Network Highlights Duplicator Pass
        if (strstr(msg, irc_nick) != NULL && current_tab_index != 0) {
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
    
    // ==========================================
    // STATE 1: WORKSPACE NAVIGATION HUB (Fn + P)
    // ==========================================
    if (current_app_mode == MODE_NAVIGATOR) {
        canvas.fillSprite(0x0000); // Wipe frame buffer clean
        
        // Extended Shield Panel Matrix: Paint layout blocks completely down the full height boundary limits
        canvas.fillRect(0, 0, 114, 135, 0x0841);   // Slate left panel container block
        canvas.fillRect(117, 0, 123, 135, 0x0000); // Master deep black right panel layer shield
        
        canvas.setTextColor(0x07E0); canvas.setCursor(6, 6); canvas.print("NETWORKS");
        
        // Render network column
        canvas.setTextColor(nav_server_select_idx == 0 ? 0xFFFF : 0x7BEF);
        canvas.setCursor(12, 24); canvas.printf("%s [ALL NETS]", (nav_server_select_idx == 0 ? ">" : " "));
        for (int i = 0; i < discovered_network_count; i++) {
            canvas.setTextColor(nav_server_select_idx == (i + 1) ? 0xFFFF : 0x7BEF);
            canvas.setCursor(12, 38 + (i * 14));
            canvas.printf("%s %s", (nav_server_select_idx == (i + 1) ? ">" : " "), discovered_networks[i]);
        }
        
        canvas.drawFastVLine(116, 0, 135, 0x7BEF); // Layout wire line
        canvas.setTextColor(0xFD20); canvas.setCursor(122, 6); canvas.print("ACTIVE ROOMS");
        
        // SCROLLABLE WINDOW GRID INDEX LIMITERS
        int channel_print_counter = 0;
        int active_window_scroll_offset = (nav_channel_select_idx >= 7) ? (nav_channel_select_idx - 6) : 0;
        
        for (int i = 0; i < gTabCount; i++) {
            bool should_print = (nav_server_select_idx == 0) || 
                                 (strcasecmp(gTabs[i].server, discovered_networks[nav_server_select_idx - 1]) == 0);
            
            if (should_print) {
                // Constrain row displays tightly to match our vertical viewport row slots budget
                if (channel_print_counter >= active_window_scroll_offset && channel_print_counter < active_window_scroll_offset + 7) {
                    int draw_y_row_coordinate = 24 + ((channel_print_counter - active_window_scroll_offset) * 14);
                    
                    uint16_t txt_color = (nav_channel_select_idx == channel_print_counter) ? 0xFFFF : 0x7BEF;
                    canvas.setCursor(126, draw_y_row_coordinate);
                    
                    int local_mention_count = 0;
                    for (int l = 0; l < gTabs[i].line_count; l++) {
                        if (gTabs[i].lines[l].is_highlight) local_mention_count++;
                    }

                    int max_chars_allowed = (local_mention_count > 0) ? 10 : 16;
                    char truncated_chan_name[20] = {0};
                    if (strlen(gTabs[i].name) > max_chars_allowed) {
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
                }
                channel_print_counter++;
            }
        }
        
        // Overlay unified control bar instructions flush across the absolute bottom edge row strip
        canvas.fillRect(0, 122, 240, 13, 0x0841);
        canvas.drawFastHLine(0, 121, 240, 0x7BEF);
        canvas.setCursor(6, 124); canvas.setTextColor(0xFFFF); canvas.print(";/. Scroll List | Enter: Switch Room Context");
        canvas.pushSprite(0, 0); ui_needs_redraw = false; return;
    }

    // ==========================================
    // STATE 3: LIVE TERMINAL CHAT VIEWPORT
    // ==========================================
    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        canvas.fillSprite(0x0000);
        Tab &t = gTabs[current_tab_index];
        // Reverse-Viewport Engine with active Scrollback Reviewer support offsets
        int current_y = 97; 
        int start_array_idx = (t.line_count - 1) - scrollback_offset_idx;
        if (start_array_idx < 0) start_array_idx = 0;

        for (int i = start_array_idx; i >= 0; i--) {
            if (current_y < 2) break;
            if (current_y > 115) { current_y -= 12; continue; }
            
            uint16_t row_bg = (i % 2 == 0) ? 0x0000 : 0x0841;
            int text_start_x = (current_tab_index == 0) ? 110 : 70;
            int max_text_width = 236 - text_start_x;
            
            String msg_text = t.lines[i].message;
            int current_char_pos = 0;
            bool first_line_pass = true;

            // Simple micro-font length-chopper string loop
            while (current_char_pos < msg_text.length()) {
                // Calculate cursor offsets dynamically depending on whether this is a trailing row pass
                int active_render_x = first_line_pass ? text_start_x : (text_start_x + 6);
                int dynamic_max_width = 236 - active_render_x;
                int dynamic_chars_budget = dynamic_max_width / 6;
                if (dynamic_chars_budget <= 0) break;

                String sub_line = msg_text.substring(current_char_pos, current_char_pos + dynamic_chars_budget);
                canvas.fillRect(0, current_y - 2, 240, 12, row_bg);
                
                if (first_line_pass) {
                    canvas.drawFastVLine(64, 0, 120, 0x7BEF); 
                    
                    char shortened_nick[9] = {0};
                    if (strlen(t.lines[i].nick) > 8) {
                        strncpy(shortened_nick, t.lines[i].nick, 6);
                        strcat(shortened_nick, "..");
                    } else {
                        strncpy(shortened_nick, t.lines[i].nick, 8);
                    }

                    if (t.lines[i].is_highlight) { 
                        canvas.fillRect(2, current_y - 1, 60, 11, 0xFD20); canvas.setTextColor(0xFFFF); 
                    } else { 
                        canvas.setTextColor(get_nick_palette_color(t.lines[i].nick)); 
                    }
                    
                    canvas.setCursor(2, current_y);
                    if (current_tab_index == 0) canvas.printf("%s", shortened_nick);
                    else canvas.printf("<%s>", shortened_nick);
                    first_line_pass = false;
                }
                
                canvas.setTextColor(t.lines[i].color); 
                canvas.setCursor(active_render_x, current_y); // Render line with smart column layout padding
                canvas.print(sub_line);
                
                // Shift your drawing line UPWARDS for the next row loop pass
                current_y -= 12; 
                current_char_pos += dynamic_chars_budget;
            }
        }
        xSemaphoreGive(irc_mutex);
    }
    canvas.pushSprite(0, 12);

    // NAVBAR RENDERING SYSTEM
    M5Cardputer.Display.fillRect(0, 0, 240, 12, 0x0841);
    M5Cardputer.Display.setTextColor(0x7BEF, 0x0841); M5Cardputer.Display.setCursor(2, 2);
    M5Cardputer.Display.printf("[%s]", gTabs[current_tab_index].server);
    M5Cardputer.Display.setTextColor(0xFFFF, 0x0841); M5Cardputer.Display.setCursor(54, 2);
    char truncated_name[12] = {0}; strncpy(truncated_name, gTabs[current_tab_index].name, 10);
    M5Cardputer.Display.print(truncated_name);

    M5Cardputer.Display.setCursor(142, 2);
    if (WiFi.status() != WL_CONNECTED) { M5Cardputer.Display.setTextColor(0xF800, 0x0841); M5Cardputer.Display.print("D"); }
    else { M5Cardputer.Display.setTextColor(0x07E0, 0x0841); M5Cardputer.Display.print("W"); }
    
    M5Cardputer.Display.setCursor(154, 2);
    if (current_audio == 0) { M5Cardputer.Display.setTextColor(0xF800, 0x0841); M5Cardputer.Display.print("H"); }
    else { M5Cardputer.Display.setTextColor(0x07E0, 0x0841); M5Cardputer.Display.print("V"); }

    // Clean Navbar Logging Status Anchor (L = Logging Active | N = No Logs)
    M5Cardputer.Display.setCursor(166, 2);
    if (channel_log_enabled == 1) { M5Cardputer.Display.setTextColor(0x07E0, 0x0841); M5Cardputer.Display.print("L"); }
    else { M5Cardputer.Display.setTextColor(0x7BEF, 0x0841); M5Cardputer.Display.print("N"); }
    
    unsigned long current_sync_sec = (millis() / 1000) + adj_time;
    uint32_t active_min = (current_sync_sec / 60) % 60;
    uint32_t active_hr  = ((current_sync_sec / 3600) + current_tz_idx) % (use_12_hour_format ? 12 : 24);
    if (use_12_hour_format && active_hr == 0) active_hr = 12;
    
    // Shift clock leftward to open up structural spacing slots
    M5Cardputer.Display.setTextColor(0xFFFF, 0x0841); 
    M5Cardputer.Display.setCursor(156, 2); 
    M5Cardputer.Display.printf("%02d:%02d", active_hr, active_min);
    
    // Pull the battery string into its own absolute far-right layout column
    M5Cardputer.Display.setCursor(208, 2); 
    M5Cardputer.Display.printf("%d%%", (int)get_calibrated_battery_percentage());

    // Low-Profile Navbar Activity Indicator Dots Engine
    // Draws tiny 2px tracking dots along the navbar floor to alert your eyes to background channel traffic
    for (int t = 1; t < gTabCount; t++) {
        if (t == current_tab_index) continue; // Skip drawing a dot for the active tab you are looking at
        
        int dot_x = 2 + (t * 6); // Space out alert pins cleanly across the left navbar edge
        if (dot_x > 130) break;   // Boundary shield to prevent dots from bleeding into network text labels
        
        bool has_unread_highlight = false;
        bool has_unread_msg = (gTabs[t].line_count > 0);
        
        for (int l = 0; l < gTabs[t].line_count; l++) {
            if (gTabs[t].lines[l].is_highlight) { has_unread_highlight = true; break; }
        }
        
        if (has_unread_highlight) {
            M5Cardputer.Display.fillRect(dot_x, 10, 3, 2, 0xFD20); // High-contrast Amber dot for unread mentions
        } else if (has_unread_msg) {
            M5Cardputer.Display.fillRect(dot_x, 10, 3, 2, 0x07FF); // Soft Cyan dot for standard background chatter
        }
    }

    // LOWER INPUT BOX
    M5Cardputer.Display.fillRect(0, 121, 240, 14, 0x0000);
    M5Cardputer.Display.drawFastHLine(0, 121, 240, 0x7BEF);
    M5Cardputer.Display.setTextColor(0xFD20, 0x0000); M5Cardputer.Display.setCursor(4, 124); M5Cardputer.Display.print("> ");M5Cardputer.Display.setTextColor(0xFFFF, 0x0000); M5Cardputer.Display.print(input_buffer.c_str());ui_needs_redraw = false;
}void handle_keyboard_inputs() {
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

    if (is_fn && M5Cardputer.Keyboard.isKeyPressed('s')) {
        current_audio = (current_audio == 1) ? 0 : 1; // 0 = Stealth Active, 1 = Normal Active
        
        if (current_audio == 0) {
            M5Cardputer.Display.setBrightness(1); // Force drop backlight to absolute minimum
            neopixelWrite(21, 0, 0, 0);          // Kill the diagnostic LED entirely
        } else {
            M5Cardputer.Display.setBrightness(screen_brightness); // Restore full user brightness
        }
        ui_needs_redraw = true;
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

    // Layer C: Critical Hardware Intercept for Fn+Arrow Punctuation Codes
    // On the Cardputer layout, Fn+Arrows outputs direct character values:
    // Fn+Left = ';' | Fn+Right = '/' | Fn+Up = ',' | Fn+Down = '.'
    if (current_app_mode == MODE_CHAT) {
        if (is_fn && M5Cardputer.Keyboard.isKeyPressed(';')) { // Fn + UP Arrow
            Tab &t = gTabs[current_tab_index];
            if (scrollback_offset_idx < t.line_count - 1) {
                scrollback_offset_idx++; // Shift old message history down into viewport frame
                ui_needs_redraw = true;
            }
            return;
        }
        if (is_fn && M5Cardputer.Keyboard.isKeyPressed('.')) { // Fn + DOWN Arrow
            if (scrollback_offset_idx > 0) {
                scrollback_offset_idx--; // Scroll back down toward the newest messages
                ui_needs_redraw = true;
            }
            return;
        }
        if ((is_alt && M5Cardputer.Keyboard.isKeyPressed(0x08)) || M5Cardputer.Keyboard.isKeyPressed('`')) {
            scrollback_offset_idx = 0; // Escape key sequence instantly snaps viewport right back to live chat
            ui_needs_redraw = true;
        }

        // Horizontal Quick-Tab Swapper (Fn + Comma = Previous Tab | Fn + Slash = Next Tab)
        if (is_fn && M5Cardputer.Keyboard.isKeyPressed(',')) { // Previous Tab
            if (gTabCount > 1) {
                current_tab_index = (current_tab_index - 1 + gTabCount) % gTabCount;
                scrollback_offset_idx = 0;
                ui_needs_redraw = true;
                
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
            // Enter maps to current_tab_index
            if (status.enter) {
                int channel_print_counter = 0;
                for (int i = 0; i < gTabCount; i++) {
                    if (strcmp(gTabs[i].server, discovered_networks[nav_server_select_idx]) == 0) {
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
                    if (nav_server_select_idx == 0 || strcmp(gTabs[i].server, discovered_networks[nav_server_select_idx - 1]) == 0) total_chans++;
                }
                if (nav_channel_select_idx < total_chans - 1) { nav_channel_select_idx++; ui_needs_redraw = true; }
            } else {
                // Determine our menu navigation boundary max depending on active layout modes
                int max_limit = (current_app_mode == MODE_WIFI) ? 4 : ((current_app_mode == MODE_SETTINGS) ? 3 : 2);
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
                // Precision Aligned Evaluation Gate matching our draw_chat_view screen layout rules
                bool is_room_visible = (nav_server_select_idx == 0) || 
                                       (strcmp(gTabs[i].server, discovered_networks[nav_server_select_idx - 1]) == 0);
                
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
                
                // AUTOMATIC READ-STATE CLEANUP ENGINE
                // Forcefully sweep the selected channel tab and wipe out all highlight alerts instantly!
                if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    Tab &current_view_tab = gTabs[current_tab_index];
                    for (int l = 0; l < current_view_tab.line_count; l++) {
                        current_view_tab.lines[l].is_highlight = false; 
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
                    bool is_room_visible = (nav_server_select_idx == 0) || 
                                           (strcmp(gTabs[i].server, discovered_networks[nav_server_select_idx - 1]) == 0);
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
                ui_needs_redraw = true; 
            }
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
            input_buffer = "";
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

        if (WiFi.status() != WL_CONNECTED) {
            set_led_mode(9); // Orange Network Fault Light
            
            if (last_wifi_fail_tick == 0) last_wifi_fail_tick = millis();
            
            // If primary network link stays dead for more than 10 seconds, failover roam to backup credentials
            if (millis() - last_wifi_fail_tick > 10000 && strlen(wifi_ssid2) > 0) {
                WiFi.disconnect(true);
                vTaskDelay(pdMS_TO_TICKS(100));
                
                using_backup_ap = !using_backup_ap; // Alternate target networks
                const char* target_ssid = using_backup_ap ? wifi_ssid2 : wifi_ssid;
                const char* target_pass = using_backup_ap ? wifi_pass2 : wifi_pass;
                
                Serial.printf("[NET] Hotspot Auto-Roaming triggered! Target: %s\n", target_ssid);
                WiFi.begin(target_ssid, target_pass);
                last_wifi_fail_tick = millis(); // Reset timer for next rotation check
            }
            continue;
        }
        last_wifi_fail_tick = 0; // Reset ticker once connection settles healthy

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
                
                // CRITICAL STRUCTURAL OVERRIDE: Forcefully halt the client and purge 
                // any leaking cryptographic session wrappers before re-allocating a new socket!
                net_client.stop(); 
                
                net_client.setInsecure();
                Serial.printf("[NET] Purged old TLS buffers. Executing clean connect pass for: %s\n", discovered_networks[i]);
                if (net_client.connect(bnc_host, bnc_port)) {
                    net_client.printf("PASS %s:%s\r\n", bnc_user, bnc_pass);
                    net_client.print("CAP REQ :server-time\r\n");
                    net_client.printf("NICK %s\r\n", irc_nick);
                    net_client.printf("USER %s/%s 0 * :M5 Client\r\n", bnc_user, discovered_networks[i]);
                }
            }

            if (net_client.connected() && net_client.available()) {
                // Secure a 0-millisecond execution timeout pass to forcefully kill read-stalls completely
                net_client.setTimeout(0);
                
                char packet_chunk[512] = {0};
                // Pull an entire data paragraph block into internal RAM in a single, atomic hardware operation
                int bytes_read = net_client.readBytesUntil('\n', packet_chunk, sizeof(packet_chunk) - 1);
                
                if (bytes_read <= 0) continue;
                packet_chunk[bytes_read] = '\0';
                
                String line = String(packet_chunk);
                line.trim(); line.replace("\r", "");
                if (line.length() == 0) continue;

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
                // 🛑 PROTOCOL DROP SHIELD MASK
                // ==========================================
                // Intercept raw 353 (NAMES list) and 366 (End of NAMES) protocol blocks.
                // Drop them out of execution instantly so they can never flood message buffers or flash screens!
                if (line.indexOf(" 353 ") != -1 || line.indexOf(" 366 ") != -1) {
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
                            cl.is_highlight = (actual_msg.indexOf(irc_nick) != -1);
                            t.line_count++;
                            
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
        
        // Native M5Unified Background Backlight Driver Controller
        if (current_audio == 0) {
            M5Cardputer.Display.setBrightness(0); // Privacy Stealth Mode: Kill panel backlight cleanly
            neopixelWrite(21, 0, 0, 0);          // Turn off the diagnostic corner LED completely
        } else if (millis() - last_input_time > 60000) {
            M5Cardputer.Display.setBrightness(10); // Ambient Sleep Dim Mode: Drop to trace visibility
        } else {
            // Guardrail protection: Ensure screen_brightness never drops below 15 to prevent panel shutoffs
            if (screen_brightness < 15) screen_brightness = 15; 
            M5Cardputer.Display.setBrightness(screen_brightness);
        }
        
        // Hierarchical 24-Mode Diagnostic LED Status Selector Matrix
        uint8_t target_led_mode = 1; // Default fallback to Mode 1 (Cyan Idle Heartbeat)

        if (safe_mode_active) {
            target_led_mode = 0;
        } else if (current_audio == 0) {
            target_led_mode = 10; // Privacy Stealth Blackout Override
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
                target_led_mode = 15; // Priority: Unread Highlight Mention Alarm Strobe (Pulsing Gold)
            } else if (current_app_mode == MODE_NAVIGATOR && nav_server_select_idx > 0 && !network_handshake_complete[nav_server_select_idx - 1]) {
                target_led_mode = 17; // State: Vibrant Flashing Neon Emerald Green strictly during active server node handshakes
            } else if (input_buffer.length() >= 390) {
                target_led_mode = 6;  // Buffer Shield: Flash Indigo near character cap
            }
        }

        set_led_mode(target_led_mode);
        
        // Hardware Frame-Rate Governor Shield
        // Locks sprite writes to exactly 50 FPS (20ms intervals) to completely eliminate diagonal CRT screen tearing!
        static unsigned long last_hardware_frame_tick = 0;
        
        if (ui_needs_redraw && (millis() - last_hardware_frame_tick >= 20)) {
            last_hardware_frame_tick = millis();
            draw_chat_view();
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
    cfg.external_spk = false; // Turn off legacy I2S audio channels cleanly to prevent core panics
    M5Cardputer.begin(cfg, true); // Initialize display and keyboard matrix cleanly
    M5Cardputer.Display.setRotation(1);
    // Create canvas sprite for middle viewport (240x109) - must be done after Display init
    // Without this, pushSprite operates on 0x0 buffer -> middle stays on splash / appears frozen
    canvas.createSprite(240, 109);
    canvas.setTextSize(1);
    
    // UN-LOCKABLE WIRE I2C REGISTER TIMEOUT PROTECTION CODES
    Wire.setTimeOut(50);
    
    // Activate Stamp module built-in NeoPixel line switch rail power output
    pinMode(38, OUTPUT);
    digitalWrite(38, HIGH);
    
    // Open clean cooperative SPI bus lane pipelines
    // Cardputer SD uses SCK=40, MISO=39, MOSI=14, CS=12 per official example
    SPI.begin(40, 39, 14, 12);
    SD.begin(12, SPI, 10000000);
    
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

    // Setup complete. Instantly unblock Core 1 graphics engine to draw status lines
    system_booted = true; 
    
    xTaskCreatePinnedToCore(irc_network_task, "NetworkTask", 8192, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(custom_ui_loop_task, "CustomUITask", 16384, NULL, 1, NULL, 1);
}

void loop() {
    vTaskDelete(NULL); // Force terminate default loop task to free up loop system registers entirely
}
