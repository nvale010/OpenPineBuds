#ifndef __APP_SPP_SERIAL_H__
#define __APP_SPP_SERIAL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "stdint.h"

void app_spp_serial_init(void);
int app_spp_serial_send(uint8_t *ptrData, uint16_t length);

#ifdef __cplusplus
}
#endif

#endif // __APP_SPP_SERIAL_H__
