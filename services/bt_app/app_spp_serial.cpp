#include "app_spp_serial.h"
#include "app_bt.h"
#include "app_mic_stream.h"
#include "app_rfcomm_mgr.h"
#include "app_spp.h"
#include "app_spp_log.h"
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

/* -------------------------------------------------------------------------
 * Command parser
 * Processes single-byte commands received over the data channel:
 *   'r' — start mic recording, stream raw PCM over this channel
 *   's' — stop mic recording
 * ---------------------------------------------------------------------- */
static void handle_command(uint8_t cmd) {
  switch (cmd) {
  case 'r':
  case 'R':
    if (app_mic_stream_is_running()) {
      const char *msg = "[cmd] Already recording\r\n";
      app_spp_serial_send((uint8_t *)msg, strlen(msg));
      app_spp_log_printf("[cmd] 'r' ignored — already recording\r\n");
    } else {
      const char *msg = "[cmd] Recording started\r\n";
      app_spp_serial_send((uint8_t *)msg, strlen(msg));
      app_spp_log_printf("[cmd] 'r' received — starting mic stream\r\n");
      app_mic_stream_start();
    }
    break;

  case 's':
  case 'S':
    if (!app_mic_stream_is_running()) {
      const char *msg = "[cmd] Not recording\r\n";
      app_spp_serial_send((uint8_t *)msg, strlen(msg));
      app_spp_log_printf("[cmd] 's' ignored — not recording\r\n");
    } else {
      app_mic_stream_stop();
      const char *msg = "[cmd] Recording stopped\r\n";
      app_spp_serial_send((uint8_t *)msg, strlen(msg));
      app_spp_log_printf("[cmd] 's' received — mic stream stopped\r\n");
    }
    break;

  default:
    /* Unknown command — echo a short help string */
    {
      const char *msg = "[cmd] Unknown. Commands: 'r'=record 's'=stop\r\n";
      app_spp_serial_send((uint8_t *)msg, strlen(msg));
    }
    break;
  }
}

/* -------------------------------------------------------------------------
 * RFCOMM callbacks
 * ---------------------------------------------------------------------- */
static int app_spp_serial_data_received(void *pDev, uint8_t process,
                                        uint8_t *pData, uint16_t dataLen) {
  TRACE(2, "SPP SERIAL RX: len=%d", dataLen);

  /* Process each received byte as a command */
  for (uint16_t i = 0; i < dataLen; i++) {
    /* Skip CR/LF so minicom line-mode works naturally */
    if (pData[i] == '\r' || pData[i] == '\n')
      continue;
    handle_command(pData[i]);
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
      const char *welcome =
          "\r\nOpenPineBuds data channel connected.\r\n"
          "Commands: 'r' = start recording  's' = stop recording\r\n";
      app_spp_serial_send((uint8_t *)welcome, strlen(welcome));
    }
    app_spp_log_printf("[serial] Data channel connected\r\n");
    break;

  case RFCOMM_DISCONNECTED:
    TRACE(0, "SPP SERIAL: Disconnected");
    /* Stop recording if a client disconnects mid-stream */
    if (app_mic_stream_is_running()) {
      app_mic_stream_stop();
    }
    spp_serial_is_connected = false;
    break;

  case RFCOMM_TX_DONE:
    /* Opportunistically drain the mic ring buffer on every TX completion */
    app_mic_stream_on_tx_done();
    break;

  default:
    TRACE(1, "SPP SERIAL: Unknown event %d", event);
    break;
  }
  return true;
}

/* -------------------------------------------------------------------------
 * Init / send
 * ---------------------------------------------------------------------- */
void app_spp_serial_init(void) {
  TRACE(0, "app_spp_serial_init");

  RFCOMM_CONFIG_T tConfig;
  memset(&tConfig, 0, sizeof(tConfig));

  tConfig.callback = app_spp_serial_callback;
  tConfig.tx_pkt_cnt = 4;
  tConfig.rfcomm_128bit_uuid = SPP_SERIAL_UUID_128;
  tConfig.rfcomm_ch =
      RFCOMM_CHANNEL_GREEN; /* Channel 6, wire 15 — dedicated data port */
  tConfig.app_id = BTIF_APP_SPP_SERVER_GREEN_ID;
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
