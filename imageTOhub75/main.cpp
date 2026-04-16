#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <vector>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <string>
#include <algorithm>
#include <map>
#include <array>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

namespace fs = std::filesystem;

// HUB75 Constants for 64x64 Matrix
const int TARGET_W = 64;
const int TARGET_H = 64;
const int PIXEL_COUNT = TARGET_W * TARGET_H;  // 4096 pixels

// File size summary:
// 24-bit: 64*64*3 = 12,288 bytes per image
// 16-bit: 64*64*2 =  8,192 bytes per image (33% smaller)
// 8-bit:  64*64*1 + 768 (palette) = 4,864 bytes per image (60% smaller)

enum ColorMode {
    MODE_24BIT = 24,  // Full RGB888 - best quality, largest files
    MODE_16BIT = 16,  // RGB565 - good quality, 33% smaller
    MODE_8BIT = 8    // 256-color palette - decent quality, 60% smaller
};

uint8_t gamma_lut[256];

void init_gamma() {
    for (int i = 0; i < 256; i++) {
        float val = std::pow((float)i / 255.0f, 2.2f) * 255.0f;
        int rounded = (int)(val + 0.5f);
        if (rounded > 0 && rounded < 8) rounded = 8;  // Flicker prevention for LEDs
        gamma_lut[i] = (uint8_t)std::clamp(rounded, 0, 255);
    }
}

// Convert RGB888 to RGB565
uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Median Cut Color Quantization for 256-color palette
struct ColorBox {
    std::vector<std::array<uint8_t, 3>> colors;

    int getLongestAxis() const {
        uint8_t minR = 255, maxR = 0, minG = 255, maxG = 0, minB = 255, maxB = 0;
        for (const auto& c : colors) {
            minR = std::min(minR, c[0]); maxR = std::max(maxR, c[0]);
            minG = std::min(minG, c[1]); maxG = std::max(maxG, c[1]);
            minB = std::min(minB, c[2]); maxB = std::max(maxB, c[2]);
        }
        int rangeR = maxR - minR;
        int rangeG = maxG - minG;
        int rangeB = maxB - minB;
        if (rangeR >= rangeG && rangeR >= rangeB) return 0;
        if (rangeG >= rangeR && rangeG >= rangeB) return 1;
        return 2;
    }

    std::array<uint8_t, 3> getAverage() const {
        if (colors.empty()) return { 0, 0, 0 };
        uint32_t sumR = 0, sumG = 0, sumB = 0;
        for (const auto& c : colors) {
            sumR += c[0]; sumG += c[1]; sumB += c[2];
        }
        return {
            (uint8_t)(sumR / colors.size()),
            (uint8_t)(sumG / colors.size()),
            (uint8_t)(sumB / colors.size())
        };
    }
};

std::vector<std::array<uint8_t, 3>> generatePalette(const uint8_t* imageData, int pixelCount, int paletteSize = 256) {
    // Collect unique colors (with some quantization to reduce count)
    std::vector<std::array<uint8_t, 3>> allColors;
    allColors.reserve(pixelCount);

    for (int i = 0; i < pixelCount; i++) {
        allColors.push_back({
            imageData[i * 3],
            imageData[i * 3 + 1],
            imageData[i * 3 + 2]
            });
    }

    // Median cut algorithm
    std::vector<ColorBox> boxes;
    boxes.push_back({ allColors });

    while ((int)boxes.size() < paletteSize && !boxes.empty()) {
        // Find box with most colors
        int maxIdx = 0;
        size_t maxSize = 0;
        for (int i = 0; i < (int)boxes.size(); i++) {
            if (boxes[i].colors.size() > maxSize) {
                maxSize = boxes[i].colors.size();
                maxIdx = i;
            }
        }

        if (boxes[maxIdx].colors.size() < 2) break;

        ColorBox& box = boxes[maxIdx];
        int axis = box.getLongestAxis();

        // Sort by longest axis
        std::sort(box.colors.begin(), box.colors.end(),
            [axis](const auto& a, const auto& b) { return a[axis] < b[axis]; });

        // Split in half
        size_t mid = box.colors.size() / 2;
        ColorBox newBox;
        newBox.colors.assign(box.colors.begin() + mid, box.colors.end());
        box.colors.resize(mid);

        boxes.push_back(newBox);
    }

    // Generate palette from box averages
    std::vector<std::array<uint8_t, 3>> palette;
    palette.reserve(paletteSize);
    for (const auto& box : boxes) {
        if (!box.colors.empty()) {
            palette.push_back(box.getAverage());
        }
    }

    // Pad palette to 256 if needed
    while (palette.size() < 256) {
        palette.push_back({ 0, 0, 0 });
    }

    return palette;
}

uint8_t findClosestPaletteIndex(uint8_t r, uint8_t g, uint8_t b,
    const std::vector<std::array<uint8_t, 3>>& palette) {
    int bestIdx = 0;
    int bestDist = INT_MAX;

    for (int i = 0; i < (int)palette.size(); i++) {
        int dr = (int)r - palette[i][0];
        int dg = (int)g - palette[i][1];
        int db = (int)b - palette[i][2];
        // Weighted distance (green is more perceptually important)
        int dist = dr * dr * 2 + dg * dg * 4 + db * db * 3;
        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = i;
        }
    }
    return (uint8_t)bestIdx;
}

bool process_image(const fs::path& inputPath, const fs::path& outputPath, ColorMode mode) {
    int w, h, channels;
    unsigned char* img_data = nullptr;

    FILE* f = nullptr;
    errno_t err = _wfopen_s(&f, inputPath.wstring().c_str(), L"rb");

    if (err != 0 || f == nullptr) {
        std::cerr << " [ERR] Could not open file: " << inputPath.filename() << std::endl;
        return false;
    }

    img_data = stbi_load_from_file(f, &w, &h, &channels, 3);
    fclose(f);

    if (!img_data) {
        return false;
    }

    // Resize to 64x64
    std::vector<uint8_t> resized_rgb(PIXEL_COUNT * 3);
    stbir_resize_uint8_linear(img_data, w, h, 0,
        resized_rgb.data(), TARGET_W, TARGET_H, 0,
        STBIR_RGB);
    stbi_image_free(img_data);

    // Apply Gamma correction
    for (int i = 0; i < PIXEL_COUNT * 3; i++) {
        resized_rgb[i] = gamma_lut[resized_rgb[i]];
    }

    std::ofstream out(outputPath, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << " [ERR] Could not create output file" << std::endl;
        return false;
    }

    // Write magic header: 1 byte for color mode
    uint8_t header = (uint8_t)mode;
    out.write((char*)&header, 1);

    switch (mode) {
    case MODE_24BIT: {
        // Write raw RGB888 data (12,288 bytes + 1 header = 12,289 bytes)
        out.write((char*)resized_rgb.data(), PIXEL_COUNT * 3);
        break;
    }

    case MODE_16BIT: {
        // Convert to RGB565 (8,192 bytes + 1 header = 8,193 bytes)
        std::vector<uint16_t> rgb565_data(PIXEL_COUNT);
        for (int i = 0; i < PIXEL_COUNT; i++) {
            rgb565_data[i] = rgb888_to_rgb565(
                resized_rgb[i * 3],
                resized_rgb[i * 3 + 1],
                resized_rgb[i * 3 + 2]
            );
        }
        out.write((char*)rgb565_data.data(), PIXEL_COUNT * 2);
        break;
    }

    case MODE_8BIT: {
        // Generate 256-color palette and indexed data
        // Format: 768 bytes palette + 4096 bytes indices = 4,864 bytes + 1 header = 4,865 bytes
        auto palette = generatePalette(resized_rgb.data(), PIXEL_COUNT, 256);

        // Write palette (256 colors * 3 bytes = 768 bytes)
        for (const auto& color : palette) {
            out.write((char*)color.data(), 3);
        }

        // Write indexed pixels
        std::vector<uint8_t> indexed(PIXEL_COUNT);
        for (int i = 0; i < PIXEL_COUNT; i++) {
            indexed[i] = findClosestPaletteIndex(
                resized_rgb[i * 3],
                resized_rgb[i * 3 + 1],
                resized_rgb[i * 3 + 2],
                palette
            );
        }
        out.write((char*)indexed.data(), PIXEL_COUNT);
        break;
    }
    }

    out.close();
    return true;
}

int main(int argc, char* argv[]) {
    std::cout << "\n=== HUB75 IMAGE CONVERTER v2 ===" << std::endl;
    std::cout << "Supports 24-bit, 16-bit, and 8-bit (256 color) modes\n" << std::endl;

    if (argc < 4) {
        std::cout << "Usage: imageTOhub75_v2.exe <input_folder> <output_folder> <mode>" << std::endl;
        std::cout << "\nModes:" << std::endl;
        std::cout << "  24  - Full RGB888 (12,289 bytes/image) - Best quality" << std::endl;
        std::cout << "  16  - RGB565     ( 8,193 bytes/image) - Good quality, 33% smaller" << std::endl;
        std::cout << "  8   - 256 colors ( 4,865 bytes/image) - Decent quality, 60% smaller" << std::endl;
        std::cout << "\nExample: imageTOhub75_v2.exe ./photos ./output 16" << std::endl;
        std::cout << "\nCapacity estimates (assuming ~1.5MB SPIFFS):" << std::endl;
        std::cout << "  24-bit: ~120 images" << std::endl;
        std::cout << "  16-bit: ~180 images" << std::endl;
        std::cout << "  8-bit:  ~300 images" << std::endl;
        return 1;
    }

    std::string inDir = argv[1];
    std::string outDir = argv[2];
    int modeArg = std::stoi(argv[3]);

    ColorMode mode;
    std::string modeName;
    switch (modeArg) {
    case 24: mode = MODE_24BIT; modeName = "24-bit RGB888"; break;
    case 16: mode = MODE_16BIT; modeName = "16-bit RGB565"; break;
    case 8:  mode = MODE_8BIT;  modeName = "8-bit Palette"; break;
    default:
        std::cerr << "Invalid mode. Use 24, 16, or 8." << std::endl;
        return 1;
    }

    init_gamma();

    try {
        if (!fs::exists(inDir)) {
            std::cerr << "Error: Input directory not found!" << std::endl;
            return 1;
        }

        if (!fs::exists(outDir)) fs::create_directories(outDir);

        int counter = 1;
        int successCount = 0;
        size_t totalBytes = 0;

        std::cout << "Converting images in " << modeName << " mode...\n" << std::endl;

        for (const auto& entry : fs::directory_iterator(inDir)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp") {
                    std::string outName = "image" + std::to_string(counter) + ".bin";
                    fs::path outPath = fs::path(outDir) / outName;

                    if (process_image(entry.path(), outPath, mode)) {
                        size_t fileSize = fs::file_size(outPath);
                        totalBytes += fileSize;
                        std::cout << " [OK] " << entry.path().filename()
                            << " -> " << outName
                            << " (" << fileSize << " bytes)" << std::endl;
                        successCount++;
                        counter++;
                    }
                    else {
                        std::cerr << " [ERR] " << entry.path().filename() << std::endl;
                    }
                }
            }
        }

        std::cout << "\n========================================" << std::endl;
        std::cout << "Finished! " << successCount << " images converted" << std::endl;
        std::cout << "Total size: " << totalBytes << " bytes ("
            << (totalBytes / 1024) << " KB)" << std::endl;
        std::cout << "Average size: " << (successCount > 0 ? totalBytes / successCount : 0)
            << " bytes/image" << std::endl;
        std::cout << "Output: " << outDir << std::endl;

    }
    catch (const std::exception& e) {
        std::cerr << "Critical Error: " << e.what() << std::endl;
    }

    return 0;
}