// drv_switch.cpp - Nintendo Switch Pro コントローラー / Joy-Con
//
// プロトコルは dekuNukem/Nintendo_Switch_Reverse_Engineering に準拠。
// 接続後にサブコマンドを順に送り (応答 0x21 を待ってから次へ)、
// 標準フルモード(0x30: ボタン + スティック + IMU 60Hz×3サンプル)に切り替える。
#include "BTGamepad.h"

#include <math.h>
#include "esp_log.h"

using gp::Channel;

static const char* TAG = "GP_SWITCH";

enum SwStep : uint8_t {
  SW_DEVICE_INFO,
  SW_STICK_CAL,
  SW_IMU_CAL,
  SW_REPORT_MODE,
  SW_IMU_ON,
  SW_VIBRATION_ON,
  SW_PLAYER_LEDS,
  SW_DONE,
};

static constexpr uint32_t SW_STEP_TIMEOUT_MS = 150;
static constexpr uint8_t SW_MAX_RETRIES = 5;
static constexpr uint32_t SW_MIN_SEND_INTERVAL_MS = 15;
static constexpr uint32_t SW_RUMBLE_REFRESH_MS = 100;

static const uint8_t SW_PLAYER_PATTERN[6] = {0x0, 0x1, 0x3, 0x7, 0xF, 0xF};

static inline int16_t le16(const uint8_t* p) { return (int16_t)(p[0] | (p[1] << 8)); }

static void decode12(const uint8_t* p, int16_t& a, int16_t& b) {
  a = (int16_t)(p[0] | ((p[1] & 0x0F) << 8));
  b = (int16_t)((p[1] >> 4) | (p[2] << 4));
}

static uint8_t stepSubcmd(uint8_t step) {
  switch (step) {
    case SW_DEVICE_INFO: return 0x02;
    case SW_STICK_CAL:
    case SW_IMU_CAL: return 0x10;
    case SW_REPORT_MODE: return 0x03;
    case SW_IMU_ON: return 0x40;
    case SW_VIBRATION_ON: return 0x48;
    case SW_PLAYER_LEDS: return 0x30;
    default: return 0;
  }
}

// ---------------------------------------------------------------------------
// 送信
// ---------------------------------------------------------------------------
// HD振動: large → 160Hz (低周波), small → 320Hz (高周波) の振幅として符号化
static uint8_t encodeAmp(uint8_t v) {
  if (v == 0) return 0;
  float amp = v / 255.0f;
  long e;
  if (amp > 0.23f) e = lroundf(log2f(amp * 8.7f) * 32.0f);
  else if (amp > 0.12f) e = lroundf(log2f(amp * 17.0f) * 16.0f);
  else e = lroundf(amp / 0.12f * 16.5f);
  return (uint8_t)constrain(e, 0, 100);
}

void Gamepad::swEncodeRumble(uint8_t* out) const {
  uint8_t eh = encodeAmp(_out.small);
  uint8_t el = encodeAmp(_out.large);
  const uint16_t hf = 0x0100;  // 320Hz
  const uint8_t lf = 0x40;     // 160Hz
  uint8_t hfAmp = eh * 2;
  uint16_t lfAmp = ((el & 1) ? 0x8000 : 0) | (0x40 + el / 2);
  uint8_t side[4] = {
    (uint8_t)(hf & 0xFF),
    (uint8_t)(hfAmp + (hf >> 8)),
    (uint8_t)(lf | (lfAmp >> 8)),
    (uint8_t)(lfAmp & 0xFF),
  };
  memcpy(out, side, 4);      // 左
  memcpy(out + 4, side, 4);  // 右
}

void Gamepad::swSendSubcmd(uint8_t id, const uint8_t* args, uint8_t len) {
  uint8_t f[1 + 11 + 38] = {};
  f[0] = 0xA2;
  f[1] = 0x01;  // 振動 + サブコマンド
  f[2] = _swCounter++ & 0x0F;
  swEncodeRumble(f + 3);
  f[11] = id;
  if (len > sizeof(f) - 12) len = sizeof(f) - 12;
  if (len) memcpy(f + 12, args, len);
  gp::classicSend(Channel::Interrupt, f, 12 + len);
}

// 現在のステップのサブコマンドを送る (コア0 / ユーザータスクのどちらからも呼ばれる)
void Gamepad::swNextStep() {
  uint8_t step = _swStep;
  _swStepMs = millis();
  switch (step) {
    case SW_DEVICE_INFO:
      swSendSubcmd(0x02, nullptr, 0);
      break;
    case SW_STICK_CAL: {
      const uint8_t a[5] = {0x3D, 0x60, 0x00, 0x00, 18};  // SPI 0x603D, 18 bytes
      swSendSubcmd(0x10, a, sizeof(a));
      break;
    }
    case SW_IMU_CAL: {
      const uint8_t a[5] = {0x20, 0x60, 0x00, 0x00, 24};  // SPI 0x6020, 24 bytes
      swSendSubcmd(0x10, a, sizeof(a));
      break;
    }
    case SW_REPORT_MODE: {
      const uint8_t a = 0x30;
      swSendSubcmd(0x03, &a, 1);
      break;
    }
    case SW_IMU_ON: {
      const uint8_t a = 0x01;
      swSendSubcmd(0x40, &a, 1);
      break;
    }
    case SW_VIBRATION_ON: {
      const uint8_t a = 0x01;
      swSendSubcmd(0x48, &a, 1);
      break;
    }
    case SW_PLAYER_LEDS: {
      uint8_t a = _out.playerNum ? SW_PLAYER_PATTERN[_out.playerNum] : (_out.playerLeds & 0x0F);
      swSendSubcmd(0x30, &a, 1);
      break;
    }
    default:
      break;
  }
}

void Gamepad::swStart() {
  for (int i = 0; i < 4; i++) _stickCal[i] = {2048, 1400, 1400};
  for (int i = 0; i < 3; i++) {
    _gyroCal[i] = {0.0f, 0.070f};          // ±2000dps 相当
    _accCal[i] = {0.0f, 1.0f / 4096.0f};  // ±8G
  }
  _swRetries = 0;
  _swStep = SW_DEVICE_INFO;
  ESP_LOGI(TAG, "init start");
  swNextStep();
}

// ---------------------------------------------------------------------------
// 受信 (コア0)
// ---------------------------------------------------------------------------
static int8_t stickValue(int16_t raw, const int16_t cal[3]) {
  int v = raw - cal[0];
  int out = v >= 0 ? v * 127 / (cal[1] ? cal[1] : 1) : v * 128 / (cal[2] ? cal[2] : 1);
  return (int8_t)constrain(out, -128, 127);
}

void Gamepad::swParseFull(const uint8_t* r) {
  GamepadState& s = _work;
  // バッテリー: 上位4bit = 残量(0,2,4,6,8) | 充電中bit
  uint8_t h = r[2] >> 4;
  s.charging = (h & 0x01) != 0;
  s.battery = (uint8_t)((h >> 1) * 25 > 100 ? 100 : (h >> 1) * 25);

  const uint8_t br = r[3], bs = r[4], bl = r[5];
  uint32_t m = 0;
  if (br & 0x01) m |= GPButton::SwitchY;
  if (br & 0x02) m |= GPButton::SwitchX;
  if (br & 0x04) m |= GPButton::SwitchB;
  if (br & 0x08) m |= GPButton::SwitchA;
  if (br & 0x40) m |= GPButton::R1;
  if (br & 0x80) m |= GPButton::ZR;
  if (bs & 0x01) m |= GPButton::Minus;
  if (bs & 0x02) m |= GPButton::Plus;
  if (bs & 0x04) m |= GPButton::R3;
  if (bs & 0x08) m |= GPButton::L3;
  if (bs & 0x10) m |= GPButton::Home;
  if (bs & 0x20) m |= GPButton::Capture;
  if (bl & 0x01) m |= GPButton::Down;
  if (bl & 0x02) m |= GPButton::Up;
  if (bl & 0x04) m |= GPButton::Right;
  if (bl & 0x08) m |= GPButton::Left;
  if (bl & 0x40) m |= GPButton::L1;
  if (bl & 0x80) m |= GPButton::ZL;
  if ((br | bl) & 0x10) m |= GPButton::SR;
  if ((br | bl) & 0x20) m |= GPButton::SL;
  s.buttons = m;
  s.l2 = (m & GPButton::ZL) ? 255 : 0;
  s.r2 = (m & GPButton::ZR) ? 255 : 0;

  int16_t lx, ly, rx, ry;
  decode12(r + 6, lx, ly);
  decode12(r + 9, rx, ry);
  const int16_t cLX[3] = {_stickCal[0].center, _stickCal[0].plus, _stickCal[0].minus};
  const int16_t cLY[3] = {_stickCal[1].center, _stickCal[1].plus, _stickCal[1].minus};
  const int16_t cRX[3] = {_stickCal[2].center, _stickCal[2].plus, _stickCal[2].minus};
  const int16_t cRY[3] = {_stickCal[3].center, _stickCal[3].plus, _stickCal[3].minus};
  s.lx = _model == GamepadModel::JoyConR ? 0 : stickValue(lx, cLX);
  s.ly = _model == GamepadModel::JoyConR ? 0 : stickValue(ly, cLY);
  s.rx = _model == GamepadModel::JoyConL ? 0 : stickValue(rx, cRX);
  s.ry = _model == GamepadModel::JoyConL ? 0 : stickValue(ry, cRY);
}

void Gamepad::swInput(const uint8_t* r, uint16_t len) {
  const uint8_t id = r[0];
  if ((id != 0x30 && id != 0x21) || len < 13) return;  // 0x3F (簡易モード) は初期化完了まで無視
  swParseFull(r);
  GamepadState& s = _work;

  if (id == 0x30 && len >= 49) {
    // IMU 3サンプルのうち最新を使う。軸を PlayStation と同じ (x=右, y=上, z=手前) に揃える
    const uint8_t* p = r + 13 + 24;
    int16_t ax = le16(p), ay = le16(p + 2), az = le16(p + 4);
    int16_t gx = le16(p + 6), gy = le16(p + 8), gz = le16(p + 10);
    s.accelRaw[0] = ax; s.accelRaw[1] = ay; s.accelRaw[2] = az;
    s.gyroRaw[0] = gx; s.gyroRaw[1] = gy; s.gyroRaw[2] = gz;
    float a[3], g[3];
    float ra[3], rg[3];
    for (int i = 0; i < 3; i++) {
      ra[i] = (s.accelRaw[i] - _accCal[i].bias) * _accCal[i].scale;
      rg[i] = (s.gyroRaw[i] - _gyroCal[i].bias) * _gyroCal[i].scale;
    }
    a[0] = -ra[1]; a[1] = ra[2]; a[2] = -ra[0];
    g[0] = -rg[1]; g[1] = rg[2]; g[2] = -rg[0];
    applyMotion(s, g, a);
  }

  if (id == 0x21 && len >= 15) {
    const uint8_t sub = r[14];
    const uint8_t step = _swStep;
    if (step < SW_DONE && sub == stepSubcmd(step)) {
      const uint8_t* d = r + 15;
      if (step == SW_DEVICE_INFO && len >= 18) {
        // d[2]: 1=Joy-Con(L) 2=Joy-Con(R) 3=Pro
        if (d[2] == 1) _model = GamepadModel::JoyConL;
        else if (d[2] == 2) _model = GamepadModel::JoyConR;
        else if (d[2] == 3) _model = GamepadModel::SwitchPro;
      } else if (step == SW_STICK_CAL && len >= 15 + 5 + 18) {
        const uint8_t* c = d + 5;
        bool blank = true;
        for (int i = 0; i < 18; i++) blank &= (c[i] == 0xFF);
        if (!blank) {
          int16_t x, y, cx, cy, mx, my;
          // 左: 中心より上の最大, 中心, 中心より下の最小
          decode12(c + 0, x, y);
          decode12(c + 3, cx, cy);
          decode12(c + 6, mx, my);
          _stickCal[0] = {cx, x, mx};
          _stickCal[1] = {cy, y, my};
          // 右: 中心, 下の最小, 上の最大
          decode12(c + 9, cx, cy);
          decode12(c + 12, mx, my);
          decode12(c + 15, x, y);
          _stickCal[2] = {cx, x, mx};
          _stickCal[3] = {cy, y, my};
        }
      } else if (step == SW_IMU_CAL && len >= 15 + 5 + 24) {
        const uint8_t* c = d + 5;
        ESP_LOGI(TAG, "IMU cal: acc0 %d %d %d accS %d %d %d gyro0 %d %d %d gyroS %d %d %d", le16(c), le16(c + 2),
                 le16(c + 4), le16(c + 6), le16(c + 8), le16(c + 10), le16(c + 12), le16(c + 14), le16(c + 16),
                 le16(c + 18), le16(c + 20), le16(c + 22));
        if (!(c[0] == 0xFF && c[1] == 0xFF)) {
          for (int i = 0; i < 3; i++) {
            int16_t accOrigin = le16(c + i * 2);
            int16_t gyroOrigin = le16(c + 12 + i * 2);
            if (16384 - accOrigin != 0) _accCal[i] = {0.0f, 4.0f / (16384 - accOrigin)};
            if (13371 - gyroOrigin != 0) _gyroCal[i] = {(float)gyroOrigin, 936.0f / (13371 - gyroOrigin)};
          }
        }
      }
      _swRetries = 0;
      _swStep = step + 1;
      if (_swStep < SW_DONE) {
        swNextStep();
      } else {
        ESP_LOGI(TAG, "init done");
      }
    }
  }

  s.ready = (_swStep == SW_DONE);
  publish(s);
}

// ---------------------------------------------------------------------------
// ユーザータスク
// ---------------------------------------------------------------------------
void Gamepad::swUpdate(uint32_t now) {
  uint8_t step = _swStep;
  if (step < SW_DONE) {
    if (now - _swStepMs > SW_STEP_TIMEOUT_MS) {
      if (_swRetries < SW_MAX_RETRIES) {
        _swRetries++;
      } else {  // 応答しないステップは飛ばす (非純正品対策)
        ESP_LOGW(TAG, "step %u skipped", step);
        _swRetries = 0;
        _swStep = step + 1;
      }
      swNextStep();
    }
    return;
  }
  if (now - _lastSendMs < SW_MIN_SEND_INTERVAL_MS) return;

  uint8_t leds = _out.playerNum ? SW_PLAYER_PATTERN[_out.playerNum] : (_out.playerLeds & 0x0F);
  uint8_t sentLeds = _sentOut.playerNum ? SW_PLAYER_PATTERN[_sentOut.playerNum % 6] : (_sentOut.playerLeds & 0x0F);
  bool rumbleChanged = _out.large != _sentOut.large || _out.small != _sentOut.small;
  bool rumbleActive = _out.large || _out.small;

  if (_outDirty || leds != sentLeds) {
    swSendSubcmd(0x30, &leds, 1);  // サブコマンドには振動データも含まれる
  } else if (rumbleChanged || (rumbleActive && now - _lastSendMs >= SW_RUMBLE_REFRESH_MS)) {
    uint8_t f[1 + 10] = {};
    f[0] = 0xA2;
    f[1] = 0x10;  // 振動のみ
    f[2] = _swCounter++ & 0x0F;
    swEncodeRumble(f + 3);
    gp::classicSend(Channel::Interrupt, f, sizeof(f));
  } else {
    return;
  }
  _sentOut = _out;
  _outDirty = false;
  _lastSendMs = now;
}
