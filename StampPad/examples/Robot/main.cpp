// StampPad ロボット操縦サンプル (M5Stamp Pico)
//
// 本体ボタン(G39)
//   短押し      : ペアリング開始 / 中止 (コントローラー側もペアリングモードに)
//   3秒長押し   : 今つながっている機体に固定 / 固定解除  (LED 紫)
//   8秒長押し   : 登録をすべて消去                       (LED 白)
// 本体LED
//   赤ゆっくり明滅=待機  青点滅=ペアリング中  水色=接続処理中  黄=初期化中
//   緑=接続中  青緑=接続中(固定)  橙高速点滅=通信途絶  暗く点滅=電池残量少
//
// タスク構成
//   コア0: Bluetooth (ライブラリ内部)
//   コア1: StampPad パッド処理 250Hz / control() 50Hz (サーボ送信) / loop() 表示
#include <Arduino.h>
#include <StampPad.h>

StampPad stamp;

#define PORT_A Serial2  // TX=22
#define PORT_B Serial1  // TX=25
static constexpr int SERVO_COUNT = 4;

// ICS サーボ。flush() で待たず送信FIFOに積むだけ
static void icsSetPos(HardwareSerial& port, uint8_t id, int angle) {
  int pos = map(constrain(angle, -135, 135), -135, 135, 3500, 11500);
  uint8_t cmd[3] = {(uint8_t)(0x80 | id), (uint8_t)((pos >> 7) & 0x7F), (uint8_t)(pos & 0x7F)};
  port.write(cmd, 3);
}

static void icsSetFree(HardwareSerial& port, uint8_t id) {
  uint8_t cmd[3] = {(uint8_t)(0x80 | id), 0, 0};
  port.write(cmd, 3);
}

static bool gyroSteer = false;

// 50Hz で呼ばれる制御ループ (専用タスク)。ここで delay しても他の処理は止まらない
static void control(const StampInput& in) {
  // ---- ボタンイベント (前回からの押下を取りこぼさない) ----
  if (in.wasPressed(GPButton::L1) && (in.model != GamepadModel::Xbox)) {
    gyroSteer = !gyroSteer;
    stamp.setPlayerNumber(gyroSteer ? 2 : 1);
  }
  if (in.wasPressed(GPButton::South)) stamp.rumblePulse(200, 100);  // × / A / B

  // ---- 操縦量 ----
  float forward = in.stickLY();
  float turn = in.stickLX();
  if (gyroSteer) {
    forward = constrain(-in.tiltPitch / 30.0f, -1.0f, 1.0f);
    turn = constrain(in.tiltRoll / 30.0f, -1.0f, 1.0f);
  }
  float throttle = in.trigger(true) - in.trigger(false);
  if (fabsf(throttle) > 0.05f) forward = throttle;
  float left = constrain(forward + turn, -1.0f, 1.0f);
  float right = constrain(forward - turn, -1.0f, 1.0f);

  // ---- サーボ送信 (通信が途切れたら脱力) ----
  bool enable = in.active();
  for (int id = 1; id <= SERVO_COUNT; id++) {
    HardwareSerial& port = (id <= SERVO_COUNT / 2) ? PORT_A : PORT_B;
    if (enable) icsSetPos(port, id, (int)(((id <= SERVO_COUNT / 2) ? left : right) * 90));
    else icsSetFree(port, id);
    vTaskDelay(pdMS_TO_TICKS(1));  // サーボ応答とぶつからないように (CPU は他に譲る)
  }
  while (PORT_A.available()) PORT_A.read();
  while (PORT_B.available()) PORT_B.read();
}

static void onConnect() { Serial.println("[PAD] connected"); }
static void onDisconnect() { Serial.println("[PAD] disconnected"); }

void setup() {
  Serial.begin(115200);
  PORT_A.begin(115200, SERIAL_8E1, -1, 22);
  PORT_B.begin(115200, SERIAL_8E1, -1, 25);

  StampPadConfig cfg;
  cfg.lightbar[0] = 0;  // PS ライトバー: 青
  cfg.lightbar[1] = 0;
  cfg.lightbar[2] = 64;
  cfg.playerNumber = 1;
  cfg.lowBatteryRumble = true;
  // cfg.gamepad.ble = false;  // Xbox を使わないなら無効にすると軽くなる

  stamp.onConnect(onConnect);
  stamp.onDisconnect(onDisconnect);
  if (!stamp.begin(cfg)) {
    Serial.println("StampPad init failed");
    while (true) delay(1000);
  }
  stamp.startControlLoop(control, 20);  // 50Hz
  Serial.println("ready. 本体ボタン短押しでペアリング");
}

void loop() {
  // シリアルからも操作できる (p: ペアリング / d: 切断 / l: 固定 / u: 固定解除 / c: 登録消去)
  if (Serial.available()) {
    switch (Serial.read()) {
      case 'p': stamp.startPairing(); break;
      case 'd': stamp.disconnect(); break;
      case 'l': stamp.lockToCurrent(); break;
      case 'u': stamp.unlock(); break;
      case 'c': stamp.clearPairings(); break;
    }
  }

  // 表示 (peek は入力イベントを消費しない)
  static uint32_t last = 0;
  if (millis() - last > 300) {
    last = millis();
    StampInput in = stamp.peek();
    if (in.connected) {
      Serial.printf("%-11s active=%d bat=%3u%% btn=%07lx L(%4d,%4d) R(%4d,%4d) L2=%3u R2=%3u tilt(%6.1f,%6.1f)\n",
                    stamp.gamepad().modelName(), in.active(), in.s.battery, (unsigned long)in.s.buttons, in.s.lx,
                    in.s.ly, in.s.rx, in.s.ry, in.s.l2, in.s.r2, in.tiltPitch, in.tiltRoll);
    }
  }
  delay(10);
}
