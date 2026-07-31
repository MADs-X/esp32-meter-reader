#include "udp_handlers.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include "meter_protocol.h" 

static const char *TAG = "UDP_DISPATCH";
extern int meter_sock;
#define RX_BUFFER_SIZE 1024
#define MAX_UDP_PORTS  8

typedef struct {
    uint16_t port;
    int sock;
    udp_handler_cb_t handler_cb;
} udp_port_map_t;

// 1. LUT (Look-Up Table) - Gerekli Portlar
static udp_port_map_t port_registry[] = {
    {8004, -1, handle_module_support},
    {8006, -1, handle_timestamp},
    {8007, -1, handle_route},
    {8008, -1, handle_meter_read},
    {8015, -1, handle_meter_read},
    {0, -1, NULL} 
};

// 2. DISPATCHER TASK (Linker'ın aradığı gövde)
static void udp_dispatcher_task(void *pvParameters) {
    uint8_t rx_buffer[RX_BUFFER_SIZE];
    int max_fd = -1;
    fd_set read_fds;

    for (int i = 0; port_registry[i].port != 0; i++) {
        int sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_IPV6);
        if (sock < 0) {
            ESP_LOGE(TAG, "Socket olusturulamadi (port %d): errno %d", port_registry[i].port, errno);
            port_registry[i].sock = -1;
            continue;
        }

        struct sockaddr_in6 addr = {0};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port_registry[i].port);
        addr.sin6_addr = in6addr_any;

        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ESP_LOGE(TAG, "Bind basarisiz (port %d): errno %d", port_registry[i].port, errno);
            close(sock);
            port_registry[i].sock = -1;
            continue;
        }

        port_registry[i].sock = sock;
        
        // --- KRİTİK DÜZELTME BAŞLANGICI ---
        // 8008 portu (Sayaç Okuma) başarıyla açıldıysa, 
        // Scheduler'ın TX (sendto) yapabilmesi için FD'yi global değişkene eşle.
        if (port_registry[i].port == 8008) {
            meter_sock = sock;
            ESP_LOGI(TAG, "Global meter_sock (TX) aktif edildi. FD: %d", meter_sock);
        }
        // --- KRİTİK DÜZELTME SONU ---

        if (sock > max_fd) max_fd = sock;
        ESP_LOGI(TAG, "UDP dinleme baslatildi: port %d, fd %d", port_registry[i].port, sock);
    }

    if (max_fd == -1) {
        ESP_LOGE(TAG, "Hicbir soket acilamadi, dispatcher kapaniyor.");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        FD_ZERO(&read_fds);
        for (int i = 0; port_registry[i].port != 0; i++) {
            if (port_registry[i].sock >= 0) {
                FD_SET(port_registry[i].sock, &read_fds);
            }
        }

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (activity < 0) {
            ESP_LOGE(TAG, "select hatasi: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        for (int i = 0; port_registry[i].port != 0; i++) {
            int sock = port_registry[i].sock;
            if (sock >= 0 && FD_ISSET(sock, &read_fds)) {
                struct sockaddr_in6 src_addr;
                socklen_t addr_len = sizeof(src_addr);
                int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&src_addr, &addr_len);
                if (len > 0) {
                    if (port_registry[i].handler_cb) {
                        port_registry[i].handler_cb(sock, rx_buffer, (size_t)len, &src_addr);
                    }
                } else if (len < 0) {
                    ESP_LOGE(TAG, "recvfrom hatasi (port %d): errno %d", port_registry[i].port, errno);
                }
            }
        }
    }
}

// 3. INIT FONKSİYONU (main.c'nin çağırdığı gövde)
void init_udp_handlers(void) {
    xTaskCreate(udp_dispatcher_task, "udp_dispatch", 4096, NULL, 5, NULL);
}

// ------------------------------------------------------------
// HANDLER UYGULAMALARI
// ------------------------------------------------------------

void handle_module_support(int sock, const uint8_t *payload, size_t len, struct sockaddr_in6 *src_addr) {
    ESP_LOGI(TAG, "8004: module_support, len=%zu", len);
}

void handle_timestamp(int sock, const uint8_t *payload, size_t len, struct sockaddr_in6 *src_addr) {
    ESP_LOGI(TAG, "8006: timestamp, len=%zu", len);
}

// 4. 8007 - Rota (Route) ve Node Bilgisi
void handle_route(int sock, const uint8_t *payload, size_t len, struct sockaddr_in6 *src_addr) {
    if (len < 9) {
        ESP_LOGW(TAG, "[8007] Paket cok kisa (%zu byte). Drop edildi.", len);
        return;
    }
    
    if (payload[0] != '#' || payload[1] != '#') {
        ESP_LOGW(TAG, "[8007] Gecersiz paket basligi. Drop edildi.");
        return;
    }

    int8_t rssi = (int8_t)payload[4];
    uint32_t fw_version = payload[5] | (payload[6] << 8) | (payload[7] << 16) | (payload[8] << 24);

    char addr_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &src_addr->sin6_addr, addr_str, sizeof(addr_str));

    ESP_LOGI(TAG, "[8007] Kaynak IP: %s | RSSI: %d dBm | FW: %lu", addr_str, rssi, fw_version);

    int start = 9;
    if (len > start + 1) {
        int hop_count = 0;
        char route_str[128] = {0};
        
        for (size_t i = start; i < len - 1; i += 2) {
            uint16_t node_id = (payload[i] << 8) | payload[i + 1];
            char hop_str[16];
            snprintf(hop_str, sizeof(hop_str), hop_count == 0 ? "0x%04X" : " -> 0x%04X", node_id);
            strncat(route_str, hop_str, sizeof(route_str) - strlen(route_str) - 1);
            hop_count++;
        }
        ESP_LOGI(TAG, "[8007] Rota: %d hop | Yol: %s", hop_count, route_str);
    } else {
        ESP_LOGI(TAG, "[8007] Rota: Dogrudan Baglanti (0 hop)");
    }
}