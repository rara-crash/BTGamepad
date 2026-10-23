// BTGamepad.h - ESP32 で Bluetooth ゲームパッドを直接扱うライブラリ
//
// 対応機種
//   Bluetooth Classic : DualShock 4 / DualSense (Edge) / Switch Pro コントローラー / Joy-Con (L/R 単体)
//   BLE               : Xbox Wireless Controller (Series X|S, ファームウェア更新済みの One S)
//
// スレッド構成 (軽量化のポイント)
//   - Bluetooth スタックと受信レポートの解析はコア0で動く (解析は1レポート数μs)
//   - update() は共有領域をコピーするだけ (ロックは数十ns)。コア1の loop() や任意のタスクから呼べる
//   - update() を呼ぶタスクは1つにすること
#pragma once

#include <Arduino.h>
#include "gp_transport.h"

// ---------------------------------------------------------------------------
// ボタン (ビットマスク)。名前は「位置」で統一し、各機種の名前は別名で用意
// ---------------------------------------------------------------------------
namespace GPButton {
enum : uint32_t {
  Up = 1u << 0,
  Right = 1u << 1,
  Down = 1u << 2,
  Left = 1u << 3,
  South = 1u << 4,  // 下のボタン
  East = 1u << 5,   // 右のボタン
  West = 1u << 6,   // 左のボタン
  North = 1u << 7,  // 上のボタン
  L1 = 1u << 8,
  R1 = 1u << 9,
  L2 = 1u << 10,  // デジタル判定
  R2 = 1u << 11,
  Select = 1u << 12,  // Share/Create / View / -
  Start = 1u << 13,   // Options / Menu / +
  L3 = 1u << 14,
  R3 = 1u << 15,
  Home = 1u << 16,      // PS / Xbox / HOME
  Touchpad = 1u << 17,  // PS: タッチパッド押し込み
  Mute = 1u << 18,      // DualSense
  Capture = 1u << 19,   // Switch キャプチャー / Xbox Share
  Paddle1 = 1u << 20,   // DualSense Edge 背面左
  Paddle2 = 1u << 21,   // DualSense Edge 背面右
  FnL = 1u << 22,       // DualSense Edge Fn左
  FnR = 1u << 23,       // DualSense Edge Fn右
  SL = 1u << 24,        // Joy-Con 側面
  SR = 1u << 25,

  // PlayStation
  Cross = South, Circle = East, Square = West, Triangle = North,
  Share = Select, Create = Select, Options = Start, PSHome = Home,
  // Xbox
  XboxA = South, XboxB = East, XboxX = West, XboxY = North,
  LB = L1, RB = R1, LT = L2, RT = R2, View = Select, Menu = Start,
  // Switch (A が右、B が下)
  SwitchA = East, SwitchB = South, SwitchX = North, SwitchY = West,
  ZL = L2, ZR = R2, Minus = Select, Plus = Start,
};
}  // namespace GPButton
namespace PSButton = GPButton;

enum class GamepadModel : uint8_t { Unknown, DualShock4, DualSense, SwitchPro, JoyConL, JoyConR, Xbox };

struct GPVec3 {
  float x, y, z;
};

struct GPTouchPoint {
  bool active;
  uint8_t id;  // 指を置くたびに増えるID
  uint16_t x;  // 0 .. touchpadWidth()-1
  uint16_t y;  // 0 .. touchpadHeight()-1
};

// update() 時点の入力スナップショット
struct GamepadState {
  uint32_t buttons;
  int8_t lx, ly, rx, ry;  // -128..127 (ly/ry は上が +)
  uint8_t l2, r2;         // 0..255 (Switch はデジタルなので 0 か 255)
  int16_t gyroRaw[3];
  int16_t accelRaw[3];
  // 軸は全機種で統一: x=右, y=上, z=手前 (PlayStation と同じ)
  GPVec3 gyro;   // deg/s  (x=ピッチ, y=ヨー, z=ロール)
  GPVec3 accel;  // G
  GPTouchPoint touch[2];
  uint8_t battery;  // 0..100 %
  bool charging;
  uint8_t triggerStatusL, triggerStatusR;  // DualSense アダプティブトリガー状態(生値)
  bool ready;            // 全機能が有効 (初期化完了)
  uint32_t reportCount;  // 受信レポート数
  uint32_t lastReportMs;
};

// ---------------------------------------------------------------------------
// DualSense アダプティブトリガー効果
//   位置(zone)はトリガーの押し込み量を 0(離した状態)〜9(奥) の10段階で表す
//   強さは 1〜8 (0 を指定すると Off)
// ---------------------------------------------------------------------------
struct PSTriggerEffect {
  uint8_t data[11];

  static PSTriggerEffect off();
  static PSTriggerEffect feedback(uint8_t position, uint8_t strength);
  static PSTriggerEffect weapon(uint8_t start, uint8_t end, uint8_t strength);  // start 2..7, end start+1..8
  static PSTriggerEffect vibration(uint8_t position, uint8_t amplitude, uint8_t frequency);
  static PSTriggerEffect bow(uint8_t start, uint8_t end, uint8_t strength, uint8_t snapForce);
  static PSTriggerEffect galloping(uint8_t start, uint8_t end, uint8_t firstFoot, uint8_t secondFoot,
                                   uint8_t frequency);
  static PSTriggerEffect machine(uint8_t start, uint8_t end, uint8_t amplitudeA, uint8_t amplitudeB,
                                 uint8_t frequency, uint8_t period);
  static PSTriggerEffect multiFeedback(const uint8_t strength[10]);
  static PSTriggerEffect slopeFeedback(uint8_t start, uint8_t end, uint8_t startStrength, uint8_t endStrength);
  static PSTriggerEffect multiVibration(uint8_t frequency, const uint8_t amplitude[10]);
  static PSTriggerEffect raw(const uint8_t bytes[11]);

  bool operator==(const PSTriggerEffect& o) const { return memcmp(data, o.data, sizeof(data)) == 0; }
  bool operator!=(const PSTriggerEffect& o) const { return !(*this == o); }
};

enum class PSMicLed : uint8_t { Off = 0, On = 1, Pulse = 2 };

struct GamepadConfig {
  bool classic = true;  // PS / Switch
  bool ble = true;      // Xbox (不要なら false で無線の占有とメモリを節約)
  const char* deviceName = "ESP32-Gamepad";
};

// ---------------------------------------------------------------------------
// 本体
// ---------------------------------------------------------------------------
class Gamepad {
 public:
  typedef void (*ConnectionCallback)(Gamepad& pad);

  Gamepad();

  bool begin(const GamepadConfig& config = GamepadConfig());
  // ESP32 の BT アドレスを指定して開始 (SixaxisPairTool で DS4 に書き込んだアドレスを使う場合)
  bool begin(const char* localMac, const GamepadConfig& config = GamepadConfig());

  // ペアリングモードのコントローラーを探して接続する
  //   PS5 : Create + PS 長押し      PS4 : Share + PS 長押し
  //   Switch: シンクロボタン長押し   Xbox: ペアボタン(上部)長押し
  bool startPairing(uint32_t timeoutMs = 30000);
  void stopPairing();
  bool isPairing() const { return gp::linkState() == gp::LinkState::Scanning; }
  bool connect(const char* classicMac);  // Classic 機を MAC 指定で接続 "xx:xx:xx:xx:xx:xx"
  void disconnect();
  void clearPairings();  // ESP32 に保存されたペアリング情報をすべて消す

  // ---- 接続相手の固定 (設定はフラッシュに保存され、再起動後も有効) ----
  // 固定中は指定した1台だけが接続できる。解除中は先に接続した1台が優先 (後から来た機体は断る)
  // 固定中に startPairing() で新しい機体をペアリングすると、固定先はその機体に切り替わる
  bool lockController(const char* mac = nullptr);  // nullptr = 今つながっている機体に固定
  void unlockController();
  bool isLocked() const { return _locked; }
  String lockedAddress() const;

  // 入力スナップショット更新・出力送信・コールバック呼び出し。ループ毎に呼ぶ
  void update();

  bool isConnected() const { return _connected; }
  bool isReady() const { return _cur.ready; }
  GamepadModel model() const { return _model; }
  const char* modelName() const;
  String localAddress() const;
  String remoteAddress() const;

  void onConnect(ConnectionCallback cb) { _onConnect = cb; }
  void onDisconnect(ConnectionCallback cb) { _onDisconnect = cb; }

  // 機能の有無
  bool hasMotion() const;
  bool hasTouchpad() const { return isPlayStation(); }
  bool hasAdaptiveTriggers() const { return _model == GamepadModel::DualSense; }
  bool hasTriggerRumble() const { return _model == GamepadModel::Xbox; }
  bool hasLightbar() const { return isPlayStation(); }

  // ---- 入力 (update() 時点の値) ----
  const GamepadState& state() const { return _cur; }
  bool button(uint32_t mask) const { return (_cur.buttons & mask) != 0; }
  bool pressed(uint32_t mask) const { return (_cur.buttons & ~_prev.buttons & mask) != 0; }
  bool released(uint32_t mask) const { return (~_cur.buttons & _prev.buttons & mask) != 0; }
  uint32_t buttons() const { return _cur.buttons; }

  int8_t lx() const { return _cur.lx; }
  int8_t ly() const { return _cur.ly; }
  int8_t rx() const { return _cur.rx; }
  int8_t ry() const { return _cur.ry; }
  float stickLX(float deadzone = 0.08f) const { return normAxis(_cur.lx, deadzone); }
  float stickLY(float deadzone = 0.08f) const { return normAxis(_cur.ly, deadzone); }
  float stickRX(float deadzone = 0.08f) const { return normAxis(_cur.rx, deadzone); }
  float stickRY(float deadzone = 0.08f) const { return normAxis(_cur.ry, deadzone); }
  uint8_t l2() const { return _cur.l2; }
  uint8_t r2() const { return _cur.r2; }

  const GPVec3& gyro() const { return _cur.gyro; }    // deg/s
  const GPVec3& accel() const { return _cur.accel; }  // G
  float tiltPitch() const { return _tiltPitch; }      // 加速度から求めた傾き [deg]
  float tiltRoll() const { return _tiltRoll; }
  float yaw() const { return _yaw; }  // ジャイロ(ヨー)積分の旋回角 [deg]
  void resetYaw() { _yaw = 0; }
  void calibrateGyro(uint32_t durationMs = 1000);  // 静止状態でゼロ点補正
  bool isCalibratingGyro() const { return _gyroCalibUntil != 0; }

  const GPTouchPoint& touch(uint8_t i) const { return _cur.touch[i & 1]; }
  uint16_t touchpadWidth() const { return 1920; }
  uint16_t touchpadHeight() const { return _model == GamepadModel::DualSense ? 1080 : 942; }

  uint8_t battery() const { return _cur.battery; }
  bool charging() const { return _cur.charging; }

  // ---- 出力 (値を設定 → 次の update() で変化があれば送信) ----
  void setRumble(uint8_t large, uint8_t small);  // large: 低周波(左) / small: 高周波(右)
  void setTriggerRumble(uint8_t left, uint8_t right);  // Xbox インパルストリガー
  void setLightbar(uint8_t r, uint8_t g, uint8_t b);   // PS
  void setLightbarFlash(uint8_t onTime, uint8_t offTime);  // DS4 (10ms単位)
  void setPlayerLeds(uint8_t mask);  // DualSense: 5bit / Switch: 4bit
  void setPlayerNumber(uint8_t n);   // 1〜5 (Switch は 1〜4)
  void setMicLed(PSMicLed mode);     // DualSense
  void setTriggerL(const PSTriggerEffect& e);  // DualSense
  void setTriggerR(const PSTriggerEffect& e);
  void setTriggers(const PSTriggerEffect& l, const PSTriggerEffect& r) {
    setTriggerL(l);
    setTriggerR(r);
  }

  // デバッグ用: 最後に受信した生レポート
  const uint8_t* rawReport(uint16_t* len) const {
    *len = _rawLen;
    return _raw;
  }

 private:
  struct Output {
    uint8_t large, small;
    uint8_t trigRumbleL, trigRumbleR;
    uint8_t r, g, b;
    uint8_t flashOn, flashOff;
    uint8_t playerLeds;
    uint8_t playerNum;  // 0 なら playerLeds をそのまま使う
    PSMicLed mic;
    PSTriggerEffect trigL, trigR;
  };
  struct Calib {
    float bias;
    float scale;  // (raw - bias) * scale
  };
  struct StickCal {
    int16_t center, plus, minus;  // Switch 12bit スティック
  };

  // コールバック (コア0)
  static void cbClassicData(gp::Channel ch, const uint8_t* d, uint16_t len);
  static void cbBleData(const uint8_t* d, uint16_t len, bool battery);
  static void cbLink(gp::Transport t, bool connected);
  static void cbName(const char* name);

  bool isPlayStation() const { return _model == GamepadModel::DualShock4 || _model == GamepadModel::DualSense; }
  bool isSwitch() const {
    return _model == GamepadModel::SwitchPro || _model == GamepadModel::JoyConL || _model == GamepadModel::JoyConR;
  }

  // 共通
  void publish(GamepadState& s);
  void applyMotion(GamepadState& s, const float g[3], const float a[3]);
  static float normAxis(int8_t v, float dz);
  static uint32_t hatToButtons(uint8_t hat);

  // PlayStation (drv_playstation.cpp)
  void psConnected();
  void psInput(const uint8_t* r, uint16_t len);
  void psFeature(const uint8_t* r, uint16_t len);
  void psParseButtons(const uint8_t* b, GamepadState& s);
  void psParseDS4(const uint8_t* x);
  void psParseDS5(const uint8_t* x);
  void psParseBasic(const uint8_t* r);
  void psRequestCalibration();
  void psUpdate(uint32_t now);
  void psSendOutput();

  // Switch (drv_switch.cpp)
  void swStart();
  void swInput(const uint8_t* r, uint16_t len);
  void swSendSubcmd(uint8_t id, const uint8_t* args, uint8_t len);
  void swNextStep();
  void swParseFull(const uint8_t* r);
  void swUpdate(uint32_t now);
  void swEncodeRumble(uint8_t* out8) const;

  // Xbox (drv_xbox.cpp)
  void xbInput(const uint8_t* d, uint16_t len);
  void xbUpdate(uint32_t now);

  static Gamepad* _instance;

  // ---- コア0 (BT タスク) が書く領域 ----
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  GamepadState _shared = {};
  GamepadState _work = {};  // 解析用ワーク
  volatile bool _linkUp = false;
  volatile GamepadModel _model = GamepadModel::Unknown;
  Calib _gyroCal[3] = {};
  Calib _accCal[3] = {};
  StickCal _stickCal[4] = {};  // Switch: LX LY RX RY
  uint8_t _raw[64] = {};
  uint16_t _rawLen = 0;
  float _tiltPitch = 0, _tiltRoll = 0, _yaw = 0;
  float _gyroOffset[3] = {0, 0, 0};
  uint32_t _lastMotionUs = 0;
  volatile uint32_t _gyroCalibUntil = 0;
  float _gyroCalibSum[3] = {0, 0, 0};
  uint32_t _gyroCalibCount = 0;

  // Switch 初期化シーケンス
  volatile uint8_t _swStep = 0;
  volatile uint8_t _swRetries = 0;
  volatile uint32_t _swStepMs = 0;
  uint8_t _swCounter = 0;
  bool _swPendingLeds = false;

  // ---- update() 側 ----
  GamepadState _cur = {};
  GamepadState _prev = {};
  bool _connected = false;
  bool _locked = false;
  uint8_t _lockAddr[6] = {};
  bool _pairingRequested = false;
  void saveLock();
  ConnectionCallback _onConnect = nullptr;
  ConnectionCallback _onDisconnect = nullptr;

  Output _out = {};
  Output _sentOut = {};
  bool _outDirty = false;
  bool _lightbarSetup = false;
  uint8_t _outSeq = 0;
  uint32_t _lastSendMs = 0;
  uint32_t _lastCalibReqMs = 0;
  uint8_t _calibRetries = 0;
};

using PSGamepad = Gamepad;
