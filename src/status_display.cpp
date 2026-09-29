#include "status_display.h"

#include "pin_config.h"

namespace {
// ~26 chars fit across 160px in the 6px GLCD font.
String truncateToWidth(const String& s, int maxChars = 26) {
  if ((int)s.length() <= maxChars) return s;
  return s.substring(0, maxChars - 1) + "\x7F";  // trailing marker
}
}  // namespace

void StatusDisplay::begin() {
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, LOW);  // backlight on (active-low on T-Dongle-S3)

  _tft.init();
  _tft.setRotation(3);  // USB connector to the right; flip to 1 if upside-down
  _tft.fillScreen(TFT_BLACK);
  _tft.setTextWrap(false);

  setState(DeviceState::Boot, "DockerGo");
}

void StatusDisplay::setState(DeviceState state, const String& label) {
  _state = state;
  _stateLabel = label;
  _drawHeader();
}

void StatusDisplay::log(const String& line) {
  for (int i = 0; i < kLogLines - 1; ++i) _log[i] = _log[i + 1];
  _log[kLogLines - 1] = truncateToWidth(line);
  _drawLog();
}

void StatusDisplay::progress(const String& label, float pct) {
  if (pct < 0) pct = 0;
  if (pct > 1) pct = 1;

  const int w = _tft.width();
  const int barX = 2, barY = 16, barW = w - 4, barH = 8;

  _tft.drawRect(barX, barY, barW, barH, TFT_DARKGREY);
  const int fill = (int)((barW - 2) * pct);
  _tft.fillRect(barX + 1, barY + 1, fill, barH - 2, TFT_CYAN);
  _tft.fillRect(barX + 1 + fill, barY + 1, (barW - 2) - fill, barH - 2, TFT_BLACK);

  // Percentage label centred over the bar.
  char pctStr[24];
  snprintf(pctStr, sizeof(pctStr), "%s %d%%", label.c_str(), (int)(pct * 100));
  _tft.setTextColor(TFT_WHITE, TFT_BLACK);
  _tft.setTextDatum(TC_DATUM);
  _tft.drawString(pctStr, w / 2, barY - 1, 1);
  _tft.setTextDatum(TL_DATUM);
  _progressVisible = true;
}

void StatusDisplay::clearProgress() {
  if (!_progressVisible) return;
  _tft.fillRect(0, 16, _tft.width(), 9, TFT_BLACK);
  _progressVisible = false;
}

void StatusDisplay::_drawHeader() {
  const int w = _tft.width();
  const uint16_t bg = _stateColor(_state);
  _tft.fillRect(0, 0, w, 15, bg);
  _tft.setTextColor(TFT_WHITE, bg);
  _tft.setTextDatum(TL_DATUM);

  String header = String(_stateName(_state));
  if (_stateLabel.length()) header += ": " + _stateLabel;
  _tft.drawString(truncateToWidth(header, 20), 2, 0, 2);
}

void StatusDisplay::_drawLog() {
  const int w = _tft.width();
  const int h = _tft.height();
  _tft.fillRect(0, kLogTop, w, h - kLogTop, TFT_BLACK);
  _tft.setTextColor(TFT_GREEN, TFT_BLACK);
  _tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < kLogLines; ++i) {
    if (_log[i].length() == 0) continue;
    _tft.drawString(_log[i], 2, kLogTop + i * kLineH, 1);
  }
}

uint16_t StatusDisplay::_stateColor(DeviceState s) const {
  switch (s) {
    case DeviceState::Boot: return TFT_BLUE;
    case DeviceState::Idle: return TFT_DARKGREY;
    case DeviceState::Scanning: return TFT_PURPLE;
    case DeviceState::ConnectingHome: return TFT_OLIVE;
    case DeviceState::Home: return TFT_DARKGREEN;
    case DeviceState::ConnectingRemote: return TFT_ORANGE;
    case DeviceState::Remote: return TFT_MAROON;
    case DeviceState::Working: return TFT_NAVY;
    case DeviceState::Error: return TFT_RED;
  }
  return TFT_BLACK;
}

const char* StatusDisplay::_stateName(DeviceState s) const {
  switch (s) {
    case DeviceState::Boot: return "BOOT";
    case DeviceState::Idle: return "IDLE";
    case DeviceState::Scanning: return "SCAN";
    case DeviceState::ConnectingHome: return "HOME..";
    case DeviceState::Home: return "HOME";
    case DeviceState::ConnectingRemote: return "SITE..";
    case DeviceState::Remote: return "SITE";
    case DeviceState::Working: return "WORK";
    case DeviceState::Error: return "ERROR";
  }
  return "?";
}
