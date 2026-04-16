# HUB75 LED Matrix Slideshow v3

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-orange)](https://platformio.org/)
[![ESP32](https://img.shields.io/badge/ESP32-HUB75-blue)](https://www.espressif.com/)
[![License](https://img.shields.io/badge/License-MIT-green)](LICENSE)

A high-performance slideshow player for 64x64 RGB LED matrices (FM6126A) with **30+ stunning transition effects** including particles, atomic blast, morphing, oil painting, and more. Supports SD card (primary) and SPIFFS (fallback) storage with multiple image formats.

![Demo](https://via.placeholder.com/800x400?text=HUB75+Slideshow+Demo)

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



