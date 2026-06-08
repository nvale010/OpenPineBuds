#ifndef __APP_SPP_LOG_H__
#define __APP_SPP_LOG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"

/**
 * Initialise the remote logging RFCOMM service (Channel 7, wire 16).
 * Call once during BT stack initialisation, after app_rfcomm_mgr_init().
 */
void app_spp_log_init(void);

/**
 * Send a formatted log line over the remote log SPP channel.
 * Safe to call from any context — drops silently if not connected or if the
 * internal ring buffer is full.
 *
 * @param fmt  printf-style format string
 * @param ...  variadic arguments
 */
void app_spp_log_printf(const char *fmt, ...);

/**
 * Send raw bytes over the remote log SPP channel.
 * Returns 0 on success, negative on error.
 */
int app_spp_log_send(uint8_t *ptrData, uint16_t length);

/**
 * Returns true if a remote log client is currently connected.
 */
bool app_spp_log_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // __APP_SPP_LOG_H__
