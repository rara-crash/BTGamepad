# StampPad

M5Stamp Pico 専用のゲームパッドライブラリです。[BTGamepad](../BTGamepad) の上に次の機能を追加しています。

- 本体ボタン（G39）：短押し = ペアリング開始/中止、3秒 = 接続相手の固定/解除、8秒 = ペアリング情報の全消去
- 本体LED（G27）：赤明滅 = 待機、青点滅 = ペアリング中、水色 = 接続処理中、黄 = 初期化中、緑 = 接続中、青緑 = 固定中、橙高速点滅 = 通信途絶、暗い橙点滅 = 電池残量低下
- 接続時：振動、ライトバー色、プレイヤー番号を設定
- 電池残量が15%以下：コントローラー側（PS はライトバー、Switch はプレイヤーランプ）でも暗い橙/点滅で知らせ、25%以上か充電で元に戻す
- マルチコア：パッド処理タスク（コア1・250Hz）と、ユーザーの制御ループ（固定周期）を分離
- フェイルセーフ：`StampInput::active()` が false なら操縦しない

```cpp
#include <StampPad.h>
StampPad stamp;

void control(const StampInput& in) {      // 50Hz の専用タスクで呼ばれる
  if (!in.active()) { /* 脱力 */ return; }
  float v = in.stickLY();
  if (in.wasPressed(GPButton::Cross)) stamp.rumblePulse();
}

void setup() {
  StampPadConfig cfg;
  stamp.begin(cfg);
  stamp.startControlLoop(control, 20);
}
void loop() {}
```

詳しい例は `examples/Robot/main.cpp` を参照してください。

使い方（platformio.ini の設定など）はリポジトリ直下の README を参照してください。
