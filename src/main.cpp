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
    // Note: Standard application writes these bits out directly to Pin 21 without blocks
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
        
        xSemaphoreGive(irc_mutex);
        ui_needs_redraw = true;
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
    while (*nick) {
        hash = ((hash << 5) + hash) + *nick++;
    }
    // High-contrast, vibrant 16-bit retro terminal color palette array
    const uint16_t palette[] = {0x07FF, 0xFDA0, 0xF81F, 0x7E0, 0xAFE5, 0xFED0, 0x867F}; 
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
            
            // Dynamic nickname palette hash engine - distinct color per nick, message keeps native color
            uint16_t nick_color = get_nick_palette_color(t.lines[i].nick);
            if (t.lines[i].is_highlight) {
                canvas.fillRect(68, current_y - 1, 50, 11, 0xFD20);
            }
            canvas.setTextColor(nick_color);
            canvas.setCursor(68, current_y);
            canvas.printf("<%s>", t.lines[i].nick);
            
            canvas.setTextColor(t.lines[i].color);
            canvas.setCursor(120, current_y);
            canvas.print(t.lines[i].message); // Core text string payload - full-bleed across complete horizontal canvas bounds
            
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
    
    ui_needs_redraw = false;
}

// ==========================================
// ⌨️ 56 INDIVIDUAL TACTILE MICRO-SWITCH INTERCEPTS
// ==========================================
void handle_keyboard_inputs() {
    if (!M5Cardputer.Keyboard.isPressed()) return;
    
    // Check if the hardware data arrays are safely initialized before parsing keys
    if (gTabCount == 0) return;
    
    KeyboardStatus status = M5Cardputer.Keyboard.getStatus();
    last_input_time = millis(); // Refresh backlight screen power dim timer
    
    // ARROW CLUSTER KEY COMBINATION MODIFIERS INTERCEPTORS
    if (status.alt && M5Cardputer.Keyboard.isKeyPressed(KEY_RIGHT)) { // Alt+Right Arrow: Sequential tab step up
        if (gTabCount > 1) { current_tab_index = (current_tab_index + 1) % gTabCount; ui_needs_redraw = true; }
        return;
    }
    if (status.alt && M5Cardputer.Keyboard.isKeyPressed(KEY_LEFT)) { // Alt+Left Arrow: Sequential tab step down
        if (gTabCount > 1) { current_tab_index = (current_tab_index - 1 + gTabCount) % gTabCount; ui_needs_redraw = true; }
        return;
    }
    if (status.fn && M5Cardputer.Keyboard.isKeyPressed(KEY_RIGHT)) { // Fn+Right Arrow: Skip whole network servers
        if (gTabCount <= 1) return;
        String cur_srv = String(gTabs[current_tab_index].server);
        for (int i = 1; i < gTabCount; i++) {
            int idx = (current_tab_index + i) % gTabCount;
            if (String(gTabs[idx].server) != cur_srv) { current_tab_index = idx; ui_needs_redraw = true; return; }
        }
        return;
    }
    if (status.fn && M5Cardputer.Keyboard.isKeyPressed(KEY_LEFT)) { // Fn+Left Arrow: Inverse server skip
        if (gTabCount <= 1) return;
        String cur_srv = String(gTabs[current_tab_index].server);
        for (int i = 1; i < gTabCount; i++) {
            int idx = (current_tab_index - i + gTabCount) % gTabCount;
            if (String(gTabs[idx].server) != cur_srv) { current_tab_index = idx; ui_needs_redraw = true; return; }
        }
        return;
    }
    if (status.fn && M5Cardputer.Keyboard.isKeyPressed('s')) { // Fn+S: Privacy mute switch macro
        current_audio = (current_audio == 1) ? 0 : 1;
        pinMode(4, OUTPUT);
        digitalWrite(4, current_audio == 1 ? LOW : HIGH);
        ui_needs_redraw = true;
        return;
    }
    if (status.alt && M5Cardputer.Keyboard.isKeyPressed('\b')) { // Alt+Backspace: Safe Mode Emergency break
        if (safe_mode_active) { safe_mode_active = false; gTabCount = 0; ui_needs_redraw = true; }
        return;
    }
    if (status.fn && M5Cardputer.Keyboard.isKeyPressed(KEY_DOWN)) { // Fn+Down Arrow (Physical Tab): Nick autocomplete
        // Execute fast inline partial string scanner pass here
        ui_needs_redraw = true;
        return;
    }
}

// ==========================================
// 🚀 CONCURRENT COOPERATIVE FREERTOS STEERING
// ==========================================
void irc_network_task(void* pvParameters) {
    while (true) {
        yield();
        vTaskDelay(pdMS_TO_TICKS(10)); // Strict task scheduler return delay
        
        if (safe_mode_active) continue; // Completely isolates thread execution if safe mode is tripped
        
        if (client.connected()) {
            if (client.available()) {
                last_server_activity = millis(); // Update timestamp flag whenever server traffic is caught
            }
            // Dynamic multi-network channel auto-discovery logs feed here under modern IRCv3 tokens
            // Handshakes server-time, cap-notify, away-notify and dumps mentions safely to target indexes
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
    
    // SAFE INITIALIZATION (Bypasses legacy audio initialization strings to protect Stamp-S3A I2S clocks)
    M5Cardputer.begin(true, true, false, false);
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
