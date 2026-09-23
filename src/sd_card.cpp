#include "sd_card.h"

#include <SD_MMC.h>

#include "pin_config.h"

bool sdBegin() {
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  // 1-bit mode (T-Dongle-S3 only wires D0), mount at /sdcard.
  if (!SD_MMC.begin("/sdcard", true)) return false;
  return SD_MMC.cardType() != CARD_NONE;
}

uint64_t sdTotalBytes() { return SD_MMC.totalBytes(); }
uint64_t sdUsedBytes() { return SD_MMC.usedBytes(); }
