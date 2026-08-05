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
#include "obis_manager.h" 

static const char *TAG = "UDP_DISPATCH";
extern int meter_sock;
#define RX_BUFFER_SIZE 1024

typedef struct {
    uint16_t port;
    int sock;
    udp_handler_cb_t handler_cb;
} udp_port_map_t;

static udp_port_map_t port_registry[] = {
    {8004, -1, handle_module_support},
    {8006, -1, handle_timestamp},
    {8007, -1, handle_route},
    {8008, -1, handle_meter_read},
    {8015, -1, handle_meter_read},
    {0, -1, NULL} 
}; //[cite: 4]

// Çift başlatmayı engellemek için güvenlik bayrağı
static bool is_initialized = false;

int get_udp_socket_for_port(uint16_t port) {
    for (int i = 0; port_registry[i].port != 0; i++) {
        if (port_registry[i].port == port) {
            return port_registry[i].sock; //[cite: 4]
        }
    }
    return -1;
}

// Task sadece Select ve Receive işlemlerine odaklanır. Soket oluşturmaz.
static void udp_dispatcher_task(void *pvParameters) {
    int max_fd = (int)(intptr_t)pvParameters; // max_fd artık parametre olarak geliyor
    // Stack overflow koruması: rx_buffer heap'e taşındı (1024 byte stack tasarrufu)
    uint8_t *rx_buffer = (uint8_t *)malloc(RX_BUFFER_SIZE);
    if (!rx_buffer) {
        ESP_LOGE(TAG, "rx_buffer heap alloc failed!");
        vTaskDelete(NULL);
        return;
    }
    fd_set read_fds;

    ESP_LOGI(TAG, "Dispatcher task basariyla basladi. Dinlenen max fd: %d", max_fd);

    while (1) {
        FD_ZERO(&read_fds);
        for (int i = 0; port_registry[i].port != 0; i++) {
            if (port_registry[i].sock >= 0) {
                FD_SET(port_registry[i].sock, &read_fds);
            }
        }

        // 500ms Timeout eklenerek select'in sonsuz kilitlenmesi engellendi
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &tv); //[cite: 4]
        
        if (activity < 0 && errno != EINTR) {
            ESP_LOGE(TAG, "select hatasi: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else if (activity > 0) {
            for (int i = 0; port_registry[i].port != 0; i++) {
                int sock = port_registry[i].sock;
                if (sock >= 0 && FD_ISSET(sock, &read_fds)) {
                    struct sockaddr_in6 src_addr;
                    socklen_t addr_len = sizeof(src_addr);
                    int len = recvfrom(sock, rx_buffer, RX_BUFFER_SIZE, 0, (struct sockaddr *)&src_addr, &addr_len);
                    if (len > 0) {
                        if (port_registry[i].handler_cb) {
                            port_registry[i].handler_cb(sock, rx_buffer, (size_t)len, &src_addr); //[cite: 4]
                        }
                    } else if (len < 0) {
                        ESP_LOGE(TAG, "recvfrom hatasi (port %d): errno %d", port_registry[i].port, errno);
                    }
                }
            }
        }

        // Protokol fonksiyonları (Örneğin zaman aşımları ve temizlik)
        check_all_collect_data(); //[cite: 4]
    }
}

void init_udp_handlers(void) {
    // 1. KORUMA: Çift çağrılmayı (Double Initialization) kesin olarak engeller
    if (is_initialized) {
        ESP_LOGW(TAG, "UDP Handler zaten baslatildi, ikinci cagri engellendi.");
        return;
    }
    
    int max_fd = -1;

    // 2. SENKRONİZASYON: Task başlamadan önce tüm soketler ana thread'de ayağa kaldırılır
    for (int i = 0; port_registry[i].port != 0; i++) {
        int sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_IPV6);
        if (sock < 0) {
            ESP_LOGE(TAG, "Socket olusturulamadi (port %d): errno %d", port_registry[i].port, errno);
            continue; // Soketi -1 bırakıyoruz[cite: 4]
        }

        // 3. EADDRINUSE (112) KALICI ÇÖZÜMÜ: Restart sonrası asılı kalan portları zorla al
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in6 addr = {0};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port_registry[i].port);
        addr.sin6_addr = in6addr_any;

        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ESP_LOGE(TAG, "Bind basarisiz (port %d): errno %d", port_registry[i].port, errno);
            close(sock);
            continue;
        }

        port_registry[i].sock = sock;
        
        if (port_registry[i].port == 8008) {
            meter_sock = sock;
            ESP_LOGI(TAG, "Global meter_sock (TX) aktif edildi. FD: %d", meter_sock); //[cite: 4]
        }

        if (sock > max_fd) max_fd = sock;
        ESP_LOGI(TAG, "UDP dinleme baslatildi: port %d, fd %d", port_registry[i].port, sock); //[cite: 4]
    }

    if (max_fd == -1) {
        ESP_LOGE(TAG, "Hicbir soket acilamadi, dispatcher baslamayacak."); //[cite: 4]
        return;
    }

    // Başarıyla bağlandığı için bayrağı çek
    is_initialized = true;

    // Task'e oluşturduğumuz max_fd değerini parametre olarak pasla
    xTaskCreate(udp_dispatcher_task, "udp_dispatch", 8192, (void*)(intptr_t)max_fd, 5, NULL); //[cite: 4]
}

// ... Diğer handler_cb fonksiyonları (handle_module_support, vb.) aynı kalacak ...
void handle_module_support(int sock, const uint8_t *payload, size_t len, struct sockaddr_in6 *src_addr) {
    ESP_LOGI(TAG, "8004: module_support, len=%zu", len); //[cite: 4]
}

void handle_timestamp(int sock, const uint8_t *payload, size_t len, struct sockaddr_in6 *src_addr) {
    ESP_LOGI(TAG, "8006: timestamp, len=%zu", len); //[cite: 4]
}

void handle_route(int sock, const uint8_t *payload, size_t len, struct sockaddr_in6 *src_addr) {
    if (len < 9 || payload[0] != '#' || payload[1] != '#') return; //[cite: 4]

    int8_t rssi = (int8_t)payload[4]; //[cite: 4]
    uint32_t fw_version = payload[5] | (payload[6] << 8) | (payload[7] << 16) | (payload[8] << 24); //[cite: 4]

    char addr_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &src_addr->sin6_addr, addr_str, sizeof(addr_str)); //[cite: 4]
    ESP_LOGI(TAG, "8007: Kaynak IP: %s | RSSI: %d dBm | FW: %lu", addr_str, rssi, (unsigned long)fw_version); //[cite: 4]

    int start = 9; //[cite: 4]
    if (len > start + 1) { //[cite: 4]
        int hop_count = 0; //[cite: 4]
        char route_str[128] = {0}; //[cite: 4]
        
        for (size_t i = start; i < len - 1; i += 2) { //[cite: 4]
            uint16_t node_id = (payload[i] << 8) | payload[i + 1]; //[cite: 4]
            char hop_str[16];
            snprintf(hop_str, sizeof(hop_str), hop_count == 0 ? "0x%04X" : " -> 0x%04X", node_id); //[cite: 4]
            strncat(route_str, hop_str, sizeof(route_str) - strlen(route_str) - 1); //[cite: 4]
            hop_count++; //[cite: 4]
        }
        ESP_LOGI(TAG, "8007: Rota: %d hop | Yol: %s", hop_count, route_str); //[cite: 4]
    } else {
        ESP_LOGI(TAG, "8007: Rota: Dogrudan Baglanti (0 hop)"); //[cite: 4]
    }

    // Node ağa katıldı/aktif: OBIS isteğini hemen tetikle
    obis_notify_node_online(src_addr);
}