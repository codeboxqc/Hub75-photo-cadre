# HUB75 LED Matrix Slideshow v3

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-orange)](https://platformio.org/)
[![ESP32](https://img.shields.io/badge/ESP32-HUB75-blue)](https://www.espressif.com/)
[![License](https://img.shields.io/badge/License-MIT-green)](LICENSE)

A high-performance slideshow player for 64x64 RGB LED matrices (FM6126A) with **30+ stunning transition effects** including particles, atomic blast, morphing, oil painting, and more. Supports SD card (primary) and SPIFFS (fallback) storage with multiple image formats.

 <img width="4080" height="1840" alt="1776346488757" src="https://github.com/user-attachments/assets/58b3d72e-eadd-4f78-8b80-262d31b0763a" />
<img width="1840" height="4080" alt="1776346488693" src="https://github.com/user-attachments/assets/222e643b-faf5-4a4b-bfd5-ba4012631ce1" />
<img width="4080" height="1840" alt="1776346488782" src="https://github.com/user-attachments/assets/7c0200b1-5efa-4f37-82f3-a1377b322a8a" />





https://github.com/user-attachments/assets/461cb867-911e-473c-b0f7-770bdc52268f



https://github.com/user-attachments/assets/381d6bd6-79db-47c6-8fbe-0930825989d9



https://github.com/user-attachments/assets/e7c9754c-0b2d-43ac-9d90-497c119752b9



https://github.com/user-attachments/assets/a1690b78-bdeb-4f24-a56a-9f60cd9922e0




## ✨ Features

- **🖼️ Multi-format Support** - 24-bit, 16-bit, and 8-bit indexed .bin images
- **🎬 30+ Transitions** - From classic crossfade to particle explosions and atomic shockwaves
- **💾 Dual Storage** - SD Card (primary) + SPIFFS (fallback)
- **🎮 Interactive** - Button control + serial commands
- **📁 Recursive Scanning** - Finds images in subfolders
- **⚡ Optimized** - DMA-driven display, minimal RAM usage
- **🔧 Easy Configuration** - Simple pin definitions and timing controls

## 🎨 Transition Effects

| Category | Effects |
|----------|---------|
| **Classic** | Crossfade, Iris Wipe, Scanline, Split Screen |
| **Organic** | Water Ripple, Smoke, Plasma, Fire, Ice Cold |
| **Glitch** | Pixel Dissolve, Glitch Noise, Shatter, Blur |
| **Geometric** | 4x4 Blocks, Mosaic, Pinwheel, Starburst |
| **Complex** | Particle Explosion, Atomic Shockwave, Morphing, Oil Painting |
| **Dynamic** | Swirl Vortex, Bounce, Zigzag, Rain Drops, Wipes |

## 📋 Requirements

### Hardware
- ESP32 development board (WROOM/WROVER)
- 64x64 HUB75 RGB LED matrix with FM6126A driver
- SD card module (optional)
- Momentary push button (optional)
- 5V power supply (adequate for your panel size)

### Software
- [PlatformIO](https://platformio.org/) (recommended) or Arduino IDE
- ESP32 board support package
- Required libraries (see below)

## 🔌 Wiring Guide

### HUB75 Matrix Connection
| Matrix Pin | GPIO Pin |
|------------|----------|
| R1 | 25 |
| G1 | 26 |
| B1 | 27 |
| R2 | 13 |
| G2 | 12 |
| B2 | 14 |
| A | 23 |
| B | 19 |
| C | 21 |
| D | 22 |
| E | 18 |
| LAT | 4 |
| CLK | 16 |
| OE | 15 |

### SD Card Module (VSPI)
| SD Module | GPIO Pin |
|-----------|----------|
| CS | 33 |
| MOSI | 17 |
| MISO | 35 |
| SCK | 5 |
| VCC | 5V |
| GND | GND |

### Button
| Button | GPIO Pin |
|--------|----------|
| Signal | 32 |
| GND | GND |

> **Note:** GPIO35 is input-only, making it ideal for SD card MISO

## 🚀 Installation

### PlatformIO (Recommended)

1. **Clone the repository**
```bash
git clone https://github.com/yourusername/hub75-slideshow.git
cd hub75-slideshow



hub75-slideshow/
├── src/
│   ├── main.cpp          # Main firmware
│   └── fx.h              # Effects header (optional)
├── data/                  # SPIFFS images (upload with uploadfs)
│   └── *.bin
├── platformio.ini        # PlatformIO configuration
├── README.md             # This file
└── LICENSE


[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
board_build.partitions = huge_app.csv
lib_deps = 
    mrfaptastic/ESP32 HUB75 LED MATRIX PANEL DMA Display@^3.0.7
build_flags = 
    -DCORE_DEBUG_LEVEL=0


Troubleshooting
"MALLOC FAILED" Error
Reduce MAX_IMAGES value

Use smaller image formats (8-bit instead of 24-bit)

Try different partition scheme

No images found on SD card
Check wiring (CS, MOSI, MISO, SCK)

Format SD card as FAT32

Ensure files have .bin extension

Check file names (no special characters)

Matrix shows garbage/no display
Verify FM6126A driver selection in code

Check all HUB75 connections

Adjust clkphase setting (try true/false)

Ensure adequate power supply (5V, 2A+ for 64x64)

Transitions are slow/jerky
Reduce transition duration

Lower panel brightness

Use 16-bit or 8-bit images for faster loading


Technical/Professional
Title	Description
ESP32-HUB75 Media Player	Professional, marketable
PixelPulse Matrix Controller	Modern, energetic
LED Canvas Pro	Artistic, premium
Matrix Stream Engine	Technical, powerful
ChromaFlow HUB75	Color-focused, elegant
Artistic/Creative
Title	Description
Pixel Morph	Emphasizes transitions
Luminous Flow	Poetic, visual
Chroma Cascade	Color transition focus
Vivid Shift	Dynamic, bright
Neon Narrator	Retro, storytelling
Open Source/Hacker
Title	Description
OpenPixel Player	Community-focused
Matrix Libre	Freedom-focused
PixelFleet	Scalable, network-ready
HUB75 Hero	Playful, memorable
ESPixel Engine	Technical, clear
🏷️ Serial/Console Title



HUB75 LED Matrix Guide
Understanding HUB75 Pinout
The HUB75 connector on your LED matrix has 16 pins. Here's the complete mapping:

HUB75 Pin	Signal	Description	ESP32 GPIO
1	R1	Red data (top half)	GPIO 25
2	G1	Green data (top half)	GPIO 26
3	B1	Blue data (top half)	GPIO 27
4	GND	Ground	GND
5	R2	Red data (bottom half)	GPIO 14
6	G2	Green data (bottom half)	GPIO 12
7	B2	Blue data (bottom half)	GPIO 13
8	GND	Ground	GND
9	A	Row address bit 0	GPIO 23
10	B	Row address bit 1	GPIO 19
11	C	Row address bit 2	GPIO 5
12	D	Row address bit 3	GPIO 17
13	E	Row address bit 4	GPIO 18
14	LAT	Latch signal	GPIO 4
15	CLK	Clock signal	GPIO 16
16	OE	Output Enable	GPIO 15
Important: The E pin (row address bit 4) is required for 64x64 panels (1/32 scan). For 32x32 or 64x32 panels (1/16 scan), the E pin can be omitted.

Supported Driver Chips
Your panel may have one of these driver chips. The code supports:

FM6126A (most common for 64x64) — used in this project

FM6124

ICN2038S

MBI5124 (requires clock_phase: true)

DP3246

Generic standard shift register

Panel Specifications
Panel Type	Resolution	Scan Rate	E Pin Required	Power Draw (approx)
64x32	2048 pixels	1/16 scan	No	2-3A
64x64	4096 pixels	1/32 scan	Yes	4-6A
128x64	8192 pixels	1/32 scan	Yes	8-12A
Panel Identification Tips
To identify your panel type:

Check the model number on the back of the PCB

Look for the driver chip markings (FM6126A, ICN2038S, etc.)

Count the rows of ICs — more ICs = higher resolution panels

Check the connector — 16-pin HUB75 is standard

Wiring Instructions
Complete Wiring Diagram


ESP32 Dev Board                        HUB75 Matrix (64x64)
┌─────────────────┐                    ┌─────────────────┐
│                 │                    │                 │
│  GPIO25 ────────┼────────────────────┼──── R1          │
│  GPIO26 ────────┼────────────────────┼──── G1          │
│  GPIO27 ────────┼────────────────────┼──── B1          │
│  GPIO14 ────────┼────────────────────┼──── R2          │
│  GPIO12 ────────┼────────────────────┼──── G2          │
│  GPIO13 ────────┼────────────────────┼──── B2          │
│  GPIO23 ────────┼────────────────────┼──── A           │
│  GPIO19 ────────┼────────────────────┼──── B           │
│  GPIO5  ────────┼────────────────────┼──── C           │
│  GPIO17 ────────┼────────────────────┼──── D           │
│  GPIO18 ────────┼────────────────────┼──── E           │
│  GPIO4  ────────┼────────────────────┼──── LAT         │
│  GPIO16 ────────┼────────────────────┼──── CLK         │
│  GPIO15 ────────┼────────────────────┼──── OE          │
│                 │                    │                 │
│  GND    ────────┼────────────────────┼──── GND         │
│                 │                    │                 │
└─────────────────┘                    └─────────────────┘

ESP32 Dev Board                        SD Card Module
┌─────────────────┐                    ┌─────────────────┐
│                 │                    │                 │
│  5V     ────────┼────────────────────┼──── VCC (5V)    │
│  GND    ────────┼────────────────────┼──── GND         │
│  GPIO33 ────────┼────────────────────┼──── CS          │
│  GPIO17 ────────┼────────────────────┼──── MOSI        │
│  GPIO35 ────────┼────────────────────┼──── MISO        │
│  GPIO5  ────────┼────────────────────┼──── SCK         │
│                 │                    │                 │
└─────────────────┘                    └─────────────────┘

ESP32 Dev Board                        Button
┌─────────────────┐                    ┌─────────────────┐
│                 │                    │                 │
│  GPIO32 ────────┼────────────────────┼──── Button Pin  │
│                 │                    │                 │
│  GND    ────────┼────────────────────┼──── GND         │
│                 │                    │                 │
└─────────────────┘                    └─────────────────┘


Step-by-Step Connection Guide
1. HUB75 Matrix (Critical — Connect First)
Connect these 14 data lines from ESP32 to the matrix:

ESP32 GPIO	HUB75 Signal
25	R1
26	G1
27	B1
14	R2
12	G2
13	B2
23	A
19	B
5	C
17	D
18	E
4	LAT
16	CLK
15	OE
GND	GND (any)
⚠️ Double-check these connections! One wrong pin can prevent the display from working entirely.

2. SD Card Module (SPI Mode)
The SD card uses the VSPI bus on ESP32:

ESP32 GPIO	SD Module Pin
5V or 3.3V	VCC
GND	GND
5	SCK
17	MOSI
35	MISO
33	CS
Note about GPIO35: This pin is input-only, which makes it perfect for MISO (no accidental output conflicts).

3. Push Button
The button uses ESP32's internal pull-up resistor, so no external resistor is needed:

ESP32 GPIO	Button Connection
32	One button terminal
GND	Other button terminal
How it works: The internal pull-up keeps the pin HIGH. When you press the button, it connects to GND and reads LOW.

Power Supply Guide
Power Requirements
Critical: A 64x64 RGB matrix can draw significant current. Undersized power supplies cause flickering, random resets, or no display at all.

Panel Size	Minimum PSU	Recommended PSU	Peak Current
64x32 (1/16 scan)	5V 3A	5V 5A	2-3A
64x64 (1/32 scan)	5V 5A	5V 10A	4-6A
128x64 (1/32 scan)	5V 8A	5V 15A	8-12A
Power Connection Best Practices
Connect power directly to the matrix — Do not power the matrix through the ESP32

Common ground — Connect GND from PSU to BOTH matrix and ESP32

Use thick wires — 18 AWG or thicker for main power lines

Add capacitor — 1000µF electrolytic across PSU output helps smooth current spikes

Separate power for ESP32 — If flickering occurs, power ESP32 separately


Picture Frame Mount (IKEA RIBBA Hack)
This popular approach uses an IKEA RIBBA frame (21×30 cm):

Materials needed:

IKEA RIBBA frame (23x23cm or 21x30cm)

Black acrylic paint for the backplate

Copper wire for standoffs (~70cm of 1mm diameter)

M3 machine screws (6x)

Steps:

Remove the backplate and paint it black

Mount the matrix to the backplate using standoffs

Secure ESP32 behind the matrix

Drill hole for power cable

Cut slot for SD card access if needed

Wall-Mount Installation
For permanent installation:

Use a flush-mount backbox

Add ventilation (matrix panels get warm)

Consider a clear acrylic cover for protection

Use angled connectors for power and data
 
 # HUB75 Image Converter v2

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](https://github.com/)
[![License](https://img.shields.io/badge/License-MIT-green)](LICENSE)

A powerful command-line tool to convert any image (JPG, PNG, BMP) to the native .bin format for HUB75 LED matrix displays (64x64 panels).

![Converter Demo](https://via.placeholder.com/800x200?text=HUB75+Image+Converter+Demo)

## 🎯 What This Tool Does

The HUB75 Image Converter transforms standard image files into the binary format required by the **PixelPulse Matrix** slideshow firmware for ESP32. It handles resizing, color conversion, and gamma correction automatically.

### Key Features

- **3 Color Modes**: 24-bit RGB888 (best), 16-bit RGB565 (good), 8-bit Palette (efficient)
- **Automatic Resizing**: Scales any image to exactly 64x64 pixels
- **Gamma Correction**: Proper LED brightness response
- **Smart Palette Generation**: Median-cut algorithm for optimal 8-bit images
- **Batch Processing**: Convert entire folders at once
- **Cross-Platform**: Works on Windows, Linux, and macOS

## 📊 File Size Comparison

| Mode | File Size | Quality | Images per MB | Use Case |
|------|-----------|---------|---------------|----------|
| **24-bit** | 12,289 bytes | ★★★★★ | ~85 | Photos, gradients, art |
| **16-bit** | 8,193 bytes | ★★★★☆ | ~127 | General purpose (recommended) |
| **8-bit** | 4,865 bytes | ★★★☆☆ | ~215 | Pixel art, logos, limited palette |

## 🔧 Installation

### Windows (Pre-built)
1. Download the latest `imageTOhub75.exe` from [Releases](https://github.com/yourusername/hub75-image-converter/releases)
2. Place it in your working directory

### Build from Source (All Platforms)

**Prerequisites:**
- C++17 compiler (GCC 7+, Clang 5+, MSVC 2019+)
- CMake 3.10+ (optional)

**Using CMake:**
```bash
# Clone the repository
git clone https://github.com/yourusername/hub75-image-converter.git
cd hub75-image-converter

# Create build directory
mkdir build && cd build

# Configure and build
cmake ..
cmake --build . --config Release
Use right-angle jumper wires to save space



imageTOhub75.exe <input_folder> <output_folder> <mode>
imageTOhub75.exe  my_images  converted 24

24-bit RGB888 (Mode 24)
Structure: [mode_byte] + [R,G,B] × 4096 pixels

Total Size: 12,289 bytes

Best for: Photos, gradients, artwork with fine color detail

Quality: Perfect color reproduction

16-bit RGB565 (Mode 16)
Structure: [mode_byte] + [RGB565_le] × 4096 pixels

Total Size: 8,193 bytes (33% smaller than 24-bit)

Best for: General purpose, most use cases

Quality: Excellent, barely noticeable difference on LEDs

8-bit Palette (Mode 8)
Structure: [mode_byte] + [768-byte palette] + [4096 indices]

Total Size: 4,865 bytes (60% smaller than 24-bit)

Best for: Pixel art, logos, limited color images

Quality: Good for images with ≤256 colors

save all bin to your SD card

