#pragma once
#include <stdint.h>

// Mounts the microSD (TF) card in SD_MMC 1-bit mode. Safe to call once at boot.
bool sdBegin();
uint64_t sdTotalBytes();
uint64_t sdUsedBytes();
