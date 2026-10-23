// gp_transport.cpp - トランスポート共通部 (Bluetooth 起動・探索・切断の振り分け)
#include "gp_transport.h"

#include <Arduino.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_log.h"

namespace gp {

namespace internal {
TransportCallbacks cb = {};
volatile Transport active = Transport::None;
volatile LinkState state = LinkState::Idle;
uint8_t remote[6] = {};
bool classicEnabled = false;
bool bleEnabled = false;
volatile bool lockActive = false;
uint8_t lockAddr[6] = {};
}  // namespace internal

using namespace internal;
static const char* TAG = "GP";

bool transportInit(const TransportCallbacks& callbacks, bool classic, bool ble, const char* deviceName) {
  cb = callbacks;
  classicEnabled = classic;
  bleEnabled = ble;

  if (!btStarted() && !btStart()) {
    ESP_LOGE(TAG, "btStart failed");
    return false;
  }
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED && esp_bluedroid_init() != ESP_OK) {
    ESP_LOGE(TAG, "esp_bluedroid_init failed");
    return false;
  }
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED && esp_bluedroid_enable() != ESP_OK) {
    ESP_LOGE(TAG, "esp_bluedroid_enable failed");
    return false;
  }
  if (classic && !classicInit(deviceName)) return false;
  if (ble && !bleInit()) return false;
  return true;
}

LinkState linkState() { return state; }
Transport activeTransport() { return active; }
const uint8_t* remoteAddress() { return remote; }

bool startScan(uint32_t timeoutMs) {
  if (state == LinkState::Connected || state == LinkState::Connecting) return false;
  state = LinkState::Scanning;
  if (classicEnabled) classicStartScan(timeoutMs);
  if (bleEnabled) bleStartScan(timeoutMs);
  return true;
}

void stopScan() {
  if (classicEnabled) classicStopScan();
  if (bleEnabled) bleStopScan();
  if (state == LinkState::Scanning) state = LinkState::Idle;
  if (bleEnabled) bleResumeBackground();
}

void disconnect() {
  stopScan();
  if (active == Transport::Classic) classicDisconnect();
  if (active == Transport::Ble) bleDisconnect();
}

void setLock(const uint8_t* addr) {
  if (addr) {
    memcpy(lockAddr, addr, 6);
    lockActive = true;
  } else {
    lockActive = false;
  }
}

void clearBonds() {
  if (classicEnabled) classicClearBonds();
  if (bleEnabled) bleClearBonds();
}

}  // namespace gp
