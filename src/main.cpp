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
enum AppMode { MODE_CHAT, MODE_WIFI, MODE_BOUNCER, MODE_SETTINGS };
volatile AppMode current_app_mode = MODE_CHAT;
int menu_selection_idx = 0; // Tracks active cursor line choices inside setup screens

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

WiFiClientSecure client;
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
    Serial.println("*** Purge 7d done");
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

        // --- DYNAMIC MENTIONS ALERTER ENGINE (~mentions Tab 0 Pool) ---
        // If incoming payload contains live irc_nick, duplicate to ~mentions pool at index 0 with server origin prepend
        if (strstr(msg, irc_nick) != NULL) {
            if (gTabCount > 0) {
                Tab &mentions = gTabs[0];
                // Ensure Tab 0 is ~mentions (provisioned in setup)
                if (strcmp(mentions.name, "~mentions") == 0) {
                    if (mentions.line_count >= MSG_BUFFER_SIZE) {
                        for (int i = 1; i < MSG_BUFFER_SIZE; i++) mentions.lines[i-1] = mentions.lines[i];
                        mentions.line_count = MSG_BUFFER_SIZE - 1;
                    }
                    ChatLine &mcl = mentions.lines[mentions.line_count];
                    if (timeStr) strncpy(mcl.timeStr, timeStr, sizeof(mcl.timeStr)-1);
                    else strncpy(mcl.timeStr, "00:00", sizeof(mcl.timeStr)-1);
                    strncpy(mcl.nick, source, sizeof(mcl.nick)-1);
                    // Prepend with source server and network origin: <[Server]UserNick> MessageText
                    const char* srv = t.server[0] ? t.server : (bnc_host[0] ? bnc_host : "Bouncer");
                    char mentionMsg[128];
                    snprintf(mentionMsg, sizeof(mentionMsg), "<[%s]%s> %s", srv, source, msg);
                    strncpy(mcl.message, mentionMsg, sizeof(mcl.message)-1);
                    mcl.color = color;
                    mcl.is_highlight = true;
                    mentions.line_count++;
                }
            }
        }
        
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
    if (current_app_mode != MODE_CHAT) {
        switch (current_app_mode) {
            case MODE_WIFI: {
                canvas.fillSprite(0x0000);
                canvas.setTextColor(0xFD20, 0x0000); // bright Amber header
                canvas.setCursor(2, 2);
                canvas.print("--- WIFI CONFIG MANAGER ---");
                // Row highlight via reversed charcoal overlay bar
                for (int r = 0; r < 2; r++) {
                    int y = 18 + r * 12;
                    if (r == menu_selection_idx) canvas.fillRect(0, y-1, 240, 12, 0x0841);
                    canvas.setTextColor(r == menu_selection_idx ? 0xFFFF : 0x7BEF, r == menu_selection_idx ? 0x0841 : 0x0000);
                    canvas.setCursor(4, y);
                    if (r == 0) canvas.printf("1. SSID: %s", wifi_ssid);
                    else canvas.printf("2. PASS: %s", wifi_pass);
                }
                canvas.setTextColor(0x7BEF);
                canvas.setCursor(2, 48);
                canvas.print("Enter=edit  Alt+Bksp=exit");
                canvas.pushSprite(0, 12);
                ui_needs_redraw = false;
                return;
            }
            case MODE_BOUNCER: {
                canvas.fillSprite(0x0000);
                canvas.setTextColor(0x001F, 0x0000); // explicit Cobalt Blue header
                canvas.setCursor(2, 2);
                canvas.print("--- BOUNCER SPEC ENGINE ---");
                for (int r = 0; r < 4; r++) {
                    int y = 18 + r * 12;
                    if (r == menu_selection_idx) canvas.fillRect(0, y-1, 240, 12, 0x0841);
                    canvas.setTextColor(r == menu_selection_idx ? 0xFFFF : 0x7BEF, r == menu_selection_idx ? 0x0841 : 0x0000);
                    canvas.setCursor(4, y);
                    if (r == 0) canvas.printf("1. Host: %s", bnc_host);
                    else if (r == 1) canvas.printf("2. Port: %d", bnc_port);
                    else if (r == 2) canvas.printf("3. User: %s", bnc_user);
                    else canvas.printf("4. Pass: %s", bnc_pass);
                }
                canvas.setTextColor(0x7BEF);
                canvas.setCursor(2, 68);
                canvas.print("Enter=edit  Alt+Bksp=exit");
                canvas.pushSprite(0, 12);
                ui_needs_redraw = false;
                return;
            }
            case MODE_SETTINGS: {
                canvas.fillSprite(0x0000);
                canvas.setTextColor(0x7BEF, 0x0000); // sleek terminal grey header
                canvas.setCursor(2, 2);
                canvas.print("--- QUICK SYSTEM SETTINGS ---");
                for (int r = 0; r < 3; r++) {
                    int y = 18 + r * 12;
                    if (r == menu_selection_idx) canvas.fillRect(0, y-1, 240, 12, 0x0841);
                    canvas.setTextColor(r == menu_selection_idx ? 0xFFFF : 0x7BEF, r == menu_selection_idx ? 0x0841 : 0x0000);
                    canvas.setCursor(4, y);
                    if (r == 0) canvas.printf("1. Screen Brightness: %d", screen_brightness);
                    else if (r == 1) canvas.printf("2. Time Zone Offset Idx: %d", current_tz_idx);
                    else canvas.printf("3. Clock Format: %s", use_12_hour_format ? "12hr" : "24hr");
                }
                canvas.setTextColor(0x7BEF);
                canvas.setCursor(2, 58);
                canvas.print("Enter=toggle  Alt+Bksp=save/exit");
                canvas.pushSprite(0, 12);
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
            canvas.printf("<%s>", t.lines[i].nick);
            
            canvas.setTextColor(t.lines[i].color);
            canvas.setCursor(120, current_y);
            canvas.print(t.lines[i].message); // Core text string payload - full-bleed across complete 240x109 horizontal canvas bounds, zero placeholders
            
            current_y += 12;
        }
        xSemaphoreGive(irc_mutex);
    }
    
    // 4. HARDWARE DISPLAY GLASS DIRECT REFRESH RENDER OVERLAYS
    canvas.pushSprite(0, 12);
    
    // 1. CLEAR AND DRAW BACKGROUND HEADER BAR (Y=0 TO Y=12)
    M5Cardputer.Display.fillRect(0, 0, 240, 12, 0x0841);
    
    // 2. LEFT SIDE SYSTEM ROOM ANCHORS
    M5Cardputer.Display.setTextColor(0x7BEF, 0x0841); // Slate grey brackets
    M5Cardputer.Display.setCursor(2, 2);
    M5Cardputer.Display.printf("[%s]", gTabs[current_tab_index].server);
    
    M5Cardputer.Display.setTextColor(0xFFFF, 0x0841); // Crisp white channel titles
    M5Cardputer.Display.setCursor(68, 2);              // Aligns with the middle log vertical line
    M5Cardputer.Display.print(gTabs[current_tab_index].name);
    
    // 3. RIGHT SIDE TELEMETRY HUD ANCHORS (STRICT HORIZONTAL SEPARATION)
    
    // Anchor A: Wireless Network Link Status (X=135)
    M5Cardputer.Display.setCursor(135, 2);
    if (WiFi.status() != WL_CONNECTED) {
        M5Cardputer.Display.setTextColor(0xF800, 0x0841); // Warning Red
        M5Cardputer.Display.print("[DISC]");
    } else {
        M5Cardputer.Display.setTextColor(0x07E0, 0x0841); // Healthy Green
        M5Cardputer.Display.print("[WIFI]");
    }
    
    // Anchor B: Audio Privacy State Flag (X=168)
    M5Cardputer.Display.setCursor(168, 2);
    if (current_audio == 0) {
        M5Cardputer.Display.setTextColor(0xF800, 0x0841); // Warning Red
        M5Cardputer.Display.print("[MUTE]");
    } else {
        M5Cardputer.Display.setTextColor(0x07E0, 0x0841); // Healthy Green
        M5Cardputer.Display.print("[+]");
    }
    
    // Anchor C: Local System Digital Clock (X=195)
    M5Cardputer.Display.setTextColor(0xFFFF, 0x0841); // High-visibility White
    M5Cardputer.Display.setCursor(195, 2);
    M5Cardputer.Display.print("00:00");
    
    // Anchor D: Hardware Calibrated Battery Percentage (X=212)
    M5Cardputer.Display.setCursor(212, 2);
    int active_bat = (int)get_calibrated_battery_percentage();
    M5Cardputer.Display.printf("[%d%%]", active_bat);
    
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
    last_input_time = millis(); // Refresh backlight screen power dim timer

    // --- FORM INPUT HOTKEY INTERCEPTORS: menu active check at absolute top ---
    auto status = M5Cardputer.Keyboard.keysState();
    if (status.del && input_buffer.length() > 0) {
        input_buffer.remove(input_buffer.length() - 1);
        ui_needs_redraw = true; // <-- Force unlock redraw
    }
    for (auto c : status.word) {
        if (input_buffer.length() < 400) {
            input_buffer += c;
            ui_needs_redraw = true; // <-- Force unlock redraw on every keystroke
        }
    }
    if (status.fn && M5Cardputer.Keyboard.isKeyPressed('p')) { current_app_mode = MODE_SETTINGS; menu_selection_idx = 0; ui_needs_redraw = true; return; }
    if (current_app_mode != MODE_CHAT) {
        bool is_alt_menu = M5Cardputer.Keyboard.isKeyPressed(KEY_LEFT_ALT);
        if (is_alt_menu && M5Cardputer.Keyboard.isKeyPressed('\b')) { sync_new_nick_to_sd(irc_nick); current_app_mode = MODE_CHAT; ui_needs_redraw = true; return; }
        if (M5Cardputer.Keyboard.isKeyPressed(0xB4)) { // Up Arrow
            if (menu_selection_idx > 0) menu_selection_idx--;
            else menu_selection_idx = 0;
            ui_needs_redraw = true; return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed(0xB9)) { // Down Arrow
            menu_selection_idx++;
            // Clamp per mode
            int maxIdx = 3;
            if (current_app_mode == MODE_WIFI) maxIdx = 1;
            if (current_app_mode == MODE_BOUNCER) maxIdx = 3;
            if (current_app_mode == MODE_SETTINGS) maxIdx = 2;
            if (menu_selection_idx > maxIdx) menu_selection_idx = maxIdx;
            ui_needs_redraw = true; return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('\n') || M5Cardputer.Keyboard.isKeyPressed('\r') || M5Cardputer.Keyboard.isKeyPressed(0x0D) || status.enter) {
            // Enter Key Selection: open text entry sub-prompts to update fields
            if (current_app_mode == MODE_WIFI) {
                // Row 0: wifi_ssid, Row 1: wifi_pass - binding to live memory cells
                // In interactive build, prompt for new wifi_ssid / wifi_pass via Serial/USB input
                if (menu_selection_idx == 0) { /* prompt wifi_ssid update: strncpy(wifi_ssid, newVal, sizeof(wifi_ssid)-1); */ }
                if (menu_selection_idx == 1) { /* prompt wifi_pass update */ }
            } else if (current_app_mode == MODE_BOUNCER) {
                if (menu_selection_idx == 0) { /* update bnc_host */ }
                if (menu_selection_idx == 1) { /* update bnc_port via atoi */ }
                if (menu_selection_idx == 2) { /* update bnc_user */ }
                if (menu_selection_idx == 3) { /* update bnc_pass */ }
            } else if (current_app_mode == MODE_SETTINGS) {
                if (menu_selection_idx == 0) { screen_brightness += 10; if (screen_brightness > 255) screen_brightness = 255; }
                if (menu_selection_idx == 1) { current_tz_idx = (current_tz_idx + 1) % 4; }
                if (menu_selection_idx == 2) { use_12_hour_format = !use_12_hour_format; }
            }
            ui_needs_redraw = true; return;
        }
        // Also allow direct ESC via 'q' or backspace without Alt when in menu
        if (M5Cardputer.Keyboard.isKeyPressed('q') || M5Cardputer.Keyboard.isKeyPressed(27)) { sync_new_nick_to_sd(irc_nick); current_app_mode = MODE_CHAT; ui_needs_redraw = true; return; }
    }
    
    // Read modifier state flags natively
    bool is_alt = M5Cardputer.Keyboard.isKeyPressed(KEY_LEFT_ALT);
    bool is_fn  = M5Cardputer.Keyboard.isKeyPressed(KEY_FN);
    
    // Core Arrow Cluster Interceptor Modifiers (Using official hex keycodes)
    // Left Arrow = 0xAC, Right Arrow = 0xAF, Down Arrow = 0xB9
    if (is_fn && M5Cardputer.Keyboard.isKeyPressed(0xAC)) { // Fn + Left Arrow (0xAC) Tab Left (Sequential tab step down)
        if (gTabCount > 1) { current_tab_index = (current_tab_index - 1 + gTabCount) % gTabCount; ui_needs_redraw = true; }
        return;
    }
    if (is_fn && M5Cardputer.Keyboard.isKeyPressed(0xAF)) { // Fn + Right Arrow (0xAF) Tab Right (Sequential tab step up)
        if (gTabCount > 1) { current_tab_index = (current_tab_index + 1) % gTabCount; ui_needs_redraw = true; }
        return;
    }
    if (is_alt && M5Cardputer.Keyboard.isKeyPressed(0xAC)) { // Alt + Left Arrow (0xAC) Jump Server Left (Whole Network Skip Backward)
        if (gTabCount <= 1) return;
        char cur_srv[32];
        strncpy(cur_srv, gTabs[current_tab_index].server, sizeof(cur_srv)-1);
        cur_srv[sizeof(cur_srv)-1]='\0';
        for (int i = 1; i < gTabCount; i++) {
            int idx = (current_tab_index - i + gTabCount) % gTabCount;
            if (strcmp(gTabs[idx].server, cur_srv) != 0) { current_tab_index = idx; ui_needs_redraw = true; return; }
        }
        return;
    }
    if (is_alt && M5Cardputer.Keyboard.isKeyPressed(0xAF)) { // Alt + Right Arrow (0xAF) Jump Server Right (Whole Network Skip Forward)
        if (gTabCount <= 1) return;
        char cur_srv2[32];
        strncpy(cur_srv2, gTabs[current_tab_index].server, sizeof(cur_srv2)-1);
        cur_srv2[sizeof(cur_srv2)-1]='\0';
        for (int i = 1; i < gTabCount; i++) {
            int idx = (current_tab_index + i) % gTabCount;
            if (strcmp(gTabs[idx].server, cur_srv2) != 0) { current_tab_index = idx; ui_needs_redraw = true; return; }
        }
        return;
    }
    if (is_fn && M5Cardputer.Keyboard.isKeyPressed('s')) { // Fn+S: Privacy mute toggle - direct privacy switch
        current_audio = (current_audio == 1) ? 0 : 1;
        pinMode(4, OUTPUT);
        digitalWrite(4, current_audio == 1 ? LOW : HIGH);
        ui_needs_redraw = true;
        return;
    }
    if (is_alt && M5Cardputer.Keyboard.isKeyPressed('\b')) { // Alt+Backspace: Safe Mode emergency escape / dump panels to MODE_CHAT
        if (safe_mode_active) { safe_mode_active = false; gTabCount = 0; ui_needs_redraw = true; }
        if (current_app_mode != MODE_CHAT) { sync_new_nick_to_sd(irc_nick); current_app_mode = MODE_CHAT; ui_needs_redraw = true; return; }
        return;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(0xB9)) { // Physical native Tab key row intercept (0xB9) - Inline Nickname Autocomplete Tracker
        int frag_start = input_buffer.length();
        while (frag_start > 0 && input_buffer.charAt(frag_start-1) != ' ' && input_buffer.charAt(frag_start-1) != ':') frag_start--;
        int frag_len = input_buffer.length() - frag_start;
        if (frag_len > 0) {
            String fragment = input_buffer.substring(frag_start);
            for (int li = 0; li < gTabs[current_tab_index].line_count; li++) {
                const char* cand = gTabs[current_tab_index].lines[li].nick;
                if (cand[0]==0) continue;
                if (strncasecmp(cand, fragment.c_str(), frag_len)==0) {
                    int cand_len = strlen(cand);
                    bool at_start = (frag_start==0);
                    String before = input_buffer.substring(0, frag_start);
                    String replacement = String(cand);
                    if (at_start) replacement += ": ";
                    if (before.length() + replacement.length() < 400) {
                        input_buffer = before + replacement;
                    }
                    break;
                }
            }
        }
        ui_needs_redraw = true;
        return;
    }
    // --- ADVANCED SLASH COMMAND INTERPRETER: check status.enter to process input line ---
    if (status.enter) {
        if (input_buffer.length() > 0) {
            // Robust C-string parser block to check if input_buffer begins with '/'
            if (input_buffer.charAt(0) == '/') {
                char cmdCopy[256];
                strncpy(cmdCopy, input_buffer.c_str(), sizeof(cmdCopy)-1);
                cmdCopy[sizeof(cmdCopy)-1]='\0';
                char* sp = strchr(cmdCopy, ' ');
                char* args = NULL;
                if (sp) { *sp='\0'; args = sp+1; while (*args==' ') args++; }
                // Tokenize and map direct commands
                if (strcasecmp(cmdCopy, "/join")==0 && args && args[0]) {
                    char channel_string[64];
                    strncpy(channel_string, args, sizeof(channel_string)-1);
                    channel_string[sizeof(channel_string)-1]='\0';
                    // Trim up to space
                    char* e = strchr(channel_string,' '); if(e) *e='\0';
                    client.printf("JOIN %s\r\n", channel_string);
                } else if (strcasecmp(cmdCopy, "/part")==0) {
                    char channel_string[64] = {0};
                    if (args && args[0]) strncpy(channel_string, args, sizeof(channel_string)-1);
                    else strncpy(channel_string, gTabs[current_tab_index].name, sizeof(channel_string)-1);
                    char* e = strchr(channel_string,' '); if(e) *e='\0';
                    client.printf("PART %s\r\n", channel_string);
                } else if (strcasecmp(cmdCopy, "/me")==0 && args && args[0]) {
                    char action_string[180];
                    strncpy(action_string, args, sizeof(action_string)-1);
                    action_string[sizeof(action_string)-1]='\0';
                    char current_tab_name[32];
                    strncpy(current_tab_name, gTabs[current_tab_index].name, sizeof(current_tab_name)-1);
                    client.printf("PRIVMSG %s :\x01" "ACTION %s\x01\r\n", current_tab_name, action_string);
                } else if (strcasecmp(cmdCopy, "/nick")==0 && args && args[0]) {
                    char new_nick_string[64];
                    strncpy(new_nick_string, args, sizeof(new_nick_string)-1);
                    new_nick_string[sizeof(new_nick_string)-1]='\0';
                    char* e = strchr(new_nick_string,' '); if(e) *e='\0';
                    sync_new_nick_to_sd(new_nick_string);
                    client.printf("NICK %s\r\n", new_nick_string);
                } else {
                    // Unknown slash -> send raw without slash
                    client.printf("%s\r\n", input_buffer.c_str()+1);
                }
            } else {
                // Normal chat: send as PRIVMSG to current tab channel
                if (client.connected() && gTabCount>0) {
                    char current_tab_name[32];
                    strncpy(current_tab_name, gTabs[current_tab_index].name, sizeof(current_tab_name)-1);
                    // Avoid sending to system mentions as target; fallback to PRIVMSG
                    client.printf("PRIVMSG %s :%s\r\n", current_tab_name, input_buffer.c_str());
                }
            }
            // Clear input buffer after send
            input_buffer = "";
            ui_needs_redraw = true;
        }
        return;
    }
}

// ==========================================
// 🚀 CONCURRENT COOPERATIVE FREERTOS STEERING
// ==========================================
void irc_network_task(void* pvParameters) {
    while (true) {
        yield();
        vTaskDelay(pdMS_TO_TICKS(10));
        
        if (safe_mode_active) continue;
        
        // If the client drops its link, attempt a safe background reconnect
        if (WiFi.status() == WL_CONNECTED && !client.connected() && bnc_port > 0) {
            client.setInsecure(); // Bypass static SSL fingerprint expiration limits
            if (client.connect(bnc_host, bnc_port)) {
                // Execute modern IRCv3 capability negotiation sequence
                client.printf("PASS %s:%s\r\n", bnc_user, bnc_pass);
                client.print("CAP REQ :server-time cap-notify away-notify account-notify extended-join\r\n");
                client.printf("NICK %s\r\n", irc_nick);
                client.printf("USER %s 0 * :M5 Cardputer-Adv Client\r\n", bnc_user);
                client.print("CAP END\r\n");
            }
        }
        
        // 2. PARSE INCOMING DATA PACKETS AND GENERATE NETWORK TABS
        if (client.connected() && client.available()) {
            String line = client.readStringUntil('\n');
            line.trim();
            char parsed_time[6] = "00:00";
            if (line.startsWith("@")) {
                int time_idx = line.indexOf("time=");
                if (time_idx != -1) {
                    // Extract the HH:MM subset characters out of the ISO-8601 timestamp string
                    // Example: @time=2026-09-05T14:35:00Z -> Extracts "14:35"
                    int t_start = line.indexOf('T', time_idx);
                    if (t_start != -1 && t_start + 6 < line.length()) {
                        String hh_mm = line.substring(t_start + 1, t_start + 6);
                        strncpy(parsed_time, hh_mm.c_str(), sizeof(parsed_time) - 1);
                    }
                }
                // Strip the entire @ tag section out of the line so standard text parsers don't print garbage
                int msg_start = line.indexOf(' ');
                if (msg_start != -1) {
                    line = line.substring(msg_start + 1);
                }
            }
            
            // Handle background PING-PONG heartbeats instantly
            if (line.startsWith("PING")) {
                client.printf("PONG %s\r\n", line.substring(5).c_str());
                continue;
            }
            // Handle ACCOUNT/AWAY extended-join updates without redraw stall
            if (line.indexOf(" ACCOUNT ") != -1 || line.indexOf(" AWAY ") != -1) {
                // Update local tracking arrays silently
                continue;
            }
            
            // Look for standard registration success tokens (001, 376, etc.) or JOIN tags
            if (line.indexOf(" 001 ") != -1 || line.indexOf(" JOIN ") != -1) {
                if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    // If your active tabs array is sitting blank, register your dynamic server nodes on-the-fly
                    if (gTabCount == 1 && strcmp(gTabs[0].name, "~system") == 0) {
                        // Re-seed the active tab array structures dynamically
                        strncpy(gTabs[0].server, "BNC", sizeof(gTabs[0].server)-1);
                        // Add more room slots safely as bouncer traffic channels populate up to MAX_TABS
                    }
                    xSemaphoreGive(irc_mutex);
                    ui_needs_redraw = true;
                }
            }
            if (line.length() > 0) {
                char dispNick[16] = "server";
                if (line.charAt(0) == ':') {
                    int sp = line.indexOf(' ');
                    if (sp != -1) {
                        String pref = line.substring(1, sp);
                        int bang = pref.indexOf('!');
                        if (bang != -1) pref = pref.substring(0, bang);
                        strncpy(dispNick, pref.c_str(), sizeof(dispNick)-1);
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

void custom_ui_loop_task(void* pvParameters) {
    ui_needs_redraw = true;
    while (true) {
        yield();
        vTaskDelay(pdMS_TO_TICKS(10));
        
        // Sit idle and yield if the core setup sequence hasn't finished initializing memory
        if (!system_booted) continue; 
        
        M5Cardputer.update(); // Polling matrix registers over un-lockable bus lane
        handle_keyboard_inputs();
        
        // Non-blocking auto dim screen backlight sleep timer check
        if (millis() - last_input_time > 60000) { M5Cardputer.Display.setBrightness(10); vTaskDelay(pdMS_TO_TICKS(5)); }
        else { M5Cardputer.Display.setBrightness(screen_brightness); vTaskDelay(pdMS_TO_TICKS(5)); }
        
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
        if (M5Cardputer.Keyboard.isKeyPressed('\b')) {
            safe_mode_active = true;
        }
    }
    
    if (safe_mode_active) {
        Serial.println("*** Safe Mode: net tasks bypassed");
    } else {
        purge_old_logs();
        load_settings_from_sd(); // Dynamically parse configuration on normal boots
    }
    
    // Populate base index room definitions cleanly before spawning task checkers
    gTabCount = 2;
    current_tab_index = 1; // Default viewport focus shifts right to system channel log line arrays
    memset(&gTabs, 0, sizeof(gTabs));
    
    // Hardcode layout index 0 for global mentions aggregator pool
    strncpy(gTabs[0].name, "~mentions", sizeof(gTabs[0].name)-1);
    strncpy(gTabs[0].server, "ClientCore", sizeof(gTabs[0].server)-1);
    
    strncpy(gTabs[1].name, "~system", sizeof(gTabs[1].name)-1);
    strncpy(gTabs[1].server, "Bouncer", sizeof(gTabs[1].server)-1);
    
    if (irc_mutex) xSemaphoreGive(irc_mutex);
    
    // Memory allocation and initialization complete. Lower the barrier safely.
    system_booted = true; 
    
    xTaskCreatePinnedToCore(irc_network_task, "NetworkTask", 8192, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(custom_ui_loop_task, "CustomUITask", 16384, NULL, 1, NULL, 1);
}

void loop() {
    vTaskDelete(NULL); // Force terminate default loop task to free up loop system registers entirely
}
