// drv_playstation.cpp - DualShock 4 / DualSense
//
// レポート形式は Linux drivers/hid/hid-playstation.c の構造体定義に準拠。
#include "BTGamepad.h"

#include <math.h>
#include "esp_log.h"

using gp::Channel;

static constexpr uint8_t REPORT_BASIC = 0x01;
static constexpr uint8_t REPORT_DS4_FULL = 0x11;
static constexpr uint8_t REPORT_DS5_FULL = 0x31;
static constexpr uint8_t FEATURE_CALIBRATION = 0x05;
static constexpr uint16_t OUTPUT_BT_SIZE = 78;  // DS4 0x11 / DS5 0x31 共通 (CRC込み)
static constexpr uint32_t MIN_SEND_INTERVAL_MS = 10;

static uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

static inline int16_t le16(const uint8_t* p) { return (int16_t)(p[0] | (p[1] << 8)); }

static inline int8_t axisX(uint8_t v) { return (int8_t)((int)v - 128); }
static inline int8_t axisY(uint8_t v) { return (int8_t)(127 - (int)v); }  // 上を + にする

static void parseTouch(const uint8_t* p, GPTouchPoint& t) {
  t.active = (p[0] & 0x80) == 0;
  t.id = p[0] & 0x7F;
  t.x = (uint16_t)(p[1] | ((p[2] & 0x0F) << 8));
  t.y = (uint16_t)((p[2] >> 4) | (p[3] << 4));
}

void Gamepad::psRequestCalibration() {
  // 校正データ(Feature 0x05)を読むとコントローラーは拡張レポート(0x11/0x31)を送り始める
  const uint8_t req[2] = {0x43 /* GET_REPORT | Feature */, FEATURE_CALIBRATION};
  gp::classicSend(Channel::Control, req, sizeof(req));
}

void Gamepad::psFeature(const uint8_t* r, uint16_t len) {
  if (r[0] != FEATURE_CALIBRATION || len < 35) return;

  // DS4(BT) と DualSense は同じ並び
  const int16_t bias[3] = {le16(r + 1), le16(r + 3), le16(r + 5)};
  const int16_t plus[3] = {le16(r + 7), le16(r + 11), le16(r + 15)};
  const int16_t minus[3] = {le16(r + 9), le16(r + 13), le16(r + 17)};
  const float speed2x = (float)le16(r + 19) + (float)le16(r + 21);
  const int16_t accPlus[3] = {le16(r + 23), le16(r + 27), le16(r + 31)};
  const int16_t accMinus[3] = {le16(r + 25), le16(r + 29), le16(r + 33)};

  Calib g[3], a[3];
  bool ok = speed2x > 0;
  for (int i = 0; i < 3 && ok; i++) {
    float denom = fabsf((float)plus[i] - bias[i]) + fabsf((float)minus[i] - bias[i]);
    float range2g = (float)accPlus[i] - accMinus[i];
    if (denom <= 0 || range2g == 0) ok = false;
    // hid-playstation と同様、ジャイロのバイアスはファームウェア側で補正済みとして 0
    g[i] = {0.0f, speed2x / denom};
    a[i] = {accPlus[i] - range2g / 2.0f, 2.0f / range2g};
  }
  if (ok) {
    memcpy(_gyroCal, g, sizeof(g));
    memcpy(_accCal, a, sizeof(a));
  }
}

void Gamepad::psInput(const uint8_t* r, uint16_t len) {
  if (r[0] == REPORT_DS5_FULL && len >= 55) {
    _model = GamepadModel::DualSense;
    psParseDS5(r + 2);
  } else if (r[0] == REPORT_DS4_FULL && len >= 45) {
    _model = GamepadModel::DualShock4;
    psParseDS4(r + 3);
  } else if (r[0] == REPORT_BASIC && len >= 10) {
    psParseBasic(r);
  }
}

void Gamepad::psParseButtons(const uint8_t* b, GamepadState& s) {
  uint32_t m = hatToButtons(b[0] & 0x0F);
  if (b[0] & 0x10) m |= GPButton::Square;
  if (b[0] & 0x20) m |= GPButton::Cross;
  if (b[0] & 0x40) m |= GPButton::Circle;
  if (b[0] & 0x80) m |= GPButton::Triangle;
  if (b[1] & 0x01) m |= GPButton::L1;
  if (b[1] & 0x02) m |= GPButton::R1;
  if (b[1] & 0x04) m |= GPButton::L2;
  if (b[1] & 0x08) m |= GPButton::R2;
  if (b[1] & 0x10) m |= GPButton::Share;
  if (b[1] & 0x20) m |= GPButton::Options;
  if (b[1] & 0x40) m |= GPButton::L3;
  if (b[1] & 0x80) m |= GPButton::R3;
  if (b[2] & 0x01) m |= GPButton::PSHome;
  if (b[2] & 0x02) m |= GPButton::Touchpad;
  if (_model == GamepadModel::DualSense) {
    if (b[2] & 0x04) m |= GPButton::Mute;
    if (b[2] & 0x10) m |= GPButton::FnL;
    if (b[2] & 0x20) m |= GPButton::FnR;
    if (b[2] & 0x40) m |= GPButton::Paddle1;
    if (b[2] & 0x80) m |= GPButton::Paddle2;
  }
  s.buttons = m;
}

void Gamepad::psParseDS4(const uint8_t* x) {
  GamepadState& s = _work;
  s.lx = axisX(x[0]);
  s.ly = axisY(x[1]);
  s.rx = axisX(x[2]);
  s.ry = axisY(x[3]);
  psParseButtons(x + 4, s);
  s.l2 = x[7];
  s.r2 = x[8];
  float g[3], a[3];
  for (int i = 0; i < 3; i++) {
    s.gyroRaw[i] = le16(x + 12 + i * 2);
    s.accelRaw[i] = le16(x + 18 + i * 2);
    g[i] = (s.gyroRaw[i] - _gyroCal[i].bias) * _gyroCal[i].scale;
    a[i] = (s.accelRaw[i] - _accCal[i].bias) * _accCal[i].scale;
  }
  uint8_t level = x[29] & 0x0F;
  bool cable = (x[29] & 0x10) != 0;
  s.charging = cable && level < 10;
  s.battery = (cable && level >= 10) ? 100 : (level * 10 + 5 > 100 ? 100 : level * 10 + 5);
  if (x[32] > 0) {  // タッチレポート数
    parseTouch(x + 34, s.touch[0]);
    parseTouch(x + 38, s.touch[1]);
  }
  s.ready = true;
  applyMotion(s, g, a);
  publish(s);
}

void Gamepad::psParseDS5(const uint8_t* x) {
  GamepadState& s = _work;
  s.lx = axisX(x[0]);
  s.ly = axisY(x[1]);
  s.rx = axisX(x[2]);
  s.ry = axisY(x[3]);
  s.l2 = x[4];
  s.r2 = x[5];
  psParseButtons(x + 7, s);
  float g[3], a[3];
  for (int i = 0; i < 3; i++) {
    s.gyroRaw[i] = le16(x + 15 + i * 2);
    s.accelRaw[i] = le16(x + 21 + i * 2);
    g[i] = (s.gyroRaw[i] - _gyroCal[i].bias) * _gyroCal[i].scale;
    a[i] = (s.accelRaw[i] - _accCal[i].bias) * _accCal[i].scale;
  }
  parseTouch(x + 32, s.touch[0]);
  parseTouch(x + 36, s.touch[1]);
  s.triggerStatusR = x[41];
  s.triggerStatusL = x[42];
  uint8_t level = x[52] & 0x0F;
  uint8_t chg = x[52] >> 4;  // 0:放電 1:充電中 2:満充電
  s.battery = (chg == 2) ? 100 : (level * 10 + 5 > 100 ? 100 : level * 10 + 5);
  s.charging = chg == 1;
  s.ready = true;
  applyMotion(s, g, a);
  publish(s);
}

// 拡張モードに入る前の簡易レポート (ボタン/スティック/トリガーのみ)
void Gamepad::psParseBasic(const uint8_t* r) {
  GamepadState& s = _work;
  s.lx = axisX(r[1]);
  s.ly = axisY(r[2]);
  s.rx = axisX(r[3]);
  s.ry = axisY(r[4]);
  psParseButtons(r + 5, s);
  s.l2 = r[8];
  s.r2 = r[9];
  s.ready = false;
  publish(s);
}

// ---------------------------------------------------------------------------
// 出力 (ユーザータスク)
// ---------------------------------------------------------------------------
void Gamepad::psUpdate(uint32_t now) {
  // 拡張レポートが来ない場合は要求を再送する
  if (!_cur.ready && now - _lastCalibReqMs > 1000 && _calibRetries < 10) {
    _lastCalibReqMs = now;
    _calibRetries++;
    psRequestCalibration();
  }
  if (!isPlayStation()) return;  // 機種が判明するまで出力形式が決まらない
  if ((_outDirty || memcmp(&_out, &_sentOut, sizeof(_out)) != 0) && now - _lastSendMs >= MIN_SEND_INTERVAL_MS) {
    psSendOutput();
  }
}

void Gamepad::psSendOutput() {
  static const uint8_t playerPattern[6] = {0x00, 0x04, 0x0A, 0x15, 0x1B, 0x1F};
  uint8_t f[1 + OUTPUT_BT_SIZE] = {};
  f[0] = 0xA2;  // HIDP DATA | Output
  uint8_t* b = f + 1;

  if (_model == GamepadModel::DualSense) {
    b[0] = REPORT_DS5_FULL;
    b[1] = (uint8_t)(_outSeq << 4);
    _outSeq = (_outSeq + 1) & 0x0F;
    b[2] = 0x10;         // tag
    uint8_t* c = b + 3;  // dualsense_output_report_common (47 bytes)

    if (!_lightbarSetup) {
      // 接続直後の点灯アニメーションを解除してライトバーを操作可能にする
      c[38] = 0x02;  // valid_flag2: lightbar setup
      c[41] = 0x02;  // light out
      _lightbarSetup = true;
    } else {
      c[0] = 0x01 | 0x02;  // valid_flag0: 互換振動 + haptics select
      if (_out.trigR != _sentOut.trigR) {
        c[0] |= 0x04;
        memcpy(c + 10, _out.trigR.data, 11);
      }
      if (_out.trigL != _sentOut.trigL) {
        c[0] |= 0x08;
        memcpy(c + 21, _out.trigL.data, 11);
      }
      c[1] = 0x01 | 0x04 | 0x10;  // valid_flag1: mic LED + lightbar + player LEDs
      c[2] = _out.small;          // motor_right
      c[3] = _out.large;          // motor_left
      c[8] = (uint8_t)_out.mic;
      c[38] = 0x04;  // valid_flag2: 新ファームウェア向け互換振動
      c[43] = _out.playerNum ? playerPattern[_out.playerNum] : (_out.playerLeds & 0x1F);
      c[44] = _out.r;
      c[45] = _out.g;
      c[46] = _out.b;
      _sentOut = _out;
      _outDirty = false;
    }
  } else {
    b[0] = REPORT_DS4_FULL;
    b[1] = 0xC0;  // HID + CRC
    b[3] = 0x07;  // valid: motor + lightbar + flash
    b[6] = _out.small;
    b[7] = _out.large;
    b[8] = _out.r;
    b[9] = _out.g;
    b[10] = _out.b;
    b[11] = _out.flashOn;
    b[12] = _out.flashOff;
    _sentOut = _out;
    _outDirty = false;
  }

  uint32_t crc = crc32(f, 1 + OUTPUT_BT_SIZE - 4);
  b[OUTPUT_BT_SIZE - 4] = crc & 0xFF;
  b[OUTPUT_BT_SIZE - 3] = (crc >> 8) & 0xFF;
  b[OUTPUT_BT_SIZE - 2] = (crc >> 16) & 0xFF;
  b[OUTPUT_BT_SIZE - 1] = (crc >> 24) & 0xFF;

  gp::classicSend(Channel::Interrupt, f, sizeof(f));
  _lastSendMs = millis();
}
