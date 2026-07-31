#ifndef OTA_FSM_H
#define OTA_FSM_H

#include "esp_err.h"
#include <stdint.h>

typedef enum {
    OTA_TARGET_ESP32 = 0,
    OTA_TARGET_CC1312
} ota_target_t;


typedef struct {
    ota_target_t target;
    char fw_url[256];
    char expected_sha256[65]; // 64 char HEX + NULL
} ota_request_t;

/**
 * @brief 
 */
esp_err_t ota_fsm_init(void);

/**
 * @brief 
 */
esp_err_t ota_fsm_trigger_update(const ota_request_t *req);

#endif 