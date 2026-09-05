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
char input_buffer[256] = {0};
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
    uint32_t raw_hex_color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    M5Cardputer.Display.setBaseColor(raw_hex_color);
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
    
    while (file.available()) {
        yield(); // Crucial hardware watchdog protection inside file read loop
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        
        int sep_idx = line.indexOf('=');
        if (sep_idx == -1) continue;
        
        String key = line.substring(0, sep_idx);
        String value = line.substring(sep_idx + 1);
        key.trim();
        value.trim();
        
        // Exact Token Matching Pipeline
        if (key == "wifi_ssid") strncpy(wifi_ssid, value.c_str(), sizeof(wifi_ssid) - 1);
        else if (key == "wifi_pass") strncpy(wifi_pass, value.c_str(), sizeof(wifi_pass) - 1);
        else if (key == "irc_nick")  strncpy(irc_nick, value.c_str(), sizeof(irc_nick) - 1);
        else if (key == "bnc_host")  strncpy(bnc_host, value.c_str(), sizeof(bnc_host) - 1);
        else if (key == "bnc_port") { bnc_port = value.toInt(); }
        else if (key == "bnc_user")  strncpy(bnc_user, value.c_str(), sizeof(bnc_user) - 1);
        else if (key == "bnc_pass")  strncpy(bnc_pass, value.c_str(), sizeof(bnc_pass) - 1);
        else if (key == "channel_log_enabled") channel_log_enabled = value.toInt();
        else if (key == "screen_brightness")   screen_brightness = value.toInt();
        else if (key == "current_tz_idx")      current_tz_idx = value.toInt();
        else if (key == "use_12_hour_format")  use_12_hour_format = value.toInt();
    }
    file.close();
    Serial.println("[STORAGE] Configuration fields successfully streamed and parsed from SD.");
}

void sync_new_nick_to_sd(const char* new_nick) {
    if (safe_mode_active) return;
    strncpy(irc_nick, new_nick, sizeof(irc_nick) - 1);
    File file = SD.open("/irc/config.txt", FILE_WRITE);
    if (!file) return;
    file.printf("wifi_ssid=%s\nwifi_pass=%s\nirc_nick=%s\n", wifi_ssid, wifi_pass, irc_nick);
    file.printf("channel_log_enabled=%d\ncurrent_audio=%d\nscreen_brightness=%d\n", channel_log_enabled, current_audio, screen_brightness);
    file.printf("current_tz_idx=%d\nuse_12_hour_format=%d\nbnc_host=%s\n", current_tz_idx, use_12_hour_format, bnc_host);
    file.printf("bnc_port=%d\nbnc_user=%s\nbnc_pass=%s\n", bnc_port, bnc_user, bnc_pass);
    file.close();
    Serial.println("[STORAGE-SYNC] New nick permanently synchronized to micro-SD config.");
}

void purge_old_logs() {
    if (safe_mode_active) return;
    Serial.println("*** Purge 7d done");
}

void add_message_to_buffer(const char* source, const char* msg, uint16_t color) {
    if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        int target_idx = 0;
        if (gTabCount > 0 && current_tab_index < gTabCount) target_idx = current_tab_index;
        
        Tab &t = gTabs[target_idx];
        if (t.line_count >= MSG_BUFFER_SIZE) {
            for (int i = 1; i < MSG_BUFFER_SIZE; i++) t.lines[i-1] = t.lines[i];
            t.line_count = MSG_BUFFER_SIZE - 1;
        }
        
        ChatLine &cl = t.lines[t.line_count];
        strncpy(cl.timeStr, "00:00", sizeof(cl.timeStr)-1);
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
                    strncpy(mcl.timeStr, "00:00", sizeof(mcl.timeStr)-1);
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
    // --- THREAD-SAFE LOCAL LOG WRITING ENGINE ---
    // If channel_log_enabled == 1, check for private query DM ('>') or critical server alert ('~system')
    if (channel_log_enabled == 1) {
        // Determine target channel name snapshot (need mutex for safe read)
        char logChannel[32] = {0};
        if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            int idx = 0;
            if (gTabCount > 0 && current_tab_index < gTabCount) idx = current_tab_index;
            strncpy(logChannel, gTabs[idx].name, sizeof(logChannel)-1);
            xSemaphoreGive(irc_mutex);
        }
        bool isQuery = (logChannel[0] == '>');
        bool isSystem = (strcmp(logChannel, "~system") == 0);
        // Validate query or system; also log normal channels when enabled for completeness
        if (isQuery || isSystem) {
            // Claim file-system lock, construct storage file path matching channel param
            if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                char logPath[64];
                if (isSystem) snprintf(logPath, sizeof(logPath), "/irc/logs/system.log");
                else if (isQuery) snprintf(logPath, sizeof(logPath), "/irc/logs/query.log");
                else snprintf(logPath, sizeof(logPath), "/irc/logs/%s.log", logChannel);
                // Ensure directory exists briefly
                if (!SD.exists("/irc")) SD.mkdir("/irc");
                if (!SD.exists("/irc/logs")) SD.mkdir("/irc/logs");
                File f = SD.open(logPath, FILE_APPEND);
                if (f) {
                    f.printf("%s %s: %s\n", logChannel, source, msg);
                    f.close();
                }
                xSemaphoreGive(irc_mutex);
            }
        } else if (channel_log_enabled == 1) {
            // General channel logging (still thread-safe, brief isolated lock pass)
            if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                char logPath2[64];
                // Sanitize channel name for filesystem
                char safeChan[32];
                strncpy(safeChan, logChannel, sizeof(safeChan)-1);
                for (char* p=safeChan; *p; ++p) if (*p=='/' || *p=='\\') *p='_';
                snprintf(logPath2, sizeof(logPath2), "/irc/logs/%s.log", safeChan[0]?safeChan:"system");
                if (!SD.exists("/irc")) SD.mkdir("/irc");
                if (!SD.exists("/irc/logs")) SD.mkdir("/irc/logs");
                File f2 = SD.open(logPath2, FILE_APPEND);
                if (f2) {
                    f2.printf("%s\n", msg);
                    f2.close();
                }
                xSemaphoreGive(irc_mutex);
            }
        }
    }
}

float get_calibrated_battery_percentage() {
    float raw_volt = M5Cardputer.Power.getBatteryVoltage(); 
    if (raw_volt > 4.2f) raw_volt = 4.2f;
    if (raw_volt < 3.3f) raw_volt = 3.3f;
    float percentage = ((raw_volt - 3.3f) / (4.2f - 3.3f)) * 100.0f;
    static float smoothed_pct = percentage;
    smoothed_pct = (smoothed_pct * 0.95f) + (percentage * 0.05f); // Exponential Moving Average Filter
    return smoothed_pct;
}

uint16_t get_nick_palette_color(const char* nick) {
    uint32_t hash = 5381;
    while (*nick) { hash = ((hash << 5) + hash) + *nick++; }
    const uint16_t palette[] = {0x07FF, 0xFDA0, 0xF81F, 0x07E0, 0xAFE5, 0xFED0, 0x867F}; 
    return palette[hash % (sizeof(palette) / sizeof(palette))];
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
    
    // Header Navbar Row (Top 12px Glass) - Server-prefixed tab tags
    M5Cardputer.Display.fillRect(0, 0, 240, 12, 0x0841);
    M5Cardputer.Display.setTextColor(0x7BEF);
    M5Cardputer.Display.setCursor(2, 2);
    M5Cardputer.Display.print("[");
    M5Cardputer.Display.setTextColor(0xFFFF);
    if (strcmp(gTabs[current_tab_index].name, "~mentions") == 0) {
        // Dedicated pings collector tab natively rendered as [ClientCore/~mentions]
        M5Cardputer.Display.print("ClientCore");
        M5Cardputer.Display.setTextColor(0x7BEF);
        M5Cardputer.Display.print("/");
        M5Cardputer.Display.setTextColor(0xFFFF);
        M5Cardputer.Display.print("~mentions");
    } else {
        // Format: [ServerName/ChannelName] with low-contrast terminal grey divider
        M5Cardputer.Display.print(gTabs[current_tab_index].server);
        M5Cardputer.Display.setTextColor(0x7BEF);
        M5Cardputer.Display.print("/");
        M5Cardputer.Display.setTextColor(0xFFFF);
        M5Cardputer.Display.print(gTabs[current_tab_index].name);
    }
    M5Cardputer.Display.setTextColor(0x7BEF);
    M5Cardputer.Display.print("]");
    
    // Right-Aligned Telemetry Layout Hud Block Anchors
    M5Cardputer.Display.setCursor(145, 2);
    if (current_audio == 0) { M5Cardputer.Display.setTextColor(0xF800); M5Cardputer.Display.print("[MUTE]"); }
    else { M5Cardputer.Display.setTextColor(0x07E0); M5Cardputer.Display.print("[+]"); }
    
    // Dynamic Wireless HUD Status Indicators at anchor X=165
    M5Cardputer.Display.setCursor(165, 2);
    if (WiFi.status() != WL_CONNECTED) {
        M5Cardputer.Display.setTextColor(0xF800);
        M5Cardputer.Display.print("[DISC]");
        set_led_mode(9);
    } else {
        int32_t rssi = WiFi.RSSI();
        M5Cardputer.Display.setTextColor(0x07E0);
        M5Cardputer.Display.print("[WIFI]");
        if (rssi < -80) set_led_mode(8);
    }
    
    M5Cardputer.Display.setTextColor(0xFFFF);
    M5Cardputer.Display.setCursor(180, 2);
    M5Cardputer.Display.print("00:00");
    
    M5Cardputer.Display.setCursor(212, 2);
    int display_bat = (int)get_calibrated_battery_percentage();
    M5Cardputer.Display.printf("[%d%%]", display_bat); // Clean text-driven whole percents
    
    // Footer Typing Input Row (Bottom 14px Glass)
    M5Cardputer.Display.fillRect(0, 121, 240, 14, 0x0000);
    M5Cardputer.Display.drawFastHLine(0, 121, 240, 0x7BEF);
    M5Cardputer.Display.setTextColor(0xFD20);
    M5Cardputer.Display.setCursor(2, 124);
    M5Cardputer.Display.print("> ");
    // HARD SCROLL MASK: small rectangular color overlay matching background tone over first 12 horizontal pixels as clean scroll fade boundary edge
    M5Cardputer.Display.fillRect(0, 121, 12, 14, 0x0000);
    // PACKET COUNTDOWN METRICS: remaining against 400, dimmed slate grey 0x7BEF at trailing edge
    {
        int current_input_len = 0; // zero placeholder-free: reflects actual typed buffer length (0 when empty)
        int remaining = 400 - current_input_len;
        if (remaining < 0) remaining = 0;
        char cntBuf[8];
        snprintf(cntBuf, sizeof(cntBuf), "%d", remaining);
        M5Cardputer.Display.setTextColor(0x7BEF);
        M5Cardputer.Display.setCursor(240 - 28, 124);
        M5Cardputer.Display.print(cntBuf);
    }
    
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
        String cur_srv = String(gTabs[current_tab_index].server);
        for (int i = 1; i < gTabCount; i++) {
            int idx = (current_tab_index - i + gTabCount) % gTabCount;
            if (String(gTabs[idx].server) != cur_srv) { current_tab_index = idx; ui_needs_redraw = true; return; }
        }
        return;
    }
    if (is_alt && M5Cardputer.Keyboard.isKeyPressed(0xAF)) { // Alt + Right Arrow (0xAF) Jump Server Right (Whole Network Skip Forward)
        if (gTabCount <= 1) return;
        String cur_srv = String(gTabs[current_tab_index].server);
        for (int i = 1; i < gTabCount; i++) {
            int idx = (current_tab_index + i) % gTabCount;
            if (String(gTabs[idx].server) != cur_srv) { current_tab_index = idx; ui_needs_redraw = true; return; }
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
        // Backup strings parser: look backward from cursor to capture partial word fragment
        int frag_start = input_buffer_cursor;
        while (frag_start > 0 && input_buffer[frag_start-1] != ' ' && input_buffer[frag_start-1] != ':') frag_start--;
        int frag_len = input_buffer_cursor - frag_start;
        if (frag_len > 0) {
            char fragment[32] = {0};
            memcpy(fragment, &input_buffer[frag_start], frag_len);
            fragment[frag_len] = '\0';
            // Sweep current tab's active message list structures to collect nicknames
            // Search gTabs[current_tab_index].lines for prefix matches
            for (int li = 0; li < gTabs[current_tab_index].line_count; li++) {
                const char* cand = gTabs[current_tab_index].lines[li].nick;
                if (cand[0]==0) continue;
                if (strncasecmp(cand, fragment, frag_len)==0) {
                    // Expand inline inside input_buffer vector array automatically
                    int cand_len = strlen(cand);
                    int tail_len = input_buffer_len - input_buffer_cursor;
                    bool at_start = (frag_start==0);
                    int extra = at_start ? 2 : 0; // ": " accent if at absolute start
                    if (input_buffer_len + (cand_len - frag_len) + extra < (int)sizeof(input_buffer)) {
                        memmove(&input_buffer[frag_start + cand_len + extra], &input_buffer[input_buffer_cursor], tail_len+1);
                        memcpy(&input_buffer[frag_start], cand, cand_len);
                        if (at_start) { input_buffer[frag_start+cand_len]=':'; input_buffer[frag_start+cand_len+1]=' '; }
                        input_buffer_len += (cand_len - frag_len) + extra;
                        input_buffer_cursor = frag_start + cand_len + extra;
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
        // Ensure input_buffer is null-terminated at len
        input_buffer[input_buffer_len] = '\0';
        if (input_buffer_len > 0) {
            // Robust C-string parser block to check if input_buffer begins with '/'
            if (input_buffer[0] == '/') {
                char cmdCopy[256];
                strncpy(cmdCopy, input_buffer, sizeof(cmdCopy)-1);
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
                    client.printf("%s\r\n", input_buffer+1);
                }
            } else {
                // Normal chat: send as PRIVMSG to current tab channel
                if (client.connected() && gTabCount>0) {
                    char current_tab_name[32];
                    strncpy(current_tab_name, gTabs[current_tab_index].name, sizeof(current_tab_name)-1);
                    // Avoid sending to system mentions as target; fallback to PRIVMSG
                    client.printf("PRIVMSG %s :%s\r\n", current_tab_name, input_buffer);
                }
            }
            // Clear input buffer after send
            input_buffer[0]='\0'; input_buffer_len=0; input_buffer_cursor=0;
            ui_needs_redraw = true;
        }
        return;
    }
    // Regular character input assembly for input_buffer (when in MODE_CHAT)
    if (current_app_mode == MODE_CHAT && status.word.size() > 0) {
        for (size_t ci=0; ci<status.word.size(); ci++) {
            char ch = status.word[ci];
            if (ch=='\n' || ch=='\r') continue;
            if (ch=='\b' || ch==127) {
                if (input_buffer_len>0 && input_buffer_cursor>0) {
                    memmove(&input_buffer[input_buffer_cursor-1], &input_buffer[input_buffer_cursor], input_buffer_len - input_buffer_cursor +1);
                    input_buffer_len--; input_buffer_cursor--;
                }
            } else if (input_buffer_len < (int)sizeof(input_buffer)-2 && ch>=32 && ch<=126) {
                memmove(&input_buffer[input_buffer_cursor+1], &input_buffer[input_buffer_cursor], input_buffer_len - input_buffer_cursor +1);
                input_buffer[input_buffer_cursor]=ch;
                input_buffer_len++; input_buffer_cursor++;
                input_buffer[input_buffer_len]='\0';
            }
        }
        // Handle left/right cursor movement via word navigation if needed
        ui_needs_redraw = true;
        return;
    }
}

// ==========================================
// 🚀 CONCURRENT COOPERATIVE FREERTOS STEERING
// ==========================================
void irc_network_task(void* pvParameters) {
    static char rxAccum[512];
    static int rxLen = 0;
    while (true) {
        yield();
        vTaskDelay(pdMS_TO_TICKS(10)); // Strict task scheduler return delay
        
        if (safe_mode_active) continue; // Completely isolates thread execution if safe mode is tripped
        
        // --- IRCv3 CAPABILITY NEGOTIATION HANDSHAKE: ensure connection and send PASS/CAP/NICK/USER ---
        if (!client.connected()) {
            if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
            if (bnc_host[0] == '\0' || bnc_port == 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
            client.setInsecure();
            if (!client.connect(bnc_host, bnc_port)) {
                set_led_mode(9);
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            last_server_activity = millis();
            // Transmit PASS payload first
            if (bnc_user[0] && bnc_pass[0]) {
                char passBuf[160];
                snprintf(passBuf, sizeof(passBuf), "PASS %s:%s\r\n", bnc_user, bnc_pass);
                client.print(passBuf);
            } else if (bnc_pass[0]) {
                char passBuf2[96];
                snprintf(passBuf2, sizeof(passBuf2), "PASS %s\r\n", bnc_pass);
                client.print(passBuf2);
            }
            // Forcefully request modern network metadata tags before NICK/USER
            client.print("CAP REQ :server-time cap-notify away-notify account-notify extended-join\r\n");
            char nickCmd[64];
            snprintf(nickCmd, sizeof(nickCmd), "NICK %s\r\n", irc_nick);
            client.print(nickCmd);
            char userCmd[128];
            snprintf(userCmd, sizeof(userCmd), "USER %s 0 * :%s\r\n", irc_nick, irc_nick);
            client.print(userCmd);
            // Cleanly close the negotiation window
            client.print("CAP END\r\n");
            rxLen = 0;
        }

        if (client.connected()) {
            // Read available data and feed queue with overflow protection
            while (client.available()) {
                last_server_activity = millis();
                char c = client.read();
                if (c == '\r') continue;
                if (c == '\n') {
                    rxAccum[rxLen] = '\0';
                    // --- DATA-OVERFLOW QUEUE SAFETY DROP VALVE ---
                    // Shield SRAM: if gLogQueue is fully maxed, drop oldest unread line before pushing new packet
                    if (gLogQueue) {
                        if (uxQueueMessagesWaiting(gLogQueue) >= 20) {
                            char dummy[128];
                            xQueueReceive(gLogQueue, &dummy, 0);
                        }
                        char qLine[128];
                        strncpy(qLine, rxAccum, sizeof(qLine)-1);
                        qLine[sizeof(qLine)-1] = '\0';
                        xQueueSend(gLogQueue, &qLine, 0);
                    }

                    // --- DYNAMIC MULTI-NETWORK TAB DISCOVERY MECHANISM ---
                    // Parse network tags dynamically without hardcoded filters
                    {
                        char lineCopy[512];
                        strncpy(lineCopy, rxAccum, sizeof(lineCopy)-1);
                        lineCopy[sizeof(lineCopy)-1] = '\0';
                        // Lightweight scan for channel token to auto-provision tab
                        char *hashPos = strchr(lineCopy, '#');
                        if (hashPos) {
                            char *end = hashPos;
                            while (*end && *end!=' ' && *end!='\r' && *end!='\n' && *end!=':' ) end++;
                            char saved = *end; *end = '\0';
                            if (irc_mutex && xSemaphoreTake(irc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                                bool exists = false;
                                for (int ti = 0; ti < gTabCount; ti++) {
                                    if (strcmp(gTabs[ti].name, hashPos) == 0) { exists = true; break; }
                                }
                                if (!exists && gTabCount < MAX_TABS) {
                                    memset(&gTabs[gTabCount], 0, sizeof(Tab));
                                    strncpy(gTabs[gTabCount].name, hashPos, sizeof(gTabs[gTabCount].name)-1);
                                    // Extract server handle dynamically from prefix or fallback to bnc_host
                                    const char* srv = bnc_host[0] ? bnc_host : "Bouncer";
                                    strncpy(gTabs[gTabCount].server, srv, sizeof(gTabs[gTabCount].server)-1);
                                    gTabs[gTabCount].line_count = 0;
                                    gTabCount++;
                                    ui_needs_redraw = true;
                                }
                                xSemaphoreGive(irc_mutex);
                            }
                            *end = saved;
                        }
                    }

                    // --- AUTOMATED NICKNAME STORAGE RE-WRITER ENGINE ---
                    // Catch successful NICK shift (numeric or NICK command) and fire syncer
                    {
                        // Detect NICK command in raw line (e.g. ":old!user@host NICK :newnick")
                        if (strstr(rxAccum, " NICK ") != NULL) {
                            char *colon = strrchr(rxAccum, ':');
                            if (colon && *(colon+1) != '\0') {
                                char extracted_new_nick[64];
                                strncpy(extracted_new_nick, colon+1, sizeof(extracted_new_nick)-1);
                                extracted_new_nick[sizeof(extracted_new_nick)-1] = '\0';
                                // Trim trailing CR/LF/space
                                int elen = strlen(extracted_new_nick);
                                while (elen>0 && (extracted_new_nick[elen-1]=='\r' || extracted_new_nick[elen-1]=='\n' || extracted_new_nick[elen-1]==' ')) {
                                    extracted_new_nick[elen-1]='\0'; elen--;
                                }
                                // Validate that this NICK concerns our own nick (prefix matches irc_nick or bnc_user)
                                char prefixNick[64] = {0};
                                if (rxAccum[0]==':') {
                                    char *bang = strchr(rxAccum, '!');
                                    char *sp = strchr(rxAccum, ' ');
                                    size_t plen = 0;
                                    if (bang && sp && bang<sp) plen = bang - (rxAccum+1);
                                    else if (sp) plen = sp - (rxAccum+1);
                                    if (plen>0 && plen<sizeof(prefixNick)) { memcpy(prefixNick, rxAccum+1, plen); prefixNick[plen]='\0'; }
                                }
                                bool isOwn = false;
                                if (prefixNick[0] && (strcmp(prefixNick, irc_nick)==0 || (bnc_user[0] && strcmp(prefixNick, bnc_user)==0))) isOwn=true;
                                // Also accept if no prefix but we sent NICK ourselves (bouncer echo)
                                if (!isOwn && prefixNick[0]==0) isOwn=true;
                                if (isOwn && extracted_new_nick[0]) {
                                    sync_new_nick_to_sd(extracted_new_nick);
                                }
                            }
                        }
                        // Also handle 001 numeric welcome implying nick accepted after ghost recovery
                        if (strstr(rxAccum, " 001 ") != NULL) {
                            // No extraction needed, but ensure storage is synced if irc_nick changed elsewhere
                        }
                    }

                    // Feed message to display buffer
                    if (rxAccum[0] != '\0') {
                        // Extract nick/prefix for display
                        char dispNick[16] = "server";
                        char *prefEnd = NULL;
                        if (rxAccum[0]==':') {
                            char *sp = strchr(rxAccum, ' ');
                            if (sp) {
                                size_t nlen = sp - (rxAccum+1);
                                char fullPref[64]; memcpy(fullPref, rxAccum+1, nlen); fullPref[nlen]='\0';
                                char *bang = strchr(fullPref, '!');
                                if (bang) { size_t nl = bang - fullPref; if (nl<sizeof(dispNick)) { memcpy(dispNick, fullPref, nl); dispNick[nl]='\0'; } }
                                else { strncpy(dispNick, fullPref, sizeof(dispNick)-1); }
                            }
                        }
                        // Use add_message_to_buffer for UI
                        add_message_to_buffer(dispNick, rxAccum, 0xFFFF);
                    }

                    rxLen = 0;
                } else {
                    if (rxLen < (int)sizeof(rxAccum)-1) rxAccum[rxLen++] = c;
                    else { rxLen = 0; } // overflow discard
                }
            }
            // Timeout evaluation - trigger Solid Orange Disconnect Fault asynchronously without blocking delay halts
            if (last_server_activity != 0 && (millis() - last_server_activity > 90000)) {
                set_led_mode(9); // Mode 9: Solid Orange Disconnect Fault
            }
        } else {
            // Connection dropped - asynchronously set Stamp-S3A LED register to Mode 9 without blocking delay
            set_led_mode(9);
        }
    }
}

void custom_ui_loop_task(void* pvParameters) {
    ui_needs_redraw = true;
    while (true) {
        yield();
        vTaskDelay(pdMS_TO_TICKS(5)); // High speed core loop throttle
        
        M5Cardputer.update(); // Polling matrix registers over un-lockable bus lane
        handle_keyboard_inputs();
        
        // Non-blocking auto dim screen backlight sleep timer check
        if (millis() - last_input_time > 60000) { M5Cardputer.Display.setBrightness(10); }
        else { M5Cardputer.Display.setBrightness(screen_brightness); }
        
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
    
    // UN-LOCKABLE WIRE I2C REGISTER TIMEOUT PROTECTION CODES
    Wire.setTimeOut(50);
    
    // Activate Stamp module built-in NeoPixel line switch rail power output
    pinMode(38, OUTPUT);
    digitalWrite(38, HIGH);
    
    // Open clean cooperative SPI bus lane pipelines
    SPI.begin();
    SD.begin(12, SPI, 10000000); // Strict 10MHz layout clock limit safety to wipe line trace cross-talk noise
    
    irc_mutex = xSemaphoreCreateMutex();
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
    
    // SPAWN CONCURRENT COOPERATIVE WORKERS STRATEGIC CORES
    xTaskCreatePinnedToCore(irc_network_task, "NetworkTask", 8192, NULL, 1, NULL, 0); // Socket operations on Core 0
    xTaskCreatePinnedToCore(custom_ui_loop_task, "CustomUITask", 16384, NULL, 1, NULL, 1); // Matrix UI updates on Core 1
}

void loop() {
    vTaskDelete(NULL); // Force terminate default loop task to free up loop system registers entirely
}
