#include "wireguard_manager.h"
#include "esp_log.h"
#include "esp_wireguard.h"
#include "esp_netif.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include <string.h>

static const char *TAG = "WG_MANAGER";

static wireguard_ctx_t s_wg_ctx = {0};
static volatile bool s_wg_connected = false;

esp_err_t wireguard_manager_start(void) {
#ifdef CONFIG_WIREGUARD_ENABLE
    if (s_wg_connected) {
        ESP_LOGW(TAG, "WireGuard is already running.");
        return ESP_OK;
    }

    wireguard_config_t config = ESP_WIREGUARD_CONFIG_DEFAULT();

    // Kriptografik bilgiler
    config.private_key = CONFIG_WIREGUARD_PRIVATE_KEY;
    config.public_key  = CONFIG_WIREGUARD_PEER_PUBLIC_KEY;
    config.endpoint    = CONFIG_WIREGUARD_ENDPOINT;
    config.port        = CONFIG_WIREGUARD_PORT;

    // ★ YEREL IP (ARAYÜZ IP'Sİ)
    config.allowed_ip      = CONFIG_WIREGUARD_LOCAL_IP;   // "10.8.161.1"
    config.allowed_ip_mask = "255.255.255.255";           // /32

    // ★ HANGİ TRAFİĞİ TÜNELLEYECEĞİMİZ?
    // Bu kütüphanede ayrı bir "allowed_ips" alanı yok.
    // Eğer tüm trafiği tünellemek istiyorsanız, aşağıdaki satırı ekleyin:
    // (ancak bu durumda Wi-Fi üzerinden internete çıkamazsınız)
    // config.allowed_ip = "0.0.0.0";
    // config.allowed_ip_mask = "0.0.0.0";
    //
    // Sadece 10.8.0.251'e gitmek istiyorsanız, yukarıdaki yerel IP ayarını koruyun
    // ve default route'u WireGuard'a ayarlayın (aşağıda yapılıyor).

    config.netif_key   = "WIFI_STA_DEF";
    config.persistent_keepalive = 25;

    esp_err_t err = esp_wireguard_init(&config, &s_wg_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wireguard_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wireguard_connect(&s_wg_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wireguard_connect failed: %s", esp_err_to_name(err));
        return err;
    }

    // ★ Peer'in hazır olmasını bekle
    int attempts = 0;
    while (attempts < 10) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        err = esp_wireguardif_peer_is_up(&s_wg_ctx);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "WireGuard peer is UP!");
            break;
        }
        attempts++;
        ESP_LOGI(TAG, "Waiting for peer... (%d/10)", attempts);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WireGuard peer did not come up!");
        return ESP_FAIL;
    }

    // ★ Varsayılan route'u WireGuard'a ayarla
    // esp_wireguard_set_default(&s_wg_ctx); // eğer bu çalışıyorsa kullan
    // Aşağıdaki manuel yöntem daha garantili:
    if (s_wg_ctx.netif) {
        LOCK_TCPIP_CORE();
        netif_set_default(s_wg_ctx.netif);
        UNLOCK_TCPIP_CORE();
        ESP_LOGI(TAG, "Default route set to WireGuard netif (%s)", s_wg_ctx.netif->name);
    } else {
        ESP_LOGE(TAG, "WireGuard netif is NULL!");
        return ESP_FAIL;
    }

    s_wg_connected = true;
    ESP_LOGI(TAG, "WireGuard interface is UP. Routing active.");

    return ESP_OK;
#else
    ESP_LOGW(TAG, "WireGuard is disabled.");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void wireguard_manager_stop(void) {
#ifdef CONFIG_WIREGUARD_ENABLE
    if (s_wg_connected) {
        ESP_LOGI(TAG, "Stopping WireGuard...");
        esp_wireguard_disconnect(&s_wg_ctx);
        s_wg_connected = false;
        ESP_LOGI(TAG, "WireGuard stopped.");
    }
#else
    ESP_LOGW(TAG, "WireGuard not enabled.");
#endif
}

bool wireguard_manager_is_connected(void) {
    return s_wg_connected;
}