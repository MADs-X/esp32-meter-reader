#ifndef FIRMWARE_UPDATE_H
#define FIRMWARE_UPDATE_H

#include <stdint.h>

void udp_ota_task(void *pvParameters);
void init_ota_cc1312(void);

#endif