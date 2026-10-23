// StampPad.h - M5Stamp Pico 専用ゲームパッドライブラリ (BTGamepad の上に構築)
//
// BTGamepad に以下を追加する:
//   - 本体ボタン(G39)でペアリング / 接続先の固定 / 登録消去
//   - 本体LED(G27, SK6812)で状態表示
//   - 接続時の振動・ライトバー色・プレイヤー番号
//   - マルチコア: パッド処理タスク(コア1) と 制御ループタスク(固定周期) を分離
//   - フェイルセーフ: 一定時間レポートが来なければ「通信断」と判定
//
// コア構成
//   コア0 : Bluetooth スタック + レポート解析 (BTGamepad 内部)
//   コア1 : padTask     … update()・ボタン・LED を一定周期で処理 (既定 250Hz)
//           controlTask … ユーザーの制御関数を固定周期で呼ぶ (サーボ送信などはここ)
//           loop()      … 空けておける (Serial 表示などに使う)
#pragma once

#include <Arduino.h>
#include <BTGamepad.h>

// 制御ループに渡される入力 (スナップショット)
struct StampInput {
  GamepadState s;
  GamepadModel model;
  bool connected;  // 接続中
  bool ready;      // 初期化完了 (ジャイロ等も有効)
  bool fresh;      // 直近 failsafeMs 以内にレポートを受信している
  uint32_t pressed;   // 前回 read() 以降に押されたボタン (取りこぼさない)
  uint32_t released;  // 前回 read() 以降に離されたボタン
  float tiltPitch, tiltRoll, yaw;

  // 操縦してよい状態か (接続中・初期化済み・通信が新しい)
  bool active() const { return connected && ready && fresh; }
  bool button(uint32_t mask) const { return (s.buttons & mask) != 0; }
  bool wasPressed(uint32_t mask) const { return (pressed & mask) != 0; }
  bool wasReleased(uint32_t mask) const { return (released & mask) != 0; }
  float stickLX(float dz = 0.08f) const { return norm(s.lx, dz); }
  float stickLY(float dz = 0.08f) const { return norm(s.ly, dz); }
  float stickRX(float dz = 0.08f) const { return norm(s.rx, dz); }
  float stickRY(float dz = 0.08f) const { return norm(s.ry, dz); }
  float trigger(bool right) const { return (right ? s.r2 : s.l2) / 255.0f; }

  static float norm(int8_t v, float dz) {
    float f = v / 127.0f;
    if (f > 1.0f) f = 1.0f;
    float a = fabsf(f);
    if (a < dz) return 0.0f;
    return copysignf((a - dz) / (1.0f - dz), f);
  }
};

struct StampPadConfig {
  GamepadConfig gamepad;  // classic / ble / deviceName

  // ハードウェア (M5Stamp Pico の標準配置)
  int8_t ledPin = 27;     // 本体 RGB LED。-1 で使わない
  int8_t buttonPin = 39;  // 本体ボタン (押すと LOW)。-1 で使わない
  uint8_t ledBrightness = 40;  // 0..255

  // ボタン操作
  uint32_t pairingTimeoutMs = 30000;  // 短押し: ペアリング開始 (もう一度押すと中止)
  uint32_t lockHoldMs = 3000;         // 3秒長押し: 今の機体に固定 / 固定解除
  uint32_t clearHoldMs = 8000;        // 8秒長押し: 登録をすべて消去

  // 接続時の演出
  bool connectRumble = true;
  uint16_t connectRumbleMs = 250;
  uint8_t lightbar[3] = {0, 0, 64};  // PS ライトバー色
  uint8_t playerNumber = 1;          // プレイヤーLED (DualSense / Switch)
  bool lowBatteryRumble = false;     // 残量低下時に一度振動で知らせる
  // 電池残量低下の表示: 本体LED とコントローラー(PS:ライトバー / Switch:プレイヤーランプ)を暗い橙で点滅
  bool lowBatteryIndicator = true;
  uint8_t lowBatteryPercent = 15;    // これ以下で表示開始
  uint8_t lowBatteryRecover = 25;    // これ以上 (または充電開始) で表示終了

  // タスク
  uint16_t padTaskHz = 250;
  uint8_t padTaskCore = 1;
  uint8_t padTaskPriority = 3;
  uint32_t failsafeMs = 300;  // これ以上レポートが途切れたら fresh=false
};

class StampPad {
 public:
  typedef void (*ControlFn)(const StampInput& in);
  typedef void (*EventFn)();

  bool begin(const StampPadConfig& config = StampPadConfig());

  // 制御ループを固定周期で実行するタスクを起動する (例: サーボ送信 50Hz → periodMs=20)
  bool startControlLoop(ControlFn fn, uint32_t periodMs, uint8_t core = 1, uint8_t priority = 2,
                        uint32_t stackSize = 4096);

  // 最新の入力を取得 (どのタスクからでも可)。pressed/released はここで消費される
  //   制御ループを使う場合は、そちらに渡される入力を使うこと (二重に消費しないため)
  StampInput read();
  // 最新の入力を見るだけ (pressed/released を消費しない)。表示・デバッグ用
  StampInput peek();

  // ---- 出力 (どのタスクからでも可) ----
  void setRumble(uint8_t large, uint8_t small);
  void rumblePulse(uint8_t strength = 160, uint16_t ms = 150);  // 指定時間だけ振動
  void setTriggerRumble(uint8_t left, uint8_t right);           // Xbox
  void setLightbar(uint8_t r, uint8_t g, uint8_t b);            // PS
  void setPlayerNumber(uint8_t n);
  void setTriggers(const PSTriggerEffect& l, const PSTriggerEffect& r);  // DualSense
  void setMicLed(PSMicLed mode);

  // ---- 本体LED ----
  // 指定色で上書き (状態表示を止める)。releaseStatusLed() で自動表示に戻す
  void setStatusLed(uint8_t r, uint8_t g, uint8_t b);
  void releaseStatusLed();

  // ---- 接続管理 ----
  void startPairing();
  void stopPairing();
  void disconnect();
  void clearPairings();
  void lockToCurrent();
  void unlock();
  bool isConnected() const { return _connected; }
  bool isPairing();
  bool isLocked();
  bool isLowBattery() const { return _lowBattery; }

  // イベント (padTask から呼ばれる。重い処理はしないこと)
  void onConnect(EventFn fn) { _onConnect = fn; }
  void onDisconnect(EventFn fn) { _onDisconnect = fn; }  // ロボット停止処理はここにも書く

  // 上級者向け: BTGamepad 本体。触るときは lockPad()/unlockPad() で囲むこと
  Gamepad& gamepad() { return _pad; }
  void lockPad() { xSemaphoreTake(_padMutex, portMAX_DELAY); }
  void unlockPad() { xSemaphoreGive(_padMutex); }

 private:
  static void padTaskEntry(void* arg);
  static void controlTaskEntry(void* arg);
  void padTask();
  void handleButton(uint32_t now);
  void updateLed(uint32_t now);
  void writeLed(uint8_t r, uint8_t g, uint8_t b);
  void flashLed(uint8_t r, uint8_t g, uint8_t b, uint8_t times);
  void applyConnectEffects();

  Gamepad _pad;
  StampPadConfig _cfg;
  SemaphoreHandle_t _padMutex = nullptr;
  portMUX_TYPE _inMux = portMUX_INITIALIZER_UNLOCKED;

  StampInput _in = {};
  uint32_t _pressedAcc = 0, _releasedAcc = 0;
  volatile bool _connected = false;
  volatile bool _pairPending = false;
  bool _readyLogged = false;
  bool _lowBattery = false;
  bool _lowBlinkOn = false;
  uint32_t _lowBlinkMs = 0;
  uint8_t _userLightbar[3] = {};
  uint8_t _userPlayer = 1;
  void updateLowBattery(uint32_t now, const GamepadState& s);

  // ボタン
  bool _btnDown = false;
  uint32_t _btnDownMs = 0;
  uint8_t _btnStage = 0;  // 長押し段階 (LED 予告用)
  uint32_t _btnDebounceMs = 0;
  bool _btnRaw = false;

  // LED
  volatile bool _ledOverride = false;
  uint8_t _ledOverrideColor[3] = {};
  uint8_t _ledLast[3] = {1, 1, 1};
  uint32_t _flashUntil = 0;
  uint8_t _flashColor[3] = {};
  uint8_t _flashTimes = 0;
  uint32_t _flashStart = 0;

  // 振動パルス
  volatile uint32_t _rumbleUntil = 0;
  bool _lowBatteryNotified = false;

  // 制御ループ
  ControlFn _controlFn = nullptr;
  uint32_t _controlPeriodMs = 20;

  EventFn _onConnect = nullptr;
  EventFn _onDisconnect = nullptr;
};
