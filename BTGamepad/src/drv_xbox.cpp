// drv_xbox.cpp - Xbox Wireless Controller (BLE, ファームウェア 5.x 以降)
//
// 入力レポート (レポートIDなし, 16〜17 bytes)
//   [0-1]LX [2-3]LY [4-5]RX [6-7]RY (0..65535)  [8-9]LT [10-11]RT (0..1023)
//   [12]十字キー(0=なし,1=上..8=左上)  [13][14]ボタン  [15]Share (Series)
// 出力レポート ID 3 (振動, 8 bytes)
//   [0]有効ビット [1]LT [2]RT [3]左(強) [4]右(弱) 各0..100  [5]持続(10ms) [6]遅延 [7]繰り返し
#include "BTGamepad.h"

static constexpr uint32_t XB_MIN_SEND_INTERVAL_MS = 20;
static constexpr uint32_t XB_RUMBLE_REFRESH_MS = 2000;  // 持続時間 2.55s が切れる前に再送

static inline uint16_t le16u(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static inline int8_t axis16(uint16_t v, bool invert) {
  int x = ((int)v - 32768) >> 8;
  if (invert) x = -x - 1;
  return (int8_t)constrain(x, -128, 127);
}

void Gamepad::xbInput(const uint8_t* d, uint16_t len) {
  if (len < 15) return;
  GamepadState& s = _work;
  s.lx = axis16(le16u(d + 0), false);
  s.ly = axis16(le16u(d + 2), true);
  s.rx = axis16(le16u(d + 4), false);
  s.ry = axis16(le16u(d + 6), true);
  uint16_t lt = le16u(d + 8) & 0x3FF;
  uint16_t rt = le16u(d + 10) & 0x3FF;
  s.l2 = lt >> 2;
  s.r2 = rt >> 2;

  uint32_t m = (d[12] >= 1 && d[12] <= 8) ? hatToButtons(d[12] - 1) : 0;
  const uint8_t b0 = d[13], b1 = d[14];
  if (b0 & 0x01) m |= GPButton::XboxA;
  if (b0 & 0x02) m |= GPButton::XboxB;
  if (b0 & 0x08) m |= GPButton::XboxX;
  if (b0 & 0x10) m |= GPButton::XboxY;
  if (b0 & 0x40) m |= GPButton::LB;
  if (b0 & 0x80) m |= GPButton::RB;
  if (b1 & 0x04) m |= GPButton::View;
  if (b1 & 0x08) m |= GPButton::Menu;
  if (b1 & 0x10) m |= GPButton::Home;
  if (b1 & 0x20) m |= GPButton::L3;
  if (b1 & 0x40) m |= GPButton::R3;
  if (len >= 16 && (d[15] & 0x01)) m |= GPButton::Capture;
  if (s.l2 > 32) m |= GPButton::LT;
  if (s.r2 > 32) m |= GPButton::RT;
  s.buttons = m;
  s.ready = true;
  publish(s);
}

void Gamepad::xbUpdate(uint32_t now) {
  if (now - _lastSendMs < XB_MIN_SEND_INTERVAL_MS) return;
  bool changed = _out.large != _sentOut.large || _out.small != _sentOut.small ||
                 _out.trigRumbleL != _sentOut.trigRumbleL || _out.trigRumbleR != _sentOut.trigRumbleR;
  bool active = _out.large || _out.small || _out.trigRumbleL || _out.trigRumbleR;
  if (!changed && !(active && now - _lastSendMs >= XB_RUMBLE_REFRESH_MS)) return;

  auto pct = [](uint8_t v) -> uint8_t { return (uint8_t)((v * 100 + 127) / 255); };
  uint8_t r[8] = {
    0x0F,  // 左右モーター + 左右トリガー
    pct(_out.trigRumbleL), pct(_out.trigRumbleR), pct(_out.large), pct(_out.small),
    0xFF,  // 持続 2.55s
    0x00, 0x00,
  };
  if (gp::bleWriteOutput(r, sizeof(r)) || !changed) {
    _sentOut = _out;
    _outDirty = false;
  }
  _lastSendMs = now;
}
