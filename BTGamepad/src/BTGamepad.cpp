// BTGamepad.cpp - 共通部 (接続管理・スナップショット・姿勢推定・出力セッター)
#include "BTGamepad.h"

#include <math.h>
#include "esp_bt_device.h"
#include "esp_system.h"
#include "esp_log.h"
#include <Preferences.h>

using gp::Channel;
using gp::Transport;

static const char* TAG = "Gamepad";

Gamepad* Gamepad::_instance = nullptr;

static bool parseMac(const char* s, uint8_t out[6]) {
  unsigned v[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
  return true;
}

static String macToString(const uint8_t* a) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
  return String(buf);
}

Gamepad::Gamepad() {
  _instance = this;
  _out.trigL = PSTriggerEffect::off();
  _out.trigR = PSTriggerEffect::off();
  _out.b = 64;
  _out.playerNum = 1;
}

// ---------------------------------------------------------------------------
// 開始・接続
// ---------------------------------------------------------------------------
bool Gamepad::begin(const GamepadConfig& config) {
  gp::TransportCallbacks cb = {&Gamepad::cbClassicData, &Gamepad::cbBleData, &Gamepad::cbLink, &Gamepad::cbName};
  Preferences prefs;
  if (prefs.begin("btgamepad", false)) {
    _locked = prefs.isKey("lock") && prefs.getBytes("lock", _lockAddr, 6) == 6;
    prefs.end();
  }
  if (_locked) gp::setLock(_lockAddr);
  if (!gp::transportInit(cb, config.classic, config.ble, config.deviceName)) return false;
  if (_locked) ESP_LOGI(TAG, "locked to %s", lockedAddress().c_str());
  ESP_LOGI(TAG, "ready. local address %s", localAddress().c_str());
  return true;
}

bool Gamepad::begin(const char* localMac, const GamepadConfig& config) {
  uint8_t mac[6];
  if (!parseMac(localMac, mac)) {
    ESP_LOGE(TAG, "invalid MAC address: %s", localMac);
    return false;
  }
  mac[5] -= 2;  // BT アドレス = ベースMAC + 2
  esp_base_mac_addr_set(mac);
  return begin(config);
}

bool Gamepad::startPairing(uint32_t timeoutMs) {
  _pairingRequested = gp::startScan(timeoutMs);
  return _pairingRequested;
}

bool Gamepad::lockController(const char* mac) {
  uint8_t a[6];
  if (mac) {
    if (!parseMac(mac, a)) return false;
  } else {
    if (!_connected) return false;
    memcpy(a, gp::remoteAddress(), 6);
  }
  memcpy(_lockAddr, a, 6);
  _locked = true;
  gp::setLock(_lockAddr);
  saveLock();
  return true;
}

void Gamepad::unlockController() {
  _locked = false;
  gp::setLock(nullptr);
  saveLock();
}

String Gamepad::lockedAddress() const { return _locked ? macToString(_lockAddr) : String(); }

void Gamepad::saveLock() {
  Preferences prefs;
  if (!prefs.begin("btgamepad", false)) return;
  if (_locked) prefs.putBytes("lock", _lockAddr, 6);
  else prefs.remove("lock");
  prefs.end();
}
void Gamepad::stopPairing() { gp::stopScan(); }

bool Gamepad::connect(const char* classicMac) {
  uint8_t mac[6];
  if (!parseMac(classicMac, mac)) return false;
  return gp::classicConnect(mac);
}

void Gamepad::disconnect() { gp::disconnect(); }
void Gamepad::clearPairings() { gp::clearBonds(); }

String Gamepad::localAddress() const {
  const uint8_t* a = esp_bt_dev_get_address();
  return a ? macToString(a) : String();
}

String Gamepad::remoteAddress() const { return macToString(gp::remoteAddress()); }

const char* Gamepad::modelName() const {
  switch (_model) {
    case GamepadModel::DualShock4: return "DualShock 4";
    case GamepadModel::DualSense: return "DualSense";
    case GamepadModel::SwitchPro: return "Switch Pro";
    case GamepadModel::JoyConL: return "Joy-Con (L)";
    case GamepadModel::JoyConR: return "Joy-Con (R)";
    case GamepadModel::Xbox: return "Xbox";
    default: return "Unknown";
  }
}

bool Gamepad::hasMotion() const { return isPlayStation() || isSwitch(); }

// ---------------------------------------------------------------------------
// トランスポートからのコールバック (コア0)
// ---------------------------------------------------------------------------
void Gamepad::cbLink(Transport t, bool connected) {
  Gamepad* self = _instance;
  if (!self) return;
  if (connected) {
    memset(&self->_work, 0, sizeof(self->_work));
    portENTER_CRITICAL(&self->_mux);
    memset(&self->_shared, 0, sizeof(self->_shared));
    portEXIT_CRITICAL(&self->_mux);
    for (int i = 0; i < 3; i++) {
      self->_gyroCal[i] = {0.0f, 1.0f / 16.0f};
      self->_accCal[i] = {0.0f, 1.0f / 8192.0f};
    }
    self->_yaw = self->_tiltPitch = self->_tiltRoll = 0;
    self->_lastMotionUs = 0;
    self->_swStep = 0xFF;
    if (t == Transport::Ble) {
      self->_model = GamepadModel::Xbox;
    } else {
      self->_model = GamepadModel::Unknown;
      // PlayStation なら拡張モードに切り替わる。Switch には無害
      self->psRequestCalibration();
    }
  }
  self->_linkUp = connected;
}

void Gamepad::cbName(const char* name) {
  Gamepad* self = _instance;
  if (!self || self->isSwitch()) return;
  GamepadModel m = GamepadModel::Unknown;
  if (strstr(name, "Pro Controller")) m = GamepadModel::SwitchPro;
  else if (strstr(name, "Joy-Con (L)")) m = GamepadModel::JoyConL;
  else if (strstr(name, "Joy-Con (R)")) m = GamepadModel::JoyConR;
  if (m != GamepadModel::Unknown) {
    self->_model = m;
    self->swStart();
  }
}

void Gamepad::cbClassicData(Channel ch, const uint8_t* d, uint16_t len) {
  Gamepad* self = _instance;
  if (!self || len < 2) return;
  if (ch == Channel::Interrupt && d[0] == 0xA1) {
    const uint8_t* r = d + 1;
    len--;
    self->_rawLen = len < sizeof(self->_raw) ? len : sizeof(self->_raw);
    memcpy(self->_raw, r, self->_rawLen);
    uint8_t id = r[0];
    if (id == 0x21 || id == 0x30 || id == 0x3F) {  // Switch 系のレポートID
      if (!self->isSwitch()) {
        self->_model = GamepadModel::SwitchPro;  // 細かい種別はデバイス情報で確定
        self->swStart();
      }
      self->swInput(r, len);
    } else {
      self->psInput(r, len);
    }
  } else if (ch == Channel::Control && d[0] == 0xA3) {
    self->psFeature(d + 1, len - 1);
  }
}

void Gamepad::cbBleData(const uint8_t* d, uint16_t len, bool battery) {
  Gamepad* self = _instance;
  if (!self || len == 0) return;
  if (battery) {
    self->_work.battery = d[0] > 100 ? 100 : d[0];
    self->publish(self->_work);
    return;
  }
  self->_rawLen = len < sizeof(self->_raw) ? len : sizeof(self->_raw);
  memcpy(self->_raw, d, self->_rawLen);
  self->xbInput(d, len);
}

// ---------------------------------------------------------------------------
// 共通処理
// ---------------------------------------------------------------------------
void Gamepad::publish(GamepadState& s) {
  s.reportCount++;
  s.lastReportMs = millis();
  portENTER_CRITICAL(&_mux);
  _shared = s;
  portEXIT_CRITICAL(&_mux);
}

uint32_t Gamepad::hatToButtons(uint8_t hat) {
  static const uint32_t table[9] = {
    GPButton::Up, GPButton::Up | GPButton::Right, GPButton::Right, GPButton::Down | GPButton::Right,
    GPButton::Down, GPButton::Down | GPButton::Left, GPButton::Left, GPButton::Up | GPButton::Left, 0,
  };
  return table[hat < 8 ? hat : 8];
}

float Gamepad::normAxis(int8_t v, float dz) {
  float f = v / 127.0f;
  if (f > 1.0f) f = 1.0f;
  float a = fabsf(f);
  if (a < dz) return 0.0f;
  return copysignf((a - dz) / (1.0f - dz), f);
}

// g: deg/s, a: G (軸は統一済み)
void Gamepad::applyMotion(GamepadState& s, const float gIn[3], const float a[3]) {
  float g[3] = {gIn[0], gIn[1], gIn[2]};
  uint32_t now = micros();
  float dt = _lastMotionUs ? (now - _lastMotionUs) * 1e-6f : 0.0f;
  if (dt > 0.1f) dt = 0.0f;  // 途切れた後は積分しない
  _lastMotionUs = now;

  if (_gyroCalibUntil) {
    for (int i = 0; i < 3; i++) _gyroCalibSum[i] += g[i];
    _gyroCalibCount++;
    if ((int32_t)(millis() - _gyroCalibUntil) >= 0) {
      if (_gyroCalibCount > 0)
        for (int i = 0; i < 3; i++) _gyroOffset[i] = _gyroCalibSum[i] / _gyroCalibCount;
      _gyroCalibUntil = 0;
    }
  }
  for (int i = 0; i < 3; i++) g[i] -= _gyroOffset[i];

  s.gyro = {g[0], g[1], g[2]};
  s.accel = {a[0], a[1], a[2]};

  // 傾き: 水平に置いた状態で 0 (重力は +Y)
  const float k = 0.1f;
  _tiltPitch += (atan2f(a[2], a[1]) * RAD_TO_DEG - _tiltPitch) * k;
  _tiltRoll += (atan2f(a[0], a[1]) * RAD_TO_DEG - _tiltRoll) * k;
  _yaw += g[1] * dt;
  if (_yaw > 180.0f) _yaw -= 360.0f;
  if (_yaw < -180.0f) _yaw += 360.0f;
}

void Gamepad::calibrateGyro(uint32_t durationMs) {
  for (int i = 0; i < 3; i++) {
    _gyroOffset[i] = 0;
    _gyroCalibSum[i] = 0;
  }
  _gyroCalibCount = 0;
  uint32_t until = millis() + durationMs;
  _gyroCalibUntil = until ? until : 1;
}

// ---------------------------------------------------------------------------
// update (ユーザータスク)
// ---------------------------------------------------------------------------
void Gamepad::update() {
  bool link = _linkUp;
  if (link != _connected) {
    _connected = link;
    if (link) {
      _calibRetries = 0;
      _lastCalibReqMs = millis();
      _lightbarSetup = false;
      _outDirty = true;
      _lastSendMs = 0;
      memset(&_sentOut, 0xFF, sizeof(_sentOut));
      // 固定中に新しい機体をペアリングしたら固定先を移す
      if (_pairingRequested && _locked && memcmp(_lockAddr, gp::remoteAddress(), 6) != 0) {
        memcpy(_lockAddr, gp::remoteAddress(), 6);
        gp::setLock(_lockAddr);
        saveLock();
        ESP_LOGI(TAG, "lock moved to %s", lockedAddress().c_str());
      }
      _pairingRequested = false;
      if (_onConnect) _onConnect(*this);
    } else {
      memset(&_cur, 0, sizeof(_cur));
      memset(&_prev, 0, sizeof(_prev));
      _model = GamepadModel::Unknown;
      if (_onDisconnect) _onDisconnect(*this);
      return;
    }
  }

  _prev = _cur;
  portENTER_CRITICAL(&_mux);
  _cur = _shared;
  portEXIT_CRITICAL(&_mux);

  if (!_connected) return;
  uint32_t now = millis();
  GamepadModel m = _model;
  if (m == GamepadModel::Xbox) xbUpdate(now);
  else if (isSwitch()) swUpdate(now);
  else psUpdate(now);  // PlayStation または判定待ち
}

// ---------------------------------------------------------------------------
// 出力セッター
// ---------------------------------------------------------------------------
void Gamepad::setRumble(uint8_t large, uint8_t small) {
  _out.large = large;
  _out.small = small;
}

void Gamepad::setTriggerRumble(uint8_t left, uint8_t right) {
  _out.trigRumbleL = left;
  _out.trigRumbleR = right;
}

void Gamepad::setLightbar(uint8_t r, uint8_t g, uint8_t b) {
  _out.r = r;
  _out.g = g;
  _out.b = b;
}

void Gamepad::setLightbarFlash(uint8_t onTime, uint8_t offTime) {
  _out.flashOn = onTime;
  _out.flashOff = offTime;
}

void Gamepad::setPlayerLeds(uint8_t mask) {
  _out.playerLeds = mask & 0x1F;
  _out.playerNum = 0;
}

void Gamepad::setPlayerNumber(uint8_t n) {
  if (n == 0) setPlayerLeds(0);
  else _out.playerNum = n > 5 ? 5 : n;
}
void Gamepad::setMicLed(PSMicLed mode) { _out.mic = mode; }
void Gamepad::setTriggerL(const PSTriggerEffect& e) { _out.trigL = e; }
void Gamepad::setTriggerR(const PSTriggerEffect& e) { _out.trigR = e; }
