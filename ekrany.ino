
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/semphr.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "stops_data.h"

#define TFT_RST_PIN 8

static constexpr int TFT_CS1_PIN = 5;
static constexpr int TFT_CS2_PIN = 17;

static constexpr uint8_t ROT_MAIN = 1;
static constexpr uint8_t ROT_ROUTE = 3;

TFT_eSPI tft = TFT_eSPI();
bool g_touchReady = false;
bool g_touchPressedLast = false;

inline void deselectDisplays() {
    digitalWrite(TFT_CS1_PIN, HIGH);
    digitalWrite(TFT_CS2_PIN, HIGH);
}

inline void selectDisplay(int csPin) {
    deselectDisplays();
    digitalWrite(csPin, LOW);
}

// Touch calibration values measured from the raw XPT2046 controller output.
static constexpr int16_t TOUCH_CAL_X_MIN = 300;
static constexpr int16_t TOUCH_CAL_X_MAX = 3800;
static constexpr int16_t TOUCH_CAL_Y_MIN = 300;
static constexpr int16_t TOUCH_CAL_Y_MAX = 3800;
static constexpr bool TOUCH_SWAP_XY = false;
static constexpr bool TOUCH_INVERT_X = true;
static constexpr bool TOUCH_INVERT_Y = true;

#define COLOR_BG      0xDF1E
#define COLOR_CARD    0xFFFF
#define COLOR_TEXT    0x0000
#define COLOR_RED     0xF800
#define COLOR_ORANGE  0xFD20
#define COLOR_ROUTE   0x7BEF
#define COLOR_BTN     0x7BEF

const char* API_BASE_URL = "https://api.zbiorkom.live/4.8/lublin/stops/getDepartures";
const char* ROUTE_API_BASE_URL = "https://api.zbiorkom.live/4.8/lublin/routes/getRoute";
const char* DEFAULT_STOP_ID = "brama-krakowska04";
const char* DEFAULT_STOP_LABEL = "BRAMA KRAKOWSKA 04";
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";
const int API_LIMIT = 10;
const int VISIBLE_DEPARTURES = 4;
const unsigned long API_REFRESH_MS = 60000UL;
const unsigned long ROUTE_FETCH_TIMEOUT_MS = 30000UL;
const int MAX_STOP_OPTIONS = 1400;
const int MAX_ROUTE_STOPS = 64;
const int ROUTE_CACHE_SIZE = 8;

const int BTN_SCROLL_UP_X = 126;
const int BTN_SCROLL_UP_Y = 210;
const int BTN_SCROLL_UP_W = 86;
const int BTN_SCROLL_UP_H = 25;

const int BTN_SCROLL_DOWN_X = 224;
const int BTN_SCROLL_DOWN_Y = 210;
const int BTN_SCROLL_DOWN_W = 86;
const int BTN_SCROLL_DOWN_H = 25;

const int BTN_STOP_PICKER_X = 270;
const int BTN_STOP_PICKER_Y = 6;
const int BTN_STOP_PICKER_W = 40;
const int BTN_STOP_PICKER_H = 24;

const int STOP_MODAL_X = 14;
const int STOP_MODAL_Y = 32;
const int STOP_MODAL_W = 292;
const int STOP_MODAL_H = 196;
const int STOP_MODAL_CLOSE_X = STOP_MODAL_X + STOP_MODAL_W - 26;
const int STOP_MODAL_CLOSE_Y = STOP_MODAL_Y + 4;
const int STOP_MODAL_CLOSE_W = 20;
const int STOP_MODAL_CLOSE_H = 18;
const int STOP_MODAL_LIST_X = STOP_MODAL_X + 10;
const int STOP_MODAL_LIST_Y = STOP_MODAL_Y + 30;
const int STOP_MODAL_LIST_W = 238;
const int STOP_MODAL_ROW_H = 24;
const int STOP_MODAL_VISIBLE_ROWS = 6;
const int STOP_MODAL_SCROLL_X = STOP_MODAL_X + STOP_MODAL_W - 34;
const int STOP_MODAL_SCROLL_UP_Y = STOP_MODAL_Y + 70;
const int STOP_MODAL_SCROLL_DOWN_Y = STOP_MODAL_Y + 158;
const int STOP_MODAL_SCROLL_W = 24;
const int STOP_MODAL_SCROLL_H = 24;

char g_stopFilterLetter = 0;

struct DepartureEntry {
    String line;
    String direction;
    int64_t departureEpochMs;
    int minutesToDeparture;
    bool valid;
};

struct RouteCacheEntry {
    bool valid;
    String line;
    String directionKey;
    int stopCount;
    uint32_t lastUsed;
    String stopIds[MAX_ROUTE_STOPS];
    String stopNames[MAX_ROUTE_STOPS];
};

struct RouteTaskRequest {
    String line;
    String direction;
    String activeStopId;
    uint32_t requestSeq;
};

DepartureEntry g_departures[API_LIMIT];
String g_activeStopId = DEFAULT_STOP_ID;
String g_activeStopLabel = DEFAULT_STOP_LABEL;
String g_stopIds[MAX_STOP_OPTIONS];
int g_stopCount = 0;
int g_stopMenuOffset = 0;
int g_activeStopIndex = -1;
bool g_stopModalOpen = false;
int g_scrollOffset = 0;
String g_lastUpdateText = "--:--:--";
String g_statusText = "Ladowanie danych...";
unsigned long g_lastApiFetchMs = 0;
unsigned long g_refreshStartedMs = 0;
SemaphoreHandle_t g_departuresMutex = nullptr;
TaskHandle_t g_refreshTaskHandle = nullptr;
volatile bool g_refreshInProgress = false;
volatile bool g_refreshRenderPending = false;

SemaphoreHandle_t g_routeMutex = nullptr;
TaskHandle_t g_routeTaskHandle = nullptr;
RouteCacheEntry g_routeCache[ROUTE_CACHE_SIZE];
String g_routeDisplayLine = "-";
String g_routeDisplayDirection = "";
String g_routeDisplayStops[MAX_ROUTE_STOPS];
int g_routeDisplayCount = 0;
String g_routeStatusText = "Wybierz kurs po lewej";
bool g_routeHasData = false;
volatile bool g_routeLoadInProgress = false;
volatile bool g_routeRenderPending = false;
unsigned long g_routeStartedMs = 0;
volatile uint32_t g_routeRequestSeq = 0;
uint32_t g_routeCacheUseCounter = 0;

void drawTimetable(TFT_eSPI& tft);
void drawTimetableData(TFT_eSPI& tft);
void drawStatusBar(TFT_eSPI& tft);
void drawScrollButtons(TFT_eSPI& tft);
void drawStopPickerButton(TFT_eSPI& tft);
void drawStopSelectionModal(TFT_eSPI& tft);
void drawDepartureCard(TFT_eSPI& tft, int index, const String& line, const String& dir, const String& timeStr, uint16_t timeColor);
void drawRouteScreen(TFT_eSPI& tft, const String& lineNumber, String stops[], int numStops, String currentTime, String currentDate);
void drawDynamicRoute(TFT_eSPI& tft, String stops[], int numStops, int startY, int stepY, int maxCols);
void drawRoutePlaceholder(TFT_eSPI& tft, const String& title, const String& message);
void renderRouteScreenFromState();
void initTouchScreen();
bool loadStopsFromEmbeddedJson();
void handleTouchInput();
bool readTouchScreenPoint(int16_t& x, int16_t& y);
bool isPointInRect(int16_t px, int16_t py, int16_t x, int16_t y, int16_t w, int16_t h);
void openStopSelectionModal();
void closeStopSelectionModal(bool fullRedraw = true);
void handleStopModalTouch(int16_t touchX, int16_t touchY);
void selectStopByIndex(int index);
String stopIdToLabel(const String& stopId);
int findStopIndexById(const String& stopId);
int findFirstStopIndexByLetter(char letter);
void changeStopFilterLetter(int delta);
int maxStopMenuOffset();
bool addStopIdIfUnique(const String& stopId);
void scrollDepartures(int delta);
int countValidDepartures();
int maxScrollOffset();
int countValidDeparturesUnlocked();
bool lockDepartures(TickType_t timeoutTicks = portMAX_DELAY);
void unlockDepartures();
bool refreshDeparturesData();
void refreshDeparturesTask(void*);
bool connectToWifi(uint32_t timeoutMs);
void initClock();
bool isClockSynced();
bool refreshDeparturesForTft1();
bool fetchDeparturesFromApi(int limit);
bool parseDeparturesPayload(const String& payload);
int64_t extractRealtimeDepartureMs(JsonArray timing);
void insertSortedDeparture(const String& line, const String& direction, int64_t departureMs, int minutesToDeparture);
void clearDepartures();
void requestRouteForDepartureIndex(int index);
void routeFetchTask(void*);
bool fetchAndPrepareRoute(const String& line, const String& requestedDirection, const String& activeStopId, String outStops[], int& outCount, String& outMatchedDirection, String& outStatusText);
String routeDirectionKey(const String& direction);
int directionMatchScore(const String& requestedKey, const String& candidateKey);
int findMatchingDirectionIndex(JsonArray variants, const String& requestedDirectionKey, String& outMatchedDirectionLabel, String& outMatchedDirectionKey);
bool extractStopsFromVariant(JsonArray variant, String stopIds[], String stopNames[], int& stopCount);
int findStopIndexInList(const String& stopId, String stopIds[], int stopCount);
void buildFilteredStopNames(const String& activeStopId, String stopIds[], String stopNames[], int stopCount, String outStops[], int& outCount);
bool routeCacheGet(const String& line, const String& directionKey, String stopIds[], String stopNames[], int& stopCount);
void routeCachePut(const String& line, const String& directionKey, String stopIds[], String stopNames[], int stopCount);
void getCurrentTimeDate(String& outTime, String& outDate);
String jsonToString(JsonVariant value);
int64_t jsonToInt64(JsonVariant value);
bool isNumericJson(JsonVariant value);
String normalizePolishText(const String& text);
String minuteLabel(int minutesToDeparture);
uint16_t departureColor(int minutesToDeparture);
bool lockRouteState(TickType_t timeoutTicks = portMAX_DELAY);
void unlockRouteState();

static const char* STOP_CONFIG_PATH = "/cfg_stop.txt";

static void saveStop(const String& stopId) {
    File f = LittleFS.open(STOP_CONFIG_PATH, "w");
    if (f) { f.print(stopId); f.close(); Serial.println("[CFG] Saved stop: " + stopId); }
    else   { Serial.println("[CFG] Failed to save stop."); }
}

// Load the previously selected stop after the embedded stop list and LittleFS are ready.
static void loadStop() {
    String saved = DEFAULT_STOP_ID;
    if (LittleFS.exists(STOP_CONFIG_PATH)) {
        File f = LittleFS.open(STOP_CONFIG_PATH, "r");
        if (f) { saved = f.readString(); f.close(); saved.trim(); }
    }
    int idx = findStopIndexById(saved);
    if (idx >= 0) {
        g_activeStopId    = saved;
        g_activeStopLabel = stopIdToLabel(saved);
        g_activeStopIndex = idx;
        Serial.println("[CFG] Restored stop: " + saved);
    } else {
        Serial.println("[CFG] Saved stop was not found, using the default.");
    }
}

// Track recently announced departures by line and rounded minute to avoid repeats.
struct AnnKey { char line[12]; int32_t epochMin; };
static const int ANN_CAP = 20;
static AnnKey    s_announced[ANN_CAP];
static int       s_announcedN = 0;

static bool isAnnounced(const String& line, int64_t ep) {
    int32_t key = (int32_t)(ep / 60000LL);
    for (int i = 0; i < s_announcedN; i++) {
        if (!line.equals(s_announced[i].line)) continue;
        // Realtime updates can drift by a few minutes between refreshes.
        int32_t diff = s_announced[i].epochMin - key;
        if (diff < 0) diff = -diff;
        if (diff <= 4) return true;
    }
    return false;
}

static void markAnnounced(const String& line, int64_t ep) {
    if (s_announcedN >= ANN_CAP) return;
    line.toCharArray(s_announced[s_announcedN].line, 12);
    s_announced[s_announcedN].epochMin = (int32_t)(ep / 60000LL);
    s_announcedN++;
}

static void pruneAnnounced() {
    if (!isClockSynced()) return;
    int32_t cutoff = (int32_t)((int64_t)time(nullptr) * 1000LL / 60000LL) - 10;
    int n = 0;
    for (int i = 0; i < s_announcedN; i++)
        if (s_announced[i].epochMin > cutoff) s_announced[n++] = s_announced[i];
    s_announcedN = n;
}

static void checkDepartureAnnouncements() {
    if (!lockDepartures(pdMS_TO_TICKS(100))) return;
    pruneAnnounced();
    for (int i = 0; i < API_LIMIT; i++) {
        if (!g_departures[i].valid) continue;
        int     mins = g_departures[i].minutesToDeparture;
        int64_t ep   = g_departures[i].departureEpochMs;
        if (mins > 0 && mins <= 5 && ep > 0 && !isAnnounced(g_departures[i].line, ep)) {
            Serial.printf("[Ann] Line %s in %d min\n",
                          g_departures[i].line.c_str(), mins);
            markAnnounced(g_departures[i].line, ep);
            audioEnqueue(g_departures[i].line);
        }
    }
    unlockDepartures();
}

void setup() {
    pinMode(TFT_CS1_PIN, OUTPUT); digitalWrite(TFT_CS1_PIN, HIGH);
    pinMode(TFT_CS2_PIN, OUTPUT); digitalWrite(TFT_CS2_PIN, HIGH);

    delay(1000);
    Serial.begin(115200);
    delay(200);

    Serial.println();
    Serial.println("=== Transit display boot (TFT_eSPI) ===");

    pinMode(TFT_CS1_PIN, OUTPUT);
    pinMode(TFT_CS2_PIN, OUTPUT);
    deselectDisplays();

    pinMode(TFT_RST_PIN, OUTPUT);
    digitalWrite(TFT_RST_PIN, LOW);
    delay(100);
    digitalWrite(TFT_RST_PIN, HIGH);
    delay(500);

    selectDisplay(TFT_CS1_PIN);
    tft.init();
    tft.setRotation(ROT_MAIN);

    selectDisplay(TFT_CS2_PIN);
    tft.init();
    tft.setRotation(ROT_ROUTE);

    selectDisplay(TFT_CS1_PIN);
    tft.setRotation(ROT_MAIN);
    initTouchScreen();

    selectDisplay(TFT_CS1_PIN);
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.drawString("EKRAN 1 (CS=5)", 20, 20, 4);
    delay(400);

    selectDisplay(TFT_CS2_PIN);
    tft.fillScreen(TFT_BLUE);
    tft.setTextColor(TFT_WHITE, TFT_BLUE);
    tft.drawString("EKRAN 2 (CS=17)", 20, 20, 4);
    delay(400);

    selectDisplay(TFT_CS1_PIN);

    g_departuresMutex = xSemaphoreCreateMutex();
    if (g_departuresMutex == nullptr) {
        Serial.println("Failed to create the departures mutex.");
    }

    g_routeMutex = xSemaphoreCreateMutex();
    if (g_routeMutex == nullptr) {
        Serial.println("Failed to create the route mutex.");
    }

    for (int i = 0; i < ROUTE_CACHE_SIZE; i++) {
        g_routeCache[i].valid = false;
        g_routeCache[i].line = "";
        g_routeCache[i].directionKey = "";
        g_routeCache[i].stopCount = 0;
        g_routeCache[i].lastUsed = 0;
    }

    if (!loadStopsFromEmbeddedJson()) {
        Serial.println("Failed to load the embedded stop list.");
    }

    audioSetup();
    loadStop();

    clearDepartures();

    drawTimetable(tft);
    renderRouteScreenFromState();

    connectToWifi(20000);
    drawTimetable(tft);
    renderRouteScreenFromState();
    initClock();
    refreshDeparturesForTft1();
}

void loop() {
    bool refreshDue = (g_lastApiFetchMs == 0 || millis() - g_lastApiFetchMs >= API_REFRESH_MS);
    if (refreshDue && !g_refreshInProgress) {
        g_refreshInProgress = true;
        g_refreshStartedMs = millis();
        if (lockDepartures(pdMS_TO_TICKS(200))) {
            g_statusText = "Aktualizacja...";
            unlockDepartures();
        }
        drawStatusBar(tft);
        if (g_stopModalOpen) {
            drawStopSelectionModal(tft);
        }

        BaseType_t taskOk = xTaskCreatePinnedToCore(
            refreshDeparturesTask,
            "apiRefresh",
            20480,
            nullptr,
            1,
            &g_refreshTaskHandle,
            1
        );

        if (taskOk != pdPASS) {
            g_refreshInProgress = false;
            g_lastApiFetchMs = millis();
            if (lockDepartures(pdMS_TO_TICKS(20))) {
                g_statusText = "Blad taska API";
                unlockDepartures();
            }
            drawStatusBar(tft);
            if (g_stopModalOpen) {
                drawStopSelectionModal(tft);
            }
        }
    }

    if (g_refreshInProgress && millis() - g_refreshStartedMs > 30000UL) {
        g_refreshInProgress = false;
        g_refreshTaskHandle = nullptr;
        g_lastApiFetchMs = millis();
        if (lockDepartures(pdMS_TO_TICKS(200))) {
            g_statusText = "Timeout odswiezania";
            unlockDepartures();
        }
        drawStatusBar(tft);
        if (g_stopModalOpen) {
            drawStopSelectionModal(tft);
        }
    }

    if (g_routeLoadInProgress && millis() - g_routeStartedMs > ROUTE_FETCH_TIMEOUT_MS) {
        if (lockRouteState(pdMS_TO_TICKS(200))) {
            g_routeLoadInProgress = false;
            g_routeTaskHandle = nullptr;
            g_routeStartedMs = 0;
            g_routeRequestSeq++;
            g_routeHasData = false;
            g_routeDisplayCount = 0;
            g_routeStatusText = "Timeout trasy";
            g_routeRenderPending = true;
            unlockRouteState();
        }
    }

    handleTouchInput();

    if (g_refreshRenderPending) {
        g_refreshRenderPending = false;
        drawTimetableData(tft);
        if (g_stopModalOpen) {
            drawStopSelectionModal(tft);
        }
        checkDepartureAnnouncements();
    }

    bool routeRenderNow = false;
    if (lockRouteState(pdMS_TO_TICKS(30))) {
        if (g_routeRenderPending) {
            g_routeRenderPending = false;
            routeRenderNow = true;
        }
        unlockRouteState();
    }

    if (routeRenderNow) {
        renderRouteScreenFromState();
    }

    delay(50);
}

void drawTimetable(TFT_eSPI& tft) {
    selectDisplay(TFT_CS1_PIN);
    tft.setRotation(ROT_MAIN);
    tft.fillScreen(COLOR_BG);

    static String cachedActiveStopLabel = DEFAULT_STOP_LABEL;
    if (lockDepartures(pdMS_TO_TICKS(20))) {
        cachedActiveStopLabel = g_activeStopLabel;
        unlockDepartures();
    }

    tft.setTextColor(COLOR_TEXT);
    tft.setTextSize(1);
    tft.setCursor(10, 8);
    tft.print("PRZYSTANEK:");
    tft.setTextSize(2);
    tft.setCursor(10, 18);
    tft.print(cachedActiveStopLabel);

    drawStopPickerButton(tft);

    drawTimetableData(tft);
}

void drawTimetableData(TFT_eSPI& tft) {
    selectDisplay(TFT_CS1_PIN);
    tft.setRotation(ROT_MAIN);
    tft.fillRect(10, 40, 300, 160, COLOR_BG);

    DepartureEntry localDepartures[API_LIMIT];
    int validCount = 0;
    if (!lockDepartures(pdMS_TO_TICKS(120))) {
        drawStatusBar(tft);
        return;
    }

    validCount = countValidDeparturesUnlocked();
    for (int i = 0; i < API_LIMIT; i++) {
        localDepartures[i] = g_departures[i];
    }
    unlockDepartures();

    int maxOffset = (validCount > VISIBLE_DEPARTURES) ? (validCount - VISIBLE_DEPARTURES) : 0;
    if (g_scrollOffset > maxOffset) {
        g_scrollOffset = maxOffset;
    }

    for (int row = 0; row < VISIBLE_DEPARTURES; row++) {
        int dataIndex = g_scrollOffset + row;
        if (dataIndex < validCount && localDepartures[dataIndex].valid) {
            drawDepartureCard(
                tft,
                row,
                localDepartures[dataIndex].line,
                localDepartures[dataIndex].direction,
                minuteLabel(localDepartures[dataIndex].minutesToDeparture),
                departureColor(localDepartures[dataIndex].minutesToDeparture)
            );
        } else {
            String emptyDir = (validCount == 0 && row == 0) ? "BRAK DANYCH" : "";
            drawDepartureCard(tft, row, "-", emptyDir, "--", COLOR_TEXT);
        }
    }

    drawStatusBar(tft);
}

void drawStatusBar(TFT_eSPI& tft) {
    selectDisplay(TFT_CS1_PIN);
    tft.setRotation(ROT_MAIN);
    tft.fillRect(10, 198, 300, 10, COLOR_BG);
    tft.setTextColor(COLOR_TEXT);
    tft.setTextSize(1);
    tft.setCursor(10, 200);

    int validCount = 0;
    String statusTextLocal = "...";
    String lastUpdateTextLocal = "--:--:--";

    if (lockDepartures(pdMS_TO_TICKS(80))) {
        validCount = countValidDeparturesUnlocked();
        statusTextLocal = g_statusText;
        lastUpdateTextLocal = g_lastUpdateText;
        unlockDepartures();
    }

    int from = (validCount == 0) ? 0 : (g_scrollOffset + 1);
    int to = (validCount == 0) ? 0 : min(validCount, g_scrollOffset + VISIBLE_DEPARTURES);

    String safeStatus = statusTextLocal + " " + lastUpdateTextLocal + " " + String(from) + "-" + String(to) + "/" + String(validCount);
    if (safeStatus.length() > 44) {
        safeStatus = safeStatus.substring(0, 43) + ".";
    }
    tft.print(safeStatus);

    drawScrollButtons(tft);
}

void drawScrollButtons(TFT_eSPI& tft) {
    selectDisplay(TFT_CS1_PIN);
    tft.setRotation(ROT_MAIN);
    bool showUp = g_scrollOffset > 0;
    bool showDown = g_scrollOffset < maxScrollOffset();

    tft.fillRect(BTN_SCROLL_UP_X, BTN_SCROLL_UP_Y, BTN_SCROLL_UP_W, BTN_SCROLL_UP_H, COLOR_BG);
    tft.fillRect(BTN_SCROLL_DOWN_X, BTN_SCROLL_DOWN_Y, BTN_SCROLL_DOWN_W, BTN_SCROLL_DOWN_H, COLOR_BG);

    if (showUp) {
        tft.fillRect(BTN_SCROLL_UP_X, BTN_SCROLL_UP_Y, BTN_SCROLL_UP_W, BTN_SCROLL_UP_H, COLOR_BTN);
        tft.setCursor(BTN_SCROLL_UP_X + 28, BTN_SCROLL_UP_Y + 8);
        tft.print("GORA");
        tft.setCursor(BTN_SCROLL_UP_X + 72, BTN_SCROLL_UP_Y + 8);
        tft.print("^");
    }

    if (showDown) {
        tft.fillRect(BTN_SCROLL_DOWN_X, BTN_SCROLL_DOWN_Y, BTN_SCROLL_DOWN_W, BTN_SCROLL_DOWN_H, COLOR_BTN);
        tft.setCursor(BTN_SCROLL_DOWN_X + 34, BTN_SCROLL_DOWN_Y + 8);
        tft.print("DOL");
        tft.setCursor(BTN_SCROLL_DOWN_X + 72, BTN_SCROLL_DOWN_Y + 8);
        tft.print("v");
    }
}

void drawStopPickerButton(TFT_eSPI& tft) {
    selectDisplay(TFT_CS1_PIN);
    tft.setRotation(ROT_MAIN);
    tft.fillRoundRect(BTN_STOP_PICKER_X, BTN_STOP_PICKER_Y, BTN_STOP_PICKER_W, BTN_STOP_PICKER_H, 4, COLOR_BTN);
    tft.drawRoundRect(BTN_STOP_PICKER_X, BTN_STOP_PICKER_Y, BTN_STOP_PICKER_W, BTN_STOP_PICKER_H, 4, COLOR_TEXT);
    tft.setTextColor(COLOR_CARD);
    tft.setTextSize(1);
    tft.setCursor(BTN_STOP_PICKER_X + 6, BTN_STOP_PICKER_Y + 8);
    tft.print("STOP");
}

int maxStopMenuOffset() {
    if (g_stopCount <= STOP_MODAL_VISIBLE_ROWS) {
        return 0;
    }
    return g_stopCount - STOP_MODAL_VISIBLE_ROWS;
}

void drawStopSelectionModal(TFT_eSPI& tft) {
    selectDisplay(TFT_CS1_PIN);
    tft.setRotation(ROT_MAIN);
    tft.fillRect(STOP_MODAL_X, STOP_MODAL_Y, STOP_MODAL_W, STOP_MODAL_H, COLOR_CARD);
    tft.drawRect(STOP_MODAL_X, STOP_MODAL_Y, STOP_MODAL_W, STOP_MODAL_H, COLOR_TEXT);

    tft.setTextColor(COLOR_TEXT);
    tft.setTextSize(1);
    tft.setCursor(STOP_MODAL_X + 8, STOP_MODAL_Y + 8);
    tft.print("WYBIERZ PRZYSTANEK");

    tft.fillRect(STOP_MODAL_CLOSE_X, STOP_MODAL_CLOSE_Y, STOP_MODAL_CLOSE_W, STOP_MODAL_CLOSE_H, COLOR_BG);
    tft.drawRect(STOP_MODAL_CLOSE_X, STOP_MODAL_CLOSE_Y, STOP_MODAL_CLOSE_W, STOP_MODAL_CLOSE_H, COLOR_TEXT);
    tft.setCursor(STOP_MODAL_CLOSE_X + 6, STOP_MODAL_CLOSE_Y + 5);
    tft.print("X");

    int filterY = STOP_MODAL_LIST_Y + STOP_MODAL_VISIBLE_ROWS * STOP_MODAL_ROW_H + 2;
    int filterLabelX = STOP_MODAL_X + 10;
    int filterPrevX = filterLabelX + 80;
    int filterNextX = filterPrevX + 26;
    int filterBtnW = 22;
    int filterBtnH = 16;

    tft.fillRect(filterLabelX, filterY, 140, filterBtnH, COLOR_CARD);
    tft.setTextColor(COLOR_TEXT);
    tft.setCursor(filterLabelX, filterY + 4);
    tft.print("LITERA:");
    tft.setCursor(filterLabelX + 48, filterY + 4);
    if (g_stopFilterLetter >= 'A' && g_stopFilterLetter <= 'Z') {
        tft.print(g_stopFilterLetter);
    } else {
        tft.print("-");
    }

    tft.fillRect(filterPrevX, filterY, filterBtnW, filterBtnH, COLOR_BG);
    tft.drawRect(filterPrevX, filterY, filterBtnW, filterBtnH, COLOR_TEXT);
    tft.setCursor(filterPrevX + 6, filterY + 4);
    tft.print("<");

    tft.fillRect(filterNextX, filterY, filterBtnW, filterBtnH, COLOR_BG);
    tft.drawRect(filterNextX, filterY, filterBtnW, filterBtnH, COLOR_TEXT);
    tft.setCursor(filterNextX + 6, filterY + 4);
    tft.print(">");

    int offset = g_stopMenuOffset;
    if (offset < 0) {
        offset = 0;
    }
    int maxOffset = maxStopMenuOffset();
    if (offset > maxOffset) {
        offset = maxOffset;
        g_stopMenuOffset = offset;
    }

    for (int row = 0; row < STOP_MODAL_VISIBLE_ROWS; row++) {
        int index = offset + row;
        int rowY = STOP_MODAL_LIST_Y + row * STOP_MODAL_ROW_H;
        bool isSelected = index == g_activeStopIndex;

        uint16_t rowBg = isSelected ? COLOR_ROUTE : COLOR_BG;
        uint16_t rowText = isSelected ? COLOR_CARD : COLOR_TEXT;

        tft.fillRect(STOP_MODAL_LIST_X, rowY, STOP_MODAL_LIST_W, STOP_MODAL_ROW_H - 2, rowBg);
        tft.drawRect(STOP_MODAL_LIST_X, rowY, STOP_MODAL_LIST_W, STOP_MODAL_ROW_H - 2, COLOR_ROUTE);

        if (index < g_stopCount) {
            String label = stopIdToLabel(g_stopIds[index]);
            if (label.length() > 30) {
                label = label.substring(0, 29) + ".";
            }
            tft.setTextColor(rowText);
            tft.setCursor(STOP_MODAL_LIST_X + 4, rowY + 8);
            tft.print(label);
        }
    }

    if (g_stopCount == 0) {
        tft.setTextColor(COLOR_TEXT);
        tft.setCursor(STOP_MODAL_LIST_X + 6, STOP_MODAL_LIST_Y + 14);
        tft.print("BRAK LISTY PRZYSTANKOW");
    }

    bool showUp = offset > 0;
    bool showDown = offset < maxOffset;

    tft.fillRect(STOP_MODAL_SCROLL_X, STOP_MODAL_SCROLL_UP_Y, STOP_MODAL_SCROLL_W, STOP_MODAL_SCROLL_H, COLOR_CARD);
    tft.fillRect(STOP_MODAL_SCROLL_X, STOP_MODAL_SCROLL_DOWN_Y, STOP_MODAL_SCROLL_W, STOP_MODAL_SCROLL_H, COLOR_CARD);

    if (showUp) {
        tft.fillRect(STOP_MODAL_SCROLL_X, STOP_MODAL_SCROLL_UP_Y, STOP_MODAL_SCROLL_W, STOP_MODAL_SCROLL_H, COLOR_BTN);
        tft.setTextColor(COLOR_CARD);
        tft.setCursor(STOP_MODAL_SCROLL_X + 8, STOP_MODAL_SCROLL_UP_Y + 8);
        tft.print("^");
    }

    if (showDown) {
        tft.fillRect(STOP_MODAL_SCROLL_X, STOP_MODAL_SCROLL_DOWN_Y, STOP_MODAL_SCROLL_W, STOP_MODAL_SCROLL_H, COLOR_BTN);
        tft.setTextColor(COLOR_CARD);
        tft.setCursor(STOP_MODAL_SCROLL_X + 8, STOP_MODAL_SCROLL_DOWN_Y + 8);
        tft.print("v");
    }
}

void openStopSelectionModal() {
    if (g_activeStopIndex >= 0) {
        g_stopMenuOffset = g_activeStopIndex - (STOP_MODAL_VISIBLE_ROWS / 2);
        if (g_stopMenuOffset < 0) {
            g_stopMenuOffset = 0;
        }
        int maxOffset = maxStopMenuOffset();
        if (g_stopMenuOffset > maxOffset) {
            g_stopMenuOffset = maxOffset;
        }
    }

    g_stopModalOpen = true;
    drawStopSelectionModal(tft);
}

void closeStopSelectionModal(bool fullRedraw) {
    g_stopModalOpen = false;
    if (fullRedraw) {
        drawTimetable(tft);
    }
}

void handleStopModalTouch(int16_t touchX, int16_t touchY) {
    if (isPointInRect(touchX, touchY, STOP_MODAL_CLOSE_X, STOP_MODAL_CLOSE_Y, STOP_MODAL_CLOSE_W, STOP_MODAL_CLOSE_H)) {
        closeStopSelectionModal(true);
        return;
    }

    int filterY = STOP_MODAL_LIST_Y + STOP_MODAL_VISIBLE_ROWS * STOP_MODAL_ROW_H + 2;
    int filterLabelX = STOP_MODAL_X + 10;
    int filterPrevX = filterLabelX + 80;
    int filterNextX = filterPrevX + 26;
    int filterBtnW = 22;
    int filterBtnH = 16;

    if (isPointInRect(touchX, touchY, filterPrevX, filterY, filterBtnW, filterBtnH)) {
        changeStopFilterLetter(-1);
        drawStopSelectionModal(tft);
        return;
    }

    if (isPointInRect(touchX, touchY, filterNextX, filterY, filterBtnW, filterBtnH)) {
        changeStopFilterLetter(+1);
        drawStopSelectionModal(tft);
        return;
    }

    if (touchY >= filterY) {
        return;
    }

    bool canScrollUp = g_stopMenuOffset > 0;
    bool canScrollDown = g_stopMenuOffset < maxStopMenuOffset();

    if (canScrollUp && isPointInRect(touchX, touchY, STOP_MODAL_SCROLL_X, STOP_MODAL_SCROLL_UP_Y, STOP_MODAL_SCROLL_W, STOP_MODAL_SCROLL_H)) {
        g_stopMenuOffset -= STOP_MODAL_VISIBLE_ROWS;
        if (g_stopMenuOffset < 0) {
            g_stopMenuOffset = 0;
        }
        drawStopSelectionModal(tft);
        return;
    }

    if (canScrollDown && isPointInRect(touchX, touchY, STOP_MODAL_SCROLL_X, STOP_MODAL_SCROLL_DOWN_Y, STOP_MODAL_SCROLL_W, STOP_MODAL_SCROLL_H)) {
        g_stopMenuOffset += STOP_MODAL_VISIBLE_ROWS;
        int maxOffset = maxStopMenuOffset();
        if (g_stopMenuOffset > maxOffset) {
            g_stopMenuOffset = maxOffset;
        }
        drawStopSelectionModal(tft);
        return;
    }

    for (int row = 0; row < STOP_MODAL_VISIBLE_ROWS; row++) {
        int rowY = STOP_MODAL_LIST_Y + row * STOP_MODAL_ROW_H;
        if (!isPointInRect(touchX, touchY, STOP_MODAL_LIST_X, rowY, STOP_MODAL_LIST_W, STOP_MODAL_ROW_H - 2)) {
            continue;
        }

        int selectedIndex = g_stopMenuOffset + row;
        if (selectedIndex < g_stopCount) {
            selectStopByIndex(selectedIndex);
        }
        return;
    }
}

void selectStopByIndex(int index) {
    if (index < 0 || index >= g_stopCount) {
        return;
    }

    String newStopId = g_stopIds[index];
    String newStopLabel = stopIdToLabel(newStopId);
    bool changed = false;

    if (lockDepartures(pdMS_TO_TICKS(700))) {
        changed = g_activeStopId != newStopId;
        g_activeStopId = newStopId;
        g_activeStopLabel = newStopLabel;
        g_activeStopIndex = index;
        g_scrollOffset = 0;
        g_statusText = changed ? "Zmiana przystanku..." : "Przystanek bez zmian";
        if (changed) {
            clearDepartures();
            saveStop(newStopId);
        }
        unlockDepartures();
    } else {
        if (lockDepartures(pdMS_TO_TICKS(20))) {
            g_statusText = "Blad zmiany przystanku";
            unlockDepartures();
        }
        drawStatusBar(tft);
        return;
    }

    closeStopSelectionModal(true);

    if (changed) {
        g_lastApiFetchMs = 0;
    }
}

int findStopIndexById(const String& stopId) {
    for (int i = 0; i < g_stopCount; i++) {
        if (g_stopIds[i] == stopId) {
            return i;
        }
    }
    return -1;
}

int findFirstStopIndexByLetter(char letter) {
    if (letter < 'A' || letter > 'Z') {
        return -1;
    }

    for (int i = 0; i < g_stopCount; i++) {
        String label = stopIdToLabel(g_stopIds[i]);
        if (label.length() == 0) {
            continue;
        }
        char c = label.charAt(0);
        if (c == letter) {
            return i;
        }
    }
    return -1;
}

void changeStopFilterLetter(int delta) {
    if (delta == 0 || g_stopCount <= 0) {
        return;
    }

    char letter = g_stopFilterLetter;
    if (letter < 'A' || letter > 'Z') {
        letter = (delta > 0) ? 'A' : 'Z';
    } else {
        letter = static_cast<char>(letter + delta);
        if (letter > 'Z') letter = 'A';
        if (letter < 'A') letter = 'Z';
    }

    // Skip empty letters so the filter always lands on a populated section.
    char startLetter = letter;
    for (int tries = 0; tries < 26; tries++) {
        int idx = findFirstStopIndexByLetter(letter);
        if (idx >= 0) {
            g_stopFilterLetter = letter;
            g_stopMenuOffset = idx;
            int maxOffset = maxStopMenuOffset();
            if (g_stopMenuOffset > maxOffset) g_stopMenuOffset = maxOffset;
            if (g_stopMenuOffset < 0)         g_stopMenuOffset = 0;
            return;
        }
        letter = static_cast<char>(letter + delta);
        if (letter > 'Z') letter = 'A';
        if (letter < 'A') letter = 'Z';
        if (letter == startLetter) break;
    }
}

String stopIdToLabel(const String& stopId) {
    String label = stopId;
    label.replace("-", " ");

    int lastNonDigit = label.length() - 1;
    while (lastNonDigit >= 0 && isdigit(static_cast<unsigned char>(label[lastNonDigit]))) {
        lastNonDigit--;
    }
    if (lastNonDigit >= 0 && lastNonDigit < label.length() - 1 && label[lastNonDigit] != ' ') {
        label = label.substring(0, lastNonDigit + 1) + " " + label.substring(lastNonDigit + 1);
    }

    label.toUpperCase();
    return label;
}

bool addStopIdIfUnique(const String& stopId) {
    if (stopId.length() == 0 || g_stopCount >= MAX_STOP_OPTIONS) {
        return false;
    }

    for (int i = 0; i < g_stopCount; i++) {
        if (g_stopIds[i] == stopId) {
            return false;
        }
    }

    g_stopIds[g_stopCount] = stopId;
    g_stopCount++;
    return true;
}

bool loadStopsFromEmbeddedJson() {
    const char* dataStart = stops_json_data;
    const char* dataEnd = stops_json_data + (sizeof(stops_json_data) - 1);
    if (dataStart == nullptr || dataEnd == nullptr || dataEnd <= dataStart) {
        return false;
    }

    const char key[] = "\"przystanki\"";
    const char* cursor = nullptr;
    for (const char* it = dataStart; it + static_cast<int>(sizeof(key)) < dataEnd; it++) {
        if (memcmp(it, key, sizeof(key) - 1) == 0) {
            cursor = it + sizeof(key) - 1;
            break;
        }
    }
    if (cursor == nullptr) {
        return false;
    }

    while (cursor < dataEnd && *cursor != '[') {
        cursor++;
    }
    if (cursor >= dataEnd) {
        return false;
    }

    cursor++;
    g_stopCount = 0;
    int parsedCount = 0;
    int duplicates = 0;

    while (cursor < dataEnd) {
        while (cursor < dataEnd && *cursor != '"' && *cursor != ']') {
            cursor++;
        }
        if (cursor >= dataEnd || *cursor == ']') {
            break;
        }

        cursor++;
        const char* tokenStart = cursor;
        while (cursor < dataEnd && *cursor != '"') {
            cursor++;
        }
        if (cursor >= dataEnd) {
            break;
        }

        parsedCount++;
        String stopId(tokenStart, static_cast<unsigned int>(cursor - tokenStart));
        if (!addStopIdIfUnique(stopId)) {
            duplicates++;
        }
        cursor++;
    }

    g_activeStopIndex = findStopIndexById(g_activeStopId);
    if (g_activeStopIndex < 0 && g_stopCount > 0) {
        g_activeStopIndex = 0;
        g_activeStopId = g_stopIds[0];
        g_activeStopLabel = stopIdToLabel(g_activeStopId);
    } else if (g_activeStopIndex >= 0) {
        g_activeStopLabel = stopIdToLabel(g_activeStopId);
    }

    Serial.printf("Stops parsed=%d unique=%d duplicates=%d\n", parsedCount, g_stopCount, duplicates);
    return g_stopCount > 0;
}

void drawDepartureCard(TFT_eSPI& tft, int index, const String& line, const String& dir, const String& timeStr, uint16_t timeColor) {
    int yPos = 40 + (index * 40);
    
    tft.fillRoundRect(10, yPos, 300, 35, 3, COLOR_CARD);

    int lineLen = static_cast<int>(line.length());
    int lineTextSize = (lineLen >= 3) ? 2 : 3;
    int lineCharWidth = 6 * lineTextSize;
    int lineX = 20;
    int lineY = (lineTextSize == 3) ? (yPos + 8) : (yPos + 11);

    tft.setTextColor(COLOR_TEXT);
    tft.setTextSize(lineTextSize);
    tft.setCursor(lineX, lineY);
    tft.print(line);

    int directionX = lineX + (lineLen * lineCharWidth) + 8;
    if (directionX < 72) {
        directionX = 72;
    }
    if (directionX > 140) {
        directionX = 140;
    }

    const int timeX = 236;
    int maxDirectionChars = (timeX - directionX - 6) / 12;
    if (maxDirectionChars < 4) {
        maxDirectionChars = 4;
    }

    String safeDir = dir;
    if (static_cast<int>(safeDir.length()) > maxDirectionChars) {
        safeDir = safeDir.substring(0, maxDirectionChars - 1) + ".";
    }

    tft.setTextSize(1);
    tft.setCursor(directionX, yPos + 6);
    tft.print("Kierunek:");
    tft.setTextSize(2);
    tft.setCursor(directionX, yPos + 16);
    tft.print(safeDir);

    tft.setTextColor(timeColor);
    
    tft.setCursor(timeX, yPos + 10);
    tft.print(timeStr);
}

static void showWifiScreen(const String& line1, const String& line2 = "", const String& line3 = "") {
    selectDisplay(TFT_CS1_PIN);
    tft.setRotation(ROT_MAIN);
    tft.fillScreen(COLOR_BG);
    tft.setTextColor(COLOR_TEXT);
    tft.setTextSize(2);
    tft.setCursor(10, 20);
    tft.print(line1);
    if (line2.length() > 0) {
        tft.setCursor(10, 50);
        tft.print(line2);
    }
    if (line3.length() > 0) {
        tft.setTextSize(1);
        tft.setCursor(10, 80);
        tft.print(line3);
    }
}

bool connectToWifi(uint32_t timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    WiFiManager wm;
    wm.setConnectTimeout(timeoutMs / 1000);

    wm.setAPCallback([](WiFiManager* wm) {
        String apIP = WiFi.softAPIP().toString();
        Serial.println("[WiFi] AP mode active, IP: " + apIP);
        showWifiScreen("Brak sieci WiFi",
                       "Polacz z:",
                       "SSID: AutorBus-Setup  IP: " + apIP);
        selectDisplay(TFT_CS2_PIN);
        tft.setRotation(ROT_ROUTE);
        tft.fillScreen(COLOR_BG);
        tft.setTextColor(COLOR_TEXT);
        tft.setTextSize(1);
        tft.setCursor(10, 20); tft.print("Konfiguracja WiFi");
        tft.setCursor(10, 40); tft.print("SSID: AutorBus-Setup");
        tft.setCursor(10, 56); tft.print("Adres: " + apIP);
    });

    bool connected = wm.autoConnect("AutorBus-Setup");

    if (connected) {
        // Force STA mode so the temporary access point is fully shut down.
        WiFi.mode(WIFI_STA);
        Serial.print("[WiFi] Connected in STA mode, IP: ");
        Serial.println(WiFi.localIP());
        return true;
    }

    Serial.println("[WiFi] Connection failed.");
    showWifiScreen("Brak WiFi", "Tryb offline");
    delay(1500);
    return false;
}

void initClock() {
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
    unsigned long startMs = millis();

    while (!isClockSynced() && millis() - startMs < 15000) {
        delay(250);
    }

    if (isClockSynced()) {
        Serial.println("Clock synchronized via NTP.");
    } else {
        Serial.println("NTP synchronization failed; minute countdowns may be limited.");
    }
}

bool isClockSynced() {
    return time(nullptr) > 1700000000;
}

void initTouchScreen() {
    g_touchReady = true;
    Serial.println("Touch panel (TFT_eSPI) ready.");
}

void handleTouchInput() {
    if (!g_touchReady) {
        return;
    }

    int16_t touchX = 0;
    int16_t touchY = 0;
    bool touched = readTouchScreenPoint(touchX, touchY);

    if (touched && !g_touchPressedLast) {
        selectDisplay(TFT_CS1_PIN);
        tft.setRotation(ROT_MAIN);

        if (g_stopModalOpen) {
            handleStopModalTouch(touchX, touchY);
            g_touchPressedLast = touched;
            return;
        }

        if (isPointInRect(touchX, touchY, BTN_STOP_PICKER_X, BTN_STOP_PICKER_Y, BTN_STOP_PICKER_W, BTN_STOP_PICKER_H)) {
            openStopSelectionModal();
            g_touchPressedLast = touched;
            return;
        }

        bool canScrollUp = g_scrollOffset > 0;
        bool canScrollDown = g_scrollOffset < maxScrollOffset();

        if (canScrollUp && isPointInRect(touchX, touchY, BTN_SCROLL_UP_X, BTN_SCROLL_UP_Y, BTN_SCROLL_UP_W, BTN_SCROLL_UP_H)) {
            scrollDepartures(-1);
            g_touchPressedLast = touched;
            return;
        } else if (canScrollDown && isPointInRect(touchX, touchY, BTN_SCROLL_DOWN_X, BTN_SCROLL_DOWN_Y, BTN_SCROLL_DOWN_W, BTN_SCROLL_DOWN_H)) {
            scrollDepartures(1);
            g_touchPressedLast = touched;
            return;
        }

        int validCount = 0;
        if (lockDepartures(pdMS_TO_TICKS(60))) {
            validCount = countValidDeparturesUnlocked();
            unlockDepartures();
        }

        for (int row = 0; row < VISIBLE_DEPARTURES; row++) {
            int dataIndex = g_scrollOffset + row;
            int cardY = 40 + (row * 40);
            if (dataIndex >= validCount) {
                continue;
            }

            if (isPointInRect(touchX, touchY, 10, cardY, 300, 35)) {
                requestRouteForDepartureIndex(dataIndex);
                g_touchPressedLast = touched;
                return;
            }
        }
    }

    g_touchPressedLast = touched;
}

bool readTouchScreenPoint(int16_t& x, int16_t& y) {
    // The touch controller uses its own CS line, so both TFT displays must be deselected first.
    deselectDisplays();

    uint16_t tx = 0;
    uint16_t ty = 0;
    if (!tft.getTouch(&tx, &ty)) {
        return false;
    }

    int16_t w = tft.width();
    int16_t h = tft.height();

    int16_t mappedX = static_cast<int16_t>(tx);
    int16_t mappedY = static_cast<int16_t>(ty);

    // Mirror the X axis to match the physical panel orientation.
    mappedX = (w - 1) - mappedX;

    // Apply the measured offset so touch targets line up with the UI.
    const int16_t offsetX = 15;
    mappedX += offsetX;

    mappedX = constrain(mappedX, 0, w - 1);
    mappedY = constrain(mappedY, 0, h - 1);

    x = mappedX;
    y = mappedY;
    return true;
}

bool isPointInRect(int16_t px, int16_t py, int16_t x, int16_t y, int16_t w, int16_t h) {
    return px >= x && px < (x + w) && py >= y && py < (y + h);
}

void scrollDepartures(int delta) {
    int newOffset = g_scrollOffset + delta;
    int maxOffset = maxScrollOffset();

    if (newOffset < 0) {
        newOffset = 0;
    }
    if (newOffset > maxOffset) {
        newOffset = maxOffset;
    }

    if (newOffset != g_scrollOffset) {
        g_scrollOffset = newOffset;
        drawTimetableData(tft);
    }
}

int countValidDepartures() {
    if (!lockDepartures(pdMS_TO_TICKS(20))) {
        return 0;
    }

    int count = countValidDeparturesUnlocked();
    unlockDepartures();
    return count;
}

int countValidDeparturesUnlocked() {
    int count = 0;
    for (int i = 0; i < API_LIMIT; i++) {
        if (g_departures[i].valid) {
            count++;
        }
    }
    return count;
}

int maxScrollOffset() {
    int validCount = countValidDepartures();
    if (validCount <= VISIBLE_DEPARTURES) {
        return 0;
    }
    return validCount - VISIBLE_DEPARTURES;
}

bool refreshDeparturesForTft1() {
    bool ok = refreshDeparturesData();
    drawTimetableData(tft);
    return ok;
}

bool refreshDeparturesData() {
    g_lastApiFetchMs = millis();

    if (!connectToWifi(12000)) {
        if (lockDepartures(pdMS_TO_TICKS(400))) {
            g_statusText = "Brak WiFi - pokazuje ostatnie dane";
            unlockDepartures();
        }
        return false;
    }

    if (!fetchDeparturesFromApi(API_LIMIT)) {
        if (lockDepartures(pdMS_TO_TICKS(400))) {
            g_statusText = "Blad API - pokazuje ostatnie dane";
            unlockDepartures();
        }
        return false;
    }

    String updateText = "brak czasu";
    if (isClockSynced()) {
        time_t now = time(nullptr);
        struct tm tmNow;
        localtime_r(&now, &tmNow);

        char buffer[9];
        strftime(buffer, sizeof(buffer), "%H:%M:%S", &tmNow);
        updateText = String(buffer);
    }

    if (lockDepartures(pdMS_TO_TICKS(400))) {
        g_lastUpdateText = updateText;
        g_statusText = "Dane online";
        unlockDepartures();
    }

    return true;
}

void refreshDeparturesTask(void*) {
    refreshDeparturesData();
    g_refreshRenderPending = true;
    g_refreshInProgress = false;
    g_refreshStartedMs = 0;
    g_refreshTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool lockDepartures(TickType_t timeoutTicks) {
    if (g_departuresMutex == nullptr) {
        return true;
    }
    return xSemaphoreTake(g_departuresMutex, timeoutTicks) == pdTRUE;
}

void unlockDepartures() {
    if (g_departuresMutex != nullptr) {
        xSemaphoreGive(g_departuresMutex);
    }
}

bool lockRouteState(TickType_t timeoutTicks) {
    if (g_routeMutex == nullptr) {
        return true;
    }
    return xSemaphoreTake(g_routeMutex, timeoutTicks) == pdTRUE;
}

void unlockRouteState() {
    if (g_routeMutex != nullptr) {
        xSemaphoreGive(g_routeMutex);
    }
}

void requestRouteForDepartureIndex(int index) {
    DepartureEntry selected;
    String activeStopId;
    bool hasSelection = false;

    if (lockDepartures(pdMS_TO_TICKS(150))) {
        if (index >= 0 && index < API_LIMIT && g_departures[index].valid) {
            selected = g_departures[index];
            activeStopId = g_activeStopId;
            hasSelection = true;
        }
        unlockDepartures();
    }

    if (!hasSelection || selected.line.length() == 0 || selected.line == "-") {
        return;
    }

    RouteTaskRequest* request = new RouteTaskRequest();
    if (request == nullptr) {
        if (lockRouteState(pdMS_TO_TICKS(80))) {
            g_routeStatusText = "Brak pamieci na trase";
            g_routeHasData = false;
            g_routeDisplayCount = 0;
            g_routeRenderPending = true;
            unlockRouteState();
        }
        return;
    }

    request->line = selected.line;
    request->direction = selected.direction;
    request->activeStopId = activeStopId;

    if (!lockRouteState(pdMS_TO_TICKS(120))) {
        delete request;
        return;
    }

    if (g_routeLoadInProgress) {
        unlockRouteState();
        delete request;
        return;
    }

    g_routeLoadInProgress = true;
    g_routeHasData = false;
    g_routeDisplayCount = 0;
    g_routeDisplayLine = request->line;
    g_routeDisplayDirection = request->direction;
    g_routeStatusText = "Ladowanie trasy...";
    g_routeStartedMs = millis();
    request->requestSeq = ++g_routeRequestSeq;
    g_routeRenderPending = true;
    unlockRouteState();

    BaseType_t taskOk = xTaskCreatePinnedToCore(
        routeFetchTask,
        "routeFetch",
        24576,
        request,
        1,
        &g_routeTaskHandle,
        1
    );

    if (taskOk != pdPASS) {
        delete request;
        if (lockRouteState(pdMS_TO_TICKS(80))) {
            g_routeLoadInProgress = false;
            g_routeTaskHandle = nullptr;
            g_routeStartedMs = 0;
            g_routeRequestSeq++;
            g_routeHasData = false;
            g_routeDisplayCount = 0;
            g_routeStatusText = "Blad taska trasy";
            g_routeRenderPending = true;
            unlockRouteState();
        }
    }
}

void routeFetchTask(void* parameter) {
    RouteTaskRequest* request = static_cast<RouteTaskRequest*>(parameter);
    if (request == nullptr) {
        if (lockRouteState(pdMS_TO_TICKS(80))) {
            g_routeLoadInProgress = false;
            g_routeTaskHandle = nullptr;
            g_routeStartedMs = 0;
            g_routeHasData = false;
            g_routeDisplayCount = 0;
            g_routeStatusText = "Pusty request trasy";
            g_routeRenderPending = true;
            unlockRouteState();
        }
        vTaskDelete(nullptr);
        return;
    }

    String line = request->line;
    String direction = request->direction;
    String activeStopId = request->activeStopId;
    uint32_t requestSeq = request->requestSeq;
    delete request;

    String preparedStops[MAX_ROUTE_STOPS];
    int preparedCount = 0;
    String matchedDirection;
    String statusText;

    bool ok = fetchAndPrepareRoute(
        line,
        direction,
        activeStopId,
        preparedStops,
        preparedCount,
        matchedDirection,
        statusText
    );

    if (lockRouteState(pdMS_TO_TICKS(300))) {
        if (requestSeq == g_routeRequestSeq) {
            g_routeLoadInProgress = false;
            g_routeTaskHandle = nullptr;
            g_routeStartedMs = 0;

            if (ok && preparedCount > 0) {
                g_routeHasData = true;
                g_routeDisplayLine = line;
                g_routeDisplayDirection = matchedDirection;
                g_routeDisplayCount = min(preparedCount, MAX_ROUTE_STOPS);
                for (int i = 0; i < g_routeDisplayCount; i++) {
                    g_routeDisplayStops[i] = preparedStops[i];
                }
                for (int i = g_routeDisplayCount; i < MAX_ROUTE_STOPS; i++) {
                    g_routeDisplayStops[i] = "";
                }
                if (matchedDirection.length() > 0) {
                    g_routeStatusText = "Kierunek: " + matchedDirection;
                } else {
                    g_routeStatusText = "Trasa gotowa";
                }
            } else {
                g_routeHasData = false;
                g_routeDisplayCount = 0;
                for (int i = 0; i < MAX_ROUTE_STOPS; i++) {
                    g_routeDisplayStops[i] = "";
                }
                g_routeDisplayLine = line;
                g_routeDisplayDirection = direction;
                g_routeStatusText = statusText.length() > 0 ? statusText : "Brak trasy";
            }

            g_routeRenderPending = true;
        }
        unlockRouteState();
    }

    vTaskDelete(nullptr);
}

String routeDirectionKey(const String& direction) {
    String normalized = normalizePolishText(direction);
    normalized.trim();
    normalized.toUpperCase();

    String compact = "";
    compact.reserve(normalized.length());
    bool previousSpace = false;

    for (int i = 0; i < normalized.length(); i++) {
        char ch = normalized.charAt(i);
        bool isSpace = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r');

        if (isSpace) {
            if (!previousSpace && compact.length() > 0) {
                compact += ' ';
            }
            previousSpace = true;
            continue;
        }

        compact += ch;
        previousSpace = false;
    }

    compact.trim();
    return compact;
}

int directionMatchScore(const String& requestedKey, const String& candidateKey) {
    if (requestedKey.length() == 0 || candidateKey.length() == 0) {
        return 0;
    }

    if (requestedKey == candidateKey) {
        return 1000;
    }

    if (candidateKey.startsWith(requestedKey) || requestedKey.startsWith(candidateKey)) {
        int lenDiff = requestedKey.length() - candidateKey.length();
        if (lenDiff < 0) {
            lenDiff = -lenDiff;
        }
        return 800 - min(lenDiff, 120);
    }

    if (candidateKey.indexOf(requestedKey) >= 0 || requestedKey.indexOf(candidateKey) >= 0) {
        return 650;
    }

    int score = 0;
    int tokenStart = 0;
    while (tokenStart < requestedKey.length()) {
        int tokenEnd = requestedKey.indexOf(' ', tokenStart);
        if (tokenEnd < 0) {
            tokenEnd = requestedKey.length();
        }

        String token = requestedKey.substring(tokenStart, tokenEnd);
        token.trim();
        if (token.length() >= 3 && candidateKey.indexOf(token) >= 0) {
            score += 60;
        }

        tokenStart = tokenEnd + 1;
    }

    return score;
}

int findMatchingDirectionIndex(JsonArray variants, const String& requestedDirectionKey, String& outMatchedDirectionLabel, String& outMatchedDirectionKey) {
    outMatchedDirectionLabel = "";
    outMatchedDirectionKey = "";

    if (variants.size() == 0) {
        return -1;
    }

    int bestIndex = -1;
    int bestScore = -1;

    for (int i = 0; i < static_cast<int>(variants.size()); i++) {
        JsonVariant variantCandidate = variants[i];
        if (!variantCandidate.is<JsonArray>()) {
            continue;
        }

        JsonArray variant = variantCandidate.as<JsonArray>();
        String label = "";
        if (variant.size() > 1) {
            label = normalizePolishText(jsonToString(variant[1]));
        }
        String key = routeDirectionKey(label);

        int score = requestedDirectionKey.length() > 0 ? directionMatchScore(requestedDirectionKey, key) : 1;
        if (score > bestScore) {
            bestScore = score;
            bestIndex = i;
            outMatchedDirectionLabel = label;
            outMatchedDirectionKey = key;
        }
    }

    if (bestIndex < 0) {
        return -1;
    }

    if (requestedDirectionKey.length() > 0 && bestScore <= 0 && variants.size() > 1) {
        outMatchedDirectionLabel = "";
        outMatchedDirectionKey = "";
        return -1;
    }

    return bestIndex;
}

bool extractStopsFromVariant(JsonArray variant, String stopIds[], String stopNames[], int& stopCount) {
    stopCount = 0;

    if (variant.size() < 3 || !variant[2].is<JsonArray>()) {
        return false;
    }

    JsonArray stops = variant[2].as<JsonArray>();
    for (JsonVariant stopVariant : stops) {
        if (stopCount >= MAX_ROUTE_STOPS) {
            break;
        }
        if (!stopVariant.is<JsonArray>()) {
            continue;
        }

        JsonArray stop = stopVariant.as<JsonArray>();
        if (stop.size() < 2) {
            continue;
        }

        String stopId = jsonToString(stop[0]);
        if (stopId.length() == 0 || stopId == "-") {
            continue;
        }

        String stopName = normalizePolishText(jsonToString(stop[1]));
        if (stopName.length() == 0 || stopName == "-") {
            stopName = stopIdToLabel(stopId);
        }

        stopIds[stopCount] = stopId;
        stopNames[stopCount] = stopName;
        stopCount++;
    }

    return stopCount > 0;
}

int findStopIndexInList(const String& stopId, String stopIds[], int stopCount) {
    if (stopId.length() == 0) {
        return -1;
    }

    for (int i = 0; i < stopCount; i++) {
        if (stopIds[i] == stopId) {
            return i;
        }
    }
    return -1;
}

void buildFilteredStopNames(const String& activeStopId, String stopIds[], String stopNames[], int stopCount, String outStops[], int& outCount) {
    outCount = 0;
    if (stopCount <= 0) {
        return;
    }

    int startIndex = findStopIndexInList(activeStopId, stopIds, stopCount);
    if (startIndex < 0) {
        startIndex = 0;
    }

    for (int i = startIndex; i < stopCount && outCount < MAX_ROUTE_STOPS; i++) {
        outStops[outCount] = stopNames[i];
        outCount++;
    }
}

bool routeCacheGet(const String& line, const String& directionKey, String stopIds[], String stopNames[], int& stopCount) {
    stopCount = 0;
    if (line.length() == 0 || directionKey.length() == 0) {
        return false;
    }

    if (!lockRouteState(pdMS_TO_TICKS(120))) {
        return false;
    }

    bool found = false;
    for (int i = 0; i < ROUTE_CACHE_SIZE; i++) {
        RouteCacheEntry& entry = g_routeCache[i];
        if (!entry.valid) {
            continue;
        }
        if (entry.line != line || entry.directionKey != directionKey) {
            continue;
        }

        stopCount = min(entry.stopCount, MAX_ROUTE_STOPS);
        for (int j = 0; j < stopCount; j++) {
            stopIds[j] = entry.stopIds[j];
            stopNames[j] = entry.stopNames[j];
        }
        entry.lastUsed = ++g_routeCacheUseCounter;
        found = true;
        break;
    }

    unlockRouteState();
    return found;
}

void routeCachePut(const String& line, const String& directionKey, String stopIds[], String stopNames[], int stopCount) {
    if (line.length() == 0 || directionKey.length() == 0 || stopCount <= 0) {
        return;
    }

    if (!lockRouteState(pdMS_TO_TICKS(200))) {
        return;
    }

    int slot = -1;
    for (int i = 0; i < ROUTE_CACHE_SIZE; i++) {
        if (g_routeCache[i].valid && g_routeCache[i].line == line && g_routeCache[i].directionKey == directionKey) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        for (int i = 0; i < ROUTE_CACHE_SIZE; i++) {
            if (!g_routeCache[i].valid) {
                slot = i;
                break;
            }
        }
    }

    if (slot < 0) {
        uint32_t oldestUse = g_routeCache[0].lastUsed;
        slot = 0;
        for (int i = 1; i < ROUTE_CACHE_SIZE; i++) {
            if (g_routeCache[i].lastUsed < oldestUse) {
                oldestUse = g_routeCache[i].lastUsed;
                slot = i;
            }
        }
    }

    RouteCacheEntry& entry = g_routeCache[slot];
    entry.valid = true;
    entry.line = line;
    entry.directionKey = directionKey;
    entry.stopCount = min(stopCount, MAX_ROUTE_STOPS);
    entry.lastUsed = ++g_routeCacheUseCounter;

    for (int i = 0; i < entry.stopCount; i++) {
        entry.stopIds[i] = stopIds[i];
        entry.stopNames[i] = stopNames[i];
    }
    for (int i = entry.stopCount; i < MAX_ROUTE_STOPS; i++) {
        entry.stopIds[i] = "";
        entry.stopNames[i] = "";
    }

    unlockRouteState();
}

bool fetchAndPrepareRoute(const String& line, const String& requestedDirection, const String& activeStopId, String outStops[], int& outCount, String& outMatchedDirection, String& outStatusText) {
    outCount = 0;
    outMatchedDirection = "";
    outStatusText = "";

    String requestedKey = routeDirectionKey(requestedDirection);

    String cachedStopIds[MAX_ROUTE_STOPS];
    String cachedStopNames[MAX_ROUTE_STOPS];
    int cachedStopCount = 0;

    if (routeCacheGet(line, requestedKey, cachedStopIds, cachedStopNames, cachedStopCount)) {
        buildFilteredStopNames(activeStopId, cachedStopIds, cachedStopNames, cachedStopCount, outStops, outCount);
        outMatchedDirection = requestedDirection;
        if (outCount <= 0) {
            outStatusText = "Pusta trasa w cache";
            return false;
        }
        return true;
    }

    if (!connectToWifi(12000)) {
        outStatusText = "Brak WiFi dla trasy";
        return false;
    }

    String url = String(ROUTE_API_BASE_URL) + "?id=" + line;
    Serial.print("GET ");
    Serial.println(url);
    Serial.print("Route request: line=");
    Serial.print(line);
    Serial.print(" direction=");
    Serial.println(requestedDirection);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, url)) {
        outStatusText = "HTTP begin trasy";
        return false;
    }

    http.setTimeout(15000);
    http.useHTTP10(true);
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        outStatusText = "HTTP trasy " + String(httpCode) + " linia " + line;
        http.end();
        return false;
    }

    int contentLength = http.getSize();

    DynamicJsonDocument filter(1024);
    filter[1][0][0] = true;
    filter[1][0][1] = true;
    filter[1][0][2][0][0] = true;
    filter[1][0][2][0][1] = true;
    filter[0][0][0] = true;
    filter[0][0][1] = true;
    filter[0][0][2][0][0] = true;
    filter[0][0][2][0][1] = true;

    DynamicJsonDocument doc(32768);
    DeserializationError error = deserializeJson(
        doc,
        *http.getStreamPtr(),
        DeserializationOption::Filter(filter)
    );
    http.end();

    if (error) {
        outStatusText = "JSON trasy " + String(error.c_str()) + " len=" + (contentLength >= 0 ? String(contentLength) : String("?")) + " linia " + line;
        Serial.print("Route JSON error: ");
        Serial.println(error.c_str());
        return false;
    }

    if (!doc.is<JsonArray>()) {
        outStatusText = "Root trasy nie tablica linia " + line;
        return false;
    }

    JsonArray root = doc.as<JsonArray>();
    JsonArray variants;

    auto isVariantsContainer = [](JsonArray candidate) -> bool {
        if (candidate.size() == 0 || !candidate[0].is<JsonArray>()) {
            return false;
        }
        JsonArray firstVariant = candidate[0].as<JsonArray>();
        return firstVariant.size() > 2 && firstVariant[2].is<JsonArray>();
    };

    if (isVariantsContainer(root)) {
        variants = root;
    } else {
        for (JsonVariant item : root) {
            if (!item.is<JsonArray>()) {
                continue;
            }
            JsonArray candidate = item.as<JsonArray>();
            if (isVariantsContainer(candidate)) {
                variants = candidate;
                break;
            }
        }
    }

    if (variants.size() == 0) {
        outStatusText = "Brak wariantow trasy linia " + line + " root=" + String(root.size());
        Serial.print("No route variants found. line=");
        Serial.print(line);
        Serial.print(" rootSize=");
        Serial.println(root.size());
        return false;
    }
    String matchedDirectionLabel;
    String matchedDirectionKey;
    int variantIndex = findMatchingDirectionIndex(variants, requestedKey, matchedDirectionLabel, matchedDirectionKey);
    if (variantIndex < 0) {
        outStatusText = "Brak dopasowania kierunku " + normalizePolishText(requestedDirection) + " linia " + line;
        return false;
    }

    JsonVariant selectedVariantValue = variants[variantIndex];
    if (!selectedVariantValue.is<JsonArray>()) {
        outStatusText = "Wariant trasy jest niepoprawny";
        return false;
    }

    JsonArray selectedVariant = selectedVariantValue.as<JsonArray>();
    String variantStopIds[MAX_ROUTE_STOPS];
    String variantStopNames[MAX_ROUTE_STOPS];
    int variantStopCount = 0;

    if (!extractStopsFromVariant(selectedVariant, variantStopIds, variantStopNames, variantStopCount)) {
        outStatusText = "Brak przystankow trasy";
        return false;
    }

    if (matchedDirectionKey.length() > 0) {
        routeCachePut(line, matchedDirectionKey, variantStopIds, variantStopNames, variantStopCount);
    }
    if (requestedKey.length() > 0 && requestedKey != matchedDirectionKey) {
        routeCachePut(line, requestedKey, variantStopIds, variantStopNames, variantStopCount);
    }

    buildFilteredStopNames(activeStopId, variantStopIds, variantStopNames, variantStopCount, outStops, outCount);
    if (outCount <= 0) {
        outStatusText = "Brak przystankow po filtrze";
        return false;
    }

    outMatchedDirection = matchedDirectionLabel.length() > 0 ? matchedDirectionLabel : requestedDirection;
    return true;
}

void getCurrentTimeDate(String& outTime, String& outDate) {
    outTime = "--:--:--";
    outDate = "brak czasu";

    if (!isClockSynced()) {
        return;
    }

    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);

    char timeBuffer[9];
    char dateBuffer[11];
    strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", &tmNow);
    strftime(dateBuffer, sizeof(dateBuffer), "%d/%m/%Y", &tmNow);

    outTime = String(timeBuffer);
    outDate = String(dateBuffer);
}

void drawRoutePlaceholder(TFT_eSPI& tft, const String& title, const String& message) {
    selectDisplay(TFT_CS2_PIN);
    tft.setRotation(ROT_ROUTE);
    tft.fillScreen(COLOR_BG);

    tft.setTextColor(COLOR_TEXT);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.print(title);

    String safeMessage = normalizePolishText(message);
    if (safeMessage.length() == 0) {
        safeMessage = "Brak danych";
    }

    tft.setTextSize(1);
    int cursorY = 42;
    int pos = 0;
    int messageLen = static_cast<int>(safeMessage.length());
    while (pos < messageLen && cursorY <= 176) {
        const int maxChars = 50;
        int end = min(pos + maxChars, messageLen);
        if (end < messageLen) {
            int lastSpace = safeMessage.lastIndexOf(' ', end - 1);
            if (lastSpace > pos) {
                end = lastSpace;
            }
        }

        String line = safeMessage.substring(pos, end);
        line.trim();
        tft.setCursor(8, cursorY);
        tft.print(line);
        cursorY += 10;

        pos = end;
        while (pos < messageLen && safeMessage.charAt(pos) == ' ') {
            pos++;
        }
    }

    selectDisplay(TFT_CS1_PIN);
}

void renderRouteScreenFromState() {
    String localLine = "-";
    String localStatus = "";
    bool localHasData = false;
    bool localLoading = false;
    int localCount = 0;
    String localStops[MAX_ROUTE_STOPS];

    if (!lockRouteState(pdMS_TO_TICKS(300))) {
        drawRoutePlaceholder(tft, "TRASA LINII", "Blad stanu trasy");
        return;
    }

    localLine = g_routeDisplayLine;
    localStatus = g_routeStatusText;
    localHasData = g_routeHasData;
    localLoading = g_routeLoadInProgress;
    localCount = min(max(g_routeDisplayCount, 0), MAX_ROUTE_STOPS);
    for (int i = 0; i < localCount; i++) {
        localStops[i] = g_routeDisplayStops[i];
    }
    unlockRouteState();

    if (localLoading) {
        String title = "TRASA LINII ";
        title += localLine;
        drawRoutePlaceholder(tft, title, "Ladowanie...");
        return;
    }

    if (!localHasData || localCount <= 0) {
        String title = "TRASA LINII";
        if (localLine.length() > 0 && localLine != "-") {
            title += " ";
            title += localLine;
        }
        drawRoutePlaceholder(tft, title, localStatus);
        return;
    }

    String currentTime;
    String currentDate;
    getCurrentTimeDate(currentTime, currentDate);
    drawRouteScreen(tft, localLine, localStops, localCount, currentTime, currentDate);

    selectDisplay(TFT_CS1_PIN);
}

bool fetchDeparturesFromApi(int limit) {
    String activeStopId;
    if (lockDepartures(pdMS_TO_TICKS(1500))) {
        activeStopId = g_activeStopId;
        unlockDepartures();
    } else {
        Serial.println("Failed to read the active stop.");
        return false;
    }

    if (activeStopId.length() == 0) {
        Serial.println("Active stop id is empty.");
        return false;
    }

    String url = String(API_BASE_URL) + "?id=" + activeStopId + "&limit=" + String(limit);
    Serial.print("GET ");
    Serial.println(url);

    WiFiClientSecure client;
    client.setInsecure(); // Certificate validation can be added later if the deployment needs it.

    HTTPClient http;
    if (!http.begin(client, url)) {
        Serial.println("HTTP begin failed.");
        return false;
    }

    http.setTimeout(12000);
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.print("HTTP code: ");
        Serial.println(httpCode);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();
    return parseDeparturesPayload(payload);
}

bool parseDeparturesPayload(const String& payload) {
    DynamicJsonDocument doc(16384);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.print("JSON error: ");
        Serial.println(error.c_str());
        return false;
    }

    if (!doc.is<JsonArray>()) {
        Serial.println("Unexpected API format: root is not an array.");
        return false;
    }

    JsonArray root = doc.as<JsonArray>();
    if (root.size() < 2 || !root[1].is<JsonArray>()) {
        Serial.println("Unexpected API format: departures array is missing.");
        return false;
    }

    JsonArray departuresRaw = root[1].as<JsonArray>();

    if (!lockDepartures(pdMS_TO_TICKS(200))) {
        Serial.println("Departures mutex is busy, skipping the refresh.");
        return false;
    }

    clearDepartures();

    int64_t nowMs = 0;
    if (isClockSynced()) {
        nowMs = static_cast<int64_t>(time(nullptr)) * 1000LL;
    }

    for (JsonVariant departureVariant : departuresRaw) {
        if (!departureVariant.is<JsonArray>()) {
            continue;
        }

        JsonArray departure = departureVariant.as<JsonArray>();

        String line = "-";
        if (departure.size() > 2) {
            if (departure[2].is<JsonArray>()) {
                JsonArray lineInfo = departure[2].as<JsonArray>();
                if (lineInfo.size() > 2) {
                    line = jsonToString(lineInfo[2]);
                }
                if ((line == "-" || line.length() == 0) && lineInfo.size() > 0) {
                    line = jsonToString(lineInfo[0]);
                }
            } else {
                line = jsonToString(departure[2]);
            }
        }

        String direction = "-";
        if (departure.size() > 1) {
            direction = normalizePolishText(jsonToString(departure[1]));
        }

        int64_t departureMs = 0;
        if (departure.size() > 7 && departure[7].is<JsonArray>()) {
            JsonArray timing = departure[7].as<JsonArray>();
            departureMs = extractRealtimeDepartureMs(timing);
        }

        if (departureMs <= 0) {
            continue;
        }

        int minutesToDeparture = -1;
        if (nowMs > 0) {
            int64_t deltaMs = departureMs - nowMs;
            if (deltaMs < 0) {
                deltaMs = 0;
            }
            minutesToDeparture = static_cast<int>((deltaMs + 59999LL) / 60000LL);
        }

        insertSortedDeparture(line, direction, departureMs, minutesToDeparture);
    }

    int parsed = countValidDeparturesUnlocked();
    unlockDepartures();

    Serial.print("Departures parsed: ");
    Serial.println(parsed);
    return parsed > 0;
}

int64_t extractRealtimeDepartureMs(JsonArray timing) {
    if (timing.size() > 1 && isNumericJson(timing[1])) {
        int64_t realtimeMs = jsonToInt64(timing[1]);
        if (realtimeMs > 0) {
            return realtimeMs;
        }
    }

    // Fall back to the planned departure only when realtime data is missing.
    if (timing.size() > 0 && isNumericJson(timing[0])) {
        int64_t plannedMs = jsonToInt64(timing[0]);
        if (plannedMs > 0) {
            return plannedMs;
        }
    }

    return 0;
}

void insertSortedDeparture(const String& line, const String& direction, int64_t departureMs, int minutesToDeparture) {
    int insertPos = API_LIMIT;
    for (int i = 0; i < API_LIMIT; i++) {
        if (!g_departures[i].valid || departureMs < g_departures[i].departureEpochMs) {
            insertPos = i;
            break;
        }
    }

    if (insertPos >= API_LIMIT) {
        return;
    }

    for (int i = API_LIMIT - 1; i > insertPos; i--) {
        g_departures[i] = g_departures[i - 1];
    }

    g_departures[insertPos].line = line;
    g_departures[insertPos].direction = direction;
    g_departures[insertPos].departureEpochMs = departureMs;
    g_departures[insertPos].minutesToDeparture = minutesToDeparture;
    g_departures[insertPos].valid = true;
}

void clearDepartures() {
    for (int i = 0; i < API_LIMIT; i++) {
        g_departures[i].line = "-";
        g_departures[i].direction = "-";
        g_departures[i].departureEpochMs = 0;
        g_departures[i].minutesToDeparture = -1;
        g_departures[i].valid = false;
    }
}

String jsonToString(JsonVariant value) {
    if (value.is<const char*>()) {
        const char* text = value.as<const char*>();
        return text != nullptr ? String(text) : String("-");
    }
    if (value.is<int>()) {
        return String(value.as<int>());
    }
    if (value.is<long>()) {
        return String(value.as<long>());
    }
    if (value.is<unsigned int>()) {
        return String(value.as<unsigned int>());
    }
    if (value.is<unsigned long>()) {
        return String(value.as<unsigned long>());
    }
    if (value.is<float>()) {
        return String(value.as<float>(), 0);
    }
    if (value.is<double>()) {
        return String(value.as<double>(), 0);
    }
    return "-";
}

int64_t jsonToInt64(JsonVariant value) {
    if (value.is<int64_t>()) {
        return value.as<int64_t>();
    }
    if (value.is<long>()) {
        return static_cast<int64_t>(value.as<long>());
    }
    if (value.is<int>()) {
        return static_cast<int64_t>(value.as<int>());
    }
    if (value.is<unsigned long>()) {
        return static_cast<int64_t>(value.as<unsigned long>());
    }
    if (value.is<float>()) {
        return static_cast<int64_t>(value.as<float>());
    }
    if (value.is<double>()) {
        return static_cast<int64_t>(value.as<double>());
    }
    if (value.is<const char*>()) {
        const char* text = value.as<const char*>();
        return text != nullptr ? atoll(text) : 0;
    }
    return 0;
}

bool isNumericJson(JsonVariant value) {
    return value.is<int64_t>() || value.is<long>() || value.is<int>() || value.is<unsigned long>() || value.is<float>() || value.is<double>();
}

String normalizePolishText(const String& text) {
    String normalized = text;

    // The default Adafruit_GFX font does not contain Polish glyphs, so map them to ASCII.
    normalized.replace("\xC4\x84", "A");
    normalized.replace("\xC4\x85", "a");
    normalized.replace("\xC4\x86", "C");
    normalized.replace("\xC4\x87", "c");
    normalized.replace("\xC4\x98", "E");
    normalized.replace("\xC4\x99", "e");
    normalized.replace("\xC5\x81", "L");
    normalized.replace("\xC5\x82", "l");
    normalized.replace("\xC5\x83", "N");
    normalized.replace("\xC5\x84", "n");
    normalized.replace("\xC3\x93", "O");
    normalized.replace("\xC3\xB3", "o");
    normalized.replace("\xC5\x9A", "S");
    normalized.replace("\xC5\x9B", "s");
    normalized.replace("\xC5\xB9", "Z");
    normalized.replace("\xC5\xBA", "z");
    normalized.replace("\xC5\xBB", "Z");
    normalized.replace("\xC5\xBC", "z");

    return normalized;
}

String minuteLabel(int minutesToDeparture) {
    if (minutesToDeparture < 0) {
        return "--";
    }
    if (minutesToDeparture == 0) {
        return "0 min";
    }
    return String(minutesToDeparture) + " min";
}

uint16_t departureColor(int minutesToDeparture) {
    if (minutesToDeparture < 0) {
        return COLOR_TEXT;
    }
    if (minutesToDeparture < 3) {
        return COLOR_RED;
    }
    if (minutesToDeparture <= 10) {
        return COLOR_ORANGE;
    }
    return COLOR_TEXT;
}

void drawRouteScreen(TFT_eSPI& tft, const String& lineNumber, String stops[], int numStops, String currentTime, String currentDate) {
    selectDisplay(TFT_CS2_PIN);
    tft.setRotation(ROT_ROUTE);
    tft.fillScreen(COLOR_BG);

    tft.setTextColor(COLOR_TEXT);
    tft.setTextSize(2);
    tft.setCursor(10, 10);
    tft.print("TRASA LINII ");
    tft.print(lineNumber);

    int maxCols = 5;
    int safeStops = max(numStops, 1);
    int numRows = (safeStops - 1) / maxCols + 1;

    const int routeTop = 58;
    const int routeBottomLimit = 230;
    int startY = routeTop;
    int stepY = 40;

    if (numRows <= 1) {
        startY = (routeTop + routeBottomLimit) / 2;
        stepY = 40;
    } else {
        int available = routeBottomLimit - routeTop - 18;
        if (available < 20) {
            available = 20;
        }
        stepY = available / (numRows - 1);
        if (stepY > 50) {
            stepY = 50;
        }
        if (stepY < 14) {
            stepY = 14;
        }
    }

    drawDynamicRoute(tft, stops, numStops, startY, stepY, maxCols);
}

void drawDynamicRoute(TFT_eSPI& tft, String stops[], int numStops, int startY, int stepY, int maxCols) {
    selectDisplay(TFT_CS2_PIN);
    tft.setRotation(ROT_ROUTE);
    // Leave extra space on the left so the first stop label stays visible.
    int startX = 35;
    int stepX = 60;

    int prevX = -1, prevY = -1;
    const bool denseLayout = stepY < 26;

    for (int i = 0; i < numStops; i++) {
        int row = i / maxCols;
        int col = i % maxCols;
        if (row % 2 != 0) col = (maxCols - 1) - col;

        int x = startX + (col * stepX);
        int y = startY + (row * stepY);

        if (i > 0) {
            tft.drawLine(prevX, prevY, x, y, COLOR_ROUTE);
            tft.drawLine(prevX, prevY-1, x, y-1, COLOR_ROUTE);
            tft.drawLine(prevX, prevY+1, x, y+1, COLOR_ROUTE);
        }
        prevX = x;
        prevY = y;
    }

    for (int i = 0; i < numStops; i++) {
        int row = i / maxCols;
        int col = i % maxCols;
        if (row % 2 != 0) col = (maxCols - 1) - col;

        int x = startX + (col * stepX);
        int y = startY + (row * stepY);

        tft.fillCircle(x, y, denseLayout ? 3 : 5, COLOR_ROUTE);

        tft.setTextSize(1);
        tft.setTextColor(COLOR_TEXT);
        
        String stopName = stops[i];
        int maxLen = denseLayout ? 7 : 9;
        if (stopName.length() > maxLen) {
            stopName = stopName.substring(0, maxLen - 1) + ".";
        }

        int textX = 0;
        int textY = 0;
        if (denseLayout) {
            const int textW = static_cast<int>(stopName.length()) * 6;
            const bool snakeGoesRight = (row % 2 == 0);
            if (snakeGoesRight) {
                textX = x + 8;
            } else {
                textX = x - 8 - textW;
            }
            textY = y - 4;
        } else {
            textX = x - (stopName.length() * 3);
            if (textX < 5) textX = 5;
            textY = (i % 2 == 0) ? (y - 12) : (y + 10);
        }

        if (textX < 0) textX = 0;
        if (textX > tft.width() - 1) textX = tft.width() - 1;
        if (textY < 0) textY = 0;
        if (textY > tft.height() - 8) textY = tft.height() - 8;
        
        tft.setCursor(textX, textY);
        tft.print(stopName);
    }
}
