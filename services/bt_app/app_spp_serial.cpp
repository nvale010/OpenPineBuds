#include "app_spp_serial.h"
#include "app_bt.h"
#include "app_rfcomm_mgr.h"
#include "app_spp.h"
#include "bt_if.h"
#include "cmsis_os.h"
#include "hal_trace.h"
#include "sdp_api.h"
#include "spp_api.h"
#include "string.h"

static uint8_t spp_serial_service_index = 0;
static bool spp_serial_is_connected = false;

osMutexDef(app_spp_serial_mutex);
osMutexDef(app_spp_serial_credit_mutex);

/* Standard SPP UUID: 00001101-0000-1000-8000-00805F9B34FB */
static const uint8_t SPP_SERIAL_UUID_128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x01, 0x11, 0x00, 0x00};

static int app_spp_serial_data_received(void *pDev, uint8_t process,
                                        uint8_t *pData, uint16_t dataLen) {
  TRACE(2, "SPP SERIAL RX: len=%d", dataLen);
  DUMP8("%02x ", pData, dataLen);

  // Echo back for testing (optional, but good for verification)
  int ret = app_spp_serial_send(pData, dataLen);
  if (ret != 0) {
    TRACE(1, "SPP SERIAL TX FAILED: %d", ret);
  }

  return 0;
}

static bool app_spp_serial_callback(RFCOMM_EVENT_E event, uint8_t instanceIndex,
                                    uint16_t connHandle, uint8_t *pBtAddr,
                                    uint8_t *pSentDataBuf,
                                    uint16_t sentDataLen) {
  switch (event) {
  case RFCOMM_INCOMING_CONN_REQ:
    TRACE(0, "SPP SERIAL: Incoming connection request");
    break;
  case RFCOMM_CONNECTED:
    TRACE(0, "SPP SERIAL: Connected");
    spp_serial_is_connected = true;
    spp_serial_service_index = instanceIndex;
    {
      const char *welcome = "\r\nHello from OpenPineBuds!\r\n";
      app_spp_serial_send((uint8_t *)welcome, strlen(welcome));
    }
    break;
  case RFCOMM_DISCONNECTED:
    TRACE(0, "SPP SERIAL: Disconnected");
    spp_serial_is_connected = false;
    break;
  case RFCOMM_TX_DONE:
    // TRACE(0, "SPP SERIAL: Tx Done");
    break;
  default:
    TRACE(1, "SPP SERIAL: Unknown event %d", event);
    break;
  }
  return true;
}

void app_spp_serial_init(void) {
  TRACE(0, "app_spp_serial_init");

  RFCOMM_CONFIG_T tConfig;
  memset(&tConfig, 0, sizeof(tConfig));

  tConfig.callback = app_spp_serial_callback;
  tConfig.tx_pkt_cnt = 4; // Credit count
  tConfig.rfcomm_128bit_uuid = SPP_SERIAL_UUID_128;
  tConfig.rfcomm_ch = RFCOMM_CHANNEL_TOTA; // Reuse TOTA channel ID (3) as it's
                                           // standard for SPP in this stack
  tConfig.app_id =
      BTIF_APP_SPP_SERVER_TOTA_ID; // Reuse ID to ensuring stack compatibility
  tConfig.spp_handle_data_event_func = app_spp_serial_data_received;

  tConfig.mutexId = osMutexCreate(osMutex(app_spp_serial_mutex));
  tConfig.creditMutexId = osMutexCreate(osMutex(app_spp_serial_credit_mutex));

  int8_t index = app_rfcomm_open(&tConfig);
  if (index == -1) {
    TRACE(0, "SPP SERIAL: Failed to open RFCOMM service");
  } else {
    TRACE(1, "SPP SERIAL: Initialized at index %d", index);
    spp_serial_service_index = index;
  }
}

int app_spp_serial_send(uint8_t *ptrData, uint16_t length) {
  if (!spp_serial_is_connected) {
    return -1;
  }
  return app_rfcomm_write(spp_serial_service_index, ptrData, length);
}
