#pragma once
// Pin map for the LILYGO T-Dongle-S3 (ESP32-S3).
// Values follow LILYGO's published pinout — verify against your board revision.

// --- 0.96" ST7735 LCD (SPI). Also declared via build_flags for TFT_eSPI. ---
#define PIN_TFT_MOSI 3
#define PIN_TFT_SCLK 5
#define PIN_TFT_CS 4
#define PIN_TFT_DC 2
#define PIN_TFT_RST 1
#define PIN_TFT_BL 38

// --- microSD (TF) card, SD_MMC 1-bit mode ---
#define PIN_SD_CLK 14
#define PIN_SD_CMD 16
#define PIN_SD_D0 17

// --- On-board controls / LED ---
#define PIN_BUTTON 0
#define PIN_APA102_DATA 40
#define PIN_APA102_CLK 39
