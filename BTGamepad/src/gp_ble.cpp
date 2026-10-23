// gp_ble.cpp - BLE HID (HID over GATT) トランスポート - Xbox Wireless Controller 用
//
// 流れ: スキャン → 接続 → 暗号化(ペアリング/ボンド) → サービス探索
//       → HID Report 特性(0x2A4D)の通知を購読 → 入力レポートを受信
#include "gp_transport.h"

#include <Arduino.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_log.h"

namespace gp {
using namespace internal;

static const char* TAG = "GP_BLE";

static constexpr uint16_t UUID_HID_SERVICE = 0x1812;
static constexpr uint16_t UUID_BATTERY_SERVICE = 0x180F;
static constexpr uint16_t UUID_HID_REPORT = 0x2A4D;
static constexpr uint16_t UUID_BATTERY_LEVEL = 0x2A19;
static constexpr uint16_t UUID_CCCD = 0x2902;
static constexpr uint16_t APPEARANCE_GAMEPAD = 0x03C4;
static constexpr int MAX_BONDS = 8;
static constexpr int MAX_CHARS = 20;

enum class ScanMode : uint8_t { Off, Background, Pairing };

static esp_gatt_if_t s_if = ESP_GATT_IF_NONE;
static volatile ScanMode s_scanMode = ScanMode::Off;
static volatile bool s_scanRunning = false;
static uint32_t s_pairDeadline = 0;

static bool s_connecting = false;
static bool s_up = false;
static uint16_t s_connId = 0;
static esp_bd_addr_t s_peer = {};
static esp_ble_addr_type_t s_peerType = BLE_ADDR_TYPE_PUBLIC;

static uint16_t s_hidStart = 0, s_hidEnd = 0, s_batStart = 0, s_batEnd = 0;
static uint16_t s_outHandle = 0;
static bool s_outNoRsp = false;
static uint16_t s_batHandle = 0;

static esp_bd_addr_t s_bonds[MAX_BONDS];
static int s_bondCount = 0;

static void refreshBondList() {
  int n = esp_ble_get_bond_device_num();
  if (n > MAX_BONDS) n = MAX_BONDS;
  if (n <= 0) {
    s_bondCount = 0;
    return;
  }
  esp_ble_bond_dev_t list[MAX_BONDS];
  esp_ble_get_bond_device_list(&n, list);
  for (int i = 0; i < n; i++) memcpy(s_bonds[i], list[i].bd_addr, 6);
  s_bondCount = n;
}

static bool isBonded(const uint8_t* addr) {
  for (int i = 0; i < s_bondCount; i++)
    if (memcmp(s_bonds[i], addr, 6) == 0) return true;
  return false;
}

// ---------------------------------------------------------------------------
// スキャン制御
// ---------------------------------------------------------------------------
static void setScanMode(ScanMode mode) {
  s_scanMode = mode;
  if (s_scanRunning) {
    esp_ble_gap_stop_scanning();
    s_scanRunning = false;
  }
  if (mode == ScanMode::Off) return;

  // 待機中は無線の占有率を下げる (Classic の再接続待ちと共存させるため)
  esp_ble_scan_params_t p = {};
  p.scan_type = BLE_SCAN_TYPE_ACTIVE;
  p.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
  p.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
  p.scan_interval = (mode == ScanMode::Pairing) ? 0x50 : 0xA0;  // 50ms / 100ms
  p.scan_window = (mode == ScanMode::Pairing) ? 0x30 : 0x18;    // 30ms / 15ms
  p.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
  esp_ble_gap_set_scan_params(&p);  // 完了イベントでスキャン開始
}

// ペアリング探索中に見つけた機器をログに出す (同じ相手は1回だけ)
static void logScanResult(esp_ble_gap_cb_param_t* p, const char* name, uint16_t appearance) {
  static esp_bd_addr_t seen[16];
  static uint8_t seenCount = 0, seenNext = 0;
  for (int i = 0; i < seenCount; i++)
    if (memcmp(seen[i], p->scan_rst.bda, 6) == 0) return;
  memcpy(seen[seenNext], p->scan_rst.bda, 6);
  seenNext = (seenNext + 1) % 16;
  if (seenCount < 16) seenCount++;
  if (!name[0] && appearance == 0) return;  // 名前も外観もない機器は省略
  const uint8_t* a = p->scan_rst.bda;
  ESP_LOGI(TAG, "found %02x:%02x:%02x:%02x:%02x:%02x appearance=0x%04x \"%s\"", a[0], a[1], a[2], a[3], a[4], a[5],
           appearance, name);
}

static bool matchPairingTarget(esp_ble_gap_cb_param_t* p) {
  uint8_t len = 0;
  char name[32] = {};
  uint16_t appearance = 0;
  uint8_t* d = esp_ble_resolve_adv_data(p->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &len);
  if (!d) d = esp_ble_resolve_adv_data(p->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_SHORT, &len);
  if (d) memcpy(name, d, len < 31 ? len : 31);
  uint8_t alen = 0;
  uint8_t* ap = esp_ble_resolve_adv_data(p->scan_rst.ble_adv, ESP_BLE_AD_TYPE_APPEARANCE, &alen);
  if (ap && alen == 2) appearance = ap[0] | (ap[1] << 8);
  logScanResult(p, name, appearance);

  if (strstr(name, "Xbox")) return true;
  d = esp_ble_resolve_adv_data(p->scan_rst.ble_adv, ESP_BLE_AD_TYPE_APPEARANCE, &len);
  if (d && len == 2 && (d[0] | (d[1] << 8)) == APPEARANCE_GAMEPAD) {
    // 外観が Gamepad かつ HID サービスを広告している
    uint8_t* u = esp_ble_resolve_adv_data(p->scan_rst.ble_adv, ESP_BLE_AD_TYPE_16SRV_CMPL, &len);
    if (!u) u = esp_ble_resolve_adv_data(p->scan_rst.ble_adv, ESP_BLE_AD_TYPE_16SRV_PART, &len);
    for (int i = 0; u && i + 1 < len; i += 2)
      if ((u[i] | (u[i + 1] << 8)) == UUID_HID_SERVICE) return true;
  }
  return false;
}

static void openPeer(const esp_bd_addr_t addr, esp_ble_addr_type_t type) {
  if (s_if == ESP_GATT_IF_NONE || active != Transport::None || state == LinkState::Connecting) return;
  setScanMode(ScanMode::Off);
  classicStopScan();
  memcpy(s_peer, addr, 6);
  s_peerType = type;
  s_connecting = true;
  state = LinkState::Connecting;
  ESP_LOGI(TAG, "connecting to %02x:%02x:%02x:%02x:%02x:%02x", addr[0], addr[1], addr[2], addr[3], addr[4],
           addr[5]);
  esp_ble_gattc_open(s_if, s_peer, s_peerType, true);
}

static void connectFailed() {
  s_connecting = false;
  if (active == Transport::None) state = LinkState::Idle;
  bleResumeBackground();
}

// ---------------------------------------------------------------------------
// GAP コールバック (BTC タスク)
// ---------------------------------------------------------------------------
static void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      if (s_scanMode != ScanMode::Off) {
        uint32_t dur = 0;  // 0 = 無期限
        if (s_scanMode == ScanMode::Pairing) {
          int32_t remain = (int32_t)(s_pairDeadline - millis());
          dur = remain > 1000 ? remain / 1000 : 1;
        }
        if (esp_ble_gap_start_scanning(dur) == ESP_OK) s_scanRunning = true;
      }
      break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
      if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
        if (s_scanMode == ScanMode::Pairing && matchPairingTarget(param)) {
          openPeer(param->scan_rst.bda, param->scan_rst.ble_addr_type);
        } else if (s_scanMode == ScanMode::Background && isBonded(param->scan_rst.bda) &&
                   addrAllowed(param->scan_rst.bda)) {
          openPeer(param->scan_rst.bda, param->scan_rst.ble_addr_type);
        }
      } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
        s_scanRunning = false;
        if (s_scanMode == ScanMode::Pairing) {  // ペアリング探索タイムアウト
          if (state == LinkState::Scanning) state = LinkState::Idle;
          bleResumeBackground();
        }
      }
      break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;

    case ESP_GAP_BLE_NC_REQ_EVT:
      esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
      break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      if (!s_connecting) break;
      if (param->ble_security.auth_cmpl.success) {
        ESP_LOGI(TAG, "encrypted, discovering services");
        refreshBondList();
        s_hidStart = s_hidEnd = s_batStart = s_batEnd = 0;
        esp_ble_gattc_search_service(s_if, s_connId, nullptr);
      } else {
        ESP_LOGW(TAG, "BLE pairing failed (reason 0x%x)", param->ble_security.auth_cmpl.fail_reason);
        // 相手側で鍵が消えている可能性があるのでボンドを消してやり直せるようにする
        esp_ble_remove_bond_device(s_peer);
        esp_ble_gap_disconnect(s_peer);
      }
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// GATTC コールバック (BTC タスク)
// ---------------------------------------------------------------------------
static void subscribe(uint16_t handle) { esp_ble_gattc_register_for_notify(s_if, s_peer, handle); }

static void enumerateCharacteristics() {
  esp_gattc_char_elem_t chars[MAX_CHARS];
  int inputs = 0;
  s_outHandle = 0;
  s_batHandle = 0;

  if (s_hidStart) {
    uint16_t count = MAX_CHARS;
    if (esp_ble_gattc_get_all_char(s_if, s_connId, s_hidStart, s_hidEnd, chars, &count, 0) == ESP_GATT_OK) {
      for (int i = 0; i < count; i++) {
        if (chars[i].uuid.len != ESP_UUID_LEN_16 || chars[i].uuid.uuid.uuid16 != UUID_HID_REPORT) continue;
        if (chars[i].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) {
          subscribe(chars[i].char_handle);
          inputs++;
        } else if (!s_outHandle &&
                   (chars[i].properties & (ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR))) {
          // Xbox の出力レポートは振動(ID 3)のみ
          s_outHandle = chars[i].char_handle;
          s_outNoRsp = (chars[i].properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) != 0;
        }
      }
    }
  }
  if (s_batStart) {
    uint16_t count = MAX_CHARS;
    if (esp_ble_gattc_get_all_char(s_if, s_connId, s_batStart, s_batEnd, chars, &count, 0) == ESP_GATT_OK) {
      for (int i = 0; i < count; i++) {
        if (chars[i].uuid.len == ESP_UUID_LEN_16 && chars[i].uuid.uuid.uuid16 == UUID_BATTERY_LEVEL &&
            (chars[i].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)) {
          s_batHandle = chars[i].char_handle;
          subscribe(s_batHandle);
        }
      }
    }
  }
  if (inputs == 0) {
    ESP_LOGW(TAG, "no HID input report found");
    esp_ble_gap_disconnect(s_peer);
  }
}

static void bleLinkUp() {
  if (s_up) return;
  s_up = true;
  s_connecting = false;
  memcpy(remote, s_peer, 6);
  active = Transport::Ble;
  state = LinkState::Connected;
  // 接続間隔を短くして遅延を減らす (7.5〜15ms)
  esp_ble_conn_update_params_t cp = {};
  memcpy(cp.bda, s_peer, 6);
  cp.min_int = 6;
  cp.max_int = 12;
  cp.latency = 0;
  cp.timeout = 400;
  esp_ble_gap_update_conn_params(&cp);
  ESP_LOGI(TAG, "HID link up");
  if (cb.link) cb.link(Transport::Ble, true);
}

static void gattcCallback(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t* param) {
  switch (event) {
    case ESP_GATTC_REG_EVT:
      if (param->reg.status == ESP_GATT_OK) s_if = gattc_if;
      break;

    case ESP_GATTC_OPEN_EVT:
      if (param->open.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "open failed status=%d", param->open.status);
        connectFailed();
        break;
      }
      s_connId = param->open.conn_id;
      esp_ble_gattc_send_mtu_req(s_if, s_connId);
      // ボンド済みなら保存済みの鍵で、未ペアなら新規ペアリングで暗号化する
      esp_ble_set_encryption(s_peer, ESP_BLE_SEC_ENCRYPT);
      break;

    case ESP_GATTC_SEARCH_RES_EVT: {
      const esp_bt_uuid_t& u = param->search_res.srvc_id.uuid;
      if (u.len != ESP_UUID_LEN_16) break;
      if (u.uuid.uuid16 == UUID_HID_SERVICE && !s_hidStart) {
        s_hidStart = param->search_res.start_handle;
        s_hidEnd = param->search_res.end_handle;
      } else if (u.uuid.uuid16 == UUID_BATTERY_SERVICE) {
        s_batStart = param->search_res.start_handle;
        s_batEnd = param->search_res.end_handle;
      }
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT:
      enumerateCharacteristics();
      break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.status != ESP_GATT_OK) break;
      esp_bt_uuid_t cccdUuid = {};
      cccdUuid.len = ESP_UUID_LEN_16;
      cccdUuid.uuid.uuid16 = UUID_CCCD;
      esp_gattc_descr_elem_t descr;
      uint16_t count = 1;
      if (esp_ble_gattc_get_descr_by_char_handle(s_if, s_connId, param->reg_for_notify.handle, cccdUuid, &descr,
                                                 &count) == ESP_GATT_OK &&
          count > 0) {
        uint8_t enable[2] = {0x01, 0x00};
        esp_ble_gattc_write_char_descr(s_if, s_connId, descr.handle, sizeof(enable), enable, ESP_GATT_WRITE_TYPE_RSP,
                                       ESP_GATT_AUTH_REQ_NONE);
      }
      break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT:
      if (param->write.status == ESP_GATT_OK && s_connecting) bleLinkUp();
      break;

    case ESP_GATTC_NOTIFY_EVT:
      if (s_up && cb.bleData) {
        cb.bleData(param->notify.value, param->notify.value_len, param->notify.handle == s_batHandle);
      }
      break;

    case ESP_GATTC_DISCONNECT_EVT: {
      bool wasUp = s_up;
      s_up = false;
      s_connecting = false;
      s_outHandle = 0;
      if (active == Transport::Ble) active = Transport::None;
      if (active == Transport::None) state = LinkState::Idle;
      ESP_LOGI(TAG, "disconnected reason=0x%x", param->disconnect.reason);
      if (wasUp && cb.link) cb.link(Transport::Ble, false);
      bleResumeBackground();
      break;
    }

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
namespace internal {

bool bleInit() {
  esp_ble_gap_register_callback(gapCallback);
  esp_ble_gattc_register_callback(gattcCallback);
  esp_ble_gattc_app_register(0x47);

  esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_BOND;
  esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
  uint8_t keySize = 16;
  uint8_t initKey = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rspKey = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &keySize, sizeof(keySize));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &initKey, sizeof(initKey));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rspKey, sizeof(rspKey));

  refreshBondList();
  ESP_LOGI(TAG, "saved pairings (BLE): %d", s_bondCount);
  for (int i = 0; i < s_bondCount; i++)
    ESP_LOGI(TAG, "  %02x:%02x:%02x:%02x:%02x:%02x", s_bonds[i][0], s_bonds[i][1], s_bonds[i][2], s_bonds[i][3],
             s_bonds[i][4], s_bonds[i][5]);
  bleResumeBackground();
  return true;
}

// 待機中: ボンド済みの Xbox コントローラーがあれば低デューティでスキャンして再接続を待つ
void bleResumeBackground() {
  if (!bleEnabled || active != Transport::None || s_connecting) return;
  if (s_scanMode == ScanMode::Pairing && state == LinkState::Scanning) return;
  refreshBondList();
  setScanMode(s_bondCount > 0 ? ScanMode::Background : ScanMode::Off);
}

bool bleStartScan(uint32_t timeoutMs) {
  if (!bleEnabled) return false;
  s_pairDeadline = millis() + timeoutMs;
  setScanMode(ScanMode::Pairing);
  return true;
}

void bleStopScan() {
  if (!bleEnabled || s_scanMode == ScanMode::Off) return;
  setScanMode(ScanMode::Off);
}

void bleDisconnect() {
  if (s_up || s_connecting) esp_ble_gap_disconnect(s_peer);
}

void bleClearBonds() {
  refreshBondList();
  for (int i = 0; i < s_bondCount; i++) esp_ble_remove_bond_device(s_bonds[i]);
  s_bondCount = 0;
  if (s_scanMode == ScanMode::Background) setScanMode(ScanMode::Off);
}

}  // namespace internal

bool bleWriteOutput(const uint8_t* data, uint16_t len) {
  if (!s_up || !s_outHandle) return false;
  return esp_ble_gattc_write_char(s_if, s_connId, s_outHandle, len, (uint8_t*)data,
                                  s_outNoRsp ? ESP_GATT_WRITE_TYPE_NO_RSP : ESP_GATT_WRITE_TYPE_RSP,
                                  ESP_GATT_AUTH_REQ_NONE) == ESP_OK;
}

}  // namespace gp
