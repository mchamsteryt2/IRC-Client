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
char irc_nick[64]  = {0};
char bnc_host[64]  = {0};
int bnc_port       = 0;
char bnc_user[64]  = {0};
char bnc_pass[64]  = {0};
int channel_log_enabled = 1;
int current_tz_idx      = 2;
int use_12_hour_format  = 1;

#define MAX_NETWORKS 5
char discovered_networks[MAX_NETWORKS][32] = {{0}};
volatile uint8_t discovered_network_count = 0;
WiFiClientSecure clients[MAX_NETWORKS];
bool network_authenticated[MAX_NETWORKS] = {false};
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
        case 1: // Mode 1: Healthy Cyan Idle Heartbeat
            b = (sin(millis() / 300.0) + 1.0) * 15;
            g = b / 3; // Shift slightly heavier blue to stop wafer color bleed
            break;
        case 2: // Mode 2: Highlight Mention Purple Pulse
            if (millis() - last_toggle > 200) { flash_state = !flash_state; last_toggle = millis(); }
            if (flash_state) { r = 40; b = 40; }
            break;
        case 3: // Mode 3: Latency Yellow Blip (Triggered on server PONG)
            r = 30; g = 25;
            break;
        case 4: // Mode 4: Storage Sync Green Burst
            g = 40;
            break;
        case 5: // Mode 5: Boundary Empty Red Flash
            r = 80;
            break;
        case 6: // Mode 6: Critical Power Crimson Heartbeat
            r = (sin(millis() / 200.0) + 1.0) * 20;
            break;
        case 8: // Mode 8: RSSI Signal Fade Magenta
            r = 25; b = 25;
            break;
        case 9: // Mode 9: Disconnect Fault Orange
            r = 90; g = 12;
            break;
        default: // Safe Mode Override: Pure, High-Saturation Laser Red Alert
            r = 60;
            break;
    }
    neopixelWrite(21, r, g, b);
}

// ==========================================
// 💾 FILE SYSTEM AND STREAM CONFIGURATION PARSER
// ==========================================
void load_settings_from_sd() {
    if (safe_mode_active) return;
    
    File file = SD.open("/irc/config.txt", FILE_READ);
    if (!file) {
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
        if (strcmp(key, "wifi_ssid")==0) strncpy(wifi_ssid, value, sizeof(wifi_ssid) - 1);
        else if (strcmp(key, "wifi_pass")==0) strncpy(wifi_pass, value, sizeof(wifi_pass) - 1);
        else if (strcmp(key, "irc_nick")==0) strncpy(irc_nick, value, sizeof(irc_nick) - 1);
        else if (strcmp(key, "bnc_host")==0) strncpy(bnc_host, value, sizeof(bnc_host) - 1);
        else if (strcmp(key, "bnc_port")==0) { bnc_port = atoi(value); }
        else if (strcmp(key, "bnc_user")==0) strncpy(bnc_user, value, sizeof(bnc_user) - 1);
        else if (strcmp(key, "bnc_pass")==0) strncpy(bnc_pass, value, sizeof(bnc_pass) - 1);
        else if (strcmp(key, "channel_log_enabled")==0) channel_log_enabled = atoi(value);
        else if (strcmp(key, "screen_brightness")==0) screen_brightness = atoi(value);
        else if (strcmp(key, "current_tz_idx")==0) current_tz_idx = atoi(value);
        else if (strcmp(key, "use_12_hour_format")==0) use_12_hour_format = atoi(value);
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
        int target_idx = 0;
        if (gTabCount > 0 && current_tab_index < gTabCount) target_idx = current_tab_index;
        
        Tab &t = gTabs[target_idx];
        if (t.line_count >= MSG_BUFFER_SIZE) {
            for (int i = 1; i < MSG_BUFFER_SIZE; i++) t.lines[i-1] = t.lines[i];
            t.line_count = MSG_BUFFER_SIZE - 1;
        }
        
        ChatLine &cl = t.lines[t.line_count];
        if (timeStr) strncpy(cl.timeStr, timeStr, sizeof(cl.timeStr)-1);
        else strncpy(cl.timeStr, "00:00", sizeof(cl.timeStr)-1);
        strncpy(cl.nick, source, sizeof(cl.nick)-1);
        strncpy(cl.message, msg, sizeof(cl.message)-1);
        cl.color = color;
        cl.is_highlight = (strstr(msg, irc_nick) != NULL);
        t.line_count++;

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
    // --- DECOUPLED LOG WRITING (SD I/O NOT UNDER irc_mutex to avoid UI freeze/crash) ---
    if (channel_log_enabled == 1) {
        char logChannel[32] = {0};
        if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            int idx = 0;
            if (gTabCount > 0 && current_tab_index < gTabCount) idx = current_tab_index;
            strncpy(logChannel, gTabs[idx].name, sizeof(logChannel)-1);
            xSemaphoreGive(irc_mutex);
        } else {
            if (gTabCount > 0 && current_tab_index < gTabCount) strncpy(logChannel, gTabs[current_tab_index].name, sizeof(logChannel)-1);
        }
        if (logChannel[0] == 0) return;
        bool sd_locked = false;
        if (sd_mutex) sd_locked = (xSemaphoreTake(sd_mutex, pdMS_TO_TICKS(50)) == pdTRUE);
        else sd_locked = true;
        if (sd_locked) {
            char logPath[64];
            bool isQuery = (logChannel[0] == '>');
            bool isSystem = (strcmp(logChannel, "~system") == 0);
            if (isSystem) snprintf(logPath, sizeof(logPath), "/irc/logs/system.log");
            else if (isQuery) snprintf(logPath, sizeof(logPath), "/irc/logs/query.log");
            else {
                char safeChan[32];
                strncpy(safeChan, logChannel, sizeof(safeChan)-1);
                for (char* p=safeChan; *p; ++p) if (*p=='/' || *p=='\\') *p='_';
                snprintf(logPath, sizeof(logPath), "/irc/logs/%s.log", safeChan[0]?safeChan:"system");
            }
            if (!SD.exists("/irc")) SD.mkdir("/irc");
            if (!SD.exists("/irc/logs")) SD.mkdir("/irc/logs");
            File f = SD.open(logPath, FILE_APPEND);
            if (f) {
                if (isSystem || isQuery) f.printf("%s %s: %s\n", logChannel, source, msg);
                else f.printf("%s\n", msg);
                f.close();
            }
            if (sd_mutex) xSemaphoreGive(sd_mutex);
        }
    }
}

float get_calibrated_battery_percentage() {
    static unsigned long last_read = 0;
    static float smoothed_pct = 50.0f;
    static float last_raw = 3.9f;
    // Throttle I2C PMIC reads to every 5s to avoid bus contention / crashes after boot
    if (millis() - last_read > 5000 || last_read == 0) {
        last_raw = M5Cardputer.Power.getBatteryVoltage();
        last_read = millis();
    }
    float raw_volt = last_raw;
    if (raw_volt > 4.2f) raw_volt = 4.2f;
    if (raw_volt < 3.3f) raw_volt = 3.3f;
    float percentage = ((raw_volt - 3.3f) / (4.2f - 3.3f)) * 100.0f;
    smoothed_pct = (smoothed_pct * 0.95f) + (percentage * 0.05f); // Exponential Moving Average Filter
    return smoothed_pct;
}

uint16_t get_nick_palette_color(const char* nick) {
    uint32_t hash = 5381;
    while (*nick) { hash = ((hash << 5) + hash) + *nick++; }
    const uint16_t palette[] = {0x07FF, 0xFDA0, 0xF81F, 0x07E0, 0xAFE5, 0xFED0, 0x867F}; 
    return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}

// ==========================================
// 🎬 RETRO-TERMINAL GRAPHICS RENDERING ENGINE
// ==========================================
void draw_chat_view() {
    if (!ui_needs_redraw) return;

    // 1. SAFEMODE DIRECT-DRAW GATE BYPASS
    if (safe_mode_active) {
        canvas.fillSprite(0x0000);
        canvas.setTextColor(0x7BEF); // Mid-contrast Terminal Slate Grey
        canvas.setCursor(10, 20);
        canvas.print("[!] INITIALIZATION BYPASS:");
        canvas.setTextColor(0xFD20); // Amber Terminal Orange
        canvas.setCursor(10, 45);
        canvas.print("~safemode console active");
        canvas.setCursor(10, 60);
        canvas.print("press Alt+Backspace to exit");
        canvas.pushSprite(0, 12);
        
        M5Cardputer.Display.fillRect(0, 0, 240, 12, 0x0841);
        M5Cardputer.Display.setTextColor(0xF800, 0x0841);
        M5Cardputer.Display.setCursor(5, 2);
        M5Cardputer.Display.print("SAFE MODE");
        ui_needs_redraw = false;
        return;
    }

    // --- MODAL DRAWING RESPONSES: route viewport on current_app_mode ---
    if (current_app_mode == MODE_NAVIGATOR) {
        canvas.fillSprite(0x0000);
        
        // --- LEFT COLUMN: NETWORK SERVERS (X=5 TO X=110) ---
        canvas.fillRect(0, 0, 114, 135, 0x0841); // Slate container block
        canvas.setTextColor(0x07E0); canvas.setCursor(6, 6); canvas.print("NETWORKS");
        for (int i = 0; i < discovered_network_count; i++) {
            uint16_t txt_color = (nav_server_select_idx == i) ? 0xFFFF : 0x7BEF;
            canvas.setTextColor(txt_color);
            canvas.setCursor(12, 24 + (i * 14));
            canvas.printf("%s %s", (nav_server_select_idx == i ? ">" : " "), discovered_networks[i]);
        }
        
        // --- RIGHT COLUMN: CHANNELS LIST ROSTER (X=120 TO X=235) ---
        canvas.drawFastVLine(116, 0, 135, 0x7BEF); // Split window wire guide line
        canvas.setTextColor(0xFD20); canvas.setCursor(122, 6); canvas.print("ACTIVE ROOMS");
        
        int channel_print_counter = 0;
        for (int i = 0; i < gTabCount; i++) {
            // Filter and draw strictly the channels belonging to our selected network node
            if (strcmp(gTabs[i].server, discovered_networks[nav_server_select_idx]) == 0) {
                uint16_t txt_color = (nav_channel_select_idx == channel_print_counter) ? 0xFFFF : 0x7BEF;
                canvas.setTextColor(txt_color);
                canvas.setCursor(126, 24 + (channel_print_counter * 14));
                canvas.printf("%s %s", (nav_channel_select_idx == channel_print_counter ? "*" : " "), gTabs[i].name);
                channel_print_counter++;
            }
        }
        
        canvas.setCursor(6, 122); canvas.setTextColor(0x7BEF); canvas.print("Arrows: Nav | Enter: Open Channel");
        canvas.pushSprite(0, 0);
        ui_needs_redraw = false;
        return;
    }
    if (current_app_mode != MODE_CHAT) {
        switch (current_app_mode) {
            case MODE_SETTINGS: {
                canvas.fillSprite(0x0000);
                canvas.setTextColor(0xFD20); canvas.setCursor(10, 5);
                canvas.print("--- SYSTEM CONFIGURATIONS ---");
                canvas.setTextColor(0xFFFF);
                canvas.setCursor(10, 24); canvas.printf("%s 1. Brightness Level: %d", (menu_selection_idx == 0 ? ">" : " "), screen_brightness);
                canvas.setCursor(10, 38); canvas.printf("%s 2. Timezone Indicator: %d", (menu_selection_idx == 1 ? ">" : " "), current_tz_idx);
                canvas.setCursor(10, 52); canvas.printf("%s 3. 12/24hr Format: %s", (menu_selection_idx == 2 ? ">" : " "), use_12_hour_format ? "12-HOUR" : "24-HOUR");
                canvas.setCursor(10, 66); canvas.printf("%s 4. Channel Log File: %s", (menu_selection_idx == 3 ? ">" : " "), channel_log_enabled ? "ON" : "OFF");
                canvas.setCursor(10, 80); canvas.printf("%s 5. Purge History: [ RUN ]", (menu_selection_idx == 4 ? ">" : " "));
                canvas.setCursor(10, 94); canvas.printf("%s 6. Erase Local Configs: [ RUN ]", (menu_selection_idx == 5 ? ">" : " "));
                canvas.setCursor(10, 110); canvas.setTextColor(0x7BEF); canvas.print("Alt+Backspace: Exit | Fn+P: Next Page");
                canvas.pushSprite(0, 12);
                ui_needs_redraw = false;
                return;
            }
            case MODE_BOUNCER: {
                canvas.fillSprite(0x0000);
                canvas.setTextColor(0x07E0); canvas.setCursor(10, 8);
                canvas.print("--- BOUNCER CONNECTION SCHEMAS ---");
                canvas.setTextColor(0xFFFF);
                
                // Explicitly cast bnc character array structures safely
                canvas.setCursor(10, 26); canvas.printf("%s 1. Server Host: %s", (menu_selection_idx == 0 ? ">" : " "), (const char*)bnc_host);
                canvas.setCursor(10, 40); canvas.printf("%s 2. Port Address: %d", (menu_selection_idx == 1 ? ">" : " "), bnc_port);
                canvas.setCursor(10, 54); canvas.printf("%s 3. Username Key: %s", (menu_selection_idx == 2 ? ">" : " "), (const char*)bnc_user);
                canvas.setCursor(10, 68); canvas.printf("%s 4. Password Token: *******", (menu_selection_idx == 3 ? ">" : " "));
                canvas.setCursor(10, 82); canvas.printf("%s 5. Synchronize Now: [ EXPORT ]", (menu_selection_idx == 4 ? ">" : " "));
                
                canvas.setCursor(10, 120); canvas.setTextColor(0x7BEF); canvas.print("Alt+Backspace: Exit | Fn+P: Next Page");
                canvas.pushSprite(0, 0);
                ui_needs_redraw = false;
                return;
            }
            case MODE_WIFI: {
                canvas.fillSprite(0x0000);
                canvas.setTextColor(0x5A1F); canvas.setCursor(10, 8);
                canvas.print("--- WI-FI CONFIG MANAGER ---");
                canvas.setTextColor(0xFFFF);
                
                // Explicitly cast raw pointers to stable string pointers
                canvas.setCursor(10, 26); canvas.printf("%s 1. Active SSID: %s", (menu_selection_idx == 0 ? ">" : " "), (const char*)wifi_ssid);
                canvas.setCursor(10, 40); canvas.printf("%s 2. Network Pass: *******", (menu_selection_idx == 1 ? ">" : " "));
                canvas.setCursor(10, 54); canvas.printf("%s 3. Scan for Airwaves: [ SCAN APs ]", (menu_selection_idx == 2 ? ">" : " "));
                canvas.setCursor(10, 68); canvas.printf("%s 4. Force Connect: [ INITIALIZE ]", (menu_selection_idx == 3 ? ">" : " "));
                
                canvas.setCursor(10, 120); canvas.setTextColor(0x7BEF); canvas.print("Alt+Backspace: Exit | Fn+P: Main Chat");
                canvas.pushSprite(0, 0);
                ui_needs_redraw = false;
                return;
            }
            default: break;
        }
    }

    // 2. SELF-HEALING UN-ALIGNED DATA SAFETY CHECKPOINT
    if (gTabCount == 0 || gTabCount > MAX_TABS || current_tab_index >= gTabCount) {
        if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            gTabCount = 1;
            current_tab_index = 0;
            memset(&gTabs, 0, sizeof(gTabs));
            strncpy(gTabs[0].name, "~system", sizeof(gTabs[0].name)-1);
            strncpy(gTabs[0].server, "Bouncer", sizeof(gTabs[0].server)-1);
            xSemaphoreGive(irc_mutex);
        }
        canvas.fillSprite(0x0000);
        canvas.setTextColor(0xFD20);
        canvas.setCursor(10, 45);
        canvas.print("SYSTEM BUFFER ALIGNING...");
        canvas.pushSprite(0, 12);
        ui_needs_redraw = true;
        return;
    }

    // 3. GENERATE MIDDLE GRAPHICS CANVAS VIEWPORT (Y=12 TO Y=121)
    canvas.fillSprite(0x0000);
    
    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        Tab &t = gTabs[current_tab_index];
        int current_y = 2;
        
        for (int i = 0; i < t.line_count; i++) {
            if (current_y + 11 > 109) break; // Strict clipping shield limit
            
            // Alternating Charcoal background strip colors row-by-row
            uint16_t row_bg = (i % 2 == 0) ? 0x0000 : 0x0841;
            canvas.fillRect(0, current_y - 2, 240, 12, row_bg);
            
            canvas.setTextColor(0x7BEF); // Slate grey timestamps
            canvas.setCursor(2, current_y);
            canvas.print(t.lines[i].timeStr);
            
            canvas.drawFastVLine(64, 0, 109, 0x7BEF); // Mid-contrast slate divider line
            
            // Alternating Charcoal background handled above - now multi-color nicknames & reverse highlights
            if (t.lines[i].is_highlight) {
                // High-contrast reverse-highlight: White text nested over solid Orange 0xFD20 background rectangle
                canvas.fillRect(68, current_y - 1, 50, 11, 0xFD20);
                canvas.setTextColor(0xFFFF);
            } else {
                canvas.setTextColor(get_nick_palette_color(t.lines[i].nick));
            }
            canvas.setCursor(68, current_y);
            if (current_tab_index == 0) {
                // Mentions mode layout: Print prefix without brackets formatting, force text column to X=160
                canvas.printf("%s", t.lines[i].nick);
                canvas.setTextColor(t.lines[i].color);
                canvas.setCursor(160, current_y); // <-- Slide text out to prevent multi-network label overlap
            } else {
                // Standard mode layout: Print standard bracketed room nickname, leave text column at X=120
                canvas.printf("<%s>", t.lines[i].nick);
                canvas.setTextColor(t.lines[i].color);
                canvas.setCursor(120, current_y);
            }
            canvas.print(t.lines[i].message); // Core text string payload - full-bleed across complete 240x109 horizontal canvas bounds, zero placeholders
            
            current_y += 12;
        }
        xSemaphoreGive(irc_mutex);
    }
    
    // 4. HARDWARE DISPLAY GLASS DIRECT REFRESH RENDER OVERLAYS
    canvas.pushSprite(0, 12);
    
    // Draw background header block bar safely across the full width
    M5Cardputer.Display.fillRect(0, 0, 240, 12, 0x0841);
    
    // Draw Server tag context anchor in low-profile slate grey
    M5Cardputer.Display.setTextColor(0x7BEF, 0x0841);
    M5Cardputer.Display.setCursor(2, 2);
    M5Cardputer.Display.printf("[%s]", gTabs[current_tab_index].server);
    
    // Draw Channel text title space in crisp high-visibility White
    M5Cardputer.Display.setTextColor(0xFFFF, 0x0841);
    M5Cardputer.Display.setCursor(54, 2); // Perfectly positioned
    
    // Safety Truncation: Copy active room name string up to a strict limit of 10 characters 
    // to physically block it from ever expanding into the right-side metrics space
    char truncated_name[12] = {0};
    strncpy(truncated_name, gTabs[current_tab_index].name, 10);
    M5Cardputer.Display.print(truncated_name);
    
    // ==========================================
    // 📡 THE COMPACT TELEMETRY GRID LAYER (X=140 TO X=238)
    // ==========================================
    
    // Anchor A: Wireless Network Radio State (X=142)
    M5Cardputer.Display.setCursor(142, 2);
    if (WiFi.status() != WL_CONNECTED) {
        M5Cardputer.Display.setTextColor(0xF800, 0x0841); // Warning Red
        M5Cardputer.Display.print("D"); // Disconnected Micro-Indicator
    } else {
        M5Cardputer.Display.setTextColor(0x07E0, 0x0841); // Healthy Green
        M5Cardputer.Display.print("W"); // Connected Wi-Fi Micro-Indicator
    }
    
    M5Cardputer.Display.setCursor(158, 2);
    if (current_audio == 0) {
        M5Cardputer.Display.setTextColor(0xF800, 0x0841); // Warning Red
        M5Cardputer.Display.print("H"); // Stealth Hidden Mode
    } else {
        M5Cardputer.Display.setTextColor(0x07E0, 0x0841); // Healthy Green
        M5Cardputer.Display.print("V"); // Standard Visible Mode
    }
    
    // Anchor C: Local System Digital Clock (X=176)
    M5Cardputer.Display.setTextColor(0xFFFF, 0x0841); // Pure White Text
    M5Cardputer.Display.setCursor(176, 2);
    // Dynamic system wall-clock ticker string builder pass
    // Reads current operational uptime to simulate active terminal time parameters
    uint32_t total_sec = millis() / 1000;
    uint32_t active_min = (total_sec / 60) % 60;
    uint32_t active_hr = ((total_sec / 3600) + current_tz_idx) % (use_12_hour_format ? 12 : 24);
    if (use_12_hour_format && active_hr == 0) active_hr = 12;
    M5Cardputer.Display.printf("%02d:%02d", active_hr, active_min);
    
    // Anchor D: Hardware Calibrated Battery Percentage (X=212)
    M5Cardputer.Display.setTextColor(0xFFFF, 0x0841);
    M5Cardputer.Display.setCursor(212, 2);
    int active_bat = (int)get_calibrated_battery_percentage();
    M5Cardputer.Display.printf("%d%%", active_bat); // Drops brackets to save 12px of screen glass width
    
    // Draw flat black footer background bar (from Y=121 to screen edge Y=135)
    M5Cardputer.Display.fillRect(0, 121, 240, 14, 0x0000);
    M5Cardputer.Display.drawFastHLine(0, 121, 240, 0x7BEF); // Mid-contrast slate divider
    
    // Draw the bright amber/orange command prompt indicator tag
    M5Cardputer.Display.setTextColor(0xFD20, 0x0000); // Enforce clear Amber on Black back-color
    M5Cardputer.Display.setCursor(4, 124);            // Shift slightly out from the screen glass bezel wall
    M5Cardputer.Display.print("> ");
    
    // Print the live interactive user typing input string characters
    M5Cardputer.Display.setTextColor(0xFFFF, 0x0000); // Crisp White text
    M5Cardputer.Display.print(input_buffer.c_str());
    
    // Place a small rectangular background overlay color mask to serve as a clean text scroll fade boundary edge
    M5Cardputer.Display.fillRect(215, 122, 25, 13, 0x0000);
    
    // Draw the remaining numerical characters indicator budget using micro-fonts
    M5Cardputer.Display.setTextColor(0x7BEF); // Dimmed slate grey
    M5Cardputer.Display.setCursor(218, 124);
    M5Cardputer.Display.printf("%03d", 400 - input_buffer.length());
    
    ui_needs_redraw = false;
}

// ==========================================
// ⌨️ 56 INDIVIDUAL TACTILE MICRO-SWITCH INTERCEPTS
// ==========================================
void handle_keyboard_inputs() {
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
    // Triggers instantly on either (Alt + Backspace [0x08]) OR a single tap of the physical Esc key (0x1B)
    if ((is_alt && M5Cardputer.Keyboard.isKeyPressed(0x08)) || M5Cardputer.Keyboard.isKeyPressed(0x1B)) {
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
                }
                xSemaphoreGive(irc_mutex);
            }
        }
        current_app_mode = MODE_CHAT;
        input_buffer = ""; // Cleanly flush any stray menu layout characters out of RAM
        ui_needs_redraw = true;
        return;
    }

    // Layer C: Critical Hardware Intercept for Fn+Arrow Punctuation Codes
    // On the Cardputer layout, Fn+Arrows outputs direct character values:
    // Fn+Left = ';' | Fn+Right = '/' | Fn+Up = ',' | Fn+Down = '.'
    if (current_app_mode == MODE_CHAT) {
        if (is_fn && M5Cardputer.Keyboard.isKeyPressed(';')) { // Fn+Left Arrow shortcut intercepted
            if (gTabCount > 1) { current_tab_index = (current_tab_index - 1 + gTabCount) % gTabCount; ui_needs_redraw = true; }
            return;
        }
        if (is_fn && M5Cardputer.Keyboard.isKeyPressed('/')) { // Fn+Right Arrow shortcut intercepted
            if (gTabCount > 1) { current_tab_index = (current_tab_index + 1) % gTabCount; ui_needs_redraw = true; }
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
                            return;
                        }
                        channel_print_counter++;
                    }
                }
                return;
            }
        }
        // Layer F: One-Handed Menu Navigation Pad (Active ONLY inside Menu modes)
        if (current_app_mode != MODE_CHAT) {
            // 1. Vertical List Scrolling (Single-press Comma and Period)
            if (M5Cardputer.Keyboard.isKeyPressed(',')) { // Comma acts as UP
                if (menu_selection_idx > 0) { menu_selection_idx--; ui_needs_redraw = true; }
                return;
            }
            if (M5Cardputer.Keyboard.isKeyPressed('.')) { // Period acts as DOWN
                int max_limit = 3;
                if (current_app_mode == MODE_SETTINGS) max_limit = 5;
                if (current_app_mode == MODE_BOUNCER)  max_limit = 4;
                
                if (menu_selection_idx < max_limit) { menu_selection_idx++; ui_needs_redraw = true; }
                return;
            }

            // 2. Horizontal Value Toggling (Single-press Semicolon and Forward Slash)
            if (current_app_mode == MODE_SETTINGS && (M5Cardputer.Keyboard.isKeyPressed(';') || M5Cardputer.Keyboard.isKeyPressed('/'))) {
                bool forward = M5Cardputer.Keyboard.isKeyPressed('/');
                
                if (menu_selection_idx == 0) { // Row 1: Brightness Horizontal Scaler
                    if (forward) {
                        if (screen_brightness == 10) screen_brightness = 60;
                        else if (screen_brightness == 60) screen_brightness = 120;
                        else if (screen_brightness == 120) screen_brightness = 200;
                        else if (screen_brightness == 200) screen_brightness = 255;
                    } else {
                        if (screen_brightness == 255) screen_brightness = 200;
                        else if (screen_brightness == 200) screen_brightness = 120;
                        else if (screen_brightness == 120) screen_brightness = 60;
                        else if (screen_brightness == 60) screen_brightness = 10;
                    }
                    M5Cardputer.Display.setBrightness(screen_brightness);
                }
                else if (menu_selection_idx == 1) { // Row 2: Timezone Offset Adjuster
                    current_tz_idx += forward ? 1 : -1;
                    if (current_tz_idx > 14) current_tz_idx = -12;
                    if (current_tz_idx < -12) current_tz_idx = 14;
                }
                else if (menu_selection_idx == 2) { // Row 3: Toggle 12/24hr Display Format
                    use_12_hour_format = !use_12_hour_format;
                }
                else if (menu_selection_idx == 3) { // Row 4: Toggle Channel Logging
                    channel_log_enabled = !channel_log_enabled;
                }
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

    // Layer D: Standard Character Stream Buffer Appender
    if (current_app_mode == MODE_CHAT) {
        if (status.del && input_buffer.length() > 0) {
            input_buffer.remove(input_buffer.length() - 1);
            ui_needs_redraw = true; // <-- Force unlock redraw
        }
        for (auto c : status.word) {
            // Hard gate to block Fn punctuation bleeding artifacts into your string line
            if (is_fn && (c == ';' || c == '/' || c == ',' || c == '.' || c == 'p')) continue;
            if (input_buffer.length() < 400) {
                input_buffer += c;
                ui_needs_redraw = true; // <-- Force unlock redraw on every keystroke
            }
        }
        if (status.enter && input_buffer.length() > 0) {
            const char* active_net = gTabs[current_tab_index].server;
            for (int i = 0; i < discovered_network_count; i++) {
                if (strcmp(discovered_networks[i], active_net) == 0 && clients[i].connected()) {
                    clients[i].printf("PRIVMSG %s :%s\r\n", gTabs[current_tab_index].name, input_buffer.c_str());
                    add_message_to_buffer(irc_nick, input_buffer.c_str(), 0xFFFF);
                    break;
                }
            }
            input_buffer = "";
            ui_needs_redraw = true;
        }
    }
}

void irc_network_task(void* pvParameters) {
    static bool master_scan_complete = false;
    static WiFiClientSecure master_client;

    while (true) {
        yield(); 
        vTaskDelay(pdMS_TO_TICKS(20)); // Prevent core starvation
        if (safe_mode_active || bnc_port == 0 || WiFi.status() != WL_CONNECTED) continue;

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
                    while (start_pos < net_list.length() && discovered_network_count < MAX_NETWORKS) {
                        int comma_idx = net_list.indexOf(',', start_pos);
                        String net_name = (comma_idx == -1) ? net_list.substring(start_pos) : net_list.substring(start_pos, comma_idx);
                        net_name.trim();

                        if (net_name.length() > 0) {
                            // Store the discovered network cleanly in a blank RAM cell
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
            yield();
            WiFiClientSecure &net_client = clients[i];

            if (!net_client.connected()) {
                network_authenticated[i] = false;
                net_client.setInsecure();
                if (net_client.connect(bnc_host, bnc_port)) {
                    net_client.printf("PASS %s:%s\r\n", bnc_user, bnc_pass);
                    net_client.print("CAP REQ :server-time cap-notify away-notify account-notify extended-join\r\n");
                    net_client.printf("NICK %s\r\n", irc_nick);
                    
                    // Inline Route Injection: Appends the discovered network string on-the-fly
                    net_client.printf("USER %s/%s 0 * :M5 Cardputer-Adv Client\r\n", bnc_user, discovered_networks[i]);
                    net_client.print("CAP END\r\n");
                    network_authenticated[i] = true;
                }
            }

            if (net_client.connected() && net_client.available()) {
                String line = net_client.readStringUntil('\n');
                line.trim(); line.replace("\r", "");
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
                    continue;
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

                String target_channel = "~system";
                String source_nick = "server";
                String chat_msg = line;
                
                if (line.indexOf(" PRIVMSG ") != -1) {
                    int priv_idx = line.indexOf(" PRIVMSG ");
                    int msg_idx = line.indexOf(" :", priv_idx);
                    if (priv_idx != -1 && msg_idx != -1) {
                        target_channel = line.substring(priv_idx + 9, msg_idx);
                        target_channel.trim();
                        chat_msg = line.substring(msg_idx + 2);
                        int ex_idx = line.indexOf('!');
                        if (ex_idx != -1 && ex_idx < priv_idx) {
                            source_nick = line.substring(1, ex_idx);
                        }
                    }
                    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        int matched_tab_idx = -1;
                        for (int ti = 0; ti < gTabCount; ti++) {
                            if (strcmp(gTabs[ti].name, target_channel.c_str()) == 0 && 
                                strcmp(gTabs[ti].server, network_context.c_str()) == 0) {
                                matched_tab_idx = ti;
                                break;
                            }
                        }
                        if (matched_tab_idx == -1 && gTabCount < MAX_TABS) {
                            matched_tab_idx = gTabCount;
                            strncpy(gTabs[matched_tab_idx].name, target_channel.c_str(), sizeof(gTabs[matched_tab_idx].name)-1);
                            strncpy(gTabs[matched_tab_idx].server, network_context.c_str(), sizeof(gTabs[matched_tab_idx].server)-1);
                            gTabs[matched_tab_idx].line_count = 0;
                            gTabCount++;
                        }
                        xSemaphoreGive(irc_mutex);
                        ui_needs_redraw = true;
                        if (matched_tab_idx != -1) {
                            int saved_idx = current_tab_index;
                            current_tab_index = matched_tab_idx;
                            add_message_to_buffer(source_nick.c_str(), chat_msg.c_str(), 0xFFFF, parsed_time);
                            if (matched_tab_idx != saved_idx) ui_needs_redraw = true;
                            current_tab_index = saved_idx;
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
        
        if (current_audio == 0) {
            M5Cardputer.Display.setBrightness(1); // Keep clamped
            vTaskDelay(pdMS_TO_TICKS(5));
        } else if (millis() - last_input_time > 60000) {
            M5Cardputer.Display.setBrightness(10);
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            M5Cardputer.Display.setBrightness(screen_brightness);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        
        // Drive Stamp-S3A LED modes asynchronously without delay halts
        set_led_mode(safe_mode_active ? 0 : 1);
        
        if (ui_needs_redraw) {
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
    
    irc_mutex = xSemaphoreCreateMutex();
    sd_mutex = xSemaphoreCreateMutex();
    gLogQueue = xQueueCreate(20, sizeof(char) * 128);
    
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
    
    if (safe_mode_active) {
        Serial.println("*** Safe Mode: net tasks bypassed");
    } else {
        purge_old_logs();
        load_settings_from_sd(); // Dynamically parse configuration on normal boots
    }
    
    // 1. Initialize the physical ESP32 radio chip
    WiFi.disconnect(true); // Force wipe any old, hung wireless states
    vTaskDelay(pdMS_TO_TICKS(100));
    WiFi.mode(WIFI_STA);   // Set to standard Station mode
    
    // Only attempt connection if your SD card settings successfully filled the buffers
    if (strlen(wifi_ssid) > 0) {
        Serial.printf("[WIFI] Connecting to target SSID: %s\n", wifi_ssid);
        WiFi.begin((const char*)wifi_ssid, (const char*)wifi_pass);
        
        // Wait up to 10 seconds for a clean local IP assignment
        int timeout_counter = 0;
        while (WiFi.status() != WL_CONNECTED && timeout_counter < 20) {
            yield();
            vTaskDelay(pdMS_TO_TICKS(500));
            timeout_counter++;
            
            // Toggle your top-right Stamp-S3A LED to Mode 9 (Orange) to show active network tuning
            set_led_mode(9); 
        }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] Connected cleanly! Assigned IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[WIFI-WARN] Connection timed out. Booting straight to offline terminal.");
    }
    
    gTabCount = 1; 
    current_tab_index = 0;
    memset(&gTabs, 0, sizeof(gTabs));
    strncpy(gTabs[0].name, "~mentions", sizeof(gTabs[0].name)-1);
    strncpy(gTabs[0].server, "ClientCore", sizeof(gTabs[0].server)-1);
    
    if (irc_mutex) xSemaphoreGive(irc_mutex);
    
    // Memory allocation and initialization complete. Lower the barrier safely.
    system_booted = true; 
    
    xTaskCreatePinnedToCore(irc_network_task, "NetworkTask", 8192, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(custom_ui_loop_task, "CustomUITask", 16384, NULL, 1, NULL, 1);
}

void loop() {
    vTaskDelete(NULL); // Force terminate default loop task to free up loop system registers entirely
}
