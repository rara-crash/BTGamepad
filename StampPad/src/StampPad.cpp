// StampPad.cpp - M5Stamp Pico 専用ゲームパッドライブラリ
#include "StampPad.h"

#include "esp_log.h"

static const char* TAG = "StampPad";

// 本体LED の色 (明るさ調整前)
static const uint8_t COL_IDLE[3] = {255, 0, 0};       // 待機: 赤 (ゆっくり明滅)
static const uint8_t COL_PAIRING[3] = {0, 0, 255};    // ペアリング中: 青 点滅
static const uint8_t COL_CONNECTING[3] = {0, 255, 255};
static const uint8_t COL_NOT_READY[3] = {255, 200, 0};
static const uint8_t COL_CONNECTED[3] = {0, 255, 0};  // 接続中: 緑
static const uint8_t COL_LOCKED[3] = {0, 255, 120};   // 接続中(固定): 青緑
static const uint8_t COL_LOST[3] = {255, 80, 0};      // 通信途絶: 橙 高速点滅
static const uint8_t COL_LOCK_HOLD[3] = {180, 0, 255};
static const uint8_t COL_CLEAR_HOLD[3] = {255, 255, 255};
static const uint8_t COL_LOW_BATTERY[3] = {255, 90, 0};  // 電池残量低下: 橙 (暗く点滅)
static const uint8_t PAD_LOW_BATTERY[3] = {40, 12, 0};   // コントローラーのライトバー: 暗い橙

// ---------------------------------------------------------------------------
// 開始
// ---------------------------------------------------------------------------
bool StampPad::begin(const StampPadConfig& config) {
  _cfg = config;
  memcpy(_userLightbar, _cfg.lightbar, 3);
  _userPlayer = _cfg.playerNumber;
  _padMutex = xSemaphoreCreateMutex();
  if (!_padMutex) return false;

  if (_cfg.buttonPin >= 0) pinMode(_cfg.buttonPin, INPUT);  // G39 は外付けプルアップ
  if (_cfg.ledPin >= 0) writeLed(0, 0, 0);

  if (!_pad.begin(_cfg.gamepad)) {
    if (_cfg.ledPin >= 0) writeLed(255, 0, 0);
    return false;
  }
  BaseType_t ok = xTaskCreatePinnedToCore(padTaskEntry, "stamppad", 4096, this, _cfg.padTaskPriority, nullptr,
                                          _cfg.padTaskCore);
  return ok == pdPASS;
}

bool StampPad::startControlLoop(ControlFn fn, uint32_t periodMs, uint8_t core, uint8_t priority,
                                uint32_t stackSize) {
  if (!fn || _controlFn) return false;
  _controlFn = fn;
  _controlPeriodMs = periodMs ? periodMs : 1;
  return xTaskCreatePinnedToCore(controlTaskEntry, "control", stackSize, this, priority, nullptr, core) == pdPASS;
}

void StampPad::controlTaskEntry(void* arg) {
  StampPad* self = (StampPad*)arg;
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    StampInput in = self->read();
    self->_controlFn(in);
    vTaskDelayUntil(&last, pdMS_TO_TICKS(self->_controlPeriodMs));
  }
}

StampInput StampPad::read() {
  portENTER_CRITICAL(&_inMux);
  StampInput in = _in;
  in.pressed = _pressedAcc;
  in.released = _releasedAcc;
  _pressedAcc = _releasedAcc = 0;
  portEXIT_CRITICAL(&_inMux);
  return in;
}

StampInput StampPad::peek() {
  portENTER_CRITICAL(&_inMux);
  StampInput in = _in;
  in.pressed = _pressedAcc;
  in.released = _releasedAcc;
  portEXIT_CRITICAL(&_inMux);
  return in;
}

// ---------------------------------------------------------------------------
// パッド処理タスク
// ---------------------------------------------------------------------------
void StampPad::padTaskEntry(void* arg) { ((StampPad*)arg)->padTask(); }

void StampPad::padTask() {
  const TickType_t period = pdMS_TO_TICKS(_cfg.padTaskHz ? 1000 / _cfg.padTaskHz : 4) ?: 1;
  TickType_t last = xTaskGetTickCount();
  uint32_t prevButtons = 0;
  bool pairAfterDisconnect = false;

  for (;;) {
    uint32_t now = millis();
    bool justConnected = false, justDisconnected = false;

    xSemaphoreTake(_padMutex, portMAX_DELAY);
    _pad.update();
    bool conn = _pad.isConnected();
    if (conn != _connected) {
      _connected = conn;
      if (conn) {
        justConnected = true;
        _lowBatteryNotified = false;
        applyConnectEffects();
      } else {
        justDisconnected = true;
        _rumbleUntil = 0;
        _lowBattery = false;
      }
    }
    if (_rumbleUntil && (int32_t)(now - _rumbleUntil) >= 0) {
      _pad.setRumble(0, 0);
      _rumbleUntil = 0;
    }

    const GamepadState& s = _pad.state();
    if (conn) updateLowBattery(now, s);

    StampInput in = {};
    in.s = s;
    in.model = _pad.model();
    in.connected = conn;
    in.ready = _pad.isReady();
    // Xbox は変化時のみ送ってくるので途絶判定は接続状態に任せる
    // now は update() より前に取っているので lastReportMs の方が新しいことがある。
    // 符号付きで比べないと桁あふれして「途絶」と誤判定し、一瞬だけ入力が全て OFF になる
    in.fresh = conn && (in.model == GamepadModel::Xbox ||
                        (int32_t)(now - s.lastReportMs) < (int32_t)_cfg.failsafeMs);
    in.tiltPitch = _pad.tiltPitch();
    in.tiltRoll = _pad.tiltRoll();
    in.yaw = _pad.yaw();
    xSemaphoreGive(_padMutex);

    uint32_t cur = conn ? s.buttons : 0;
    portENTER_CRITICAL(&_inMux);
    _in = in;
    _pressedAcc |= cur & ~prevButtons;
    _releasedAcc |= ~cur & prevButtons;
    portEXIT_CRITICAL(&_inMux);
    prevButtons = cur;

    if (in.ready && !_readyLogged) {
      _readyLogged = true;
      ESP_LOGI(TAG, "ready: %s", _pad.modelName());
    }
    if (!conn) _readyLogged = false;
    if (justConnected) {
      ESP_LOGI(TAG, "connected: %s", _pad.remoteAddress().c_str());
      if (_onConnect) _onConnect();
    }
    if (justDisconnected) {
      ESP_LOGI(TAG, "disconnected");
      if (_onDisconnect) _onDisconnect();
      if (_pairPending) {
        _pairPending = false;
        pairAfterDisconnect = true;
      }
    }
    if (pairAfterDisconnect) {
      pairAfterDisconnect = false;
      startPairing();
    }

    handleButton(now);
    updateLed(now);
    vTaskDelayUntil(&last, period);
  }
}

// 電池残量低下の判定と表示 (_padMutex 取得済みで呼ばれる)
void StampPad::updateLowBattery(uint32_t now, const GamepadState& s) {
  if (!_pad.isReady() || s.battery == 0) return;
  bool low = _lowBattery;
  if (!low && s.battery <= _cfg.lowBatteryPercent && !s.charging) low = true;
  if (low && (s.battery >= _cfg.lowBatteryRecover || s.charging)) low = false;

  if (low != _lowBattery) {
    _lowBattery = low;
    if (low) {
      ESP_LOGI(TAG, "low battery %u%%", s.battery);
      if (_cfg.lowBatteryRumble && !_lowBatteryNotified) {
        _lowBatteryNotified = true;
        _pad.setRumble(200, 200);
        _rumbleUntil = now + 600;
      }
      _lowBlinkMs = 0;
    } else {  // 元の表示に戻す
      _pad.setLightbar(_userLightbar[0], _userLightbar[1], _userLightbar[2]);
      _pad.setPlayerNumber(_userPlayer);
    }
  }
  if (!_lowBattery || !_cfg.lowBatteryIndicator) return;

  // 0.5秒ごとに点灯/消灯を切り替える (出力レポートは切り替え時だけ送られる)
  if (_lowBlinkMs && now - _lowBlinkMs < 500) return;
  _lowBlinkMs = now;
  _lowBlinkOn = !_lowBlinkOn;
  GamepadModel m = _pad.model();
  if (m == GamepadModel::DualShock4 || m == GamepadModel::DualSense) {
    if (_lowBlinkOn) _pad.setLightbar(PAD_LOW_BATTERY[0], PAD_LOW_BATTERY[1], PAD_LOW_BATTERY[2]);
    else _pad.setLightbar(0, 0, 0);
  } else if (m == GamepadModel::SwitchPro || m == GamepadModel::JoyConL || m == GamepadModel::JoyConR) {
    _pad.setPlayerNumber(_lowBlinkOn ? _userPlayer : 0);
  }
}

void StampPad::applyConnectEffects() {
  // _padMutex 取得済みで呼ばれる
  _pad.setPlayerNumber(_userPlayer);
  _pad.setLightbar(_userLightbar[0], _userLightbar[1], _userLightbar[2]);
  if (_cfg.connectRumble) {
    _pad.setRumble(160, 160);
    _rumbleUntil = millis() + _cfg.connectRumbleMs;
  }
}

// ---------------------------------------------------------------------------
// 本体ボタン
//   短押し      : ペアリング開始 / 中止
//   3秒長押し   : 接続中の機体に固定 / 固定解除   (LED 紫で予告)
//   8秒長押し   : 登録をすべて消去               (LED 白で予告)
// ---------------------------------------------------------------------------
void StampPad::handleButton(uint32_t now) {
  if (_cfg.buttonPin < 0) return;
  bool raw = digitalRead(_cfg.buttonPin) == LOW;
  if (raw != _btnRaw) {
    _btnRaw = raw;
    _btnDebounceMs = now;
  }
  if (now - _btnDebounceMs < 30 || raw == _btnDown) {
    // 押しっぱなし中の段階表示
    if (_btnDown) {
      uint32_t held = now - _btnDownMs;
      if (held >= _cfg.clearHoldMs) _btnStage = 2;
      else if (held >= _cfg.lockHoldMs) _btnStage = 1;
    }
    return;
  }

  _btnDown = raw;
  if (raw) {
    _btnDownMs = now;
    _btnStage = 0;
    return;
  }

  // 離した
  uint8_t stage = _btnStage;
  _btnStage = 0;
  if (stage == 2) {
    ESP_LOGI(TAG, "button: clear pairings");
    disconnect();
    unlock();
    clearPairings();
    flashLed(255, 255, 255, 5);
  } else if (stage == 1) {
    if (isLocked()) {
      ESP_LOGI(TAG, "button: unlock");
      unlock();
      flashLed(180, 0, 255, 1);
    } else if (_connected) {
      ESP_LOGI(TAG, "button: lock to current");
      lockToCurrent();
      flashLed(180, 0, 255, 2);
    } else {
      flashLed(255, 0, 0, 3);  // 未接続なので固定できない
    }
  } else {
    if (isPairing()) {
      ESP_LOGI(TAG, "button: stop pairing");
      stopPairing();
    } else {
      ESP_LOGI(TAG, "button: start pairing");
      startPairing();
    }
  }
}

// ---------------------------------------------------------------------------
// 本体LED
// ---------------------------------------------------------------------------
void StampPad::writeLed(uint8_t r, uint8_t g, uint8_t b) {
  if (_cfg.ledPin < 0) return;
  uint16_t k = _cfg.ledBrightness;
  uint8_t c[3] = {(uint8_t)(r * k / 255), (uint8_t)(g * k / 255), (uint8_t)(b * k / 255)};
  if (memcmp(c, _ledLast, 3) == 0) return;  // 変化がなければ送らない
  memcpy(_ledLast, c, 3);
  neopixelWrite(_cfg.ledPin, c[0], c[1], c[2]);
}

void StampPad::flashLed(uint8_t r, uint8_t g, uint8_t b, uint8_t times) {
  _flashColor[0] = r;
  _flashColor[1] = g;
  _flashColor[2] = b;
  _flashTimes = times;
  _flashStart = millis();
  _flashUntil = _flashStart + times * 300;
}

static void scaled(uint8_t out[3], const uint8_t c[3], uint16_t k) {
  for (int i = 0; i < 3; i++) out[i] = (uint8_t)(c[i] * k / 255);
}

void StampPad::updateLed(uint32_t now) {
  if (_cfg.ledPin < 0) return;
  if (_ledOverride) {
    writeLed(_ledOverrideColor[0], _ledOverrideColor[1], _ledOverrideColor[2]);
    return;
  }
  uint8_t c[3] = {0, 0, 0};

  if (_flashUntil && (int32_t)(now - _flashUntil) < 0) {
    if (((now - _flashStart) % 300) < 150) memcpy(c, _flashColor, 3);
  } else if (_btnDown && _btnStage == 2) {
    memcpy(c, COL_CLEAR_HOLD, 3);
  } else if (_btnDown && _btnStage == 1) {
    memcpy(c, COL_LOCK_HOLD, 3);
  } else {
    _flashUntil = 0;
    gp::LinkState ls = gp::linkState();
    StampInput in;
    portENTER_CRITICAL(&_inMux);
    in = _in;
    portEXIT_CRITICAL(&_inMux);

    if (in.connected) {
      if (!in.ready) memcpy(c, COL_NOT_READY, 3);
      else if (!in.fresh) {
        if ((now / 100) % 2) memcpy(c, COL_LOST, 3);
      } else {
        if (_lowBattery) {
          // 電池残量低下: 暗い橙で点滅
          if ((now / 500) % 2) scaled(c, COL_LOW_BATTERY, 60);
        } else {
          memcpy(c, isLocked() ? COL_LOCKED : COL_CONNECTED, 3);
        }
      }
    } else if (ls == gp::LinkState::Scanning) {
      if ((now / 125) % 2) memcpy(c, COL_PAIRING, 3);
    } else if (ls == gp::LinkState::Connecting) {
      memcpy(c, COL_CONNECTING, 3);
    } else {
      // 待機中: ゆっくり明滅 (2秒周期)
      uint32_t t = now % 2000;
      uint16_t k = t < 1000 ? t * 255 / 1000 : (2000 - t) * 255 / 1000;
      scaled(c, COL_IDLE, 20 + k * 200 / 255);
    }
  }
  writeLed(c[0], c[1], c[2]);
}

void StampPad::setStatusLed(uint8_t r, uint8_t g, uint8_t b) {
  _ledOverrideColor[0] = r;
  _ledOverrideColor[1] = g;
  _ledOverrideColor[2] = b;
  _ledOverride = true;
}

void StampPad::releaseStatusLed() { _ledOverride = false; }

// ---------------------------------------------------------------------------
// 出力・接続管理 (どのタスクからでも可)
// ---------------------------------------------------------------------------
#define WITH_PAD(stmt)                          \
  do {                                          \
    xSemaphoreTake(_padMutex, portMAX_DELAY);   \
    stmt;                                       \
    xSemaphoreGive(_padMutex);                  \
  } while (0)

void StampPad::setRumble(uint8_t large, uint8_t small) {
  WITH_PAD({
    _rumbleUntil = 0;
    _pad.setRumble(large, small);
  });
}

void StampPad::rumblePulse(uint8_t strength, uint16_t ms) {
  WITH_PAD({
    _pad.setRumble(strength, strength);
    _rumbleUntil = millis() + ms;
  });
}

void StampPad::setTriggerRumble(uint8_t left, uint8_t right) { WITH_PAD(_pad.setTriggerRumble(left, right)); }
void StampPad::setLightbar(uint8_t r, uint8_t g, uint8_t b) {
  WITH_PAD({
    _userLightbar[0] = r;
    _userLightbar[1] = g;
    _userLightbar[2] = b;
    if (!_lowBattery || !_cfg.lowBatteryIndicator) _pad.setLightbar(r, g, b);  // 低下表示中は回復後に反映
  });
}

void StampPad::setPlayerNumber(uint8_t n) {
  WITH_PAD({
    _userPlayer = n;
    if (!_lowBattery || !_cfg.lowBatteryIndicator) _pad.setPlayerNumber(n);
  });
}
void StampPad::setTriggers(const PSTriggerEffect& l, const PSTriggerEffect& r) { WITH_PAD(_pad.setTriggers(l, r)); }
void StampPad::setMicLed(PSMicLed mode) { WITH_PAD(_pad.setMicLed(mode)); }

void StampPad::startPairing() {
  if (_connected) {
    // 接続中なら切断してから探索する (切断完了は padTask で検出)
    _pairPending = true;
    disconnect();
    return;
  }
  WITH_PAD(_pad.startPairing(_cfg.pairingTimeoutMs));
}

void StampPad::stopPairing() { WITH_PAD(_pad.stopPairing()); }
void StampPad::disconnect() { WITH_PAD(_pad.disconnect()); }
void StampPad::clearPairings() { WITH_PAD(_pad.clearPairings()); }
void StampPad::lockToCurrent() { WITH_PAD(_pad.lockController()); }
void StampPad::unlock() { WITH_PAD(_pad.unlockController()); }
bool StampPad::isPairing() { return gp::linkState() == gp::LinkState::Scanning; }
bool StampPad::isLocked() { return _pad.isLocked(); }
