# BTGamepad / StampPad

ESP32 で PS4 / PS5 / Switch / Xbox のコントローラーを Bluetooth で直接つなぐ Arduino ライブラリです。
Bluepad32 では扱えなかったジャイロ、PS5 のタッチパッドとアダプティブトリガーなども使えます。
Bluepad32 の改造ではなく、Arduino-ESP32 に標準で入っている Bluedroid を直接使う独自の実装です。

| ライブラリ | 内容 | 対象ボード |
|---|---|---|
| [BTGamepad](BTGamepad/) | コントローラーとの通信・入力・振動/LED などの出力 | Bluetooth Classic を持つ初代 ESP32 系 (DevKit, M5Stack Core, ATOM, M5Stamp Pico など) |
| [StampPad](StampPad/) | BTGamepad の上に、本体ボタンでのペアリング・状態LED・マルチコアの制御ループ・フェイルセーフを追加 | M5Stamp Pico |

## 対応コントローラー

| 機能 | DualShock 4 | DualSense | Switch Pro / Joy-Con | Xbox (BLE) |
|---|:-:|:-:|:-:|:-:|
| ボタン / スティック / トリガー | ○ | ○ | ○ (トリガーはデジタル) | ○ |
| ジャイロ・加速度 | ○ | ○ | ○ | - |
| タッチパッド (2点) | ○ | ○ | - | - |
| 振動 | ○ | ○ | ○ | ○ |
| トリガーの特殊機能 | - | アダプティブトリガー | - | インパルストリガー |
| LED | ライトバー | ライトバー / プレイヤー / マイク | プレイヤーランプ | - |
| バッテリー残量 | ○ | ○ | ○ | ○ |

8BitDo などの他社製コントローラーは Switch モードで使えます。
実機で確認済み: DualSense、Switch モードの他社製コントローラー。DualShock 4 と純正 Xbox は未確認です。

## 必要な環境

- **Arduino-ESP32 2.x**（PlatformIO なら `platform = espressif32@6.5.0`）。Bluedroid の内部 API を使うため 3.x には未対応です
- 初代 ESP32（ESP32-S3 / C3 / C6 には Bluetooth Classic がないため PS・Switch は使えません）
- Classic と BLE を両方使うのでプログラムが大きくなります。`huge_app.csv` パーティションを推奨します

## 使い方 (PlatformIO)

```sh
git clone https://github.com/raracrash/BTGamepad.git C:/pio-libs/BTGamepad
```

```ini
[env:m5stamp-pico]
platform = espressif32@6.5.0
board = m5stamp-pico
framework = arduino
board_build.partitions = huge_app.csv
lib_extra_dirs = C:/pio-libs/BTGamepad   ; clone したフォルダ (中の BTGamepad と StampPad が見つかる)
```

> **注意:** clone 先のパスに日本語などの英数字以外の文字が入っていると、コンパイラがファイルを開けずにビルドが失敗します。英数字だけのパスに clone するか、ジャンクションで英数字のパスを作ってください。
> ```powershell
> New-Item -ItemType Junction -Path C:\pio-libs -Target "<clone したフォルダ>"
> ```

Arduino IDE の場合は、`BTGamepad` と `StampPad` のフォルダを `Arduino/libraries` にコピーしてください（ボードマネージャーで esp32 2.0.x を選ぶ）。

## 最小の例

```cpp
#include <BTGamepad.h>
Gamepad pad;

void setup() { pad.begin(); pad.startPairing(); }
void loop() {
  pad.update();
  if (pad.isConnected() && pad.pressed(GPButton::Cross)) pad.setRumble(200, 200);
}
```

M5Stamp Pico なら StampPad を使うと、本体ボタンでペアリングでき、制御ループも別タスクで回せます。詳しくは各フォルダの README と `examples` を参照してください。

## ライセンス

MIT License（[LICENSE](LICENSE)）。
`BTGamepad/src/stack/` のヘッダーは Bluedroid（Broadcom / Google）由来で、Apache License 2.0 です（[NOTICE](BTGamepad/src/stack/NOTICE)）。

通信内容の解析にあたって、Linux カーネルの `hid-playstation`、dekuNukem 氏の Nintendo Switch Reverse Engineering、Nielk1 氏の DualSense トリガー効果の資料を参考にしました。
