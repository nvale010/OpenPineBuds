#ifndef __APP_MIC_STREAM_H__
#define __APP_MIC_STREAM_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 1 = stream FB + FF mics at 4 kHz each, interleaved [FB, FF];
   0 = original FB-only 8 kHz mono stream. */
#ifndef MIC_STREAM_DUAL
#define MIC_STREAM_DUAL 1
#endif

/**
 * Initialise the mic stream module.
 * Call once at startup (before any start/stop calls).
 */
void app_mic_stream_init(void);

/**
 * Start capturing from the internal (feedback) mic and streaming raw 16-bit
 * PCM samples over the data RFCOMM channel (RFCOMM_CHANNEL_GREEN, wire 15).
 *
 * Parameters (MIC_STREAM_DUAL = 1): 4 kHz, 16-bit, 2 channels [FB, FF].
 * Parameters (MIC_STREAM_DUAL = 0): 8 kHz, 16-bit, 1 channel (FB only).
 * Safe to call from any task context.
 */
void app_mic_stream_start(void);

/**
 * Stop the capture stream and free the hardware path.
 * Safe to call even if not currently recording.
 */
void app_mic_stream_stop(void);

/**
 * Returns true if capture is currently active.
 */
bool app_mic_stream_is_running(void);

/**
 * Called from app_spp_serial's RFCOMM_TX_DONE callback to drain the ring
 * buffer opportunistically without needing a separate polling task.
 */
void app_mic_stream_on_tx_done(void);

#ifdef __cplusplus
}
#endif

#endif // __APP_MIC_STREAM_H__
