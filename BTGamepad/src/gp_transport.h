// gp_transport.h - Bluetooth トランスポート層 (内部用)
//
//  Classic: HID Control(PSM 0x11) / Interrupt(PSM 0x13) を Bluedroid L2CAP で直接扱う
//           → DualShock 4 / DualSense / Switch Pro / Joy-Con
//  BLE    : HID over GATT のレポート特性を購読する
//           → Xbox Wireless Controller (ファームウェア 5.x 以降)
//
// コールバックはすべて Bluetooth スタックのタスク (コア0) から呼ばれる。
// 重い処理・ログ出力・ブロッキングは禁止。
#pragma once

#include <stdint.h>
#include <stdbool.h>

namespace gp {

enum class Transport : uint8_t { None, Classic, Ble };
enum class Channel : uint8_t { Control = 0, Interrupt = 1 };

enum class LinkState : uint8_t {
  Idle,        // 未接続 (再接続待ち)
  Scanning,    // ペアリング相手を探索中
  Connecting,  // 接続処理中
  Connected,
};

struct TransportCallbacks {
  // Classic: ch = Control/Interrupt, data は HIDP ヘッダ(0xA1 等)付き
  void (*classicData)(Channel ch, const uint8_t* data, uint16_t len);
  // BLE: 入力レポート通知 (レポートIDなし)。battery = true ならバッテリー特性
  void (*bleData)(const uint8_t* data, uint16_t len, bool battery);
  // 接続/切断
  void (*link)(Transport t, bool connected);
  // Classic: リモートデバイス名が判明した
  void (*name)(const char* name);
};

// ---- 共通 ----
bool transportInit(const TransportCallbacks& cb, bool classic, bool ble, const char* deviceName);
LinkState linkState();
Transport activeTransport();
const uint8_t* remoteAddress();
bool startScan(uint32_t timeoutMs);  // 有効なトランスポートすべてで探索
void stopScan();
void disconnect();
void clearBonds();
// 接続を許可する相手を1台に限定する (nullptr で解除)。ペアリング探索で見つけた相手は対象外
void setLock(const uint8_t* addr);

// ---- Classic ----
bool classicConnect(const uint8_t addr[6]);
bool classicSend(Channel ch, const uint8_t* data, uint16_t len);

// ---- BLE ----
bool bleWriteOutput(const uint8_t* data, uint16_t len);

// 内部 (gp_classic.cpp / gp_ble.cpp 間)
namespace internal {
extern TransportCallbacks cb;
extern volatile Transport active;
extern volatile LinkState state;
extern uint8_t remote[6];
extern bool classicEnabled, bleEnabled;
extern volatile bool lockActive;
extern uint8_t lockAddr[6];
inline bool addrAllowed(const uint8_t* a) {
  if (!lockActive) return true;
  for (int i = 0; i < 6; i++)
    if (a[i] != lockAddr[i]) return false;
  return true;
}
bool classicInit(const char* deviceName);
bool classicStartScan(uint32_t timeoutMs);
void classicStopScan();
void classicDisconnect();
void classicClearBonds();
bool bleInit();
bool bleStartScan(uint32_t timeoutMs);
void bleStopScan();
void bleDisconnect();
void bleClearBonds();
void bleResumeBackground();
}  // namespace internal

}  // namespace gp
