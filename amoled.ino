/*
 * RCM Injector for Waveshare Amoled touch 1.8 v1
*/

//#define Serial Serial1
//#define DEBUG_SERIAL

//since boards 3.3.11 breaks usb injection add this, remove this code once the esp-idf code is fixed by the devs
//the line below can be 0 on boards lower than 3.3.11 but setting to 1 is backwards compatible.
#define WITH_ENUM_FILTER_WORKAROUND 1

#include <Arduino.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <vector>
#define CONFIG_ASYNC_TCP_STACK_SIZE 4096
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <FFat.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include "include/ESPmDNS.h"
#include <cctype>
#include <esp32-hal-psram.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include <esp_cpu.h>
#include "esp_task_wdt.h"
#include "esp_vfs_fat.h"
//
#include "soc/rtc_cntl_reg.h"   // or use the raw address
//
#include "include/index_gz.h"
#include "include/editor_gz.h"
#include "include/config.h"
#include "include/firmware_update_html_gz.h"
#include "include/tar_gz.h"
#include "include/css_gz.h"
#include "include/info_gz.h"
#include "include/ESPWebFileManager.h"
#include "include/SimpleFTPServer.h"
#include "USB.h"
#include "USBMSC.h"

// ==================== POPCORN UI/AUDIO INCLUDES ====================
// power/battery support for the Waveshare ESP32-S3 AMOLED touchscreen v1.
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "audio_engine.h"
#include "qmi8658.h"
#include "pin_config.h"
#include <Adafruit_XCA9554.h>
#include <math.h>
#include "XPowersLib.h"

// === Bluetooth ===
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

struct BatteryReading readBattery();

BLECharacteristic* ipCharacteristic;
BLECharacteristic* commandCharacteristic;
BLECharacteristic* responseCharacteristic;
#define SERVICE_UUID "8f41ca5d-f679-45bb-a603-9bc1c5eedefc"
#define CHARACTERISTIC_UUID "432954d8-eef6-41b4-9686-749f35e6fa6d"
#define COMMAND_UUID   "0000abcd-0000-1000-8000-00805f9b34fb"
#define RESPONSE_UUID  "0000dcba-0000-1000-8000-00805f9b34fb"
static bool listInProgress = false;
static unsigned long lastCommandTime = 0;
const unsigned long COMMAND_DEBOUNCE_MS = 400;  // 400ms debounce

// ==================== RCM INJECTOR INCLUDES ====================
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "usb/usb_host.h"
#include "wear_levelling.h"

// ==================== RCM INJECTOR DEFINES ====================
#define TAG "RCM_INJECTOR"
#define APX_VID 0x0955
#define APX_PID 0x7321
#define MAX_LENGTH 0x30298
#define RCM_PAYLOAD_ADDR 0x40010000
#define INTERMEZZO_LOCATION 0x4001F000
#define PAYLOAD_LOAD_BLOCK 0x40020000
#define SEND_CHUNK_SIZE 0x1000

// RCM Logging macros (redirect to Serial)
#define RCM_LOG_I(fmt, ...) Serial.printf("[RCM][INFO] " fmt "\n", ##__VA_ARGS__)
#define RCM_LOG_E(fmt, ...) Serial.printf("[RCM][ERROR] " fmt "\n", ##__VA_ARGS__)
#define RCM_LOG_W(fmt, ...) Serial.printf("[RCM][WARN] " fmt "\n", ##__VA_ARGS__)

// Intermezzo binary (required for RCM exploit)
static const uint8_t intermezzo_bin[] PROGMEM = {
	0x44, 0x00, 0x9F, 0xE5, 0x01, 0x11, 0xA0, 0xE3, 0x40, 0x20, 0x9F, 0xE5, 0x00, 0x20, 0x42, 0xE0,
	0x08, 0x00, 0x00, 0xEB, 0x01, 0x01, 0xA0, 0xE3, 0x10, 0xFF, 0x2F, 0xE1, 0x00, 0x00, 0xA0, 0xE1,
	0x2C, 0x00, 0x9F, 0xE5, 0x2C, 0x10, 0x9F, 0xE5, 0x02, 0x28, 0xA0, 0xE3, 0x01, 0x00, 0x00, 0xEB,
	0x20, 0x00, 0x9F, 0xE5, 0x10, 0xFF, 0x2F, 0xE1, 0x04, 0x30, 0x90, 0xE4, 0x04, 0x30, 0x81, 0xE4,
	0x04, 0x20, 0x52, 0xE2, 0xFB, 0xFF, 0xFF, 0x1A, 0x1E, 0xFF, 0x2F, 0xE1, 0x20, 0xF0, 0x01, 0x40,
	0x5C, 0xF0, 0x01, 0x40, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00, 0x01, 0x40
};

// ==================== RCM INJECTOR GLOBALS ====================
static usb_host_client_handle_t rcm_client_hdl = NULL;
static usb_device_handle_t rcm_dev_hdl = NULL;
static volatile bool rcm_device_connected = false;
static volatile bool rcm_injection_done = false;
static volatile bool rcm_injection_active = false;  // prevents multiple injections
static TaskHandle_t rcm_usb_task_handle = NULL;
static TaskHandle_t rcm_injection_task_handle = NULL;
static bool injectionWifiWasActive = false;
static bool injectionMdnsWasActive = false;
static bool injectionBleWasActive = false;
static bool bluetoothConfigEnabled = false;
// ==================== GLOBALS ====================
// This board (Waveshare ESP32-S3 AMOLED 1.8 v1) doesn't wire a CS pin to the
// microSD slot - the card sits on the ESP32-S3's 1-bit SDMMC bus instead
// (CLK/CMD/DATA0). Using SD_MMC here instead of the SPI-based SD library.
// #define SD SD_MMC lets every existing "SD.xxx" call below keep working
// unchanged, since SD_MMC exposes the same FS-derived API as SD.
#include <SD_MMC.h>
#define SD SD_MMC

// Pin numbers per Waveshare's official pin_config.h for this board. If your
// project's pin_config.h already defines these (it's included above via
// "include/config.h" / pin_config.h in some sketch variants), these guarded
// defines are skipped and yours are used instead.
#ifndef SDMMC_CLK
#define SDMMC_CLK 2
#endif
#ifndef SDMMC_CMD
#define SDMMC_CMD 1
#endif
#ifndef SDMMC_DATA
#define SDMMC_DATA 3
#endif

// EXIO7 on the onboard XCA9554 IO expander gates power/select to the microSD
// slot on this board - it has to be driven HIGH before SD_MMC.begin() will
// find a card, even though it isn't a SPI CS pin.
// Set to true temporarily to let SD_MMC auto-format the card if it can't be
// mounted (e.g. it's exFAT, has no filesystem, or is corrupted). This WIPES
// the card. Use it once to test, then set back to false.
#define SD_ENABLE_EXPANDER_PIN 7
#define SD_FORMAT_IF_MOUNT_FAILED false

bool sdMounted = false;

//Stop UI
bool multimedia = true;
bool stopui = false;

String firmwareVersion = "v1.0.0";
bool mdnsRunning = false;
unsigned int bootTime = 0;
uint16_t TIME2SLEEP = 10;
bool autosleep = false;
bool preOpAutosleep = false;

bool removeconf = false;

TaskHandle_t ftpTaskHandle = NULL;
TaskHandle_t dnsTaskHandle = NULL;
TaskHandle_t otaTaskHandle = NULL;

USBMSC dev;

const char* ALLOWED_EXTENSIONS[] PROGMEM = { "txt", "html", "js", "mjs", "css", "cache" };
const int ALLOWED_EXT_COUNT = sizeof ALLOWED_EXTENSIONS / sizeof ALLOWED_EXTENSIONS[0];

WiFiClient otaClient;
HTTPClient otaHttp;
WiFiClient* otaStream = nullptr;
WiFiClientSecure otaSecureClient;
WiFiClient otaInsecureClient;

int otaContentLength = 0;
int otaTotalWritten = 0;
bool otaInProgress = false;
String ota_url = "";
bool doOta = false;
AsyncEventSource otaEvents("/ota-progress");
volatile const char* formatStatus = "IDLE";

IPAddress Local_IP(192, 168, 0, 1);
IPAddress Gateway(192, 168, 0, 1);
IPAddress Subnet_Mask(255, 255, 255, 0);
String ipStr;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
bool hasIndexFile = false;
bool AP_Running = false;
bool activeOperation = false;

#define FILESYS FFat
ESPWebFileManager fileManager;

DNSServer dnsServer;
FtpServer ftpSrv;
bool allowFTP = false;

size_t totalPSRAM = 0;
size_t availablePSRAM = 0;
bool psramAvailable = false;
bool monitorPSRAM = false;

char* fileChunk = nullptr;
size_t currentChunk = 0;
size_t totalChunks = 0;
size_t fileSize = 0;
String currentFilename;

int megabytes = 8;
int bytes = megabytes * 1024 * 1024;
size_t MAX_FILE_SIZE = 0;
#define CHUNK_SIZE 32768

enum DownloadState {
	IDLE,
	DOWNLOADING,
	EXTRACTING,
	COMPLETE,
	ERROR
};

volatile DownloadState currentState = IDLE;
String downloadUrl = "";
uint8_t* tar_data = nullptr;
size_t tar_size = 0;

volatile size_t download_progress = 0;
size_t download_total = 0;
size_t extracted_files = 0;
size_t total_files = 0;
String current_file = "";

// ==================== POPCORN DISPLAY / AUDIO / BUTTONS (from popcorn.ino) ====================
HWCDC USBSerial;

// ---------------- DISPLAY ----------------
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_SH8601 *gfx = new Arduino_SH8601(bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT);

// ---------------- TOUCH ----------------
std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus = std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);
void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(IIC_Bus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT, Arduino_IIC_Touch_Interrupt));

Adafruit_XCA9554 expander;

// ---------------- POWER / BATTERY ----------------
XPowersPMU power;
bool pmuOk = false;

// ---------------- SCREEN DIMENSIONS ----------------
#define PHYSICAL_WIDTH  368
#define PHYSICAL_HEIGHT 448
#define SCREEN_WIDTH    448
#define SCREEN_HEIGHT   368
#define CENTER_X        (SCREEN_WIDTH / 2)
#define CENTER_Y        (SCREEN_HEIGHT / 2)

// ---------------- STARFIELD ----------------
#define NUM_STARS       100
#define MAX_DEPTH       1000
#define MIN_DEPTH       10

struct Star {
  int16_t x, y, z;
  uint8_t brightness;
} stars[NUM_STARS];

uint16_t *starBuffer = (uint16_t*)ps_malloc(SCREEN_WIDTH * SCREEN_HEIGHT * 2);

// ---------------- 3D ROTATING CUBE ----------------
#define CUBE_SIZE 60
#define CUBE_CENTER_Z 200

float cubeVertices[8][3] = {
  {-CUBE_SIZE, -CUBE_SIZE, -CUBE_SIZE}, {CUBE_SIZE, -CUBE_SIZE, -CUBE_SIZE},
  {CUBE_SIZE, CUBE_SIZE, -CUBE_SIZE}, {-CUBE_SIZE, CUBE_SIZE, -CUBE_SIZE},
  {-CUBE_SIZE, -CUBE_SIZE, CUBE_SIZE}, {CUBE_SIZE, -CUBE_SIZE, CUBE_SIZE},
  {CUBE_SIZE, CUBE_SIZE, CUBE_SIZE}, {-CUBE_SIZE, CUBE_SIZE, CUBE_SIZE}
};

int cubeEdges[12][2] = {
  {0,1}, {1,2}, {2,3}, {3,0},
  {4,5}, {5,6}, {6,7}, {7,4},
  {0,4}, {1,5}, {2,6}, {3,7}
};

float rotX = 0, rotY = 0, rotZ = 0;
float cubeSpeedX = 0.02f;
float cubeSpeedY = 0.03f;
float cubeSpeedZ = 0.01f;

// ---------------- SINUS SCROLLER ----------------
#define FONT_WIDTH      32
#define FONT_HEIGHT     32
#define BASE_FONT_WIDTH 32
#define BASE_FONT_HEIGHT 32
#define SCROLL_SPEED    5
#define SINE_AMPLITUDE  30
#define SINE_FREQUENCY  0.2f

float fontScale = 1.1f;
const float MIN_SCALE = 0.5f;
const float MAX_SCALE = 2.5f;
const float SCALE_STEP = 0.1f;

String mainmessage;

const char *scrollTextOriginal;

char batteryStatusText[160] = "READING BATTERY *** ";

// Pointer to whichever string is currently being scrolled. Touch toggles this.
const char *scrollText;
bool showAltText = false;   // true = show battery info
unsigned long lastBatteryUpdate = 0;
#define BATTERY_UPDATE_INTERVAL_MS 1000

// ---------------- PAYLOAD CYCLE STATUS TEXT ----------------
// Briefly shown on screen (borrowing the same scroller) after a BOOT-button
// hold cycles to the next payload, then reverts to whatever was showing before.
char payloadStatusText[160] = "";
bool showingPayloadStatus = false;
unsigned long payloadStatusShownAt = 0;
#define PAYLOAD_STATUS_DISPLAY_MS 120000   // how long to show "PAYLOAD: x.bin" before reverting

// Live feedback shown WHILE the BOOT button is still held down, so you know
// what will happen before you let go (cycle payload vs. factory reset).
enum BootHoldFeedbackState { BOOT_HOLD_NONE = 0, BOOT_HOLD_CYCLE = 1, BOOT_HOLD_RESET = 2 };
char bootHoldText[160] = "";
bool showingBootHoldFeedback = false;
BootHoldFeedbackState bootHoldFeedbackState = BOOT_HOLD_NONE;
int scrollX = SCREEN_WIDTH;
float sineOffset = 0;
void setScrollText(const char* text, int startX = SCREEN_WIDTH);

// Digital-7 font 32x32 arrays -- lives in its own PROGMEM header, see font.h
#include "font.h"

int8_t sinTable[256];

volatile bool touchInterruptFlag = false;
bool touchActive = false;
unsigned long lastTouch = 0;

// Ignore any touch events (including spurious ones fired by the touch
// controller as it powers up) until this time has passed, so a phantom
// touch at boot can't flip showAltText and cause the IP text to briefly
// flash before switching to battery info.
#define TOUCH_BOOT_GUARD_MS 1500
unsigned long touchGuardUntil = 0;

// ---------------- DOUBLE-TAP (screen on/off) ----------------
// A single tap is held "pending" briefly to see if a second tap follows;
// if it does within the window, it's treated as a double-tap and toggles
// the screen (like handleBootPress()) instead of the text.
#define DOUBLE_TAP_WINDOW_MS 800   // max gap between two taps to count as a double-tap
bool tapPending = false;
unsigned long tapPendingSince = 0;

// ---------------- BUTTONS ----------------
// PWR button lives on the XCA9554 IO expander, pin 4, active HIGH.
// BOOT button is on GPIO0, active LOW (internal pull-up).
#define PWR_BUTTON_EXPANDER_PIN 4
#define BOOT_BUTTON_PIN         0

#define BUTTON_DEBOUNCE_MS   30    // time a reading must be stable before it's trusted
#define PWR_LONG_PRESS_MS    800   // hold longer than this on PWR = power off
#define BOOT_CYCLE_PAYLOAD_MS 3000   // hold BOOT this long (but less than the reset hold) to cycle to the next payload
#define RESET_LONG_PRESS_MS    10000   // hold longer than this - remove config.json

// Generic non-blocking debounced button state, sampled once per loop().
struct DebouncedButton {
  bool rawState;            // last raw pin reading
  bool stableState;         // debounced, "official" state
  unsigned long lastEdgeMs; // time rawState last changed (for debounce window)
  unsigned long pressStartMs;
  bool activeHigh;          // true = pressed reads HIGH, false = pressed reads LOW
  bool armed;                // false until we've seen this button at rest at least once
};

DebouncedButton pwrButton  = { false, false, 0, 0, true };   // expander pin 4, active HIGH
DebouncedButton bootButton = { true,  true,  0, 0, false };  // GPIO0, active LOW (idle HIGH)

bool screenOn   = true;
uint8_t savedBrightness = 255;

bool updateButton(DebouncedButton &btn, bool rawRead, bool &pressedEdge, unsigned long &releaseDuration) {
  unsigned long now = millis();

  if (rawRead != btn.rawState) {
    btn.rawState = rawRead;
    btn.lastEdgeMs = now;
  }

  if ((now - btn.lastEdgeMs) >= BUTTON_DEBOUNCE_MS && btn.rawState != btn.stableState) {
    btn.stableState = btn.rawState;
    bool nowPressed = btn.activeHigh ? btn.stableState : !btn.stableState;
    
    if (!btn.armed) {
      if (!nowPressed) {
        btn.armed = true;
      }
      return false;
    }

    if (nowPressed) {
      btn.pressStartMs = now;
      pressedEdge = true;
      return true;
    } else {
      releaseDuration = now - btn.pressStartMs;
      pressedEdge = false;
      return true;
    }
  }

  return false;
}

void initStars() {
  for (int i = 0; i < NUM_STARS; i++) {
    stars[i].x = random(-CENTER_X * 2, CENTER_X * 2);
    stars[i].y = random(-CENTER_Y * 2, CENTER_Y * 2);
    stars[i].z = random(MIN_DEPTH, MAX_DEPTH);
    stars[i].brightness = random(50, 255);
  }
}

void buildSinTable() {
  for (int i = 0; i < 256; i++) {
    sinTable[i] = (int8_t)(sin(i * 0.0245436926) * 127);
  }
}

int getSine(int angle) {
  return sinTable[angle & 255];
}

void drawCharScaled(int x, int y, char c, uint16_t color, float scale) {
  int idx;
  
  if (c == '%') idx = 0;
  else if (c == '*') idx = 1;
  else if (c == '?') idx = 2;
  else if (c == ',') idx = 3;
  else if (c == '-') idx = 4;
  else if (c == '.') idx = 5;
  else if (c == '=') idx = 6;
  else if (c == '+') idx = 7;
  else if (c == '$') idx = 8;
  else if (c == '#') idx = 9;
  else if (c == '@') idx = 10;
  else if (c == ':') idx = 11;
  else if (c == '!') idx = 12;
  else if (c == '|') idx = 13;
  else if (c == '[') idx = 14;
  else if (c == ']') idx = 15;
  else if (c == '{') idx = 16;
  else if (c == '}') idx = 17;
  else if (c == '<') idx = 18;
  else if (c == '>') idx = 19;
  else if (c >= '0' && c <= '9') idx = c - '0' + 20;
  else if (c >= 'A' && c <= 'Z') idx = c - 'A' + 30;
  else if (c >= 'a' && c <= 'z') idx = c - 'a' + 30;
  else if (c == ' ') return;
  else idx = 4; // unknown char -> DASH, same fallback convention as before
  
  int scaledWidth = (int)(BASE_FONT_WIDTH * scale);
  int scaledHeight = (int)(BASE_FONT_HEIGHT * scale);
  
  if (x < -scaledWidth || x > SCREEN_WIDTH || y < -scaledHeight || y > SCREEN_HEIGHT) {
    return;
  }
  
  const uint32_t *charData = font32x32[idx];
  
  for (int row = 0; row < BASE_FONT_HEIGHT; row++) {
    uint32_t rowData = charData[row];
    if (rowData == 0) continue;
    
    int scaledYStart = y + (int)(row * scale);
    int scaledYEnd = y + (int)((row + 1) * scale);
    
    for (int col = 0; col < BASE_FONT_WIDTH; col++) {
      if (rowData & (1UL << (31 - col))) {
        int scaledXStart = x + (int)(col * scale);
        int scaledXEnd = x + (int)((col + 1) * scale);
        
        for (int py = scaledYStart; py < scaledYEnd; py++) {
          if (py < 0 || py >= SCREEN_HEIGHT) continue;
          for (int px = scaledXStart; px < scaledXEnd; px++) {
            if (px >= 0 && px < SCREEN_WIDTH) {
              starBuffer[py * SCREEN_WIDTH + px] = color;
            }
          }
        }
      }
    }
  }
}

// 3D Cube Functions
void rotatePoint(float point[3], float rx, float ry, float rz, float result[3]) {
  float x = point[0], y = point[1], z = point[2];
  
  float y1 = y * cos(rx) - z * sin(rx);
  float z1 = y * sin(rx) + z * cos(rx);
  y = y1; z = z1;
  
  float x2 = x * cos(ry) + z * sin(ry);
  float z2 = -x * sin(ry) + z * cos(ry);
  x = x2; z = z2;
  
  float x3 = x * cos(rz) - y * sin(rz);
  float y3 = x * sin(rz) + y * cos(rz);
  x = x3; y = y3;
  
  result[0] = x;
  result[1] = y;
  result[2] = z;
}

void projectPoint(float point[3], int &x, int &y, float &depth) {
  float perspective = 300.0f / (300.0f + point[2] + CUBE_CENTER_Z);
  x = CENTER_X + (int)(point[0] * perspective);
  y = CENTER_Y + (int)(point[1] * perspective);
  depth = point[2] + CUBE_CENTER_Z;
}

void drawLine(int x0, int y0, int x1, int y1, uint16_t color, float brightness) {
  uint8_t r = ((color >> 11) & 0x1F) * brightness;
  uint8_t g = ((color >> 5) & 0x3F) * brightness;
  uint8_t b = (color & 0x1F) * brightness;
  uint16_t fadedColor = (r << 11) | (g << 5) | b;
  
  int dx = abs(x1 - x0);
  int dy = abs(y1 - y0);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx - dy;
  
  while (true) {
    if (x0 >= 0 && x0 < SCREEN_WIDTH && y0 >= 0 && y0 < SCREEN_HEIGHT) {
      starBuffer[y0 * SCREEN_WIDTH + x0] = fadedColor;
    }
    
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void drawCube() {
  float rotated[8][3];
  int projected[8][2];
  float depths[8];
  
  for (int i = 0; i < 8; i++) {
    rotatePoint(cubeVertices[i], rotX, rotY, rotZ, rotated[i]);
    projectPoint(rotated[i], projected[i][0], projected[i][1], depths[i]);
  }
  
  uint16_t cubeColor = gfx->color565(0, 255, 255);
  
  for (int i = 0; i < 12; i++) {
    int v1 = cubeEdges[i][0];
    int v2 = cubeEdges[i][1];
    
    float avgDepth = (depths[v1] + depths[v2]) / 2.0f;
    float brightness = 1.0f - (avgDepth / 400.0f);
    if (brightness < 0.3f) brightness = 0.3f;
    if (brightness > 1.0f) brightness = 1.0f;
    
    drawLine(projected[v1][0], projected[v1][1], 
             projected[v2][0], projected[v2][1], 
             cubeColor, brightness);
  }
  
  rotX += cubeSpeedX;
  rotY += cubeSpeedY;
  rotZ += cubeSpeedZ;
  
  if (rotX > 2 * PI) rotX -= 2 * PI;
  if (rotY > 2 * PI) rotY -= 2 * PI;
  if (rotZ > 2 * PI) rotZ -= 2 * PI;
}

void setScrollText(const char* text, int startX) {
  scrollText = text;
  scrollX = startX;
}

void drawSinusScroller() {
  int textLen = strlen(scrollText);
  int scaledWidth = (int)(BASE_FONT_WIDTH * fontScale);
  int scaledHeight = (int)(BASE_FONT_HEIGHT * fontScale);
  int charSpacing = (int)(8 * fontScale);
  
  int safeTop = 0;
  int safeBottom = SCREEN_HEIGHT;
  int safeCenterY = (safeTop + safeBottom) / 2;
  int safeAmplitude = min(SINE_AMPLITUDE, (safeBottom - safeTop - scaledHeight) / 2);
  
  for (int i = 0; i < textLen; i++) {
    int charX = scrollX + (i * (scaledWidth + charSpacing));
    int angle = (int)(sineOffset * 10) + (i * 25);
    int sineY = getSine(angle);
    int charY = safeCenterY + (sineY * safeAmplitude / 128) - (scaledHeight / 2);
    
    uint8_t r = 128 + getSine(angle);
    uint8_t g = 128 + getSine(angle + 85);
    uint8_t b = 128 + getSine(angle + 170);
    uint16_t color = gfx->color565(r, g, b);
    
    drawCharScaled(charX, charY, scrollText[i], color, fontScale);
  }
  
  scrollX -= SCROLL_SPEED;
  
  int totalWidth = textLen * (scaledWidth + charSpacing);
  if (scrollX < -totalWidth) {
    scrollX = SCREEN_WIDTH;
  }
  
  sineOffset += SINE_FREQUENCY;
}

void rotateBufferToDisplay() {
  static uint16_t *rotatedBuffer = (uint16_t*)ps_malloc(PHYSICAL_WIDTH * PHYSICAL_HEIGHT * 2);
  
  for (int y = 0; y < SCREEN_HEIGHT; y++) {
    for (int x = 0; x < SCREEN_WIDTH; x++) {
      int physX = y;
      int physY = (SCREEN_WIDTH - 1) - x;
      rotatedBuffer[physY * PHYSICAL_WIDTH + physX] = starBuffer[y * SCREEN_WIDTH + x];
    }
  }
  
  gfx->draw16bitRGBBitmap(0, 0, rotatedBuffer, PHYSICAL_WIDTH, PHYSICAL_HEIGHT);
}

void updateAndDrawStars() {
  memset(starBuffer, 0, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
  
  for (int i = 0; i < NUM_STARS; i++) {
    stars[i].z -= 4;
    
    if (stars[i].z < MIN_DEPTH) {
      stars[i].x = random(-CENTER_X * 2, CENTER_X * 2);
      stars[i].y = random(-CENTER_Y * 2, CENTER_Y * 2);
      stars[i].z = MAX_DEPTH;
      stars[i].brightness = random(50, 255);
    }
    
    int scale = 256;
    int16_t sx = CENTER_X + (stars[i].x * scale / stars[i].z);
    int16_t sy = CENTER_Y + (stars[i].y * scale / stars[i].z);
    
    if (sx >= 0 && sx < SCREEN_WIDTH && sy >= 0 && sy < SCREEN_HEIGHT) {
      uint8_t brightness = stars[i].brightness * (MAX_DEPTH - stars[i].z) / MAX_DEPTH;
      if (brightness < 50) brightness = 50;
      if (brightness > 255) brightness = 255;
      
      uint16_t color = gfx->color565(brightness, brightness, brightness);
      
      int size = (MAX_DEPTH - stars[i].z) / 200 + 1;
      if (size > 3) size = 3;
      
      for (int dy = 0; dy < size; dy++) {
        for (int dx = 0; dx < size; dx++) {
          int px = sx + dx;
          int py = sy + dy;
          if (px < SCREEN_WIDTH && py < SCREEN_HEIGHT) {
            starBuffer[py * SCREEN_WIDTH + px] = color;
          }
        }
      }
    }
  }
  
  // Draw 3D cube
  drawCube();
  
  // Draw scroller on top
  drawSinusScroller();
  
  rotateBufferToDisplay();
}

void Arduino_IIC_Touch_Interrupt(void) {
  FT3168->IIC_Interrupt_Flag = true;
  touchInterruptFlag = true;
}

// ---------------- BUTTON ACTIONS ----------------
void powerOffDevice() {
  //USBSerial.println("[PWR] Powering off...");
  multimedia = false;
  Wire.beginTransmission(0x34);
  Wire.write(0x10); // REG_OFF_CTL
  Wire.write(0x01); // Power off bit
  Wire.endTransmission();
}

void handlePwrShortPress() {
  toggleAudioPlayback();
  //USBSerial.printf("[PWR] Short press -> audio %s\n", isAudioPlaying() ? "ON" : "OFF");
}

void handlePwrLongPress() {
  //USBSerial.println("[PWR] Long press -> power off");
  powerOffDevice();
}

void handleBootPress() {
  screenOn = !screenOn;
  if (screenOn) {
    gfx->setBrightness(savedBrightness);
  } else {
    gfx->setBrightness(0);
  }
  //USBSerial.printf("[BOOT] Short press -> screen %s\n", screenOn ? "ON" : "OFF");
}

struct BatteryReading {
  bool pmuOk;
  bool connected;
  int percent;
  uint16_t voltageMv;
  bool charging;
  bool vbusIn;
};

BatteryReading readBattery() {
  BatteryReading r{};
  r.pmuOk = pmuOk;
  if (!pmuOk) return r;

  r.connected = power.isBatteryConnect();
  r.charging = power.isCharging();
  r.vbusIn = power.isVbusIn();

  if (r.connected) {
    int pct = power.getBatteryPercent();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    r.percent = pct;
    r.voltageMv = power.getBattVoltage();
  }
  return r;
}

void updateBatteryStatusText() {
  BatteryReading r = readBattery();

  if (!r.pmuOk) {
    snprintf(batteryStatusText, sizeof(batteryStatusText), "PMU NOT FOUND *** ");
    return;
  }

  if (!r.connected) {
    snprintf(batteryStatusText, sizeof(batteryStatusText),
             "NO BATTERY CONNECTED *** ");
    return;
  }

  const char *chargeState;
  if (r.charging) chargeState = "CHARGING";
  else if (r.vbusIn) chargeState = "CHARGER CONNECTED";
  else chargeState = "ON BATTERY";

  snprintf(batteryStatusText, sizeof(batteryStatusText),
           "BATTERY %d%% - %u MV - %s ",
           r.percent, (unsigned)r.voltageMv, chargeState);
}

// Scans /payloads for .bin files (sorted alphabetically) so a long BOOT
// press can step through them, wrapping around at the end of the list.
std::vector<String> listPayloadBinFiles() {
	std::vector<String> files;
	File root = sdMounted ? SD.open("/payloads") : FILESYS.open("/payloads");
	if (!root || !root.isDirectory()) {
		if (root) root.close();
		return files;
	}

	File file = root.openNextFile();
	while (file) {
		if (!file.isDirectory()) {
			String name = String(file.name());
			int slash = name.lastIndexOf('/');
			if (slash >= 0) name = name.substring(slash + 1);   // some FS return a full path
			if (name.endsWith(".bin")) files.push_back(name);
		}
		file = root.openNextFile();
	}
	root.close();

	std::sort(files.begin(), files.end(), [](const String &a, const String &b) {
		return a < b;
	});
	return files;
}

// Poll both buttons; called once per loop(), never blocks.
void pollButtons() {
  bool pwrEdgePressed;
  unsigned long pwrReleaseDuration;
  bool pwrRaw = expander.digitalRead(PWR_BUTTON_EXPANDER_PIN);
  if (updateButton(pwrButton, pwrRaw, pwrEdgePressed, pwrReleaseDuration)) {
    if (!pwrEdgePressed) { // release edge -> classify short vs long
      if (pwrReleaseDuration >= PWR_LONG_PRESS_MS) {
        handlePwrLongPress();
      } else {
        handlePwrShortPress();
      }
    }
  }

  bool bootEdgePressed;
  unsigned long bootReleaseDuration;
  bool bootRaw = digitalRead(BOOT_BUTTON_PIN);
  if (updateButton(bootButton, bootRaw, bootEdgePressed, bootReleaseDuration)) {
    if (!bootEdgePressed) {
    	// Release edge -> commit whatever action the hold duration earned.
    	// (Live feedback below already told the user what this would do.)
    	if (bootReleaseDuration >= RESET_LONG_PRESS_MS) {
        removeconf = true; //hold for 10 seconds
      } else if (bootReleaseDuration >= BOOT_CYCLE_PAYLOAD_MS) {
        cyclePayload(); //hold for 3+ seconds (but under the reset hold) to switch payloads
      } else {
        handleBootPress();
      }
    }
  }

  // Live feedback while BOOT is currently held down: once the hold crosses a
  // threshold, swap the scroller to say what releasing NOW will do. Only
  // recompute when the threshold changes, not every loop iteration.
  bool bootCurrentlyHeld = bootButton.armed &&
      (bootButton.activeHigh ? bootButton.stableState : !bootButton.stableState);

  if (bootCurrentlyHeld) {
    unsigned long heldMs = millis() - bootButton.pressStartMs;

    if (heldMs >= RESET_LONG_PRESS_MS) {
      if (bootHoldFeedbackState != BOOT_HOLD_RESET) {
        snprintf(bootHoldText, sizeof(bootHoldText), "FACTORY");
        setScrollText(bootHoldText, 88); //112
        showingBootHoldFeedback = true;
        bootHoldFeedbackState = BOOT_HOLD_RESET;
      }
    } else if (heldMs >= BOOT_CYCLE_PAYLOAD_MS) {
      if (bootHoldFeedbackState != BOOT_HOLD_CYCLE) {
        // Preview which payload releasing now would select, without writing anything yet.
        std::vector<String> files = listPayloadBinFiles();
        String preview = files.empty() ? "NO PAYLOADS FOUND"
                                        : files[(getCurrentPayloadIndex(files) + 1) % (int)files.size()];
        //snprintf(bootHoldText, sizeof(bootHoldText), "RELEASE FOR: %s *** ", preview.c_str());
				snprintf(bootHoldText, sizeof(bootHoldText), "RELEASE");
        setScrollText(bootHoldText, 88); //7 = number of letters in RELEASE
        showingBootHoldFeedback = true;
        bootHoldFeedbackState = BOOT_HOLD_CYCLE;
      }
    }
    // heldMs < BOOT_CYCLE_PAYLOAD_MS: too short to act on yet, leave the display alone.
  } else if (showingBootHoldFeedback) {
    showingBootHoldFeedback = false;
    bootHoldFeedbackState = BOOT_HOLD_NONE;
    // Don't stomp on the post-release payload confirmation set by cyclePayload() above.
    if (!showingPayloadStatus) {
      setScrollText(showAltText ? batteryStatusText : scrollTextOriginal);
    }
  }
}

// ==================== END POPCORN BLOCK ====================

// ==================== RCM INJECTOR FUNCTIONS ====================
// Dummy callback - required but we don't rely on it
static void rcm_dummy_cb(usb_transfer_t* transfer) {}

void cleanup_rcm_tasks() {
    if (rcm_injection_active) {
        // Wait a bit if injection is still happening
        while (rcm_injection_active) vTaskDelay(pdMS_TO_TICKS(10));
    }

		#ifdef DEBUG_SERIAL
    Serial.println("Cleaning up RCM USB tasks...");
		#endif

    // Properly uninstall USB host first
    if (rcm_client_hdl) {
        usb_host_client_deregister(rcm_client_hdl);
        rcm_client_hdl = NULL;
    }

    // Uninstall the USB host library
    usb_host_uninstall();

		// Now safe to delete tasks
    if (rcm_usb_task_handle) {
        vTaskDelete(rcm_usb_task_handle);
        rcm_usb_task_handle = NULL;
    }
		
    if (rcm_injection_task_handle) {
        vTaskDelete(rcm_injection_task_handle);
        rcm_injection_task_handle = NULL;
    }
		
		#ifdef DEBUG_SERIAL
    Serial.println("RCM USB cleanup completed");
		#endif
}

static void rcm_usb_event_cb(const usb_host_client_event_msg_t* event, void* arg) {
	if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
		usb_device_handle_t test_hdl;
		esp_err_t err = usb_host_device_open(rcm_client_hdl, event->new_dev.address, &test_hdl);
		if (err == ESP_OK) {
			const usb_device_desc_t* dev_desc;
			err = usb_host_get_device_descriptor(test_hdl, &dev_desc);
			if (err == ESP_OK && dev_desc->idVendor == APX_VID && dev_desc->idProduct == APX_PID) {
				RCM_LOG_I("*** SWITCH RCM DETECTED ***");
				rcm_dev_hdl = test_hdl;
				rcm_device_connected = true;
				return;
			}
			usb_host_device_close(rcm_client_hdl, test_hdl);
		}
	} else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
		rcm_device_connected = false;
		rcm_dev_hdl = NULL;
	}
}

static bool rcm_wait_for_transfer(usb_transfer_t* xfer, uint32_t timeout_ms, size_t expected_bytes) {
	uint32_t waited = 0;
	while (waited < timeout_ms) {
		if (xfer->status != 0 || xfer->actual_num_bytes >= expected_bytes) {
			RCM_LOG_I("Transfer done: status=%d, actual=%d", xfer->status, xfer->actual_num_bytes);
			return true;
		}
		vTaskDelay(pdMS_TO_TICKS(1));
		waited++;
	}
	RCM_LOG_W("Timeout: status=%d, actual=%d", xfer->status, xfer->actual_num_bytes);
	return false;
}

static bool rcm_read_device_id(void) {
	RCM_LOG_I("Reading Device ID...");

	usb_transfer_t* xfer = NULL;
	if (usb_host_transfer_alloc(64, 0, &xfer) != ESP_OK) return false;

	xfer->device_handle = rcm_dev_hdl;
	xfer->bEndpointAddress = 0x81;
	xfer->callback = rcm_dummy_cb;
	xfer->timeout_ms = 3000;
	xfer->num_bytes = 64;

	if (usb_host_transfer_submit(xfer) != ESP_OK) {
		usb_host_transfer_free(xfer);
		return false;
	}

	bool done = rcm_wait_for_transfer(xfer, 3000, 16);
	bool success = false;
	if (done && xfer->actual_num_bytes >= 16) {
		char ascii_buf[33] = { 0 };
		for (int i = 0; i < 16; i++) sprintf(&ascii_buf[i * 2], "%02x", xfer->data_buffer[i]);
		RCM_LOG_I("Device ID: %s", ascii_buf);
		success = true;
	}

	usb_host_transfer_free(xfer);
	return success || done;
}

static bool rcm_send_chunk(uint8_t* data, size_t len) {
	usb_transfer_t* xfer = NULL;
	if (usb_host_transfer_alloc(len, 0, &xfer) != ESP_OK) return false;

	memcpy(xfer->data_buffer, data, len);
	xfer->device_handle = rcm_dev_hdl;
	xfer->bEndpointAddress = 0x01;
	xfer->callback = rcm_dummy_cb;
	xfer->timeout_ms = 5000;
	xfer->num_bytes = len;

	if (usb_host_transfer_submit(xfer) != ESP_OK) {
		usb_host_transfer_free(xfer);
		return false;
	}

	bool done = rcm_wait_for_transfer(xfer, 5000, len);
	bool success = (done && (xfer->status == USB_TRANSFER_STATUS_COMPLETED || xfer->actual_num_bytes == len));

	usb_host_transfer_free(xfer);
	return success;
}

static void rcm_delay_2ms(void) {
	for (int i = 0; i < 480000; ++i) {}
}

static bool rcm_send_payload(uint8_t* payload_buf, uint32_t payload_len) {
	RCM_LOG_I("Sending %" PRIu32 " bytes...", payload_len);

	int chunks = 0;
	for (uint32_t offset = 0; offset < payload_len; offset += SEND_CHUNK_SIZE) {
		if (!rcm_send_chunk(&payload_buf[offset], SEND_CHUNK_SIZE)) {
			RCM_LOG_E("Failed at chunk %d", chunks);
			break;
		}
		chunks++;
		if (chunks % 50 == 0) RCM_LOG_I("Sent %d chunks", chunks);
		rcm_delay_2ms();
	}

	RCM_LOG_I("Sent %d chunks", chunks);

	if ((chunks % 2) != 1) {
		uint8_t zero[SEND_CHUNK_SIZE] = { 0 };
		rcm_send_chunk(zero, SEND_CHUNK_SIZE);
	}

	return chunks > 0;
}

static void rcm_smash_stack(void) {
	RCM_LOG_I("Smashing stack...");

	size_t total_size = 8 + 0x7000;
	uint8_t* buffer = (uint8_t*)heap_caps_aligned_alloc(64, total_size, MALLOC_CAP_DMA);
	if (!buffer) return;

	buffer[0] = 0x82;
	buffer[1] = 0x00;
	buffer[2] = 0x00;
	buffer[3] = 0x00;
	buffer[4] = 0x00;
	buffer[5] = 0x00;
	buffer[6] = 0x00;
	buffer[7] = 0x70;
	memset(buffer + 8, 0, 0x7000);

	usb_transfer_t* xfer = NULL;
	if (usb_host_transfer_alloc(total_size, 0, &xfer) != ESP_OK) {
		heap_caps_free(buffer);
		return;
	}

	memcpy(xfer->data_buffer, buffer, total_size);
	heap_caps_free(buffer);

	xfer->device_handle = rcm_dev_hdl;
	xfer->bEndpointAddress = 0;
	xfer->callback = rcm_dummy_cb;
	xfer->timeout_ms = 1000;
	xfer->num_bytes = 0x7008;

	if (usb_host_transfer_submit_control(rcm_client_hdl, xfer) != ESP_OK) {
		RCM_LOG_E("Submit failed");
		usb_host_transfer_free(xfer);
		return;
	}

	rcm_wait_for_transfer(xfer, 1000, 0);
	RCM_LOG_I("Smash result: status=%d (error/timeout expected)", xfer->status);
	usb_host_transfer_free(xfer);
}

// Pause WiFi (AP+STA), mDNS, and BLE advertising just before the payload
// transfer, and restore whichever of them were actually running afterwards.
// With WiFi + AP + mDNS + BLE all active simultaneously, RAM is tight enough
// and their radios/DMA busy enough that the payload transfer over USB can
// get corrupted -- pausing them for the few hundred ms the transfer takes
// avoids that.
static void rcm_pause_wireless_for_injection() {
	injectionWifiWasActive = (WiFi.getMode() != WIFI_MODE_NULL);
	if (injectionWifiWasActive) {
		RCM_LOG_I("Pausing WiFi for injection");
		esp_wifi_stop(); // stop only -- keeps config so esp_wifi_start() resumes cleanly
	}

	injectionMdnsWasActive = mdnsRunning;
	if (injectionMdnsWasActive) {
		RCM_LOG_I("Pausing mDNS for injection");
		Stop_mdns_service();
	}

	injectionBleWasActive = (bluetoothConfigEnabled && BLEDevice::getInitialized());
	if (injectionBleWasActive) {
		RCM_LOG_I("Pausing BLE advertising for injection");
		BLEDevice::getAdvertising()->stop();
		btStop(); //turn off bluetooth
	}

	// Give the radios a moment to actually go quiet before touching USB.
	if (injectionWifiWasActive || injectionMdnsWasActive || injectionBleWasActive) {
		vTaskDelay(pdMS_TO_TICKS(30));
	}
}

static void rcm_resume_wireless_after_injection() {
	if (injectionWifiWasActive) {
		RCM_LOG_I("Resuming WiFi after injection");
		esp_wifi_start();
		injectionWifiWasActive = false;
	}
	if (injectionMdnsWasActive) {
		RCM_LOG_I("Resuming mDNS after injection");
		Start_mdns_service();
		injectionMdnsWasActive = false;
	}
	if (injectionBleWasActive) {
		RCM_LOG_I("Resuming BLE advertising after injection");
		btStart();
		BLEDevice::getAdvertising()->start();
		injectionBleWasActive = false;
	}
}

static bool rcm_inject_payload(void) {
	RCM_LOG_I("=== INJECTION START ===");
	rcm_injection_active = true;

	if (usb_host_interface_claim(rcm_client_hdl, rcm_dev_hdl, 0, 0) != ESP_OK) {
		RCM_LOG_E("Claim failed");
		rcm_injection_active = false;
		return false;
	}
	RCM_LOG_I("Interface claimed");
	vTaskDelay(pdMS_TO_TICKS(100));

	rcm_read_device_id();

	// Try PSRAM first, fall back to DMA-capable memory if needed
	uint8_t* payload_buf = NULL;

	// Check if we have enough PSRAM
	size_t freePsram = ESP.getFreePsram();
	RCM_LOG_I("Free PSRAM: %d bytes", freePsram);

	if (freePsram > MAX_LENGTH) {
		// Use PSRAM - allocate with 64-byte alignment for USB DMA
		payload_buf = (uint8_t*)heap_caps_aligned_alloc(64, MAX_LENGTH, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
		RCM_LOG_I("Allocated payload buffer in PSRAM");
	}

	// If PSRAM allocation failed or not enough PSRAM, try internal RAM
	if (!payload_buf) {
		payload_buf = (uint8_t*)heap_caps_aligned_alloc(64, MAX_LENGTH, MALLOC_CAP_DMA);
		RCM_LOG_I("Allocated payload buffer in internal RAM");
	}

	if (!payload_buf) {
		RCM_LOG_E("Failed to allocate payload buffer (%d bytes)", MAX_LENGTH);
		usb_host_interface_release(rcm_client_hdl, rcm_dev_hdl, 0);
		rcm_injection_active = false;
		return false;
	}

	memset(payload_buf, 0, MAX_LENGTH);
	*(uint32_t*)payload_buf = MAX_LENGTH;
	uint32_t idx = 0x2a8;

	// Build the RCM spray (stack overwrite)
	uint32_t* spray = (uint32_t*)&payload_buf[idx];
	for (uint32_t addr = RCM_PAYLOAD_ADDR; addr < INTERMEZZO_LOCATION; addr += 4) {
		*spray++ = INTERMEZZO_LOCATION;
		idx += 4;
	}

	// Copy intermezzo bootloader
	memcpy(&payload_buf[idx], intermezzo_bin, sizeof(intermezzo_bin));
	idx += sizeof(intermezzo_bin);

	// Set index to payload load block
	idx = (PAYLOAD_LOAD_BLOCK - RCM_PAYLOAD_ADDR) + 0x2a8;

	// Try to load payload from SD or FFAT
	String payloadPath = getDefaultPayload();
	bool payloadLoaded = false;

	if (payloadPath.length() > 0) {
		RCM_LOG_I("Loading payload: %s", payloadPath.c_str());

		// Check if file exists first
		bool fileExists = false;
		if (sdMounted) {
			fileExists = SD.exists(payloadPath);
		} else {
			fileExists = FILESYS.exists(payloadPath);
		}

		if (fileExists) {
			File f;
			if (sdMounted) {
				f = SD.open(payloadPath, "rb");
			} else {
				f = FILESYS.open(payloadPath, "rb");
			}

			if (f) {
				size_t fsize = f.size();
				RCM_LOG_I("Payload file size: %d bytes", fsize);

				if (fsize > 0 && (idx + fsize) < MAX_LENGTH) {
					size_t bytesRead = f.read(&payload_buf[idx], fsize);
					if (bytesRead == fsize) {
						idx += fsize;
						payloadLoaded = true;
						RCM_LOG_I("Payload loaded successfully: %d bytes", fsize);
					} else {
						RCM_LOG_E("Failed to read payload file, read %d of %d bytes", bytesRead, fsize);
					}
				} else {
					RCM_LOG_E("Payload too large or empty: %zu bytes (max: %" PRIu32 ")", fsize, MAX_LENGTH - idx);
				}
				f.close();
			} else {
				RCM_LOG_E("Failed to open payload file");
			}
		} else {
			RCM_LOG_E("Payload file not found: %s", payloadPath.c_str());
		}
	} else {
		RCM_LOG_W("No default payload configured");
	}

	if (!payloadLoaded) {
		RCM_LOG_W("No payload loaded, sending intermezzo only");
	}

	RCM_LOG_I("Total payload size to send: %" PRIu32 " bytes", idx);

	// Quiet the radios before the timing-sensitive USB transfer.
	rcm_pause_wireless_for_injection();

	// Send the payload
	bool sendSuccess = rcm_send_payload(payload_buf, idx);
	if (!sendSuccess) {
		RCM_LOG_E("Payload send failed");
	}

	heap_caps_free(payload_buf);

	// Perform stack smash to trigger execution
	rcm_smash_stack();

	RCM_LOG_I("=== INJECTION COMPLETE ===");
	usb_host_interface_release(rcm_client_hdl, rcm_dev_hdl, 0);
	rcm_injection_done = true;
	rcm_injection_active = false;
	return true;
}

static void rcm_usb_host_task(void* arg) {
	while (1) {
		usb_host_lib_handle_events(portMAX_DELAY, NULL);
	}
}

static void rcm_injection_task(void* arg) {
	usb_host_client_config_t cfg = {
		.is_synchronous = false,
		.max_num_event_msg = 5,
		.async = { .client_event_callback = rcm_usb_event_cb, .callback_arg = NULL }
	};

	esp_err_t err = usb_host_client_register(&cfg, &rcm_client_hdl);
	if (err != ESP_OK) {
		RCM_LOG_E("USB client registration failed: %d", err);
		vTaskDelete(NULL);
		return;
	}
	RCM_LOG_I("USB Client registered, waiting for events...");

	while (1) {
		usb_host_client_handle_events(rcm_client_hdl, portMAX_DELAY);
		if (rcm_device_connected && !rcm_injection_done) {
			if (rcm_inject_payload()) {
				// Payload is fully sent (and the smash attempted) -- safe to turn
				// WiFi/mDNS/BLE back on now.
				rcm_resume_wireless_after_injection();
			}
			
			// Wait for disconnect before allowing next injection
			while (rcm_device_connected) {
				vTaskDelay(pdMS_TO_TICKS(100));
			}
			rcm_injection_done = false;  // Reset
		}
	}
}

// ==================== ORIGINAL Non-RCM FUNCTIONS ====================

void createDirectories(String path) {
	if (!path.startsWith("/")) path = "/" + path;
	int i = 1;
	while (i < path.length()) {
		int slashIndex = path.indexOf('/', i);
		if (slashIndex == -1) slashIndex = path.length();
		String subDir = path.substring(0, slashIndex);

		if (sdMounted) {
			if (!SD.exists(subDir)) SD.mkdir(subDir);
		} else {
			if (!FILESYS.exists(subDir)) FILESYS.mkdir(subDir);
		}
		i = slashIndex + 1;
	}
}

void onWebSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
	if (type == WS_EVT_CONNECT) sendProgressUpdate();
}

void sendProgressUpdate() {
	String json = String() + "{\"download\":" + String(download_progress) + ",\"total\":" + String(download_total) + ",\"extracted\":" + String(extracted_files) + ",\"total_files\":" + String(total_files) + ",\"current_file\":\"" + current_file + "\"" + ",\"downloading\":" + (currentState != IDLE ? "true" : "false") + "}";
	ws.textAll(json);
	vTaskDelay(pdMS_TO_TICKS(50));
}

bool downloadTarToPSRAM(const char* url, size_t* out_size) {
	HTTPClient http;
	WiFiClient client;
	WiFiClientSecure secureClient;
	String formatted_url = encodeUrlSpacesAndTabs(String(url));

	if (formatted_url.indexOf("bit.ly") != -1 && formatted_url.startsWith("http://")) {
		formatted_url = "https://" + formatted_url.substring(7);
	}

	bool isHttps = formatted_url.startsWith("https://");
	bool onlineURL = formatted_url.startsWith("https://") || formatted_url.startsWith("http://");

	if (onlineURL) {
		if (!isUrlReachable(formatted_url)) {
			current_file = "Error: URL unreachable";
			if (isHttps) secureClient.stop();
			else client.stop();
			return false;
		}
	}

	const char* final_url = formatted_url.c_str();

	if (isHttps) secureClient.setInsecure();

	http.setTimeout(10000);
	http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	http.addHeader("User-Agent", "Mozilla/5.0");
	http.addHeader("Connection", "close");

	if (!http.begin(isHttps ? secureClient : client, final_url)) {
		current_file = "Failed to begin HTTP connection";
		return false;
	}

	int httpCode = http.GET();
	if (httpCode != HTTP_CODE_OK) {
		current_file = String("HTTP GET failed: ") + String(httpCode);
		http.end();
		if (isHttps) secureClient.stop();
		else client.stop();
		return false;
	}

	*out_size = http.getSize();
	if (*out_size <= 0) {
		http.end();
		if (isHttps) secureClient.stop();
		else client.stop();
		return false;
	}

	tar_data = (uint8_t*)ps_malloc(*out_size);
	if (!tar_data) {
		http.end();
		if (isHttps) secureClient.stop();
		else client.stop();
		return false;
	}

	WiFiClient* stream = http.getStreamPtr();
	size_t total_read = 0;
	while (http.connected() && total_read < *out_size) {
		size_t available = stream->available();
		if (available) {
			int bytes = stream->readBytes(tar_data + total_read, min(available, *out_size - total_read));
			total_read += bytes;
			download_progress = total_read;
			download_total = *out_size;
			sendProgressUpdate();
			vTaskDelay(pdMS_TO_TICKS(50));
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}

	http.end();
	if (isHttps) secureClient.stop();
	else client.stop();

	if (total_read != *out_size) {
		free(tar_data);
		tar_data = nullptr;
		return false;
	}
	return true;
}

void extractTar() {
	if (!tar_data || tar_size == 0) {
		currentState = ERROR;
		current_file = "No data to extract";
		sendProgressUpdate();
		return;
	}

	extracted_files = 0;
	total_files = 0;

	for (size_t offset = 0; offset + 512 <= tar_size;) {
		const char* header = (const char*)(tar_data + offset);
		if (header[0] == '\0') break;

		char filename[101] = { 0 };
		strncpy(filename, header, 100);

		if (filename[strlen(filename) - 1] != '/') total_files++;

		char size_str[12] = { 0 };
		strncpy(size_str, header + 124, 11);
		size_t file_size = strtol(size_str, NULL, 8);
		offset += 512 + ((file_size + 511) / 512) * 512;
	}

	sendProgressUpdate();

	current_file = "Extracting files...";
	for (size_t offset = 0; offset + 512 <= tar_size;) {
		const char* header = (const char*)(tar_data + offset);
		if (header[0] == '\0') break;

		char filename[100 + 1] = { 0 };
		strncpy(filename, header, 100);
		current_file = String(filename);

		char size_str[12] = { 0 };
		strncpy(size_str, header + 124, 11);
		size_t file_size = strtol(size_str, NULL, 8);

		if (filename[strlen(filename) - 1] != '/') {
			int lastSlash = current_file.lastIndexOf('/');
			if (lastSlash > 0) {
				String dirPath = current_file.substring(0, lastSlash);
				createDirectories(dirPath);
			}

			File f;
			if (sdMounted) f = SD.open("/" + current_file, "w");
			else f = FILESYS.open("/" + current_file, "w");

			if (f) {
				f.write(tar_data + offset + 512, file_size);
				f.close();
				extracted_files++;
			}
		} else {
			createDirectories(current_file);
		}

		size_t total_block = ((file_size + 511) / 512) * 512;
		offset += 512 + total_block;
		sendProgressUpdate();
		vTaskDelay(pdMS_TO_TICKS(50));
	}

	current_file = "Extraction complete!";
	free(tar_data);
	tar_data = nullptr;
	currentState = COMPLETE;
	sendProgressUpdate();
}

void handleDownloadState() {
	static uint32_t lastStateChange = 0;
	const uint32_t stateDelay = 100;

	if (millis() - lastStateChange < stateDelay) return;
	lastStateChange = millis();

	switch (currentState) {
		case IDLE:
			current_file = "Waiting for files";
			sendProgressUpdate();
			break;
		case DOWNLOADING:
			if (downloadTarToPSRAM(downloadUrl.c_str(), &tar_size)) {
				current_file = "Extracting files...";
				sendProgressUpdate();
				currentState = EXTRACTING;
			}
			break;
		case EXTRACTING:
			extractTar();
			break;
		case COMPLETE:
			current_file = "All files extracted";
			sendProgressUpdate();
			activeOperation = false;
			autosleep = preOpAutosleep;
			if (autosleep) bootTime = millis(); //fresh countdown
			break;
		case ERROR:
			downloadUrl = "";
			current_file = "Unable to extract TAR";
			sendProgressUpdate();
			currentState = IDLE;
			activeOperation = false;
			autosleep = preOpAutosleep;
      if (autosleep) bootTime = millis(); //fresh countdown
			break;
	}
}

bool isUrlReachable(String url) {
	bool reachable = false;
	WiFiClient* client = getOTAClient(url);
	if (!client) return false;

	HTTPClient otaHttp;
	otaHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	otaHttp.setRedirectLimit(10);
	otaHttp.addHeader("User-Agent", "Mozilla/5.0");

	if (otaHttp.begin(*client, url)) {
		int httpCode = otaHttp.GET();
		String httpCodeStr = String(httpCode);
		otaEvents.send(httpCodeStr, "htmlstatus", millis());
		if (httpCode >= 200 && httpCode <= 399) reachable = true;
		otaHttp.end();
	}
	return reachable;
}

struct FTPSettings {
	const char* username;
	const char* password;
	const char* motd;
};

FTPSettings ftpSettings = {
	"ftp-user",
	"ftp-pass",
	"Welcome To PS4-Hack FTP Server"
};

struct Config {
	String wifi_ssid;
	String wifi_password;
	bool use_wifi;
	bool use_bluetooth;
	String ap_ssid;
	String ap_password;
	bool allow_ftp;
	String ftp_username;
	String ftp_password;
	bool use_sdcard;
	bool auto_sleep;
	String delay_minutes;
	String hostname;
	bool use_station;
	bool use_mdns;
};

Config config;

bool saveConfiguration() {
	// Validation helper - ensures string is printable ASCII, no control chars
	auto validateString = [](const String& val, size_t maxLen, bool allowEmpty = true) -> bool {
		if (val.length() > maxLen) return false;
		if (!allowEmpty && val.length() == 0) return false;
		for (size_t i = 0; i < val.length(); i++) {
			char c = val[i];
			if (c < 32 || c > 126) return false;  // Only printable ASCII
		}
		return true;
	};

	// Validation helper for int strings (no floats, no negatives if min >= 0)
	auto validateIntString = [](const String& val, int minVal, int maxVal) -> bool {
		if (val.length() == 0 || val.length() > 10) return false;

		// Check all characters are digits (or minus sign at start if minVal < 0)
		for (size_t i = 0; i < val.length(); i++) {
			if (i == 0 && val[i] == '-' && minVal < 0) continue;
			if (!isdigit(val[i])) return false;
		}

		// Check range
		char* endptr;
		long num = strtol(val.c_str(), &endptr, 10);
		if (*endptr != '\0') return false;
		return (num >= minVal && num <= maxVal);
	};

	// Validation helper for valid hostname
	auto validateHostname = [](const String& val) -> bool {
		if (val.length() == 0 || val.length() > 32) return false;
		if (val[0] == '-' || val[val.length() - 1] == '-') return false;
		for (size_t i = 0; i < val.length(); i++) {
			char c = val[i];
			if (!isalnum(c) && c != '-') return false;
		}
		return true;
	};

	// Validate all config values before saving
	if (!validateString(config.wifi_ssid, 32)) config.wifi_ssid = "ESP32";
	if (!validateString(config.wifi_password, 64)) config.wifi_password = "";
	if (!validateString(config.ap_ssid, 32)) config.ap_ssid = "ESP32";
	if (!validateString(config.ap_password, 64)) config.ap_password = "";
	if (!validateString(config.ftp_username, 32)) config.ftp_username = "ftp-user";
	if (!validateString(config.ftp_password, 32)) config.ftp_password = "ftp-pass";

	// Delay minutes: 0-65535 (uint16_t range)
	if (!validateIntString(config.delay_minutes, 0, 65535)) config.delay_minutes = "10";

	// Hostname validation
	if (!validateHostname(config.hostname)) config.hostname = "esp32";

	// Now proceed with saving
	FILESYS.remove("/config.json");

	File file = FILESYS.open("/config.json", "w");
	if (!file) {
		return false;
	}

	JsonDocument doc;
	doc["wifi_ssid"] = config.wifi_ssid;
	doc["wifi_password"] = config.wifi_password;
	doc["use_wifi"] = config.use_wifi;
	doc["use_bluetooth"] = config.use_bluetooth;
	doc["ap_ssid"] = config.ap_ssid;
	doc["ap_password"] = config.ap_password;
	doc["allow_ftp"] = config.allow_ftp;
	doc["ftp_username"] = config.ftp_username;
	doc["ftp_password"] = config.ftp_password;
	doc["use_sdcard"] = config.use_sdcard;
	doc["auto_sleep"] = config.auto_sleep;
	doc["delay_minutes"] = config.delay_minutes;
	doc["hostname"] = config.hostname;
	doc["use_station"] = config.use_station;
	doc["use_mdns"] = config.use_mdns;

	if (serializeJson(doc, file) == 0) {
		file.close();
		return false;
	}

	file.close();
	return true;
}

bool loadConfiguration() {
	// Set defaults first
	config.wifi_ssid = "ESP32";
	config.wifi_password = "ESP32";
	config.use_wifi = false;
	config.use_bluetooth = false;
	config.ap_ssid = "ESP32";
	config.ap_password = "";
	config.allow_ftp = false;
	config.ftp_username = "ftp-user";
	config.ftp_password = "ftp-pass";
	config.use_sdcard = false;
	config.auto_sleep = autosleep;
	config.delay_minutes = String(TIME2SLEEP);
	config.hostname = "esp32";
	config.use_station = false;
	config.use_mdns = true;

	// Check if file exists
	if (!FILESYS.exists("/config.json")) {
		saveConfiguration();
		return false;
	}

	File file = FILESYS.open("/config.json", "r");
	if (!file || file.isDirectory()) {
		FILESYS.remove("/config.json");
		saveConfiguration();
		return false;
	}

	size_t size = file.size();
	if (size == 0 || size > 8192) {
		file.close();
		FILESYS.remove("/config.json");
		saveConfiguration();
		return false;
	}

	std::unique_ptr<char[]> buf(new char[size + 1]);
	file.readBytes(buf.get(), size);
	file.close();
	buf[size] = '\0';

	JsonDocument doc;
	DeserializationError error = deserializeJson(doc, buf.get());
	if (error) {
		FILESYS.remove("/config.json");
		saveConfiguration();
		return false;
	}

	// STRICT validation helper for int strings - REJECTS floats completely
	auto safeIntString = [&doc](const char* key, String& target, int minVal, int maxVal) {
		if (doc[key].isNull()) return;

		JsonVariant var = doc[key];

		// REJECT float/double types completely
		if (var.is<double>() || var.is<float>()) {
			return;  // Keep default, don't accept floats
		}

		long val = 0;
		bool valid = false;
		const char* str = nullptr;

		if (var.is<int>() || var.is<long>()) {
			val = var.as<long>();
			valid = true;
		} else if (var.is<const char*>()) {
			str = var.as<const char*>();
			if (str && strlen(str) > 0) {
				// Check for decimal point (reject floats disguised as strings)
				for (size_t i = 0; i < strlen(str); i++) {
					if (str[i] == '.' || str[i] == ',') return;  // Reject floats
				}

				// Check all characters are digits (or minus at start)
				for (size_t i = 0; i < strlen(str); i++) {
					if (i == 0 && str[i] == '-' && minVal < 0) continue;
					if (!isdigit((unsigned char)str[i])) return;  // Invalid char
				}

				char* endptr;
				val = strtol(str, &endptr, 10);
				if (*endptr == '\0') valid = true;
			}
		}

		// Final range check
		if (valid && val >= minVal && val <= maxVal) {
			target = String(val);
		}
		// Else keep default
	};

	// Helper for strings with max length and printable ASCII check
	auto safeString = [&doc](const char* key, String& target, size_t maxLen) {
		if (doc[key].isNull()) return;

		const char* val = doc[key].as<const char*>();
		if (!val) return;

		size_t len = strlen(val);
		if (len > maxLen) return;

		// Check printable ASCII only
		for (size_t i = 0; i < len; i++) {
			if (val[i] < 32 || val[i] > 126) return;
		}

		target = val;
	};

	// Helper for boolean (accepts bool, int 0/1, or strings)
	auto safeBool = [&doc](const char* key, bool& target) {
		if (doc[key].isNull()) return;

		JsonVariant var = doc[key];
		if (var.is<bool>()) {
			target = var.as<bool>();
		} else if (var.is<int>()) {
			target = var.as<int>() != 0;
		} else if (var.is<const char*>()) {
			const char* str = var.as<const char*>();
			if (strcasecmp(str, "true") == 0 || strcmp(str, "1") == 0) {
				target = true;
			} else if (strcasecmp(str, "false") == 0 || strcmp(str, "0") == 0) {
				target = false;
			}
		}
	};

	// Load with strict validation
	safeString("wifi_ssid", config.wifi_ssid, 32);
	safeString("wifi_password", config.wifi_password, 64);
	safeBool("use_wifi", config.use_wifi);
	safeBool("use_bluetooth", config.use_bluetooth);
	safeString("ap_ssid", config.ap_ssid, 32);
	safeString("ap_password", config.ap_password, 64);
	safeBool("allow_ftp", config.allow_ftp);
	safeString("ftp_username", config.ftp_username, 32);
	safeString("ftp_password", config.ftp_password, 32);

	// CRITICAL: delay_minutes must be 1-65535 (NOT 0, to prevent instant sleep)
	safeIntString("delay_minutes", config.delay_minutes, 1, 65535);

	safeBool("use_sdcard", config.use_sdcard);
	safeBool("auto_sleep", config.auto_sleep);
	safeBool("use_station", config.use_station);
	safeBool("use_mdns", config.use_mdns);

	// Hostname validation
	if (doc["hostname"].is<const char*>()) {
		const char* val = doc["hostname"];
		if (val && strlen(val) > 0 && strlen(val) <= 32) {
			bool valid = true;
			if (val[0] == '-' || val[strlen(val) - 1] == '-') valid = false;
			for (size_t i = 0; i < strlen(val); i++) {
				if (!isalnum((unsigned char)val[i]) && val[i] != '-') {
					valid = false;
					break;
				}
			}
			if (valid) config.hostname = val;
		}
	}

	return true;
}

void taskmaster(String state) {
	if (state == "suspend") {
		if (dnsTaskHandle) vTaskSuspend(dnsTaskHandle);
		dnsServer.stop();
	}
	if (state == "resume") {
		if (dnsTaskHandle) vTaskResume(dnsTaskHandle);
		dnsServer.start(53, "*", Local_IP);
	}
}

void full_restart(void) {
    // Preferred (uses the proper register definition)
    REG_WRITE(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);
    // Equivalent raw form (from TRM / reported working):
    // REG_WRITE(0x60008000, 1UL << 31);
    while (1) {}   // never returns
}

void startAccessPoint() {
	if (config.ap_ssid.length() == 0) config.ap_ssid = "ESP32-AP";

	if (config.use_station && WiFi.status() == WL_CONNECTED) WiFi.mode(WIFI_AP_STA);
	else WiFi.mode(WIFI_AP);

	WiFi.softAPConfig(Local_IP, Gateway, Subnet_Mask);
	WiFi.softAP(config.ap_ssid.c_str(), config.ap_password.c_str());

	esp_wifi_set_ps(WIFI_PS_NONE);
	esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

	dnsServer.setTTL(30);
	dnsServer.setErrorReplyCode(DNSReplyCode::ServerFailure);
	dnsServer.start(53, "*", Local_IP);

	AP_Running = true;
}

void startWiFi() {
	if (config.use_wifi && config.wifi_ssid.length() > 0) {
		WiFi.begin(config.wifi_ssid.c_str(), config.wifi_password.c_str());
		unsigned long startAttemptTime = millis();
		while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) delay(500);

		if (WiFi.status() == WL_CONNECTED) {
			if (config.use_station) {
				startAccessPoint();
				allowFTP = config.allow_ftp;
			}
			return;
		}
	}
	startAccessPoint();
	allowFTP = config.allow_ftp;
}

String formatBytes(uint64_t bytes) {
	if (bytes < 1024) return String(bytes) + " B";
	else if (bytes < (1024 * 1024)) return String(bytes / 1024.0, 2) + " KB";
	else if (bytes < (1024ull * 1024 * 1024)) return String(bytes / 1024.0 / 1024.0, 2) + " MB";
	else return String(bytes / 1024.0 / 1024.0 / 1024.0, 2) + " GB";
}

bool removeAllFilesInDir(File dir) {
	while (true) {
		File entry = dir.openNextFile();
		if (!entry) break;
		String path = entry.path();

		if (entry.isDirectory()) {
			if (path.endsWith("/.") || path.endsWith("/..")) {
				entry.close();
				continue;
			}
			removeAllFilesInDir(entry);
			entry.close();
			if (!SD.rmdir(path.c_str())) return false;
		} else {
			entry.close();
			if (path != "/config.json") {
				if (!SD.remove(path.c_str())) return false;
			}
		}
	}
	return true;
}

void formatFileSystem() {
	if (sdMounted) {
		File root = SD.open("/");
		bool success = removeAllFilesInDir(root);
		if (success) root.close();
	} else {
		FILESYS.end();
		bool formatted = FILESYS.format();
		if (formatted) FILESYS.begin();
	}
}

void startFormattingInBackground() {
	formatStatus = "IN_PROGRESS";
	xTaskCreate([](void*) {
		formatFileSystem();
		formatStatus = "DONE";
		vTaskDelete(NULL);
	},
	            "FormatTask", 4096, NULL, 1, NULL);
}

void initMemorySystem() {
	psramAvailable = hasPSRAM();
	if (psramAvailable) {
		totalPSRAM = ESP.getPsramSize();
		availablePSRAM = ESP.getFreePsram();
	}
}

void* allocPSRAM(size_t size, const char* purpose = "") {
	if (!psramAvailable) return nullptr;
	void* buffer = psram_malloc(size);
	if (buffer) availablePSRAM -= size;
	return buffer;
}

void freePSRAM(void* buffer, size_t size) {
	if (buffer && psramAvailable) {
		free(buffer);
		availablePSRAM += size;
	}
}

bool hasPSRAM() {
	#if CONFIG_SPIRAM
	return psramFound();
	#else
	return false;
	#endif
}

void* psram_malloc(size_t size) {
	if (!hasPSRAM()) return nullptr;
	return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void freeChunk() {
	if (fileChunk) {
		memset(fileChunk, 0, CHUNK_SIZE);
		freePSRAM(fileChunk, CHUNK_SIZE);
		fileChunk = nullptr;
	}
	currentChunk = 0;
	totalChunks = 0;
	fileSize = 0;
	currentFilename = "";
}

bool isAllowedExtension(String filename) {
	for (int i = 0; i < ALLOWED_EXT_COUNT; i++) {
		if (filename.endsWith(ALLOWED_EXTENSIONS[i])) return true;
	}
	return false;
}

void listFilesRecursive(File dir, String parentPath, String& output) {
	while (File entry = dir.openNextFile()) {
		String entryName = entry.name();
		if (entryName.startsWith(".")) {
			entry.close();
			continue;
		}
		String path = (parentPath == "/" ? "/" : parentPath + "/") + entryName;
		if (entry.isDirectory()) listFilesRecursive(entry, path, output);
		else if (isAllowedExtension(entryName)) {
			if (output.length() > 1) output += ",";
			output += "\"" + path + "\"";
		}
		entry.close();
	}
}

uint64_t getUsedBytes(fs::FS& fs, const char* path = "/") {
	uint64_t total = 0;
	File root = fs.open(path);
	if (!root || !root.isDirectory()) return 0;
	File file = root.openNextFile();
	while (file) {
		if (file.isDirectory()) total += getUsedBytes(fs, file.path());
		else total += file.size();
		file = root.openNextFile();
	}
	return total;
}

void populateBatteryJson(JsonObject obj) {
	BatteryReading r = readBattery();

	obj["pmu_found"] = r.pmuOk;
	obj["connected"] = r.connected;
	obj["percent"] = r.connected ? r.percent : 0;
	obj["voltage_mv"] = r.connected ? r.voltageMv : 0;
	obj["charging"] = r.charging;
	obj["vbus_in"] = r.vbusIn;

	const char* state;
	if (!r.pmuOk) state = "unavailable";
	else if (r.charging) state = "charging";
	else if (r.vbusIn) state = "vbus";
	else if (r.connected) state = "battery";
	else state = "no_battery";
	obj["state"] = state;
}

void handleBatteryStatus(AsyncWebServerRequest* request) {
	JsonDocument doc;
	populateBatteryJson(doc.to<JsonObject>());

	String json;
	serializeJson(doc, json);
	request->send(200, "application/json", json);
}

void handleSystemInfo(AsyncWebServerRequest* request) {
	JsonDocument doc;

	if (sdMounted) {
		uint64_t total = SD.cardSize();
		uint64_t used = getUsedBytes(SD);
		uint64_t free = total > used ? total - used : 0;
		doc["fs"]["total"] = formatBytes(total).c_str();
		doc["fs"]["used"] = formatBytes(used).c_str();
		doc["fs"]["free"] = formatBytes(free).c_str();
	} else {
		doc["fs"]["total"] = formatBytes(FILESYS.totalBytes());
		doc["fs"]["used"] = formatBytes(FILESYS.usedBytes());
		doc["fs"]["free"] = formatBytes(FILESYS.totalBytes() - FILESYS.usedBytes());
	}

	doc["memory"]["psram"]["total"] = formatBytes(ESP.getPsramSize());
	doc["memory"]["psram"]["free"] = formatBytes(ESP.getFreePsram());
	doc["memory"]["psram"]["max_alloc"] = formatBytes(ESP.getMaxAllocPsram());
	doc["memory"]["heap"]["total"] = formatBytes(ESP.getHeapSize());
	doc["memory"]["heap"]["free"] = formatBytes(ESP.getFreeHeap());
	doc["memory"]["heap"]["max_alloc"] = formatBytes(ESP.getMaxAllocHeap());

	doc["sketch"]["size"] = formatBytes(ESP.getSketchSize());
	doc["sketch"]["free_space"] = formatBytes(ESP.getFreeSketchSpace());
	doc["sketch"]["md5"] = ESP.getSketchMD5();

	doc["system"]["compile_date"] = String(__DATE__) + " " + String(__TIME__);
	doc["system"]["chip_model"] = ESP.getChipModel();
	doc["system"]["chip_revision"] = ESP.getChipRevision();
	doc["system"]["cpu_freq"] = String(ESP.getCpuFreqMHz()) + " MHz";
	doc["system"]["sdk_version"] = ESP.getSdkVersion();
	doc["system"]["flash_size"] = formatBytes(ESP.getFlashChipSize());
	doc["chip"]["cores"] = ESP.getChipCores();

	const char* flashMode;
	switch (ESP.getFlashChipMode()) {
		case FM_QIO: flashMode = "QIO"; break;
		case FM_QOUT: flashMode = "QOUT"; break;
		case FM_DIO: flashMode = "DIO"; break;
		case FM_DOUT: flashMode = "DOUT"; break;
		case FM_FAST_READ: flashMode = "FAST_READ"; break;
		case FM_SLOW_READ: flashMode = "SLOW_READ"; break;
		case FM_UNKNOWN:
		default: flashMode = "UNKNOWN"; break;
	}
	doc["chip"]["flash_mode"] = flashMode;

	uint8_t baseMac[6];
	esp_read_mac(baseMac, ESP_MAC_WIFI_STA);
	char macStr[18];
	snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
	         baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);

	esp_read_mac(baseMac, ESP_MAC_WIFI_SOFTAP);
	char macStr2[18];
	snprintf(macStr2, sizeof(macStr2), "%02X:%02X:%02X:%02X:%02X:%02X",
	         baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);

	esp_read_mac(baseMac, ESP_MAC_ETH);
	char baseMacStr[18];
	snprintf(baseMacStr, sizeof(baseMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
	         baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);

	doc["mac"]["wifi"] = macStr;
	doc["mac"]["ap"] = macStr2;
	doc["mac"]["base"] = baseMacStr;
	
	bool usb_cdc_enabled = false;
	
	#ifdef ARDUINO_USB_MODE
    usb_cdc_enabled = (ARDUINO_USB_MODE == 1);
  #elif defined(USBCON)
    usb_cdc_enabled = true;
  #endif
  
  #ifdef ARDUINO_USB_CDC_ON_BOOT
    usb_cdc_enabled = usb_cdc_enabled && ARDUINO_USB_CDC_ON_BOOT;
  #endif

	#if !CONFIG_IDF_TARGET_ESP32S2
		doc["usb"]["cdc_enabled"] = usb_cdc_enabled;
		doc["usb"]["serial_connected"] = Serial ? true : false;
		doc["usb"]["vid"] = USB_VID ? USB_VID : 0x303a;
		doc["usb"]["pid"] = USB_PID ? USB_PID : 0x1001;
	#endif

	#if defined(USB_PRODUCT)
		doc["usb"]["product_name"] = USB_PRODUCT;
	#else
		doc["usb"]["product_name"] = "ESP32 USB Device";
	#endif

	#if defined(USB_MANUFACTURER)
		doc["usb"]["manufacturer"] = USB_MANUFACTURER;
	#else
		doc["usb"]["manufacturer"] = "Espressif";
	#endif

	String ipStr;
	if (WiFi.status() == WL_CONNECTED) {
		ipStr = WiFi.localIP().toString();  // Prioritize WiFi IP when connected
	} else if (AP_Running) {
		ipStr = WiFi.softAPIP().toString();  // Fall back to AP IP
	} else {
		ipStr = "0.0.0.0";  // No connection
	}

	const char* ipCStr = ipStr.c_str();
	doc["server"]["ip"] = ipCStr;
	doc["rcm"]["active"] = rcm_injection_active;
	doc["rcm"]["device_connected"] = rcm_device_connected;

	populateBatteryJson(doc["battery"].to<JsonObject>());

	String json;
	serializeJson(doc, json);
	request->send(200, "application/json", json);
}

void serveCompressedHTML(AsyncWebServerRequest* request) {
	AsyncWebServerResponse* response = request->beginResponse(
	  200, "text/html", firmware_update_html_gz, firmware_update_html_gz_len);
	response->addHeader("Content-Encoding", "gzip");
	request->send(response);
}

void handleVersion(AsyncWebServerRequest* request) {
	if (sdMounted) {
		if (SD.exists("/version.txt")) {
			File versionFile = SD.open("/version.txt", "r");
			if (versionFile) {
				String version = versionFile.readString();
				versionFile.close();
				request->send(200, "text/plain", version);
				return;
			}
		}
	} else {
		if (FILESYS.exists("/version.txt")) {
			File versionFile = FILESYS.open("/version.txt", "r");
			if (versionFile) {
				String version = versionFile.readString();
				versionFile.close();
				request->send(200, "text/plain", version);
				return;
			}
		}
	}
	request->send(200, "text/plain", firmwareVersion);
}

void handleUpload(AsyncWebServerRequest* request) {
	request->send(200, "text/plain", "Upload started");
}

void handleFirmwareUpload(AsyncWebServerRequest* request, String filename,
                          size_t index, uint8_t* data, size_t len, bool final) {
	if (!index) {
		if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {}
		otaEvents.send("0", "progress", millis());
		preOpAutosleep = autosleep;
		autosleep = false;
	}

	if (Update.write(data, len) != len) {
		otaEvents.send("Update write failed", "error", millis());
		autosleep = preOpAutosleep;
		return;
	}

	if (final) {
		if (Update.end(true)) {
			otaEvents.send("100", "progress", millis());
			otaEvents.send("complete", "complete", millis());
		} else {
			otaEvents.send("Update failed", "error", millis());
		}
		autosleep = preOpAutosleep;
		if (autosleep) bootTime = millis(); //fresh countdown
	}
}

void handleFlashRequest(AsyncWebServerRequest* request) {
	autosleep = false;
	request->send(200, "text/plain", "Flashing now...");
}

String encodeUrlSpacesAndTabs(const String& url) {
	String encodedUrl = "";
	for (int i = 0; i < url.length(); i++) {
		char c = url.charAt(i);
		if (c == ' ') encodedUrl += "%20";
		else if (c == '\t') encodedUrl += "%09";
		else encodedUrl += c;
	}
	return encodedUrl;
}

WiFiClient* getOTAClient(const String& url) {
	if (url.isEmpty()) return nullptr;
	if (url.startsWith("https://")) {
		otaSecureClient.setInsecure();
		otaSecureClient.setTimeout(15000);
		return &otaSecureClient;
	} else if (url.startsWith("http://")) {
		return &otaClient;
	}
	return nullptr;
}

void startOTA(String url) {
	if (ESP.getFreeHeap() < 20000) return;

	if (url.indexOf("bit.ly") != -1 && url.startsWith("http://")) {
		url = "https://" + url.substring(7);
	}

	url = encodeUrlSpacesAndTabs(url);

	if (!isUrlReachable(url)) {
		otaEvents.send("unreachable", "unreachable", millis());
		return;
	}

	WiFiClient* client = getOTAClient(url);
	if (!client) return;

	otaTotalWritten = 0;
	otaHttp.setTimeout(15000);
	otaHttp.setConnectTimeout(10000);
	otaHttp.setRedirectLimit(10);
	otaHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	otaHttp.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
	otaHttp.addHeader("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8");
	otaHttp.addHeader("Accept-Language", "en-US,en;q=0.5");
	otaHttp.addHeader("Connection", "close");

	if (otaHttp.begin(*client, url)) {
		int httpCode = otaHttp.GET();
		if (httpCode == HTTP_CODE_OK) {
			otaContentLength = otaHttp.getSize();
			if (otaContentLength <= 0) {
				String contentLength = otaHttp.header("Content-Length");
				if (contentLength.length() > 0) otaContentLength = contentLength.toInt();
				else {
					otaEvents.send("Update failed: Could not determine file size", "error", millis());
					otaHttp.end();
					return;
				}
			}

			if (!Update.begin(otaContentLength)) {
				otaEvents.send("Update failed: Not enough space", "error", millis());
				otaHttp.end();
				if (url.startsWith("https://")) otaSecureClient.stop();
				else otaClient.stop();
				return;
			}
			
			preOpAutosleep = autosleep;
			autosleep = false;

			otaStream = otaHttp.getStreamPtr();
			otaInProgress = true;
		} else {
			otaHttp.end();
			if (url.startsWith("https://")) otaSecureClient.stop();
			else otaClient.stop();
		}
	} else {
		otaEvents.send("Update failed: HTTP begin failed", "error", millis());
		return;
	}
}

void processOTA() {
	if (!otaInProgress || otaContentLength <= 0 || !otaStream) {
		return;
	}

	uint8_t buff[1024];
	size_t available = otaStream->available();
	if (available) {
		int read = otaStream->readBytes(buff, std::min((int)sizeof(buff), otaContentLength));
		if (read <= 0) {
			otaInProgress = false;
			otaHttp.end();
			autosleep = preOpAutosleep;
			if (autosleep) bootTime = millis();
			return;
		}

		if (Update.write(buff, read) != (size_t)read) {
			otaInProgress = false;
			otaHttp.end();
			autosleep = preOpAutosleep;
			if (autosleep) bootTime = millis();
			return;
		}

		otaContentLength -= read;
		otaTotalWritten += read;
		yield();

		int progress = (otaTotalWritten * 100) / (otaTotalWritten + otaContentLength);
		otaEvents.send(String(progress).c_str(), "progress", millis());
	}

	if (otaContentLength <= 0) {
		if (Update.end() && Update.isFinished()) {
			otaEvents.send("100", "progress", millis());
			otaEvents.send("complete", "complete", millis());
		}
		otaInProgress = false;
		otaHttp.end();
		autosleep = preOpAutosleep;
		if (autosleep) bootTime = millis();
	}
}

bool startftpserver() {
	allowFTP = true;
	if (config.use_mdns) MDNS.addService("ftp", "tcp", 21);
	if (sdMounted) ftpSrv.setFileSystem(SD);
	ftpSrv.begin(config.ftp_username.c_str(), config.ftp_password.c_str(), ftpSettings.motd);
	xTaskCreate(ftpServerTask, "FTPServer", 4096, NULL, 1, &ftpTaskHandle);
	return allowFTP;
}

bool stopftpserver() {
	if (ftpTaskHandle == NULL) return true;
	allowFTP = false;
	xTaskNotify(ftpTaskHandle, 0, eNoAction);
	const TickType_t timeout = pdMS_TO_TICKS(1000);
	const TickType_t startTime = xTaskGetTickCount();
	while (eTaskGetState(ftpTaskHandle) != eDeleted && (xTaskGetTickCount() - startTime) < timeout) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	if (eTaskGetState(ftpTaskHandle) != eDeleted) vTaskDelete(ftpTaskHandle);
	ftpTaskHandle = NULL;
	return true;
}

void ftpServerTask(void* pvParameters) {
	while (true) {
		if (allowFTP) ftpSrv.handleFTP();
		uint32_t notificationValue;
		if (xTaskNotifyWait(0, ULONG_MAX, &notificationValue, 0) == pdTRUE) {
			ftpSrv.end();
			vTaskDelete(NULL);
			return;
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void dnsServerTask(void* pvParameters) {
	while (true) {
		dnsServer.processNextRequest();
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void otaUpdateTask(void* pvParameters) {
	while (true) {
		if (otaInProgress) processOTA();
		vTaskDelay(100 / portTICK_PERIOD_MS);
	}
	vTaskDelete(NULL);
}

void setupServer() {
	if (sdMounted) hasIndexFile = SD.exists("/index.html");
	else hasIndexFile = FILESYS.exists("/index.html");

	server.on("/config.json", HTTP_GET, [](AsyncWebServerRequest* request) {
		request->redirect("/index.html");
	});

	server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
		AsyncWebServerResponse* response = request->beginResponse(200, "text/html", index_gz, index_gz_len);
		response->addHeader("Content-Encoding", "gzip");
		request->send(response);
	});

	server.on("/esp32.local", HTTP_GET, [](AsyncWebServerRequest* request) {
		request->redirect("/");
	});

	if (sdMounted) server.serveStatic("/", SD, "/");
	else server.serveStatic("/", FILESYS, "/");

	server.onNotFound([](AsyncWebServerRequest* request) {
		if (hasIndexFile) request->redirect("/index.html");
		else request->redirect("/file");
	});

	server.on("/edit", HTTP_GET, [](AsyncWebServerRequest* request) {
		AsyncWebServerResponse* response = request->beginResponse(200, "text/html", editor_gz, editor_gz_len);
		response->addHeader("Content-Encoding", "gzip");
		request->send(response);
	});

	server.on("/psram-list-files", HTTP_GET, [](AsyncWebServerRequest* request) {
		String jsonResponse = "[";
		File root;
		if (sdMounted) root = SD.open("/");
		else root = FILESYS.open("/");
		String output = "";
		listFilesRecursive(root, "", output);
		root.close();
		jsonResponse += output;
		jsonResponse += "]";
		AsyncWebServerResponse* response = request->beginChunkedResponse("application/json",
		                                                                 [jsonResponse](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
			                                                                 if (index >= jsonResponse.length()) return 0;
			                                                                 size_t len = std::min(maxLen, jsonResponse.length() - index);
			                                                                 memcpy(buffer, jsonResponse.c_str() + index, len);
			                                                                 return len;
		                                                                 });
		request->send(response);
	});

	server.on("/psram-read", HTTP_GET, [](AsyncWebServerRequest* request) {
		if (!request->hasParam("file")) {
			request->send(400, "text/plain", "Missing file parameter");
			return;
		}
		String filename = request->getParam("file")->value();
		if (filename.startsWith("/")) filename = filename.substring(1);

		File file;
		if (sdMounted) file = SD.open("/" + filename, "r");
		else file = FILESYS.open("/" + filename, "r");

		if (!file || file.isDirectory()) {
			request->send(404, "text/plain", "File not found");
			return;
		}

		size_t fileSize = file.size();
		if (fileSize > MAX_FILE_SIZE) {
			file.close();
			request->send(413, "text/plain", "File too large");
			return;
		}

		char* buffer = (char*)allocPSRAM(fileSize + 1, "file read");
		if (!buffer) {
			buffer = (char*)malloc(fileSize + 1);
			if (!buffer) {
				file.close();
				request->send(500, "text/plain", "Memory allocation failed");
				return;
			}
		}

		size_t bytesRead = file.readBytes(buffer, fileSize);
		buffer[bytesRead] = '\0';
		file.close();
		request->send(200, "text/plain", buffer);

		if (psramAvailable) freePSRAM(buffer, fileSize + 1);
		else free(buffer);
	});

	server.on("/psram-file-size", HTTP_GET, [](AsyncWebServerRequest* request) {
		if (!request->hasParam("file")) {
			request->send(400, "text/plain", "Missing file parameter");
			return;
		}
		String filename = request->getParam("file")->value();
		if (!filename.startsWith("/")) filename = "/" + filename;
		if (!isAllowedExtension(filename)) {
			request->send(403, "text/plain", "File type not allowed");
			return;
		}

		File file;
		if (sdMounted) file = SD.open(filename, "r");
		else file = FILESYS.open(filename, "r");

		if (!file || file.isDirectory()) {
			request->send(404, "text/plain", "File not found");
			return;
		}

		JsonDocument doc;
		doc["name"] = filename;
		doc["size"] = file.size();
		file.close();

		String json;
		serializeJson(doc, json);
		request->send(200, "application/json", json);
	});

	server.on(
	  "/psram-save", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
	  [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
		  static String buffer;
		  if (index == 0) {
			  size_t safeSize = min(total, (size_t)(psramAvailable ? availablePSRAM / 2 : 16384));
			  buffer.reserve(safeSize);
			  buffer = "";
		  }
		  buffer.concat((char*)data, len);
		  if (index + len != total) return;

		  JsonDocument doc;
		  DeserializationError error = deserializeJson(doc, buffer);
		  if (error) {
			  request->send(400, "text/plain", String("JSON error: ") + error.c_str());
			  return;
		  }

		  String filename = doc["filename"].as<String>();
		  String content = doc["content"].as<String>();
		  if (!filename.startsWith("/")) filename = "/" + filename;

		  File file;
		  if (sdMounted) file = SD.open(filename, "w");
		  else file = FILESYS.open(filename, "w");

		  if (!file) {
			  request->send(500, "text/plain", "Failed to open file");
			  return;
		  }

		  size_t bytesWritten = file.print(content);
		  file.close();
		  request->send(200, "text/plain",
		                bytesWritten == content.length() ? "Saved successfully" : "Incomplete write");
	  });

	server.on("/esp32-startftp", HTTP_POST, [](AsyncWebServerRequest* request) {
		if (allowFTP) request->send(200, "text/plain", "RUNNING");
		else {
			bool success = startftpserver();
			if (success) request->send(200, "text/plain", "OK");
			else request->send(500, "text/plain", "FAIL");
		}
	});

	server.on("/esp32-stopftp", HTTP_POST, [](AsyncWebServerRequest* request) {
		if (!allowFTP) request->send(200, "text/plain", "NOTRUNNING");
		else {
			bool stopped = stopftpserver();
			if (stopped) request->send(200, "text/plain", "OK");
			else request->send(500, "text/plain", "FAIL");
		}
	});

	server.on("/esp32-format", HTTP_POST, [](AsyncWebServerRequest* request) {
		startFormattingInBackground();
		request->send(200, "text/plain", "OK");
	});

	server.on("/esp32-status", HTTP_GET, [](AsyncWebServerRequest* request) {
		request->send(200, "text/plain", (const char*)formatStatus);
	});

	server.on("/usboff", HTTP_POST, [](AsyncWebServerRequest* request) {
		ESP.restart();
	});

	server.on("/system-info", HTTP_GET, [](AsyncWebServerRequest* request) {
		handleSystemInfo(request);
	});

	server.on("/battery-status", HTTP_GET, [](AsyncWebServerRequest* request) {
		handleBatteryStatus(request);
	});

	server.on("/ota", HTTP_GET, serveCompressedHTML);
	server.on("/esp32-version", HTTP_GET, handleVersion);
	server.on("/upload-ota", HTTP_POST, handleUpload, handleFirmwareUpload);
	server.on("/esp32-update-local", HTTP_POST, handleFlashRequest);
	server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest* request) {
		request->send(200, "text/plain", "Rebooting...");
		request->onDisconnect([]() {
			delay(100);
			full_restart();
		});
	});

	server.on(
	  "/esp32-update", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
	  [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
		  JsonDocument doc;
		  DeserializationError error = deserializeJson(doc, data, len);
		  if (error || !doc["url"].is<String>()) {
			  request->send(400, "text/plain", "Invalid JSON");
			  return;
		  }
		  ota_url = doc["url"].as<String>();
		  request->send(200, "text/plain", "OTA started");
		  static unsigned long waitforreply = 0;
		  if (millis() - waitforreply > 500) {
			  doOta = true;
			  waitforreply = millis();
		  }
			else{
				//do nothing yet;
			}
	  });

	server.on("/get-config", HTTP_GET, [](AsyncWebServerRequest* request) {
		JsonDocument doc;
		doc["use_wifi"] = config.use_wifi;
		doc["use_bluetooth"] = config.use_bluetooth;
		doc["wifi_ssid"] = config.wifi_ssid;
		doc["wifi_password"] = config.wifi_password;
		doc["ap_ssid"] = config.ap_ssid;
		doc["ap_password"] = config.ap_password;
		doc["allow_ftp"] = config.allow_ftp;
		doc["ftp_username"] = config.ftp_username;
		doc["ftp_password"] = config.ftp_password;
		doc["use_sdcard"] = config.use_sdcard;
		doc["auto_sleep"] = config.auto_sleep;
		doc["delay_minutes"] = config.delay_minutes;
		doc["hostname"] = config.hostname;
		doc["use_station"] = config.use_station;
		doc["use_mdns"] = config.use_mdns;

		String json;
		serializeJson(doc, json);
		request->send(200, "application/json", json);
	});

	server.on(
	  "/save-config", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
	  [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
		  static String jsonBody;
		  if (index == 0) jsonBody = "";
		  jsonBody.concat((char*)data, len);
		  if (index + len == total) {
			  JsonDocument doc;
			  DeserializationError error = deserializeJson(doc, jsonBody);
			  if (error) {
				  String errMsg = "{\"error\":\"JSON parse failed: " + String(error.c_str()) + "\"}";
				  request->send(400, "application/json", errMsg);
				  return;
			  }

			  config.use_wifi = doc["use_wifi"] | config.use_wifi;
			  config.use_bluetooth = doc["use_bluetooth"] | config.use_bluetooth;
			  config.wifi_ssid = doc["wifi_ssid"] | config.wifi_ssid;
			  config.wifi_password = doc["wifi_password"] | config.wifi_password;
			  config.ap_ssid = doc["ap_ssid"] | config.ap_ssid;
			  config.ap_password = doc["ap_password"] | config.ap_password;
			  config.allow_ftp = doc["allow_ftp"] | config.allow_ftp;
			  config.ftp_username = doc["ftp_username"] | config.ftp_username;
			  config.ftp_password = doc["ftp_password"] | config.ftp_password;
			  config.use_sdcard = doc["use_sdcard"] | config.use_sdcard;
			  config.auto_sleep = doc["auto_sleep"] | config.auto_sleep;
			  config.delay_minutes = doc["delay_minutes"] | config.delay_minutes;
			  config.hostname = doc["hostname"] | config.hostname;
			  config.use_station = doc["use_station"] | config.use_station;
			  config.use_mdns = doc["use_mdns"] | config.use_mdns;

			  if (saveConfiguration()) request->send(200, "application/json", "{\"status\":\"success\"}");
			  else request->send(500, "application/json", "{\"error\":\"Filesystem error\"}");
		  }
	  });

	server.on("/config", HTTP_GET, [](AsyncWebServerRequest* request) {
		AsyncWebServerResponse* response = request->beginResponse(200, "text/html", config_html_gz, config_html_gz_len);
		response->addHeader("Content-Encoding", "gzip");
		request->send(response);
	});

	server.on("/info", HTTP_GET, [](AsyncWebServerRequest* request) {
		AsyncWebServerResponse* response = request->beginResponse(200, "text/html", info_gz, info_gz_len);
		response->addHeader("Content-Encoding", "gzip");
		request->send(response);
	});

	server.on("/downloader", HTTP_GET, [](AsyncWebServerRequest* request) {
		AsyncWebServerResponse* response = request->beginResponse(200, "text/html", tar_gz, tar_gz_len);
		response->addHeader("Content-Encoding", "gzip");
		request->send(response);
	});
	
	server.on("/styles.css", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(200, "text/css", css_gz, css_gz_len);
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
	});

	server.on("/start-download", HTTP_POST, [](AsyncWebServerRequest* request) {
		if (currentState != IDLE) {
			request->send(400, "text/plain", "Download already in progress");
			return;
		}
		activeOperation = true;
		preOpAutosleep = autosleep;
    autosleep = false;
		if (request->hasParam("url", true)) {
			downloadUrl = request->getParam("url", true)->value();
			download_progress = 0;
			download_total = 0;
			extracted_files = 0;
			total_files = 0;
			current_file = "Starting download...";
			currentState = DOWNLOADING;
			current_file = "Downloading...";
			sendProgressUpdate();
			request->send(200, "text/plain", "Download started");
		} else {
			request->send(400, "text/plain", "URL parameter missing");
		}
	});

	server.on("/mmstop", HTTP_GET, [](AsyncWebServerRequest* request) {
		multimedia = false;
		request->send(200, "text/plain", "OK");
	});

	server.on("/mmstart", HTTP_GET, [](AsyncWebServerRequest* request) {
		multimedia = true;
		request->send(200, "text/plain", "OK");
	});

	server.on(
	  "/upload-tar", HTTP_POST, [](AsyncWebServerRequest* request) {
		  request->send(200);
	  },
	  [](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
		  static uint8_t* upload_buffer = nullptr;
		  static size_t upload_size = 0;
		  static size_t total_received = 0;
		  static uint32_t last_progress_update = 0;

		  if (!index) {
			  activeOperation = true;
			  preOpAutosleep = autosleep;
        autosleep = false;
			  if (upload_buffer) {
				  free(upload_buffer);
				  upload_buffer = nullptr;
			  }
			  currentState = DOWNLOADING;
			  download_progress = 0;
			  download_total = request->contentLength();
			  extracted_files = 0;
			  total_files = 0;
			  total_received = 0;
			  current_file = "Receiving upload...";
			  last_progress_update = millis();

			  if (download_total > 0) {
				  upload_buffer = (uint8_t*)ps_malloc(download_total);
				  if (!upload_buffer) {
					  request->send(500, "text/plain", "Memory allocation failed");
					  return;
				  }
			  }
		  }

		  if (len > 0) {
			  if (!upload_buffer) {
				  uint8_t* new_buffer = (uint8_t*)ps_realloc(upload_buffer, total_received + len);
				  if (!new_buffer) {
					  request->send(500, "text/plain", "Memory allocation failed");
					  return;
				  }
				  upload_buffer = new_buffer;
			  }
			  memcpy(upload_buffer + index, data, len);
			  total_received += len;

			  if (millis() - last_progress_update > 200 || final) {
				  download_progress = total_received;
				  sendProgressUpdate();
				  last_progress_update = millis();
			  }
		  }

		  if (final) {
			  upload_size = total_received;
			  current_file = "Extracting uploaded files...";
			  sendProgressUpdate();
			  if (upload_buffer && upload_size > 0) {
				  activeOperation = true;
				  tar_data = upload_buffer;
				  tar_size = upload_size;
				  currentState = EXTRACTING;
			  } else {
				  if (upload_buffer) free(upload_buffer);
				  upload_buffer = nullptr;
				  currentState = ERROR;
				  current_file = "Upload failed - no data";
				  sendProgressUpdate();
			  }
		  }
	  });

	otaEvents.onConnect([](AsyncEventSourceClient* client) {
		client->send("0", "progress", millis());
		client->send("waiting", "waiting", millis());
	});

	server.begin();
	ws.onEvent(onWebSocketEvent);
	server.addHandler(&ws);
	server.addHandler(&otaEvents);
}

void removeconfig() {
	if (FILESYS.remove("/config.json")) full_restart();
}

void Start_mdns_service() {
	if (config.use_mdns) {
		if (!MDNS.begin(config.hostname.c_str())) return;
		else {
			MDNS.addService("http", "tcp", 80);
			mdnsRunning = true;
		}
	}
}

void Stop_mdns_service() {
	if (mdnsRunning) {
		MDNS.end();
		mdnsRunning = false;
	}
}

void shutdownWireless() {
	esp_wifi_stop();
	esp_wifi_deinit();
}

void deepsleep() {
    #ifdef DEBUG_SERIAL
    Serial.println("Shutting down everything...");
		#endif

		powerOffDevice(); //code under here should never run - but leave it anyway...

		stopui = true;
		stopAudioEngine(); // no-op if already suspended
		gfx->fillScreen(RGB565_BLACK);

    // === Shutdown Bluetooth cleanly ===
    if (config.use_bluetooth) {
			#ifdef DEBUG_SERIAL
			Serial.println("Disabling Bluetooth before sleep...");
			#endif
      if (BLEDevice::getInitialized()) {
				BLEDevice::deinit(true);   // Full cleanup
				btStop(); //turn off bluetooth
      }
    }

    // Shutdown WiFi
    shutdownWireless();

    delay(100);
    #ifdef DEBUG_SERIAL
		Serial.flush();
    Serial.println("Entering deep sleep now...");
		#endif
}

String getDefaultPayload() {
	if (sdMounted) {
		if (SD.exists("/payload.txt")) {
			File file = SD.open("/payload.txt", "r");
			String defaultPayload = file.readString();
			file.close();
			defaultPayload.trim();
			if (defaultPayload.length() > 0 && FILESYS.exists("/payloads/" + defaultPayload)) {
				return "/payloads/" + defaultPayload;
			}
		}
	} else {
		if (FILESYS.exists("/payload.txt")) {
			File file = FILESYS.open("/payload.txt", "r");
			String defaultPayload = file.readString();
			file.close();
			defaultPayload.trim();
			if (defaultPayload.length() > 0 && FILESYS.exists("/payloads/" + defaultPayload)) {
				return "/payloads/" + defaultPayload;
			}
		}
	}

	if (sdMounted) {
		if (SD.exists("/payloads/default.bin")) return "/payloads/default.bin";
	} else {
		if (FILESYS.exists("/payloads/default.bin")) return "/payloads/default.bin";
	}

	if (sdMounted) {
		File root = SD.open("/payloads");
		File file = root.openNextFile();
		while (file) {
			if (String(file.name()).endsWith(".bin")) {
				String path = String(file.name());
				file.close();
				root.close();
				return path;
			}
			file = root.openNextFile();
		}
	} else {
		File root = FILESYS.open("/payloads");
		File file = root.openNextFile();
		while (file) {
			if (String(file.name()).endsWith(".bin")) {
				String path = String(file.name());
				file.close();
				root.close();
				return path;
			}
			file = root.openNextFile();
		}
	}
	return "";
}

// ---------------- BOOT-BUTTON PAYLOAD CYCLING ----------------
int getCurrentPayloadIndex(const std::vector<String> &files) {
	String current;
	File f = sdMounted ? SD.open("/payload.txt", "r") : FILESYS.open("/payload.txt", "r");
	if (f) {
		current = f.readString();
		f.close();
		current.trim();
	}
	if (current.length() == 0) return -1;

	for (size_t i = 0; i < files.size(); i++) {
		if (files[i] == current) return (int)i;
	}
	return -1;
}

void cyclePayload() {
	std::vector<String> files = listPayloadBinFiles();

	if (files.empty()) {
		snprintf(payloadStatusText, sizeof(payloadStatusText), "NO PAYLOADS FOUND *** ");
		showingPayloadStatus = true;
		payloadStatusShownAt = millis();
		setScrollText(payloadStatusText);
		#ifdef DEBUG_SERIAL
		Serial.println("[BOOT] Cycle payload: /payloads has no .bin files");
		#endif
		return;
	}

	int idx = getCurrentPayloadIndex(files);
	int nextIdx = (idx + 1) % (int)files.size();
	String next = files[nextIdx];

	File f = sdMounted ? SD.open("/payload.txt", FILE_WRITE) : FILESYS.open("/payload.txt", FILE_WRITE);
	bool wrote = false;
	if (f) {
		f.print(next);
		f.close();
		wrote = true;
	}

	snprintf(payloadStatusText, sizeof(payloadStatusText), "%s: %s *** ",
	         wrote ? "PAYLOAD" : "PAYLOAD (SAVE FAILED)", next.c_str());
	showingPayloadStatus = true;
	payloadStatusShownAt = millis();

	String temp = String(payloadStatusText);
	temp.replace("PAYLOAD: ", "");
	strncpy(payloadStatusText, temp.c_str(), sizeof(payloadStatusText));
	payloadStatusText[sizeof(payloadStatusText) - 1] = '\0';

	setScrollText(payloadStatusText);

	#ifdef DEBUG_SERIAL
	Serial.printf("[BOOT] Cycled payload -> %s (%s)\n", next.c_str(), wrote ? "saved" : "save failed");
	#endif
}

// Bluetooth File Handlers
void sendFileList() {
	// Clear any pending notifications first
  responseCharacteristic->setValue("");
  
  // Small delay to let stack clear
  vTaskDelay(pdMS_TO_TICKS(50));

	if (listInProgress) {
    RCM_LOG_W("List already in progress - ignoring duplicate request");
    return;
  }
  listInProgress = true;

  if (!responseCharacteristic) {
		#ifdef DEBUG_SERIAL
    Serial.println("Error: responseCharacteristic is null");
		#endif
    return;
  }

  // Try without trailing slash first
  File root = sdMounted ? SD.open("/payloads") : FILESYS.open("/payloads");
  
  // If failed, try with trailing slash
  if (!root) {
    root = sdMounted ? SD.open("/payloads/") : FILESYS.open("/payloads/");
  }

  // Auto-create if missing
  if (!root || !root.isDirectory()) {
		#ifdef DEBUG_SERIAL
    Serial.println("Payloads dir not openable, attempting to create...");
		#endif
    FILESYS.mkdir("/payloads");
    root = FILESYS.open("/payloads");
  }

  if (!root || !root.isDirectory()) {
		#ifdef DEBUG_SERIAL
    Serial.println("CRITICAL: Still cannot open /payloads after mkdir!");
		#endif
    responseCharacteristic->setValue("ERROR: NO FS");
    responseCharacteristic->notify();
    return;
  }

  #ifdef DEBUG_SERIAL
	Serial.println("Successfully opened /payloads directory for BLE listing");
	#endif

  File file = root.openNextFile();
  int fileCount = 0;

  while (file) {
    String name = file.name();
    if (name.endsWith(".bin")) {           // only show .bin files
			name = name + "\n";
      responseCharacteristic->setValue(name.c_str());
      responseCharacteristic->notify();
      Serial.printf("BLE sent: %s\n", name.c_str());
      fileCount++;
      vTaskDelay(pdMS_TO_TICKS(50));       // increased delay for stability
    }
    file = root.openNextFile();
  }

  responseCharacteristic->setValue("END");
  responseCharacteristic->notify();
	#ifdef DEBUG_SERIAL
  Serial.printf("BLE file list complete - %d files sent\n", fileCount);
	#endif

  root.close();
	listInProgress = false;
}

void handleFileSelection(String filename) {
  if (!responseCharacteristic) {
		#ifdef DEBUG_SERIAL
    Serial.println("Error: responseCharacteristic is null");
		#endif
    return;
  }
  
  filename.trim();
	#ifdef DEBUG_SERIAL
  Serial.println("Selecting file: [" + filename + "]");
	#endif
  
  File f;
	if (sdMounted) f = SD.open("/payload.txt", FILE_WRITE);
	else f = FILESYS.open("/payload.txt", FILE_WRITE);

  if (f) {
    f.println(filename);
    f.close();
    responseCharacteristic->setValue("OK");
		#ifdef DEBUG_SERIAL
    Serial.println("File selection saved");
		#endif
  } else {
    responseCharacteristic->setValue("ERROR");
		#ifdef DEBUG_SERIAL
    Serial.println("Failed to save selection");
		#endif
  }

  responseCharacteristic->notify();
}

void handleSendContent(String content) {
  if (!responseCharacteristic) {
		#ifdef DEBUG_SERIAL
    Serial.println("Error: responseCharacteristic is null");
		#endif
    return;
  }

  if (content.length() == 0) {
    responseCharacteristic->setValue("ERROR: EMPTY CONTENT");
    responseCharacteristic->notify();
    return;
  }

  File f;
	if (sdMounted) f = SD.open("/payload.txt", FILE_WRITE);
	else f = FILESYS.open("/payload.txt", FILE_WRITE);
  
	if (f) {
    f.print(content); // Use print() instead of println() to avoid extra newline if unwanted
    f.close();
		#ifdef DEBUG_SERIAL
    Serial.println("Content saved to /payload.txt (preserved case)");
		#endif
    responseCharacteristic->setValue("OK");
  } else {
		#ifdef DEBUG_SERIAL
    Serial.println("Failed to open /payload.txt for writing");
		#endif
    responseCharacteristic->setValue("ERROR: WRITE FAILED");
  }

  responseCharacteristic->notify();
}

// Bluetooth Callbacks
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue();
    value.trim(); // Remove leading/trailing whitespace/newlines
		
		// Debounce: ignore commands too close together
		unsigned long now = millis();
		if (now - lastCommandTime < COMMAND_DEBOUNCE_MS) {
			RCM_LOG_W("Command debounced - too soon");
			return;
		}
		lastCommandTime = now;

    #ifdef DEBUG_SERIAL
		Serial.printf("BLE Write Received (%d bytes): '%s'\n", value.length(), value.c_str());
		#endif

    // Make a copy for case-insensitive command detection
    String cmd = value;
    cmd.toUpperCase();

    if (cmd == "LIST") {
			#ifdef DEBUG_SERIAL
      Serial.println("Command: LIST → sending file list");
			#endif
      sendFileList();
    }
    else if (cmd.startsWith("PLD:")) {
      String filename = value.substring(4);   // Use ORIGINAL value (case preserved)
      filename.trim();
			#ifdef DEBUG_SERIAL
      Serial.println("Command: PLD: → " + filename);
			#endif
      handleFileSelection(filename);
    }
    else if (cmd.startsWith("SEND:")) {
      String content = value.substring(5);    // Use ORIGINAL value (case preserved)
      content.trim();
			#ifdef DEBUG_SERIAL
      Serial.println("Command: SEND → content: '" + content + "'");
			#endif
      handleSendContent(content);             // We'll create this function
    }
		else if (cmd.startsWith("GETIP")) {
			ipStr = ipStr + "\n";
			responseCharacteristic->setValue(ipStr);
      responseCharacteristic->notify();
    }
		else if (cmd.startsWith("REBOOT")) {
			ESP.restart();  //software reset
    }
    else {
			#ifdef DEBUG_SERIAL
      Serial.println("Unknown command: " + value);
			#endif
      if (responseCharacteristic) {
        responseCharacteristic->setValue("UNKNOWN_CMD");
        responseCharacteristic->notify();
      }
    }
  }
};

class MyServerCallbacks : public BLEServerCallbacks {
	void onConnect(BLEServer* pServer) {
		RCM_LOG_I("BLE Client Connected");
	}

	void onDisconnect(BLEServer* pServer) {
		RCM_LOG_I("BLE Client Disconnected - Restarting Advertising");
		// Restart advertising so the device shows up again
		pServer->getAdvertising()->start();
	}
};

#if WITH_ENUM_FILTER_WORKAROUND
static bool usb_enum_filter(const usb_device_desc_t* desc, uint8_t* bConfigurationValue) {
    return true;  // Allow all USB devices to enumerate
}
#endif

// =============================================
// EARLIEST POSSIBLE EXECUTION - Runs on Core 1
// =============================================
void setup1() {
	//
}

void loop1() {
    // Leave empty or put very light tasks here
    //vTaskDelay(pdMS_TO_TICKS(1000));
}

// ==================== SETUP AND LOOP - Runs on Core 0 ====================
void setup() {
	// ---- Popcorn UI: USB-Serial/JTAG debug console (separate from native USB "Serial") ----
	/*
	USBSerial.begin(115200);
	USBSerial.setTxTimeoutMs(0);
	USBSerial.println("Digital-7 Font Demo - Starfield + Cube + Checkerboards + Scroller");
	*/

	#ifdef DEBUG_SERIAL
	Serial.begin(115200);
	delay(1000);
	if (Serial.available() > 0) {
		Serial.println("USB Serial connected");
	}

	// Print wake-up reason
	esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
	Serial.printf("Wakeup reason: %d ", wakeup_reason);
	
	switch (wakeup_reason) {
			case ESP_SLEEP_WAKEUP_TIMER:    Serial.println("(Timer)"); break;
			case ESP_SLEEP_WAKEUP_TOUCHPAD: Serial.println("(Touch)"); break;
			case ESP_SLEEP_WAKEUP_EXT0:     Serial.println("(EXT0)"); break;
			case ESP_SLEEP_WAKEUP_EXT1:     Serial.println("(EXT1)"); break;
			default:                        Serial.println("(Power-on or other)"); break;
	}

	Serial.println();
	Serial.println("================================");
	Serial.println("RCM Injector + ESP32 OS v1.0");
	Serial.printf("CPU: %" PRIu32 " MHz\n", ESP.getCpuFreqMHz());
	Serial.println("================================");
	#endif

	// Initialize filesystem first (needed for payload loading)
	if (!FILESYS.begin(true)) {
	#ifdef DEBUG_SERIAL
		Serial.println("FFat Mount Failed - attempting to format...");
	#endif
		formatFileSystem();
		if (!FILESYS.begin(true)) {
	#ifdef DEBUG_SERIAL
			Serial.println("FFat Mount STILL Failed - using minimal defaults");
	#endif
			config.ap_ssid = "ESP32-Config";
			startAccessPoint();
			setupServer();
			return;
		}
	}

	// Load configuration
	if (!loadConfiguration()) {
		#ifdef DEBUG_SERIAL
		Serial.println("Using default configuration");
		#endif
	}

	// ==================== POPCORN UI: I2C / EXPANDER / SD CARD ====================
	// Moved ahead of RCM injector init below: SD_MMC needs the expander (for
	// EXIO7/SDCS) ready first, and the RCM payload loader needs the SD card
	// mounted before it decides whether to read the payload from SD or flash.
	Wire.begin(IIC_SDA, IIC_SCL);
	Wire.setClock(400000);

	// Accelerometer for shake-to-switch-song. Non-fatal if not found -
	// qmi8658_poll_for_shake() just stays a no-op and everything else runs
	// exactly as before.
	qmi8658_init();

	if (!expander.begin(0x20)) {
		#ifdef DEBUG_SERIAL
		Serial.println("XCA9554 expander not found at 0x20!");
		#endif
		//USBSerial.println("XCA9554 not found!");
		while (1);
	}
	#ifdef DEBUG_SERIAL
	Serial.println("XCA9554 expander found at 0x20");
	#endif

	expander.pinMode(0, OUTPUT);
	expander.pinMode(1, OUTPUT);
	expander.pinMode(2, OUTPUT);
	expander.pinMode(6, OUTPUT);
	expander.pinMode(SD_ENABLE_EXPANDER_PIN, OUTPUT);
	expander.digitalWrite(0, HIGH);
	expander.digitalWrite(1, HIGH);
	expander.digitalWrite(2, HIGH);
	expander.digitalWrite(6, HIGH);
	expander.digitalWrite(SD_ENABLE_EXPANDER_PIN, HIGH);
	#ifdef DEBUG_SERIAL
	Serial.printf("EXIO%d (SDCS) driven HIGH, now reads back as %d\n",
		SD_ENABLE_EXPANDER_PIN, expander.digitalRead(SD_ENABLE_EXPANDER_PIN));
	#endif

	// Initialize SD card if enabled.
	// No CS pin on this board - the card is on the 1-bit SDMMC bus, and it
	// only becomes visible once EXIO7 (just set above) is driven high.
	// Mounted here, before RCM init, so the payload can be read from SD.
	#ifdef DEBUG_SERIAL
	Serial.printf("config.use_sdcard = %d\n", config.use_sdcard);
	#endif
	if (config.use_sdcard) {
		bool pinsOk = SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
		#ifdef DEBUG_SERIAL
		Serial.printf("SD_MMC.setPins(CLK=%d, CMD=%d, DATA=%d) -> %s\n",
			SDMMC_CLK, SDMMC_CMD, SDMMC_DATA, pinsOk ? "ok" : "FAILED");
		#endif
		bool mountOk = SD_MMC.begin("/sdcard", true, SD_FORMAT_IF_MOUNT_FAILED);  // true = 1-bit mode
		#ifdef DEBUG_SERIAL
		Serial.printf("SD_MMC.begin(\"/sdcard\", 1-bit) -> %s\n", mountOk ? "ok" : "FAILED");
		#endif
		if (!mountOk) {
			#ifdef DEBUG_SERIAL
			Serial.println("SD card initialization failed!");
			#endif
			File file = FILESYS.open("/config.json", "w");
			JsonDocument doc;
			doc["use_sdcard"] = false;
			file.close();
			sdMounted = false;
			full_restart();
			return;
		}
		uint8_t cardType = SD_MMC.cardType();
		#ifdef DEBUG_SERIAL
		const char* cardTypeName =
			cardType == CARD_NONE ? "NONE" :
			cardType == CARD_MMC  ? "MMC"  :
			cardType == CARD_SD   ? "SDSC" :
			cardType == CARD_SDHC ? "SDHC" : "UNKNOWN";
		Serial.printf("SD_MMC.cardType() -> %s (%d)\n", cardTypeName, cardType);
		#endif
		if (cardType == CARD_NONE) {
			#ifdef DEBUG_SERIAL
			Serial.println("No SD card attached");
			#endif
			return;
		}
		#ifdef DEBUG_SERIAL
		Serial.printf("SD card size: %llu MB\n", SD_MMC.cardSize() / (1024ULL * 1024ULL));
		#endif
		sdMounted = true;
	}

	// ==================== INITIALIZE RCM INJECTOR ====================
	// This runs BEFORE WiFi to ensure USB host is ready immediately
	RCM_LOG_I("Initializing RCM Injector...");
	bluetoothConfigEnabled = config.use_bluetooth; // used by rcm_pause_wireless_for_injection()

	usb_host_config_t host_config = {};  // All fields zeroed/false/NULL
	host_config.skip_phy_setup = false;
	host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
	#if WITH_ENUM_FILTER_WORKAROUND
	host_config.enum_filter_cb = usb_enum_filter;
	#endif

	esp_err_t err = usb_host_install(&host_config);
	if (err != ESP_OK) {
		RCM_LOG_E("USB Host install failed: %d", err);
	} else {
		RCM_LOG_I("USB Host installed successfully");

		// Create RCM tasks - higher priority than WiFi tasks
		xTaskCreatePinnedToCore(rcm_usb_host_task, "rcm_usb_host", 4096, NULL, 20, &rcm_usb_task_handle, 0);
		xTaskCreatePinnedToCore(rcm_injection_task, "rcm_inject", 8192, NULL, 19, &rcm_injection_task_handle, 0);

		RCM_LOG_I("RCM Injector ready - waiting for Nintendo Switch in RCM mode...");
	}

	// ==================== POPCORN UI: DISPLAY / TOUCH / AUDIO / BUTTONS ====================
	// Runs after RCM init and SD mount above, but before WiFi/BLE so the
	// animated screen comes up quickly.

	// Initialize Power & Audio Subsystem
	pinMode(PA, OUTPUT);
	digitalWrite(PA, HIGH);

	initAudioSystem();
	startMODPlayer();

	// ---- Buttons (popcorn's PWR + BOOT debounced buttons) ----
	expander.pinMode(PWR_BUTTON_EXPANDER_PIN, INPUT);
	pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

	// Seed debounce state with the actual current reading so we don't get a
	// spurious "press" the first time pollButtons() runs.
	pwrButton.rawState = pwrButton.stableState = expander.digitalRead(PWR_BUTTON_EXPANDER_PIN);
	pwrButton.armed = !(pwrButton.activeHigh ? pwrButton.stableState : !pwrButton.stableState);

	bootButton.rawState = bootButton.stableState = digitalRead(BOOT_BUTTON_PIN);
	bootButton.armed = !(bootButton.activeHigh ? bootButton.stableState : !bootButton.stableState);

	// ---- Battery / PMU ----
	// Non-fatal if this fails -- the demo still runs, battery text just says
	// "PMU NOT FOUND" instead of blocking startup.
	pmuOk = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
	if (!pmuOk) {
		//USBSerial.println("PMU not found - battery status will be unavailable");
	}
	
	else {
		// Diagnostic: if the board resets/shuts down again right after waking
		// from a long time off, check this value -- a low reading here points
		// at battery/brownout rather than the button-hold software issue.
		//USBSerial.printf("[PMU] Boot battery voltage: %d mV (connected: %s, charging: %s)\n", power.getBattVoltage(), power.isBatteryConnect() ? "yes" : "no", power.isCharging() ? "yes" : "no");
	}

	while (!FT3168->begin()) {
		//USBSerial.println("Touch init failed");
		delay(500);
	}

	// Clear any spurious touch interrupt the controller fired while it was
	// powering up / starting, and hold off on processing touches for a
	// short grace period so a phantom touch can't toggle showAltText
	// before the user has actually touched the screen.
	touchInterruptFlag = false;
	FT3168->IIC_Interrupt_Flag = false;
	tapPending = false;
	lastTouch = millis();
	touchGuardUntil = millis() + TOUCH_BOOT_GUARD_MS;

	if (!gfx->begin()) {
		//USBSerial.println("Display init failed");
		while (1);
	}

	gfx->setBrightness(255);
	gfx->fillScreen(RGB565_BLACK);

	buildSinTable();
	initStars();

	//USBSerial.printf("Popcorn UI initialized - Font scale: %.1f\n", fontScale);
	// ==================== END POPCORN UI INIT ====================

	// Initialize network (after RCM USB setup)
	if (config.use_wifi && config.wifi_ssid.length() > 0) {
		startWiFi();
		#ifdef DEBUG_SERIAL
		Serial.println("MAC Address: " + WiFi.macAddress());
		#endif
	} else {
		startAccessPoint();
	}

	if (WiFi.status() == WL_CONNECTED) {
		ipStr = WiFi.localIP().toString();
	}
	else if (AP_Running) {
	ipStr = WiFi.softAPIP().toString();  // Fall back to AP IP
	}
	else {
	ipStr = "0.0.0.0";  // No connection
	}

	#ifdef DEBUG_SERIAL
	Serial.println("IP Address: " + ipStr);
	#endif
	
	Start_mdns_service();
	allowFTP = config.allow_ftp;
	ftpSettings.username = config.ftp_username.c_str();
	ftpSettings.password = config.ftp_password.c_str();
	MAX_FILE_SIZE = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	initMemorySystem();
	MAX_FILE_SIZE = ESP.getFreePsram();
	xTaskCreate(dnsServerTask, "DNSServer", 2048, NULL, 3, &dnsTaskHandle);
	freeChunk();
	setupServer();
	if (sdMounted) {
	if (!fileManager.begin(SD)) {
	#ifdef DEBUG_SERIAL
		Serial.println("Failed to initialize file system");
	#endif
	} else {
		fileManager.setServer(&server);
	}
	} else {
	if (!fileManager.begin(FILESYS)) {
		#ifdef DEBUG_SERIAL
		Serial.println("Failed to initialize file system");
		#endif
	} else {
		fileManager.setServer(&server);
	}
	}

	if (allowFTP) startftpserver();
	bootTime = millis();
	long get_time = config.delay_minutes.toInt();
	
	if (get_time >= 0 && get_time <= 65535) TIME2SLEEP = (uint16_t)get_time;
	autosleep = config.auto_sleep;
	String pl = getDefaultPayload();
	#ifdef DEBUG_SERIAL
	if (pl != "") Serial.printf("Default payload %s found\n", pl.c_str());
	else Serial.printf("No payloads found\n");
	#endif

	// ==================== BLE INITIALIZATION ====================
	if (config.use_bluetooth) {
		#ifdef DEBUG_SERIAL
		Serial.println("Initializing Bluetooth...");
		#endif

		String deviceName = "Modchip"; //keep this name or the Andoid app won't find the correct device to connect to.
		deviceName += " (" + ipStr + ")";

		// Make sure it's fully deinitialized first (important after wake-up)
		if (BLEDevice::getInitialized()) {
				BLEDevice::deinit(true);
		}

		BLEDevice::init(deviceName.c_str());

		// Set MTU here - applies to all connections
    BLEDevice::setMTU(512); //default is 20
		
		BLEServer* bleServer = BLEDevice::createServer();
		bleServer->setCallbacks(new MyServerCallbacks());

		BLEService* bleService = bleServer->createService(SERVICE_UUID);

		// IP Characteristic
		ipCharacteristic = bleService->createCharacteristic(
				CHARACTERISTIC_UUID,
				BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
		ipCharacteristic->addDescriptor(new BLE2902());
		ipCharacteristic->setValue(deviceName.c_str());

		// Command & Response Characteristics...
		commandCharacteristic = bleService->createCharacteristic(
				COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE);
		commandCharacteristic->setCallbacks(new MyCallbacks());

		responseCharacteristic = bleService->createCharacteristic(
				RESPONSE_UUID, BLECharacteristic::PROPERTY_NOTIFY);
		responseCharacteristic->addDescriptor(new BLE2902());

		bleService->start();

		BLEAdvertising* advertising = BLEDevice::getAdvertising();
		advertising->addServiceUUID(SERVICE_UUID);
		advertising->setScanResponse(true);
		advertising->start();

		#ifdef DEBUG_SERIAL
		Serial.println("BLE Advertising started as: " + deviceName);
		#endif
	}

	//allowed with included font.h - % * ? , - . = + $ # @ : ! | [ ] { } < > 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ
	mainmessage = "IP:" + ipStr + " "
								"*** Elysium Firmware: " + firmwareVersion + " *** Created by MrDude *** "
								"For , Mcpicklerick , ...... why are you still reading this? you must be bored ,,,";
	scrollTextOriginal = mainmessage.c_str();
	scrollText = scrollTextOriginal;
}

void loop() {
	if (rcm_injection_done && rcm_usb_task_handle != NULL) {
		full_restart(); //just reboot the waveshare amoled dongle, we can then do multiple injections....
	}
	
	// OTA handling
	if (doOta) {
		doOta = false;
		xTaskCreatePinnedToCore(otaUpdateTask, "OTA", 4096, NULL, 4, &otaTaskHandle, 0);
		startOTA(ota_url);
	}

	if (removeconf) removeconfig();

	// ==================== POPCORN UI: BUTTONS + ANIMATED DISPLAY ====================
	pollButtons();

	// Shake the board 3 times within 2 seconds -> switch to a different song.
	// qmi8658_poll_for_shake() is cheap to call every loop (it self-throttles
	// its I2C reads) and only ever returns true on the exact loop a shake
	// gesture completes.
	if (qmi8658_poll_for_shake()) {
		switchToRandomSong();
	}

	bool touched = (touchInterruptFlag || FT3168->IIC_Interrupt_Flag) && (millis() >= touchGuardUntil);

	if (touched) {
		touchInterruptFlag = false;
		FT3168->IIC_Interrupt_Flag = false;

		if (!touchActive && millis() - lastTouch > 300) {
			touchActive = true;
			unsigned long now = millis();

			if (tapPending && (now - tapPendingSince) <= DOUBLE_TAP_WINDOW_MS) {
				// Second tap arrived in time -> double-tap: toggle the screen
				// (same action as handleBootPress()) and cancel the pending
				// single-tap text toggle.
				tapPending = false;
				handleBootPress();
				//USBSerial.printf("[Touch] Double-tap -> screen %s\n", screenOn ? "ON" : "OFF");
			} else {
				// First tap of a possible pair -- hold off on the text toggle
				// to see if a second tap follows within the window.
				tapPending = true;
				tapPendingSince = now;
			}

			lastTouch = now;
		}
	} else {
		touchActive = false;
	}

	// No second tap arrived in time -> treat the pending tap as a single tap.
	if (tapPending && (millis() - tapPendingSince) > DOUBLE_TAP_WINDOW_MS) {
		tapPending = false;

		showAltText = !showAltText;
		if (showAltText) {
			updateBatteryStatusText(); // refresh immediately so it's not stale on first show
			setScrollText(batteryStatusText);
			lastBatteryUpdate = millis();
		} else {
			setScrollText(scrollTextOriginal);
		}
		//USBSerial.printf("[Touch] Showing %s text\n", showAltText ? "BATTERY" : "ORIGINAL");
	}

	if (!stopui) {
		// While battery text is on screen, refresh it roughly once a second.
		if (showAltText && (millis() - lastBatteryUpdate >= BATTERY_UPDATE_INTERVAL_MS)) {
			updateBatteryStatusText();
			lastBatteryUpdate = millis();
		}

		// After a BOOT-hold payload cycle, show the new selection briefly, then
		// fall back to whatever the scroller was showing before (battery or the
		// welcome text, per showAltText).
		if (showingPayloadStatus && (millis() - payloadStatusShownAt >= PAYLOAD_STATUS_DISPLAY_MS)) {
			showingPayloadStatus = false;
			setScrollText(showAltText ? batteryStatusText : scrollTextOriginal);
		}

		if (screenOn) {
			updateAndDrawStars();
		}
	}
	// ==================== END POPCORN UI ====================

	if (multimedia) {
		stopui = false;
		resumeAudioEngine(); // no-op if already running
	} else {
		stopui = true;
		stopAudioEngine(); // no-op if already suspended
		gfx->fillScreen(RGB565_BLACK);
	}

	//TAR Download/Upload/Extraction
	if (activeOperation) {
		handleDownloadState();
	}

	if (autosleep && TIME2SLEEP > 0) {
		if (millis() >= (bootTime + (TIME2SLEEP * 60000UL))) {
			deepsleep();
		}
	}

	static unsigned long lastCleanup = 0;
	unsigned long now = millis();
	
	// Cleanup every 1s
	if (now - lastCleanup > 1000) {
		lastCleanup = now;
		ws.cleanupClients();
	}

	vTaskDelay(pdMS_TO_TICKS(5));
}