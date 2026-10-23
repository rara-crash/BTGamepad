// gp_classic.cpp - Bluetooth Classic HID トランスポート (Bluedroid L2CAP 直叩き)
#include "gp_transport.h"

#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"

extern "C" {
#include "stack/bt_types.h"
#include "stack/btm_api.h"
#include "stack/l2c_api.h"
#include "stack/l2cdefs.h"
}

namespace gp {
using namespace internal;

static const char* TAG = "GP_CLASSIC";

struct L2capChan {
  uint16_t cid;
  bool cfgLocal;   // こちらの Config Req が承認された
  bool cfgRemote;  // 相手の Config Req に応答した
  bool open() const { return cid != 0 && cfgLocal && cfgRemote; }
  void reset() { cid = 0; cfgLocal = cfgRemote = false; }
};

static L2capChan s_ctrl = {};
static L2capChan s_intr = {};
static bool s_outgoing = false;
static bool s_up = false;
static char s_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {};

static volatile bool s_scanActive = false;
static uint32_t s_scanDeadline = 0;
static volatile bool s_pendingConnect = false;
static esp_bd_addr_t s_pendingAddr = {};
static char s_pendingName[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {};

static tL2CAP_CFG_INFO s_cfgInfo = {};

// ---------------------------------------------------------------------------
// L2CAP コールバック (BTU タスク)
// ---------------------------------------------------------------------------
static void linkUp() {
  s_up = true;
  s_scanActive = false;
  active = Transport::Classic;
  state = LinkState::Connected;
  bleStopScan();
  ESP_LOGI(TAG, "HID link up %02x:%02x:%02x:%02x:%02x:%02x", remote[0], remote[1], remote[2], remote[3],
           remote[4], remote[5]);
  if (cb.link) cb.link(Transport::Classic, true);
  if (s_name[0]) {
    if (cb.name) cb.name(s_name);
  } else {
    esp_bt_gap_read_remote_name(remote);  // 再接続時は名前を問い合わせて機種判定に使う
  }
}

static void checkLinkUp() {
  // 発信時: Control が開いたら Interrupt を張る
  if (s_outgoing && s_ctrl.open() && s_intr.cid == 0) {
    uint16_t cid = L2CA_ConnectReq(BT_PSM_HID_INTERRUPT, remote);
    if (cid == 0) {
      ESP_LOGE(TAG, "interrupt channel connect request failed");
      L2CA_DisconnectReq(s_ctrl.cid);
      return;
    }
    s_intr.cid = cid;
  }
  if (!s_up && s_ctrl.open() && s_intr.open()) linkUp();
}

static void linkDown() {
  bool wasUp = s_up;
  s_up = false;
  s_ctrl.reset();
  s_intr.reset();
  s_outgoing = false;
  s_name[0] = 0;
  if (active == Transport::Classic || active == Transport::None) {
    active = Transport::None;
    state = s_scanActive ? LinkState::Scanning : LinkState::Idle;
  }
  if (wasUp) {
    if (cb.link) cb.link(Transport::Classic, false);
    bleResumeBackground();
  }
}

static void onConnectInd(BD_ADDR bd_addr, uint16_t cid, uint16_t psm, uint8_t id) {
  L2capChan* ch = (psm == BT_PSM_HID_CONTROL) ? &s_ctrl : (psm == BT_PSM_HID_INTERRUPT) ? &s_intr : nullptr;
  // 断る条件:
  //  - 別のコントローラーが接続中 (同時接続は1台)
  //  - ペアリング探索中 (登録済みの別機体が割り込むのを防ぐ)
  //  - 接続相手が固定されていて、その相手ではない
  const char* reason = nullptr;
  if (active == Transport::Ble ||
      (psm == BT_PSM_HID_CONTROL && s_ctrl.cid != 0 && memcmp(bd_addr, remote, 6) != 0))
    reason = "busy";
  else if (psm == BT_PSM_HID_CONTROL && (s_scanActive || s_pendingConnect))
    reason = "pairing in progress";
  else if (!addrAllowed(bd_addr))
    reason = "locked to another controller";
  ESP_LOGI(TAG, "incoming psm=0x%02x from %02x:%02x:%02x:%02x:%02x:%02x%s%s", psm, bd_addr[0], bd_addr[1],
           bd_addr[2], bd_addr[3], bd_addr[4], bd_addr[5], reason ? " -> rejected: " : "", reason ? reason : "");
  if (!ch || reason) {
    L2CA_ErtmConnectRsp(bd_addr, id, cid, L2CAP_CONN_NO_RESOURCES, L2CAP_CONN_NO_RESOURCES, nullptr);
    return;
  }
  if (psm == BT_PSM_HID_CONTROL) {
    memcpy(remote, bd_addr, BD_ADDR_LEN);
    s_outgoing = false;
    s_intr.reset();
  }
  ch->reset();
  ch->cid = cid;
  state = LinkState::Connecting;
  L2CA_ErtmConnectRsp(bd_addr, id, cid, L2CAP_CONN_OK, L2CAP_CONN_OK, nullptr);
  L2CA_ConfigReq(cid, &s_cfgInfo);
}

static void onConnectCfm(uint16_t cid, uint16_t result) {
  if (result == L2CAP_CONN_OK) {
    L2CA_ConfigReq(cid, &s_cfgInfo);
    return;
  }
  ESP_LOGW(TAG, "L2CAP connect failed (result=%u)", result);
  if (cid == s_intr.cid && s_ctrl.cid) L2CA_DisconnectReq(s_ctrl.cid);
  linkDown();
}

static void onConfigInd(uint16_t cid, tL2CAP_CFG_INFO* cfg) {
  cfg->result = L2CAP_CFG_OK;
  L2CA_ConfigRsp(cid, cfg);
  if (cid == s_ctrl.cid) s_ctrl.cfgRemote = true;
  if (cid == s_intr.cid) s_intr.cfgRemote = true;
  checkLinkUp();
}

static void onConfigCfm(uint16_t cid, tL2CAP_CFG_INFO* cfg) {
  if (cfg->result != L2CAP_CFG_OK) return;
  if (cid == s_ctrl.cid) s_ctrl.cfgLocal = true;
  if (cid == s_intr.cid) s_intr.cfgLocal = true;
  checkLinkUp();
}

static void onDisconnectInd(uint16_t cid, bool ackNeeded) {
  if (ackNeeded) L2CA_DisconnectRsp(cid);
  if (cid == s_ctrl.cid && s_intr.cid) L2CA_DisconnectReq(s_intr.cid);
  if (cid == s_intr.cid && s_ctrl.cid) L2CA_DisconnectReq(s_ctrl.cid);
  if (cid == s_ctrl.cid || cid == s_intr.cid) linkDown();
}

static void onDisconnectCfm(uint16_t cid, uint16_t) {
  if (cid == s_ctrl.cid || cid == s_intr.cid) linkDown();
}

static void onDataInd(uint16_t cid, BT_HDR* buf) {
  if (cb.classicData && buf->length > 0 && s_up) {
    const uint8_t* p = buf->data + buf->offset;
    if (cid == s_intr.cid) cb.classicData(Channel::Interrupt, p, buf->length);
    else if (cid == s_ctrl.cid) cb.classicData(Channel::Control, p, buf->length);
  }
  free(buf);
}

static const tL2CAP_APPL_INFO s_l2capInfo = {
  onConnectInd, onConnectCfm, nullptr, onConfigInd, onConfigCfm,
  onDisconnectInd, onDisconnectCfm, nullptr, onDataInd, nullptr, nullptr,
};

// ---------------------------------------------------------------------------
// GAP (探索・ペアリング) コールバック (BTC タスク)
// ---------------------------------------------------------------------------
static bool isSupportedController(const esp_bt_gap_cb_param_t* p, char* nameOut) {
  uint32_t cod = 0;
  char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {};

  for (int i = 0; i < p->disc_res.num_prop; i++) {
    const esp_bt_gap_dev_prop_t& prop = p->disc_res.prop[i];
    if (prop.type == ESP_BT_GAP_DEV_PROP_COD) {
      cod = *(uint32_t*)prop.val;
    } else if (prop.type == ESP_BT_GAP_DEV_PROP_BDNAME) {
      int n = prop.len < ESP_BT_GAP_MAX_BDNAME_LEN ? prop.len : ESP_BT_GAP_MAX_BDNAME_LEN;
      memcpy(name, prop.val, n);
    } else if (prop.type == ESP_BT_GAP_DEV_PROP_EIR && name[0] == 0) {
      uint8_t len = 0;
      uint8_t* n = esp_bt_gap_resolve_eir_data((uint8_t*)prop.val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
      if (!n) n = esp_bt_gap_resolve_eir_data((uint8_t*)prop.val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
      if (n) memcpy(name, n, len > ESP_BT_GAP_MAX_BDNAME_LEN ? ESP_BT_GAP_MAX_BDNAME_LEN : len);
    }
  }

  uint8_t major = (cod >> 8) & 0x1F;  // 0x05 = Peripheral
  uint8_t minor = (cod >> 2) & 0x0F;  // 0x02 = Gamepad, 0x01 = Joystick
  bool gamepadCod = (major == 0x05) && (minor == 0x02 || minor == 0x01);

  ESP_LOGI(TAG, "found %02x:%02x:%02x:%02x:%02x:%02x cod=0x%06x \"%s\"", p->disc_res.bda[0], p->disc_res.bda[1],
           p->disc_res.bda[2], p->disc_res.bda[3], p->disc_res.bda[4], p->disc_res.bda[5], (unsigned)cod, name);

  strcpy(nameOut, name);
  // DS4 "Wireless Controller" / DS5 "DualSense (Edge) Wireless Controller"
  // Switch "Pro Controller" / "Joy-Con (L)" / "Joy-Con (R)"
  if (name[0]) {
    return strstr(name, "Wireless Controller") || strstr(name, "Pro Controller") || strstr(name, "Joy-Con");
  }
  return gamepadCod;
}

static void startDiscovery() { esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 5 /* x1.28s */, 0); }

static void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
  switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
      char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
      if (s_scanActive && !s_pendingConnect && isSupportedController(param, name)) {
        memcpy(s_pendingAddr, param->disc_res.bda, ESP_BD_ADDR_LEN);
        strcpy(s_pendingName, name);
        s_pendingConnect = true;
        esp_bt_gap_cancel_discovery();
      }
      break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      if (param->disc_st_chg.state != ESP_BT_GAP_DISCOVERY_STOPPED) break;
      if (s_pendingConnect) {
        s_pendingConnect = false;
        if (classicConnect(s_pendingAddr)) strcpy(s_name, s_pendingName);
      } else if (s_scanActive) {
        if ((int32_t)(millis() - s_scanDeadline) < 0) {
          startDiscovery();
        } else {
          s_scanActive = false;
          if (state == LinkState::Scanning) state = LinkState::Idle;
        }
      }
      break;

    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
      if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS && s_up && cb.name) {
        strncpy(s_name, (const char*)param->read_rmt_name.rmt_name, ESP_BT_GAP_MAX_BDNAME_LEN);
        cb.name(s_name);
      }
      break;

    case ESP_BT_GAP_AUTH_CMPL_EVT:
      if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "paired: %s", param->auth_cmpl.device_name);
      } else {
        ESP_LOGW(TAG, "authentication failed, status=%d", param->auth_cmpl.stat);
      }
      break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
      esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
      esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
      break;
    }

#ifdef CONFIG_BT_SSP_ENABLED
    case ESP_BT_GAP_CFM_REQ_EVT:
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      break;
#endif

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
static bool registerPsm(const char* name, uint16_t psm, uint8_t secId) {
  if (!L2CA_Register(psm, (tL2CAP_APPL_INFO*)&s_l2capInfo)) {
    ESP_LOGE(TAG, "L2CA_Register(%s) failed", name);
    return false;
  }
  // 着信: 認証を要求しない (コントローラー側が必要に応じて認証してくる)
  BTM_SetSecurityLevel(false, name, secId, BTM_SEC_NONE, psm, 0, 0);
  // 発信: 認証 + 暗号化 (ペアリング/ボンディングを発生させる)
  BTM_SetSecurityLevel(true, name, secId, BTM_SEC_OUT_AUTHENTICATE | BTM_SEC_OUT_ENCRYPT, psm, 0, 0);
  return true;
}

namespace internal {

bool classicInit(const char* deviceName) {
  esp_bt_dev_set_device_name(deviceName);
  esp_bt_gap_register_callback(gapCallback);

#ifdef CONFIG_BT_SSP_ENABLED
  esp_bt_io_cap_t ioCap = ESP_BT_IO_CAP_NONE;  // Just Works
  esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &ioCap, sizeof(ioCap));
#endif
  esp_bt_pin_code_t pin = {};
  esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin);

  if (!registerPsm("GP-HIDC", BT_PSM_HID_CONTROL, BTM_SEC_SERVICE_FIRST_EMPTY)) return false;
  if (!registerPsm("GP-HIDI", BT_PSM_HID_INTERRUPT, BTM_SEC_SERVICE_FIRST_EMPTY + 1)) return false;

  // ペアリング済みコントローラーからの再接続を受け付ける
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

  int n = esp_bt_gap_get_bond_device_num();
  ESP_LOGI(TAG, "saved pairings (Classic): %d", n);
  if (n > 0) {
    esp_bd_addr_t* list = (esp_bd_addr_t*)malloc(sizeof(esp_bd_addr_t) * n);
    if (list) {
      esp_bt_gap_get_bond_device_list(&n, list);
      for (int i = 0; i < n; i++)
        ESP_LOGI(TAG, "  %02x:%02x:%02x:%02x:%02x:%02x", list[i][0], list[i][1], list[i][2], list[i][3], list[i][4],
                 list[i][5]);
      free(list);
    }
  }
  return true;
}

bool classicStartScan(uint32_t timeoutMs) {
  s_scanActive = true;
  s_pendingConnect = false;
  s_scanDeadline = millis() + timeoutMs;
  startDiscovery();
  return true;
}

void classicStopScan() {
  if (!s_scanActive) return;
  s_scanActive = false;
  esp_bt_gap_cancel_discovery();
}

void classicDisconnect() {
  if (s_intr.cid) L2CA_DisconnectReq(s_intr.cid);
  if (s_ctrl.cid) L2CA_DisconnectReq(s_ctrl.cid);
}

void classicClearBonds() {
  int n = esp_bt_gap_get_bond_device_num();
  if (n <= 0) return;
  esp_bd_addr_t* list = (esp_bd_addr_t*)malloc(sizeof(esp_bd_addr_t) * n);
  if (!list) return;
  esp_bt_gap_get_bond_device_list(&n, list);
  for (int i = 0; i < n; i++) esp_bt_gap_remove_bond_device(list[i]);
  free(list);
}

}  // namespace internal

bool classicConnect(const uint8_t addr[6]) {
  if (!classicEnabled || state == LinkState::Connected || state == LinkState::Connecting) return false;
  bleStopScan();
  memcpy(remote, addr, BD_ADDR_LEN);
  s_ctrl.reset();
  s_intr.reset();
  s_outgoing = true;
  s_name[0] = 0;
  state = LinkState::Connecting;
  ESP_LOGI(TAG, "connecting to %02x:%02x:%02x:%02x:%02x:%02x", addr[0], addr[1], addr[2], addr[3], addr[4],
           addr[5]);
  uint16_t cid = L2CA_ConnectReq(BT_PSM_HID_CONTROL, remote);
  if (cid == 0) {
    ESP_LOGE(TAG, "control channel connect request failed");
    linkDown();
    return false;
  }
  s_ctrl.cid = cid;
  return true;
}

bool classicSend(Channel ch, const uint8_t* data, uint16_t len) {
  if (!s_up) return false;
  uint16_t cid = (ch == Channel::Control) ? s_ctrl.cid : s_intr.cid;
  if (cid == 0) return false;

  BT_HDR* buf = (BT_HDR*)malloc(sizeof(BT_HDR) + L2CAP_MIN_OFFSET + len);
  if (!buf) return false;
  buf->event = 0;
  buf->layer_specific = 0;
  buf->offset = L2CAP_MIN_OFFSET;
  buf->length = len;
  memcpy(buf->data + buf->offset, data, len);
  // バッファの所有権はスタックに渡る (失敗時もスタック側で解放される)
  return L2CA_DataWrite(cid, buf) != L2CAP_DW_FAILED;
}

}  // namespace gp
