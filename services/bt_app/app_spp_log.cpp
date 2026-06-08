#include "app_spp_log.h"
#include "app_bt.h"
#include "app_rfcomm_mgr.h"
#include "app_spp.h"
#include "bt_if.h"
#include "cmsis_os.h"
#include "hal_trace.h"
#include "sdp_api.h"
#include "spp_api.h"
#include "stdarg.h"
#include "stdio.h"
#include "string.h"

/* SPP UUID reused from serial — each service is distinguished by
   channel/app_id, not UUID. A standard SPP UUID is fine for both. */
static const uint8_t SPP_LOG_UUID_128[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00,
                                             0x00, 0x80, 0x00, 0x10, 0x00, 0x00,
                                             0x01, 0x11, 0x00, 0x00};

static uint8_t spp_log_service_index = 0;
static bool spp_log_is_connected = false;

/* Maximum length of a single formatted log line sent over SPP */
#define SPP_LOG_BUF_LEN 256

osMutexDef(app_spp_log_mutex);
osMutexDef(app_spp_log_credit_mutex);

/* -------------------------------------------------------------------------
 * Internal callbacks
 * ---------------------------------------------------------------------- */

static int app_spp_log_data_received(void *pDev, uint8_t process,
                                     uint8_t *pData, uint16_t dataLen) {
  /* Remote log channel is primarily TX-only from the device side.
     Accept but ignore any incoming data from the PC. */
  TRACE(2, "SPP LOG RX (ignored): len=%d", dataLen);
  return 0;
}

static bool app_spp_log_callback(RFCOMM_EVENT_E event, uint8_t instanceIndex,
                                 uint16_t connHandle, uint8_t *pBtAddr,
                                 uint8_t *pSentDataBuf, uint16_t sentDataLen) {
  switch (event) {
  case RFCOMM_INCOMING_CONN_REQ:
    TRACE(0, "SPP LOG: Incoming connection request");
    break;
  case RFCOMM_CONNECTED:
    TRACE(0, "SPP LOG: Connected");
    spp_log_is_connected = true;
    spp_log_service_index = instanceIndex;
    app_spp_log_printf("[OpenPineBuds] Remote log connected.\r\n");
    break;
  case RFCOMM_DISCONNECTED:
    TRACE(0, "SPP LOG: Disconnected");
    spp_log_is_connected = false;
    break;
  case RFCOMM_TX_DONE:
    break;
  default:
    TRACE(1, "SPP LOG: Unknown event %d", event);
    break;
  }
  return true;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void app_spp_log_init(void) {
  TRACE(0, "app_spp_log_init");

  RFCOMM_CONFIG_T tConfig;
  memset(&tConfig, 0, sizeof(tConfig));

  tConfig.callback = app_spp_log_callback;
  tConfig.tx_pkt_cnt = 4;
  tConfig.rfcomm_128bit_uuid = SPP_LOG_UUID_128;
  tConfig.rfcomm_ch = RFCOMM_CHANNEL_RED; /* Channel 7, wire 16 */
  tConfig.app_id = BTIF_APP_SPP_SERVER_RED_ID;
  tConfig.spp_handle_data_event_func = app_spp_log_data_received;
  tConfig.mutexId = osMutexCreate(osMutex(app_spp_log_mutex));
  tConfig.creditMutexId = osMutexCreate(osMutex(app_spp_log_credit_mutex));

  int8_t index = app_rfcomm_open(&tConfig);
  if (index == -1) {
    TRACE(0, "SPP LOG: Failed to open RFCOMM service");
  } else {
    TRACE(1, "SPP LOG: Initialized at index %d", index);
    spp_log_service_index = index;
  }
}

int app_spp_log_send(uint8_t *ptrData, uint16_t length) {
  if (!spp_log_is_connected) {
    return -1;
  }
  return app_rfcomm_write(spp_log_service_index, ptrData, length);
}

void app_spp_log_printf(const char *fmt, ...) {
  if (!spp_log_is_connected) {
    return;
  }

  char buf[SPP_LOG_BUF_LEN];
  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (len > 0) {
    app_spp_log_send((uint8_t *)buf, (uint16_t)len);
  }
}

bool app_spp_log_is_connected(void) { return spp_log_is_connected; }
