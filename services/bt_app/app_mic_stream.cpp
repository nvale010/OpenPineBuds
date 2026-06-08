/**
 * app_mic_stream.cpp
 *
 * Captures raw PCM from the main microphone (AUD_INPUT_PATH_MAINMIC) and
 * streams it over the data RFCOMM channel.
 *
 * Stream parameters: 8 kHz, 16-bit signed, 1 channel (mono).
 * Uses AUD_STREAM_ID_2 which has no capture users elsewhere in the codebase
 * and does not conflict with the BT audio / SCO path on AUD_STREAM_ID_0.
 *
 * Architecture:
 *   app_spp_serial RX (spp_read_thread)
 *     --> app_mic_stream_start/stop() posts a mailbox message
 *   Main app thread (app_mic_stream_process)
 *     --> af_stream_open/start/stop/close (must run here, not in SPP thread)
 *   DMA callback (IRQ context)
 *     --> ring_write() + prime pump on first full chunk
 *   RFCOMM_TX_DONE (BT stack callback)
 *     --> mic_stream_pump() sends next chunk, sustaining the pipeline
 */

#include "app_mic_stream.h"
#include "app_spp_log.h"
#include "app_spp_serial.h"
#include "app_thread.h"
#include "audioflinger.h"
#include "cmsis_os.h"
#include "hal_aud.h"
#include "hal_trace.h"
#include "string.h"

/* ---------------------------------------------------------------------------
 * Capture parameters
 *
 * AUD_INPUT_PATH_ANC_WNR delivers interleaved stereo:
 *   [FF_mic, FB_mic, FF_mic, FB_mic, ...]
 *   CH0 = FF (feed-forward, external)
 *   CH1 = FB (feed-backward, internal ear canal) ← the one we want
 *
 * We capture stereo and de-interleave in the callback, keeping only CH1.
 * Output to the ring buffer is mono 16-bit 8kHz.
 * ---------------------------------------------------------------------- */
#define MIC_SAMPLE_RATE 8000
#define MIC_SAMPLE_BITS 16
#define MIC_CHANNEL_NUM 2 /* stereo: FF + FB interleaved */
#define MIC_FRAME_LEN 480 /* samples per channel per DMA half-buffer (60ms) */
#define MIC_LOOP_CNT 4    /* ping-pong multiplier — total buffer ~240ms */
/* DMA callback fires every MIC_FRAME_LEN samples = 60ms = ~16 Hz.
   TX chunk = 480 bytes = 240 mono samples = 30ms.
   DMA rate (16 Hz) >> TX rate (~20 Hz) with no common sub-harmonic near
   the 0.1-2 Hz respiration band. */

typedef short mic_pcm_t;

#define MIC_DMA_BUF_SIZE                                                       \
  (MIC_FRAME_LEN * sizeof(mic_pcm_t) * MIC_CHANNEL_NUM * MIC_LOOP_CNT * 2)

/* Use AUD_STREAM_ID_0 — the only stream the codec hardware is wired to.
   Do not run BT audio simultaneously. */
#define MIC_STREAM_ID AUD_STREAM_ID_0

/* ---------------------------------------------------------------------------
 * Mailbox message IDs
 * ---------------------------------------------------------------------- */
typedef enum {
  MIC_STREAM_MSG_START = 0,
  MIC_STREAM_MSG_STOP,
} MIC_STREAM_MSG_T;

/* ---------------------------------------------------------------------------
 * Lock-free ring buffer (~250 ms at 8kHz 16-bit mono)
 * ---------------------------------------------------------------------- */
#define RING_BUF_SIZE 4096 /* power of two */
#define RING_BUF_MASK (RING_BUF_SIZE - 1)

static uint8_t ring_buf[RING_BUF_SIZE];
static volatile uint32_t ring_wr = 0;
static volatile uint32_t ring_rd = 0;

static inline uint32_t ring_used(void) {
  return (ring_wr - ring_rd) & RING_BUF_MASK;
}
static inline uint32_t ring_free(void) {
  return RING_BUF_SIZE - 1 - ring_used();
}

static void ring_write(const uint8_t *src, uint32_t len) {
  if (len > ring_free())
    return; /* drop — preserve existing data */
  uint32_t wr = ring_wr & RING_BUF_MASK;
  uint32_t tail = RING_BUF_SIZE - wr;
  if (len <= tail) {
    memcpy(&ring_buf[wr], src, len);
  } else {
    memcpy(&ring_buf[wr], src, tail);
    memcpy(&ring_buf[0], src + tail, len - tail);
  }
  __sync_synchronize();
  ring_wr = (ring_wr + len) & RING_BUF_MASK;
}

static uint32_t ring_read(uint8_t *dst, uint32_t len) {
  uint32_t avail = ring_used();
  if (len > avail)
    len = avail;
  if (len == 0)
    return 0;
  uint32_t rd = ring_rd & RING_BUF_MASK;
  uint32_t tail = RING_BUF_SIZE - rd;
  if (len <= tail) {
    memcpy(dst, &ring_buf[rd], len);
  } else {
    memcpy(dst, &ring_buf[rd], tail);
    memcpy(dst + tail, &ring_buf[0], len - tail);
  }
  __sync_synchronize();
  ring_rd = (ring_rd + len) & RING_BUF_MASK;
  return len;
}

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------- */
static bool mic_running = false;
static bool stream_is_open = false; /* tracks af_stream_open/close state */
static volatile bool tx_in_flight =
    false; /* true while a packet is queued in RFCOMM */

static uint8_t __attribute__((aligned(4))) mic_dma_buf[MIC_DMA_BUF_SIZE];

#define TX_CHUNK_SIZE                                                          \
  480 /* 30 ms @ 8kHz 16-bit — one DMA frame of mono output */
static uint8_t tx_chunk[TX_CHUNK_SIZE];

static void mic_stream_pump(void); /* forward declaration */

/* ---------------------------------------------------------------------------
 * DMA audio callback — IRQ context
 * Called every MIC_FRAME_LEN samples (~15 ms at 8kHz).
 * Input is interleaved stereo [FF0, FB0, FF1, FB1, ...].
 * We extract only the FB channel (index 1) into the ring buffer.
 * ---------------------------------------------------------------------- */
static uint32_t mic_stream_callback(uint8_t *buf, uint32_t len) {
  if (!mic_running)
    return len;

  mic_pcm_t *stereo = (mic_pcm_t *)buf;
  uint32_t total_pairs = len / (sizeof(mic_pcm_t) * MIC_CHANNEL_NUM);

  /* De-interleave: keep only FB mic (channel index 1) */
  for (uint32_t i = 0; i < total_pairs; i++) {
    mic_pcm_t fb_sample = stereo[i * MIC_CHANNEL_NUM + 1];
    ring_write((uint8_t *)&fb_sample, sizeof(mic_pcm_t));
  }

  /* Only pump if no packet is currently queued — avoids tx_free_list OOM */
  if (!tx_in_flight) {
    mic_stream_pump();
  }

  return len;
}

/* ---------------------------------------------------------------------------
 * TX pump — one chunk per RFCOMM_TX_DONE, self-sustaining pipeline
 * ---------------------------------------------------------------------- */
static void mic_stream_pump(void) {
  if (!mic_running)
    return;

  uint32_t avail = ring_used();
  if (avail == 0)
    return;

  uint32_t send_len = avail < TX_CHUNK_SIZE ? avail : TX_CHUNK_SIZE;
  uint32_t got = ring_read(tx_chunk, send_len);
  if (got == 0)
    return;

  tx_in_flight = true;
  int ret = app_spp_serial_send(tx_chunk, (uint16_t)got);
  if (ret != 0) {
    /* Send failed — clear the flag so the DMA callback can retry */
    tx_in_flight = false;
    TRACE(1, "app_mic_stream: tx busy (%d), dropping %u bytes", ret, got);
  }
}

/* ---------------------------------------------------------------------------
 * Main app thread handler — af_stream calls must run here
 * ---------------------------------------------------------------------- */
static int app_mic_stream_process(APP_MESSAGE_BODY *msg_body) {
  MIC_STREAM_MSG_T msg = (MIC_STREAM_MSG_T)msg_body->message_id;

  if (msg == MIC_STREAM_MSG_START) {
    /* Always stop/close first — unconditionally. The codec driver has no
       guard against double-open on ID_0; we must ensure it is fully closed
       before calling af_stream_open again regardless of our state flag. */
    if (stream_is_open || mic_running) {
      TRACE(0, "app_mic_stream: stopping previous session before start");
      mic_running = false;
      tx_in_flight = false;
      af_stream_stop(MIC_STREAM_ID, AUD_STREAM_CAPTURE);
      af_stream_close(MIC_STREAM_ID, AUD_STREAM_CAPTURE);
      stream_is_open = false;
    }

    TRACE(0, "app_mic_stream_start");
    ring_wr = 0;
    ring_rd = 0;
    tx_in_flight = false;

    struct AF_STREAM_CONFIG_T cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channel_num = (enum AUD_CHANNEL_NUM_T)MIC_CHANNEL_NUM;
    cfg.sample_rate = (enum AUD_SAMPRATE_T)MIC_SAMPLE_RATE;
    cfg.bits = (enum AUD_BITS_T)MIC_SAMPLE_BITS;
    cfg.vol = 15; /* max gain: index 15 = 28 dB (was 12 = 22 dB) */
    cfg.chan_sep_buf = false;
    cfg.device = AUD_STREAM_USE_INT_CODEC;
    cfg.io_path = AUD_INPUT_PATH_ANC_WNR; /* FB mic (ear canal) */
    cfg.handler = mic_stream_callback;
    cfg.data_ptr = mic_dma_buf;
    cfg.data_size = sizeof(mic_dma_buf);

    af_stream_open(MIC_STREAM_ID, AUD_STREAM_CAPTURE, &cfg);
    stream_is_open = true;
    mic_running = true;
    af_stream_start(MIC_STREAM_ID, AUD_STREAM_CAPTURE);

    app_spp_log_printf(
        "[mic] Recording started (8kHz 16-bit mono, FB mic)\r\n");
    TRACE(0, "app_mic_stream: capture started");

  } else if (msg == MIC_STREAM_MSG_STOP) {
    if (!stream_is_open) {
      TRACE(0, "app_mic_stream: not open, ignoring stop");
      return 0;
    }

    TRACE(0, "app_mic_stream_stop");
    mic_running = false;
    tx_in_flight = false;

    af_stream_stop(MIC_STREAM_ID, AUD_STREAM_CAPTURE);
    af_stream_close(MIC_STREAM_ID, AUD_STREAM_CAPTURE);
    stream_is_open = false;

    mic_stream_pump(); /* flush remaining buffered data */

    app_spp_log_printf("[mic] Recording stopped\r\n");
    TRACE(0, "app_mic_stream: capture stopped");
  }

  return 0;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void app_mic_stream_init(void) {
  TRACE(0, "app_mic_stream_init");
  ring_wr = 0;
  ring_rd = 0;
  mic_running = false;
  stream_is_open = false;
  tx_in_flight = false;
  app_set_threadhandle(APP_MODUAL_WNR, app_mic_stream_process);
}

void app_mic_stream_start(void) {
  APP_MESSAGE_BLOCK msg;
  memset(&msg, 0, sizeof(msg));
  msg.mod_id = APP_MODUAL_WNR;
  msg.msg_body.message_id = MIC_STREAM_MSG_START;
  app_mailbox_put(&msg);
}

void app_mic_stream_stop(void) {
  APP_MESSAGE_BLOCK msg;
  memset(&msg, 0, sizeof(msg));
  msg.mod_id = APP_MODUAL_WNR;
  msg.msg_body.message_id = MIC_STREAM_MSG_STOP;
  app_mailbox_put(&msg);
}

bool app_mic_stream_is_running(void) { return mic_running; }

void app_mic_stream_on_tx_done(void) {
  tx_in_flight = false; /* packet slot returned — safe to send next chunk */
  mic_stream_pump();
}
