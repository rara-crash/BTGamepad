// BTGamepad デモ - ロボット操縦を想定したサンプル (PS4 / PS5 / Switch / Xbox 共通)
//
// コア構成
//   コア0: Bluetooth スタック + 受信レポートの解析 (ライブラリ内部、何もしなくてよい)
//   コア1: loop()      … pad.update() と操縦量の計算 (毎周期すぐ終わる)
//          servoTask   … サーボへのシリアル送信 (固定周期。delay は他タスクに CPU を譲る)
//   loop() からサーボ送信を切り離すことで、送信待ちでパッド処理が止まらない。
//
// 操作:
//   左スティック / R2・L2      : 前後・旋回
//   ○×△□ (Xbox: B A Y X)     : アダプティブトリガーのモード切替 (DualSense)
//   L1                         : ジャイロ操舵 ON/OFF (傾けて操縦, PS / Switch)
//   R1                         : ジャイロのゼロ点補正 (机に置いて押す)
//   タッチパッド               : 触れている間、左右位置で旋回 (PS)
//   Home + Start               : 切断
// シリアルモニタ: 'p' = ペアリング開始 (接続中なら切断してから) / 'd' = 切断 / 'c' = ペアリング情報を消去
//                'l' = 今の機体に固定 / 'u' = 固定解除 (先着1台)
#include <Arduino.h>
#include <BTGamepad.h>

Gamepad pad;

// ---------------------------------------------------------------------------
// サーボ送信タスク (コア1)
// ---------------------------------------------------------------------------
#define PORT_A Serial2  // TX=22
#define PORT_B Serial1  // TX=25
static constexpr int SERVO_COUNT = 4;
static constexpr uint32_t SERVO_PERIOD_MS = 20;  // 50Hz

struct DriveCommand {
  float left, right;  // -1.0〜1.0
  bool enable;
};
static DriveCommand g_cmd = {0, 0, false};
static portMUX_TYPE g_cmdMux = portMUX_INITIALIZER_UNLOCKED;

static void setCommand(const DriveCommand& c) {
  portENTER_CRITICAL(&g_cmdMux);
  g_cmd = c;
  portEXIT_CRITICAL(&g_cmdMux);
}

static DriveCommand getCommand() {
  portENTER_CRITICAL(&g_cmdMux);
  DriveCommand c = g_cmd;
  portEXIT_CRITICAL(&g_cmdMux);
  return c;
}

// ICS サーボ (YMv1.2 と同じ形式)。flush() で待たず、UART の送信FIFOに積むだけ
static void icsSetPos(HardwareSerial& port, uint8_t id, int angle) {
  int pos = map(constrain(angle, -135, 135), -135, 135, 3500, 11500);
  uint8_t cmd[3] = {(uint8_t)(0x80 | id), (uint8_t)((pos >> 7) & 0x7F), (uint8_t)(pos & 0x7F)};
  port.write(cmd, 3);
}

static void icsSetFree(HardwareSerial& port, uint8_t id) {
  uint8_t cmd[3] = {(uint8_t)(0x80 | id), 0, 0};
  port.write(cmd, 3);
}

static void servoTask(void*) {
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    DriveCommand c = getCommand();
    for (int id = 1; id <= SERVO_COUNT; id++) {
      HardwareSerial& port = (id <= SERVO_COUNT / 2) ? PORT_A : PORT_B;
      if (!c.enable) {
        icsSetFree(port, id);
      } else {
        float v = (id <= SERVO_COUNT / 2) ? c.left : c.right;
        icsSetPos(port, id, (int)(v * 90));
      }
      vTaskDelay(pdMS_TO_TICKS(1));  // サーボの応答とぶつからないよう間隔をあける (CPU は他へ譲る)
    }
    while (PORT_A.available()) PORT_A.read();  // エコー/応答を捨てる
    while (PORT_B.available()) PORT_B.read();
    vTaskDelayUntil(&last, pdMS_TO_TICKS(SERVO_PERIOD_MS));
  }
}

// ---------------------------------------------------------------------------
// パッド
// ---------------------------------------------------------------------------
static bool gyroSteer = false;
static uint8_t triggerMode = 0;
static uint32_t rumbleUntil = 0;
static bool pairAfterDisconnect = false;

static void beginPairing() {
  if (pad.startPairing(30000)) {
    Serial.println("pairing... (30s) コントローラーをペアリングモードにしてください");
  } else {
    Serial.println("pairing failed to start");
  }
}

static void applyTriggerMode() {
  static const uint8_t colors[4][3] = {{0, 0, 64}, {0, 64, 0}, {64, 0, 0}, {64, 0, 64}};
  switch (triggerMode) {
    case 0: pad.setTriggers(PSTriggerEffect::off(), PSTriggerEffect::off()); break;
    case 1: pad.setTriggers(PSTriggerEffect::feedback(2, 4), PSTriggerEffect::slopeFeedback(0, 9, 1, 8)); break;
    case 2: pad.setTriggers(PSTriggerEffect::weapon(2, 6, 8), PSTriggerEffect::weapon(3, 7, 8)); break;
    case 3: pad.setTriggers(PSTriggerEffect::vibration(3, 6, 30), PSTriggerEffect::machine(1, 9, 7, 3, 20, 2)); break;
  }
  pad.setLightbar(colors[triggerMode][0], colors[triggerMode][1], colors[triggerMode][2]);
}

static void onConnect(Gamepad& p) {
  Serial.printf("[PAD] connected %s\n", p.remoteAddress().c_str());
  p.setPlayerNumber(1);
  applyTriggerMode();
  p.setRumble(128, 128);  // 短く振動
  rumbleUntil = millis() + 200;
}

static void onDisconnect(Gamepad&) {
  Serial.println("[PAD] disconnected");
  setCommand({0, 0, false});  // 通信が切れたら必ず止める
  if (pairAfterDisconnect) {
    pairAfterDisconnect = false;
    beginPairing();
  }
}

void setup() {
  Serial.begin(115200);
  PORT_A.begin(115200, SERIAL_8E1, -1, 22);
  PORT_B.begin(115200, SERIAL_8E1, -1, 25);

  pad.onConnect(onConnect);
  pad.onDisconnect(onDisconnect);
  GamepadConfig cfg;
  cfg.ble = true;  // Xbox を使わないなら false にすると軽くなる
  if (!pad.begin(cfg)) {
    Serial.println("Bluetooth init failed");
    while (true) delay(1000);
  }
  Serial.printf("ESP32 BT address: %s\n", pad.localAddress().c_str());
  Serial.println("'p' = pairing / 'd' = disconnect / 'c' = clear pairings / 'l' = lock / 'u' = unlock");
  Serial.printf("lock: %s\n", pad.isLocked() ? pad.lockedAddress().c_str() : "off (first come)");

  xTaskCreatePinnedToCore(servoTask, "servo", 4096, nullptr, 2, nullptr, 1);
}

void loop() {
  pad.update();

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'p') {
      if (pad.isConnected()) {
        Serial.println("disconnecting current controller...");
        pairAfterDisconnect = true;
        pad.disconnect();
      } else {
        beginPairing();
      }
    } else if (c == 'd') {
      Serial.println(pad.isConnected() ? "disconnecting..." : "not connected");
      pad.disconnect();
    } else if (c == 'l') {
      if (pad.lockController()) Serial.printf("locked to %s\n", pad.lockedAddress().c_str());
      else Serial.println("lock failed (not connected)");
    } else if (c == 'u') {
      pad.unlockController();
      Serial.println("unlocked (first come)");
    } else if (c == 'r') {
      const GamepadState& s = pad.state();
      Serial.printf("raw acc %d %d %d gyro %d %d %d\n", s.accelRaw[0], s.accelRaw[1], s.accelRaw[2], s.gyroRaw[0],
                    s.gyroRaw[1], s.gyroRaw[2]);
    } else if (c == 'c') {
      pad.clearPairings();
      Serial.println("pairings cleared");
    }
  }

  if (!pad.isConnected()) {
    delay(5);
    return;
  }

  // ---- ボタンイベント ----
  if (pad.pressed(GPButton::Circle)) triggerMode = 0, applyTriggerMode();
  if (pad.pressed(GPButton::Cross)) triggerMode = 1, applyTriggerMode();
  if (pad.pressed(GPButton::Triangle)) triggerMode = 2, applyTriggerMode();
  if (pad.pressed(GPButton::Square)) triggerMode = 3, applyTriggerMode();
  if (pad.pressed(GPButton::L1) && pad.hasMotion()) {
    gyroSteer = !gyroSteer;
    pad.setPlayerNumber(gyroSteer ? 2 : 1);
  }
  if (pad.pressed(GPButton::R1)) {
    pad.calibrateGyro(1000);
    pad.resetYaw();
  }
  if (pad.button(GPButton::Home) && pad.pressed(GPButton::Start)) pad.disconnect();

  // ---- 操縦量 (-1.0〜1.0) ----
  float forward = pad.stickLY();
  float turn = pad.stickLX();
  if (gyroSteer) {
    forward = constrain(-pad.tiltPitch() / 30.0f, -1.0f, 1.0f);
    turn = constrain(pad.tiltRoll() / 30.0f, -1.0f, 1.0f);
  }
  const GPTouchPoint& t0 = pad.touch(0);
  if (pad.hasTouchpad() && t0.active) {
    turn = constrain((t0.x - pad.touchpadWidth() / 2) / (pad.touchpadWidth() / 2.0f), -1.0f, 1.0f);
  }
  float throttle = (pad.r2() - pad.l2()) / 255.0f;
  if (fabsf(throttle) > 0.05f) forward = throttle;

  float left = constrain(forward + turn, -1.0f, 1.0f);
  float right = constrain(forward - turn, -1.0f, 1.0f);
  setCommand({left, right, pad.isReady()});

  if ((int32_t)(millis() - rumbleUntil) >= 0) {
    pad.setRumble(0, (uint8_t)(fabsf(forward) * 60));
    pad.setTriggerRumble(pad.l2() / 4, pad.r2() / 4);  // Xbox: トリガーが震える
  }

  // ---- 表示 (間引く。Serial 出力は重いので頻度を上げすぎない) ----
  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 200) {
    lastPrint = millis();
    const GamepadState& s = pad.state();
    Serial.printf("%-11s bat=%3u%% btn=%07lx L(%4d,%4d) R(%4d,%4d) L2=%3u R2=%3u", pad.modelName(), s.battery,
                  (unsigned long)s.buttons, s.lx, s.ly, s.rx, s.ry, s.l2, s.r2);
    if (pad.hasMotion())
      Serial.printf(" gyro(%6.1f,%6.1f,%6.1f) acc(%5.2f,%5.2f,%5.2f) yaw=%6.1f", s.gyro.x, s.gyro.y, s.gyro.z,
                    s.accel.x, s.accel.y, s.accel.z, pad.yaw());
    if (t0.active) Serial.printf(" touch(%4u,%4u)", t0.x, t0.y);
    Serial.printf(" drive L=%5.2f R=%5.2f\n", left, right);
  }

  delay(1);  // 1ms 休んで他タスク(servoTask)に譲る
}
