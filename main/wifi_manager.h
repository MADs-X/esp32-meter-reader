#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Wi-Fi istasyon (STA) modunu başlatır ve ağa bağlanır.
 *
 * @param ssid     Bağlanılacak ağın adı
 * @param password Ağın şifresi
 * @return ESP_OK başarılı başlatma, aksi durumda hata kodu.
 */
esp_err_t wifi_manager_start(const char *ssid, const char *password);

/**
 * @brief Cihazın IP alıp almadığını kontrol eder.
 *
 * @return IP alındıysa ve ağa bağlıysa true.
 */
bool wifi_manager_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MANAGER_H */