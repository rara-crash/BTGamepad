# BTGamepad

ESP32 (M5Stamp Pico など) で Bluetooth ゲームパッドを直接扱うライブラリです。
Bluepad32 では使えなかったジャイロ・タッチパッド・アダプティブトリガーなども扱えます。

## 対応機種と機能

| 機能 | DS4 | DualSense | Switch Pro | Joy-Con | Xbox |
|---|:-:|:-:|:-:|:-:|:-:|
| 接続方式 | Classic | Classic | Classic | Classic | **BLE** |
| ボタン / スティック | ○ | ○ | ○ | ○ (片側) | ○ |
| アナログトリガー | ○ | ○ | デジタル | デジタル | ○ |
| ジャイロ・加速度 | ○ | ○ | ○ | ○ | - |
| タッチパッド (2点) | ○ | ○ | - | - | - |
| 振動 | ○ | ○ | ○ (HD振動) | ○ | ○ |
| トリガー | - | **アダプティブ** | - | - | **インパルス(振動)** |
| LED | ライトバー | ライトバー/プレイヤー/マイク | プレイヤー | プレイヤー | - |
| バッテリー | ○ | ○ | ○ (25%刻み) | ○ | ○ |

Xbox は現行ファームウェア (Series X|S、更新済みの One S) の BLE 接続に対応しています。

## 軽さとコア構成

```
コア0: Bluetooth スタック + 受信レポートの解析 (1レポート数μs, ログ出力なし)
コア1: loop() / ユーザーのタスク
        └ pad.update() は共有領域をコピーするだけ (ロックは数十ns)
```

- ライブラリ内部で `delay` やブロッキング処理は使っていません。
- 出力（振動・LED）は「値が変わったときだけ」送信し、送信間隔も制限しています。
- `update()` は1つのタスクからだけ呼んでください（loop() でも自作タスクでもOK）。
- Xbox を使わない場合は `cfg.ble = false` にすると BLE の待ち受けスキャンが止まり、無線と CPU の負担が減ります。

**サーボのシリアル送信は `loop()` から分けて専用タスクにする**のがおすすめです（`examples/Basic/main.cpp` の `servoTask` 参照）。
`delay()` / `vTaskDelay()` は待っている間 CPU を他のタスクに譲るので、送信タスク内で待っても `loop()` は止まりません。
`flush()` は送信完了まで待つので、ICS サーボのように「書いて少し待つ」用途では `write()` + `vTaskDelay(1)` で十分です。

## ペアリング

1. 書き込んでシリアルモニタ (115200) を開く
2. コントローラーをペアリングモードにする

   | 機種 | 操作 |
   |---|---|
   | PS5 | Create + PS 長押し（ライトバー高速点滅） |
   | PS4 | Share + PS 長押し（ライトバー高速点滅） |
   | Switch Pro / Joy-Con | シンクロボタン（上部の小さいボタン）長押し |
   | Xbox | ペアボタン（上部 USB 端子横）長押し（Xbox ボタン高速点滅） |

3. シリアルモニタで `p`（コードでは `pad.startPairing()`）
4. 一度ペアリングすれば、以降は **ホーム/PS/Xbox ボタンを押すだけで自動接続** します

うまくいかない時は `c`（`pad.clearPairings()`）で ESP32 側の情報を消してやり直してください。
DS4 は従来どおり SixaxisPairTool で書き込んだアドレスでも使えます: `pad.begin("01:02:03:04:05:06")`

## API 早見表

```cpp
#include <BTGamepad.h>
Gamepad pad;

void setup() { pad.begin(); }
void loop() {
  pad.update();
  if (!pad.isConnected()) return;

  pad.model();                           // GamepadModel::DualSense など
  pad.isReady();                         // 初期化完了 (ジャイロ等が有効)

  // ボタン: 位置の名前で統一。各機種の名前でも書ける
  pad.button(GPButton::South);           // 押している間 (= Cross = XboxA = SwitchB)
  pad.pressed(GPButton::Cross);          // 押した瞬間
  pad.released(GPButton::XboxB);         // 離した瞬間
  pad.stickLX(); pad.stickLY();          // -1.0〜1.0 (デッドゾーン込み, 上が+)
  pad.l2(); pad.r2();                    // 0〜255

  pad.gyro(); pad.accel();               // deg/s, G (軸は全機種で x=右 y=上 z=手前)
  pad.tiltPitch(); pad.tiltRoll();       // 傾き [deg]
  pad.yaw(); pad.resetYaw();             // 旋回角 [deg]
  pad.calibrateGyro(1000);               // 静止状態でゼロ点補正
  pad.touch(0).active; pad.touch(0).x;   // タッチパッド
  pad.battery(); pad.charging();

  pad.setRumble(large, small);           // 全機種
  pad.setTriggerRumble(l, r);            // Xbox
  pad.setLightbar(r, g, b);              // PS
  pad.setPlayerNumber(1);                // DualSense / Switch
  pad.setTriggerR(PSTriggerEffect::weapon(2, 6, 8));  // DualSense
}
```

機能の有無は `hasMotion()` / `hasTouchpad()` / `hasAdaptiveTriggers()` / `hasTriggerRumble()` / `hasLightbar()` で確認できます。

### アダプティブトリガー効果 (DualSense)

位置は 0(離した状態)〜9(奥まで)、強さは 1〜8。

| 関数 | 内容 |
|---|---|
| `off()` | 抵抗なし |
| `feedback(pos, strength)` | pos より奥で一定の抵抗 |
| `slopeFeedback(start, end, s0, s1)` | 抵抗がだんだん変化 |
| `multiFeedback(strength[10])` | ゾーン毎に抵抗を指定 |
| `weapon(start, end, strength)` | 銃のトリガー（途中で重くなり、カチッと抜ける） |
| `bow(start, end, strength, snap)` | 弓 |
| `vibration(pos, amp, freq)` | pos より奥で振動 |
| `multiVibration(freq, amp[10])` | ゾーン毎に振幅を指定 |
| `galloping(...)` / `machine(...)` | 馬の足音風 / マシンガン風 |
| `raw(bytes[11])` | 11バイトを直接指定 |

## 構成

```
BTGamepad/src/
├─ BTGamepad.h/.cpp     公開API・共通処理 (スナップショット・姿勢推定・セッター)
├─ drv_playstation.cpp  DS4 / DualSense
├─ drv_switch.cpp       Switch Pro / Joy-Con
├─ drv_xbox.cpp         Xbox
├─ trigger_effect.cpp   アダプティブトリガー効果
├─ gp_transport.*       Bluetooth 起動・探索の振り分け
├─ gp_classic.cpp       Classic: L2CAP (HID Control/Interrupt) 直叩き
├─ gp_ble.cpp           BLE: HID over GATT
└─ stack/               Bluedroid 内部ヘッダ (Apache 2.0)
```

Bluedroid の内部 L2CAP API を使うため **Arduino-ESP32 2.x (platform espressif32@6.5.0)** が必要です。
Classic + BLE を両方リンクするので `huge_app.csv`（OTA なし・アプリ3MB）パーティションを使っています。

## 注意

- 同時接続は 1 台です。
- 接続直後は初期化中のため、ジャイロ等は `isReady()` が true になってから有効です。
- Switch の IMU は軸を PlayStation と同じ向きに並べ替えていますが、実機での向き確認がまだです。符号が逆なら教えてください。
- Joy-Con 単体は縦持ちの配置のまま返します（横持ちの読み替えはしていません）。
