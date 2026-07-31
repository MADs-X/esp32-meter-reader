#ifndef WIREGUARD_MANAGER_H
#define WIREGUARD_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialize and start WireGuard client.
 *
 * NOTE: Ensure SNTP (Time) is synchronized before calling this function,
 * otherwise the WireGuard cryptographic handshake will silently fail.
 *
 * @return ESP_OK on success, ESP_FAIL or specific error code on failure.
 */
esp_err_t wireguard_manager_start(void);

/**
 * @brief Stop WireGuard client and safely clean up lwIP resources.
 */
void wireguard_manager_stop(void);

/**
 * @brief Check if WireGuard is currently connected and the interface is UP.
 *
 * @return true if connected and routeable, false otherwise.
 */
bool wireguard_manager_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* WIREGUARD_MANAGER_H */