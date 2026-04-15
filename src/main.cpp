// =============================================================================
// HUB75 SLIDESHOW v3 - FINAL BUILD
// SD Card (primary) + SPIFFS (fallback)
// Supports 24-bit, 16-bit, 8-bit .bin images — 64x64 FM6126A panel
//
// SD WIRING:
//   GND  -> GND
//   VCC  -> 5V    (module has onboard 3.3V LDO regulator)
//   MISO -> GPIO 35  (input-only, safest for MISO)
//   MOSI -> GPIO 17
//   SCK  -> GPIO 5
//   CS   -> GPIO 33
//
// SERIAL COMMANDS:
//   n        = next image
//   s        = status
//   l        = list all images
//   r        = rescan storage
//   g<num>   = jump to image (e.g. g42)
//   1-999    = set slideshow interval in minutes
//
// PlatformIO CLI:
//ctr ship p    "core cli"
//pio run --target uploadfs
//   pio run --target upload
//   pio run --target erase
// =============================================================================

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <SPIFFS.h>
#include <SD.h>
#include <SPI.h>

using fs::File;

// =============================================================================
// PIN CONFIGURATION - HUB75
// =============================================================================
#define R1_PIN  25
#define G1_PIN  26
#define B1_PIN  27
#define R2_PIN  13
#define G2_PIN  12
#define B2_PIN  14
#define A_PIN   23
#define B_PIN   19
#define C_PIN   21
#define D_PIN   22
#define E_PIN   18
#define LAT_PIN  4
#define CLK_PIN 16
#define OE_PIN  15

// =============================================================================
// PIN CONFIGURATION - BUTTON & SD
// =============================================================================
#define BUTTON_PIN      32
#define BUTTON_DEBOUNCE 50

#define SD_CS_PIN  33
#define SD_MISO    35   // input-only GPIO, safest for MISO
#define SD_MOSI    17
#define SD_SCK      5

// =============================================================================
// PANEL CONFIGURATION
// =============================================================================
#define PANEL_RES_X 64
#define PANEL_RES_Y 64
#define PANEL_CHAIN  1
#define PIXEL_COUNT (PANEL_RES_X * PANEL_RES_Y)  // 4096 pixels

// =============================================================================
// IMAGE FORMAT CONSTANTS
// =============================================================================
#define MODE_24BIT  24
#define MODE_16BIT  16
#define MODE_8BIT    8

// File sizes (excluding 1-byte header):
//   24-bit: 64*64*3              = 12,288 bytes (+1 header = 12,289)
//   16-bit: 64*64*2              =  8,192 bytes (+1 header =  8,193)
//   8-bit:  768 palette + 4096 i =  4,864 bytes (+1 header =  4,865)

#define MAX_FILE_SIZE  12289   // worst case: 24-bit + header
#define PALETTE_SIZE    768    // 256 colors x 3 bytes RGB

// =============================================================================
// TIMING
// =============================================================================
#define AUTO_CHANGE_MINUTES  1
#define TRANSITION_DURATION  1000  // crossfade ms

// =============================================================================
// STORAGE
// =============================================================================
#define MAX_IMAGES 1000

enum StorageType {
    STORAGE_NONE,
    STORAGE_SPIFFS,
    STORAGE_SD
};

// =============================================================================
// GLOBALS
// =============================================================================
MatrixPanel_I2S_DMA* display = nullptr;

uint16_t* currentFrame = nullptr;  // 8KB RGB565 frame buffer
uint16_t* nextFrame    = nullptr;  // 8KB RGB565 frame buffer
uint8_t*  fileBuffer   = nullptr;  // ~12KB raw file read buffer

String      imageFiles[MAX_IMAGES];
int         imageCount        = 0;
int         currentImageIndex = 0;
int         nextImageIndex    = 0;

StorageType activeStorage = STORAGE_NONE;

// Slideshow interval — uncomment real minutes for production
//unsigned long autoChangeInterval = (unsigned long)AUTO_CHANGE_MINUTES * 60UL * 1000UL;
unsigned long autoChangeInterval  = (unsigned long)AUTO_CHANGE_MINUTES * 60UL * 100UL;  // fast for testing
unsigned long lastChangeTime      = 0;
unsigned long transitionStartTime = 0;
bool          isTransitioning     = false;

unsigned long lastButtonPress = 0;
bool          lastButtonState = HIGH;

String        serialBuffer = "";

// SPI bus for SD card (VSPI - separate from HUB75 I2S)
SPIClass spiSD(VSPI);

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================
void startTransition();
bool loadImageToFrame(int index, uint16_t* frame);
void displayFrame(uint16_t* frame);

// =============================================================================
// COLOR CONVERSION HELPERS
// =============================================================================
inline uint16_t rgb888_to_565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

inline void rgb565_to_888(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = ((c >> 11) & 0x1F) << 3;
    g = ((c >>  5) & 0x3F) << 2;
    b = ( c        & 0x1F) << 3;
}

// =============================================================================
// LOADING COUNTER - shown on HUB75 while SD scans files
// Displays count every 10 files to avoid slowing the scan
// =============================================================================
void showLoadingCount(int count) {
    if (count % 10 != 0 && count != 0 && count != 1) return;

    display->fillScreen(display->color565(0, 0, 0));

    // "Loading SD..." header in cyan
    display->setTextSize(1);
    display->setTextColor(display->color565(0, 220, 220));
    display->setCursor(4, 4);
    display->print("Loading SD...");

    // Big centered count number in white
    display->setTextSize(2);
    display->setTextColor(display->color565(255, 255, 255));
    String countStr = String(count);
    int textWidth   = countStr.length() * 12;  // textSize 2 = ~12px per char
    int x           = (PANEL_RES_X - textWidth) / 2;
    display->setCursor(x, 26);
    display->print(countStr);

    // "images found" footer in dim grey
    display->setTextSize(1);
    display->setTextColor(display->color565(80, 80, 80));
    display->setCursor(4, 52);
    display->print("Found");
}

// =============================================================================
// OPEN FILE FROM ACTIVE STORAGE
// =============================================================================
File openImageFile(const String& path) {
    if (activeStorage == STORAGE_SD) {
        return SD.open(path, FILE_READ);
    } else {
        return SPIFFS.open(path, "r");
    }
}

// =============================================================================
// DECODE IMAGE FROM FILE BUFFER
// Returns true on success; decoded RGB565 stored in 'frame'
// =============================================================================
bool decodeImage(uint8_t* data, size_t dataSize, uint16_t* frame) {
    if (dataSize < 2) return false;

    uint8_t  mode        = data[0];
    uint8_t* payload     = data + 1;
    size_t   payloadSize = dataSize - 1;

    switch (mode) {

        case MODE_24BIT: {
            // 12,288 bytes packed RGB888
            if (payloadSize < (size_t)(PIXEL_COUNT * 3)) {
                Serial.printf("24-bit: expected %d, got %d\n", PIXEL_COUNT * 3, payloadSize);
                return false;
            }
            for (int i = 0; i < PIXEL_COUNT; i++) {
                frame[i] = rgb888_to_565(
                    payload[i * 3],
                    payload[i * 3 + 1],
                    payload[i * 3 + 2]
                );
            }
            Serial.print("[24-bit] ");
            return true;
        }

        case MODE_16BIT: {
            // 8,192 bytes packed RGB565 little-endian
            if (payloadSize < (size_t)(PIXEL_COUNT * 2)) {
                Serial.printf("16-bit: expected %d, got %d\n", PIXEL_COUNT * 2, payloadSize);
                return false;
            }
            memcpy(frame, payload, PIXEL_COUNT * 2);
            Serial.print("[16-bit] ");
            return true;
        }

        case MODE_8BIT: {
            // 768-byte palette + 4096 indices
            if (payloadSize < (size_t)(PALETTE_SIZE + PIXEL_COUNT)) {
                Serial.printf("8-bit: expected %d, got %d\n", PALETTE_SIZE + PIXEL_COUNT, payloadSize);
                return false;
            }
            uint16_t palette565[256];
            for (int i = 0; i < 256; i++) {
                palette565[i] = rgb888_to_565(
                    payload[i * 3],
                    payload[i * 3 + 1],
                    payload[i * 3 + 2]
                );
            }
            uint8_t* indices = payload + PALETTE_SIZE;
            for (int i = 0; i < PIXEL_COUNT; i++) {
                frame[i] = palette565[indices[i]];
            }
            Serial.print("[8-bit] ");
            return true;
        }

        default: {
            // Legacy: old 24-bit raw with no header byte
            if (dataSize == 12288) {
                Serial.print("[Legacy 24-bit] ");
                for (int i = 0; i < PIXEL_COUNT; i++) {
                    frame[i] = rgb888_to_565(
                        data[i * 3],
                        data[i * 3 + 1],
                        data[i * 3 + 2]
                    );
                }
                return true;
            }
            Serial.printf("Unknown format: header=%d, size=%d\n", mode, dataSize);
            return false;
        }
    }
}

// =============================================================================
// LOAD IMAGE FILE AND DECODE TO FRAME
// =============================================================================
bool loadImageToFrame(int index, uint16_t* frame) {
    if (index < 0 || index >= imageCount) return false;

    Serial.printf("Load [%d] %s ... ", index, imageFiles[index].c_str());

    File f = openImageFile(imageFiles[index]);
    if (!f) { Serial.println("OPEN FAILED"); return false; }

    size_t fileSize = f.size();
    if (fileSize > MAX_FILE_SIZE) {
        Serial.printf("FILE TOO LARGE (%d bytes)\n", fileSize);
        f.close();
        return false;
    }

    size_t bytesRead = f.read(fileBuffer, fileSize);
    f.close();

    if (bytesRead != fileSize) {
        Serial.printf("READ ERROR (got %d of %d)\n", bytesRead, fileSize);
        return false;
    }

    if (!decodeImage(fileBuffer, bytesRead, frame)) {
        Serial.println("DECODE FAILED");
        return false;
    }

    Serial.println("OK");
    return true;
}

// =============================================================================
// DISPLAY FRAME (RGB565) ON MATRIX
// =============================================================================
void displayFrame(uint16_t* frame) {
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            display->drawPixel(x, y, frame[y * PANEL_RES_X + x]);
        }
    }
}

// =============================================================================
// CROSSFADE BETWEEN TWO FRAMES
// progress: 0.0 = full 'from', 1.0 = full 'to'
// =============================================================================
void crossFadeFrames(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    float q  = 1.0f - progress;
    uint8_t r1, g1, b1, r2, g2, b2;

    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            int i = y * PANEL_RES_X + x;
            rgb565_to_888(from[i], r1, g1, b1);
            rgb565_to_888(to[i],   r2, g2, b2);
            display->drawPixel(x, y, display->color565(
                (uint8_t)(r1 * q + r2 * progress),
                (uint8_t)(g1 * q + g2 * progress),
                (uint8_t)(b1 * q + b2 * progress)
            ));
        }
    }
}

// =============================================================================
// JUMP TO NEXT IMAGE (no fade)
// =============================================================================
void jumpToNext() {
    isTransitioning = false;
    if (imageCount == 0) return;

    int target = (currentImageIndex + 1) % imageCount;
    Serial.printf("Jump -> image %d\n", target);

    if (loadImageToFrame(target, currentFrame)) {
        currentImageIndex = target;
        lastChangeTime    = millis();
        displayFrame(currentFrame);
        Serial.printf("Now showing image %d\n", currentImageIndex);
    }
}

// =============================================================================
// START CROSSFADE TRANSITION TO NEXT IMAGE
// =============================================================================
void startTransition() {
    if (isTransitioning) {
        Serial.println("Already transitioning - hard jump");
        jumpToNext();
        return;
    }
    if (imageCount < 2) {
        jumpToNext();
        return;
    }

    nextImageIndex = (currentImageIndex + 1) % imageCount;

    if (!loadImageToFrame(nextImageIndex, nextFrame)) {
        Serial.println("Load failed - skip");
        return;
    }

    isTransitioning     = true;
    transitionStartTime = millis();
    Serial.printf("Fade: %d -> %d\n", currentImageIndex, nextImageIndex);
}

// =============================================================================
// UPDATE CROSSFADE (called every loop)
// =============================================================================
void updateTransition() {
    if (!isTransitioning) return;

    float progress = (float)(millis() - transitionStartTime) / (float)TRANSITION_DURATION;

    if (progress >= 1.0f) {
        // Fade complete - swap buffers
        isTransitioning   = false;
        currentImageIndex = nextImageIndex;
        lastChangeTime    = millis();

        uint16_t* tmp = currentFrame;
        currentFrame  = nextFrame;
        nextFrame     = tmp;

        displayFrame(currentFrame);
        Serial.printf("Fade done -> image %d\n", currentImageIndex);
    } else {
        crossFadeFrames(currentFrame, nextFrame, progress);
    }
}

// =============================================================================
// INIT DISPLAY
// =============================================================================
void initDisplay() {
    HUB75_I2S_CFG::i2s_pins pins = {
        R1_PIN, G1_PIN, B1_PIN,
        R2_PIN, G2_PIN, B2_PIN,
        A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
        LAT_PIN, OE_PIN, CLK_PIN
    };

    HUB75_I2S_CFG config(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN, pins);
    config.clkphase = false;
    config.driver   = HUB75_I2S_CFG::FM6126A;

    display = new MatrixPanel_I2S_DMA(config);
    display->begin();
    display->setBrightness8(90);
    display->clearScreen();

    Serial.println("Display OK");
}

// =============================================================================
// SCAN DIRECTORY FOR .BIN FILES
// Recursive - finds files in subfolders
// Updates HUB75 loading counter as each file is found
// =============================================================================
void scanDirectory(File dir, const String& path) {
    while (imageCount < MAX_IMAGES) {
        File entry = dir.openNextFile();
        if (!entry) break;

        String name = String(entry.name());

        if (entry.isDirectory()) {
            String subPath = path + "/" + name;
            scanDirectory(entry, subPath);
        } else {
            String lowerName = name;
            lowerName.toLowerCase();

            if (lowerName.endsWith(".bin")) {
                String fullPath = path + "/" + name;
                fullPath.replace("//", "/");

                Serial.printf("  [%3d] %-30s %6d bytes\n",
                              imageCount, fullPath.c_str(), entry.size());

                imageFiles[imageCount++] = fullPath;

                // Live counter on HUB75 during scan
                showLoadingCount(imageCount);
            }
        }
        entry.close();
    }
}

// =============================================================================
// INIT SD CARD
// =============================================================================
bool initSD() {
    Serial.println("\n--- Checking SD Card ---");

    spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS_PIN);
    delay(500);  // give card time to power up

    // if (!SD.begin(SD_CS_PIN, spiSD, 4000000)) {  // 4MHz - safe for cheap modules
   if (!SD.begin(SD_CS_PIN, spiSD )) {
        Serial.println("SD card not found or failed to initialize");
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("No SD card attached");
        return false;
    }

    Serial.print("SD Card Type: ");
    switch (cardType) {
        case CARD_MMC:  Serial.println("MMC");     break;
        case CARD_SD:   Serial.println("SDSC");    break;
        case CARD_SDHC: Serial.println("SDHC");    break;
        default:        Serial.println("UNKNOWN"); break;
    }
    Serial.printf("SD Card Size: %lluMB\n", SD.cardSize() / (1024 * 1024));
    Serial.printf("Used space:   %lluMB\n", SD.usedBytes()  / (1024 * 1024));

    // Show initial loading screen before scan starts
    showLoadingCount(0);

    Serial.println("\nScanning for .bin images...");
    File root = SD.open("/");
    if (!root) {
        Serial.println("Failed to open SD root directory");
        return false;
    }

    scanDirectory(root, "");
    root.close();

    if (imageCount > 0) {
        Serial.printf("\nFound %d images on SD card\n", imageCount);
        activeStorage = STORAGE_SD;
        return true;
    }

    Serial.println("No .bin images found on SD card");
    return false;
}

// =============================================================================
// INIT SPIFFS (fallback if no SD card)
// =============================================================================
bool initSPIFFS() {
    Serial.println("\n--- Checking SPIFFS ---");

    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS MOUNT FAILED");
        return false;
    }

    Serial.printf("SPIFFS total: %d bytes, used: %d bytes\n",
                  SPIFFS.totalBytes(), SPIFFS.usedBytes());
    Serial.printf("Free space:   %d bytes\n",
                  SPIFFS.totalBytes() - SPIFFS.usedBytes());

    Serial.println("\nScanning for .bin images...");
    File root = SPIFFS.open("/");
    File file = root.openNextFile();

    while (file && imageCount < MAX_IMAGES) {
        String name  = String(file.name());
        size_t fsize = file.size();

        if (name.endsWith(".bin")) {
            Serial.printf("  %-25s %6d bytes\n", name.c_str(), fsize);
            imageFiles[imageCount++] = name.startsWith("/") ? name : "/" + name;
            showLoadingCount(imageCount);
        }
        file = root.openNextFile();
    }

    if (imageCount > 0) {
        Serial.printf("\nFound %d images in SPIFFS\n", imageCount);
        activeStorage = STORAGE_SPIFFS;
        return true;
    }

    Serial.println("No .bin images found in SPIFFS");
    return false;
}

// =============================================================================
// INIT STORAGE - tries SD first, falls back to SPIFFS
// =============================================================================
void initStorage() {
    imageCount    = 0;
    activeStorage = STORAGE_NONE;

    if (initSD()) {
        Serial.println("\n*** Using SD Card for images ***");
        return;
    }
    if (initSPIFFS()) {
        Serial.println("\n*** Using SPIFFS for images ***");
        return;
    }

    Serial.println("\n*** NO STORAGE AVAILABLE ***");
}

// =============================================================================
// INIT BUTTON
// =============================================================================
void initButton() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.printf("Button on GPIO %d\n", BUTTON_PIN);
}

// =============================================================================
// CHECK BUTTON (debounced)
// =============================================================================
void checkButton() {
    bool          state = digitalRead(BUTTON_PIN);
    unsigned long now   = millis();

    if (state == LOW && lastButtonState == HIGH) {
        if (now - lastButtonPress > BUTTON_DEBOUNCE) {
            Serial.println("--- BUTTON ---");
            lastButtonPress = now;
            lastChangeTime  = now;
            startTransition();
        }
    }
    lastButtonState = state;
}

// =============================================================================
// CHECK AUTO CHANGE TIMER
// =============================================================================
void checkAutoChange() {
    if (isTransitioning) return;
    if (millis() - lastChangeTime >= autoChangeInterval) {
        Serial.println("--- TIMER ---");
        startTransition();
    }
}

// =============================================================================
// CHECK SERIAL COMMANDS
// =============================================================================
void checkSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            serialBuffer.trim();
            if (serialBuffer.length() == 0) { serialBuffer = ""; return; }

            if (serialBuffer.equalsIgnoreCase("n")) {
                Serial.println("Serial: next");
                lastChangeTime = millis();
                startTransition();
            }
            else if (serialBuffer.equalsIgnoreCase("s")) {
                Serial.println("\n=== STATUS ===");
                Serial.printf("Storage:  %s\n",
                    activeStorage == STORAGE_SD     ? "SD Card" :
                    activeStorage == STORAGE_SPIFFS ? "SPIFFS"  : "None");
                Serial.printf("Images:   %d\n", imageCount);
                Serial.printf("Current:  [%d] %s\n", currentImageIndex,
                              imageFiles[currentImageIndex].c_str());
                Serial.printf("Interval: %lu min\n", autoChangeInterval / 60000UL);
                Serial.printf("Heap:     %d bytes free\n", ESP.getFreeHeap());
                if (activeStorage == STORAGE_SD)
                    Serial.printf("SD free:  %llu MB\n",
                        (SD.totalBytes() - SD.usedBytes()) / (1024 * 1024));
                else if (activeStorage == STORAGE_SPIFFS)
                    Serial.printf("SPIFFS free: %d bytes\n",
                        SPIFFS.totalBytes() - SPIFFS.usedBytes());
            }
            else if (serialBuffer.equalsIgnoreCase("l")) {
                Serial.println("\n=== IMAGE LIST ===");
                for (int i = 0; i < imageCount; i++) {
                    Serial.printf("[%3d] %s%s\n", i, imageFiles[i].c_str(),
                        i == currentImageIndex ? " <-- CURRENT" : "");
                }
            }
            else if (serialBuffer.equalsIgnoreCase("r")) {
                Serial.println("Rescanning storage...");
                initStorage();
                if (imageCount > 0) {
                    currentImageIndex = 0;
                    loadImageToFrame(0, currentFrame);
                    displayFrame(currentFrame);
                }
            }
            else if (serialBuffer.startsWith("g") || serialBuffer.startsWith("G")) {
                int targetIdx = serialBuffer.substring(1).toInt();
                if (targetIdx >= 0 && targetIdx < imageCount) {
                    Serial.printf("Going to image %d\n", targetIdx);
                    if (loadImageToFrame(targetIdx, currentFrame)) {
                        currentImageIndex = targetIdx;
                        lastChangeTime    = millis();
                        displayFrame(currentFrame);
                    }
                } else {
                    Serial.printf("Invalid index. Range: g0 to g%d\n", imageCount - 1);
                }
            }
            else {
                int mins = serialBuffer.toInt();
                if (mins >= 1 && mins <= 999) {
                    autoChangeInterval = (unsigned long)mins * 60UL * 1000UL;
                    lastChangeTime     = millis();
                    Serial.printf("Interval set to %d minute(s)\n", mins);
                } else {
                    Serial.println("Commands: n=next  s=status  l=list  r=rescan  g<n>=goto  1-999=minutes");
                }
            }
            serialBuffer = "";
        } else {
            serialBuffer += c;
        }
    }
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== HUB75 SLIDESHOW v3 - FINAL BUILD ===");
    Serial.println("SD Card (primary) + SPIFFS (fallback)");
    Serial.println("Supports 24-bit, 16-bit, 8-bit images\n");

    // Allocate frame buffers
    currentFrame = (uint16_t*)malloc(PIXEL_COUNT * 2);  // 8KB
    nextFrame    = (uint16_t*)malloc(PIXEL_COUNT * 2);  // 8KB
    fileBuffer   = (uint8_t*)malloc(MAX_FILE_SIZE);     // ~12KB

    if (!currentFrame || !nextFrame || !fileBuffer) {
        Serial.println("MALLOC FAILED - not enough heap!");
        while (true) delay(1000);
    }

    Serial.printf("Free heap after alloc: %d bytes\n", ESP.getFreeHeap());

    // Init display first - needed to show loading screen during SD scan
    initDisplay();

    // Init storage - shows live counter on HUB75 while scanning files
    initStorage();

    initButton();

    if (imageCount == 0) {
        // No images - show error on panel
        display->fillScreen(display->color565(60, 0, 0));
        display->setTextSize(1);
        display->setTextColor(display->color565(255, 80, 0));
        display->setCursor(4, 10);
        display->print("NO IMAGES");
        display->setCursor(4, 22);
        display->setTextColor(display->color565(150, 150, 150));
        display->print("SD: copy .bin");
        display->setCursor(4, 32);
        display->print("to card root");

        Serial.println("No .bin files found!");
        Serial.println("SD: copy .bin files to SD card root or subfolders");
    } else {
        // Load and display first image
        if (loadImageToFrame(0, currentFrame)) {
            delay(200);
            displayFrame(currentFrame);
            displayFrame(currentFrame);  // double-draw ensures DMA buffer is flushed
            currentImageIndex = 0;
            Serial.println("First image displayed");
        } else {
            display->fillScreen(display->color565(150, 100, 0));
            Serial.println("Failed to load first image");
        }
    }

    lastChangeTime = millis();

    Serial.println("\n========== READY ==========");
    Serial.printf("Storage:  %s\n",
        activeStorage == STORAGE_SD     ? "SD Card" :
        activeStorage == STORAGE_SPIFFS ? "SPIFFS"  : "None");
    Serial.printf("Images:   %d\n", imageCount);
    Serial.printf("Interval: %lu min\n", autoChangeInterval / 60000UL);
    Serial.printf("Button:   GPIO%d\n", BUTTON_PIN);
    Serial.println("Commands: n=next  s=status  l=list  r=rescan  g<n>=goto  1-999=minutes");
    Serial.println("===========================\n");
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
    checkSerial();
    checkButton();
    checkAutoChange();
    updateTransition();
    delay(10);
}