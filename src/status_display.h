#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

// High-level device state, drives the coloured header on the LCD.
enum class DeviceState {
  Boot,
  Idle,
  Scanning,
  ConnectingHome,
  Home,
  ConnectingRemote,
  Remote,
  Working,
  Error,
};

// Renders a status UI on the 0.96" ST7735 (160x80 landscape):
//   [ coloured header: state + label      ]
//   [ progress bar (optional)             ]
//   [ scrolling log of recent events      ]
class StatusDisplay {
 public:
  void begin();
  void setState(DeviceState state, const String& label);
  void log(const String& line);
  void progress(const String& label, float pct);  // pct 0..1
  void clearProgress();

 private:
  static constexpr int kLogLines = 6;
  static constexpr int kLineH = 9;
  static constexpr int kLogTop = 26;

  TFT_eSPI _tft;
  DeviceState _state = DeviceState::Boot;
  String _stateLabel;
  String _log[kLogLines];
  bool _progressVisible = false;

  void _drawHeader();
  void _drawLog();
  uint16_t _stateColor(DeviceState s) const;
  const char* _stateName(DeviceState s) const;
};
