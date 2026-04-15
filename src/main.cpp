
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

#include "fx.h"

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
#define TRANSITION_DURATION  3000  // crossfade ms

// =============================================================================
// STORAGE
// =============================================================================
#define MAX_IMAGES 2000

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


 
enum TransitionType {
    TRANS_CROSSFADE = 0,
    TRANS_WAVE_RIPPLE = 1,
    TRANS_PIXEL_DISSOLVE = 2,
    TRANS_GLITCH_NOISE = 3,
    TRANS_4X4_BLOCKS = 4,
    TRANS_IRIS_WIPE = 5,
    TRANS_WATER = 6,
    TRANS_GENERATIVE = 7,
    TRANS_SMOKE = 8,
    ORIGINAL = 9,
    TRANS_PLASMA = 10,
    TRANS_FIRE = 11,
    TRANS_ICE_COLD = 12,
    TRANS_SWIRL = 13,
    TRANS_SCANLINE = 14,
    TRANS_PINWHEEL = 15,
    TRANS_MOSAIC = 16,
    TRANS_BLUR = 17,
    TRANS_SPLIT = 18,
    TRANS_BOUNCE = 19,
    TRANS_PARTICLES = 20,      // New: flying particle explosion
    TRANS_ATOMIC = 21,         // New: nuclear blast / shockwave
    TRANS_ZIGZAG = 22,         // New: zigzag pattern wipe
    TRANS_OIL_PAINTING = 23,   // New: painterly smear effect
    TRANS_MORPH = 24,          // New: organic shape morphing
    TRANS_BUMP = 25,           // New: bump map / displacement
    TRANS_STARBURST = 26,      // New: radial burst from center
    TRANS_RAIN = 27,           // New: falling rain drops
    TRANS_SHATTER = 28,        // New: glass shatter effect
    TRANS_WIPES = 29,          // New: multiple wipes directions
    TRANS_MAX
};


TransitionType currentTransitionType = TRANS_CROSSFADE;
unsigned long currentTransitionDuration = 1000;



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
    display->print("Loading SD");

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
    //display->print("Found");
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
// MATH / NOISE HELPERS FOR TRANSITIONS
// =============================================================================

// Simple and fast pseudo-random hash based on coordinates
inline float hash21(int x, int y) {
    uint32_t a = x * 3284157443 + y * 1911520717;
    a ^= a << 10;
    a ^= a >> 15;
    a *= 2048419325;
    a ^= a << 10;
    a ^= a >> 15;
    return (float)a / 4294967295.0f;
}

// Fast 2D value noise
float valueNoise(float x, float y) {
    int ix = (int)floor(x);
    int iy = (int)floor(y);
    float fx = x - ix;
    float fy = y - iy;

    // Smoothstep interpolation
    float ux = fx * fx * (3.0f - 2.0f * fx);
    float uy = fy * fy * (3.0f - 2.0f * fy);

    float n00 = hash21(ix, iy);
    float n10 = hash21(ix + 1, iy);
    float n01 = hash21(ix, iy + 1);
    float n11 = hash21(ix + 1, iy + 1);

    float nx0 = n00 * (1.0f - ux) + n10 * ux;
    float nx1 = n01 * (1.0f - ux) + n11 * ux;

    return nx0 * (1.0f - uy) + nx1 * uy;
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
// TRANSITION 1: WAVE / RIPPLE
// =============================================================================
void renderTransitionWaveRipple(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    float offsetMagnitude = (1.0f - progress) * 32.0f; 

    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            // Ripple carries Image B over Image A
            float wave = sinf((float)y * 0.2f + progress * 10.0f) * offsetMagnitude;
            int srcX = x + (int)wave;
            
            if (progress > (float)x / PANEL_RES_X) {
                // Image B sweeping in
                if (srcX >= 0 && srcX < PANEL_RES_X) {
                    display->drawPixel(x, y, to[y * PANEL_RES_X + srcX]);
                } else {
                    display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
                }
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 2: PIXEL DISSOLVE
// =============================================================================
void renderTransitionPixelDissolve(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            float threshold = hash21(x, y);
            if (progress > threshold) {
                display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 3: GLITCH / NOISE
// =============================================================================
void renderTransitionGlitchNoise(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        // Horizontal glitch shifting
        int shift = 0;
        if (hash21(y, (int)(progress * 20.0f)) > 0.9f) {
            shift = (int)((hash21(y, 100) - 0.5f) * 10.0f * (1.0f - progress));
        }

        for (int x = 0; x < PANEL_RES_X; x++) {
            int srcX = constrain(x + shift, 0, PANEL_RES_X - 1);
            float noiseMask = hash21(x, y + (int)(progress * 100.0f));
            
            if (progress > noiseMask) {
                display->drawPixel(x, y, to[y * PANEL_RES_X + srcX]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + srcX]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 4: 4x4 PIXEL BLOCKS
// =============================================================================
void renderTransition4x4Blocks(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            int blockX = x / 4;
            int blockY = y / 4;
            float threshold = hash21(blockX, blockY);
            
            if (progress > threshold) {
                display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 5: IRIS WIPE (Keep the same shape)
// =============================================================================
void renderTransitionIrisWipe(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    
    float centerX = PANEL_RES_X / 2.0f;
    float centerY = PANEL_RES_Y / 2.0f;
    float maxRadius = sqrtf(centerX*centerX + centerY*centerY);
    float currentRadius = maxRadius * progress;

    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            float dx = x - centerX;
            float dy = y - centerY;
            float dist = sqrtf(dx*dx + dy*dy);
            
            if (dist < currentRadius) {
                display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 6: WATER TRANSLUCID
// =============================================================================
void renderTransitionWater(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            // Refraction distortion based on noise
            float n = valueNoise(x * 0.1f, y * 0.1f + progress * 5.0f);
            int distortion = (int)((n - 0.5f) * 10.0f * sinf(progress * 3.14159f));
            
            int srcX = constrain(x + distortion, 0, PANEL_RES_X - 1);
            int srcY = constrain(y + distortion, 0, PANEL_RES_Y - 1);
            
            if (progress > n) {
                display->drawPixel(x, y, to[srcY * PANEL_RES_X + srcX]);
            } else {
                display->drawPixel(x, y, from[srcY * PANEL_RES_X + srcX]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 7: GENERATIVE ART
// =============================================================================
void renderTransitionGenerative(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            // Procedural geometric pattern
            float val = sinf(x * 0.2f) * cosf(y * 0.2f) * 0.5f + 0.5f;
            float mask = fmodf(val + progress, 1.0f);
            
            if (progress > 0.9f || mask < progress) {
                display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 8: TOXIC SMOKE FOG
// =============================================================================
void renderTransitionSmoke(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            // Thick fog noise mask
            float n1 = valueNoise(x * 0.05f, y * 0.05f - progress * 2.0f);
            float n2 = valueNoise(x * 0.1f + 10.0f, y * 0.1f + progress * 3.0f);
            float smokeMask = (n1 + n2) * 0.5f;
            
            if (progress + smokeMask * 0.5f > 0.8f) {
                display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
            } else {
                // Tint to a toxic green slightly if in transition zone
                if (progress > 0.1f && progress + smokeMask > 0.5f) {
                    uint8_t r, g, b;
                    rgb565_to_888(from[y * PANEL_RES_X + x], r, g, b);
                    g = min(255, g + (int)(smokeMask * 100));
                    display->drawPixel(x, y, display->color565(r, g, b));
                } else {
                    display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
                }
            }
        }
    }
}
  
 

// =============================================================================
// CROSSFADE BETWEEN TWO FRAMES
// progress: 0.0 = full 'from', 1.0 = full 'to'
// =============================================================================
void renderTransitionCrossfade(uint16_t* from, uint16_t* to, float progress) {
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
// TRANSITION 10: PLASMA FLOW
// =============================================================================
void renderTransitionPlasma(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    float timePhase = progress * 8.0f;
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            // Create plasma pattern
            float v1 = sinf(x * 0.1f + timePhase);
            float v2 = sinf(y * 0.1f + timePhase * 0.7f);
            float v3 = sinf((x + y) * 0.1f + timePhase * 1.3f);
            float plasma = (v1 + v2 + v3) / 3.0f;  // range -1 to 1
            float mask = (plasma + 1.0f) / 2.0f;   // range 0 to 1
            
            if (progress > mask * 0.8f) {
                display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 11: FIRE / HEAT WAVE
// =============================================================================
void renderTransitionFire(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    float heatIntensity = progress * 1.5f;
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            // Fire distortion - rising heat waves
            float waveY = y - sinf(x * 0.2f + progress * 15.0f) * 8.0f * (1.0f - progress);
            int srcY = constrain((int)waveY, 0, PANEL_RES_Y - 1);
            
            // Fire mask - bottom-to-top flame
            float fireMask = 1.0f - (float)y / PANEL_RES_Y;
            fireMask = powf(fireMask, 0.5f + progress * 2.0f);
            
            if (progress > fireMask) {
                // Apply fire color tint to new image
                uint8_t r, g, b;
                rgb565_to_888(to[srcY * PANEL_RES_X + x], r, g, b);
                // Add red/orange tint for fire
                r = min(255, r + (int)(fireMask * 100));
                g = max(0, g - (int)(fireMask * 80));
                display->drawPixel(x, y, display->color565(r, g, b));
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 12: ICE COLD REVEAL
// =============================================================================
void renderTransitionIceCold(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            // Ice crystal noise
            float iceNoise = valueNoise(x * 0.15f, y * 0.15f + progress * 10.0f);
            float iceMask = (iceNoise + progress) / 2.0f;
            
            // Frost spread - from edges inward
            float edgeDist = min(min(x, PANEL_RES_X - 1 - x), min(y, PANEL_RES_Y - 1 - y));
            float edgeFactor = 1.0f - (edgeDist / (PANEL_RES_X / 2.0f));
            float finalMask = (iceMask + edgeFactor) / 2.0f;
            
            if (progress > finalMask * 0.9f) {
                // Add cold blue tint
                uint8_t r, g, b;
                rgb565_to_888(to[y * PANEL_RES_X + x], r, g, b);
                b = min(255, b + 40);
                g = max(0, g - 20);
                display->drawPixel(x, y, display->color565(r, g, b));
            } else if (progress < 0.3f && finalMask > progress) {
                // Cold tint on old image
                uint8_t r, g, b;
                rgb565_to_888(from[y * PANEL_RES_X + x], r, g, b);
                b = min(255, b + 60);
                display->drawPixel(x, y, display->color565(r, g, b));
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 13: SPINNING SWIRL / VORTEX
// =============================================================================
void renderTransitionSwirl(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    float centerX = PANEL_RES_X / 2.0f;
    float centerY = PANEL_RES_Y / 2.0f;
    float swirlAngle = progress * TWO_PI * 3.0f;
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            float dx = x - centerX;
            float dy = y - centerY;
            float dist = sqrtf(dx*dx + dy*dy);
            float angle = atan2f(dy, dx);
            
            // Spiral distortion
            float newAngle = angle + swirlAngle * (1.0f - dist / (centerX + centerY));
            int srcX = constrain((int)(centerX + cosf(newAngle) * dist), 0, PANEL_RES_X - 1);
            int srcY = constrain((int)(centerY + sinf(newAngle) * dist), 0, PANEL_RES_Y - 1);
            
            float spiralMask = 1.0f - (dist / (centerX + centerY));
            if (progress > spiralMask) {
                display->drawPixel(x, y, to[srcY * PANEL_RES_X + srcX]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 14: SCANLINE WIPE
// =============================================================================
void renderTransitionScanline(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    float scanPosition = progress * PANEL_RES_Y;
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        // Scanline thickness and glow
        float distanceToScan = fabsf(y - scanPosition);
        float glow = 1.0f - min(1.0f, distanceToScan / 3.0f);
        
        for (int x = 0; x < PANEL_RES_X; x++) {
            if (y < scanPosition) {
                // Already scanned - show new image with slight glow
                if (glow > 0.5f) {
                    uint8_t r, g, b;
                    rgb565_to_888(to[y * PANEL_RES_X + x], r, g, b);
                    r = min(255, r + (int)(glow * 80));
                    display->drawPixel(x, y, display->color565(r, g, b));
                } else {
                    display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
                }
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 15: PINWHEEL (Rotating Wedge)
// =============================================================================
void renderTransitionPinwheel(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    float centerX = PANEL_RES_X / 2.0f;
    float centerY = PANEL_RES_Y / 2.0f;
    float wedgeAngle = progress * TWO_PI * 2.0f;
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            float dx = x - centerX;
            float dy = y - centerY;
            float angle = atan2f(dy, dx) + PI;
            
            // Determine if point is within the rotating wedge
            float wedgeStart = wedgeAngle;
            float wedgeEnd = wedgeAngle + PI / 2.0f;
            
            if (wedgeStart > TWO_PI) wedgeStart -= TWO_PI;
            if (wedgeEnd > TWO_PI) wedgeEnd -= TWO_PI;
            
            bool inWedge;
            if (wedgeStart < wedgeEnd) {
                inWedge = (angle >= wedgeStart && angle <= wedgeEnd);
            } else {
                inWedge = (angle >= wedgeStart || angle <= wedgeEnd);
            }
            
            if (inWedge || progress > 0.95f) {
                display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 16: MOSAIC (Expanding Blocks)
// =============================================================================
void renderTransitionMosaic(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    // Block size decreases from large to small
    int blockSize = max(1, (int)(32 * (1.0f - progress)));
    
    for (int y = 0; y < PANEL_RES_Y; y += blockSize) {
        for (int x = 0; x < PANEL_RES_X; x += blockSize) {
            float blockProgress = (progress - (float)x / PANEL_RES_X * 0.5f) * 2.0f;
            blockProgress = constrain(blockProgress, 0.0f, 1.0f);
            
            if (blockProgress > 0.5f) {
                // Fill block with new image
                int sampleX = min(x + blockSize/2, PANEL_RES_X - 1);
                int sampleY = min(y + blockSize/2, PANEL_RES_Y - 1);
                uint16_t color = to[sampleY * PANEL_RES_X + sampleX];
                for (int by = 0; by < blockSize && y + by < PANEL_RES_Y; by++) {
                    for (int bx = 0; bx < blockSize && x + bx < PANEL_RES_X; bx++) {
                        display->drawPixel(x + bx, y + by, color);
                    }
                }
            } else {
                // Show old image in block
                for (int by = 0; by < blockSize && y + by < PANEL_RES_Y; by++) {
                    for (int bx = 0; bx < blockSize && x + bx < PANEL_RES_X; bx++) {
                        display->drawPixel(x + bx, y + by, from[(y+by) * PANEL_RES_X + (x+bx)]);
                    }
                }
            }
        }
    }
}

// =============================================================================
// TRANSITION 17: DIRECTIONAL BLUR WIPE
// =============================================================================
void renderTransitionBlur(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    int blurAmount = (int)(8 * (1.0f - progress));
    float threshold = progress;
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            if ((float)x / PANEL_RES_X < threshold) {
                // Blurred region showing new image
                int offsetX = (int)((hash21(x, y) - 0.5f) * blurAmount);
                int srcX = constrain(x + offsetX, 0, PANEL_RES_X - 1);
                display->drawPixel(x, y, to[y * PANEL_RES_X + srcX]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 18: SPLIT AND SLIDE
// =============================================================================
void renderTransitionSplit(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    int splitX = (int)(PANEL_RES_X * progress);
    int splitY = (int)(PANEL_RES_Y * progress);
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            // Screen splits into quadrants that slide
            bool showNew = false;
            if (x < splitX && y < splitY) showNew = true;           // Top-left
            else if (x >= splitX && y < splitY) showNew = true;     // Top-right  
            else if (x < splitX && y >= splitY) showNew = true;     // Bottom-left
            else if (x >= splitX && y >= splitY) showNew = true;    // Bottom-right
            
            if (showNew) {
                display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 19: BOUNCE / EXPANDING CIRCLE
// =============================================================================
void renderTransitionBounce(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    float centerX = PANEL_RES_X / 2.0f;
    float centerY = PANEL_RES_Y / 2.0f;
    
    // Bounce effect - radius expands, then contracts, then expands
    float bounceProgress = sinf(progress * PI);
    float radius = (centerX + centerY) / 2.0f * bounceProgress;
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            float dx = x - centerX;
            float dy = y - centerY;
            float dist = sqrtf(dx*dx + dy*dy);
            
            if (dist < radius) {
                display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}
 // Add these to your TransitionType enum (before TRANS_MAX):


// =============================================================================
// TRANSITION 20: PARTICLE EXPLOSION
// =============================================================================
void renderTransitionParticles(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    
    // Particle count increases with progress
    int particleCount = (int)(progress * 800);
    
    // First, show mostly old image with particles of new image appearing
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
        }
    }
    
    // Draw exploding particles
    for (int i = 0; i < particleCount; i++) {
        // Use deterministic pseudo-random based on progress and index
        float rnd = hash21(i, (int)(progress * 1000));
        int px = (int)(rnd * PANEL_RES_X);
        int py = (int)(hash21(i, (int)(progress * 2000)) * PANEL_RES_Y);
        
        // Particle velocity - outward from center
        float centerX = PANEL_RES_X / 2.0f;
        float centerY = PANEL_RES_Y / 2.0f;
        float dx = px - centerX;
        float dy = py - centerY;
        float dist = sqrtf(dx*dx + dy*dy);
        float maxDist = sqrtf(centerX*centerX + centerY*centerY);
        float speedFactor = progress * (1.0f - dist / maxDist);
        
        int srcX = constrain(px + (int)(dx * speedFactor), 0, PANEL_RES_X - 1);
        int srcY = constrain(py + (int)(dy * speedFactor), 0, PANEL_RES_Y - 1);
        
        display->drawPixel(px, py, to[srcY * PANEL_RES_X + srcX]);
        
        // Draw particle glow (3x3 block)
        if (i % 3 == 0 && progress > 0.3f) {
            for (int ox = -1; ox <= 1; ox++) {
                for (int oy = -1; oy <= 1; oy++) {
                    int nx = px + ox, ny = py + oy;
                    if (nx >= 0 && nx < PANEL_RES_X && ny >= 0 && ny < PANEL_RES_Y) {
                        if (hash21(i * 10 + ox, oy) > 0.7f) {
                            display->drawPixel(nx, ny, to[srcY * PANEL_RES_X + srcX]);
                        }
                    }
                }
            }
        }
    }
}

// =============================================================================
// TRANSITION 21: ATOMIC EXPLOSION / SHOCKWAVE
// =============================================================================
void renderTransitionAtomic(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    float centerX = PANEL_RES_X / 2.0f;
    float centerY = PANEL_RES_Y / 2.0f;
    
    // Expanding shockwave ring
    float shockwaveRadius = (centerX + centerY) / 2.0f * progress;
    float shockwaveWidth = 6.0f * (1.0f - progress) + 2.0f;
    
    // Flash intensity (bright white at start)
    float flashIntensity = max(0.0f, 1.0f - progress * 3.0f);
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            float dx = x - centerX;
            float dy = y - centerY;
            float dist = sqrtf(dx*dx + dy*dy);
            
            // Shockwave ring
            float distToRing = fabsf(dist - shockwaveRadius);
            
            if (dist < shockwaveRadius - shockwaveWidth) {
                // Inside shockwave - show new image with distortion
                float angle = atan2f(dy, dx);
                float distortion = sinf(dist * 0.5f - progress * 20.0f) * 3.0f;
                int srcX = constrain(x + (int)(cosf(angle) * distortion), 0, PANEL_RES_X - 1);
                int srcY = constrain(y + (int)(sinf(angle) * distortion), 0, PANEL_RES_Y - 1);
                uint16_t color = to[srcY * PANEL_RES_X + srcX];
                
                // Add flash whiteout
                if (flashIntensity > 0.1f) {
                    uint8_t r, g, b;
                    rgb565_to_888(color, r, g, b);
                    r = min(255, r + (int)(flashIntensity * 200));
                    g = min(255, g + (int)(flashIntensity * 200));
                    b = min(255, b + (int)(flashIntensity * 200));
                    color = display->color565(r, g, b);
                }
                display->drawPixel(x, y, color);
            } else if (distToRing < shockwaveWidth) {
                // Shockwave ring - bright white/yellow
                float intensity = 1.0f - (distToRing / shockwaveWidth);
                uint8_t val = (uint8_t)(200 + intensity * 55);
                display->drawPixel(x, y, display->color565(val, val, 100 + (int)(intensity * 155)));
            } else {
                // Outside shockwave - old image with burn effect
                uint8_t r, g, b;
                rgb565_to_888(from[y * PANEL_RES_X + x], r, g, b);
                // Burn edges
                float edgeFactor = min(1.0f, dist / (centerX * 0.5f));
                r = min(255, r + (int)(edgeFactor * 100 * progress));
                g = max(0, g - (int)(edgeFactor * 80 * progress));
                display->drawPixel(x, y, display->color565(r, g, b));
            }
        }
    }
}

// =============================================================================
// TRANSITION 22: ZIGZAG WIPE
// =============================================================================
void renderTransitionZigzag(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    float frequency = 8.0f;
    float amplitude = 12.0f;
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        // Zigzag line moves down
        float zigzagX = (sinf(y / frequency + progress * 15.0f) * amplitude) + (progress * PANEL_RES_X);
        int wipeX = (int)zigzagX;
        
        for (int x = 0; x < PANEL_RES_X; x++) {
            if (x < wipeX) {
                // Show new image behind the zigzag
                display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
        
        // Draw the zigzag line itself
        if (wipeX >= 0 && wipeX < PANEL_RES_X) {
            display->drawPixel(wipeX, y, display->color565(255, 255, 255));
            if (wipeX + 1 < PANEL_RES_X) {
                display->drawPixel(wipeX + 1, y, display->color565(200, 200, 200));
            }
        }
    }
}

// =============================================================================
// TRANSITION 23: OIL PAINTING SMEAR
// =============================================================================
void renderTransitionOilPainting(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    int smearRadius = (int)(5 * (1.0f - progress)) + 1;
    
    for (int y = 0; y < PANEL_RES_Y; y += 2) {
        for (int x = 0; x < PANEL_RES_X; x += 2) {
            // Smear direction - swirling motion
            float angle = atan2f(y - PANEL_RES_Y/2, x - PANEL_RES_X/2) + progress * 10.0f;
            int offsetX = (int)(cosf(angle) * smearRadius);
            int offsetY = (int)(sinf(angle) * smearRadius);
            
            int srcX = constrain(x + offsetX, 0, PANEL_RES_X - 1);
            int srcY = constrain(y + offsetY, 0, PANEL_RES_Y - 1);
            
            float mixFactor = progress * (1.0f - (float)(x + y) / (PANEL_RES_X + PANEL_RES_Y));
            mixFactor = constrain(mixFactor, 0.0f, 1.0f);
            
            if (mixFactor > 0.5f) {
                uint16_t color = to[srcY * PANEL_RES_X + srcX];
                // Paintbrush effect - fill block
                for (int dy = 0; dy < 2 && y + dy < PANEL_RES_Y; dy++) {
                    for (int dx = 0; dx < 2 && x + dx < PANEL_RES_X; dx++) {
                        // Add slight color variation for brush stroke effect
                        float variation = 0.8f + hash21(x + dx, y + dy) * 0.4f;
                        uint8_t r, g, b;
                        rgb565_to_888(color, r, g, b);
                        r = (uint8_t)(r * variation);
                        g = (uint8_t)(g * variation);
                        b = (uint8_t)(b * variation);
                        display->drawPixel(x + dx, y + dy, display->color565(r, g, b));
                    }
                }
            } else {
                // Show old image
                for (int dy = 0; dy < 2 && y + dy < PANEL_RES_Y; dy++) {
                    for (int dx = 0; dx < 2 && x + dx < PANEL_RES_X; dx++) {
                        display->drawPixel(x + dx, y + dy, from[(y+dy) * PANEL_RES_X + (x+dx)]);
                    }
                }
            }
        }
    }
}

// =============================================================================
// TRANSITION 24: ORGANIC MORPHING
// =============================================================================
void renderTransitionMorph(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            // Organic warping using multiple sine waves
            float warpX = sinf(y * 0.15f + progress * 8.0f) * 4.0f * (1.0f - progress);
            float warpY = cosf(x * 0.12f + progress * 6.0f) * 4.0f * (1.0f - progress);
            warpX += sinf((x + y) * 0.08f + progress * 12.0f) * 3.0f * progress;
            warpY += cosf((x - y) * 0.07f + progress * 10.0f) * 3.0f * progress;
            
            int srcX = constrain(x + (int)warpX, 0, PANEL_RES_X - 1);
            int srcY = constrain(y + (int)warpY, 0, PANEL_RES_Y - 1);
            
            // Morph between source images based on progress and position
            float morphFactor = progress + sinf(x * 0.2f + y * 0.15f) * 0.15f;
            morphFactor = constrain(morphFactor, 0.0f, 1.0f);
            
            if (morphFactor > 0.7f) {
                display->drawPixel(x, y, to[srcY * PANEL_RES_X + srcX]);
            } else if (morphFactor < 0.3f) {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            } else {
                // Blend zone
                uint8_t r1, g1, b1, r2, g2, b2;
                rgb565_to_888(from[y * PANEL_RES_X + x], r1, g1, b1);
                rgb565_to_888(to[srcY * PANEL_RES_X + srcX], r2, g2, b2);
                float t = (morphFactor - 0.3f) / 0.4f;
                display->drawPixel(x, y, display->color565(
                    (uint8_t)(r1 * (1.0f - t) + r2 * t),
                    (uint8_t)(g1 * (1.0f - t) + g2 * t),
                    (uint8_t)(b1 * (1.0f - t) + b2 * t)
                ));
            }
        }
    }
}

// =============================================================================
// TRANSITION 25: BUMP / DISPLACEMENT MAP
// =============================================================================
void renderTransitionBump(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            // 3D bump effect using multiple noise layers
            float bumpX = (valueNoise(x * 0.1f, y * 0.1f) - 0.5f) * 12.0f * (1.0f - progress);
            float bumpY = (valueNoise(x * 0.08f + 10.0f, y * 0.08f + 5.0f) - 0.5f) * 12.0f * (1.0f - progress);
            
            // Add circular bump from center
            float centerX = PANEL_RES_X / 2.0f;
            float centerY = PANEL_RES_Y / 2.0f;
            float dx = x - centerX;
            float dy = y - centerY;
            float dist = sqrtf(dx*dx + dy*dy) / (centerX + centerY);
            float circularBump = sinf(dist * PI) * 8.0f * progress;
            bumpX += dx * circularBump / (dist + 0.1f);
            bumpY += dy * circularBump / (dist + 0.1f);
            
            int srcX = constrain(x + (int)bumpX, 0, PANEL_RES_X - 1);
            int srcY = constrain(y + (int)bumpY, 0, PANEL_RES_Y - 1);
            
            // Lighting effect based on bump slope
            float slopeX = bumpX - (valueNoise((x+1) * 0.1f, y * 0.1f) - 0.5f) * 12.0f * (1.0f - progress);
            float slopeY = bumpY - (valueNoise(x * 0.08f + 10.0f, (y+1) * 0.08f + 5.0f) - 0.5f) * 12.0f * (1.0f - progress);
            float light = 0.7f + (slopeX + slopeY) * 0.05f;
            light = constrain(light, 0.3f, 1.0f);
            
            uint8_t r, g, b;
            rgb565_to_888(to[srcY * PANEL_RES_X + srcX], r, g, b);
            r = (uint8_t)(r * light);
            g = (uint8_t)(g * light);
            b = (uint8_t)(b * light);
            display->drawPixel(x, y, display->color565(r, g, b));
        }
    }
}

// =============================================================================
// TRANSITION 26: STARBURST RADIAL
// =============================================================================
void renderTransitionStarburst(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    float centerX = PANEL_RES_X / 2.0f;
    float centerY = PANEL_RES_Y / 2.0f;
    int rays = 12;
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            float dx = x - centerX;
            float dy = y - centerY;
            float angle = atan2f(dy, dx);
            float dist = sqrtf(dx*dx + dy*dy);
            float maxDist = sqrtf(centerX*centerX + centerY*centerY);
            
            // Starburst ray pattern
            float rayAngle = fmodf(angle + progress * TWO_PI, TWO_PI / rays);
            float rayStrength = 1.0f - fabsf(rayAngle - (TWO_PI / rays / 2.0f)) * (rays / 2.0f);
            rayStrength = max(0.0f, min(1.0f, rayStrength * 2.0f));
            
            // Burst radius expands with progress
            float burstRadius = maxDist * progress;
            float radialMask = 1.0f - (dist / burstRadius);
            radialMask = constrain(radialMask, 0.0f, 1.0f);
            
            float finalMask = (rayStrength + radialMask) / 2.0f;
            
            if (finalMask > 0.6f) {
                display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
}

// =============================================================================
// TRANSITION 27: RAIN DROPS
// =============================================================================
void renderTransitionRain(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    int dropCount = (int)(progress * 200);
    
    // Start with old image
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
        }
    }
    
    // Draw raindrops that reveal new image
    for (int i = 0; i < dropCount; i++) {
        int x = (int)(hash21(i, 0) * PANEL_RES_X);
        int y = (int)(hash21(i, 1) * PANEL_RES_Y);
        int dropSize = 1 + (int)(hash21(i, 2) * 3);
        
        // Raindrop "splash" radius grows with progress
        int splashRadius = (int)(dropSize * progress * 2);
        
        for (int dy = -splashRadius; dy <= splashRadius; dy++) {
            for (int dx = -splashRadius; dx <= splashRadius; dx++) {
                int nx = x + dx;
                int ny = y + dy;
                if (nx >= 0 && nx < PANEL_RES_X && ny >= 0 && ny < PANEL_RES_Y) {
                    float dist = sqrtf(dx*dx + dy*dy);
                    if (dist <= splashRadius) {
                        float alpha = 1.0f - (dist / splashRadius);
                        if (alpha > 0.5f) {
                            display->drawPixel(nx, ny, to[ny * PANEL_RES_X + nx]);
                        }
                    }
                }
            }
        }
    }
}

// =============================================================================
// TRANSITION 28: SHATTER / CRACKED GLASS
// =============================================================================
void renderTransitionShatter(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    int crackCount = (int)(progress * 30) + 5;
    
    // Pre-calculate crack lines for this frame
    static int cracks[50][4];  // x1, y1, x2, y2
    static bool cracksCalculated = false;
    
    if (!cracksCalculated && progress > 0.01f) {
        cracksCalculated = true;
        for (int i = 0; i < 50; i++) {
            cracks[i][0] = (int)(hash21(i, 0) * PANEL_RES_X);
            cracks[i][1] = (int)(hash21(i, 1) * PANEL_RES_Y);
            cracks[i][2] = (int)(hash21(i, 2) * PANEL_RES_X);
            cracks[i][3] = (int)(hash21(i, 3) * PANEL_RES_Y);
        }
    }
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            bool isCrack = false;
            float crackDistance = 999.0f;
            
            // Check distance to nearest crack
            for (int i = 0; i < crackCount && i < 50; i++) {
                // Line distance calculation
                float x1 = cracks[i][0], y1 = cracks[i][1];
                float x2 = cracks[i][2], y2 = cracks[i][3];
                float dx = x2 - x1;
                float dy = y2 - y1;
                float len = sqrtf(dx*dx + dy*dy);
                if (len > 0) {
                    float t = ((x - x1) * dx + (y - y1) * dy) / (len * len);
                    t = constrain(t, 0.0f, 1.0f);
                    float projX = x1 + t * dx;
                    float projY = y1 + t * dy;
                    float dist = sqrtf((x - projX)*(x - projX) + (y - projY)*(y - projY));
                    if (dist < crackDistance) crackDistance = dist;
                }
            }
            
            if (crackDistance < 2.0f) {
                // Crack - bright line
                display->drawPixel(x, y, display->color565(200, 200, 255));
            } else if (progress > 0.3f && hash21(x, y) < progress * 0.8f) {
                // Shattered area reveals new image
                int offsetX = (int)((hash21(x + 100, y) - 0.5f) * 8 * progress);
                int offsetY = (int)((hash21(x, y + 100) - 0.5f) * 8 * progress);
                int srcX = constrain(x + offsetX, 0, PANEL_RES_X - 1);
                int srcY = constrain(y + offsetY, 0, PANEL_RES_Y - 1);
                display->drawPixel(x, y, to[srcY * PANEL_RES_X + srcX]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
    }
    
    if (progress >= 0.99f) {
        cracksCalculated = false;
    }
}

// =============================================================================
// TRANSITION 29: MULTI-DIRECTION WIPES
// =============================================================================
void renderTransitionWipes(uint16_t* from, uint16_t* to, float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    
    // Choose wipe pattern based on progress sub-phase
    int pattern = (int)(progress * 8) % 4;
    float subProgress = fmodf(progress * 4, 1.0f);
    
    for (int y = 0; y < PANEL_RES_Y; y++) {
        for (int x = 0; x < PANEL_RES_X; x++) {
            bool showNew = false;
            
            switch (pattern) {
                case 0: // Diagonal wipe
                    showNew = (x + y) < (PANEL_RES_X + PANEL_RES_Y) * subProgress;
                    break;
                case 1: // Center out
                    showNew = (abs(x - PANEL_RES_X/2) + abs(y - PANEL_RES_Y/2)) < 
                              (PANEL_RES_X/2 + PANEL_RES_Y/2) * subProgress;
                    break;
                case 2: // Checkerboard
                    showNew = ((x + y) % 4) < (int)(subProgress * 4);
                    break;
                case 3: // Spiral
                    int spiralX = x - PANEL_RES_X/2;
                    int spiralY = y - PANEL_RES_Y/2;
                    float angle = atan2f(spiralY, spiralX);
                    float spiralProgress = (angle + PI) / (TWO_PI);
                    showNew = spiralProgress < subProgress;
                    break;
            }
            
            if (showNew) {
                display->drawPixel(x, y, to[y * PANEL_RES_X + x]);
            } else {
                display->drawPixel(x, y, from[y * PANEL_RES_X + x]);
            }
        }
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

    // Randomize next transition
    currentTransitionType = (TransitionType)random(0, TRANS_MAX);
    
    // Assign specific duration per transition
    switch (currentTransitionType) {
        case TRANS_CROSSFADE:      currentTransitionDuration = 2500; break;
        case TRANS_WAVE_RIPPLE:    currentTransitionDuration = 2500; break;
        case TRANS_PIXEL_DISSOLVE: currentTransitionDuration = 2500; break;
        case TRANS_GLITCH_NOISE:   currentTransitionDuration = 2500; break;
        case TRANS_4X4_BLOCKS:     currentTransitionDuration = 2500; break;
        case TRANS_IRIS_WIPE:      currentTransitionDuration = 2500; break;
        case TRANS_WATER:          currentTransitionDuration = 2500; break;
        case TRANS_GENERATIVE:     currentTransitionDuration = 2500; break;
        case TRANS_SMOKE:          currentTransitionDuration = 2500; break;
        case ORIGINAL:             currentTransitionDuration = 2500; break;
        case TRANS_PLASMA:         currentTransitionDuration = 2500; break;
        case TRANS_FIRE:           currentTransitionDuration = 2800; break;
        case TRANS_ICE_COLD:       currentTransitionDuration = 2500; break;
        case TRANS_SWIRL:          currentTransitionDuration = 2500; break;
        case TRANS_SCANLINE:       currentTransitionDuration = 2000; break;
        case TRANS_PINWHEEL:       currentTransitionDuration = 2500; break;
        case TRANS_MOSAIC:         currentTransitionDuration = 2500; break;
        case TRANS_BLUR:           currentTransitionDuration = 2500; break;
        case TRANS_SPLIT:          currentTransitionDuration = 2500; break;
        case TRANS_BOUNCE:         currentTransitionDuration = 2500; break;
        case TRANS_PARTICLES:      currentTransitionDuration = 3000; break;
        case TRANS_ATOMIC:         currentTransitionDuration = 2800; break;
        case TRANS_ZIGZAG:         currentTransitionDuration = 2500; break;
        case TRANS_OIL_PAINTING:   currentTransitionDuration = 3000; break;
        case TRANS_MORPH:          currentTransitionDuration = 2800; break;
        case TRANS_BUMP:           currentTransitionDuration = 2500; break;
        case TRANS_STARBURST:      currentTransitionDuration = 2500; break;
        case TRANS_RAIN:           currentTransitionDuration = 2800; break;
        case TRANS_SHATTER:        currentTransitionDuration = 3000; break;
        case TRANS_WIPES:          currentTransitionDuration = 2500; break;
        default:                   currentTransitionDuration = 2500; break;
    }

    Serial.printf("Transition: %d (dur: %lu) - Image: %d -> %d\n", 
                  currentTransitionType, currentTransitionDuration, 
                  currentImageIndex, nextImageIndex);
}



// =============================================================================
// UPDATE TRANSITION (called every loop)
// =============================================================================
void updateTransition() {
    if (!isTransitioning) return;

    float progress = (float)(millis() - transitionStartTime) / (float)currentTransitionDuration;

    if (progress >= 1.0f) {
        // Fade complete - swap buffers
        isTransitioning   = false;
        currentImageIndex = nextImageIndex;
        lastChangeTime    = millis();

        uint16_t* tmp = currentFrame;
        currentFrame  = nextFrame;
        nextFrame     = tmp;

        displayFrame(currentFrame);
        Serial.printf("Transition done -> image %d\n", currentImageIndex);
    } else {

        switch (currentTransitionType) {
            case TRANS_CROSSFADE:      renderTransitionCrossfade(currentFrame, nextFrame, progress); break;
            case TRANS_WAVE_RIPPLE:    renderTransitionWaveRipple(currentFrame, nextFrame, progress); break;
            case TRANS_PIXEL_DISSOLVE: renderTransitionPixelDissolve(currentFrame, nextFrame, progress); break;
            case TRANS_GLITCH_NOISE:   renderTransitionGlitchNoise(currentFrame, nextFrame, progress); break;
            case TRANS_4X4_BLOCKS:     renderTransition4x4Blocks(currentFrame, nextFrame, progress); break;
            case TRANS_IRIS_WIPE:      renderTransitionIrisWipe(currentFrame, nextFrame, progress); break;
            case TRANS_WATER:          renderTransitionWater(currentFrame, nextFrame, progress); break;
            case TRANS_GENERATIVE:     renderTransitionGenerative(currentFrame, nextFrame, progress); break;
            case TRANS_SMOKE:          renderTransitionSmoke(currentFrame, nextFrame, progress); break;
            case ORIGINAL:             crossFadeFrames(currentFrame, nextFrame, progress); break;
            case TRANS_PLASMA:         renderTransitionPlasma(currentFrame, nextFrame, progress); break;
            case TRANS_FIRE:           renderTransitionFire(currentFrame, nextFrame, progress); break;
            case TRANS_ICE_COLD:       renderTransitionIceCold(currentFrame, nextFrame, progress); break;
            case TRANS_SWIRL:          renderTransitionSwirl(currentFrame, nextFrame, progress); break;
            case TRANS_SCANLINE:       renderTransitionScanline(currentFrame, nextFrame, progress); break;
            case TRANS_PINWHEEL:       renderTransitionPinwheel(currentFrame, nextFrame, progress); break;
            case TRANS_MOSAIC:         renderTransitionMosaic(currentFrame, nextFrame, progress); break;
            case TRANS_BLUR:           renderTransitionBlur(currentFrame, nextFrame, progress); break;
            case TRANS_SPLIT:          renderTransitionSplit(currentFrame, nextFrame, progress); break;
            case TRANS_BOUNCE:         renderTransitionBounce(currentFrame, nextFrame, progress); break;
            case TRANS_PARTICLES:      renderTransitionParticles(currentFrame, nextFrame, progress); break;
            case TRANS_ATOMIC:         renderTransitionAtomic(currentFrame, nextFrame, progress); break;
            case TRANS_ZIGZAG:         renderTransitionZigzag(currentFrame, nextFrame, progress); break;
            case TRANS_OIL_PAINTING:   renderTransitionOilPainting(currentFrame, nextFrame, progress); break;
            case TRANS_MORPH:          renderTransitionMorph(currentFrame, nextFrame, progress); break;
            case TRANS_BUMP:           renderTransitionBump(currentFrame, nextFrame, progress); break;
            case TRANS_STARBURST:      renderTransitionStarburst(currentFrame, nextFrame, progress); break;
            case TRANS_RAIN:           renderTransitionRain(currentFrame, nextFrame, progress); break;
            case TRANS_SHATTER:        renderTransitionShatter(currentFrame, nextFrame, progress); break;
            case TRANS_WIPES:          renderTransitionWipes(currentFrame, nextFrame, progress); break;
            default:                   renderTransitionCrossfade(currentFrame, nextFrame, progress); break;
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


